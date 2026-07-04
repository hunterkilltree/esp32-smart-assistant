#pragma once
#include <cstddef>
#include <cstdint>

// Installs the I2S TX driver (external amp, PLAYBACK_SAMPLE_RATE) and
// starts the streaming playback task. Call once from setup().
void audioPlaybackInit();

// Queues raw mono 16-bit PCM bytes for playback. Non-blocking; drops and
// logs if the buffer is full. Used for engine TTS audio and the pin-check
// speaker tone test alike.
void audioPlaybackWrite(const uint8_t *data, size_t len);

// Discards everything queued but not yet played (barge-in / abort).
void audioPlaybackClear();

// Software gain (0–100%) applied to samples before the I2S write.
// Persistence is the caller's job (Controls saves it to NVS).
void audioPlaybackSetVolume(uint8_t percent);
uint8_t audioPlaybackGetVolume();
