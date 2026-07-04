#pragma once

// WiFi upkeep: reconnect with exponential backoff, first-connect logging,
// ArduinoOTA start/servicing, and the on-screen WiFi dot.
//
// Call every loop() iteration. Returns true while WiFi is connected;
// when it returns false, skip everything that needs the network.
bool wifiLinkLoop();
