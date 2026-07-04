// Standalone diagnostic firmware for the esp32-s3-eye-pincheck environment.
// Runs each pin/peripheral test in isolation with an obvious expected
// result, printed to Serial, so a human can confirm it against real
// hardware before that pin gets locked in pins_config.h.
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Bounce2.h>
#include <math.h>

#include "config.h"
#include "pins_config.h"
#include "AdcButtons.h"
#include "Display.h"
#include "AudioCapture.h"
#include "AudioPlayback.h"
#include "CameraCapture.h"

namespace {

Adafruit_NeoPixel statusLed(1, PIN_RGB_LED, NEO_GRB + NEO_KHZ800);
Bounce2::Button wakeButton;

void testLed() {
  Serial.println("\n[PinCheck] LED test: expect RED -> GREEN -> BLUE -> OFF, ~1s each (PIN_RGB_LED)");
  statusLed.setPixelColor(0, statusLed.Color(64, 0, 0));
  statusLed.show();
  delay(1000);
  statusLed.setPixelColor(0, statusLed.Color(0, 64, 0));
  statusLed.show();
  delay(1000);
  statusLed.setPixelColor(0, statusLed.Color(0, 0, 64));
  statusLed.show();
  delay(1000);
  statusLed.setPixelColor(0, statusLed.Color(0, 0, 0));
  statusLed.show();
}

void testDisplay() {
  Serial.println("[PinCheck] LCD test: expect backlight ON and text \"LCD OK\" visible (PIN_LCD_*)");
  displayShowStatus("LCD OK");
  delay(1500);
}

void testButtonPin() {
  Serial.println("[PinCheck] Button test: press the wake button now (5s window, PIN_BUTTON)...");
  displayShowStatus("PRESS BTN");
  unsigned long start = millis();
  bool pressed = false;
  while (millis() - start < 5000) {
    wakeButton.update();
    if (wakeButton.pressed()) {
      Serial.println("[PinCheck] Button PRESSED -- detected OK");
      displayShowStatus("BTN: YES");
      pressed = true;
      delay(1000);
      break;
    }
  }
  if (!pressed) {
    displayShowStatus("BTN: NO");
    delay(500);
    Serial.println("[PinCheck] No press detected in window (expected if you didn't press it)");
  }
}

void testMic() {
  // Event-triggered rather than "watch a number for N seconds": easier to
  // confirm at a glance, no need to time the clap against a narrow window.
  //
  // Uses PEAK amplitude (max |sample| in the chunk), not RMS: a clap is a
  // brief transient, and averaging it into a full ~32ms chunk's RMS can
  // wash it out. Peak reacts immediately to a single loud sample.
  //
  // Threshold is RELATIVE to a slowly-tracked ambient peak floor rather
  // than a fixed guess — a fixed absolute RMS threshold (300) didn't
  // trigger, and rather than guess a second fixed number blind, this
  // adapts to whatever this specific mic's actual idle level turns out
  // to be.
  constexpr float CLAP_RATIO = 2.5f;    // clap must be this many times the ambient floor
  constexpr float CLAP_MARGIN = 50.0f;  // ...plus an absolute floor so near-silence can't "jump" 2.5x nothing
  constexpr unsigned long CLAP_HOLD_MS = 800;

  Serial.println("[PinCheck] Mic test: clap or make a loud noise near the mic -- LCD should flash CLAP! (PIN_MIC_I2S_*)");
  displayShowStatus("CLAP TEST");
  delay(500);

  audioCaptureStart();
  unsigned long start = millis();
  unsigned long lastDisplayUpdate = 0;
  bool clapShown = false;
  unsigned long clapShownAt = 0;
  float baseline = -1.0f;  // seeded from the first reading
  static uint8_t chunk[AUDIO_CHUNK_BYTES];
  char rmsText[16];
  while (millis() - start < 8000) {
    if (audioCaptureDequeueChunk(chunk)) {
      auto *samples = reinterpret_cast<int16_t *>(chunk);
      size_t n = AUDIO_CHUNK_BYTES / sizeof(int16_t);
      int16_t peak = 0;
      for (size_t i = 0; i < n; i++) {
        int16_t mag = (int16_t)abs((int)samples[i]);
        if (mag > peak) peak = mag;
      }
      Serial.printf("[PinCheck] Mic peak: %d (baseline %.1f)\n", peak, baseline);

      if (baseline < 0) {
        baseline = (float)peak;
      }

      unsigned long now = millis();
      if ((float)peak > baseline * CLAP_RATIO + CLAP_MARGIN) {
        Serial.println("[PinCheck] CLAP DETECTED");
        displayShowStatus("CLAP!");
        
        delay(500);
        clapShown = true;
        clapShownAt = now;
      } else if (clapShown && now - clapShownAt > CLAP_HOLD_MS) {
        clapShown = false;
      }

      // Slow-moving floor so the trigger tracks the room, not a guessed constant.
      baseline = baseline * 0.95f + (float)peak * 0.05f;

      // Throttled so digits are readable instead of flickering every ~32ms chunk.
      if (!clapShown && now - lastDisplayUpdate >= 200) {
        lastDisplayUpdate = now;
        snprintf(rmsText, sizeof(rmsText), "PK:%d", (int)peak);
        displayShowStatus(rmsText);
      }
    }
  }
  audioCaptureStop();
}

void testAdcButtons() {
  // Calibration test for the 4-button ADC ladder on GPIO1: shows the live
  // millivolt reading, and the button name once a press matches one of the
  // config.h ADC_BTN_* windows. If a press shows only a changing mV value
  // but no name, that measured value IS the calibration data — copy it
  // into the matching window in config.h.
  Serial.println("[PinCheck] ADC buttons test: press MENU / PLAY / UP / DN (8s window, PIN_ADC_BUTTONS)...");
  displayShowStatus("PRESS 4BTN");
  delay(500);

  unsigned long start = millis();
  unsigned long lastDisplayUpdate = 0;
  char text[20];
  while (millis() - start < 8000) {
    AdcButton b = adcButtonsPoll();
    unsigned long now = millis();
    if (b != AdcButton::NONE) {
      snprintf(text, sizeof(text), "%s %dmV", adcButtonName(b), adcButtonsLastMv());
      Serial.printf("[PinCheck] ADC button: %s\n", text);
      displayShowStatus(text);
      lastDisplayUpdate = now + 800;  // hold the hit on screen briefly
    } else if (now > lastDisplayUpdate && now - start > 600) {
      lastDisplayUpdate = now + 250;
      snprintf(text, sizeof(text), "mV:%d", adcButtonsLastMv());
      displayShowStatus(text);
    }
    delay(5);
  }
}

void testSpeaker() {
  Serial.println("[PinCheck] Speaker test: writing a 1kHz tone burst -- expect an audible tone ONLY if an "
                  "external amp is wired (PIN_SPK_I2S_*); silence is expected otherwise since this board "
                  "has no official speaker interface");
  static int16_t tone[256];
  for (int i = 0; i < 256; i++) {
    tone[i] = (int16_t)(8000.0f * sinf(2.0f * (float)M_PI * 1000.0f * i / AUDIO_SAMPLE_RATE));
  }
  for (int rep = 0; rep < 60; rep++) {
    audioPlaybackWrite(reinterpret_cast<uint8_t *>(tone), sizeof(tone));
  }
  delay(1000);
}

void testCamera() {
  Serial.println("[PinCheck] Camera test: capturing one JPEG frame (CAM_PIN_*)...");
  bool ok = cameraCaptureSnapshot([](const uint8_t *data, size_t len) {
    (void)data;
    Serial.printf("[PinCheck] Camera OK -- captured %u bytes\n", (unsigned)len);
  });
  if (!ok) {
    Serial.println("[PinCheck] Camera capture FAILED");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n===== ESP32-S3-EYE PIN CHECK =====");

  displayInit();

  statusLed.begin();
  statusLed.setBrightness(40);

  wakeButton.attach(PIN_BUTTON, INPUT_PULLUP);
  wakeButton.interval(BUTTON_DEBOUNCE_MS);
  wakeButton.setPressedState(LOW);

  adcButtonsInit();
  audioCaptureInit();
  audioPlaybackInit();
  if (!cameraCaptureInit()) {
    Serial.println("[PinCheck] Camera init FAILED");
  }
}

void loop() {
  testLed();
  testDisplay();
  testButtonPin();
  testAdcButtons();
  testMic();
  testSpeaker();
  testCamera();

  Serial.println("===== PIN CHECK COMPLETE -- repeating in 5s =====\n");
  delay(5000);
}
