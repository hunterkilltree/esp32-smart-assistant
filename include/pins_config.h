#pragma once

// ---- Pin verification status legend ----
// [UNCONFIRMED]              Sourced from a datasheet/BSP or guessed, but not yet
//                            confirmed against this specific board via the
//                            esp32-s3-eye-pincheck firmware + a human watching it.
// [CONFIRMED — DO NOT CHANGE] Verified on real hardware via a pin-check test with an
//                            explicit "yes" from the user. Do not edit without asking
//                            first — re-verify with pin-check if you do.
//
// Workflow: build/flash the `esp32-s3-eye-pincheck` environment
// (`./upload_project.sh --pincheck`), watch/confirm each test's expected output,
// then flip that pin's tag to [CONFIRMED — DO NOT CHANGE] with the date and what
// was observed.

// ---- Camera pins (CAMERA_MODEL_ESP32S3_EYE) ----
// [CONFIRMED — DO NOT CHANGE] Verified 2026-07-03 via esp32-s3-eye-pincheck:
// camera captured real JPEG frames ("Camera OK -- captured 19663/23642 bytes")
// after fixing PSRAM config in platformio.ini (board_build.arduino.memory_type
// = qio_opi) — the pins themselves were already correct, the earlier failure
// was "frame buffer malloc failed" from PSRAM being misconfigured, not a pin
// issue.
#define CAM_PIN_PWDN     -1
#define CAM_PIN_RESET    -1
#define CAM_PIN_XCLK     15
#define CAM_PIN_SIOD     4
#define CAM_PIN_SIOC     5
#define CAM_PIN_Y9       16
#define CAM_PIN_Y8       17
#define CAM_PIN_Y7       18
#define CAM_PIN_Y6       12
#define CAM_PIN_Y5       10
#define CAM_PIN_Y4       8
#define CAM_PIN_Y3       9
#define CAM_PIN_Y2       11
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK     13

// ---- Mic (onboard digital I2S mic) ----
// [SUSPECTED HARDWARE FAULT — pins are correct, do not change] Pins
// triple-confirmed 2026-07-03 from three independent sources that all
// agree exactly: espressif/esp-bsp, espressif/esp-skainet, AND the actual
// official ESP32-S3-EYE-MB v2.2 schematic (SCH_ESP32-S3-EYE-MB_20211201_
// V2.2.pdf), which names the exact chip: MSM261S4030H0R (WS=pin3->IO42,
// SCK=pin6->IO41, SDOUT=pin7->IO2).
//
// Do NOT reuse GPIO 26/27/28/29/30/31/32/33/34/35/36/37 for this or
// anything else on this board — an earlier attempt at different mic pin
// values (WS=32, SCK=26, SD=33, from an unclear/incorrect source) crashed
// the board into a watchdog reset loop, because those GPIOs carry this
// module's internal SPI flash bus (26-32) and octal PSRAM bus (33-37).
//
// Despite correct pins and confirmed 3.3V power at the mic's VDD pin
// (multimeter-checked by the user), the mic never once responded to
// sound across an extensive set of tests on 2026-07-03:
//   - I2S_NUM_0 and I2S_NUM_1
//   - 16-bit direct read and 32-bit read + >>14 shift (the latter matches
//     espressif/esp-skainet's own proven wake-word driver exactly)
//   - I2S_COMM_FORMAT_STAND_I2S and I2S_COMM_FORMAT_STAND_MSB
//   - ONLY_LEFT and ONLY_RIGHT channel select (RIGHT reads pure zero,
//     confirming slot-select logic is sane; LEFT has a real, structurally
//     changing, non-zero signal under every config combination above)
//   - Fixed and adaptive/relative (ambient-floor-tracking) RMS thresholds
//   - RMS-based and peak-based (more transient-sensitive) detection
//   - Both a loud clap and a close air-puff ("blow test", usually the
//     most reliable way to trigger a MEMS mic)
// Every configuration produces a real, live, non-zero, config-sensitive
// signal on the LEFT slot — proving the digital I2S link itself is
// electrically alive — but with zero correlation to any actual sound.
//
// This points to a hardware-level problem specific to this board's mic
// (bad solder joint on SCK/WS/SD despite VDD/GND being fine, or a dead/
// defective MSM261S4030H0R component), not a firmware/config issue.
// Before spending more firmware effort here: check SCK/WS/SD continuity
// with a multimeter or probe them with an oscilloscope for real clocking
// activity, or consider this mic non-functional and wire an external I2S
// mic module to free GPIOs instead.
#define PIN_MIC_I2S_WS    42  // BSP_I2S_LCLK / GPIO_I2S_LRCK
#define PIN_MIC_I2S_SCK   41  // BSP_I2S_SCLK / GPIO_I2S_SCLK
#define PIN_MIC_I2S_SD    2   // BSP_I2S_DSIN / GPIO_I2S_SDIN

// // ---- Speaker (external I2S amp) — NOT part of the stock board ----
// // [UNCONFIRMED] The ESP32-S3-EYE v2.2 has no built-in speaker or documented
// // speaker header — confirmed by the official BSP (I2S DOUT/MCLK are both NC).
// // These pins are arbitrary picks that avoid every GPIO the BSP lists as already
// // in use (I2C, I2S mic, LCD, camera, SD, LED, button). If you wire an external
// // amp, confirm against your v2.2 board's actual expansion header/free pads via
// // the pin-check speaker test before trusting these — GPIO46 in particular is a
// // strapping pin and risky to drive at boot.
// #warning "pins_config.h: PIN_SPK_I2S_* are UNCONFIRMED guesses — this board has no official speaker interface; confirm via pin-check before wiring an external amp"
// #define PIN_SPK_I2S_BCLK  1
// #define PIN_SPK_I2S_LRC   14
// #define PIN_SPK_I2S_DOUT  46
//
// While these stay undefined (no amp wired), AudioPlayback.cpp skips the
// I2S TX driver entirely and silently discards TTS audio.
//
// RETIRED GUESSES — per the board pin map (pin_io.png), all three of the
// old picks were already taken: GPIO1 = 4-button ADC ladder, GPIO14 =
// battery detect (BAT_DET), GPIO46 = strapping. In fact every GPIO on this
// board is spoken for except the SD-card trio GPIO38/39/40 (unused by this
// project — no SD card). When wiring an external I2S amp, use those:
//   #define PIN_SPK_I2S_BCLK  38
//   #define PIN_SPK_I2S_LRC   39
//   #define PIN_SPK_I2S_DOUT  40
// (then verify with `./upload_project.sh --pincheck` before trusting them,
// and don't use an SD card at the same time).

// ---- ADC button ladder (MENU / PLAY / UP / DN) ----
// [Pin confirmed by pin_io.png ("Buttons: GPIO1, GPIO0, EN"); voltage
// windows still UNCONFIRMED] The four front buttons share ONE input: a
// resistor ladder on GPIO1 (ADC1_CH0). Idle reads near 3.1V; each pressed
// button pulls the node to a distinct lower voltage. The classification
// windows live in config.h (ADC_BTN_*) and are guesses until confirmed:
// run `./upload_project.sh --pincheck`, press each button, and read its
// real mV off the LCD/serial, then tighten the windows.
// Never assign anything else (e.g. speaker pins) to GPIO1.
#define PIN_ADC_BUTTONS   1   // ADC1_CH0 — 4-button resistor ladder

// ---- Battery detect ----
// From pin_io.png: BAT_DET on GPIO14 (analog battery-voltage divider).
// Not used by the firmware yet — documented so nothing repurposes the pin.
#define PIN_BAT_DET       14

// ---- RGB LED ----
// [CONFIRMED — DO NOT CHANGE] Verified 2026-07-03 via esp32-s3-eye-pincheck:
// user confirmed the LED cycled colors as expected.
#define PIN_RGB_LED       3   // BSP_LED_1_IO

// ---- Button ----
// [CONFIRMED — DO NOT CHANGE] Verified 2026-07-03 via esp32-s3-eye-pincheck:
// user pressed the physical BOOT button during the "PRESS BTN" window and
// confirmed the LCD showed "BTN: YES".
#define PIN_BUTTON        0   // BSP_BUTTON_5_IO — also the BOOT/strapping pin. Opening a serial
                               // monitor toggles DTR/RTS and can look like a spurious press during
                               // the reset it triggers; that's expected, not a wiring bug.

// ---- LCD (onboard ST7789) ----
// [CONFIRMED — DO NOT CHANGE] Verified 2026-07-03 via esp32-s3-eye-pincheck:
// user confirmed "LCD OK" text and backlight both appeared as expected.
#define PIN_LCD_SCLK      21  // BSP_LCD_PCLK
#define PIN_LCD_MOSI      47  // BSP_LCD_DATA0
#define PIN_LCD_CS        44  // BSP_LCD_CS
#define PIN_LCD_DC        43  // BSP_LCD_DC
#define PIN_LCD_RST       -1 // No LCD reset wire exists: pin_io.png lists exactly five LCD
                               // signals (48/47/44/21/43) and puts GPIO45 under "Strapping".
                               // GPIO45 selects the flash voltage (VDD_SPI) at reset — driving
                               // it as a fake RST risks a board that won't boot. Keep -1; the
                               // driver does a software reset instead.
#define PIN_LCD_BL        48  // BSP_LCD_BACKLIGHT — must be driven HIGH or the screen stays dark even while rendering correctly
