#include "WifiLink.h"

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "Display.h"
#include "Reliability.h"
#include "AiEngine.h"

namespace {

unsigned long s_lastAttemptMs = 0;
unsigned int s_retryShift = 0;
bool s_everConnected = false;

void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  // Modem power-save (the ESP32 default) periodically parks the radio,
  // adding latency spikes that stall the TLS WebSocket long enough for its
  // heartbeat to declare the link dead — the main source of mid-call
  // drops. Costs some battery; this device is USB-powered.
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

}  // namespace

bool wifiLinkLoop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (s_everConnected) {
      // Reflect the drop on-screen once, not every loop iteration.
      displayShowConnectivity(false, aiEngineSocketConnected());
    }
    s_everConnected = false;
    unsigned long now = millis();
    unsigned long backoff =
        min(WIFI_RECONNECT_BASE_MS << s_retryShift, WIFI_RECONNECT_MAX_MS);
    if (now - s_lastAttemptMs > backoff) {
      s_lastAttemptMs = now;
      if (s_retryShift < WIFI_RECONNECT_MAX_SHIFT) s_retryShift++;
      connectWiFi();
    }
    return false;
  }

  if (!s_everConnected) {
    s_everConnected = true;
    s_retryShift = 0;
    Serial.printf("[WiFi] Connected, IP: %s\n",
                  WiFi.localIP().toString().c_str());
    displayShowConnectivity(true, aiEngineSocketConnected());
    reliabilityInitOTA();
  }
  reliabilityHandleOTA();
  return true;
}
