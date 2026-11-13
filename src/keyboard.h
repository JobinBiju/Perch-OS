#pragma once
// On-screen QWERTY keyboard: a 128 px band from y 112; 28 px keys r6 on a 30 px pitch, rows at 116/146/176/206.
#include "ui.h"

struct Keyboard {
  static constexpr int GAP = 2, ROWS = 4;
  // landscape: 28 px keys on a 30 px pitch, rows from y116; portrait: 21x32 keys on a 23 px pitch, rows at 184 / 218 / 252 / 286
  static int KH() { return portrait() ? 32 : 28; } static int CELL() { return portrait() ? 23 : 30; } static int MOD_W() { return portrait() ? 31 : 42; }
  static int Y0() { return H - ROWS * (KH() + GAP) - (portrait() ? 0 : 4); }
  struct BKey { int x, w; };
  static const BKey *bottom() {   // 123, comma, space, full stop, Done
    static const BKey L[5] = {{11, 42}, {55, 28}, {85, 150}, {237, 28}, {267, 42}}, P[5] = {{6, 38}, {46, 21}, {69, 96}, {167, 21}, {190, 44}};
    return portrait() ? P : L;
  }
  bool visible = false, shift = false, sym = false;
  String *target = nullptr; bool password = false;
  std::function<void()> onDone, onChange;
  int pressedRow = -1, pressedCol = -1; uint32_t pressedAt = 0;

  const char *row0() { return sym ? "1234567890" : "qwertyuiop"; }
  const char *row1() { return sym ? "-/:;()$&@\"" : "asdfghjkl"; }
  const char *row2() { return sym ? "_.,?!'" : "zxcvbnm"; }

  void open(String *t, bool pw, std::function<void()> done, std::function<void()> change = nullptr) {
    target = t; password = pw; onDone = done; onChange = change; visible = true; shift = false; sym = false;
  }
  void close() { visible = false; target = nullptr; }

  // key geometry: returns x,w for key i of n in a row, using a full-width cell of 32 px
  void keyRect(int row, int i, int n, int &x, int &w) {
    int total = n * CELL() - GAP; int x0 = (W - total) / 2;
    x = x0 + i * CELL(); w = CELL() - GAP;
  }
  void drawKey(int x, int y, int w, const char *label, uint16_t bg, uint16_t fg, bool big = false) {
    frame.fillSmoothRoundRect(x, y, w, KH(), 6, bg);
    text(x + w / 2, y + KH() / 2, label, fg, big ? (const lgfx::IFont *)F16 : (const lgfx::IFont *)F12, textdatum_t::middle_center);
  }
  void draw() {
    if (!visible) return;
    frame.fillRect(0, Y0() - 4, W, H - Y0() + 4, C.KEYDARK);
    uint16_t key = C.KEY, act = C.DIM;
    char lbl[2] = {0, 0};
    const char *rows[3] = {row0(), row1(), row2()};
    for (int r = 0; r < 3; r++) {
      int y = Y0() + r * (KH() + GAP); int n = strlen(rows[r]); int x, w;
      for (int i = 0; i < n; i++) {
        keyRect(r, i, n, x, w);
        char ch = rows[r][i]; if (shift && !sym) ch = toupper(ch);
        lbl[0] = ch;
        bool pressed = (pressedRow == r && pressedCol == i);
        drawKey(x, y, w, lbl, pressed ? C.ACCENT : key, C.TEXT, true);
      }
      if (r == 2) { // shift and backspace on the sides
        keyRect(2, 0, n, x, w);
        int sx = x - MOD_W() - 4, bx = x + n * CELL() + 2;
        if (sx < 2) sx = 2; if (bx + MOD_W() > W - 4) bx = W - 4 - MOD_W();
        int cx = sx + MOD_W() / 2, cy = y + KH() / 2;
        if (!sym) { // shift key (letters only)
          frame.fillSmoothRoundRect(sx, y, MOD_W(), KH(), 6, shift ? C.TEXT : act);
          uint16_t fg = shift ? C.KEYDARK : C.TEXT;
          frame.fillTriangle(cx, cy - 7, cx - 7, cy + 1, cx + 7, cy + 1, fg); frame.fillRect(cx - 3, cy + 1, 6, 6, fg);
        }
        frame.fillSmoothRoundRect(bx, y, MOD_W(), KH(), 6, act);
        // backspace glyph
        cx = bx + MOD_W() / 2; cy = y + KH() / 2;
        frame.fillTriangle(cx - 9, cy, cx - 3, cy - 6, cx - 3, cy + 6, C.TEXT); frame.fillRect(cx - 3, cy - 6, 12, 13, C.TEXT);
        frame.drawLine(cx, cy - 3, cx + 6, cy + 3, C.KEY); frame.drawLine(cx, cy + 3, cx + 6, cy - 3, C.KEY);
      }
    }
    // bottom row: 123 | , | space | . | Done
    int y = Y0() + 3 * (KH() + GAP);
    const BKey *b = bottom();
    drawKey(b[0].x, y, b[0].w, sym ? "ABC" : "123", act, C.TEXT);
    drawKey(b[1].x, y, b[1].w, ",", key, C.TEXT, true);
    drawKey(b[2].x, y, b[2].w, "space", key, C.TEXT);
    drawKey(b[3].x, y, b[3].w, ".", key, C.TEXT, true);
    frame.fillSmoothRoundRect(b[4].x, y, b[4].w, KH(), 6, C.ACCENT); text(b[4].x + b[4].w / 2, y + KH() / 2, "Done", C.WHITE, F12M, textdatum_t::middle_center);
  }
  // returns true if the tap was on the keyboard
  bool tap(int x, int y) {
    if (!visible || y < Y0() - 4) return false;
    int r = (y - Y0()) / (KH() + GAP); if (r < 0) r = 0; if (r > 3) r = 3;
    int ry = Y0() + r * (KH() + GAP);
    if (r == 3) {
      const BKey *b = bottom(); int k = -1; for (int i = 0; i < 5; i++) if (hit(x, y, b[i].x - 1, ry, b[i].w + 2, KH())) k = i;
      if (k == 0) { sym = !sym; shift = false; } else if (k == 1) type(','); else if (k == 2) type(' '); else if (k == 3) type('.'); else if (k == 4) { if (onDone) onDone(); }
      return true;
    }
    const char *rows[3] = {row0(), row1(), row2()}; int n = strlen(rows[r]); int kx, kw;
    for (int i = 0; i < n; i++) {
      keyRect(r, i, n, kx, kw);
      if (x >= kx - 1 && x < kx + kw + 1) {
        char ch = rows[r][i]; if (shift && !sym) { ch = toupper(ch); shift = false; }
        type(ch); pressedRow = r; pressedCol = i; pressedAt = millis(); return true;
      }
    }
    if (r == 2) {
      keyRect(2, 0, n, kx, kw);
      if (x < kx) { if (!sym) shift = !shift; }
      else { if (target && target->length()) { target->remove(target->length() - 1); if (onChange) onChange(); } }
    }
    return true;
  }
  void type(char c) { if (target) { *target += c; if (onChange) onChange(); } }
  // clears the pressed highlight after a short time; returns true if a redraw is needed
  bool tick() { if (pressedRow >= 0 && millis() - pressedAt > 120) { pressedRow = pressedCol = -1; return true; } return false; }
};
Keyboard kb;

// A text field: rounded box showing `s` (masked when password) with a caret.
void drawField(int x, int y, int w, int h, const String &s, bool password, const char *placeholder, int pad = 12) {
  frame.fillSmoothRoundRect(x, y, w, h, 10, C.CARD2);
  frame.setFont(F16); frame.setTextDatum(textdatum_t::middle_left);
  String shown = s;
  if (password) { shown = ""; for (size_t i = 0; i < s.length(); i++) shown += (i + 1 == s.length()) ? s[i] : '*'; }
  // keep the tail visible
  while (shown.length() > 1 && frame.textWidth(shown) > w - pad - 16) shown.remove(0, 1);
  if (shown.length()) { frame.setTextColor(C.TEXT); frame.drawString(shown, x + pad, y + h / 2); }
  else { frame.setTextColor(C.DIM); frame.drawString(placeholder, x + pad, y + h / 2); }
  int cx = x + pad + (shown.length() ? frame.textWidth(shown) : 0) + 2;
  if ((millis() / 500) & 1) frame.fillRect(cx, y + 8, 2, h - 16, C.ACCENT);
}
