#include "XiaozhiProtocol.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "config.h"

namespace {

WebSocketsClient s_ws;
XiaozhiCallbacks s_cbs = {};
char s_deviceId[18] = "";
char s_clientId[37] = "";
char s_sessionId[40] = "";
bool s_socketConnected = false;
bool s_ready = false;
unsigned long s_helloSentAt = 0;

void makeClientId() {
  // Persistent random UUIDv4, like xiaozhi's NVS-stored Client-Id: the
  // backend uses it (plus the MAC) to identify this physical device across
  // reboots, so it must not change once generated.
  Preferences prefs;
  prefs.begin("xiaozhi", false);
  String saved = prefs.getString("client_id", "");
  if (saved.length() == 36) {
    strlcpy(s_clientId, saved.c_str(), sizeof(s_clientId));
  } else {
    uint8_t b[16];
    for (int i = 0; i < 16; i += 4) {
      uint32_t r = esp_random();
      memcpy(b + i, &r, 4);
    }
    b[6] = (b[6] & 0x0F) | 0x40;  // version 4
    b[8] = (b[8] & 0x3F) | 0x80;  // variant 1
    snprintf(s_clientId, sizeof(s_clientId),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
             b[11], b[12], b[13], b[14], b[15]);
    prefs.putString("client_id", s_clientId);
    Serial.printf("[XZ] Generated new Client-Id: %s\n", s_clientId);
  }
  prefs.end();
}

void sendClientHello() {
  JsonDocument doc;
  doc["type"] = "hello";
  doc["version"] = XIAOZHI_PROTOCOL_VERSION;
  doc["transport"] = "websocket";
  JsonObject audio = doc["audio_params"].to<JsonObject>();
  audio["format"] = "opus";
  audio["sample_rate"] = AUDIO_SAMPLE_RATE;
  audio["channels"] = AUDIO_CHANNELS;
  audio["frame_duration"] = OPUS_FRAME_DURATION_MS;
  String out;
  serializeJson(doc, out);
  s_ws.sendTXT(out);
  s_helloSentAt = millis();
  Serial.printf("[XZ] -> %s\n", out.c_str());
}

void sendWithSession(JsonDocument &doc) {
  doc["session_id"] = s_sessionId;
  String out;
  serializeJson(doc, out);
  s_ws.sendTXT(out);
  Serial.printf("[XZ] -> %s\n", out.c_str());
}

void handleServerText(uint8_t *payload, size_t length) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("[XZ] Bad JSON from server: %s\n", err.c_str());
    return;
  }
  const char *type = doc["type"] | "";

  if (strcmp(type, "hello") == 0) {
    strlcpy(s_sessionId, doc["session_id"] | "", sizeof(s_sessionId));
    uint32_t rate = doc["audio_params"]["sample_rate"] | PLAYBACK_SAMPLE_RATE_DEFAULT;
    s_ready = true;
    Serial.printf("[XZ] Server hello: session=%s, downlink %u Hz\n",
                  s_sessionId, (unsigned)rate);
    if (s_cbs.onReady) s_cbs.onReady(rate);
  } else if (strcmp(type, "tts") == 0) {
    const char *state = doc["state"] | "";
    const char *text = doc["text"] | "";
    if (text[0]) Serial.printf("[XZ] TTS %s: %s\n", state, text);
    if (s_cbs.onTtsState) s_cbs.onTtsState(state, text);
  } else if (strcmp(type, "stt") == 0) {
    const char *text = doc["text"] | "";
    Serial.printf("[XZ] STT: %s\n", text);
    if (s_cbs.onStt) s_cbs.onStt(text);
  } else if (strcmp(type, "llm") == 0) {
    const char *emotion = doc["emotion"] | "";
    Serial.printf("[XZ] Emotion: %s\n", emotion);
    if (emotion[0] && s_cbs.onEmotion) s_cbs.onEmotion(emotion);
  } else if (strcmp(type, "goodbye") == 0) {
    Serial.println("[XZ] Server said goodbye");
    s_sessionId[0] = '\0';
    if (s_cbs.onGoodbye) s_cbs.onGoodbye();
  } else if (strcmp(type, "system") == 0) {
    const char *command = doc["command"] | "";
    Serial.printf("[XZ] System command: %s\n", command);
    if (strcmp(command, "reboot") == 0) ESP.restart();
  } else if (strcmp(type, "mcp") == 0) {
    // MCP (device tool calls) not implemented — we never advertise the
    // feature, but answer politely if a server probes anyway.
    Serial.println("[XZ] MCP request ignored (feature not advertised)");
  } else {
    Serial.printf("[XZ] Unhandled message type: %s\n", type);
  }
}

void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[XZ] Socket connected, sending hello");
      s_socketConnected = true;
      s_ready = false;
      sendClientHello();
      break;
    case WStype_DISCONNECTED:
      if (s_socketConnected) {
        Serial.println("[XZ] Socket disconnected");
        s_socketConnected = false;
        s_ready = false;
        s_sessionId[0] = '\0';
        if (s_cbs.onDisconnected) s_cbs.onDisconnected();
      }
      break;
    case WStype_TEXT:
      handleServerText(payload, length);
      break;
    case WStype_BIN:
      // Protocol version 1: a binary frame is one raw Opus frame.
      if (s_ready && s_cbs.onAudio) s_cbs.onAudio(payload, length);
      break;
    case WStype_ERROR:
      Serial.println("[XZ] Socket error");
      break;
    default:
      break;
  }
}

}  // namespace

void xzInit(const XiaozhiCallbacks &cbs) {
  s_cbs = cbs;
  String mac = WiFi.macAddress();
  mac.toLowerCase();
  strlcpy(s_deviceId, mac.c_str(), sizeof(s_deviceId));
  makeClientId();
  Serial.printf("[XZ] Device-Id: %s, Client-Id: %s\n", s_deviceId, s_clientId);
}

const char *xzDeviceId() { return s_deviceId; }
const char *xzClientId() { return s_clientId; }

void xzConnect(const char *wsUrl, const char *token) {
  // Split "wss://host[:port]/path" by hand — no URL parser in the core.
  bool tls = strncmp(wsUrl, "wss://", 6) == 0;
  const char *rest = wsUrl + (tls ? 6 : (strncmp(wsUrl, "ws://", 5) == 0 ? 5 : 0));
  char host[96];
  const char *slash = strchr(rest, '/');
  size_t hostLen = slash ? (size_t)(slash - rest) : strlen(rest);
  if (hostLen >= sizeof(host)) hostLen = sizeof(host) - 1;
  memcpy(host, rest, hostLen);
  host[hostLen] = '\0';
  const char *path = slash ? slash : "/";

  uint16_t port = tls ? 443 : 80;
  char *colon = strchr(host, ':');
  if (colon != nullptr) {
    *colon = '\0';
    port = (uint16_t)atoi(colon + 1);
  }

  char headers[256];
  snprintf(headers, sizeof(headers),
           "Authorization: Bearer %s\r\n"
           "Protocol-Version: %d\r\n"
           "Device-Id: %s\r\n"
           "Client-Id: %s",
           token, XIAOZHI_PROTOCOL_VERSION, s_deviceId, s_clientId);
  s_ws.setExtraHeaders(headers);
  s_ws.onEvent(onWsEvent);
  s_ws.setReconnectInterval(WS_RECONNECT_INTERVAL_MS);
  s_ws.enableHeartbeat(15000, 3000, 2);

  Serial.printf("[XZ] Connecting %s -> %s:%u%s (tls=%d)\n", wsUrl, host, port,
                path, tls ? 1 : 0);
  if (tls) {
    // No CA pinned: the WebSockets lib falls back to setInsecure() — fine
    // for this hobby device; pin a cert via beginSslWithCA if needed later.
    s_ws.beginSSL(host, port, path);
  } else {
    s_ws.begin(host, port, path);
  }
}

void xzLoop() {
  s_ws.loop();
  // Server hello never arrived: kill the socket and let the lib reconnect.
  if (s_socketConnected && !s_ready &&
      millis() - s_helloSentAt > HELLO_TIMEOUT_MS) {
    Serial.println("[XZ] Server hello timeout, reconnecting");
    s_ws.disconnect();
  }
}

bool xzSocketConnected() { return s_socketConnected; }
bool xzReady() { return s_ready; }

void xzSendListenStart(const char *mode) {
  if (!s_ready) return;
  JsonDocument doc;
  doc["type"] = "listen";
  doc["state"] = "start";
  doc["mode"] = mode;
  sendWithSession(doc);
}

void xzSendListenStop() {
  if (!s_ready) return;
  JsonDocument doc;
  doc["type"] = "listen";
  doc["state"] = "stop";
  sendWithSession(doc);
}

void xzSendAbort(const char *reason) {
  if (!s_ready) return;
  JsonDocument doc;
  doc["type"] = "abort";
  if (reason && reason[0]) doc["reason"] = reason;
  sendWithSession(doc);
}

void xzSendAudio(const uint8_t *opusData, size_t len) {
  if (!s_ready || len == 0) return;
  s_ws.sendBIN(opusData, len);
}
