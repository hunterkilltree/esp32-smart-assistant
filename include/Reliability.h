#pragma once

// Starts the task watchdog — reboots if the main loop stalls. Call once
// from setup().
void reliabilityInitWatchdog();

// Feeds the watchdog; call once per loop() iteration.
void reliabilityFeedWatchdog();

// Starts OTA updates. Safe to call repeatedly; only takes effect once.
// Call after WiFi first connects.
void reliabilityInitOTA();

// Services pending OTA requests; call once per loop() iteration while
// WiFi is connected.
void reliabilityHandleOTA();
