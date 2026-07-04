#pragma once
#include <cstddef>
#include <cstdint>

// Mono 16-bit PCM frame read from the mic: one 60ms Opus frame's worth of
// samples at 16kHz (the xiaozhi protocol's uplink framing).
constexpr size_t AUDIO_CHUNK_SAMPLES = 960;
constexpr size_t AUDIO_CHUNK_BYTES   = AUDIO_CHUNK_SAMPLES * sizeof(int16_t);

// Installs the I2S RX driver, creates the Opus encoder, and starts the
// background capture + encode tasks. Call once from setup().
void audioCaptureInit();

// Enables/disables continuous PCM capture. The capture task keeps running
// but only pushes frames to the queue while enabled, gated by assistant
// state (LISTENING) in main.cpp.
void audioCaptureStart();
void audioCaptureStop();

// Routes captured PCM through the Opus encoder task into the encoded-frame
// queue (main flow). Leave disabled for the self-test / pin-check paths,
// which consume raw PCM via audioCaptureDequeueChunk() instead — the two
// consumers must not compete for the same PCM queue.
void audioCaptureEnableOpus(bool enabled);

// Non-blocking. Copies the next queued raw PCM frame into outBuf (must be
// AUDIO_CHUNK_BYTES). Returns true if a frame was available. Only for
// self-test / pin-check; unused while Opus routing is enabled.
bool audioCaptureDequeueChunk(uint8_t *outBuf);

// Non-blocking. Copies the next encoded Opus frame into outBuf and returns
// its byte length, or 0 if none is queued (or it exceeds cap).
size_t audioCaptureDequeueOpus(uint8_t *outBuf, size_t cap);

// True once trailing silence (energy-based VAD) has exceeded the
// configured timeout. Diagnostic only — endpointing is done server-side
// by the xiaozhi backend in "auto" listen mode.
bool audioCaptureSilenceTimeoutHit();
