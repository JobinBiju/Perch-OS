#pragma once
// Shared UI core: one full-screen 16-bit frame buffer in internal RAM, a navigation stack,
// a status bar, and small drawing helpers. Screens draw into `frame`, then flush rows to the panel.
#include <Arduino.h>
#include <functional>
#include "lgfx_cfg.h"
#include "fonts.h"

int W = 320, H = 240; static constexpr int SB_H = 20;   // status bar 20, nav header 20..48, content from 52
static constexpr int NAV_Y = 20, NAV_H = 28, CONTENT_Y = 52, MARGIN = 12;
static constexpr int BRIGHT_MIN = 4, BRIGHT_MAX = 255;   // backlight PWM range exposed to the sliders (44 kHz, 8-bit: 4 is a faint glow)
static constexpr int ROW_H = 36, ROW_PITCH = 40;   // list rows: full width less the margins, 36 high on a 40 pitch
#define ROW_W (W - 2 * MARGIN)
#define CHEV_X (W - MARGIN - 16)   /* chevron centre, 16 px in from the row's right edge */
inline bool portrait() { return H > W; }
LGFX lcd;
LGFX_Sprite frame(&lcd);

// ---- palette (switchable light / dark) ----
inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return lcd.color565(r, g, b); }
struct Palette { uint16_t BG, CARD, CARD2, LINE, TEXT, DIM, ACCENT, BLUE, GREEN, ORANGE, RED, YELLOW, SKY, INDIGO, WHITE, KEY, KEYDARK, GLOW, CLOUD, GREENBG; } C;
bool themeDark = true;
// accent colour (Settings > Accent): every selected state, primary button and marker uses C.ACCENT
struct AccentDef { const char *name; uint8_t r, g, b; };
static const AccentDef ACCENTS[] = {{"Blue", 10, 132, 255}, {"Purple", 175, 82, 222}, {"Pink", 255, 45, 85}, {"Orange", 255, 149, 0}, {"Green", 52, 199, 89}, {"Teal", 48, 176, 199}};
static constexpr int NACCENTS = sizeof ACCENTS / sizeof *ACCENTS;
int accentIdx = 0;
uint16_t accentColor(int i) { const AccentDef &a = ACCENTS[constrain(i, 0, NACCENTS - 1)]; return rgb(a.r, a.g, a.b); }
void applyAccent(int i) { accentIdx = constrain(i, 0, NACCENTS - 1); C.ACCENT = accentColor(accentIdx); }
void applyTheme(bool dark) {
  themeDark = dark;
  C.ACCENT = accentColor(accentIdx); C.BLUE = rgb(10, 132, 255); C.GREEN = rgb(52, 199, 89); C.ORANGE = rgb(255, 149, 0); C.RED = rgb(255, 69, 58);
  C.YELLOW = rgb(255, 214, 10); C.SKY = rgb(60, 160, 240); C.INDIGO = rgb(90, 90, 200); C.WHITE = rgb(255, 255, 255);
  if (dark) {
    C.BG = rgb(10, 12, 20); C.CARD = rgb(28, 30, 40); C.CARD2 = rgb(44, 46, 58); C.LINE = rgb(60, 62, 76);
    C.TEXT = rgb(240, 240, 245); C.DIM = rgb(150, 152, 165); C.KEY = rgb(70, 72, 86); C.KEYDARK = rgb(44, 46, 58);
    C.GLOW = rgb(16, 20, 36); C.CLOUD = rgb(255, 255, 255); C.GREENBG = rgb(22, 58, 40);
  } else {
    C.BG = rgb(242, 242, 247); C.CARD = rgb(255, 255, 255); C.CARD2 = rgb(229, 229, 234); C.LINE = rgb(200, 200, 206);
    C.TEXT = rgb(20, 20, 26); C.DIM = rgb(120, 120, 130); C.KEY = rgb(255, 255, 255); C.KEYDARK = rgb(209, 212, 218);
    C.GLOW = rgb(232, 232, 240); C.CLOUD = rgb(200, 206, 216); C.GREENBG = rgb(214, 243, 224);
  }
}

// rot 0..3 = 0, 90, 180, 270 degrees; 0 is landscape with USB on the right. LovyanGFX turns the touch panel with it.
// W and H follow, and the frame buffer is rebuilt in the new shape (same pixel count either way).
inline void applyRotation(int rot) {
  lcd.setRotation((rot + 1) & 3); bool resize = lcd.width() != W;
  W = lcd.width(); H = lcd.height();
  if (resize && frame.getBuffer()) { frame.deleteSprite(); frame.createSprite(W, H); }
}

// ---- frame flushing ----
inline void flushRows(int y, int h) {
  if (y < 0) { h += y; y = 0; }
  if (y + h > H) h = H - y;
  if (h <= 0) return;
  lcd.pushImage(0, y, W, h, (const lgfx::swap565_t *)frame.getBuffer() + y * W);
}
inline void flushAll() { flushRows(0, H); }

// ---- screens & navigation ----
struct Screen {
  virtual ~Screen() {}
  virtual const char *name() { return "?"; }
  virtual void enter() {}                     // about to be shown: runs just before the first render of a push or pop
  virtual void leave() {}
  virtual void render() = 0;                  // draw the whole screen into `frame`
  virtual void tick() {}                      // called every loop; may draw + flush itself
  virtual void onDown(int x, int y) {}
  virtual void onMove(int x, int y, int dx, int dy) {}
  virtual void onTap(int x, int y) {}
  virtual void onUp(int x, int y) {}
  bool dirty = false;                         // set to request render+flushAll
  virtual bool titleBack() { return true; }   // tapping the title-bar chevron pops the screen
  // generous back-chevron zone: the top-left 64x62 px; a short tap here pops even if the finger slid a little
  virtual bool backHit(int x, int y) { return titleBack() && x < 64 && y < SB_H + 40; }
  virtual void onBack();
};

Screen *navStack[6]; int navDepth = 0;  // navStack[0] is home
Screen *current() { return navDepth ? navStack[navDepth - 1] : nullptr; }
void pushScreen(Screen *s);  // defined in main.cpp (does the slide-in animation)
void popScreen();
void popScreens(int n);
void Screen::onBack() { popScreen(); }
void popToHome();
void flushAll();

// ---- helpers ----
void setFont(const lgfx::IFont *f) { frame.setFont(f); }
void text(int x, int y, const char *s, uint16_t col, const lgfx::IFont *f, textdatum_t d = textdatum_t::top_left) {
  frame.setFont(f); frame.setTextDatum(d); frame.setTextColor(col); frame.drawString(s, x, y);
}
void textf(int x, int y, uint16_t col, const lgfx::IFont *f, textdatum_t d, const char *fmt, ...) {
  char buf[96]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  text(x, y, buf, col, f, d);
}
void card(int x, int y, int w, int h, uint16_t col = C.CARD, int r = 10) { frame.fillSmoothRoundRect(x, y, w, h, r, col); }
// 2 px stroke helpers (Lucide-style strokes drawn with two 1 px lines)
void line2(int x0, int y0, int x1, int y1, uint16_t col) { frame.drawLine(x0, y0, x1, y1, col); frame.drawLine(x0 + 1, y0, x1 + 1, y1, col); }
// Lucide icon glyph (see fontgen.py ICONS) centred at (cx,cy)
void icon(int cx, int cy, const char *glyph, const lgfx::IFont *f, uint16_t col) { text(cx, cy, glyph, col, f, textdatum_t::middle_center); }
void chevron(int x, int y, uint16_t col) { icon(x, y, IC_CHEVRON_RIGHT, IC16, col); }      // ">" 16 px centred at (x,y)
void chevronLeft(int x, int y, uint16_t col) { icon(x, y, IC_CHEVRON_LEFT, IC16, col); }   // "<" 16 px centred at (x,y)
bool hit(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

int batPercent(); bool batUsb(); bool batPresent(); // from battery.h
// 18 px Lucide battery glyph centred at (cx,cy): charging / full / medium / low by state
void batteryIcon(int cx, int cy) {
  int pct = batPercent(); bool usb = batUsb();
  icon(cx, cy, usb ? IC_BATTERY_CHARGING : pct > 60 ? IC_BATTERY_FULL : pct > 25 ? IC_BATTERY_MEDIUM : IC_BATTERY_LOW, IC18, usb ? C.GREEN : pct <= 20 ? C.RED : C.TEXT);
}

// ---- status bar: time F12M at x12, battery + wifi on the right with a 12 px margin ----
bool clockValid(); String clockHHMM(); int wifiBars(); // from net.h
void drawStatusBar() {
  frame.fillRect(0, 0, W, SB_H, C.BG);
  text(MARGIN, SB_H / 2, clockValid() ? clockHHMM().c_str() : "--:--", C.TEXT, F12M, textdatum_t::middle_left);
  icon(W - 20, SB_H / 2, IC_WIFI, IC16, wifiBars() > 0 ? C.TEXT : C.LINE);   // 16 px, 12 px margin
  batteryIcon(W - 45, SB_H / 2);                                               // 18 px, 8 px left of the wifi glyph
}

// back chevron "<": Lucide chevron-left 20 px in a 28 px target at x 4..32 (tap zone: Screen::backHit)
void drawBack(int cy) { icon(18, cy, IC_CHEVRON_LEFT, IC20, C.ACCENT); }
// nav header: F16B title centred in the 28 px band under the status bar, back hint when not home
void drawTitle(const char *t) {
  text(W / 2, NAV_Y + NAV_H / 2, t, C.TEXT, F16B, textdatum_t::middle_center);
  if (navDepth > 1) drawBack(NAV_Y + NAV_H / 2);
}

// ---- 14 px Lucide glyphs for the Settings icon tiles (white on a coloured 24 px tile centred at cx,cy) ----
void glyphPin(int cx, int cy, uint16_t fg, uint16_t) { icon(cx, cy, IC_MAP_PIN, IC14, fg); }
void glyphSun(int cx, int cy, uint16_t fg) { icon(cx, cy, IC_SUN, IC14, fg); }
void glyphThermo(int cx, int cy, uint16_t fg, uint16_t) { icon(cx, cy, IC_THERMOMETER, IC14, fg); }
void glyphMoon(int cx, int cy, uint16_t fg, uint16_t) { icon(cx, cy, IC_MOON, IC14, fg); }
void glyphRotate(int cx, int cy, uint16_t fg) { icon(cx, cy, IC_ROTATE_CW, IC14, fg); }
void glyphDrive(int cx, int cy, uint16_t fg, uint16_t) { icon(cx, cy, IC_HARD_DRIVE, IC14, fg); }
void glyphInfo(int cx, int cy, uint16_t fg) { icon(cx, cy, IC_INFO, IC14, fg); }
void glyphLock(int cx, int cy, uint16_t fg) { icon(cx, cy, IC_LOCK, IC12, fg); }
