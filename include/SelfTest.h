#pragma once
#include <Adafruit_NeoPixel.h>

// Boot self-test: runs once in setup(), BEFORE the main flow starts, and
// shows every result on the LCD (colored PASS/WARN/FAIL/SKIP checklist +
// summary screen). Checks, in order:
//
//   PSRAM   — octal PSRAM detected and sized (camera frame buffers need it)
//   LED     — RGB status LED cycles red/green/blue (visual check)
//   BUTTON  — wake button reads released (not stuck low) at boot
//   CONFIG  — secrets.h values are not the placeholder template values
//   WIFI    — connects to the configured AP within a timeout, reports IP
//   MIC     — I2S capture task delivers live (non-flat) PCM data
//   SPKR    — skipped by design: speaker pins are unverified, no amp wired
//   CAMERA  — sensor init + one real JPEG capture, reports its size
//
// It also owns the one-time driver init calls (audio capture/playback,
// camera) so each init doubles as its test — setup() must NOT init them
// again. Blocking (up to ~30s worst case with WiFi timeout); run it before
// the task watchdog is armed.
struct SelfTestReport {
  int passed = 0;
  int warned = 0;
  int failed = 0;
  bool wifiConnected = false;
  bool cameraOk = false;
  bool micDataLive = false;
};

// Runs all checks, paints the checklist + summary on the LCD, then holds
// the summary until the wake button is pressed or a timeout elapses.
// `led` must already be begin()-initialized.
SelfTestReport selfTestRun(Adafruit_NeoPixel &led);
