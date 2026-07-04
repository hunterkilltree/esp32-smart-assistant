#include "AudioPlayback.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>

#include "config.h"
#include "pins_config.h"

// No speaker pins defined (no external amp wired yet — see pins_config.h):
// build without the I2S TX driver; queued audio is consumed and discarded
// so the rest of the pipeline behaves identically.
#if defined(PIN_SPK_I2S_BCLK) && defined(PIN_SPK_I2S_LRC) && defined(PIN_SPK_I2S_DOUT)
#define HAS_SPEAKER 1
#else
#define HAS_SPEAKER 0
#endif

namespace {

// 64 KB buffers ~0.7s of 24 kHz PCM. The engines stream TTS slightly
// faster than realtime, so bursts beyond this are dropped with a log —
// bump this (or move the buffer to PSRAM) if drops show up once an amp
// is actually wired.
constexpr size_t PLAYBACK_RINGBUF_BYTES     = 64 * 1024;
constexpr size_t PLAYBACK_ITEM_MAX_BYTES    = 2048;
constexpr TickType_t PLAYBACK_WRITE_TIMEOUT = pdMS_TO_TICKS(20);

RingbufHandle_t s_ringBuf = nullptr;
volatile bool s_clearRequested = false;
volatile uint8_t s_volume = VOLUME_DEFAULT;  // percent

void playbackTask(void *) {
  for (;;) {
    size_t itemLen = 0;
    void *item = xRingbufferReceive(s_ringBuf, &itemLen, pdMS_TO_TICKS(100));
    if (item == nullptr) continue;

#if !HAS_SPEAKER
    // No amp wired: consume and discard so the ring buffer never fills.
    vRingbufferReturnItem(s_ringBuf, item);
    s_clearRequested = false;
    continue;
#else

    if (s_clearRequested) {
      // Drain without playing until the queue is empty.
      vRingbufferReturnItem(s_ringBuf, item);
      size_t drainLen = 0;
      void *drain;
      while ((drain = xRingbufferReceive(s_ringBuf, &drainLen, 0)) != nullptr) {
        vRingbufferReturnItem(s_ringBuf, drain);
      }
      s_clearRequested = false;
      i2s_zero_dma_buffer(I2S_NUM_0);
      continue;
    }

    auto *samples = reinterpret_cast<int16_t *>(item);
    size_t count = itemLen / sizeof(int16_t);
    uint8_t vol = s_volume;
    if (vol != 100) {
      for (size_t i = 0; i < count; i++) {
        samples[i] = (int16_t)(((int32_t)samples[i] * vol) / 100);
      }
    }
    size_t bytesWritten = 0;
    i2s_write(I2S_NUM_0, samples, itemLen, &bytesWritten, portMAX_DELAY);
    vRingbufferReturnItem(s_ringBuf, item);
#endif  // HAS_SPEAKER
  }
}

}  // namespace

void audioPlaybackInit() {
  // Uses I2S_NUM_0 (swapped from I2S_NUM_1 2026-07-03): the mic needed
  // I2S_NUM_1 specifically to match espressif/esp-skainet's proven working
  // driver for this board. Since the speaker has no official pin/port
  // assignment at all (no built-in speaker on this board), it's free to
  // use whichever port the mic doesn't need.
  s_ringBuf = xRingbufferCreate(PLAYBACK_RINGBUF_BYTES, RINGBUF_TYPE_NOSPLIT);

#if !HAS_SPEAKER
  Serial.println("[Playback] No speaker pins defined — TTS audio will be discarded");
#else
  i2s_config_t i2sConfig = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = PLAYBACK_SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 4,
      .dma_buf_len = 512,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0,
  };

  i2s_pin_config_t pinConfig = {
      .bck_io_num = PIN_SPK_I2S_BCLK,
      .ws_io_num = PIN_SPK_I2S_LRC,
      .data_out_num = PIN_SPK_I2S_DOUT,
      .data_in_num = I2S_PIN_NO_CHANGE,
  };

  i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, nullptr);
  i2s_set_pin(I2S_NUM_0, &pinConfig);
#endif  // HAS_SPEAKER

  xTaskCreatePinnedToCore(playbackTask, "audio_playback", 8192, nullptr, 5, nullptr, 1);
}

void audioPlaybackWrite(const uint8_t *data, size_t len) {
  if (s_ringBuf == nullptr) return;
  // Split into ring items the playback task consumes one at a time.
  while (len > 0) {
    size_t chunk = len > PLAYBACK_ITEM_MAX_BYTES ? PLAYBACK_ITEM_MAX_BYTES : len;
    if (xRingbufferSend(s_ringBuf, data, chunk, PLAYBACK_WRITE_TIMEOUT) != pdTRUE) {
      Serial.println("[Playback] Ring buffer full, dropped audio");
      return;
    }
    data += chunk;
    len -= chunk;
  }
}

void audioPlaybackClear() {
  s_clearRequested = true;
}

void audioPlaybackSetVolume(uint8_t percent) {
  s_volume = percent > 100 ? 100 : percent;
}

uint8_t audioPlaybackGetVolume() {
  return s_volume;
}
