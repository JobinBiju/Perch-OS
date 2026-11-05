#pragma once
// Home screen: big clock, date, and a row of app icons.
#include "ui.h"
#include "net.h"
#include "app_face.h"
#include "app_weather.h"
#include "app_settings.h"
#include "app_spotify.h"
#include "app_book.h"
#include <math.h>

struct AppIcon { const char *label; Screen *screen; void (*launch)() = nullptr; };
AppIcon apps[] = {{"Robo", &face::faceScreen}, {"Weather", &weatherScreen}, {"Spotify", &spotifyScreen}, {"Book", &libraryScreen, bookLaunch}, {"Settings", &settingsScreen}};
static constexpr int NAPPS = sizeof(apps) / sizeof(apps[0]);
// App drawer. Page 0 keeps the clock and weather, with one row of 5 icons; every further page is two rows of 5.
// Swipe left/right to page; dots under the row show the page when there is more than one.
static constexpr int ICON = 48, ICON_Y = 156, ICON_GAP = 12, ROW_A = CONTENT_Y;   // 48 px tiles on a 60 px pitch, 16 px margins
static constexpr int NPAGES = NAPPS <= 5 ? 1 : 1 + (NAPPS - 5 + 9) / 10;
int iconX(int col) { return 16 + col * (ICON + ICON_GAP); }
int pageStart(int p) { return p == 0 ? 0 : 5 + (p - 1) * 10; }
int pageEnd(int p) { int e = p == 0 ? 5 : pageStart(p) + 10; return e < NAPPS ? e : NAPPS; }
int slotY(int p, int slot) { return (p == 0 || slot >= 5) ? ICON_Y : ROW_A; }

// Spotify mark at the existing 22-pixel radius. Curves are fitted to the reference.
static void drawSpotifyLogo(int cx, int cy) {
  frame.fillSmoothCircle(cx, cy, 22, rgb(30, 215, 96));

  // Cubic Bezier control points relative to the disc centre, followed by
  // stroke radius. Each lower stroke is shorter and thinner.
  static const float waves[3][9] = {
    {-13.1f, -7.0f, -5.0f, -10.0f, 6.0f, -9.0f, 14.1f, -4.3f, 2.10f},
    {-11.9f,  0.1f, -4.6f,  -2.1f, 4.1f, -1.3f, 11.2f,  2.7f, 1.70f},
    {-11.4f,  6.8f, -4.8f,   4.8f, 3.0f,  5.7f,  8.7f,  8.9f, 1.35f}
  };
  static constexpr int STEPS = 20;
  struct Segment { float x, y, dx, dy, invLength2; };

  for (const auto &w : waves) {
    Segment segments[STEPS];
    float prevX = w[0], prevY = w[1];
    float minX = prevX, maxX = prevX, minY = prevY, maxY = prevY;
    const float radius = w[8];

    for (int n = 1; n <= STEPS; ++n) {
      const float t = n / float(STEPS), u = 1.0f - t;
      const float px = u*u*u*w[0] + 3*u*u*t*w[2] + 3*u*t*t*w[4] + t*t*t*w[6];
      const float py = u*u*u*w[1] + 3*u*u*t*w[3] + 3*u*t*t*w[5] + t*t*t*w[7];
      const float dx = px - prevX, dy = py - prevY;
      const float length2 = dx*dx + dy*dy;
      segments[n - 1] = {prevX, prevY, dx, dy, length2 > 0 ? 1.0f / length2 : 0.0f};
      if (px < minX) minX = px;
      if (px > maxX) maxX = px;
      if (py < minY) minY = py;
      if (py > maxY) maxY = py;
      prevX = px; prevY = py;
    }

    const int left = int(floorf(minX - radius - 1));
    const int right = int(ceilf(maxX + radius + 1));
    const int top = int(floorf(minY - radius - 1));
    const int bottom = int(ceilf(maxY + radius + 1));

    // Distance to the whole curve gives round caps and joins. Paint each
    // pixel once so overlapping antialiased segments cannot create seams.
    for (int py = top; py <= bottom; ++py) {
      for (int px = left; px <= right; ++px) {
        float nearest2 = 1.0e10f;
        for (const auto &s : segments) {
          float t = ((px - s.x)*s.dx + (py - s.y)*s.dy) * s.invLength2;
          if (t < 0) t = 0;
          if (t > 1) t = 1;
          const float dx = px - (s.x + t*s.dx);
          const float dy = py - (s.y + t*s.dy);
          const float distance2 = dx*dx + dy*dy;
          if (distance2 < nearest2) nearest2 = distance2;
        }
        const float coverage = radius + 0.5f - sqrtf(nearest2);
        if (coverage <= 0) continue;
        const float green = coverage >= 1 ? 0.0f : 1.0f - coverage;
        frame.drawPixel(cx + px, cy + py,
          rgb(uint8_t(30*green + 0.5f), uint8_t(215*green + 0.5f), uint8_t(96*green + 0.5f)));
      }
    }
  }
}

// Launcher-sized weather mark. Composite the cloud over the sun before
// averaging samples, so cloud joins and the sun overlap have clean edges.
static void drawWeatherAppIcon(int cx, int cy, uint16_t bg) {
  static constexpr int AA = 4, SAMPLES = AA * AA;
  static const float rayDirections[8][2] = {
    {1, 0}, {0.70710678f, 0.70710678f}, {0, 1}, {-0.70710678f, 0.70710678f},
    {-1, 0}, {-0.70710678f, -0.70710678f}, {0, -1}, {0.70710678f, -0.70710678f}
  };
  const int br = ((bg >> 11) & 31) * 255 / 31;
  const int bgc = ((bg >> 5) & 63) * 255 / 63;
  const int bb = (bg & 31) * 255 / 31;
  auto inCircle = [](float x, float y, float ox, float oy, float r) {
    const float dx = x - ox, dy = y - oy;
    return dx*dx + dy*dy <= r*r;
  };

  for (int py = -24; py <= 17; ++py) {
    for (int px = -20; px <= 24; ++px) {
      int cloudSamples = 0, sunSamples = 0;
      for (int sy = 0; sy < AA; ++sy) {
        for (int sx = 0; sx < AA; ++sx) {
          const float x = px + (sx + 0.5f) / AA - 0.5f;
          const float y = py + (sy + 0.5f) / AA - 0.5f;
          const bool cloud = inCircle(x, y, -3, 3, 10)
                          || inCircle(x, y, -12, 9, 6)
                          || inCircle(x, y, 9, 8, 7)
                          || (x >= -12 && x <= 9 && y >= 8 && y <= 15);
          if (cloud) { ++cloudSamples; continue; }

          const float dx = x - 8, dy = y + 8;
          const float distance2 = dx*dx + dy*dy;
          bool sun = distance2 <= 64; // radius 8, behind the cloud
          if (!sun && distance2 >= 100 && distance2 <= 225) {
            for (const auto &d : rayDirections) {
              float along = dx*d[0] + dy*d[1];
              if (along < 11) along = 11;
              if (along > 14) along = 14;
              const float rx = dx - along*d[0], ry = dy - along*d[1];
              if (rx*rx + ry*ry <= 1) { sun = true; break; }
            }
          }
          if (sun) ++sunSamples;
        }
      }
      if (!cloudSamples && !sunSamples) continue;
      const int backgroundSamples = SAMPLES - cloudSamples - sunSamples;
      const uint8_t r = (br*backgroundSamples + 255*sunSamples + 247*cloudSamples + SAMPLES/2) / SAMPLES;
      const uint8_t g = (bgc*backgroundSamples + 208*sunSamples + 250*cloudSamples + SAMPLES/2) / SAMPLES;
      const uint8_t b = (bb*backgroundSamples + 64*sunSamples + 255*cloudSamples + SAMPLES/2) / SAMPLES;
      frame.drawPixel(cx + px, cy + py, rgb(r, g, b));
    }
  }
}

// Launcher-sized book mark: open cover, two curling pages with text strokes, 4x supersampled.
static void drawBookAppIcon(int cx, int cy, uint16_t bg) {
  static constexpr int AA = 4, SAMPLES = AA * AA;
  const int br = ((bg >> 11) & 31) * 255 / 31;
  const int bgc = ((bg >> 5) & 63) * 255 / 63;
  const int bb = (bg & 31) * 255 / 31;

  // Each page rises away from the binding, then gently curls toward its edge.
  auto pageTop = [](float a) {
    float t = (a - 0.65f) / 18.85f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return -8.0f - 16.0f*t + 12.0f*t*t;
  };
  auto inSheet = [&](float a, float y, float edge, float drop) {
    if (a > edge) return false;
    const float top = pageTop(a) + drop, bottom = top + 22.0f;
    if (y < top || y > bottom) return false;
    const float radius = 1.2f;
    if (a > edge - radius) {
      const float dx = a - (edge - radius);
      if (y < top + radius) {
        const float dy = y - (top + radius);
        if (dx*dx + dy*dy > radius*radius) return false;
      }
      if (y > bottom - radius) {
        const float dy = y - (bottom - radius);
        if (dx*dx + dy*dy > radius*radius) return false;
      }
    }
    return true;
  };

  for (int py = -16; py <= 18; ++py) {
    for (int px = -23; px <= 23; ++px) {
      int sumR = 0, sumG = 0, sumB = 0;
      bool painted = false;
      for (int sy = 0; sy < AA; ++sy) {
        for (int sx = 0; sx < AA; ++sx) {
          const float x = px + (sx + 0.5f) / AA - 0.5f;
          const float y = py + (sy + 0.5f) / AA - 0.5f;
          const float a = x < 0 ? -x : x;
          int r = br, g = bgc, b = bb;

          // Cover extends beyond the paper and continues through the binding.
          if (inSheet(a, y, 21.0f, 2.2f)) {
            r = 80; g = 105; b = 132;
            painted = true;
          }
          if (a >= 0.65f && inSheet(a, y, 19.5f, 0.0f)) {
            r = x < 0 ? 237 : 255;
            g = x < 0 ? 239 : 252;
            b = 242;
            // Gentle page shading beside the central fold.
            const float shade = a < 3.0f ? (3.0f - a) / 2.35f : 0.0f;
            const int shadow = int(20.0f*shade + 0.5f);
            r -= shadow; g -= shadow; b -= shadow;
            painted = true;

            // Three curved text strokes; the final line is shorter.
            for (int line = 0; line < 3; ++line) {
              float first = 4.8f, last = 15.4f;
              if (line == 2) {
                if (x < 0) first = 8.0f;
                else last = 12.2f;
              }
              float q = a;
              if (q < first) q = first;
              if (q > last) q = last;
              const float dx = a - q;
              const float dy = y - (pageTop(q) + 6.0f + line*4.3f);
              if (dx*dx + dy*dy <= 0.36f) {
                r = 151; g = 164; b = 179;
                break;
              }
            }
          }
          sumR += r; sumG += g; sumB += b;
        }
      }
      if (!painted) continue;
      frame.drawPixel(cx + px, cy + py,
        rgb(uint8_t((sumR + SAMPLES/2) / SAMPLES),
            uint8_t((sumG + SAMPLES/2) / SAMPLES),
            uint8_t((sumB + SAMPLES/2) / SAMPLES)));
    }
  }
}

void drawAppIcon(int i, int x, int y, bool pressed) {
  uint16_t bg;
  switch (i) {
    case 0: bg = rgb(8, 12, 22); break;
    case 1: bg = rgb(40, 120, 220); break;
    case 2: bg = rgb(0, 0, 0); break;
    case 3: bg = rgb(255, 150, 40); break;
    default: bg = rgb(120, 122, 135); break;
  }
  if (pressed) bg = C.CARD2;
  frame.fillSmoothRoundRect(x, y, ICON, ICON, 12, bg);
  int cx = x + ICON / 2, cy = y + ICON / 2;
  switch (i) {
    case 0: // robo eyes + smile
      frame.fillSmoothRoundRect(cx - 18, cy - 12, 12, 16, 4, face::EYE_BLUE); frame.fillSmoothRoundRect(cx + 6, cy - 12, 12, 16, 4, face::EYE_BLUE);
      frame.fillArc(cx, cy + 6, 9, 7, 30, 150, face::EYE_BLUE); break;
    case 1: // Weather: sun behind one continuous cloud silhouette
      drawWeatherAppIcon(cx, cy, bg);
      break;
    case 2: // Spotify: rounded, asymmetric waves from the reference
      drawSpotifyLogo(cx, cy);
      break;
    case 3: // Book: open cover with curling pages
      drawBookAppIcon(cx, cy, pressed ? C.CARD2 : bg);
      break;
    default: // gear
      for (int k = 0; k < 8; k++) { float a = k * PI / 4; frame.fillSmoothCircle(cx + cosf(a) * 15, cy + sinf(a) * 15, 4, C.WHITE); }
      frame.fillSmoothCircle(cx, cy, 14, C.WHITE); frame.fillSmoothCircle(cx, cy, 6, pressed ? C.CARD2 : bg); break;
  }
}

// The icons are painted per pixel with supersampling (~50 ms each), so each one is rendered once per
// theme and pressed state into a PSRAM sprite and blitted from then on.
static LGFX_Sprite *iconCache[NAPPS][2]; static uint16_t iconCacheBG[NAPPS][2];
void drawAppIconCached(int i, int x, int y, bool pressed) {
  LGFX_Sprite *&sp = iconCache[i][pressed];
  if (sp && iconCacheBG[i][pressed] == C.BG) {
    if (x >= 0 && y >= 0 && x + ICON <= W && y + ICON <= H) {
      uint16_t *dst = (uint16_t *)frame.getBuffer(); const uint16_t *src = (const uint16_t *)sp->getBuffer();
      for (int r = 0; r < ICON; r++) memcpy(dst + (y + r) * W + x, src + r * ICON, ICON * 2);
    } else sp->pushSprite(&frame, x, y);
    return;
  }
  drawAppIcon(i, x, y, pressed);
  if (x < 0 || y < 0 || x + ICON > W || y + ICON > H) return;   // partly off-screen while a page slides: cache later
  if (!sp) {
    sp = new LGFX_Sprite(&lcd); sp->setPsram(true); sp->setColorDepth(16);
    if (!sp->createSprite(ICON, ICON)) { delete sp; sp = nullptr; return; }
  }
  uint16_t *dst = (uint16_t *)sp->getBuffer(); const uint16_t *src = (const uint16_t *)frame.getBuffer();
  for (int r = 0; r < ICON; r++) memcpy(dst + r * ICON, src + (y + r) * W + x, ICON * 2);
  iconCacheBG[i][pressed] = C.BG;
}

struct HomeScreen : Screen {
  int pressed = -1; uint32_t last = 0; String lastClock;
  int page = 0, dragX = 0, downX = 0, snapTo = 0, snapFrom = 0; uint32_t snapT0 = 0; bool dragH = false, snapping = false;
  static constexpr int SNAP_MS = 220;
  const char *name() override { return "Home"; }
  // weather card: (176,40) 128x80 r12, taps open the Weather app
  static constexpr int WX_X = 176, WX_Y = 40, WX_W = 128, WX_H = 80;
  void render() override {
    frame.fillScreen(C.BG); drawStatusBar();
    const int ox = -page * W + dragX;   // page 0 content slides with the drawer
    if (ox > -W && ox < W) {
      const int LX = 16 + ox;
      // clock block: F40B + AM/PM on one baseline (y 72), date 24 px below
      if (clockValid()) {
        time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm); char b[32];
        String ap; String tm12 = clock12(&ap); lastClock = clockHHMM(); last = millis();
        frame.setFont(F40B); int tw = frame.textWidth(tm12);
        text(LX, 72, tm12.c_str(), C.TEXT, F40B, textdatum_t::baseline_left);
        text(LX + tw + 8, 72, ap.c_str(), C.DIM, F16B, textdatum_t::baseline_left);
        strftime(b, sizeof b, "%a, %d %b", &tm);
        text(LX, 96, b, C.DIM, F16, textdatum_t::middle_left);
      } else {
        text(LX, 72, "--:--", C.DIM, F40B, textdatum_t::baseline_left);
        text(LX, 96, netConnected() ? "Syncing time..." : "Connect Wi-Fi to set the clock", C.DIM, F16, textdatum_t::middle_left);
      }
      if (wx.ok) {   // weather card: icon + F24B temperature, condition F12M, city F12 dim; 10 px padding
        int cx = WX_X + ox; card(cx, WX_Y, WX_W, WX_H, C.CARD, 12);
        char tb[12]; snprintf(tb, sizeof tb, "%d", (int)roundf(toUnit(wx.temp)));
        drawWeatherIcon(wx.code, cx + 26, WX_Y + 23, 11);
        frame.setFont(F24B); int tw = frame.textWidth(tb);
        text(cx + 50, WX_Y + 23, tb, C.TEXT, F24B, textdatum_t::middle_left);
        frame.drawCircle(cx + 50 + tw + 5, WX_Y + 15, 3, C.TEXT); frame.drawCircle(cx + 50 + tw + 5, WX_Y + 15, 2, C.TEXT);
        text(cx + 10, WX_Y + 47, wxText(wx.code), C.TEXT, F12M, textdatum_t::middle_left);
        String city = wx.city; frame.setFont(F12); while (city.length() > 1 && frame.textWidth(city) > WX_W - 20) city.remove(city.length() - 1);
        text(cx + 10, WX_Y + 61, city.c_str(), C.DIM, F12, textdatum_t::middle_left);
      }
    }
    // icons: current page plus its neighbours, shifted by the drag/snap offset so pages slide in
    for (int p = page - 1; p <= page + 1; p++) {
      if (p < 0 || p >= NPAGES) continue;
      int off = (p - page) * W + dragX;
      if (off <= -W || off >= W) continue;
      for (int i = pageStart(p); i < pageEnd(p); i++) {
        int slot = i - pageStart(p), x = iconX(slot % 5) + off, y = slotY(p, slot);
        drawAppIconCached(i, x, y, pressed == i);
        text(x + ICON / 2, y + ICON + 13, apps[i].label, C.TEXT, F12, textdatum_t::middle_center);
      }
    }
    if (NPAGES > 1) {   // page dots
      int dy = ICON_Y + ICON + 28, x0 = W / 2 - (NPAGES - 1) * 6;
      for (int p = 0; p < NPAGES; p++) frame.fillSmoothCircle(x0 + p * 12, dy, p == page ? 3 : 2, p == page ? C.TEXT : C.DIM);
    }
  }
  int iconAt(int x, int y) {
    for (int i = pageStart(page); i < pageEnd(page); i++) {
      int slot = i - pageStart(page), ix = iconX(slot % 5), iy = slotY(page, slot);
      if (x >= ix - 6 && x < ix + ICON + 6 && y >= iy - 6 && y < iy + ICON + 20) return i;
    }
    return -1;
  }
  void onDown(int x, int y) override { if (snapping) return; downX = x; dragH = false; dragX = 0; pressed = iconAt(x, y); if (pressed >= 0) dirty = true; }
  void onMove(int x, int y, int dx, int dy) override {
    if (snapping) return;
    if (!dragH) { if (abs(x - downX) < 12) return; dragH = true; pressed = -1; }
    int d = x - downX;
    if ((page == 0 && d > 0) || (page == NPAGES - 1 && d < 0)) d /= 3;   // rubber-band at the ends
    if (d != dragX) { dragX = d; dirty = true; }
  }
  void onUp(int x, int y) override {
    if (pressed >= 0) { pressed = -1; dirty = true; }
    if (dragH) {
      dragH = false;
      int d = x - downX;
      if (d < -40 && page < NPAGES - 1) snapTo = -W; else if (d > 40 && page > 0) snapTo = W; else snapTo = 0;
      snapFrom = dragX; snapT0 = millis(); snapping = true; dirty = true;
    }
  }
  void onTap(int x, int y) override {
    int i = iconAt(x, y); pressed = -1;
    if (i >= 0) { if (apps[i].launch) apps[i].launch(); else pushScreen(apps[i].screen); }
    else if (page == 0 && wx.ok && hit(x, y, WX_X, WX_Y, WX_W, WX_H)) pushScreen(&weatherScreen);
    else dirty = true;
  }
  void tick() override {
    if (snapping) {   // ease the drawer to the target page over SNAP_MS, then commit
      float t = (millis() - snapT0) / (float)SNAP_MS; if (t > 1) t = 1;
      float e = 1 - (1 - t) * (1 - t) * (1 - t);
      dragX = snapFrom + (int)((snapTo - snapFrom) * e);
      if (t >= 1) { dragX = 0; if (snapTo < 0) page++; else if (snapTo > 0) page--; snapping = false; }
      dirty = true; return;
    }
    String c = clockValid() ? clockHHMM() : "";
    if (c != lastClock || millis() - last > 10000) { lastClock = c; last = millis(); dirty = true; }
  }
} homeScreen;
