// Gemini Live API client (AI_ENGINE_GEMINI implementation of AiEngine.h).
//
// One WSS session to generativelanguage.googleapis.com running the
// BidiGenerateContent stream: a JSON `setup` message configures the model,
// system prompt, and the set_emotion tool; then base64 16 kHz PCM flows up
// inside `realtimeInput` messages. Replies come back as TTS audio (this
// model only supports AUDIO modality — the frames are discarded unread,
// the board has no speaker) plus an outputTranscription of the reply text.
// VAD/endpointing is automatic server-side.
// Docs: https://ai.google.dev/api/live
//
// Threading: the WebSocket lives on its OWN FreeRTOS task (core 0, away
// from the Arduino loop on core 1) so display draws or other main-loop
// stalls can never delay ping/pong servicing — previously the main cause
// of "random" disconnects. All s_ws access happens on that task:
//   uplink   — aiEngineSendAudio() (main task) copies PCM into s_txQueue;
//              the socket task encodes + sends.
//   downlink — the socket task parses server messages into s_evQueue;
//              aiEngineLoop() (main task) dispatches them to the
//              registered callbacks, so all state logic stays single-
//              threaded on the main loop.
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
volatile bool s_socketConnected = false;
volatile bool s_ready = false;
volatile unsigned long s_setupSentAt = 0;

// Latest session-resumption handle (sessionResumptionUpdate messages).
// Presented in the next setup so a reconnect restores the conversation
// instead of starting a cold session mid-call. Socket-task only.
char s_resumeHandle[128] = "";

// ---- main task -> socket task: outgoing items ----
enum class TxType : uint8_t { AUDIO, STREAM_END };
constexpr size_t TX_MAX_SAMPLES = AUDIO_SAMPLE_RATE * AUDIO_CHUNK_MS / 1000;
struct TxItem {
  TxType type;
  uint16_t samples;
  int16_t pcm[TX_MAX_SAMPLES];
};
QueueHandle_t s_txQueue = nullptr;

// ---- socket task -> main task: parsed server events ----
enum class EvType : uint8_t {
  READY, DISCONNECTED, TURN_COMPLETE, INTERRUPTED,
  EMOTION, REPLY_TEXT, TRANSCRIPT,
};
struct Event {
  EvType type;
  char emotion[16];  // EMOTION only
  char *text;        // heap-owned; freed by the dispatcher (may be null)
};
QueueHandle_t s_evQueue = nullptr;

void queueEvent(EvType type, const char *emotion = nullptr,
                const char *text = nullptr) {
  Event ev = {};
  ev.type = type;
  if (emotion) strlcpy(ev.emotion, emotion, sizeof(ev.emotion));
  ev.text = (text && text[0]) ? strdup(text) : nullptr;
  if (xQueueSend(s_evQueue, &ev, 0) != pdTRUE) {
    free(ev.text);
    Serial.println("[Gemini] Event queue full — event dropped");
  }
}

// Uplink message: 60 ms of PCM base64-encoded plus the JSON envelope.
// One static buffer, reused every chunk. Socket-task only.
char s_txBuf[6 * 1024];

// ---- everything below here runs on the socket task ----

void sendSetup() {
  JsonDocument doc;
  JsonObject setup = doc["setup"].to<JsonObject>();
  setup["model"] = GEMINI_LIVE_MODEL;
  // This native-audio live model only accepts AUDIO response modality
  // (a TEXT setup gets the session closed immediately).
  setup["generationConfig"]["responseModalities"][0] = "AUDIO";
  setup["systemInstruction"]["parts"][0]["text"] = AI_SYSTEM_PROMPT;
  // Transcript of the spoken reply — arrives as
  // serverContent.outputTranscription chunks (onTranscript).
  setup["outputAudioTranscription"].to<JsonObject>();

  JsonObject fn =
      setup["tools"][0]["functionDeclarations"][0].to<JsonObject>();
  fn["name"] = "set_emotion";
  fn["description"] =
      "Set the facial expression and short caption shown on the robot's "
      "screen. Call this once before every spoken reply.";
  JsonObject params = fn["parameters"].to<JsonObject>();
  params["type"] = "object";
  JsonObject emotion = params["properties"]["emotion"].to<JsonObject>();
  emotion["type"] = "string";
  JsonArray allowed = emotion["enum"].to<JsonArray>();
  allowed.add("happy");
  allowed.add("sad");
  allowed.add("neutral");
  allowed.add("thinking");
  JsonObject text = params["properties"]["text"].to<JsonObject>();
  text["type"] = "string";
  text["description"] =
      "Very short caption of the reply shown on the screen (max 6 words, "
      "same language as the spoken reply).";
  JsonObject speech = params["properties"]["speech"].to<JsonObject>();
  speech["type"] = "string";
  speech["description"] =
      "The complete text of the reply you are about to speak, word for "
      "word, in the same language.";
  params["required"][0] = "emotion";
  params["required"][1] = "text";
  params["required"][2] = "speech";

  // Ask for resumption handles; offer the last one so a reconnect picks
  // the conversation back up where the dropped socket left it.
  JsonObject resume = setup["sessionResumption"].to<JsonObject>();
  if (s_resumeHandle[0]) {
    resume["handle"] = s_resumeHandle;
    Serial.println("[Gemini] Resuming previous session");
  }

  String out;
  serializeJson(doc, out);
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
    queueEvent(EvType::READY);
    return;
  }

  if (doc["toolCall"].is<JsonObject>()) {
    for (JsonObject call : doc["toolCall"]["functionCalls"].as<JsonArray>()) {
      const char *name = call["name"] | "";
      const char *id = call["id"] | "";
      if (strcmp(name, "set_emotion") == 0) {
        const char *emotion = call["args"]["emotion"] | "";
        const char *text = call["args"]["text"] | "";
        const char *speech = call["args"]["speech"] | "";
        Serial.printf("[Gemini] set_emotion(%s, \"%s\")\n", emotion, text);
        Serial.printf("[Gemini] reply text (%u bytes): \"%s\"\n",
                      (unsigned)strlen(speech), speech);
        if (emotion[0]) queueEvent(EvType::EMOTION, emotion, text);
        if (speech[0]) queueEvent(EvType::REPLY_TEXT, nullptr, speech);
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
      queueEvent(EvType::INTERRUPTED);
    }
    // modelTurn inlineData parts are TTS audio — deliberately ignored
    // (no speaker; skipping the decode keeps this task fast).
    const char *transcript = content["outputTranscription"]["text"] | "";
    if (transcript[0]) {
      Serial.printf("[Gemini] transcript: \"%s\"\n", transcript);
      queueEvent(EvType::TRANSCRIPT, nullptr, transcript);
    }
    if (content["turnComplete"] | false) {
      Serial.println("[Gemini] Turn complete");
      queueEvent(EvType::TURN_COMPLETE);
    }
    return;
  }

  if (doc["sessionResumptionUpdate"].is<JsonObject>()) {
    JsonObject update = doc["sessionResumptionUpdate"];
    const char *handle = update["newHandle"] | "";
    if ((update["resumable"] | false) && handle[0]) {
      strlcpy(s_resumeHandle, handle, sizeof(s_resumeHandle));
    }
    return;
  }

  if (doc["goAway"].is<JsonObject>()) {
    // Session lifetime limit — the server will close soon; the WebSockets
    // lib reconnects and setup re-runs (resumption keeps the context).
    Serial.println("[Gemini] goAway — server is ending this session");
    return;
  }

  // Anything else is unexpected — log it so a rejected setup or an error
  // shows its reason instead of just a silent disconnect loop.
  String unknown;
  serializeJson(doc, unknown);
  Serial.printf("[Gemini] Unhandled message: %.300s\n", unknown.c_str());
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
        if (!s_ready && s_resumeHandle[0]) {
          // Died during setup — the offered resumption handle may be
          // expired/rejected. Drop it so the retry starts a fresh session
          // instead of looping on a poisoned handle.
          Serial.println("[Gemini] Discarding resumption handle");
          s_resumeHandle[0] = '\0';
        }
        s_socketConnected = false;
        s_ready = false;
        queueEvent(EvType::DISCONNECTED);
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

void sendQueuedAudio(const TxItem &item) {
  static const char prefix[] =
      "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\","
      "\"data\":\"";
  static const char suffix[] = "\"}}}";

  size_t b64Len = 0;
  size_t headerLen = sizeof(prefix) - 1;
  if (mbedtls_base64_encode(
          (unsigned char *)s_txBuf + headerLen,
          sizeof(s_txBuf) - headerLen - sizeof(suffix), &b64Len,
          (const unsigned char *)item.pcm,
          item.samples * sizeof(int16_t)) != 0) {
    Serial.println("[Gemini] uplink chunk too large for tx buffer");
    return;
  }
  memcpy(s_txBuf, prefix, headerLen);
  memcpy(s_txBuf + headerLen + b64Len, suffix, sizeof(suffix));  // incl. NUL

  s_ws.sendTXT((uint8_t *)s_txBuf, headerLen + b64Len + sizeof(suffix) - 1);
}

void socketTask(void *) {
  static char path[256];
  snprintf(path, sizeof(path), GEMINI_PATH_FMT, GEMINI_API_KEY);
  s_ws.onEvent(onWsEvent);
  s_ws.setReconnectInterval(WS_RECONNECT_INTERVAL_MS);
  // Generous pong tolerance: while big TTS frames stream in, the pong sits
  // behind them in the same TCP/TLS pipe — a tight window makes the client
  // itself kill perfectly healthy mid-reply connections.
  s_ws.enableHeartbeat(WS_PING_INTERVAL_MS, WS_PONG_TIMEOUT_MS,
                       WS_PONG_RETRIES);
  Serial.printf("[Gemini] Connecting wss://%s...\n", GEMINI_HOST);
  // No CA pinned: the WebSockets lib falls back to setInsecure() — fine
  // for this hobby device; pin a cert via beginSslWithCA if needed later.
  s_ws.beginSSL(GEMINI_HOST, 443, path);

  static TxItem item;  // 2 KB — keep it off the task stack
  for (;;) {
    s_ws.loop();

    while (xQueueReceive(s_txQueue, &item, 0) == pdTRUE) {
      if (!s_ready) continue;  // discard stale uplink from before a drop
      switch (item.type) {
        case TxType::AUDIO:
          sendQueuedAudio(item);
          break;
        case TxType::STREAM_END: {
          // Local mutable copy: the WS library masks payloads in place.
          char msg[] = "{\"realtimeInput\":{\"audioStreamEnd\":true}}";
          s_ws.sendTXT((uint8_t *)msg, sizeof(msg) - 1);
          break;
        }
      }
    }

    // setupComplete never arrived: kill the socket, let the lib reconnect.
    if (s_socketConnected && !s_ready &&
        millis() - s_setupSentAt > SETUP_TIMEOUT_MS) {
      Serial.println("[Gemini] Setup ack timeout, reconnecting");
      s_ws.disconnect();
    }

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

}  // namespace

void aiEngineInit(const AiEngineCallbacks &cbs) { s_cbs = cbs; }

void aiEngineConnect() {
  if (s_txQueue) return;  // already started
  s_txQueue = xQueueCreate(8, sizeof(TxItem));
  s_evQueue = xQueueCreate(16, sizeof(Event));
  // Core 0: the Arduino loop (display, state machine) runs on core 1, so
  // the socket keeps being serviced even while the LCD is mid-redraw.
  // 16 KB stack: TLS handshake + JSON parsing happen on this task.
  xTaskCreatePinnedToCore(socketTask, "gemini_ws", 16384, nullptr, 2,
                          nullptr, 0);
}

// Dispatches queued server events to the callbacks on the caller's (main)
// task, keeping all conversation/state logic single-threaded.
void aiEngineLoop() {
  if (!s_evQueue) return;
  Event ev;
  while (xQueueReceive(s_evQueue, &ev, 0) == pdTRUE) {
    switch (ev.type) {
      case EvType::READY:
        if (s_cbs.onReady) s_cbs.onReady();
        break;
      case EvType::DISCONNECTED:
        if (s_cbs.onDisconnected) s_cbs.onDisconnected();
        break;
      case EvType::TURN_COMPLETE:
        if (s_cbs.onTurnComplete) s_cbs.onTurnComplete();
        break;
      case EvType::INTERRUPTED:
        if (s_cbs.onInterrupted) s_cbs.onInterrupted();
        break;
      case EvType::EMOTION:
        if (s_cbs.onEmotion) s_cbs.onEmotion(ev.emotion, ev.text ? ev.text : "");
        break;
      case EvType::REPLY_TEXT:
        if (ev.text && s_cbs.onReplyText) s_cbs.onReplyText(ev.text);
        break;
      case EvType::TRANSCRIPT:
        if (ev.text && s_cbs.onTranscript) s_cbs.onTranscript(ev.text);
        break;
    }
    free(ev.text);
  }
}

bool aiEngineSocketConnected() { return s_socketConnected; }
bool aiEngineReady() { return s_ready; }

void aiEngineSendAudio(const int16_t *pcm, size_t samples) {
  if (!s_ready || samples == 0 || !s_txQueue) return;
  if (samples > TX_MAX_SAMPLES) samples = TX_MAX_SAMPLES;
  static TxItem item;  // only ever touched by the main task
  item.type = TxType::AUDIO;
  item.samples = (uint16_t)samples;
  memcpy(item.pcm, pcm, samples * sizeof(int16_t));
  // Queue full = uplink backpressure — drop the chunk rather than block.
  xQueueSend(s_txQueue, &item, 0);
}

void aiEngineSendAudioStreamEnd() {
  if (!s_ready || !s_txQueue) return;
  static TxItem item;
  item.type = TxType::STREAM_END;
  item.samples = 0;
  xQueueSend(s_txQueue, &item, 0);
}

void aiEngineAbort() {
  // Gemini Live has no explicit cancel message — interruption is driven by
  // new user audio server-side. The caller clears local playback; the
  // remainder of the in-flight reply is discarded as it arrives.
}

bool aiEngineCommitUtterance() {
  return false;  // streaming engine — the server endpoints the utterance
}

void aiEngineSendImage(const uint8_t *, size_t) {
  // Not wired for the Live API (would go out as a realtimeInput video
  // frame); snapshots are a GEMINI_REST feature for now.
}

const char *aiEngineName() { return "Gemini Live"; }

#endif  // AI_ENGINE == AI_ENGINE_GEMINI
