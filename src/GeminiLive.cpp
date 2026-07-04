// Gemini Live API client (AI_ENGINE_GEMINI implementation of AiEngine.h).
//
// One WSS session to generativelanguage.googleapis.com running the
// BidiGenerateContent stream: a JSON `setup` message configures the model,
// system prompt, and the set_emotion tool; then base64 16 kHz PCM flows up
// inside `realtimeInput` messages and base64 24 kHz PCM TTS comes back in
// `serverContent` messages. VAD/endpointing is automatic server-side.
// Docs: https://ai.google.dev/api/live
#include "config.h"

#if AI_ENGINE == AI_ENGINE_GEMINI

#include "AiEngine.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <mbedtls/base64.h>

namespace {

constexpr char GEMINI_HOST[] = "generativelanguage.googleapis.com";
constexpr char GEMINI_PATH_FMT[] =
    "/ws/google.ai.generativelanguage.v1beta.GenerativeService."
    "BidiGenerateContent?key=%s";

WebSocketsClient s_ws;
AiEngineCallbacks s_cbs = {};
bool s_socketConnected = false;
bool s_ready = false;
unsigned long s_setupSentAt = 0;

// Uplink message: 60 ms of 24 kHz PCM worst-case is 2880 B -> 3840 B of
// base64, plus the JSON envelope. One static buffer, reused every chunk.
char s_txBuf[6 * 1024];

// Downlink PCM scratch: b64 audio is decoded in slices into this.
uint8_t s_pcmBuf[3072];

void sendSetup() {
  JsonDocument doc;
  JsonObject setup = doc["setup"].to<JsonObject>();
  setup["model"] = GEMINI_LIVE_MODEL;
  setup["generationConfig"]["responseModalities"][0] = "AUDIO";
  setup["systemInstruction"]["parts"][0]["text"] = AI_SYSTEM_PROMPT;

  JsonObject fn =
      setup["tools"][0]["functionDeclarations"][0].to<JsonObject>();
  fn["name"] = "set_emotion";
  fn["description"] =
      "Set the facial expression shown on the robot's screen. Call this "
      "once before every spoken reply.";
  JsonObject params = fn["parameters"].to<JsonObject>();
  params["type"] = "object";
  JsonObject emotion = params["properties"]["emotion"].to<JsonObject>();
  emotion["type"] = "string";
  JsonArray allowed = emotion["enum"].to<JsonArray>();
  allowed.add("happy");
  allowed.add("sad");
  allowed.add("neutral");
  allowed.add("thinking");
  params["required"][0] = "emotion";

  String out;
  serializeJson(doc, out);
  // Serial.printf("[Gemini] -> setup (%s)\n", out.c_str());

  s_ws.sendTXT(out);
  s_setupSentAt = millis();
  Serial.printf("[Gemini] -> setup (%s)\n", GEMINI_LIVE_MODEL);
}

void sendToolResponse(const char *id) {
  JsonDocument doc;
  JsonObject resp = doc["toolResponse"]["functionResponses"][0].to<JsonObject>();
  resp["id"] = id;
  resp["name"] = "set_emotion";
  resp["response"]["output"] = "ok";
  String out;
  serializeJson(doc, out);
  s_ws.sendTXT(out);
}

// Decodes a base64 audio blob in slices (so arbitrarily large messages
// never need one huge PCM buffer) and hands the PCM to onAudio.
void emitBase64Audio(const char *b64) {
  size_t remaining = strlen(b64);
  while (remaining >= 4) {
    // Largest 4-multiple slice whose decoded size fits the scratch buffer.
    size_t slice = (sizeof(s_pcmBuf) / 3) * 4;
    if (slice > remaining) slice = remaining & ~(size_t)3;
    size_t decoded = 0;
    if (mbedtls_base64_decode(s_pcmBuf, sizeof(s_pcmBuf), &decoded,
                              (const unsigned char *)b64, slice) != 0) {
      Serial.println("[Gemini] base64 audio decode failed");
      return;
    }
    if (decoded > 0 && s_cbs.onAudio) {
      s_cbs.onAudio(reinterpret_cast<const int16_t *>(s_pcmBuf),
                    decoded / sizeof(int16_t));
    }
    b64 += slice;
    remaining -= slice;
  }
}

void handleServerMessage(uint8_t *payload, size_t length) {
  JsonDocument doc;
  // Mutable input -> zero-copy parse: strings (the big base64 audio blob)
  // point into the WS payload instead of being duplicated.
  DeserializationError err =
      deserializeJson(doc, reinterpret_cast<char *>(payload), length);
  if (err) {
    Serial.printf("[Gemini] Bad JSON from server: %s\n", err.c_str());
    return;
  }

  if (doc["setupComplete"].is<JsonObject>()) {
    s_ready = true;
    Serial.println("[Gemini] Setup complete — session ready");
    if (s_cbs.onReady) s_cbs.onReady();
    return;
  }

  if (doc["toolCall"].is<JsonObject>()) {
    for (JsonObject call : doc["toolCall"]["functionCalls"].as<JsonArray>()) {
      const char *name = call["name"] | "";
      const char *id = call["id"] | "";
      if (strcmp(name, "set_emotion") == 0) {
        const char *emotion = call["args"]["emotion"] | "";
        Serial.printf("[Gemini] set_emotion(%s)\n", emotion);
        if (emotion[0] && s_cbs.onEmotion) s_cbs.onEmotion(emotion);
      } else {
        Serial.printf("[Gemini] Unknown tool call: %s\n", name);
      }
      if (id[0]) sendToolResponse(id);
    }
    return;
  }

  if (doc["serverContent"].is<JsonObject>()) {
    JsonObject content = doc["serverContent"];
    if (content["interrupted"] | false) {
      Serial.println("[Gemini] Reply interrupted by user speech");
      if (s_cbs.onInterrupted) s_cbs.onInterrupted();
    }
    for (JsonObject part : content["modelTurn"]["parts"].as<JsonArray>()) {
      const char *b64 = part["inlineData"]["data"] | "";
      if (b64[0]) emitBase64Audio(b64);
    }
    if (content["turnComplete"] | false) {
      Serial.println("[Gemini] Turn complete");
      if (s_cbs.onTurnComplete) s_cbs.onTurnComplete();
    }
    return;
  }

  if (doc["goAway"].is<JsonObject>()) {
    // Session lifetime limit — the server will close soon; the WebSockets
    // lib reconnects and setup re-runs (conversation context is lost).
    Serial.println("[Gemini] goAway — server is ending this session");
    return;
  }
}

void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[Gemini] Socket connected, sending setup");
      s_socketConnected = true;
      s_ready = false;
      delay(300);  // IMPORTANT for ESP32 + TLS + WS upgrade
      sendSetup();
      break;
    case WStype_DISCONNECTED:
      if (s_socketConnected) {
        Serial.println("[Gemini] Socket disconnected");
        s_socketConnected = false;
        s_ready = false;
        if (s_cbs.onDisconnected) s_cbs.onDisconnected();
      }
      break;
    case WStype_TEXT:
    case WStype_BIN:
      // Gemini Live sends its JSON messages as binary frames.
      handleServerMessage(payload, length);
      break;
    case WStype_ERROR:
      Serial.println("[Gemini] Socket error");
      break;
    default:
      break;
  }
}

}  // namespace

void aiEngineInit(const AiEngineCallbacks &cbs) { s_cbs = cbs; }

void aiEngineConnect() {
  static char path[256];
  snprintf(path, sizeof(path), GEMINI_PATH_FMT, GEMINI_API_KEY);
  s_ws.onEvent(onWsEvent);
  s_ws.setReconnectInterval(WS_RECONNECT_INTERVAL_MS);
  s_ws.enableHeartbeat(15000, 3000, 2);
  Serial.printf("[Gemini] Connecting wss://%s...\n", GEMINI_HOST);
  // No CA pinned: the WebSockets lib falls back to setInsecure() — fine
  // for this hobby device; pin a cert via beginSslWithCA if needed later.
  s_ws.beginSSL(GEMINI_HOST, 443, path);
}

void aiEngineLoop() {
  s_ws.loop();
  // setupComplete never arrived: kill the socket and let the lib reconnect.
  if (s_socketConnected && !s_ready &&
      millis() - s_setupSentAt > SETUP_TIMEOUT_MS) {
    Serial.println("[Gemini] Setup ack timeout, reconnecting");
    s_ws.disconnect();
  }
}

bool aiEngineSocketConnected() { return s_socketConnected; }
bool aiEngineReady() { return s_ready; }

void aiEngineSendAudio(const int16_t *pcm, size_t samples) {
  if (!s_ready || samples == 0) return;

  static const char prefix[] =
      "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\","
      "\"data\":\"";
  static const char suffix[] = "\"}}}";

  size_t b64Len = 0;
  size_t headerLen = sizeof(prefix) - 1;
  if (mbedtls_base64_encode(
          (unsigned char *)s_txBuf + headerLen,
          sizeof(s_txBuf) - headerLen - sizeof(suffix), &b64Len,
          (const unsigned char *)pcm, samples * sizeof(int16_t)) != 0) {
    Serial.println("[Gemini] uplink chunk too large for tx buffer");
    return;
  }
  memcpy(s_txBuf, prefix, headerLen);
  memcpy(s_txBuf + headerLen + b64Len, suffix, sizeof(suffix));  // incl. NULL

  s_ws.sendTXT((uint8_t *)s_txBuf, headerLen + b64Len + sizeof(suffix) - 1);
}

void aiEngineAbort() {
  // Gemini Live has no explicit cancel message — interruption is driven by
  // new user audio server-side. The caller clears local playback; the
  // remainder of the in-flight reply is discarded as it arrives.
}

const char *aiEngineName() { return "Gemini Live"; }

#endif  // AI_ENGINE == AI_ENGINE_GEMINI
