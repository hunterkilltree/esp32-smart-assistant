#include "Reliability.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>

namespace {
constexpr uint32_t WATCHDOG_TIMEOUT_S = 10;
bool s_otaStarted = false;
}  // namespace

void reliabilityInitWatchdog() {
  esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
  esp_task_wdt_add(nullptr);
}

void reliabilityFeedWatchdog() {
  esp_task_wdt_reset();
}

void reliabilityInitOTA() {
  if (s_otaStarted) return;

  ArduinoOTA.setHostname("esp32-smart-assistant");
  ArduinoOTA.onStart([]() { Serial.println("[OTA] Update starting"); });
  ArduinoOTA.onEnd([]() { Serial.println("[OTA] Update complete"); });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error [%u]\n", (unsigned)error);
  });
  ArduinoOTA.begin();

  s_otaStarted = true;
}

void reliabilityHandleOTA() {
  ArduinoOTA.handle();
}
