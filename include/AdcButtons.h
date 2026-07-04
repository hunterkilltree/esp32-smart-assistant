#pragma once
#include <cstdint>

// Driver for the 4 front buttons (MENU / PLAY / UP / DN) that share one
// ADC resistor ladder on GPIO1 — each pressed button pulls the pin to a
// distinct voltage (windows in config.h, [UNCONFIRMED] until pincheck'd).

enum class AdcButton : uint8_t { NONE, MENU, PLAY, UP, DOWN };

void adcButtonsInit();

// Call every loop() iteration (self-throttled to ADC_BTN_POLL_MS).
// Returns a button exactly once per physical press (edge-triggered,
// debounced over two consecutive polls), NONE otherwise.
AdcButton adcButtonsPoll();

// Last raw reading in mV — for the pincheck calibration test.
int adcButtonsLastMv();

const char *adcButtonName(AdcButton b);
