#pragma once
// Weather app: current conditions + 5-day forecast from Open-Meteo (location from IP).
#include "ui.h"
#include "net.h"

// simple vector weather glyphs, size s (radius-ish), centered at cx,cy
void drawSun(int cx, int cy, int s, uint16_t col = C.YELLOW) {
  frame.fillSmoothCircle(cx, cy, s, col);
  for (int i = 0; i < 8; i++) { float a = i * PI / 4; int x0 = cx + cosf(a) * (s + 3), y0 = cy + sinf(a) * (s + 3), x1 = cx + cosf(a) * (s + 7), y1 = cy + sinf(a) * (s + 7); frame.drawLine(x0, y0, x1, y1, col); frame.drawLine(x0 + 1, y0, x1 + 1, y1, col); }
}
void drawCloud(int cx, int cy, int s, uint16_t col = 0xFFFF) { if (col == 0xFFFF) col = C.CLOUD;
  frame.fillSmoothCircle(cx - s / 2, cy, s * 0.55f, col); frame.fillSmoothCircle(cx + s / 10, cy - s / 4, s * 0.7f, col);
  frame.fillSmoothCircle(cx + s / 2 + s / 6, cy + s / 10, s * 0.5f, col); frame.fillSmoothRoundRect(cx - s / 2, cy, s + s / 6 + s / 2, s * 0.55f, s / 4, col);
}
void drawDrops(int cx, int cy, int s, uint16_t col, int n = 3) { for (int i = 0; i < n; i++) { int x = cx - s / 2 + i * (s / (n - 1 > 0 ? n - 1 : 1)); frame.drawLine(x, cy, x - 2, cy + s / 3, col); frame.drawLine(x + 1, cy, x - 1, cy + s / 3, col); } }
void drawWeatherIcon(int code, int cx, int cy, int s) {
  if (code == 0 || code == 1) { drawSun(cx, cy, s); return; }
  if (code == 2) { drawSun(cx - s / 3, cy - s / 3, s * 0.7f); drawCloud(cx + s / 6, cy + s / 4, s * 0.8f); return; }
  if (code == 3 || code == 45 || code == 48) { drawCloud(cx, cy, s, code == 3 ? C.DIM : C.LINE); return; }
  if (code >= 71 && code <= 77 || code == 85 || code == 86) { drawCloud(cx, cy - s / 3, s * 0.8f); for (int i = 0; i < 3; i++) { int x = cx - s / 2 + i * s / 2; frame.drawCircle(x, cy + s * 0.75f, 2, C.CLOUD); } return; }
  if (code >= 95) { drawCloud(cx, cy - s / 3, s * 0.8f, C.DIM); frame.fillTriangle(cx + 2, cy + s / 4, cx - 5, cy + s * 0.8f, cx + 1, cy + s * 0.75f, C.YELLOW); frame.fillTriangle(cx - 2, cy + s * 0.6f, cx + 5, cy + s * 0.5f, cx - 2, cy + s * 1.1f, C.YELLOW); return; }
  drawCloud(cx, cy - s / 3, s * 0.8f, C.DIM); drawDrops(cx, cy + s * 0.45f, s, C.SKY); // rain / drizzle / showers
}

struct WeatherScreen : Screen {
  uint32_t lastDraw = 0; bool wasBusy = false, wasOk = false;
  const char *name() override { return "Weather"; }
  void enter() override { if (!wx.ok || millis() - wx.fetchedAt > 10 * 60 * 1000UL) wxRequest(); }
  void render() override {
    frame.fillScreen(C.BG); drawStatusBar();
    const char *wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (!wx.ok) {
      drawTitle("Weather");
      const char *msg = !netConnected() ? "No Wi-Fi. Connect in Settings." : wx.busy ? "Loading..." : wx.err.length() ? wx.err.c_str() : "Tap to refresh";
      drawWeatherIcon(2, W / 2, 100, 24);
      text(W / 2, 150, msg, C.DIM, F16, textdatum_t::top_center);
      return;
    }
    drawTitle(wx.city.c_str());
    // now block: F40B temperature on baseline 88, condition F16 dim, stats F12; glyph on the right
    char t[16]; snprintf(t, sizeof t, "%d", (int)roundf(toUnit(wx.temp)));
    frame.setFont(F40B); int tw = frame.textWidth(t);
    text(MARGIN, 88, t, C.TEXT, F40B, textdatum_t::baseline_left);
    frame.drawCircle(MARGIN + tw + 6, 62, 4, C.TEXT); frame.drawCircle(MARGIN + tw + 6, 62, 3, C.TEXT);
    text(MARGIN, 107, wxText(wx.code), C.DIM, F16, textdatum_t::middle_left);
    {   // "H 32 · L 24   79%   4 km/h": values F12M, labels dim, 12 px between groups
      int x = MARGIN, y = 127; char b[16];
      auto piece = [&](const char *str, uint16_t col, const lgfx::IFont *f, int gap) { text(x, y, str, col, f, textdatum_t::middle_left); frame.setFont(f); x += frame.textWidth(str) + gap; };
      snprintf(b, sizeof b, "H %d", wx.nDays ? (int)roundf(toUnit(wx.days[0].hi)) : 0); piece(b, C.TEXT, F12M, 4);
      piece("\xC2\xB7", C.DIM, F12, 4);
      snprintf(b, sizeof b, "L %d", wx.nDays ? (int)roundf(toUnit(wx.days[0].lo)) : 0); piece(b, C.TEXT, F12M, 12);
      snprintf(b, sizeof b, "%d%%", wx.hum); piece(b, C.DIM, F12, 12);
      snprintf(b, sizeof b, "%d km/h", (int)wx.wind); piece(b, C.DIM, F12, 0);
    }
    drawWeatherIcon(wx.code, W - 42, portrait() ? 79 : 81, 22);
    // 5-day card: (12,140) 296x88 r12; equal columns, day on top, glyph in the middle, hi lo on the bottom
    int n = wx.nDays;
    if (portrait()) {   // portrait: 5-day list card (12,144) 216x164, 32 px rows with hairlines: day, glyph, hi lo
      card(MARGIN, 144, W - 2 * MARGIN, n * 32 + 4, C.CARD, 12);
      for (int i = 0; i < n; i++) {
        int cy = 162 + i * 32;
        text(MARGIN + 12, cy, i == 0 ? "Today" : wd[wx.days[i].wday], i == 0 ? C.TEXT : C.DIM, i == 0 ? F12M : F12, textdatum_t::middle_left);
        drawWeatherIcon(wx.days[i].code, W / 2, cy - 3, 8);
        char hi[8], lo[8]; snprintf(hi, sizeof hi, "%d", (int)roundf(toUnit(wx.days[i].hi))); snprintf(lo, sizeof lo, "%d", (int)roundf(toUnit(wx.days[i].lo)));
        frame.setFont(F12); int lw = frame.textWidth(lo);
        text(W - MARGIN - 12, cy, lo, C.DIM, F12, textdatum_t::middle_right); text(W - MARGIN - 12 - lw - 4, cy, hi, C.TEXT, F12M, textdatum_t::middle_right);
        if (i + 1 < n) frame.drawFastHLine(MARGIN + 12, cy + 16, W - 2 * MARGIN - 24, C.CARD2);
      }
      n = 0;
    } else card(MARGIN, 140, W - 2 * MARGIN, 88, C.CARD, 12);
    for (int i = 0; i < n; i++) {
      int cx = 16 + (288 * i + 144) / n;
      text(cx, 155, i == 0 ? "Today" : wd[wx.days[i].wday], i == 0 ? C.TEXT : C.DIM, i == 0 ? F12M : F12, textdatum_t::middle_center);
      drawWeatherIcon(wx.days[i].code, cx, 184, 9);
      char hi[8], lo[8]; snprintf(hi, sizeof hi, "%d", (int)roundf(toUnit(wx.days[i].hi))); snprintf(lo, sizeof lo, "%d", (int)roundf(toUnit(wx.days[i].lo)));
      text(cx - 2, 220, hi, C.TEXT, F12, textdatum_t::bottom_right); text(cx + 2, 220, lo, C.DIM, F12, textdatum_t::bottom_left);
    }
    if (wx.busy) text(W - MARGIN, H - 4, "updating", C.DIM, F12, textdatum_t::bottom_right);
  }
  void tick() override {
    if (wx.busy != wasBusy || wx.ok != wasOk || millis() - lastDraw > 30000) { wasBusy = wx.busy; wasOk = wx.ok; lastDraw = millis(); dirty = true; }
  }
  void onTap(int x, int y) override { if (y > SB_H) { wxRequest(); dirty = true; } }
} weatherScreen;
