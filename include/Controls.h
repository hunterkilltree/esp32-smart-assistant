#pragma once

// User controls on the 4-button ADC ladder (see AdcButtons.h):
//   PLAY — second talk button (same behavior as BOOT: start/stop/barge-in)
//   UP   — volume +10% (software gain, saved to NVS, on-screen bar)
//   DN   — volume −10%
//   MENU — toggle the info/debug screen (state, network, IDs, heap)

// Loads the saved volume and initializes the ADC button driver.
// Call once from setup().
void controlsInit();

// Call every loop() iteration. Handles MENU/UP/DN internally and manages
// the overlay timeouts. Returns true when PLAY was pressed (the caller
// merges it with the BOOT button as the wake/talk trigger).
bool controlsLoop();
