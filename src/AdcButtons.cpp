#include "AdcButtons.h"

#include <Arduino.h>

#include "config.h"
#include "pins_config.h"

namespace {

int s_lastMv = 0;
AdcButton s_stable = AdcButton::NONE;    // debounced current state
AdcButton s_candidate = AdcButton::NONE; // last single-poll classification
bool s_reported = false;                 // press event already emitted?
unsigned long s_lastPollMs = 0;

AdcButton classify(int mv) {
  if (mv >= ADC_BTN_PRESS_MAX_MV) return AdcButton::NONE;
  if (mv >= ADC_BTN_UP_MIN_MV && mv <= ADC_BTN_UP_MAX_MV) return AdcButton::UP;
  if (mv >= ADC_BTN_DOWN_MIN_MV && mv <= ADC_BTN_DOWN_MAX_MV) return AdcButton::DOWN;
  if (mv >= ADC_BTN_PLAY_MIN_MV && mv <= ADC_BTN_PLAY_MAX_MV) return AdcButton::PLAY;
  if (mv >= ADC_BTN_MENU_MIN_MV && mv <= ADC_BTN_MENU_MAX_MV) return AdcButton::MENU;
  // Pressed (below idle) but in no configured window: the [UNCONFIRMED]
  // windows in config.h are off for this board — log the real value so
  // they can be corrected.
  Serial.printf("[AdcBtn] Unclassified press: %d mV (adjust ADC_BTN_* windows)\n", mv);
  return AdcButton::NONE;
}

}  // namespace

void adcButtonsInit() {
  analogSetPinAttenuation(PIN_ADC_BUTTONS, ADC_11db);  // full 0–3.1V range
  s_lastMv = (int)analogReadMilliVolts(PIN_ADC_BUTTONS);
}

AdcButton adcButtonsPoll() {
  unsigned long now = millis();
  if (now - s_lastPollMs < ADC_BTN_POLL_MS) return AdcButton::NONE;
  s_lastPollMs = now;

  s_lastMv = (int)analogReadMilliVolts(PIN_ADC_BUTTONS);
  AdcButton reading = classify(s_lastMv);

  // Debounce: a state must be seen on two consecutive polls to count.
  if (reading != s_candidate) {
    s_candidate = reading;
    return AdcButton::NONE;
  }
  if (reading == s_stable) return AdcButton::NONE;
  s_stable = reading;

  if (s_stable == AdcButton::NONE) {
    s_reported = false;  // released — re-arm
    return AdcButton::NONE;
  }
  if (s_reported) return AdcButton::NONE;
  s_reported = true;
  Serial.printf("[AdcBtn] %s pressed (%d mV)\n", adcButtonName(s_stable), s_lastMv);
  return s_stable;
}

int adcButtonsLastMv() { return s_lastMv; }

const char *adcButtonName(AdcButton b) {
  switch (b) {
    case AdcButton::MENU: return "MENU";
    case AdcButton::PLAY: return "PLAY";
    case AdcButton::UP:   return "UP";
    case AdcButton::DOWN: return "DOWN";
    default:              return "NONE";
  }
}
