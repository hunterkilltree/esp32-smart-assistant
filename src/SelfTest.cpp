#include "SelfTest.h"

#include <Arduino.h>
#include <WiFi.h>
#include <string.h>

#include "config.h"
#include "pins_config.h"
#include "AiEngine.h"
#include "Display.h"
#include "AudioCapture.h"
#include "AudioPlayback.h"
#include "CameraCapture.h"

namespace {

// Template values from secrets.h.example — if these are still in secrets.h,
// the user never filled in real credentials/keys.
constexpr char PLACEHOLDER_SSID[] = "your-wifi-ssid";
#if AI_ENGINE == AI_ENGINE_OPENAI
constexpr char PLACEHOLDER_KEY[] = "your-openai-api-key";
#define AI_API_KEY OPENAI_API_KEY
#else
constexpr char PLACEHOLDER_KEY[] = "your-gemini-api-key";
#define AI_API_KEY GEMINI_API_KEY
#endif

size_t lastSnapshotBytes = 0;

void record(SelfTestReport &report, TestStatus status, const char *detail) {
  switch (status) {
    case TestStatus::PASS: report.passed++; break;
    case TestStatus::WARN: report.warned++; break;
    case TestStatus::FAIL: report.failed++; break;
    case TestStatus::SKIP: break;
  }
  displayBootResult(status, detail);
  // Breathe between rows so results land at a human-readable rhythm
  // instead of several near-instant checks flashing by at once.
  delay(SELFTEST_STEP_PAUSE_MS);
}

// Holds the current screen for up to holdMs; a BOOT-button press skips
// ahead (waits for release so one press can't skip two screens).
void holdScreen(unsigned long holdMs) {
  unsigned long start = millis();
  while (millis() - start < holdMs) {
    if (digitalRead(PIN_BUTTON) == LOW) {
      while (digitalRead(PIN_BUTTON) == LOW) delay(10);
      break;
    }
    delay(20);
  }
}

void testPsram(SelfTestReport &report) {
  displayBootStep("PSRAM");
  if (psramFound()) {
    char detail[24];
    snprintf(detail, sizeof(detail), "%u MB detected",
             (unsigned)(ESP.getPsramSize() / (1024 * 1024)));
    Serial.printf("[SelfTest] PSRAM: %s\n", detail);
    record(report, TestStatus::PASS, detail);
  } else {
    Serial.println("[SelfTest] PSRAM: NOT FOUND (check qio_opi memory_type)");
    record(report, TestStatus::FAIL, "not found - camera will fail");
  }
}

void testLed(SelfTestReport &report, Adafruit_NeoPixel &led) {
  displayBootStep("LED");
  Serial.println("[SelfTest] LED: cycling R/G/B");
  const uint32_t colors[3] = {led.Color(64, 0, 0), led.Color(0, 64, 0),
                              led.Color(0, 0, 64)};
  for (uint32_t c : colors) {
    led.setPixelColor(0, c);
    led.show();
    delay(300);
  }
  led.setPixelColor(0, 0);
  led.show();
  // Output-only device: the test exercises it; correctness is confirmed
  // visually (and was locked in via the pin-check firmware).
  record(report, TestStatus::PASS, "R/G/B cycled");
}

void testButton(SelfTestReport &report) {
  displayBootStep("BUTTON");
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  delay(20);
  if (digitalRead(PIN_BUTTON) == HIGH) {
    Serial.println("[SelfTest] BUTTON: released (not stuck)");
    record(report, TestStatus::PASS, "released, not stuck");
  } else {
    Serial.println("[SelfTest] BUTTON: reads LOW at boot (held or stuck?)");
    record(report, TestStatus::WARN, "reads LOW - held or stuck?");
  }
}

void testConfig(SelfTestReport &report) {
  displayBootStep("CONFIG");
  bool ssidPlaceholder = (strcmp(WIFI_SSID, PLACEHOLDER_SSID) == 0) ||
                         (strlen(WIFI_SSID) == 0);
  bool keyPlaceholder = (strcmp(AI_API_KEY, PLACEHOLDER_KEY) == 0) ||
                        (strlen(AI_API_KEY) == 0);

  if (ssidPlaceholder) {
    Serial.println("[SelfTest] CONFIG: WIFI_SSID is still the template placeholder");
    record(report, TestStatus::FAIL, "secrets.h: WiFi not set");
  } else if (keyPlaceholder) {
    Serial.printf("[SelfTest] CONFIG: %s API key is still the template placeholder\n",
                  aiEngineName());
    record(report, TestStatus::FAIL, "secrets.h: AI key not set");
  } else {
    Serial.println("[SelfTest] CONFIG: secrets.h looks filled in");
    record(report, TestStatus::PASS, "secrets.h filled in");
  }
}

void testWifi(SelfTestReport &report) {
  displayBootStep("WIFI");
  Serial.printf("[SelfTest] WIFI: connecting to %s...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // no modem power-save — see WifiLink.cpp
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < SELFTEST_WIFI_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    report.wifiConnected = true;
    char detail[32];
    snprintf(detail, sizeof(detail), "IP %s", WiFi.localIP().toString().c_str());
    Serial.printf("[SelfTest] WIFI: connected, %s\n", detail);
    record(report, TestStatus::PASS, detail);
  } else {
    Serial.println("[SelfTest] WIFI: no connection within timeout");
    record(report, TestStatus::FAIL, "timeout - check credentials");
  }
}

void testMic(SelfTestReport &report) {
  displayBootStep("MIC");
  Serial.println("[SelfTest] MIC: sampling I2S data...");
  audioCaptureStart();

  static uint8_t chunk[AUDIO_CHUNK_BYTES];
  unsigned long start = millis();
  unsigned int chunks = 0;
  int16_t minSample = 0, maxSample = 0;
  while (millis() - start < SELFTEST_MIC_SAMPLE_MS) {
    if (!audioCaptureDequeueChunk(chunk)) {
      delay(5);
      continue;
    }
    chunks++;
    auto *samples = reinterpret_cast<int16_t *>(chunk);
    for (size_t i = 0; i < AUDIO_CHUNK_SAMPLES; i++) {
      if (samples[i] < minSample) minSample = samples[i];
      if (samples[i] > maxSample) maxSample = samples[i];
    }
  }
  audioCaptureStop();

  Serial.printf("[SelfTest] MIC: %u chunks, sample range [%d, %d]\n", chunks,
                minSample, maxSample);
  if (chunks == 0) {
    record(report, TestStatus::FAIL, "no I2S data received");
  } else if (maxSample == minSample) {
    record(report, TestStatus::WARN, "I2S up but signal is flat");
  } else {
    // Data flows and varies — the I2S wiring/driver is alive. Whether the
    // mic actually hears sound can't be auto-verified (and this board's
    // mic is a suspected hardware fault — see pins_config.h).
    report.micDataLive = true;
    record(report, TestStatus::PASS, "I2S data live");
  }
}

void testSpeaker(SelfTestReport &report) {
  displayBootStep("SPKR");
  // The stock board has no speaker interface; PIN_SPK_I2S_* are unverified
  // guesses for a future external amp (see pins_config.h). The TX driver is
  // installed (needed for playback once an amp exists) but there is nothing
  // measurable to test, so this is an intentional SKIP, not a failure.
  Serial.println("[SelfTest] SPKR: skipped (no amp wired, pins unverified)");
  record(report, TestStatus::SKIP, "no amp wired (by design)");
}

void testCamera(SelfTestReport &report) {
  displayBootStep("CAMERA");
  Serial.println("[SelfTest] CAMERA: init + one JPEG capture...");
  if (!cameraCaptureInit()) {
    Serial.println("[SelfTest] CAMERA: init failed");
    record(report, TestStatus::FAIL, "init failed");
    return;
  }

  lastSnapshotBytes = 0;
  bool ok = cameraCaptureSnapshot([](const uint8_t *data, size_t len) {
    (void)data;
    lastSnapshotBytes = len;
  });

  if (ok && lastSnapshotBytes > 0) {
    report.cameraOk = true;
    char detail[28];
    snprintf(detail, sizeof(detail), "JPEG %u bytes",
             (unsigned)lastSnapshotBytes);
    Serial.printf("[SelfTest] CAMERA: OK, %s\n", detail);
    record(report, TestStatus::PASS, detail);
  } else {
    Serial.println("[SelfTest] CAMERA: capture failed");
    record(report, TestStatus::FAIL, "capture failed");
  }
}

}  // namespace

SelfTestReport selfTestRun(Adafruit_NeoPixel &led) {
  SelfTestReport report;

  Serial.println("\n[SelfTest] ===== BOOT SELF TEST =====");
  displayBootBegin("SELF TEST");

  // Driver init lives here so init success/failure IS part of the test.
  audioCaptureInit();
  audioPlaybackInit();

  testPsram(report);
  testLed(report, led);
  testButton(report);
  testConfig(report);
  testWifi(report);
  testMic(report);
  testSpeaker(report);
  testCamera(report);

  Serial.printf("[SelfTest] ===== DONE: %d pass, %d warn, %d fail =====\n",
                report.passed, report.warned, report.failed);

  // Let the finished checklist sit long enough to actually read every row
  // (BOOT press skips ahead) before the summary screen replaces it.
  holdScreen(SELFTEST_CHECKLIST_HOLD_MS);

  char info1[40];
  if (report.wifiConnected) {
    snprintf(info1, sizeof(info1), "IP: %s", WiFi.localIP().toString().c_str());
  } else {
    snprintf(info1, sizeof(info1), "WiFi: offline");
  }
  char info2[48];
  snprintf(info2, sizeof(info2), "AI: %s", aiEngineName());
  displayBootSummary(report.passed, report.warned, report.failed, info1, info2);

  // Hold the summary until the wake button is pressed or the timeout ends,
  // so the results are actually readable before the face screen replaces them.
  holdScreen(SELFTEST_SUMMARY_HOLD_MS);

  return report;
}
