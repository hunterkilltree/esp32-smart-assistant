#include "CameraCapture.h"

#include <Arduino.h>
#include <esp_camera.h>

#include "pins_config.h"

namespace {
bool s_cameraReady = false;
}

bool cameraCaptureInit() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_Y2;
  config.pin_d1 = CAM_PIN_Y3;
  config.pin_d2 = CAM_PIN_Y4;
  config.pin_d3 = CAM_PIN_Y5;
  config.pin_d4 = CAM_PIN_Y6;
  config.pin_d5 = CAM_PIN_Y7;
  config.pin_d6 = CAM_PIN_Y8;
  config.pin_d7 = CAM_PIN_Y9;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[Camera] Init failed: 0x%x\n", err);
    s_cameraReady = false;
    return false;
  }

  s_cameraReady = true;
  return true;
}

bool cameraCaptureSnapshot(void (*onFrame)(const uint8_t *jpegData, size_t len)) {
  if (!s_cameraReady) return false;

  camera_fb_t *fb = esp_camera_fb_get();
  if (fb == nullptr) {
    Serial.println("[Camera] Snapshot capture failed");
    return false;
  }

  if (onFrame != nullptr) {
    onFrame(fb->buf, fb->len);
  }

  esp_camera_fb_return(fb);
  return true;
}
