#pragma once

// Backend bring-up phases, in order:
//   OTA_CHECK       — POST device info to the xiaozhi OTA endpoint
//   ACTIVATION_WAIT — device not bound: activation code on the LCD,
//                     re-poll every 10s (or immediately on button press)
//   CONNECTING      — bound: open the WebSocket (hello handled by protocol)
//   RUNNING         — steady state; conversation logic takes over
//
// Call every loop() iteration once WiFi is up. Returns true only in
// RUNNING; while false, skip the conversation logic.
bool backendSessionLoop(bool buttonPressed);
