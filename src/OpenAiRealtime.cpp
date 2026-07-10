// OpenAI Realtime API client (AI_ENGINE_OPENAI implementation of AiEngine.h).
//
// One WSS session to api.openai.com/v1/realtime: a `session.update` event
// configures the model, voice, system prompt, server-side VAD, and the
// set_emotion tool; then base64 24 kHz PCM flows up in
// `input_audio_buffer.append` events and base64 24 kHz PCM TTS comes back
// in `response.output_audio.delta` events.
// Docs: https://platform.openai.com/docs/guides/realtime
#include "config.h"

#if AI_ENGINE == AI_ENGINE_OPENAI

#include "AiEngine.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <mbedtls/base64.h>

namespace {

constexpr char OPENAI_HOST[] = "api.openai.com";

WebSocketsClient s_ws;
AiEngineCallbacks s_cbs = {};
bool s_socketConnected = false;
bool s_ready = false;
unsigned long s_setupSentAt = 0;
// After a set_emotion call we ask for a follow-up response (the spoken
// reply); the tool-call response's own `response.done` must not be
// reported as end-of-turn.
bool s_suppressNextDone = false;

// Uplink event: 60 ms of 24 kHz PCM is 2880 B -> 3840 B of base64, plus
// the JSON envelope. One static buffer, reused every chunk.
char s_txBuf[6 * 1024];

// Downlink PCM scratch: b64 audio deltas are decoded in slices into this.
uint8_t s_pcmBuf[3072];

void sendJson(JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  s_ws.sendTXT(out);
}

void sendSessionUpdate() {
  JsonDocument doc;
  doc["type"] = "session.update";
  JsonObject session = doc["session"].to<JsonObject>();
  session["type"] = "realtime";
  // TEXT-only replies — a LAN server speaks them (SpeakServer.h); the
  // board has no speaker and skipping TTS keeps the socket healthy.
  session["output_modalities"][0] = "text";
  session["instructions"] = AI_SYSTEM_PROMPT;

  JsonObject input = session["audio"]["input"].to<JsonObject>();
  input["format"]["type"] = "audio/pcm";
  input["format"]["rate"] = 24000;
  input["turn_detection"]["type"] = "server_vad";
  JsonObject output = session["audio"]["output"].to<JsonObject>();
  output["format"]["type"] = "audio/pcm";
  output["format"]["rate"] = 24000;
  output["voice"] = OPENAI_VOICE;

  JsonObject tool = session["tools"][0].to<JsonObject>();
  tool["type"] = "function";
  tool["name"] = "set_emotion";
  tool["description"] =
      "Set the facial expression and short caption shown on the robot's "
      "screen. Call this once before every spoken reply.";
  JsonObject params = tool["parameters"].to<JsonObject>();
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
  session["tool_choice"] = "auto";

  sendJson(doc);
  s_setupSentAt = millis();
  Serial.printf("[OpenAI] -> session.update (%s)\n", OPENAI_REALTIME_MODEL);
}

// Answers a completed set_emotion call and asks the model to continue with
// the spoken reply.
void sendToolOutputAndContinue(const char *callId) {
  JsonDocument doc;
  doc["type"] = "conversation.item.create";
  JsonObject item = doc["item"].to<JsonObject>();
  item["type"] = "function_call_output";
  item["call_id"] = callId;
  item["output"] = "ok";
  sendJson(doc);

  JsonDocument follow;
  follow["type"] = "response.create";
  sendJson(follow);
  s_suppressNextDone = true;
}

// Decodes a base64 audio delta in slices and hands the PCM to onAudio.
void emitBase64Audio(const char *b64) {
  size_t remaining = strlen(b64);
  while (remaining >= 4) {
    size_t slice = (sizeof(s_pcmBuf) / 3) * 4;
    if (slice > remaining) slice = remaining & ~(size_t)3;
    size_t decoded = 0;
    if (mbedtls_base64_decode(s_pcmBuf, sizeof(s_pcmBuf), &decoded,
                              (const unsigned char *)b64, slice) != 0) {
      Serial.println("[OpenAI] base64 audio decode failed");
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

void handleServerEvent(uint8_t *payload, size_t length) {
  JsonDocument doc;
  // Mutable input -> zero-copy parse: the big base64 audio delta points
  // into the WS payload instead of being duplicated.
  DeserializationError err =
      deserializeJson(doc, reinterpret_cast<char *>(payload), length);
  if (err) {
    Serial.printf("[OpenAI] Bad JSON from server: %s\n", err.c_str());
    return;
  }
  const char *type = doc["type"] | "";

  if (strcmp(type, "session.created") == 0) {
    sendSessionUpdate();
  } else if (strcmp(type, "session.updated") == 0) {
    if (!s_ready) {
      s_ready = true;
      Serial.println("[OpenAI] Session configured — ready");
      if (s_cbs.onReady) s_cbs.onReady();
    }
  } else if (strcmp(type, "response.output_audio.delta") == 0 ||
             strcmp(type, "response.audio.delta") == 0) {  // legacy name
    const char *b64 = doc["delta"] | "";
    if (b64[0]) emitBase64Audio(b64);
  } else if (strcmp(type, "response.output_text.delta") == 0 ||
             strcmp(type, "response.text.delta") == 0) {  // legacy name
    const char *transcript = doc["delta"] | "";
    if (transcript[0] && s_cbs.onTranscript) s_cbs.onTranscript(transcript);
  } else if (strcmp(type, "input_audio_buffer.speech_started") == 0) {
    // User is talking over the reply — barge-in.
    if (s_cbs.onInterrupted) s_cbs.onInterrupted();
  } else if (strcmp(type, "input_audio_buffer.speech_stopped") == 0) {
    Serial.println("[OpenAI] Utterance ended — model thinking");
    if (s_cbs.onUserSpeechEnd) s_cbs.onUserSpeechEnd();
  } else if (strcmp(type, "response.function_call_arguments.done") == 0) {
    const char *name = doc["name"] | "";
    const char *callId = doc["call_id"] | "";
    if (strcmp(name, "set_emotion") == 0) {
      JsonDocument args;
      if (deserializeJson(args, doc["arguments"] | "") ==
          DeserializationError::Ok) {
        const char *emotion = args["emotion"] | "";
        const char *text = args["text"] | "";
        const char *speech = args["speech"] | "";
        Serial.printf("[OpenAI] set_emotion(%s, \"%s\", speech %u bytes)\n",
                      emotion, text, (unsigned)strlen(speech));
        if (emotion[0] && s_cbs.onEmotion) s_cbs.onEmotion(emotion, text);
        if (speech[0] && s_cbs.onReplyText) s_cbs.onReplyText(speech);
      }
    } else {
      Serial.printf("[OpenAI] Unknown tool call: %s\n", name);
    }
    if (callId[0]) sendToolOutputAndContinue(callId);
  } else if (strcmp(type, "response.done") == 0) {
    if (s_suppressNextDone) {
      s_suppressNextDone = false;  // tool-call response — reply still coming
    } else {
      Serial.println("[OpenAI] Turn complete");
      if (s_cbs.onTurnComplete) s_cbs.onTurnComplete();
    }
  } else if (strcmp(type, "error") == 0) {
    Serial.printf("[OpenAI] Error: %s\n",
                  (const char *)(doc["error"]["message"] | "unknown"));
  }
}

void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[OpenAI] Socket connected, awaiting session.created");
      s_socketConnected = true;
      s_ready = false;
      s_suppressNextDone = false;
      s_setupSentAt = millis();
      break;
    case WStype_DISCONNECTED:
      if (s_socketConnected) {
        Serial.println("[OpenAI] Socket disconnected");
        s_socketConnected = false;
        s_ready = false;
        if (s_cbs.onDisconnected) s_cbs.onDisconnected();
      }
      break;
    case WStype_TEXT:
      handleServerEvent(payload, length);
      break;
    case WStype_ERROR:
      Serial.println("[OpenAI] Socket error");
      break;
    default:
      break;
  }
}

}  // namespace

void aiEngineInit(const AiEngineCallbacks &cbs) { s_cbs = cbs; }

void aiEngineConnect() {
  static char path[128];
  snprintf(path, sizeof(path), "/v1/realtime?model=%s", OPENAI_REALTIME_MODEL);
  static char headers[256];
  snprintf(headers, sizeof(headers), "Authorization: Bearer %s",
           OPENAI_API_KEY);
  s_ws.setExtraHeaders(headers);
  s_ws.onEvent(onWsEvent);
  s_ws.setReconnectInterval(WS_RECONNECT_INTERVAL_MS);
  s_ws.enableHeartbeat(15000, 3000, 2);
  Serial.printf("[OpenAI] Connecting wss://%s%s...\n", OPENAI_HOST, path);
  // No CA pinned: the WebSockets lib falls back to setInsecure() — fine
  // for this hobby device; pin a cert via beginSslWithCA if needed later.
  s_ws.beginSSL(OPENAI_HOST, 443, path);
}

void aiEngineLoop() {
  s_ws.loop();
  // Session was never configured: kill the socket and let the lib reconnect.
  if (s_socketConnected && !s_ready &&
      millis() - s_setupSentAt > SETUP_TIMEOUT_MS) {
    Serial.println("[OpenAI] Session setup timeout, reconnecting");
    s_ws.disconnect();
  }
}

bool aiEngineSocketConnected() { return s_socketConnected; }
bool aiEngineReady() { return s_ready; }

void aiEngineSendAudio(const int16_t *pcm, size_t samples) {
  if (!s_ready || samples == 0) return;

  static const char prefix[] =
      "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
  static const char suffix[] = "\"}";

  size_t b64Len = 0;
  size_t headerLen = sizeof(prefix) - 1;
  if (mbedtls_base64_encode(
          (unsigned char *)s_txBuf + headerLen,
          sizeof(s_txBuf) - headerLen - sizeof(suffix), &b64Len,
          (const unsigned char *)pcm, samples * sizeof(int16_t)) != 0) {
    Serial.println("[OpenAI] uplink chunk too large for tx buffer");
    return;
  }
  memcpy(s_txBuf, prefix, headerLen);
  memcpy(s_txBuf + headerLen + b64Len, suffix, sizeof(suffix));  // incl. NUL
  s_ws.sendTXT((uint8_t *)s_txBuf, headerLen + b64Len + sizeof(suffix) - 1);
}

void aiEngineAbort() {
  if (!s_ready) return;
  JsonDocument doc;
  doc["type"] = "response.cancel";
  sendJson(doc);
}

const char *aiEngineName() { return "OpenAI Realtime"; }

#endif  // AI_ENGINE == AI_ENGINE_OPENAI
