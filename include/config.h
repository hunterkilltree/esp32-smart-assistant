#pragma once
#include "secrets.h"

// ---- Timing ----
constexpr unsigned long WIFI_RECONNECT_BASE_MS   = 2000;   // doubles per failed attempt
constexpr unsigned long WIFI_RECONNECT_MAX_MS    = 30000;  // backoff cap
constexpr unsigned int  WIFI_RECONNECT_MAX_SHIFT = 5;      // caps 2000 << shift before min()
constexpr unsigned long WS_RECONNECT_INTERVAL_MS = 5000;
constexpr unsigned long BUTTON_DEBOUNCE_MS       = 25;

// ---- Audio (xiaozhi protocol: Opus both directions) ----
// Uplink: mono 16-bit 16 kHz PCM from the mic, Opus-encoded in 60 ms frames.
// Downlink: Opus frames at the sample rate announced in the server hello
// (24 kHz on the official backend), decoded and written to I2S TX.
constexpr uint32_t AUDIO_SAMPLE_RATE       = 16000;
constexpr uint8_t  AUDIO_BITS_PER_SAMPLE   = 16;
constexpr uint8_t  AUDIO_CHANNELS          = 1;
constexpr uint32_t OPUS_FRAME_DURATION_MS  = 60;
constexpr uint32_t PLAYBACK_SAMPLE_RATE_DEFAULT = 24000;
// A 60 ms Opus frame at speech bitrates is well under 1 KB even in bad cases.
constexpr size_t   OPUS_MAX_FRAME_BYTES    = 1500;

// ---- xiaozhi protocol ----
constexpr int           XIAOZHI_PROTOCOL_VERSION   = 1;
constexpr unsigned long HELLO_TIMEOUT_MS           = 10000;  // server hello must arrive within this
constexpr unsigned long OTA_CHECK_RETRY_MS         = 10000;  // re-poll while waiting for activation
constexpr unsigned long OTA_CHECK_FAIL_RETRY_MS    = 30000;  // re-poll after an OTA check error

// ---- Boot self-test (runs before the main flow; see SelfTest.h) ----
// Screen pacing is deliberately slow so a human can actually read each
// result — every hold can be skipped early by pressing the BOOT button.
constexpr unsigned long SPLASH_HOLD_MS             = 3000;   // splash banner hold
constexpr unsigned long SELFTEST_WIFI_TIMEOUT_MS   = 15000;  // max wait for first WiFi connect
constexpr unsigned long SELFTEST_MIC_SAMPLE_MS     = 1500;   // how long to sample mic data
constexpr unsigned long SELFTEST_STEP_PAUSE_MS     = 700;    // pause after each result row lands
constexpr unsigned long SELFTEST_CHECKLIST_HOLD_MS = 10000;  // finished checklist hold (button skips)
constexpr unsigned long SELFTEST_SUMMARY_HOLD_MS   = 15000;  // summary screen hold (button skips)

// ---- ADC button ladder (MENU/PLAY/UP/DN share GPIO1, see pins_config.h) ----
// [UNCONFIRMED — calibrate via pincheck] Non-overlapping mV windows per
// button. Anything below ADC_BTN_PRESS_MAX_MV counts as "some button down";
// a press that matches no window is logged with its measured mV so the
// window can be corrected from real hardware.
constexpr int ADC_BTN_PRESS_MAX_MV = 2900;  // idle (none pressed) reads ~3100
constexpr int ADC_BTN_UP_MIN_MV    = 150,  ADC_BTN_UP_MAX_MV    = 600;
constexpr int ADC_BTN_DOWN_MIN_MV  = 650,  ADC_BTN_DOWN_MAX_MV  = 1150;
constexpr int ADC_BTN_PLAY_MIN_MV  = 1650, ADC_BTN_PLAY_MAX_MV  = 2200;
constexpr int ADC_BTN_MENU_MIN_MV  = 2250, ADC_BTN_MENU_MAX_MV  = 2750;
constexpr unsigned long ADC_BTN_POLL_MS = 15;  // 2 stable polls = a press

// ---- Volume (UP/DN buttons, software gain on decoded TTS) ----
constexpr uint8_t VOLUME_DEFAULT = 70;   // percent, persisted in NVS
constexpr uint8_t VOLUME_STEP    = 10;
constexpr unsigned long VOLUME_OVERLAY_MS = 1500;  // bar hold before face returns

// ---- State machine ----
// Give up on THINKING (waiting for the backend's response) after this long
// and return to IDLE, so a dead/absent backend can't wedge the device.
constexpr unsigned long THINKING_TIMEOUT_MS = 15000;

// ---- VAD (local signal-liveness only — the xiaozhi backend runs the real
// VAD/endpointing server-side in "auto" listen mode) ----
constexpr float         VAD_RMS_THRESHOLD      = 500.0f;
constexpr unsigned long VAD_SILENCE_TIMEOUT_MS = 1200;
