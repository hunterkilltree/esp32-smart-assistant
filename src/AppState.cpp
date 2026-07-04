#include "AppState.h"

#include <Arduino.h>
#include <WiFi.h>

#include "AiEngine.h"
#include "AudioCapture.h"

namespace {

Adafruit_NeoPixel *s_led = nullptr;
AssistantState s_state = AssistantState::IDLE;
unsigned long s_stateEnteredMs = 0;

}  // namespace

void appStateInit(Adafruit_NeoPixel *led) {
  s_led = led;
}

void appShowFace(Expression expr, const char *statusText) {
  displayShowFace(expr, statusText);
  displayShowConnectivity(WiFi.status() == WL_CONNECTED, aiEngineSocketConnected());
}

void appRepaintFace() {
  switch (s_state) {
    case AssistantState::IDLE:
      appShowFace(Expression::NEUTRAL, "Ready");
      break;
    case AssistantState::LISTENING:
      appShowFace(Expression::LISTENING, "Listening...");
      break;
    case AssistantState::THINKING:
      appShowFace(Expression::THINKING, "Thinking...");
      break;
    case AssistantState::SPEAKING:
      appShowFace(Expression::SPEAKING, "Speaking...");
      break;
  }
}

void appStateSet(AssistantState newState) {
  if (newState == s_state) return;
  AssistantState previous = s_state;
  s_state = newState;
  s_stateEnteredMs = millis();

  if (newState == AssistantState::LISTENING) {
    audioCaptureStart();
  } else if (previous == AssistantState::LISTENING) {
    audioCaptureStop();
  }

  switch (s_state) {
    case AssistantState::IDLE:
      s_led->setPixelColor(0, s_led->Color(0, 0, 32));   // dim blue
      break;
    case AssistantState::LISTENING:
      s_led->setPixelColor(0, s_led->Color(0, 64, 0));   // green
      break;
    case AssistantState::THINKING:
      s_led->setPixelColor(0, s_led->Color(48, 48, 0));  // yellow
      break;
    case AssistantState::SPEAKING:
      s_led->setPixelColor(0, s_led->Color(64, 24, 0));  // amber
      break;
  }
  s_led->show();
  appRepaintFace();
}

AssistantState appStateGet() { return s_state; }

unsigned long appStateEnteredMs() { return s_stateEnteredMs; }
