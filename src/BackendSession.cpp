#include "BackendSession.h"

#include <Arduino.h>

#include "config.h"
#include "AppState.h"
#include "AiEngine.h"

namespace {

enum class Phase { CONNECTING, RUNNING };

Phase s_phase = Phase::CONNECTING;

}  // namespace

bool backendSessionLoop(bool buttonPressed) {
  (void)buttonPressed;
  switch (s_phase) {
    case Phase::CONNECTING:
      Serial.printf("[Backend] Starting %s session\n", aiEngineName());
      aiEngineConnect();
      s_phase = Phase::RUNNING;
      appShowFace(Expression::THINKING, "Connecting...");
      return false;
    case Phase::RUNNING:
      return true;
  }
  return false;
}
