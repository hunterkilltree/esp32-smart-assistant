// ESP32-S3-EYE smart assistant — direct-to-AI realtime voice firmware.
//
// The device streams live mic PCM over one WebSocket straight to the
// configured AI engine (Gemini Live API or OpenAI Realtime API — pick in
// secrets.h), which runs VAD + ASR + LLM + TTS server-side and streams
// spoken PCM audio back. A set_emotion tool call from the model drives the
// face on the LCD (happy / sad / neutral / thinking).
//
// main.cpp is deliberately thin — just the boot sequence and the loop
// pipeline. Each stage lives in its own module so it can be debugged alone:
//   AppState        — state machine + face/LED feedback
//   WifiLink        — WiFi reconnect/backoff + ArduinoOTA
//   BackendSession  — engine WebSocket bring-up
//   AiEngine        — engine client (GeminiLive.cpp / OpenAiRealtime.cpp)
//   Conversation    — talk button, mic→engine PCM pump, state timeouts
//   Controls        — 4-button ADC ladder: PLAY=talk, UP/DN=volume, MENU=info
//
// Buttons: BOOT or PLAY = start / stop / barge-in a conversation,
//          UP / DN = volume, MENU = info screen, RST = reset (hardwired).
//
// Boot flow:  splash → self-test → engine WebSocket setup → IDLE face.
// Talk flow:  talk button → LISTENING (mic streams, server endpointing) →
// THINKING → set_emotion tool call (face) → SPEAKING (TTS audio) → back to
// LISTENING (continuous conversation) until the button ends it.
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Bounce2.h>

#include "config.h"
#include "pins_config.h"
#include "AiEngine.h"
#include "AppState.h"
#include "AudioCapture.h"
#include "BackendSession.h"
#include "Controls.h"
#include "Conversation.h"
#include "Display.h"
#include "Reliability.h"
#include "SelfTest.h"
#include "WifiLink.h"

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
  // API key) on the LCD before the main flow starts. Also performs the
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
  conversationInit();  // registers the AI engine callbacks
  controlsInit();      // ADC buttons + saved volume

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

  if (!backendSessionLoop(talkPressed)) return;  // still connecting

  aiEngineLoop();                // pump the WebSocket
  conversationLoop(talkPressed); // talk button, mic uplink, state timeouts
}
