// ESP32-S3-EYE smart assistant — xiaozhi-protocol firmware.
//
// Modeled on https://github.com/78/xiaozhi-esp32: continuous Opus audio
// streaming over WebSocket to a xiaozhi backend (official xiaozhi.me cloud
// or a self-hosted xiaozhi-esp32-server), which runs VAD + ASR + LLM + TTS
// and streams Opus TTS audio back.
//
// main.cpp is deliberately thin — just the boot sequence and the loop
// pipeline. Each stage lives in its own module so it can be debugged alone:
//   AppState        — state machine + face/LED feedback
//   WifiLink        — WiFi reconnect/backoff + ArduinoOTA
//   BackendSession  — OTA check → activation code → WebSocket connect
//   XiaozhiProtocol — wire protocol (hello, listen/abort, tts/stt/llm)
//   Conversation    — talk button, mic→server Opus pump, state timeouts
//   Controls        — 4-button ADC ladder: PLAY=talk, UP/DN=volume, MENU=info
//
// Buttons: BOOT or PLAY = start / stop / barge-in a conversation,
//          UP / DN = volume, MENU = info screen, RST = reset (hardwired).
//
// Boot flow:  splash → self-test → [activation code screen until the device
// is bound at xiaozhi.me] → WebSocket hello → IDLE face.
// Talk flow:  talk button → listen start (auto mode, server endpointing) →
// STT text → THINKING → TTS start → SPEAKING (+ llm emotion faces) → TTS
// stop → back to LISTENING (continuous conversation) until button/goodbye.
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Bounce2.h>

#include "config.h"
#include "pins_config.h"
#include "AppState.h"
#include "AudioCapture.h"
#include "BackendSession.h"
#include "Controls.h"
#include "Conversation.h"
#include "Display.h"
#include "Reliability.h"
#include "SelfTest.h"
#include "WifiLink.h"
#include "XiaozhiProtocol.h"

Adafruit_NeoPixel statusLed(1, PIN_RGB_LED, NEO_GRB + NEO_KHZ800);
Bounce2::Button wakeButton;

void setup() {
  Serial.begin(115200);
  delay(400);  // let the USB serial enumerate before printing anything

  // Splash first so a freshly flashed board proves it's alive right away,
  // even if everything after this fails.
  displayInit();
  delay(SPLASH_HOLD_MS);

  statusLed.begin();
  statusLed.setBrightness(40);
  statusLed.show();

  wakeButton.attach(PIN_BUTTON, INPUT_PULLUP);
  wakeButton.interval(BUTTON_DEBOUNCE_MS);
  wakeButton.setPressedState(LOW);

  // Boot self-test: verifies pins, peripherals, and configuration (WiFi,
  // WS endpoint) on the LCD before the main flow starts. Also performs the
  // one-time audio/camera driver inits and the first WiFi connect.
  SelfTestReport report = selfTestRun(statusLed);
  if (report.failed > 0) {
    Serial.printf("[Boot] Continuing with %d failed check(s) — degraded mode\n",
                  report.failed);
  }

  // If the summary screen was skipped with the BOOT button, wait for the
  // release so the same press doesn't double as a wake trigger below.
  while (digitalRead(PIN_BUTTON) == LOW) delay(10);
  wakeButton.update();

  // Armed only after the self-test: its blocking checks (WiFi timeout, mic
  // sampling) would otherwise trip the 10s watchdog.
  reliabilityInitWatchdog();

  appStateInit(&statusLed);
  conversationInit();  // registers the xiaozhi protocol callbacks
  controlsInit();      // ADC buttons + saved volume

  // From here on the mic PCM stream belongs to the Opus uplink.
  audioCaptureEnableOpus(true);

  appShowFace(Expression::NEUTRAL, "Connecting...");
}

void loop() {

  reliabilityFeedWatchdog();

  if (!wifiLinkLoop()) return;  // WiFi down — nothing else can run

  wakeButton.update();
  // BOOT and PLAY are interchangeable talk buttons; controlsLoop() also
  // services UP/DN (volume) and MENU (info screen) internally. Evaluated
  // separately so neither read is short-circuited away.
  bool playPressed = controlsLoop();
  bool bootPressed = wakeButton.pressed();
  bool talkPressed = playPressed || bootPressed;

  if (!backendSessionLoop(talkPressed)) return;  // still activating/connecting

  xzLoop();                      // pump the WebSocket
  conversationLoop(talkPressed); // talk button, mic uplink, state timeouts
}
