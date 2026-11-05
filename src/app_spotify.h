#pragma once
// Spotify remote screen: album art, track info, progress, transport buttons, volume.
#include "ui.h"
#include "spotify.h"

struct SpotifyScreen : Screen {
  static constexpr int ART = 80, ART_X = MARGIN, ART_Y = CONTENT_Y, TX = 104;   // art 80x80 r10, text column at x104
  static constexpr int CY = 204, BTN_PREV = 72, BTN_PLAY = 124, BTN_NEXT = 176, VOL_X = 222, VOL_W = 86;
  uint32_t last = 0; int pressed = -1; bool dragVol = false; int localVol = -1; uint32_t volSentAt = 0;
  static const uint16_t GREEN() { return rgb(30, 215, 96); }
  const char *name() override { return "Spotify"; }
  void enter() override { sp.wantPolling = true; spKick(); }
  void leave() override { sp.wantPolling = false; }

  void ellipsize(String &s, const lgfx::IFont *f, int maxw) { frame.setFont(f); if (frame.textWidth(s) <= maxw) return; while (s.length() > 1 && frame.textWidth(s + "...") > maxw) s.remove(s.length() - 1); s += "..."; }
  void render() override {
    frame.fillScreen(C.BG); drawStatusBar(); drawBack(NAV_Y + NAV_H / 2);   // no title: the track info is the heading
    if (!sp.linked) {
      text(W / 2, 100, "Not linked to Spotify", C.TEXT, F16B, textdatum_t::middle_center);
      text(W / 2, 124, "Run spotify_auth.py on your Mac", C.DIM, F12, textdatum_t::middle_center);
      text(W / 2, 140, "with the board plugged in.", C.DIM, F12, textdatum_t::middle_center);
      return;
    }
    if (!netConnected()) { text(W / 2, 110, "No Wi-Fi. Connect in Settings.", C.DIM, F16, textdatum_t::middle_center); return; }
    if (sp.err.length()) text(W / 2, H - 4, sp.err.c_str(), C.RED, F12, textdatum_t::bottom_center);
    if (!sp.have) {
      card(ART_X, ART_Y, ART, ART, C.CARD, 10);
      text(TX, ART_Y + 9, !sp.lastPoll ? (sp.err.length() ? "Can't reach Spotify" : "Connecting...") : "Nothing playing", C.TEXT, F16B, textdatum_t::middle_left);
      text(TX, ART_Y + 31, "Start playback on your phone", C.DIM, F12, textdatum_t::middle_left);
      drawControls(); return;
    }
    // album art
    if (sp.art && sp.artLoaded == sp.artUrl) frame.drawJpg(sp.art, sp.artLen, ART_X, ART_Y, ART, ART, 0, 0, 1.25f, 1.25f);
    else card(ART_X, ART_Y, ART, ART, C.CARD, 10);
    // track info: title F16B, artist F12M, album F12, device with a green dot; rows on a 4 px grid
    int tw = W - TX - MARGIN;
    String t = sp.title, a = sp.artist, al = sp.album;
    ellipsize(t, F16B, tw); text(TX, ART_Y + 9, t.c_str(), C.TEXT, F16B, textdatum_t::middle_left);
    ellipsize(a, F12M, tw); text(TX, ART_Y + 31, a.c_str(), C.DIM, F12M, textdatum_t::middle_left);
    ellipsize(al, F12, tw); text(TX, ART_Y + 51, al.c_str(), C.DIM, F12, textdatum_t::middle_left);
    if (sp.device.length()) { String d = "on " + sp.device; ellipsize(d, F12, tw - 12); frame.fillSmoothCircle(TX + 3, ART_Y + 71, 3, GREEN()); text(TX + 12, ART_Y + 71, d.c_str(), GREEN(), F12, textdatum_t::middle_left); }
    // progress: (12,148) 296x4 r2, times 8 px below
    int py = 148, px = MARGIN, pw = W - 2 * MARGIN; int prog = spProgressMs();
    frame.fillSmoothRoundRect(px, py, pw, 4, 2, C.LINE);
    if (sp.durMs > 0) frame.fillSmoothRoundRect(px, py, pw * prog / sp.durMs, 4, 2, GREEN());
    textf(px, py + 19, C.DIM, F12, textdatum_t::middle_left, "%d:%02d", prog / 60000, (prog / 1000) % 60);
    textf(px + pw, py + 19, C.DIM, F12, textdatum_t::middle_right, "%d:%02d", sp.durMs / 60000, (sp.durMs / 1000) % 60);
    drawControls();
  }
  void drawControls() {
    // prev
    uint16_t c = C.TEXT;
    frame.fillSmoothCircle(BTN_PREV, CY, 18, pressed == 0 ? C.CARD2 : C.CARD);
    frame.fillTriangle(BTN_PREV + 8, CY - 8, BTN_PREV + 8, CY + 8, BTN_PREV - 4, CY, c); frame.fillRect(BTN_PREV - 8, CY - 8, 3, 16, c);
    // play / pause
    frame.fillSmoothCircle(BTN_PLAY, CY, 22, (pressed == 1 || sp.pending) ? C.DIM : C.TEXT); // dim while waiting for Spotify to confirm
    uint16_t ic = C.BG;
    if (sp.have && sp.playing) { frame.fillRect(BTN_PLAY - 8, CY - 9, 5, 18, ic); frame.fillRect(BTN_PLAY + 3, CY - 9, 5, 18, ic); }
    else frame.fillTriangle(BTN_PLAY - 6, CY - 10, BTN_PLAY - 6, CY + 10, BTN_PLAY + 11, CY, ic);
    // next
    frame.fillSmoothCircle(BTN_NEXT, CY, 18, pressed == 2 ? C.CARD2 : C.CARD);
    frame.fillTriangle(BTN_NEXT - 8, CY - 8, BTN_NEXT - 8, CY + 8, BTN_NEXT + 4, CY, c); frame.fillRect(BTN_NEXT + 5, CY - 8, 3, 16, c);
    // volume
    int v = localVol >= 0 ? localVol : sp.volume; if (v < 0) v = 0;
    frame.fillSmoothRoundRect(VOL_X, CY - 2, VOL_W, 4, 2, C.LINE); frame.fillSmoothRoundRect(VOL_X, CY - 2, VOL_W * v / 100, 4, 2, C.TEXT);
    frame.fillSmoothCircle(VOL_X + VOL_W * v / 100, CY, 6, C.TEXT);
    // speaker glyph
    int sx = 204; frame.fillRect(sx, CY - 3, 3, 6, C.DIM); frame.fillTriangle(sx + 3, CY - 3, sx + 9, CY - 7, sx + 9, CY + 7, C.DIM); frame.fillTriangle(sx + 3, CY + 3, sx + 9, CY + 7, sx + 3, CY - 3, C.DIM);
  }
  int buttonAt(int x, int y) {
    if (abs(y - CY) > 26) return -1;
    if (abs(x - BTN_PREV) < 22) return 0; if (abs(x - BTN_PLAY) < 26) return 1; if (abs(x - BTN_NEXT) < 22) return 2;
    if (x > VOL_X - 10 && x < VOL_X + VOL_W + 10) return 3; return -1;
  }
  void setVolFromX(int x) { localVol = constrain((x - VOL_X) * 100 / VOL_W, 0, 100); dirty = true; }
  void onDown(int x, int y) override { int b = buttonAt(x, y); if (b == 3) { dragVol = true; setVolFromX(x); } else if (b >= 0) { pressed = b; dirty = true; } }
  void onMove(int x, int y, int dx, int dy) override { if (dragVol) setVolFromX(x); }
  void onUp(int x, int y) override {
    if (dragVol) { dragVol = false; spCommand("vol", localVol); volSentAt = millis(); }
    if (pressed >= 0) { pressed = -1; dirty = true; }
  }
  void onTap(int x, int y) override {
    int b = buttonAt(x, y);
    if (sp.pending) return; // one command at a time
    if (b == 1) spCommand(sp.have && sp.playing ? "pause" : "play");
    else if (b == 0) spCommand("prev"); else if (b == 2) spCommand("next");
    if (b >= 0 && b < 3) dirty = true;
  }
  void tick() override {
    static bool wasPending = false; if (sp.artNew) { sp.artNew = false; dirty = true; }
    if (sp.pending != wasPending) { wasPending = sp.pending; dirty = true; }
    if (localVol >= 0 && !dragVol && millis() - volSentAt > 3000) localVol = -1;
    if (millis() - last > 1000) { last = millis(); dirty = true; }
  }
} spotifyScreen;
