#include "BackendSession.h"

#include <Arduino.h>

#include "config.h"
#include "AppState.h"
#include "Display.h"
#include "XiaozhiOta.h"
#include "XiaozhiProtocol.h"

namespace {

enum class Phase { OTA_CHECK, ACTIVATION_WAIT, CONNECTING, RUNNING };

Phase s_phase = Phase::OTA_CHECK;
unsigned long s_nextOtaCheckMs = 0;
int s_otaFailures = 0;
char s_wsUrl[160];
char s_wsToken[96];
char s_activationCode[16] = "";

void runOtaCheck() {
  XiaozhiOtaResult result;
  if (!xiaozhiOtaCheck(result)) {
    s_otaFailures++;
    if (s_otaFailures >= 3) {
      // OTA endpoint unreachable (or a self-hosted server without one):
      // connect with the configured defaults instead of never starting.
      Serial.println("[OTA] Giving up on OTA check — using secrets.h defaults");
      strlcpy(s_wsUrl, XIAOZHI_WS_URL, sizeof(s_wsUrl));
      strlcpy(s_wsToken, XIAOZHI_WS_TOKEN, sizeof(s_wsToken));
      s_phase = Phase::CONNECTING;
      return;
    }
    s_nextOtaCheckMs = millis() + OTA_CHECK_FAIL_RETRY_MS;
    return;
  }

  s_otaFailures = 0;
  strlcpy(s_wsUrl, result.wsUrl[0] ? result.wsUrl : XIAOZHI_WS_URL,
          sizeof(s_wsUrl));
  strlcpy(s_wsToken, result.wsToken[0] ? result.wsToken : XIAOZHI_WS_TOKEN,
          sizeof(s_wsToken));

  if (result.hasActivation) {
    if (strcmp(s_activationCode, result.activationCode) != 0) {
      strlcpy(s_activationCode, result.activationCode, sizeof(s_activationCode));
      Serial.printf("[Backend] Enter code %s at xiaozhi.me to bind this device\n",
                    s_activationCode);
      displayShowActivation(s_activationCode);
    }
    s_phase = Phase::ACTIVATION_WAIT;
    s_nextOtaCheckMs = millis() + OTA_CHECK_RETRY_MS;
  } else {
    Serial.println("[Backend] Device is bound — connecting WebSocket");
    s_phase = Phase::CONNECTING;
  }
}

}  // namespace

bool backendSessionLoop(bool buttonPressed) {
  switch (s_phase) {
    case Phase::OTA_CHECK:
      if (millis() >= s_nextOtaCheckMs) runOtaCheck();
      return false;
    case Phase::ACTIVATION_WAIT:
      // Re-poll on a timer, or immediately on a button press.
      if (buttonPressed || millis() >= s_nextOtaCheckMs) {
        s_phase = Phase::OTA_CHECK;
        s_nextOtaCheckMs = 0;
      }
      return false;
    case Phase::CONNECTING:
      xzConnect(s_wsUrl, s_wsToken);
      s_phase = Phase::RUNNING;
      appShowFace(Expression::THINKING, "Connecting...");
      return false;
    case Phase::RUNNING:
      return true;
  }
  return false;
}
