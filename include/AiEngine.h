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
// The model is given a set_emotion(emotion, text) tool — emotion one of
// happy|sad|neutral|thinking, text a short on-screen caption — and a
// system prompt telling it to call the tool before every reply — that tool
// call drives the face + caption on the LCD (onEmotion).
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
  // set_emotion tool call: emotion is "happy" | "sad" | "neutral" |
  // "thinking"; text is a short caption to show on the LCD (may be "").
  void (*onEmotion)(const char *emotion, const char *text);
  // The `speech` argument of the set_emotion tool call: the complete text
  // of the reply the model is about to speak. Arrives once, at the START
  // of the reply — the authoritative full text for the speak server.
  void (*onReplyText)(const char *text);
  // One chunk of the streamed transcript of the spoken reply (UTF-8,
  // NUL-terminated, in order). Fallback source: chunks at the tail can be
  // lost if the socket drops mid-reply.
  void (*onTranscript)(const char *text);
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

// Tells the engine the mic stream is deliberately pausing (local VAD found
// silence) so the gap isn't mistaken for a stall. Sending audio again
// resumes the stream. Gemini: realtimeInput.audioStreamEnd; OpenAI: no-op
// (server-VAD sessions tolerate uplink gaps).
void aiEngineSendAudioStreamEnd();

// Called when local VAD decides the utterance is over. Request/response
// engines (GEMINI_REST) submit the recorded audio and return true — the
// caller should move to THINKING and await the reply via the callbacks.
// Streaming engines return false (endpointing is server-side; nothing to
// commit).
bool aiEngineCommitUtterance();

// Cancels the in-flight model response (button barge-in). Local playback
// must be cleared by the caller.
void aiEngineAbort();

// "Gemini Live" or "OpenAI Realtime" — for the info screen / boot summary.
const char *aiEngineName();
