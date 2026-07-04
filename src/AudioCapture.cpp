#include "AudioCapture.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <math.h>
#include <opus.h>

#include "config.h"
#include "pins_config.h"

namespace {

QueueHandle_t s_pcmQueue = nullptr;
QueueHandle_t s_opusQueue = nullptr;
volatile bool s_capturing = false;
volatile bool s_opusEnabled = false;
volatile bool s_silenceTimeoutHit = false;
OpusEncoder *s_encoder = nullptr;

// One encoded uplink frame. 60ms of 16kHz mono speech at ~24kbps VBR is
// ~180 bytes; the cap leaves generous headroom.
struct OpusFrame {
  uint16_t len;
  uint8_t data[OPUS_MAX_FRAME_BYTES];
};

void captureTask(void *) {
  // 32-bit read + >>14 shift, matching espressif/esp-skainet's actual
  // working mic driver for this board (boards/esp32s3-eye/bsp_board.c) —
  // not the generic esp-bsp reference, which is for the multi-purpose
  // codec API and turned out not to be what this board's real wake-word
  // firmware uses. Read raw 32-bit words, narrow to the existing
  // int16_t pipeline below.
  static int32_t raw[AUDIO_CHUNK_SAMPLES];
  static int16_t buf[AUDIO_CHUNK_SAMPLES];
  unsigned long lastVoiceMs = millis();

  for (;;) {
    if (!s_capturing) {
      vTaskDelay(pdMS_TO_TICKS(20));
      lastVoiceMs = millis();
      continue;
    }

    size_t bytesRead = 0;
    i2s_read(I2S_NUM_1, raw, sizeof(raw), &bytesRead, portMAX_DELAY);
    if (bytesRead < sizeof(raw)) continue;  // Opus needs exactly full frames

    for (size_t i = 0; i < AUDIO_CHUNK_SAMPLES; i++) {
      buf[i] = (int16_t)(raw[i] >> 14);
    }

    // Energy-based VAD: RMS over the frame. Diagnostic only — the xiaozhi
    // backend does the real endpointing server-side in "auto" listen mode.
    int64_t sumSquares = 0;
    for (size_t i = 0; i < AUDIO_CHUNK_SAMPLES; i++) {
      sumSquares += (int32_t)buf[i] * (int32_t)buf[i];
    }
    float rms = sqrtf((float)sumSquares / (float)AUDIO_CHUNK_SAMPLES);

    unsigned long now = millis();
    if (rms > VAD_RMS_THRESHOLD) {
      lastVoiceMs = now;
      s_silenceTimeoutHit = false;
    } else if (now - lastVoiceMs > VAD_SILENCE_TIMEOUT_MS) {
      s_silenceTimeoutHit = true;
    }

    xQueueSend(s_pcmQueue, buf, 0);
  }
}

// Opus encode runs in its own task: libopus needs ~25KB of stack, far more
// than the capture task or the Arduino loop task have.
void encodeTask(void *) {
  static int16_t pcm[AUDIO_CHUNK_SAMPLES];
  static OpusFrame frame;

  for (;;) {
    if (!s_opusEnabled) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (xQueueReceive(s_pcmQueue, pcm, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    if (s_encoder == nullptr) continue;

    opus_int32 n = opus_encode(s_encoder, pcm, AUDIO_CHUNK_SAMPLES, frame.data,
                               sizeof(frame.data));
    if (n <= 0) {
      Serial.printf("[Opus] encode failed: %d\n", (int)n);
      continue;
    }
    frame.len = (uint16_t)n;
    if (xQueueSend(s_opusQueue, &frame, 0) != pdTRUE) {
      Serial.println("[Opus] uplink queue full, dropped frame");
    }
  }
}

}  // namespace

void audioCaptureInit() {
  s_pcmQueue = xQueueCreate(4, AUDIO_CHUNK_BYTES);
  s_opusQueue = xQueueCreate(4, sizeof(OpusFrame));

  int err = 0;
  s_encoder = opus_encoder_create(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS,
                                  OPUS_APPLICATION_VOIP, &err);
  if (err != OPUS_OK || s_encoder == nullptr) {
    Serial.printf("[Opus] encoder create failed: %d\n", err);
    s_encoder = nullptr;
  } else {
    // Complexity 3 keeps a 60ms frame well under its real-time budget on
    // the S3 at 240MHz; VBR ~24kbps is plenty for 16kHz speech ASR.
    opus_encoder_ctl(s_encoder, OPUS_SET_COMPLEXITY(3));
    opus_encoder_ctl(s_encoder, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(s_encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(s_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  }

  i2s_config_t i2sConfig = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = AUDIO_SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // matches esp-skainet's actual working mic driver
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // confirmed correct 2026-07-03: ONLY_RIGHT reads pure
                                                      // zero/dead silence, ONLY_LEFT has a real (if quiet) signal
      .communication_format = I2S_COMM_FORMAT_STAND_MSB,  // [EXPERIMENT] was STAND_I2S — if the mic
                                                             // uses left-justified/MSB timing instead of
                                                             // true Philips I2S (1-bit WS/data delay),
                                                             // STAND_I2S reads every sample shifted by
                                                             // one bit, which could explain a present-
                                                             // but-uncorrelated-with-loudness signal
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 6,
      .dma_buf_len = 480,  // 960-sample frames span two DMA buffers
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0,
  };

  i2s_pin_config_t pinConfig = {
      .bck_io_num = PIN_MIC_I2S_SCK,
      .ws_io_num = PIN_MIC_I2S_WS,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = PIN_MIC_I2S_SD,
  };

  i2s_driver_install(I2S_NUM_1, &i2sConfig, 0, nullptr);
  i2s_set_pin(I2S_NUM_1, &pinConfig);

  xTaskCreatePinnedToCore(captureTask, "audio_capture", 4096, nullptr, 5, nullptr, 1);
  xTaskCreatePinnedToCore(encodeTask, "opus_encode", 32768, nullptr, 4, nullptr, 1);
}

void audioCaptureStart() {
  s_silenceTimeoutHit = false;
  s_capturing = true;
}

void audioCaptureStop() {
  s_capturing = false;
}

void audioCaptureEnableOpus(bool enabled) {
  s_opusEnabled = enabled;
}

bool audioCaptureDequeueChunk(uint8_t *outBuf) {
  return xQueueReceive(s_pcmQueue, outBuf, 0) == pdTRUE;
}

size_t audioCaptureDequeueOpus(uint8_t *outBuf, size_t cap) {
  static OpusFrame frame;
  if (xQueueReceive(s_opusQueue, &frame, 0) != pdTRUE) return 0;
  if (frame.len > cap) return 0;
  memcpy(outBuf, frame.data, frame.len);
  return frame.len;
}

bool audioCaptureSilenceTimeoutHit() {
  return s_silenceTimeoutHit;
}
