#pragma once

// AI engine bring-up, in order:
//   CONNECTING — open the provider WebSocket (session setup handled by the
//                engine client; auto-reconnect from then on)
//   RUNNING    — steady state; conversation logic takes over
//
// Call every loop() iteration once WiFi is up. Returns true only in
// RUNNING; while false, skip the conversation logic.
bool backendSessionLoop(bool buttonPressed);
