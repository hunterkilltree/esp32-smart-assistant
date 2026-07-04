#include "Controls.h"

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

#include "config.h"
#include "AdcButtons.h"
#include "AppState.h"
#include "AudioPlayback.h"
#include "Display.h"
#include "XiaozhiProtocol.h"

namespace {

bool s_infoVisible = false;
unsigned long s_volumeOverlayUntilMs = 0;

void saveVolume(uint8_t volume) {
  Preferences prefs;
  prefs.begin("xiaozhi", false);
  prefs.putUChar("volume", volume);
  prefs.end();
}

uint8_t loadVolume() {
  Preferences prefs;
  prefs.begin("xiaozhi", true);
  uint8_t v = prefs.getUChar("volume", VOLUME_DEFAULT);
  prefs.end();
  return v > 100 ? 100 : v;
}

void changeVolume(int delta) {
  int v = (int)audioPlaybackGetVolume() + delta;
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  audioPlaybackSetVolume((uint8_t)v);
  saveVolume((uint8_t)v);
  Serial.printf("[Controls] Volume -> %d%%\n", v);

  s_infoVisible = false;  // volume bar replaces the info screen if open
  displayShowVolume((uint8_t)v);
  s_volumeOverlayUntilMs = millis() + VOLUME_OVERLAY_MS;
}

void toggleInfoScreen() {
  s_infoVisible = !s_infoVisible;
  if (!s_infoVisible) {
    appRepaintFace();
    return;
  }

  char l1[40], l2[40], l3[40], l4[40], l5[40], l6[40], l7[40];
  const char *stateName;
  switch (appStateGet()) {
    case AssistantState::LISTENING: stateName = "LISTENING"; break;
    case AssistantState::THINKING:  stateName = "THINKING";  break;
    case AssistantState::SPEAKING:  stateName = "SPEAKING";  break;
    default:                        stateName = "IDLE";      break;
  }
  snprintf(l1, sizeof(l1), "State: %s", stateName);
  snprintf(l2, sizeof(l2), "WiFi: %s (%d dBm)",
           WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "offline",
           (int)WiFi.RSSI());
  snprintf(l3, sizeof(l3), "IP: %s", WiFi.localIP().toString().c_str());
  snprintf(l4, sizeof(l4), "WS: %s", xzReady()          ? "ready"
                                     : xzSocketConnected() ? "handshake..."
                                                           : "down");
  snprintf(l5, sizeof(l5), "Dev: %s", xzDeviceId());
  snprintf(l6, sizeof(l6), "Vol: %d%%", (int)audioPlaybackGetVolume());
  snprintf(l7, sizeof(l7), "Heap: %u KB free",
           (unsigned)(ESP.getFreeHeap() / 1024));

  const char *lines[] = {l1, l2, l3, l4, l5, l6, l7};
  displayShowInfo("INFO", lines, 7);
}

}  // namespace

void controlsInit() {
  adcButtonsInit();
  audioPlaybackSetVolume(loadVolume());
  Serial.printf("[Controls] Volume restored: %d%%\n",
                (int)audioPlaybackGetVolume());
}

bool controlsLoop() {
  // Volume overlay expired — put the face back (unless the info screen
  // was opened meanwhile).
  if (s_volumeOverlayUntilMs != 0 && millis() >= s_volumeOverlayUntilMs) {
    s_volumeOverlayUntilMs = 0;
    if (!s_infoVisible) appRepaintFace();
  }

  switch (adcButtonsPoll()) {
    case AdcButton::PLAY:
      s_infoVisible = false;
      return true;  // caller treats it as the wake/talk button
    case AdcButton::UP:
      changeVolume(+VOLUME_STEP);
      break;
    case AdcButton::DOWN:
      changeVolume(-VOLUME_STEP);
      break;
    case AdcButton::MENU:
      toggleInfoScreen();
      break;
    default:
      break;
  }
  return false;
}
