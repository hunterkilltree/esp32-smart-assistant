#pragma once
#include <cstdint>

// Onboard 1.3" 240x240 ST7789 LCD UI.
//
// Three screen families:
//   1. Splash        — colorful boot banner, shown first thing in setup().
//   2. Boot checklist — the self-test screen: one row per check, colored
//      PASS/WARN/FAIL/SKIP results, then a summary banner.
//   3. Face          — the main-flow screen: a big expressive face whose
//      color and mouth/eyes track the assistant state or a backend-sent
//      emotion, plus a status bar and WiFi/WS connectivity dots.

// Facial expression shown on the main-flow screen (matches the project
// spec: smile = positive, sad = error/not understood, thinking = processing).
enum class Expression : uint8_t {
  NEUTRAL,    // idle / ready — calm cyan face, soft smile
  HAPPY,      // positive response — green, big smile
  SAD,        // error / not understood — red, frown
  THINKING,   // processing / waiting — yellow, flat mouth + "..."
  LISTENING,  // mic open — green, round attentive eyes
  SPEAKING    // TTS playing — orange, open mouth
};

// Result of a single boot self-test check.
enum class TestStatus : uint8_t { PASS, WARN, FAIL, SKIP };

// Initializes the LCD (SPI, backlight) and shows the splash screen.
// Call as early as possible in setup() so a freshly flashed board gives
// immediate visual proof the new firmware is actually running.
void displayInit();

// Splash banner (title + subtitle over a rainbow strip).
void displayShowSplash(const char *title, const char *subtitle);

// ---- Boot checklist ----
// displayBootBegin() clears the screen and draws the header; then for each
// check call displayBootStep(name) followed by displayBootResult(status,
// detail). Rows stack downward; up to 8 fit.
void displayBootBegin(const char *title);
void displayBootStep(const char *name);
void displayBootResult(TestStatus status, const char *detail = nullptr);

// Summary banner after all checks: colored verdict (green/yellow/red from
// the counts) plus up to two info lines (e.g. IP address, WS endpoint).
void displayBootSummary(int passed, int warned, int failed,
                        const char *info1, const char *info2);

// ---- Main-flow face screen ----
// Full-screen redraw: top status bar (state text), big face, caption.
// Call displayShowConnectivity() afterwards to restore the WiFi/WS dots.
void displayShowFace(Expression expr, const char *statusText);

// Two small dots in the top-right corner: WiFi and WebSocket link state
// (green = up, red = down). Cheap to call on every change.
void displayShowConnectivity(bool wifiOk, bool wsOk);

// First-boot binding screen: the 6-digit xiaozhi.me activation code, big
// and readable, with instructions. Shown until the backend reports the
// device as bound.
void displayShowActivation(const char *code);

// Volume overlay: a horizontal bar + percentage over the bottom of the
// current screen (UP/DN buttons). The face repaints over it afterwards.
void displayShowVolume(uint8_t percent);

// Info/debug screen (MENU button): title bar + up to 8 small text rows.
void displayShowInfo(const char *title, const char *lines[], int count);

// Legacy plain-text status (black screen, white text). Still used by the
// pin-check diagnostic firmware; the main firmware uses the face screens.
void displayShowStatus(const char *text);
