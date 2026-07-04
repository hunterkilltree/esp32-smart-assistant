#include "AppState.h"

#include <Arduino.h>
#include <WiFi.h>

#include "AiEngine.h"
#include "AudioCapture.h"
#include "config.h"

namespace {

Adafruit_NeoPixel *s_led = nullptr;
AssistantState s_state = AssistantState::IDLE;
unsigned long s_stateEnteredMs = 0;

// Model-chosen face + caption for the in-flight turn (set_emotion tool).
bool s_turnFaceActive = false;
Expression s_turnExpr = Expression::NEUTRAL;
char s_turnCaption[EMOTION_TEXT_MAX] = "";

}  // namespace

void appStateInit(Adafruit_NeoPixel *led) {
  s_led = led;
}

void appShowFace(Expression expr, const char *statusText,
                 const char *caption) {
  displayShowFace(expr, statusText, caption);
  displayShowConnectivity(WiFi.status() == WL_CONNECTED, aiEngineSocketConnected());
}

void appSetTurnFace(Expression expr, const char *caption) {
  s_turnFaceActive = true;
  s_turnExpr = expr;
  strlcpy(s_turnCaption, caption ? caption : "", sizeof(s_turnCaption));
  appRepaintFace();
}

void appClearTurnFace() {
  s_turnFaceActive = false;
  s_turnCaption[0] = '\0';
}

bool appTurnFaceActive() { return s_turnFaceActive; }

void appRepaintFace() {
  switch (s_state) {
    case AssistantState::IDLE:
      appShowFace(Expression::NEUTRAL, "Ready");
      break;
    case AssistantState::RESULT:
      // Finished reply held on screen until the next talk-button press.
      if (s_turnFaceActive) {
        appShowFace(s_turnExpr, "BOOT/PLAY = talk", s_turnCaption);
      } else {  // model never called set_emotion this turn
        appShowFace(Expression::HAPPY, "BOOT/PLAY = talk");
      }
      break;
    case AssistantState::LISTENING:
      appShowFace(Expression::LISTENING, "Listening...");
      break;
    case AssistantState::THINKING:
      if (s_turnFaceActive) {
        appShowFace(s_turnExpr, "Thinking...", s_turnCaption);
      } else {
        appShowFace(Expression::THINKING, "Thinking...");
      }
      break;
    case AssistantState::SPEAKING:
      if (s_turnFaceActive) {
        appShowFace(s_turnExpr, "Speaking...", s_turnCaption);
      } else {
        appShowFace(Expression::SPEAKING, "Speaking...");
      }
      break;
  }
}

const char *appStateName(AssistantState s) {
  switch (s) {
    case AssistantState::LISTENING: return "LISTENING";
    case AssistantState::THINKING:  return "THINKING";
    case AssistantState::SPEAKING:  return "SPEAKING";
    case AssistantState::RESULT:    return "RESULT";
    case AssistantState::IDLE:
    default:                        return "IDLE";
  }
}

void appStateSet(AssistantState newState) {
  if (newState == s_state) return;
  AssistantState previous = s_state;
  s_state = newState;
  s_stateEnteredMs = millis();
  Serial.printf("[State] %s -> %s\n", appStateName(previous),
                appStateName(newState));

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
    case AssistantState::RESULT:
      s_led->setPixelColor(0, s_led->Color(16, 0, 32));  // dim purple
      break;
  }
  s_led->show();
  appRepaintFace();
}

AssistantState appStateGet() { return s_state; }

unsigned long appStateEnteredMs() { return s_stateEnteredMs; }
