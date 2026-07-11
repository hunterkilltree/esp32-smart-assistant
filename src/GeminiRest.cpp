// Gemini REST client (AI_ENGINE_GEMINI_REST implementation of AiEngine.h).
//
// Request/response instead of a streaming socket: the utterance is
// recorded locally (voice-gated by Conversation), wrapped as a WAV, and
// sent in ONE generateContent HTTPS request; the model answers with ONE
// JSON object carrying the full reply:
//   { "user_text": ..., "emotion": ..., "text": ..., "speech": ... }
// No persistent connection to keep alive — far more robust on a weak
// hotspot than the Live API's WebSocket. The model also transcribes the
// user (user_text), so multi-turn context is kept as plain text pairs.
//
// Threading mirrors GeminiLive.cpp: the blocking HTTP round trip runs on
// its own task (core 0); results come back to the main task through an
// event queue drained by aiEngineLoop(), so all conversation/state logic
// stays single-threaded.
#include "config.h"

#if AI_ENGINE == AI_ENGINE_GEMINI_REST

#include "AiEngine.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>

namespace {

constexpr char GEMINI_REST_URL_FMT[] =
    "https://generativelanguage.googleapis.com/v1beta/models/"
    "%s:generateContent?key=%s";

// Set to true to Serial-dump every request body exactly as sent to Gemini
// (the base64 audio is elided — only its length is printed).
constexpr bool GEMINI_REST_LOG_TX = true;

AiEngineCallbacks s_cbs = {};
// "Ready" simply means WiFi is up — there is no session to establish.
bool s_wifiWasUp = false;

// ---- utterance recording (PSRAM) ----
// The WAV header lives at the start of the same buffer so the whole file
// can be base64-encoded in one pass at commit time.
constexpr size_t WAV_HEADER_BYTES = 44;
constexpr size_t PCM_MAX_BYTES =
    (size_t)AUDIO_SAMPLE_RATE * 2 * UTTERANCE_MAX_MS / 1000;
uint8_t *s_wav = nullptr;          // WAV_HEADER_BYTES + PCM_MAX_BYTES
size_t s_pcmLen = 0;               // PCM bytes recorded so far (main task)
volatile bool s_requestPending = false;  // worker owns s_wav while true
volatile uint32_t s_generation = 0;      // bumped by abort — stale replies drop
size_t s_reqWavLen = 0;                  // total WAV bytes handed to worker

// Request body assembly buffer (PSRAM): JSON envelope + base64 WAV + JPEG.
size_t s_bodyCap = 0;
char *s_body = nullptr;

// Camera snapshot for the current round (PSRAM), captured at the button
// press and sent as an image/jpeg part next to the utterance WAV. Written
// by the main task only while no request is pending (the worker owns it
// between commit and completion); one-shot — cleared after each request.
constexpr size_t JPEG_MAX_BYTES = 96 * 1024;  // VGA q12 is typically 20-40 KB
uint8_t *s_jpeg = nullptr;
size_t s_jpegLen = 0;

// ---- short conversation memory, kept as text on both sides ----
struct TurnPair {
  char user[161];
  char model[201];
};
constexpr int HISTORY_MAX = 6;
TurnPair s_history[HISTORY_MAX];
int s_histCount = 0;  // worker-task only after init

// ---- worker -> main events ----
enum class EvType : uint8_t { EMOTION, REPLY_TEXT, TURN_COMPLETE };
struct Event {
  EvType type;
  char emotion[16];
  char *text;  // heap-owned; freed by the dispatcher (may be null)
};
QueueHandle_t s_evQueue = nullptr;
TaskHandle_t s_worker = nullptr;

void queueEvent(EvType type, const char *emotion = nullptr,
                const char *text = nullptr) {
  Event ev = {};
  ev.type = type;
  if (emotion) strlcpy(ev.emotion, emotion, sizeof(ev.emotion));
  ev.text = (text && text[0]) ? strdup(text) : nullptr;
  if (xQueueSend(s_evQueue, &ev, 0) != pdTRUE) {
    free(ev.text);
    Serial.println("[GeminiRest] Event queue full — event dropped");
  }
}

// ---- request body building (worker task) ----

// Appends raw text at *p, bounded by the end of s_body. Returns false when
// out of space (request is then abandoned).
bool appendRaw(char *&p, const char *s) {
  size_t n = strlen(s);
  if (p + n >= s_body + s_bodyCap) return false;
  memcpy(p, s, n);
  p += n;
  return true;
}

// Appends s as JSON string content (escapes quotes/backslashes/control).
bool appendEscaped(char *&p, const char *s) {
  for (; *s; s++) {
    char c = *s;
    if (p + 8 >= s_body + s_bodyCap) return false;
    if (c == '"' || c == '\\') {
      *p++ = '\\';
      *p++ = c;
    } else if ((unsigned char)c < 0x20) {
      p += snprintf(p, 8, "\\u%04x", c);
    } else {
      *p++ = c;
    }
  }
  return true;
}

// Base64-encodes `len` bytes of `data` at *p, bounded by the end of s_body.
bool appendBase64(char *&p, const uint8_t *data, size_t len) {
  size_t room = s_bodyCap - (p - s_body);
  size_t b64Len = 0;
  if (mbedtls_base64_encode((unsigned char *)p, room, &b64Len, data, len) !=
      0) {
    return false;
  }
  p += b64Len;
  return true;
}

void writeWavHeader(uint8_t *h, uint32_t pcmBytes) {
  const uint32_t rate = AUDIO_SAMPLE_RATE, byteRate = rate * 2;
  memcpy(h, "RIFF", 4);
  uint32_t riffLen = 36 + pcmBytes;
  memcpy(h + 4, &riffLen, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  uint32_t fmtLen = 16;
  memcpy(h + 16, &fmtLen, 4);
  uint16_t fmt = 1, channels = 1, blockAlign = 2, bits = 16;
  memcpy(h + 20, &fmt, 2);
  memcpy(h + 22, &channels, 2);
  memcpy(h + 24, &rate, 4);
  memcpy(h + 28, &byteRate, 4);
  memcpy(h + 32, &blockAlign, 2);
  memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &pcmBytes, 4);
}

// Builds the full generateContent body into s_body. Returns its length,
// or 0 on overflow.
size_t buildRequestBody() {
  char *p = s_body;
  bool ok = appendRaw(p,
      "{\"systemInstruction\":{\"parts\":[{\"text\":\"");
  ok = ok && appendEscaped(p, AI_SYSTEM_PROMPT_REST);
  // responseSchema pins the output to exactly our four fields — without it
  // thinking-class models leak reasoning text around the JSON.
  ok = ok && appendRaw(p,
      "\"}]},\"generationConfig\":{\"responseMimeType\":\"application/json\","
      "\"responseSchema\":{\"type\":\"OBJECT\",\"properties\":{"
      "\"user_text\":{\"type\":\"STRING\"},\"emotion\":{\"type\":\"STRING\"},"
      "\"text\":{\"type\":\"STRING\"},\"speech\":{\"type\":\"STRING\"}},"
      "\"required\":[\"user_text\",\"emotion\",\"text\",\"speech\"]}}"
      ",\"contents\":[");
  for (int i = 0; ok && i < s_histCount; i++) {
    ok = appendRaw(p, "{\"role\":\"user\",\"parts\":[{\"text\":\"");
    ok = ok && appendEscaped(p, s_history[i].user);
    ok = ok && appendRaw(p, "\"}]},{\"role\":\"model\",\"parts\":[{\"text\":\"");
    ok = ok && appendEscaped(p, s_history[i].model);
    ok = ok && appendRaw(p, "\"}]},");
  }
  // Final user turn: the round's camera snapshot (if one was captured),
  // then the utterance WAV. History stays text-only — the image is sent
  // once and dropped.
  ok = ok && appendRaw(p, "{\"role\":\"user\",\"parts\":[");
  const char *imgB64Start = p, *imgB64End = p;
  size_t jpegLen = s_jpegLen;  // read once — abort may clear it mid-build
  if (jpegLen > 0) {
    ok = ok && appendRaw(p,
        "{\"inlineData\":{\"mimeType\":\"image/jpeg\",\"data\":\"");
    imgB64Start = p;
    ok = ok && appendBase64(p, s_jpeg, jpegLen);
    imgB64End = p;
    ok = ok && appendRaw(p, "\"}},");
  }
  ok = ok && appendRaw(p,
      "{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"");
  const char *wavB64Start = p;
  ok = ok && appendBase64(p, s_wav, s_reqWavLen);
  const char *wavB64End = p;
  ok = ok && appendRaw(p, "\"}}]}]}");
  if (!ok) {
    Serial.println("[GeminiRest] Request body overflow");
    return 0;
  }
  if (GEMINI_REST_LOG_TX) {
    Serial.printf("[GeminiRest] TX body (%u bytes, base64 blobs elided):\n",
                  (unsigned)(p - s_body));
    Serial.write((const uint8_t *)s_body, imgB64Start - s_body);
    if (imgB64End > imgB64Start) {
      Serial.printf("<%u base64 image chars>",
                    (unsigned)(imgB64End - imgB64Start));
    }
    Serial.write((const uint8_t *)imgB64End, wavB64Start - imgB64End);
    Serial.printf("<%u base64 audio chars>",
                  (unsigned)(wavB64End - wavB64Start));
    Serial.write((const uint8_t *)wavB64End, p - wavB64End);
    Serial.println();
  }
  return p - s_body;
}

void pushHistory(const char *user, const char *model) {
  if (s_histCount == HISTORY_MAX) {
    memmove(&s_history[0], &s_history[1],
            sizeof(TurnPair) * (HISTORY_MAX - 1));
    s_histCount--;
  }
  strlcpy(s_history[s_histCount].user, user, sizeof(s_history[0].user));
  strlcpy(s_history[s_histCount].model, model, sizeof(s_history[0].model));
  s_histCount++;
}

void reportFailure(const char *why) {
  Serial.printf("[GeminiRest] Request failed: %s\n", why);
  queueEvent(EvType::EMOTION, "sad", "Error");
  queueEvent(EvType::TURN_COMPLETE);
}

// One utterance -> one HTTPS round trip -> events. Worker task.
void performRequest() {
  size_t bodyLen = buildRequestBody();
  if (bodyLen == 0) {
    reportFailure("body overflow");
    return;
  }

  static char url[320];
  snprintf(url, sizeof(url), GEMINI_REST_URL_FMT, GEMINI_REST_MODEL,
           GEMINI_API_KEY);

  Serial.printf("[GeminiRest] -> generateContent (%u KB audio)\n",
                (unsigned)(s_reqWavLen / 1024));
  unsigned long t0 = millis();

  WiFiClientSecure client;
  client.setInsecure();  // same trust posture as the WS engines
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(GEMINI_REST_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    reportFailure("begin() — bad URL");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t *)s_body, bodyLen);
  if (code != 200) {
    String err = http.getString();
    http.end();
    err.replace('\n', ' ');  // keep the whole reason on one log line
    char why[240];
    snprintf(why, sizeof(why), "HTTP %d %.200s", code, err.c_str());
    reportFailure(why);
    return;
  }
  String payload = http.getString();
  http.end();
  Serial.printf("[GeminiRest] <- reply in %lu ms\n", millis() - t0);

  JsonDocument resp;
  if (deserializeJson(resp, payload)) {
    reportFailure("bad envelope JSON");
    return;
  }
  const char *inner =
      resp["candidates"][0]["content"]["parts"][0]["text"] | "";
  // Belt and braces: if the model wrapped the JSON in stray text anyway,
  // parse just the outermost {...} span.
  const char *jsonStart = strchr(inner, '{');
  const char *jsonEnd = jsonStart ? strrchr(jsonStart, '}') : nullptr;
  JsonDocument reply;
  if (!jsonStart || !jsonEnd ||
      deserializeJson(reply, jsonStart, jsonEnd - jsonStart + 1)) {
    Serial.printf("[GeminiRest] Unparseable reply: %.200s\n", inner);
    reportFailure("bad reply JSON");
    return;
  }

  const char *userText = reply["user_text"] | "";
  const char *emotion = reply["emotion"] | "neutral";
  const char *caption = reply["text"] | "";
  const char *speech = reply["speech"] | "";
  Serial.printf("[GeminiRest] user: \"%s\"\n", userText);
  Serial.printf("[GeminiRest] reply text (%u bytes): \"%s\"\n",
                (unsigned)strlen(speech), speech);

  queueEvent(EvType::EMOTION, emotion, caption);
  if (speech[0]) queueEvent(EvType::REPLY_TEXT, nullptr, speech);
  queueEvent(EvType::TURN_COMPLETE);
  pushHistory(userText, speech);
}

void workerTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t gen = s_generation;
    performRequest();
    if (gen != s_generation) {
      // Aborted while in flight — performRequest's events refer to a turn
      // the user already cancelled; Conversation's IDLE guards drop them.
      Serial.println("[GeminiRest] Reply for an aborted turn");
    }
    s_pcmLen = 0;
    s_jpegLen = 0;  // snapshot is one-shot — never resend it
    s_requestPending = false;
  }
}

}  // namespace

void aiEngineInit(const AiEngineCallbacks &cbs) { s_cbs = cbs; }

void aiEngineConnect() {
  if (s_evQueue) return;  // already started
  s_wav = (uint8_t *)heap_caps_malloc(WAV_HEADER_BYTES + PCM_MAX_BYTES,
                                      MALLOC_CAP_SPIRAM);
  // Optional — a failed alloc just means rounds go out without a snapshot.
  s_jpeg = (uint8_t *)heap_caps_malloc(JPEG_MAX_BYTES, MALLOC_CAP_SPIRAM);
  if (!s_jpeg) Serial.println("[GeminiRest] No JPEG buffer — audio only");
  // Envelope + history + base64 WAV and JPEG (4/3 of raw) + slack.
  s_bodyCap = 4096 + ((WAV_HEADER_BYTES + PCM_MAX_BYTES) * 4) / 3 +
              (JPEG_MAX_BYTES * 4) / 3 + 512;
  s_body = (char *)heap_caps_malloc(s_bodyCap, MALLOC_CAP_SPIRAM);
  if (!s_wav || !s_body) {
    Serial.println("[GeminiRest] PSRAM alloc failed — engine disabled");
    return;
  }
  s_evQueue = xQueueCreate(8, sizeof(Event));
  xTaskCreatePinnedToCore(workerTask, "gemini_rest", 16384, nullptr, 2,
                          &s_worker, 0);
  Serial.printf("[GeminiRest] Ready — %s, %lus max utterance\n",
                GEMINI_REST_MODEL, (unsigned long)(UTTERANCE_MAX_MS / 1000));
}

void aiEngineLoop() {
  // No session: "connected/ready" tracks WiFi. Report transitions the same
  // way the streaming engines report their socket.
  bool wifiUp = WiFi.status() == WL_CONNECTED && s_evQueue != nullptr;
  if (wifiUp && !s_wifiWasUp) {
    s_wifiWasUp = true;
    if (s_cbs.onReady) s_cbs.onReady();
  } else if (!wifiUp && s_wifiWasUp) {
    s_wifiWasUp = false;
    if (s_cbs.onDisconnected) s_cbs.onDisconnected();
  }

  if (!s_evQueue) return;
  Event ev;
  while (xQueueReceive(s_evQueue, &ev, 0) == pdTRUE) {
    switch (ev.type) {
      case EvType::EMOTION:
        if (s_cbs.onEmotion) s_cbs.onEmotion(ev.emotion, ev.text ? ev.text : "");
        break;
      case EvType::REPLY_TEXT:
        if (ev.text && s_cbs.onReplyText) s_cbs.onReplyText(ev.text);
        break;
      case EvType::TURN_COMPLETE:
        if (s_cbs.onTurnComplete) s_cbs.onTurnComplete();
        break;
    }
    free(ev.text);
  }
}

bool aiEngineSocketConnected() { return s_wifiWasUp; }
bool aiEngineReady() { return s_wifiWasUp; }

void aiEngineSendAudio(const int16_t *pcm, size_t samples) {
  if (!s_wav || s_requestPending || samples == 0) return;
  size_t bytes = samples * sizeof(int16_t);
  if (s_pcmLen + bytes > PCM_MAX_BYTES) return;  // utterance cap reached
  memcpy(s_wav + WAV_HEADER_BYTES + s_pcmLen, pcm, bytes);
  s_pcmLen += bytes;
}

void aiEngineSendAudioStreamEnd() {}  // meaningless without a stream

void aiEngineSendImage(const uint8_t *jpeg, size_t len) {
  if (!s_jpeg || !jpeg || len == 0) return;
  if (s_requestPending) {  // worker owns s_jpeg — don't race it
    Serial.println("[GeminiRest] Snapshot skipped — request in flight");
    return;
  }
  if (len > JPEG_MAX_BYTES) {
    Serial.printf("[GeminiRest] Snapshot too big (%u KB) — skipped\n",
                  (unsigned)(len / 1024));
    return;
  }
  memcpy(s_jpeg, jpeg, len);
  s_jpegLen = len;
  Serial.printf("[GeminiRest] Snapshot attached (%u KB)\n",
                (unsigned)(len / 1024));
}

bool aiEngineCommitUtterance() {
  // Require at least ~200 ms of audio so stray noise blips don't fire a
  // whole round trip.
  if (!s_wav || s_requestPending ||
      s_pcmLen < AUDIO_SAMPLE_RATE * 2 / 5) {
    s_pcmLen = 0;
    return false;
  }
  writeWavHeader(s_wav, (uint32_t)s_pcmLen);
  s_reqWavLen = WAV_HEADER_BYTES + s_pcmLen;
  s_requestPending = true;  // worker owns s_wav/s_body until it clears this
  xTaskNotifyGive(s_worker);
  return true;
}

void aiEngineAbort() {
  s_generation++;   // a reply already in flight belongs to a dead turn
  s_pcmLen = 0;     // drop any half-recorded utterance
  // Drop the round's snapshot too — unless the worker owns it right now
  // (it clears it itself when the in-flight request finishes).
  if (!s_requestPending) s_jpegLen = 0;
}

const char *aiEngineName() { return "Gemini REST"; }

#endif  // AI_ENGINE == AI_ENGINE_GEMINI_REST
