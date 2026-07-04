#include "Display.h"

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "pins_config.h"

namespace {

Adafruit_ST7789 tft(PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);

constexpr int16_t W = 240;
constexpr int16_t H = 240;

// RGB565 palette
constexpr uint16_t COL_BG     = 0x0000;  // black
constexpr uint16_t COL_TEXT   = 0xFFFF;  // white
constexpr uint16_t COL_MUTED  = 0x8410;  // gray
constexpr uint16_t COL_BAR    = 0x2124;  // dark gray (status bar)
constexpr uint16_t COL_CYAN   = 0x07FF;
constexpr uint16_t COL_GREEN  = 0x07E0;
constexpr uint16_t COL_RED    = 0xF800;
constexpr uint16_t COL_YELLOW = 0xFFE0;
constexpr uint16_t COL_ORANGE = 0xFD20;
constexpr uint16_t COL_AZURE  = 0x3C7F;
constexpr uint16_t COL_MAGENTA= 0xF81F;

// Boot checklist cursor: rows stack downward from under the header.
int16_t bootRowY = 34;
constexpr int16_t BOOT_ROW_H = 25;

// The backlight (GPIO48) must stay HIGH or the panel goes dark even while
// rendering correctly. Re-asserted on every full-screen draw as insurance:
// a brownout, reset, or stray pin write would otherwise leave the screen
// black until the next reboot.
void ensureBacklight() {
  if (PIN_LCD_BL >= 0) {
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, LOW);
  }
}

// Classic GFX font: 6*size px per char (incl. 1px spacing).
void printCentered(const char *s, int16_t y, uint8_t size, uint16_t color) {
  int16_t x = (W - (int16_t)strlen(s) * 6 * size) / 2;
  if (x < 0) x = 0;
  tft.setTextSize(size);
  tft.setTextColor(color);
  tft.setCursor(x, y);
  tft.print(s);
}

void drawRainbowStrip(int16_t y, int16_t h) {
  static const uint16_t rainbow[6] = {COL_RED, COL_ORANGE, COL_YELLOW,
                                      COL_GREEN, COL_CYAN, COL_MAGENTA};
  for (int i = 0; i < 6; i++) {
    tft.fillRect(i * (W / 6), y, W / 6, h, rainbow[i]);
  }
}

uint16_t expressionColor(Expression e) {
  switch (e) {
    case Expression::HAPPY:     return COL_GREEN;
    case Expression::SAD:       return COL_RED;
    case Expression::THINKING:  return COL_YELLOW;
    case Expression::LISTENING: return COL_AZURE;
    case Expression::SPEAKING:  return COL_ORANGE;
    case Expression::NEUTRAL:
    default:                    return COL_CYAN;
  }
}

// Draws a face of radius r centered at (cx, cy). All feature geometry is
// proportional to r so the same code renders both the big main-flow face
// and the small self-test summary face.
void drawFace(int16_t cx, int16_t cy, int16_t r, Expression e) {
  const uint16_t accent = expressionColor(e);
  tft.fillCircle(cx, cy, r, accent);

  // Mouth first: the "crescent" smiles/frowns are made by drawing a black
  // circle and re-covering most of it with an accent circle — drawing the
  // eyes afterwards keeps the cover circle from erasing them.
  switch (e) {
    case Expression::HAPPY: {  // wide smile (bottom sliver of a circle)
      int16_t mr = r / 2;
      tft.fillCircle(cx, cy + r * 19 / 100, mr, COL_BG);
      tft.fillCircle(cx, cy + r * 19 / 100 - r * 17 / 100, mr, accent);
      break;
    }
    case Expression::NEUTRAL: {  // soft smile
      int16_t mr = r * 42 / 100;
      tft.fillCircle(cx, cy + r * 22 / 100, mr, COL_BG);
      tft.fillCircle(cx, cy + r * 22 / 100 - r * 11 / 100, mr, accent);
      break;
    }
    case Expression::SAD: {  // frown (top sliver of a circle)
      int16_t mr = r * 33 / 100;
      tft.fillCircle(cx, cy + r * 56 / 100, mr, COL_BG);
      tft.fillCircle(cx, cy + r * 56 / 100 + r * 11 / 100, mr, accent);
      break;
    }
    case Expression::THINKING:  // flat mouth
      tft.fillRoundRect(cx - r * 30 / 100, cy + r * 42 / 100,
                        r * 60 / 100, r / 10 + 2, r / 20 + 1, COL_BG);
      break;
    case Expression::SPEAKING:  // open mouth
      tft.fillCircle(cx, cy + r * 44 / 100, r * 22 / 100, COL_BG);
      break;
    case Expression::LISTENING:  // small attentive "o"
      tft.fillCircle(cx, cy + r * 42 / 100, r * 14 / 100, COL_BG);
      break;
  }

  // Eyes.
  int16_t ex = r * 39 / 100, ey = r * 31 / 100;
  switch (e) {
    case Expression::THINKING:  // flat "concentrating" eyes
      tft.fillRoundRect(cx - ex - r * 15 / 100, cy - ey, r * 30 / 100,
                        r / 12 + 2, r / 24 + 1, COL_BG);
      tft.fillRoundRect(cx + ex - r * 15 / 100, cy - ey, r * 30 / 100,
                        r / 12 + 2, r / 24 + 1, COL_BG);
      break;
    case Expression::LISTENING: {  // big round eyes with a glint
      int16_t er = r * 18 / 100;
      tft.fillCircle(cx - ex, cy - ey, er, COL_BG);
      tft.fillCircle(cx + ex, cy - ey, er, COL_BG);
      tft.fillCircle(cx - ex + er / 3, cy - ey - er / 3, er / 4 + 1, COL_TEXT);
      tft.fillCircle(cx + ex + er / 3, cy - ey - er / 3, er / 4 + 1, COL_TEXT);
      break;
    }
    default: {
      int16_t er = r * 14 / 100;
      tft.fillCircle(cx - ex, cy - ey, er, COL_BG);
      tft.fillCircle(cx + ex, cy - ey, er, COL_BG);
      break;
    }
  }

  // Extra character touches.
  if (e == Expression::SAD) {  // a little tear under one eye
    tft.fillCircle(cx - ex, cy - ey + r * 38 / 100, r * 8 / 100 + 1, COL_AZURE);
  }
  if (e == Expression::THINKING) {  // thought-bubble dots, up and to the right
    tft.fillCircle(cx + r * 90 / 100,  cy - r,             r / 20 + 2, accent);
    tft.fillCircle(cx + r * 110 / 100, cy - r * 115 / 100, r / 12 + 2, accent);
    tft.fillCircle(cx + r * 130 / 100, cy - r * 135 / 100, r / 8 + 2, accent);
  }
}

}  // namespace

void displayInit() {
  if (PIN_LCD_BL >= 0) {
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, LOW);
  }

  SPI.begin(PIN_LCD_SCLK, -1, PIN_LCD_MOSI, PIN_LCD_CS);
  tft.init(240, 240);
  tft.setRotation(0);

  displayShowSplash("SMART ASSISTANT", "ESP32-S3-EYE v2.2");
}

void displayShowSplash(const char *title, const char *subtitle) {
  ensureBacklight();
  tft.fillScreen(COL_BG);
  drawRainbowStrip(0, 8);
  drawRainbowStrip(H - 8, 8);

  drawFace(W / 2, 82, 44, Expression::HAPPY);

  printCentered(title, 146, 2, COL_TEXT);
  printCentered(subtitle, 172, 1, COL_MUTED);
  printCentered("booting...", 206, 1, COL_CYAN);
}

void displayBootBegin(const char *title) {
  ensureBacklight();
  tft.fillScreen(COL_BG);
  tft.fillRect(0, 0, W, 26, COL_CYAN);
  printCentered(title, 6, 2, COL_BG);
  bootRowY = 34;
}

void displayBootStep(const char *name) {
  tft.setTextSize(2);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(6, bootRowY);
  tft.print(name);
  tft.setTextColor(COL_MUTED);
  tft.setCursor(186, bootRowY);
  tft.print("....");
}

void displayBootResult(TestStatus status, const char *detail) {
  const char *label;
  uint16_t color;
  switch (status) {
    case TestStatus::PASS: label = "PASS"; color = COL_GREEN;  break;
    case TestStatus::WARN: label = "WARN"; color = COL_YELLOW; break;
    case TestStatus::FAIL: label = "FAIL"; color = COL_RED;    break;
    case TestStatus::SKIP:
    default:               label = "SKIP"; color = COL_MUTED;  break;
  }
  tft.fillRect(184, bootRowY - 1, W - 184, 18, COL_BG);
  tft.setTextSize(2);
  tft.setTextColor(color);
  tft.setCursor(186, bootRowY);
  tft.print(label);

  if (detail && detail[0]) {
    tft.setTextSize(1);
    tft.setTextColor(COL_MUTED);
    tft.setCursor(14, bootRowY + 16);
    // ~37 chars fit at size 1 with the indent; GFX clips overflow anyway.
    tft.print(detail);
  }
  bootRowY += BOOT_ROW_H;
}

void displayBootSummary(int passed, int warned, int failed,
                        const char *info1, const char *info2) {
  uint16_t banner;
  const char *verdict;
  Expression face;
  if (failed > 0) {
    banner = COL_RED;    verdict = "CHECK FAILED";  face = Expression::SAD;
  } else if (warned > 0) {
    banner = COL_YELLOW; verdict = "OK / WARNINGS"; face = Expression::NEUTRAL;
  } else {
    banner = COL_GREEN;  verdict = "ALL SYSTEMS GO"; face = Expression::HAPPY;
  }

  tft.fillScreen(COL_BG);
  tft.fillRect(0, 0, W, 44, banner);
  printCentered("SELF TEST", 6, 2, COL_BG);
  printCentered(verdict, 26, 1, COL_BG);

  drawFace(W / 2, 106, 42, face);

  // Colored counts, printed piecewise so each keeps its own color.
  char buf[8];
  const int16_t countsY = 166;
  int16_t x = (W - 13 * 12) / 2;  // "P:n  W:n  F:n" -> 13 chars at size 2
  tft.setTextSize(2);
  tft.setCursor(x, countsY);
  tft.setTextColor(COL_GREEN);
  snprintf(buf, sizeof(buf), "P:%d", passed);
  tft.print(buf);
  tft.setTextColor(COL_YELLOW);
  snprintf(buf, sizeof(buf), "  W:%d", warned);
  tft.print(buf);
  tft.setTextColor(COL_RED);
  snprintf(buf, sizeof(buf), "  F:%d", failed);
  tft.print(buf);

  if (info1 && info1[0]) printCentered(info1, 192, 1, COL_TEXT);
  if (info2 && info2[0]) printCentered(info2, 204, 1, COL_TEXT);
  printCentered("BOOT btn or wait to continue", 226, 1, COL_MUTED);
}

void displayShowFace(Expression expr, const char *statusText) {
  const uint16_t accent = expressionColor(expr);

  ensureBacklight();
  tft.fillScreen(COL_BG);
  tft.fillRect(0, 0, W, 24, COL_BAR);
  tft.setTextSize(2);
  tft.setTextColor(COL_MUTED);
  tft.setCursor(8, 4);
  tft.print("ASSISTANT");

  drawFace(W / 2, 130, 72, expr);

  printCentered(statusText, 216, 2, accent);
}

void displayShowConnectivity(bool wifiOk, bool wsOk) {
  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(184, 9);
  tft.print("W");
  tft.fillCircle(198, 12, 5, wifiOk ? COL_GREEN : COL_RED);
  tft.setTextSize(1);
  tft.setCursor(210, 9);
  tft.print("S");
  tft.fillCircle(224, 12, 5, wsOk ? COL_GREEN : COL_RED);
}

void displayShowActivation(const char *code) {
  ensureBacklight();
  tft.fillScreen(COL_BG);
  tft.fillRect(0, 0, W, 26, COL_MAGENTA);
  printCentered("ACTIVATE", 6, 2, COL_BG);

  printCentered("Visit xiaozhi.me", 44, 2, COL_TEXT);
  printCentered("Console > Add Device", 70, 1, COL_MUTED);
  printCentered("and enter this code:", 84, 1, COL_MUTED);

  // 6 digits at size 5 are 180px wide — fills the screen nicely.
  printCentered(code, 116, 5, COL_GREEN);

  printCentered("checking again soon...", 186, 1, COL_MUTED);
  printCentered("BOOT btn = check now", 200, 1, COL_MUTED);
  drawRainbowStrip(H - 8, 8);
}

void displayShowVolume(uint8_t percent) {
  if (percent > 100) percent = 100;
  // Overlay strip across the bottom; the next face redraw wipes it.
  const int16_t y = 196, h = 36;
  tft.fillRect(0, y, W, h, COL_BAR);
  char label[12];
  snprintf(label, sizeof(label), "VOL %d%%", percent);
  printCentered(label, y + 4, 1, COL_TEXT);
  const int16_t barX = 20, barY = y + 18, barW = W - 40, barH = 10;
  tft.drawRect(barX, barY, barW, barH, COL_TEXT);
  int16_t fill = (int16_t)((barW - 4) * percent / 100);
  tft.fillRect(barX + 2, barY + 2, barW - 4, barH - 4, COL_BG);
  if (fill > 0) tft.fillRect(barX + 2, barY + 2, fill, barH - 4, COL_GREEN);
}

void displayShowInfo(const char *title, const char *lines[], int count) {
  ensureBacklight();
  tft.fillScreen(COL_BG);
  tft.fillRect(0, 0, W, 26, COL_AZURE);
  printCentered(title, 6, 2, COL_BG);

  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT);
  int16_t y = 40;
  for (int i = 0; i < count && i < 8; i++) {
    tft.setCursor(8, y);
    tft.print(lines[i]);
    y += 22;
  }
  printCentered("MENU = back", 224, 1, COL_MUTED);
}

void displayShowStatus(const char *text) {
  ensureBacklight();
  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_TEXT);
  tft.setTextSize(3);
  tft.setCursor(20, 110);
  tft.print(text);
}
