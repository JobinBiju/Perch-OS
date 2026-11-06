#pragma once
// Settings: Wi-Fi (scan, pick, type password), location, brightness, units, theme, rotation, storage, about.
// Layout: 296 px cards inside 12 px margins; list rows 36 tall on a 40 px pitch from y 52.
#include "ui.h"
#include "net.h"
#include "keyboard.h"
#include "storage.h"

// ---------- Wi-Fi password entry ----------
struct WifiPasswordScreen : Screen {
  String ssid, pass;
  const char *name() override { return "Password"; }
  void begin(const String &s) { ssid = s; pass = ""; }
  void enter() override {
    kb.open(&pass, true, [this]() {
      netRemember(ssid, pass); kb.close(); netConnectTo(ssid, pass); popScreen();
    }, [this]() { dirty = true; });
  }
  void leave() override { kb.close(); }
  void render() override {
    frame.fillScreen(C.BG); drawStatusBar(); drawTitle("Join Network");
    textf(MARGIN, 59, C.DIM, F12, textdatum_t::middle_left, "Password for \"%s\"", ssid.c_str());
    drawField(MARGIN, 70, ROW_W, 36, pass, true, "Password", 36); icon(MARGIN + 20, 88, IC_LOCK, IC16, C.DIM);
    kb.draw();
  }
  void tick() override { static uint32_t last = 0; if (kb.tick()) dirty = true; if (millis() - last > 500) { last = millis(); dirty = true; } }
  void onTap(int x, int y) override { if (kb.tap(x, y)) dirty = true; }
} wifiPasswordScreen;

// ---------- Wi-Fi list ----------
struct WifiScreen : Screen {
  int scroll = 0, n = 0, pull = 0; uint32_t lastStatus = 0, seenSeq = 0; int pressed = -1; bool failed = false;
  static constexpr int LIST_Y = 122;   // status card 52..96, "Networks" label, rows from 122
  const char *name() override { return "Wi-Fi"; }
  bool scanning() { return netState == NS_SCANNING; }
  void startScan() { failed = !netStartScan(); dirty = true; }
  void enter() override { n = 0; startScan(); }
  void leave() override { if (!netConnected()) netNextTry = 0; }
  String current() { return netConnected() ? WiFi.SSID() : cfg.ssid; }
  bool canForget() { return current().length() && netFind(current()) >= 0; }
  void render() override {
    frame.fillScreen(C.BG); drawStatusBar(); drawTitle("Wi-Fi");
    // status card: wifi glyph, state + ssid · ip, Forget chip
    card(MARGIN, CONTENT_Y, ROW_W, 44, C.CARD, 10);
    bool on = netConnected(); uint16_t col = on ? C.GREEN : nnets ? C.ORANGE : C.DIM;
    icon(33, 74, IC_WIFI, IC18, col);
    int right = W - MARGIN - 12;
    if (canForget()) { frame.setFont(F12M); int fw = frame.textWidth("Forget") + 20; frame.fillSmoothRoundRect(right - fw, CONTENT_Y + 10, fw, 24, 12, C.CARD2); text(right - fw / 2, CONTENT_Y + 22, "Forget", C.RED, F12M, textdatum_t::middle_center); right -= fw + 8; }
    text(52, CONTENT_Y + 14, on ? "Connected" : netStatusText(), col, F12M, textdatum_t::middle_left);
    String sub = on ? WiFi.SSID() + " \xC2\xB7 " + WiFi.localIP().toString() : nnets ? String(nnets) + " saved" : String("Pick a network to join");
    frame.setFont(F12); while (sub.length() > 1 && frame.textWidth(sub) > right - 52) sub.remove(sub.length() - 1);
    text(52, CONTENT_Y + 30, sub.c_str(), C.DIM, F12, textdatum_t::middle_left);
    text(MARGIN, 111, "Networks", C.DIM, F12M, textdatum_t::middle_left);
    // list
    frame.setClipRect(0, LIST_Y, W, H - LIST_Y);
    if (n == 0) text(W / 2, LIST_Y + 36, scanning() ? "Scanning..." : failed ? "Scan failed. Tap to retry" : "No networks found. Tap to retry", C.DIM, F16, textdatum_t::middle_center);
    for (int i = 0; i < n; i++) {
      int y = LIST_Y + i * ROW_PITCH - scroll + pull; if (y > H || y + ROW_H < LIST_Y) continue;
      card(MARGIN, y, ROW_W, ROW_H, pressed == i ? C.CARD2 : C.CARD, 10); int cy = y + ROW_H / 2;
      String ss = WiFi.SSID(i); if (!ss.length()) ss = "(hidden)";
      bool cur = netConnected() && ss == WiFi.SSID(); bool known = netFind(ss) >= 0;
      int r = WiFi.RSSI(i); bool locked = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      // right to left: chevron, lock, signal, "saved"
      chevron(CHEV_X, cy, C.DIM); int rx = CHEV_X - 8 - 10;
      if (locked) { glyphLock(rx - 6, cy, C.DIM); rx -= 12 + 10; }
      icon(rx - 8, cy, IC_WIFI, IC16, r > -75 ? C.TEXT : C.DIM); rx -= 16 + 10;
      if (known && !cur) { text(rx, cy, "saved", C.DIM, F12, textdatum_t::middle_right); frame.setFont(F12); rx -= frame.textWidth("saved") + 10; }
      frame.setFont(F16); while (ss.length() > 1 && frame.textWidth(ss) > rx - 24) ss.remove(ss.length() - 1);
      text(24, cy, ss.c_str(), cur ? C.BLUE : C.TEXT, F16, textdatum_t::middle_left);
    }
    frame.clearClipRect();
    if (n) { frame.fillRect(0, H - 24, W, 24, C.BG); text(W / 2, 223, "Tap a network to join \xC2\xB7 pull to rescan", C.DIM, F12, textdatum_t::middle_center); }
  }
  void tick() override {
    if (seenSeq != scanSeq) { seenSeq = scanSeq; n = scanCount; failed = (n == 0 && !scanning()); scroll = 0; dirty = true; }
    if (millis() - lastStatus > 1000) { lastStatus = millis(); dirty = true; }
  }
  void onMove(int x, int y, int dx, int dy) override {
    int maxScroll = max(0, n * ROW_PITCH - (H - 24 - LIST_Y)); pressed = -1;
    if (scroll == 0 && dy > 0 && y >= LIST_Y) pull = min(pull + dy / 2, 40); else scroll = constrain(scroll - dy, 0, maxScroll);
    dirty = true;
  }
  void onUp(int x, int y) override { if (pull >= 24 && !scanning()) startScan(); if (pull) { pull = 0; dirty = true; } }
  void onTap(int x, int y) override {
    if (y < LIST_Y) {
      if (x > W - 100 && y >= CONTENT_Y && y < CONTENT_Y + 44 && canForget()) {
        netForget(current()); cfg.ssid = ""; cfg.pass = ""; cfgSave(); WiFi.disconnect(); netState = NS_IDLE; netNextTry = millis() + 2000; dirty = true;
      }
      return;
    }
    if (n == 0) { if (!scanning()) startScan(); return; }   // tap the empty list to rescan
    int i = (y - LIST_Y + scroll) / ROW_PITCH; if (i < 0 || i >= n) return;
    String ss = WiFi.SSID(i); if (!ss.length()) return;
    int k = netFind(ss);
    if (k >= 0) { netConnectTo(nets[k].ssid, nets[k].pass); dirty = true; return; }                 // saved: join directly
    if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) { netRemember(ss, ""); netConnectTo(ss, ""); dirty = true; return; }
    wifiPasswordScreen.begin(ss); pushScreen(&wifiPasswordScreen);
  }
} wifiScreen;

// ---------- About: two grouped cards of key / value rows ----------
struct AboutScreen : Screen {
  uint32_t last = 0;
  const char *name() override { return "About"; }
  // card of `n` rows, 20 px each with hairlines, 6 px top/bottom padding; returns the y below it
  int group(int y, int n, std::function<void(int, int)> row) {
    card(MARGIN, y, ROW_W, 12 + n * 20, C.CARD, 12);
    for (int i = 0; i < n; i++) { int ry = y + 6 + i * 20; row(i, ry + 10); if (i + 1 < n) frame.drawFastHLine(MARGIN + 12, ry + 19, ROW_W - 24, C.CARD2); }
    return y + 12 + n * 20;
  }
  void kv(int cy, const char *k, String v) {
    text(MARGIN + 12, cy, k, C.DIM, F12, textdatum_t::middle_left);
    frame.setFont(F12M); while (v.length() > 1 && frame.textWidth(v) > 200) v.remove(v.length() - 1);
    text(W - MARGIN - 12, cy, v.c_str(), C.TEXT, F12M, textdatum_t::middle_right);
  }
  void render() override {
    frame.fillScreen(C.BG); drawStatusBar(); drawTitle("About");
    int y = group(CONTENT_Y, 5, [&](int i, int cy) {
      switch (i) {
        case 0: kv(cy, "Board", "LCDWiki ES3C28P \xC2\xB7 ESP32-S3"); break;
        case 1: kv(cy, "Wi-Fi", netConnected() ? WiFi.SSID() + " \xC2\xB7 " + WiFi.localIP().toString() : String(netStatusText())); break;
        case 2: kv(cy, "Signal", netConnected() ? String(WiFi.RSSI()) + " dBm" : "-"); break;
        case 3: kv(cy, "Portal", netConnected() ? "http://perch.local" : "-"); break;
        case 4: kv(cy, "Location", wx.ok ? wx.city : "-"); break;
      }
    });
    group(y + 4, 3, [&](int i, int cy) {
      switch (i) {
        case 0: kv(cy, "Battery", String(bat.mv / 1000.0f, 2) + " V \xC2\xB7 " + String(bat.pct) + "%" + (bat.usb ? " \xC2\xB7 on USB" : "")); break;
        case 1: kv(cy, "Memory", "heap " + String(ESP.getFreeHeap() / 1024) + " KB \xC2\xB7 PSRAM " + String(ESP.getFreePsram() / 1024) + " KB"); break;
        case 2: kv(cy, "Books", storageText()); break;
      }
    });
  }
  void tick() override { if (millis() - last > 2000) { last = millis(); dirty = true; } }
} aboutScreen;

// ---------- Location: typed entry (reached from the saved-places list) ----------
struct LocationEditScreen : Screen {
  String val;
  const char *name() override { return "Location"; }
  void enter() override {
    val = placeIndex(cfg.place) >= 0 ? String("") : cfg.place;
    kb.open(&val, false, [this]() { val.trim(); cfg.place = val; cfgSave(); wxInvalidate(); wxRequest(); kb.close(); popScreens(2); }, [this]() { dirty = true; });
  }
  void leave() override { kb.close(); }
  void render() override {
    frame.fillScreen(C.BG); drawStatusBar(); drawTitle("Other city");
    text(MARGIN, 59, "City, Region \xC2\xB7 blank = use IP", C.DIM, F12, textdatum_t::middle_left);
    drawField(MARGIN, 70, ROW_W, 36, val, false, "City, Region", 36); icon(MARGIN + 20, 88, IC_MAP_PIN, IC16, C.DIM);
    kb.draw();
  }
  void tick() override { static uint32_t last = 0; if (kb.tick()) dirty = true; if (millis() - last > 500) { last = millis(); dirty = true; } }
  void onTap(int x, int y) override { if (kb.tap(x, y)) dirty = true; }
} locationEditScreen;

// ---------- Location: saved places, automatic, or another city ----------
struct LocationScreen : Screen {
  static constexpr int NROWS = NPLACES + 2;   // Automatic, saved places, Other
  int scroll = 0, pressed = -1;
  const char *name() override { return "Location"; }
  void enter() override { pressed = -1; }
  int rowY(int i) { return CONTENT_Y + i * ROW_PITCH - scroll; }
  int selected() { if (!cfg.place.length()) return 0; int p = placeIndex(cfg.place); return p >= 0 ? p + 1 : NROWS - 1; }
  void render() override {
    frame.fillScreen(C.BG); drawStatusBar(); drawTitle("Location");
    frame.setClipRect(0, CONTENT_Y, W, H - CONTENT_Y);
    int sel = selected();
    for (int i = 0; i < NROWS; i++) {
      int y = rowY(i); if (y > H || y + ROW_H < CONTENT_Y) continue;
      card(MARGIN, y, ROW_W, ROW_H, pressed == i ? C.CARD2 : C.CARD, 10); int cy = y + ROW_H / 2;
      bool on = i == sel; uint16_t col = on ? C.BLUE : C.TEXT;
      if (i == 0) { text(MARGIN + 12, cy, "Automatic", col, F16, textdatum_t::middle_left); text(CHEV_X - (on ? 18 : 8), cy, "from IP address", C.DIM, F12, textdatum_t::middle_right); }
      else if (i <= NPLACES) text(MARGIN + 12, cy, PLACES[i - 1].name, col, F16, textdatum_t::middle_left);
      else {
        text(MARGIN + 12, cy, "Other city", col, F16, textdatum_t::middle_left);
        String v = on ? cfg.place : "type a name"; frame.setFont(F12); while (v.length() > 1 && frame.textWidth(v) > 150) v.remove(v.length() - 1);
        text(CHEV_X - 18, cy, v.c_str(), C.DIM, F12, textdatum_t::middle_right); chevron(CHEV_X, cy, C.DIM);
      }
      if (on && i < NROWS - 1) icon(CHEV_X, cy, IC_CHECK, IC16, C.BLUE);
    }
    frame.clearClipRect();
  }
  int rowAt(int x, int y) { if (y < CONTENT_Y) return -1; for (int i = 0; i < NROWS; i++) if (hit(x, y, MARGIN, rowY(i), ROW_W, ROW_H)) return i; return -1; }
  void onDown(int x, int y) override { pressed = rowAt(x, y); if (pressed >= 0) dirty = true; }
  void onMove(int x, int y, int dx, int dy) override { pressed = -1; int maxScroll = max(0, CONTENT_Y + NROWS * ROW_PITCH - 4 + MARGIN - H); scroll = constrain(scroll - dy, 0, maxScroll); dirty = true; }
  void onUp(int x, int y) override { if (pressed >= 0) { pressed = -1; dirty = true; } }
  void onTap(int x, int y) override {
    int i = rowAt(x, y); pressed = -1; dirty = true;
    if (i < 0) return;
    if (i == NROWS - 1) { pushScreen(&locationEditScreen); return; }
    String p = i == 0 ? String("") : String(PLACES[i - 1].name);
    if (p == cfg.place) return;
    cfg.place = p; cfgSave(); wxInvalidate(); wxRequest();
  }
} locationScreen;

// ---------- Storage ----------
struct StorageScreen : Screen {
  uint32_t last = 0;
  const char *name() override { return "Storage"; }
  void enter() override { storageRetrySD(); dirty = true; }
  // volume card 296x76 r12, 12 px padding: glyph + title + status pill, 6 px bar, usage line
  void drawCard(int y, const char *title, bool present, bool active, uint64_t total, uint64_t used, const char *note) {
    card(MARGIN, y, ROW_W, 76, C.CARD, 12);
    int x = MARGIN + 12, r = W - MARGIN - 12;
    icon(x + 8, y + 22, IC_HARD_DRIVE, IC16, C.TEXT);
    text(x + 24, y + 22, title, C.TEXT, F16B, textdatum_t::middle_left);
    const char *st = !present ? "not found" : active ? "books here" : "idle";
    frame.setFont(F12M); int pw = frame.textWidth(st) + 16;
    frame.fillSmoothRoundRect(r - pw, y + 12, pw, 20, 10, active ? C.GREENBG : C.CARD2);
    text(r - pw / 2, y + 22, st, active ? C.GREEN : C.DIM, F12M, textdatum_t::middle_center);
    frame.fillSmoothRoundRect(x, y + 42, r - x, 6, 3, C.LINE);
    if (present && total) frame.fillSmoothRoundRect(x, y + 42, max(8, (int)(used * (r - x) / total)), 6, 3, active ? C.BLUE : C.DIM);
    text(x, y + 65, present ? (bytesText(used) + " used of " + bytesText(total)).c_str() : note, C.DIM, F12, textdatum_t::middle_left);
  }
  void render() override {
    frame.fillScreen(C.BG); drawStatusBar(); drawTitle("Storage");
    uint64_t ft = 0, fu = 0; bool flash = storageFlash(ft, fu);
    drawCard(CONTENT_Y, "SD card", storageSD, storageSD, storageSD ? SD_MMC.totalBytes() : 0, storageSD ? SD_MMC.usedBytes() : 0, "insert a FAT32 card (up to 32 GB)");
    drawCard(CONTENT_Y + 84, "Internal flash", flash, bookFS == &LittleFS, ft, fu, "");
    text(W / 2, 227, "Portal uploads go to the active volume", C.DIM, F12, textdatum_t::middle_center);
  }
  void tick() override { if (millis() - last > 3000) { last = millis(); storageRetrySD(); dirty = true; } }   // picks up a card inserted while open
} storageScreen;

// ---------- main settings list (scrollable) ----------
struct SettingsScreen : Screen {
  static constexpr int Y0 = CONTENT_Y, NROWS = 9;
  static constexpr int SL_X = W - MARGIN - 12 - 120, SL_W = 120;   // brightness slider, 12 px from the card's right edge
  static constexpr int TG_X = W - MARGIN - 8 - 76, TG_W = 76;      // 76x24 toggle, 8 px from the card's right edge
  enum { R_WIFI, R_LOC, R_BRIGHT, R_SLEEP, R_UNITS, R_THEME, R_FLIP, R_STORAGE, R_ABOUT };
  static constexpr int SLEEP_STEPS[] = {0, 1, 2, 5, 10, 30};   // minutes; 0 = never
  bool dragBright = false; int scroll = 0; uint32_t last = 0;
  const char *name() override { return "Settings"; }
  void enter() override { scroll = 0; }
  int rowY(int i) { return Y0 + i * ROW_PITCH - scroll; }
  void tile(int y, uint16_t col) { frame.fillSmoothRoundRect(MARGIN + 10, y + 6, 24, 24, 6, col); }   // 24 px icon tile r6; glyph centre (34, y+18)
  void toggle(int y, bool right, const char *l, const char *r) {
    frame.fillSmoothRoundRect(TG_X, y + 6, TG_W, 24, 12, C.CARD2);
    frame.fillSmoothRoundRect(TG_X + 2 + (right ? 36 : 0), y + 8, 36, 20, 10, C.BLUE);
    text(TG_X + 20, y + 18, l, right ? C.DIM : C.WHITE, F12M, textdatum_t::middle_center);
    text(TG_X + 56, y + 18, r, right ? C.WHITE : C.DIM, F12M, textdatum_t::middle_center);
  }
  void value(int cy, String v, int maxw = 140) { frame.setFont(F12); while (v.length() > 1 && frame.textWidth(v) > maxw) v.remove(v.length() - 1); text(CHEV_X - 18, cy, v.c_str(), C.DIM, F12, textdatum_t::middle_right); }
  void render() override {
    frame.fillScreen(C.BG); drawStatusBar(); drawTitle("Settings");
    frame.setClipRect(0, Y0, W, H - Y0);
    for (int i = 0; i < NROWS; i++) {
      int y = rowY(i); if (y > H || y + ROW_H < Y0) continue;
      card(MARGIN, y, ROW_W, ROW_H, C.CARD, 10); int cy = y + ROW_H / 2, gx = 34, gy = y + 18;
      switch (i) {
        case R_WIFI:
          tile(y, C.BLUE); icon(gx, gy, IC_WIFI, IC14, C.WHITE);
          text(56, cy, "Wi-Fi", C.TEXT, F16, textdatum_t::middle_left);
          value(cy, netConnected() ? WiFi.SSID() : cfg.ssid.length() ? cfg.ssid : "Off"); chevron(CHEV_X, cy, C.DIM); break;
        case R_LOC:
          tile(y, C.RED); glyphPin(gx, gy, C.WHITE, C.RED);
          text(56, cy, "Location", C.TEXT, F16, textdatum_t::middle_left);
          value(cy, cfg.place.length() ? cfg.place : "Automatic"); chevron(CHEV_X, cy, C.DIM); break;
        case R_BRIGHT: {
          tile(y, C.ORANGE); glyphSun(gx, gy, C.WHITE);
          text(56, cy, "Brightness", C.TEXT, F16, textdatum_t::middle_left);
          frame.fillSmoothRoundRect(SL_X, cy - 4, SL_W, 8, 4, C.LINE);
          int fill = (cfg.bright - BRIGHT_MIN) * SL_W / (BRIGHT_MAX - BRIGHT_MIN);
          frame.fillSmoothRoundRect(SL_X, cy - 4, fill, 8, 4, C.ORANGE); frame.fillSmoothCircle(SL_X + constrain(fill, 8, SL_W - 8), cy, 8, C.WHITE); break; }
        case R_SLEEP:
          tile(y, C.INDIGO); icon(gx, gy, IC_TIMER, IC14, C.WHITE);
          text(56, cy, "Display sleep", C.TEXT, F16, textdatum_t::middle_left);
          value(cy, cfg.sleepMin ? String(cfg.sleepMin) + " min" : "Never"); chevron(CHEV_X, cy, C.DIM); break;
        case R_UNITS:
          tile(y, C.GREEN); glyphThermo(gx, gy, C.WHITE, C.GREEN);
          text(56, cy, "Temperature", C.TEXT, F16, textdatum_t::middle_left);
          toggle(y, cfg.fahrenheit, "\xC2\xB0" "C", "\xC2\xB0" "F"); break;
        case R_THEME:
          tile(y, C.INDIGO); glyphMoon(gx, gy, C.WHITE, C.INDIGO);
          text(56, cy, "Appearance", C.TEXT, F16, textdatum_t::middle_left);
          toggle(y, themeDark, "Light", "Dark"); break;
        case R_FLIP:
          tile(y, C.SKY); glyphRotate(gx, gy, C.WHITE);
          text(56, cy, "Rotation", C.TEXT, F16, textdatum_t::middle_left);
          toggle(y, cfg.flip, "0\xC2\xB0", "180\xC2\xB0"); break;
        case R_STORAGE:
          tile(y, C.GREEN); glyphDrive(gx, gy, C.WHITE, C.GREEN);
          text(56, cy, "Storage", C.TEXT, F16, textdatum_t::middle_left);
          value(cy, storageName()); chevron(CHEV_X, cy, C.DIM); break;
        case R_ABOUT:
          tile(y, C.DIM); glyphInfo(gx, gy, C.WHITE);
          text(56, cy, "About", C.TEXT, F16, textdatum_t::middle_left);
          chevron(CHEV_X, cy, C.DIM); break;
      }
    }
    frame.clearClipRect();
  }
  void setBrightFromX(int x) { cfg.bright = constrain(BRIGHT_MIN + (x - SL_X) * (BRIGHT_MAX - BRIGHT_MIN) / SL_W, BRIGHT_MIN, BRIGHT_MAX); lcd.setBrightness(cfg.bright); dirty = true; }
  int rowAt(int x, int y) { if (y < Y0) return -1; for (int i = 0; i < NROWS; i++) if (hit(x, y, MARGIN, rowY(i), ROW_W, ROW_H)) return i; return -1; }
  void onDown(int x, int y) override { dragBright = (rowAt(x, y) == R_BRIGHT && x > SL_X - 16); if (dragBright) setBrightFromX(x); }
  void onMove(int x, int y, int dx, int dy) override {
    if (dragBright) { setBrightFromX(x); return; }
    int maxScroll = max(0, Y0 + NROWS * ROW_PITCH - 4 + MARGIN - H); scroll = constrain(scroll - dy, 0, maxScroll); dirty = true;
  }
  void onUp(int x, int y) override { if (dragBright) { dragBright = false; cfgSave(); } }
  void onTap(int x, int y) override {
    switch (rowAt(x, y)) {
      case R_WIFI: pushScreen(&wifiScreen); break;
      case R_LOC: pushScreen(&locationScreen); break;
      case R_SLEEP: { int n = sizeof SLEEP_STEPS / sizeof *SLEEP_STEPS, i = 0; while (i < n && SLEEP_STEPS[i] != cfg.sleepMin) i++;   // tap cycles the timeout
        cfg.sleepMin = SLEEP_STEPS[(i + 1) % n]; cfgSave(); dirty = true; break; }
      case R_UNITS: cfg.fahrenheit = !cfg.fahrenheit; cfgSave(); dirty = true; break;
      case R_THEME: cfg.dark = !cfg.dark; applyTheme(cfg.dark); cfgSave(); dirty = true; break;
      case R_FLIP: cfg.flip = !cfg.flip; applyRotation(cfg.flip); cfgSave(); dirty = true; break;
      case R_STORAGE: pushScreen(&storageScreen); break;
      case R_ABOUT: pushScreen(&aboutScreen); break;
    }
  }
  void tick() override { if (millis() - last > 2000) { last = millis(); dirty = true; } }
} settingsScreen;
