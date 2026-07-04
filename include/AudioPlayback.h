#pragma once
#include <cstddef>
#include <cstdint>

// Installs the I2S TX driver (external amp) and starts the streaming
// playback/decode task. Call once from setup().
void audioPlaybackInit();

// Sets the downlink sample rate announced by the server hello (24kHz on the
// official xiaozhi backend). The playback task reconfigures I2S and
// recreates the Opus decoder on the next frame. Safe to call repeatedly.
void audioPlaybackConfigure(uint32_t sampleRate);

// Queues one encoded Opus frame (as received in a WS binary message) for
// decode + playback. Non-blocking; drops and logs if the buffer is full.
void audioPlaybackWriteOpus(const uint8_t *data, size_t len);

// Queues raw mono 16-bit PCM bytes for playback, bypassing the decoder.
// Kept for the pin-check speaker tone test.
void audioPlaybackWrite(const uint8_t *data, size_t len);

// Discards everything queued but not yet played (barge-in / abort).
void audioPlaybackClear();

// Software gain (0–100%) applied to decoded TTS samples before I2S write.
// Persistence is the caller's job (Controls saves it to NVS).
void audioPlaybackSetVolume(uint8_t percent);
uint8_t audioPlaybackGetVolume();
