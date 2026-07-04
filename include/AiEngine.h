#pragma once
#include <cstddef>
#include <cstdint>

// AI engine client — one realtime voice WebSocket session straight to the
// provider (no middle backend). The engine is chosen at compile time via
// AI_ENGINE in secrets.h:
//   AI_ENGINE_GEMINI — Gemini Live API   (src/GeminiLive.cpp)
//   AI_ENGINE_OPENAI — OpenAI Realtime API (src/OpenAiRealtime.cpp)
//
// Both engines speak raw little-endian mono 16-bit PCM (no codec):
//   uplink  — mic PCM at AUDIO_SAMPLE_RATE (16 kHz Gemini, 24 kHz OpenAI),
//             base64 inside JSON messages
//   downlink — TTS PCM at PLAYBACK_SAMPLE_RATE (24 kHz both)
//
// The model is given a set_emotion(happy|sad|neutral|thinking) tool and a
// system prompt telling it to call the tool before every reply — that tool
// call drives the face on the LCD (onEmotion).
//
// All calls, including the callbacks, happen on the Arduino loop task.

struct AiEngineCallbacks {
  // Session configured and ready for conversation.
  void (*onReady)();
  void (*onDisconnected)();
  // One chunk of decoded TTS audio: mono 16-bit PCM at PLAYBACK_SAMPLE_RATE.
  void (*onAudio)(const int16_t *pcm, size_t samples);
  // Server-side VAD finished an utterance — the model is thinking now.
  // (OpenAI only; Gemini has no explicit endpoint event.)
  void (*onUserSpeechEnd)();
  // The model finished its spoken reply.
  void (*onTurnComplete)();
  // The user barged in over the model's reply — discard queued playback.
  void (*onInterrupted)();
  // set_emotion tool call: "happy" | "sad" | "neutral" | "thinking".
  void (*onEmotion)(const char *emotion);
};

// Registers callbacks. Call once from setup(), before aiEngineConnect().
void aiEngineInit(const AiEngineCallbacks &cbs);

// Starts connecting (non-blocking; serviced by aiEngineLoop()). The
// WebSockets library auto-reconnects a dropped socket; the session setup
// handshake re-runs on every (re)connect.
void aiEngineConnect();

// Pump. Call every loop() iteration after aiEngineConnect().
void aiEngineLoop();

bool aiEngineSocketConnected();  // TCP/WS link up
bool aiEngineReady();            // session setup completed

// Streams one chunk of mic PCM (mono 16-bit at AUDIO_SAMPLE_RATE).
// No-op until aiEngineReady().
void aiEngineSendAudio(const int16_t *pcm, size_t samples);

// Cancels the in-flight model response (button barge-in). Local playback
// must be cleared by the caller.
void aiEngineAbort();

// "Gemini Live" or "OpenAI Realtime" — for the info screen / boot summary.
const char *aiEngineName();
