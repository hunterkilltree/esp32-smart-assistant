#pragma once
#include <cstddef>
#include <cstdint>

#include "config.h"

// Mono 16-bit PCM frame read from the mic: one uplink chunk's worth of
// samples at the engine's input rate (16 kHz Gemini / 24 kHz OpenAI).
constexpr size_t AUDIO_CHUNK_SAMPLES =
    AUDIO_SAMPLE_RATE * AUDIO_CHUNK_MS / 1000;
constexpr size_t AUDIO_CHUNK_BYTES = AUDIO_CHUNK_SAMPLES * sizeof(int16_t);

// Installs the I2S RX driver and starts the background capture task.
// Call once from setup().
void audioCaptureInit();

// Enables/disables continuous PCM capture. The capture task keeps running
// but only pushes frames to the queue while enabled, gated by assistant
// state (LISTENING) in AppState.
void audioCaptureStart();
void audioCaptureStop();

// Non-blocking. Copies the next queued raw PCM frame into outBuf (must be
// AUDIO_CHUNK_BYTES). Returns true if a frame was available. Consumed by
// the Conversation uplink pump in the main flow, and directly by the
// self-test / pin-check diagnostics (never both at once).
bool audioCaptureDequeueChunk(uint8_t *outBuf);

// True once trailing silence (energy-based VAD) has exceeded the
// configured timeout. Diagnostic only — endpointing is done server-side
// by the AI engine on the streamed audio.
bool audioCaptureSilenceTimeoutHit();

// Milliseconds since the last frame whose RMS crossed VAD_RMS_THRESHOLD.
// Pinned near 0 while capture is disabled — only grows during a round.
// Used to end LISTENING after prolonged user silence.
unsigned long audioCaptureMsSinceVoice();
