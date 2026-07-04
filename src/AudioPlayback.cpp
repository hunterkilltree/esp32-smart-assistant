#include "AudioPlayback.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>
#include <opus.h>

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

constexpr size_t PLAYBACK_RINGBUF_BYTES     = 32 * 1024;
constexpr TickType_t PLAYBACK_WRITE_TIMEOUT = pdMS_TO_TICKS(20);

// Ring buffer items: small header + payload, so Opus frames (main flow) and
// raw PCM (pin-check tone test) can share one queue.
enum : uint8_t { ITEM_PCM = 0, ITEM_OPUS = 1 };
struct __attribute__((packed)) ItemHeader {
  uint8_t kind;
  uint16_t len;
};

RingbufHandle_t s_ringBuf = nullptr;
OpusDecoder *s_decoder = nullptr;
volatile uint32_t s_pendingRate = AUDIO_SAMPLE_RATE;
uint32_t s_currentRate = AUDIO_SAMPLE_RATE;
volatile bool s_clearRequested = false;
volatile uint8_t s_volume = VOLUME_DEFAULT;  // percent

// Decoded PCM scratch: 120ms at 24kHz of headroom over the expected 60ms.
constexpr int DECODE_MAX_SAMPLES = 2880;

void enqueueItem(uint8_t kind, const uint8_t *data, size_t len) {
  if (s_ringBuf == nullptr || len == 0) return;
  static uint8_t tmp[sizeof(ItemHeader) + OPUS_MAX_FRAME_BYTES];
  if (len > OPUS_MAX_FRAME_BYTES) {
    Serial.printf("[Playback] item too large (%u bytes), dropped\n", (unsigned)len);
    return;
  }
  ItemHeader hdr = {kind, (uint16_t)len};
  memcpy(tmp, &hdr, sizeof(hdr));
  memcpy(tmp + sizeof(hdr), data, len);
  if (xRingbufferSend(s_ringBuf, tmp, sizeof(hdr) + len, PLAYBACK_WRITE_TIMEOUT) != pdTRUE) {
    Serial.println("[Playback] Ring buffer full, dropped audio frame");
  }
}

// Decode also runs in its own task: libopus needs ~25KB of stack.
void playbackTask(void *) {
#if HAS_SPEAKER
  static int16_t pcm[DECODE_MAX_SAMPLES];
#endif

  for (;;) {
#if HAS_SPEAKER
    // Apply a server-announced sample rate change between frames.
    if (s_pendingRate != s_currentRate) {
      s_currentRate = s_pendingRate;
      i2s_set_sample_rates(I2S_NUM_0, s_currentRate);
      if (s_decoder != nullptr) {
        opus_decoder_destroy(s_decoder);
        s_decoder = nullptr;
      }
      Serial.printf("[Playback] sample rate -> %u\n", (unsigned)s_currentRate);
    }
#endif

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

    auto *hdr = reinterpret_cast<ItemHeader *>(item);
    uint8_t *payload = reinterpret_cast<uint8_t *>(item) + sizeof(ItemHeader);
    size_t bytesWritten = 0;

    if (hdr->kind == ITEM_PCM) {
      i2s_write(I2S_NUM_0, payload, hdr->len, &bytesWritten, portMAX_DELAY);
    } else {
      if (s_decoder == nullptr) {
        int err = 0;
        s_decoder = opus_decoder_create(s_currentRate, AUDIO_CHANNELS, &err);
        if (err != OPUS_OK || s_decoder == nullptr) {
          Serial.printf("[Opus] decoder create failed: %d\n", err);
          s_decoder = nullptr;
          vRingbufferReturnItem(s_ringBuf, item);
          continue;
        }
      }
      int samples = opus_decode(s_decoder, payload, hdr->len, pcm,
                                DECODE_MAX_SAMPLES, 0);
      if (samples > 0) {
        uint8_t vol = s_volume;
        if (vol != 100) {
          for (int i = 0; i < samples; i++) {
            pcm[i] = (int16_t)(((int32_t)pcm[i] * vol) / 100);
          }
        }
        i2s_write(I2S_NUM_0, pcm, (size_t)samples * sizeof(int16_t),
                  &bytesWritten, portMAX_DELAY);
      } else {
        Serial.printf("[Opus] decode failed: %d\n", samples);
      }
    }
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
      .sample_rate = AUDIO_SAMPLE_RATE,
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

  xTaskCreatePinnedToCore(playbackTask, "audio_playback", 32768, nullptr, 5, nullptr, 1);
}

void audioPlaybackConfigure(uint32_t sampleRate) {
  s_pendingRate = sampleRate;
}

void audioPlaybackWriteOpus(const uint8_t *data, size_t len) {
  enqueueItem(ITEM_OPUS, data, len);
}

void audioPlaybackWrite(const uint8_t *data, size_t len) {
  // Raw PCM can exceed the per-item cap; split it.
  while (len > 0) {
    size_t chunk = len > OPUS_MAX_FRAME_BYTES ? OPUS_MAX_FRAME_BYTES : len;
    enqueueItem(ITEM_PCM, data, chunk);
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
