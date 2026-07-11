#pragma once
#include <cstddef>
#include <cstdint>

// Engine ids — defined before secrets.h so AI_ENGINE can be set there.
#define AI_ENGINE_GEMINI      1  // Gemini Live API (streaming WebSocket)
#define AI_ENGINE_OPENAI      2  // OpenAI Realtime API (streaming WebSocket)
#define AI_ENGINE_GEMINI_REST 3  // Gemini generateContent (one HTTPS request
                                 // per utterance, full reply in one response
                                 // — no persistent socket to keep alive)

#include "secrets.h"

#ifndef AI_ENGINE
#define AI_ENGINE AI_ENGINE_GEMINI
#endif

// ---- AI engine (see include/AiEngine.h) ----
// Models can be overridden in secrets.h if a newer one ships.
#ifndef GEMINI_LIVE_MODEL
#define GEMINI_LIVE_MODEL "models/gemini-3.1-flash-live-preview"
#endif
// generateContent, audio-in. Verified available for this project's key on
// 2026-07-10 (gemini-2.5-flash is "no longer available to new users").
// flash-lite: fast, non-thinking — right latency class for a voice round.
#ifndef GEMINI_REST_MODEL
#define GEMINI_REST_MODEL "gemini-3.1-flash-lite"
#endif
#ifndef OPENAI_REALTIME_MODEL
#define OPENAI_REALTIME_MODEL "gpt-realtime"
#endif
#ifndef OPENAI_VOICE
#define OPENAI_VOICE "marin"
#endif

// Shared system prompt: short spoken answers + the set_emotion tool call
// that drives the LCD face (happy/sad/neutral/thinking) and puts a short
// caption of the reply on the screen.
#define AI_SYSTEM_PROMPT                                                     \
  "You are a cheerful voice assistant living inside a small robot that has " \
  "a face display. This is a spoken conversation: answer in one or two "     \
  "short sentences, no lists, no markdown. Before every reply, call the "    \
  "set_emotion function with three arguments: emotion — how your reply "    \
  "feels (happy for positive or friendly answers, sad for errors, bad "     \
  "news, or when you cannot help, neutral for plain factual answers); "     \
  "text — a very short caption of your reply to show on the screen, "       \
  "at most 6 words, in the same language you answer in; and speech — the "  \
  "complete text of the reply you are about to speak, word for word, in "   \
  "the same language."

// Longest on-screen caption kept from a set_emotion call (bytes, incl.
// NUL). Sized for ~6 words of Vietnamese — diacritics are 2–3 bytes each.
constexpr size_t EMOTION_TEXT_MAX = 96;

// ---- Gemini REST engine (AI_ENGINE_GEMINI_REST) ----
// The whole utterance is recorded locally (voice-gated), then sent as one
// WAV inside one generateContent request; the model answers with one JSON
// object carrying the full reply. Strict JSON is requested via
// responseMimeType, and the model also transcribes the user so multi-turn
// context can be kept as plain text.
#define AI_SYSTEM_PROMPT_REST                                                \
  "You are a cheerful voice assistant living inside a small robot that "    \
  "has a face display. You receive the user's voice message, and usually "  \
  "also a photo your own camera took when the user started talking — if "   \
  "the user asks what you see (or their question is about the "             \
  "surroundings), answer from that photo. Respond with "                    \
  "ONLY a JSON object with exactly these fields: \"user_text\" - a short "  \
  "transcription of what the user said; \"emotion\" - one of happy, sad, "  \
  "neutral, thinking, matching how your reply feels (happy for positive "   \
  "or friendly answers, sad for errors or bad news, neutral for plain "     \
  "factual answers); \"text\" - a very short caption of your reply for "    \
  "the screen, at most 6 words, in the same language the user spoke; "      \
  "\"speech\" - your full spoken reply, one or two short sentences, no "    \
  "lists, no markdown, in the same language the user spoke."

constexpr unsigned long UTTERANCE_MAX_MS       = 12000;  // per-turn recording cap
constexpr unsigned long GEMINI_REST_TIMEOUT_MS = 30000;  // HTTP response wait

// ---- Speak server (reply-text relay) ----
// After each spoken reply the full transcript is POSTed as plain text to
// this LAN endpoint (see src/SpeakServer.cpp). Override in secrets.h, or
// define it empty ("") there to disable the relay.
#ifndef SPEAK_SERVER_URL
#define SPEAK_SERVER_URL "http://192.168.1.77:8080/speak"
#endif
constexpr unsigned long SPEAK_SERVER_TIMEOUT_MS      = 3000;   // TCP connect
// The server may only answer after it finishes speaking the text — allow it
// plenty of time. Sends run on their own task, so this never blocks the
// main loop / WebSocket pump.
constexpr unsigned long SPEAK_SERVER_READ_TIMEOUT_MS = 20000;
// Longest reply transcript kept per turn (bytes incl. NUL; extra is dropped).
constexpr size_t TRANSCRIPT_MAX = 2048;
// The POST is sent only after the transcript has settled — no new chunk for
// this long — so trailing chunks (which can arrive after turnComplete or a
// mid-reply interrupt/WS-drop) are included in one complete message.
constexpr unsigned long TRANSCRIPT_SETTLE_MS = 1000;

// ---- Timing ----
constexpr unsigned long WIFI_RECONNECT_BASE_MS   = 2000;   // doubles per failed attempt
constexpr unsigned long WIFI_RECONNECT_MAX_MS    = 30000;  // backoff cap
constexpr unsigned int  WIFI_RECONNECT_MAX_SHIFT = 5;      // caps 2000 << shift before min()
constexpr unsigned long WS_RECONNECT_INTERVAL_MS = 5000;
// WS keepalive: ping cadence, how long a pong may take, and how many
// misses count as a dead link. The pong shares the TCP/TLS pipe with the
// (unused but unavoidable) TTS audio frames, so on a slow hotspot it can
// legitimately take several seconds — err on the tolerant side; a truly
// dead socket is still caught in ~ a minute, and WiFi loss is detected
// separately by wifiLinkLoop().
constexpr unsigned long WS_PING_INTERVAL_MS = 20000;
constexpr unsigned long WS_PONG_TIMEOUT_MS  = 10000;
constexpr uint8_t       WS_PONG_RETRIES     = 3;
constexpr unsigned long BUTTON_DEBOUNCE_MS       = 25;
constexpr unsigned long SETUP_TIMEOUT_MS         = 10000;  // session setup ack must arrive within this

// ---- Audio (raw PCM both directions — no codec) ----
// Uplink: mono 16-bit PCM from the mic at the engine's required rate,
// base64-encoded into the engine's JSON audio message.
// Downlink: mono 16-bit PCM TTS at 24 kHz (both engines), written to I2S TX.
#if AI_ENGINE == AI_ENGINE_OPENAI
constexpr uint32_t AUDIO_SAMPLE_RATE = 24000;  // OpenAI Realtime pcm is 24 kHz
#else
constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;  // Gemini Live input rate
#endif
constexpr uint8_t  AUDIO_BITS_PER_SAMPLE = 16;
constexpr uint8_t  AUDIO_CHANNELS        = 1;
constexpr uint32_t AUDIO_CHUNK_MS        = 60;   // one uplink message per chunk
constexpr uint32_t PLAYBACK_SAMPLE_RATE  = 24000;

// ---- Boot self-test (runs before the main flow; see SelfTest.h) ----
// Screen pacing is deliberately slow so a human can actually read each
// result — every hold can be skipped early by pressing the BOOT button.
constexpr unsigned long SPLASH_HOLD_MS             = 3000;   // splash banner hold
constexpr unsigned long SELFTEST_WIFI_TIMEOUT_MS   = 15000;  // max wait for first WiFi connect
constexpr unsigned long SELFTEST_MIC_SAMPLE_MS     = 1500;   // how long to sample mic data
constexpr unsigned long SELFTEST_STEP_PAUSE_MS     = 700;    // pause after each result row lands
constexpr unsigned long SELFTEST_CHECKLIST_HOLD_MS = 10000;  // finished checklist hold (button skips)
constexpr unsigned long SELFTEST_SUMMARY_HOLD_MS   = 15000;  // summary screen hold (button skips)

// ---- ADC button ladder (MENU/PLAY/UP/DN share GPIO1, see pins_config.h) ----
// [UNCONFIRMED — calibrate via pincheck] Non-overlapping mV windows per
// button. Anything below ADC_BTN_PRESS_MAX_MV counts as "some button down";
// a press that matches no window is logged with its measured mV so the
// window can be corrected from real hardware.
constexpr int ADC_BTN_PRESS_MAX_MV = 2900;  // idle (none pressed) reads ~3100
constexpr int ADC_BTN_UP_MIN_MV    = 150,  ADC_BTN_UP_MAX_MV    = 600;
constexpr int ADC_BTN_DOWN_MIN_MV  = 650,  ADC_BTN_DOWN_MAX_MV  = 1150;
constexpr int ADC_BTN_PLAY_MIN_MV  = 1650, ADC_BTN_PLAY_MAX_MV  = 2200;
constexpr int ADC_BTN_MENU_MIN_MV  = 2250, ADC_BTN_MENU_MAX_MV  = 2750;
constexpr unsigned long ADC_BTN_POLL_MS = 15;  // 2 stable polls = a press

// ---- Volume (UP/DN buttons, software gain on TTS playback) ----
constexpr uint8_t VOLUME_DEFAULT = 70;   // percent, persisted in NVS
constexpr uint8_t VOLUME_STEP    = 10;
constexpr unsigned long VOLUME_OVERLAY_MS = 1500;  // bar hold before face returns

// ---- State machine ----
// Give up on THINKING (waiting for the engine's response) after this long
// and return to IDLE, so a dead connection can't wedge the device.
// (Sized for the REST engine too: one generateContent round trip with
// audio input usually takes 3–8 s, occasionally more.)
constexpr unsigned long THINKING_TIMEOUT_MS = 25000;

// End a LISTENING round once the user has been silent (no frame above
// VAD_RMS_THRESHOLD) for this long. WS drops do NOT end the round — only
// user silence or this board's absolute backstop below.
constexpr unsigned long LISTENING_SILENCE_TIMEOUT_MS = 15000;

// Voice-gated uplink: mic chunks are streamed only while voice was heard
// within this window. Streaming silence continuously (~43 KB/s of TLS)
// congests a weak uplink until the WS heartbeat starves — the cause of
// drops during long listening rounds. The window must include enough
// trailing silence for the server VAD to endpoint an utterance (~1 s);
// when it lapses the engine is told the pause is intentional
// (audioStreamEnd) and the uplink resumes on the next voiced frame.
constexpr unsigned long UPLINK_SILENCE_HANGOVER_MS = 2000;
// Absolute cap on one LISTENING round — backstop for a mic whose noise
// floor sits above the VAD threshold (would otherwise never look silent).
constexpr unsigned long LISTENING_MAX_MS = 120000;

// ---- VAD (local signal-liveness only — both engines run the real
// VAD/endpointing server-side on the streamed audio) ----
constexpr float         VAD_RMS_THRESHOLD      = 500.0f;
constexpr unsigned long VAD_SILENCE_TIMEOUT_MS = 1200;
