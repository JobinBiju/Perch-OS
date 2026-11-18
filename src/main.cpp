// Perch: a tiny phone-style launcher for the ES3C28P ESP32-S3 2.8" touch display.
// Home (clock + app icons) -> Robo face / Weather / Spotify / Book / Settings. Swipe from the left edge to go back;
// pull down from the status bar to adjust the brightness. Settings > Display sleep turns the backlight off after idle;
// Light sleep and Deep sleep (battery only) halt the chip until the screen is touched.
#include <Arduino.h>
#include "ui.h"
#include "net.h"
#include "keyboard.h"
#include "battery.h"
#include "home.h"
#include "spotify.h"
#include "portal.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

bool ccOpen = false;   // the control centre is down and owns the touch panel (see below)
// ---- navigation with transitions ----
static inline float easeOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
const lgfx::swap565_t *fbuf() { return (const lgfx::swap565_t *)frame.getBuffer(); }

// Screens get enter() before their first render, so any work it starts (scans, reads) lands before the
// slide instead of freezing its end; the slide's last step pushes only the strip not yet shown.
void pushScreen(Screen *s) {
  if (navDepth >= 6) return;
  ccOpen = false;
  if (current()) current()->leave();
  navStack[navDepth++] = s;
  s->enter(); s->render(); s->dirty = false;
  uint32_t t0 = millis(); const int dur = 220; int x = W;
  while (true) {
    float p = (millis() - t0) / (float)dur; if (p >= 1) break;
    x = (int)(W * (1 - easeOut(p)));
    lcd.pushImage(x, 0, W, H, fbuf());
  }
  if (x > 0) { lcd.setClipRect(0, 0, x, H); lcd.pushImage(0, 0, W, H, fbuf()); lcd.clearClipRect(); }
  if (s->dirty) { s->render(); flushAll(); s->dirty = false; }
  Serial.printf("nav: -> %s (depth %d)\n", s->name(), navDepth);
}
// `from` = how many px of the underlying screen are already revealed (edge-swipe in progress)
void popScreenAnimated(int from) {
  if (navDepth <= 1) return;
  ccOpen = false;
  Screen *top = current(); top->leave(); navDepth--;
  Screen *below = current();
  below->enter(); below->render(); below->dirty = false;
  uint32_t t0 = millis(); const int dur = 200; int x = from;
  while (true) {
    float p = (millis() - t0) / (float)dur; if (p >= 1) break;
    x = from + (int)((W - from) * easeOut(p));
    lcd.setClipRect(0, 0, x, H); lcd.pushImage(0, 0, W, H, fbuf()); lcd.clearClipRect();
  }
  if (x < W) { lcd.setClipRect(x, 0, W - x, H); lcd.pushImage(0, 0, W, H, fbuf()); lcd.clearClipRect(); }
  if (below->dirty) { below->render(); flushAll(); below->dirty = false; }
  Serial.printf("nav: <- %s (depth %d)\n", below->name(), navDepth);
}
void popScreen() { popScreenAnimated(0); }
void popScreens(int n) { while (n > 1 && navDepth > 2) { current()->leave(); navDepth--; n--; } popScreenAnimated(0); }  // drop n screens, animating only the last
void popToHome() { popScreens(navDepth - 1); }

// ---- touch + gestures (real touch merged with serial-injected synthetic touches) ----
struct Synth { bool active = false; int x0, y0, x1, y1; uint32_t t0, dur; } synth;
bool readTouch(int &x, int &y) {
  if (synth.active) {
    uint32_t el = millis() - synth.t0;
    if (el >= synth.dur) { synth.active = false; return false; }
    float p = synth.dur ? el / (float)synth.dur : 1; x = synth.x0 + (synth.x1 - synth.x0) * p; y = synth.y0 + (synth.y1 - synth.y0) * p; return true;
  }
  return lcd.getTouch(&x, &y);
}
bool edgeMode = false, edgePending = false, dragging = false; int tx0, ty0, txl, tyl; uint32_t tStart = 0;
static constexpr int EDGE_W = 28, BACK_THRESH = 110;

// ---- control centre: a touch that starts in the status bar and moves down pulls a panel over the screen. It holds four
// tiles (Wi-Fi, Airplane, Dark, Rotate), the brightness slider, and Sleep and Settings. It stays until it is pushed back
// up, or the screen below it is tapped. Landscape: 320x196 with the tiles in a row; portrait: 240x260 with them 2 x 2.
bool pullPending = false, pullMode = false; uint32_t pullSeen = 0;
static constexpr int PULL_ZONE = 36, PULL_THRESH = 12, PULL_DROP_MS = 150;   // start in the top 36 px
int ccReveal = 0, ccPrev = 0; uint32_t ccDrawn = 0;   // ccReveal: how many px of the panel show (its bottom edge)
void displaySleep(); void lightSleep(int secs); void bookRelayout();
static int ccH() { return portrait() ? 260 : 196; }
static int ccCols() { return portrait() ? 2 : 4; } static int ccTileW() { return portrait() ? 104 : 68; }
static int ccTileX(int i) { return MARGIN + (i % ccCols()) * (ccTileW() + 8); } static int ccTileY(int i) { return 28 + (i / ccCols()) * 64; }
static int ccBrightY() { return portrait() ? 156 : 92; } static int ccActY() { return ccBrightY() + 44; } static int ccBtnW() { return (W - 2 * MARGIN - 8) / 2; }
static int ccTrackX() { return MARGIN + 12 + 16 + 10; } static int ccTrackW() { return W - 2 * MARGIN - 24 - 16 - 10 - 10 - 30; }
void ccDraw() {   // over whatever is in the frame; everything is shifted up by the part of the panel still hidden
  int ph = ccH(), o = ph - ccReveal;
  frame.setClipRect(0, 0, W, ccReveal);
  frame.fillSmoothRoundRect(0, -o - 16, W, ph + 16, 16, C.CARD);   // only the bottom corners are on screen
  drawStatusBar(-o, true);
  struct Tile { const char *glyph, *label; bool on; } tiles[4] = {{IC_WIFI, "Wi-Fi", netEnabled()}, {IC_PLANE, "Airplane", cfg.airplane}, {IC_MOON, "Dark", themeDark}, {IC_ROTATE_CW, "Rotate", false}};
  for (int i = 0; i < 4; i++) {
    int x = ccTileX(i), y = ccTileY(i) - o, w = ccTileW(); bool on = tiles[i].on;
    frame.fillSmoothRoundRect(x, y, w, 56, 12, on ? C.ACCENT : C.CARD2);
    icon(x + w / 2, y + 19, tiles[i].glyph, IC20, on ? C.WHITE : C.TEXT);
    text(x + w / 2, y + 41, tiles[i].label, on ? C.WHITE : C.DIM, F12M, textdatum_t::middle_center);
  }
  int by = ccBrightY() - o, cy = by + 18, tx = ccTrackX(), tw = ccTrackW();
  frame.fillSmoothRoundRect(MARGIN, by, W - 2 * MARGIN, 36, 10, C.CARD2);
  icon(MARGIN + 20, cy, IC_SUN, IC16, C.ORANGE);
  frame.fillSmoothRoundRect(tx, cy - 4, tw, 8, 4, C.LINE);
  int fill = (cfg.bright - BRIGHT_MIN) * tw / (BRIGHT_MAX - BRIGHT_MIN);
  frame.fillSmoothRoundRect(tx, cy - 4, fill, 8, 4, C.ORANGE); frame.fillSmoothCircle(tx + constrain(fill, 8, tw - 8), cy, 8, C.WHITE);
  textf(W - MARGIN - 12, cy, C.TEXT, F12M, textdatum_t::middle_right, "%d%%", (cfg.bright - BRIGHT_MIN) * 100 / (BRIGHT_MAX - BRIGHT_MIN));
  static const char *const act[2] = {"Sleep", "Settings"}; static const char *const actIcon[2] = {IC_POWER, IC_SETTINGS};
  for (int i = 0; i < 2; i++) {
    int x = MARGIN + i * (ccBtnW() + 8), y = ccActY() - o, w = ccBtnW();
    frame.fillSmoothRoundRect(x, y, w, 36, 10, C.CARD2);
    frame.setFont(F16); int lw = frame.textWidth(act[i]), ix = x + (w - (16 + 8 + lw)) / 2;
    icon(ix + 8, y + 18, actIcon[i], IC16, C.TEXT); text(ix + 24, y + 18, act[i], C.TEXT, F16, textdatum_t::middle_left);
  }
  frame.fillSmoothRoundRect(W / 2 - 20, ccActY() + 46 - o, 40, 4, 2, C.LINE);   // grabber
  frame.clearClipRect();
}
// Redraw the screen underneath, lay the panel over it, and push the rows that changed.
void ccShow(bool all = false) {
  Screen *s = current(); s->render(); s->dirty = false;
  if (ccReveal > 0) ccDraw();
  if (all) flushAll(); else flushRows(0, max(ccReveal, ccPrev) + 2);
  ccPrev = ccReveal; ccDrawn = millis();
}
void ccSettle(bool open) {   // ease the panel the rest of the way down, or back up
  int from = ccReveal, to = open ? ccH() : 0; uint32_t t0 = millis();
  while (from != to) { float p = (millis() - t0) / 160.0f; if (p >= 1) break; ccReveal = from + (int)((to - from) * easeOut(p)); ccShow(); }
  ccReveal = to; ccOpen = open; ccShow(!open);
}
void ccPull(int y) { ccReveal = constrain(y + 12, 0, ccH()); ccShow(); }   // the panel's edge follows the finger
// Wi-Fi or Airplane changed: power the radio down, or bring it back and reconnect.
void netApplyRadio() {
  if (netEnabled()) { if (WiFi.getMode() == WIFI_OFF) { WiFi.mode(WIFI_STA); WiFi.setSleep(true); netState = NS_IDLE; netNextTry = 0; netConnect(); } return; }
  xSemaphoreTake(netMutex, portMAX_DELAY);   // let a request in flight finish
  portalNetDown(); WiFi.disconnect(true); WiFi.mode(WIFI_OFF); netState = NS_IDLE;
  xSemaphoreGive(netMutex);
}
void ccSetBright(int x) { cfg.bright = constrain(BRIGHT_MIN + (x - ccTrackX()) * (BRIGHT_MAX - BRIGHT_MIN) / ccTrackW(), BRIGHT_MIN, BRIGHT_MAX); lcd.setBrightness(cfg.bright); ccShow(); }
void ccTap(int x, int y) {
  if (y >= ccH()) { ccSettle(false); return; }   // a tap on the screen below closes it
  for (int i = 0; i < 4; i++) if (hit(x, y, ccTileX(i) - 4, ccTileY(i) - 4, ccTileW() + 8, 64)) {
    if (i == 0) { if (cfg.airplane) { cfg.airplane = false; cfg.wifiOn = true; } else cfg.wifiOn = !cfg.wifiOn; }   // turning Wi-Fi on also leaves airplane mode
    else if (i == 1) cfg.airplane = !cfg.airplane;
    else if (i == 2) { cfg.dark = !cfg.dark; applyTheme(cfg.dark); }
    else { cfg.rot = (cfg.rot + 1) & 3; applyRotation(cfg.rot); bookRelayout(); current()->enter(); ccReveal = ccPrev = ccH(); }
    cfgSave();
    if (i < 2) { ccShow(); netApplyRadio(); }   // show the tile first: switching the radio off can take a moment
    ccShow(true); return;
  }
  for (int i = 0; i < 2; i++) if (hit(x, y, MARGIN + i * (ccBtnW() + 8), ccActY() - 4, ccBtnW(), 44)) {
    ccOpen = false; ccReveal = 0;
    if (i == 0) { ccShow(true); if (bat.usb) displaySleep(); else lightSleep(0); return; }   // on battery: light sleep, which the deep-sleep timer follows
    int k = -1; for (int d = 0; d < navDepth; d++) if (navStack[d] == &settingsScreen) k = d;
    if (k == navDepth - 1) ccShow(true); else if (k >= 0) popScreens(navDepth - 1 - k); else pushScreen(&settingsScreen);
    return;
  }
}
bool touching = false;
// While the panel is down every touch is its own: taps on the controls, a drag on the slider, a push upwards to close.
void ccTouch(bool t, int x, int y) {
  static bool down = false; static int x0, y0, lx, ly, mode;   // mode: 0 undecided, 1 brightness slider, 2 pushing the panel up
  if (t && !down) {
    down = touching = true; x0 = lx = x; y0 = ly = y; mode = hit(x, y, MARGIN, ccBrightY() - 6, W - 2 * MARGIN, 48) ? 1 : 0;
    if (mode == 1) ccSetBright(x);
  } else if (t) {
    if (mode == 1) { if (x != lx) ccSetBright(x); }
    else { if (mode == 0 && y0 - y > 16) mode = 2; if (mode == 2 && y != ly) { ccReveal = constrain(ccH() + (y - y0), 0, ccH()); ccShow(); } }
    lx = x; ly = y;
  } else if (down) {
    down = touching = false;
    if (mode == 1) cfgSave();
    else if (mode == 2) ccSettle(ccReveal > ccH() - 48);
    else if (abs(lx - x0) + abs(ly - y0) < 16) ccTap(x0, y0);
  }
}

// Display sleep: after cfg.sleepMin minutes without a touch the backlight goes off; the next touch only wakes it.
uint32_t lastActivity = 0; bool asleep = false, wakeTouch = false;
void displaySleep() { if (asleep) return; asleep = true; lcd.setBrightness(0); bookSyncPush(); Serial.println("display: sleep"); }
void displayWake() { lastActivity = millis(); if (!asleep) return; asleep = false; lcd.setBrightness(cfg.bright); Serial.println("display: wake"); }

// Battery sleep. Both wake on the touch controller's INT line (GPIO 17, an RTC pin, low while a finger is down).
// Light sleep: panel asleep, Wi-Fi off, CPU halted; RAM and the open screen survive, Wi-Fi reconnects on wake.
// Deep sleep: everything off but the RTC; a touch reboots the board. `secs` > 0 also wakes on a timer (serial tests).
// A computer on USB drops the board while it sleeps and does not look again by itself. Only reachable through the serial
// `y` command (automatic sleep is off while a computer is attached): a few seconds after such a wake, if no host is
// sending frames, detach and re-attach the USB pull-up so the serial port comes back.
uint32_t usbCheckAt = 0;
void usbReattachLoop() {
  if (!usbCheckAt || (int32_t)(millis() - usbCheckAt) < 0) return;
  usbCheckAt = 0;
  uint32_t f0 = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & 0x7FF; delay(20);
  if ((REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & 0x7FF) != f0) return;   // the host is still talking to us
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP); delay(100);
  SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);
}
int lastSleepS = -1, lastSleepCause = 0;   // last light sleep: seconds slept and what ended it (esp_sleep_wakeup_cause_t), for the `p` line
static constexpr gpio_num_t WAKE_PIN = GPIO_NUM_17, BL_PIN = GPIO_NUM_45, TP_RST_PIN = GPIO_NUM_18, AMP_PIN = GPIO_NUM_1;
RTC_DATA_ATTR uint16_t deepCount = 0, wakeCount = 0, strayCount = 0;   // since power was applied: deep sleeps, touch wakes, wakes nobody followed up on
bool strayWatch = false;                                                // woken from deep sleep by a touch, and not touched again yet
void deepSleep(int secs = 0) {
  Serial.printf("power: deep sleep%s\n", secs ? " (timed)" : ""); Serial.flush();
  bookSave(); bookSyncPush(); syncFlush(6000); deepCount++; batSleepMv = bat.raw;
  lcd.setBrightness(0); lcd.sleep(); portalNetDown(); WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  face::codecOff(); neopixelWrite(PIN_LED, 0, 0, 0); if (storageSD) SD_MMC.end();   // nothing but the touch chip stays powered up
  digitalWrite(PIN_AMP_EN, HIGH); gpio_hold_en(AMP_PIN);                             // or the amplifier's enable pin floats and it may switch on
  gpio_hold_en(BL_PIN); gpio_hold_en(TP_RST_PIN); gpio_deep_sleep_hold_en();   // backlight stays off, the touch chip stays out of reset (released in setup)
  rtc_gpio_pullup_en(WAKE_PIN); rtc_gpio_pulldown_dis(WAKE_PIN);
  esp_sleep_enable_ext0_wakeup(WAKE_PIN, 0);
  if (secs) esp_sleep_enable_timer_wakeup(secs * 1000000ULL);
  esp_deep_sleep_start();
}
void lightSleep(int secs = 0) {
  Serial.printf("power: light sleep%s\n", secs ? " (timed)" : "");
  bookSyncPush(); syncFlush(6000);
  xSemaphoreTake(netMutex, portMAX_DELAY);   // let a request in flight finish; the Spotify and weather workers park on this
  ccOpen = false;
  lcd.setBrightness(0); lcd.sleep(); asleep = true;
  portalNetDown(); WiFi.disconnect(true); WiFi.mode(WIFI_OFF); face::codecOff();
  Serial.flush(); delay(50);
  static bool hadHost = false; hadHost |= bat.host;   // sticky: right after a re-attach the host flag can read false for a few seconds
  disableCore0WDT();                          // a sleep longer than the 5 s task watchdog would reboot the board on wake
  uint32_t t0 = millis(); bool deep = false;   // millis() keeps counting through light sleep
  gpio_wakeup_enable(WAKE_PIN, GPIO_INTR_LOW_LEVEL); esp_sleep_enable_gpio_wakeup();
  for (;;) {
    uint32_t left = secs ? (uint32_t)secs * 1000UL - min((uint32_t)secs * 1000UL, (uint32_t)(millis() - t0)) : 30000;
    if (secs && left < 200) break;                                           // timed test: done
    esp_sleep_enable_timer_wakeup(min(left, (uint32_t)30000) * 1000ULL);     // surface every 30 s to check the deep-sleep deadline
    esp_light_sleep_start();
    lastSleepCause = (int)esp_sleep_get_wakeup_cause();
    if (lastSleepCause == ESP_SLEEP_WAKEUP_GPIO) break;
    if (secs) continue;
    if (cfg.deepMin && millis() - lastActivity > (uint32_t)cfg.deepMin * 60000UL) { deep = true; break; }
  }
  gpio_wakeup_disable(WAKE_PIN); esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  enableCore0WDT();
  if (hadHost) usbCheckAt = millis() + 3000;
  if (deep) { xSemaphoreGive(netMutex); deepSleep(); }
  face::codecOn();
  if (netEnabled()) { WiFi.mode(WIFI_STA); WiFi.setSleep(true); netState = NS_IDLE; netNextTry = 0; netConnect(); }
  xSemaphoreGive(netMutex);
  lcd.wakeup(); delay(120);                   // the panel needs 120 ms after sleep-out
  Screen *s = current(); s->render(); flushAll(); s->dirty = false;
  lcd.setBrightness(cfg.bright); asleep = false; lastActivity = millis(); wakeTouch = true;   // the touch that woke it does nothing else
  lastSleepS = (millis() - t0) / 1000; Serial.printf("power: awake after %d s (cause %d)\n", lastSleepS, lastSleepCause);
}
void sleepLoop() {
  if (touching) return;
  uint32_t idle = millis() - lastActivity;
  if (cfg.sleepMin && !asleep && !face::ambient && idle > (uint32_t)cfg.sleepMin * 60000UL) { if (cfg.ambient && bat.usb) face::ambientStart(); else displaySleep(); }
  if (face::ambient) { if (!bat.usb) { face::ambientEnd(); displaySleep(); } return; }   // the screensaver needs USB power, and holds off the deeper sleeps
  if (face::focusEnd) return;                 // a running focus timer keeps the CPU awake
  if (bat.usb) return;                        // on USB power, a computer or a charger, only the display sleeps
  if (strayWatch && idle > 60000) { strayCount++; deepSleep(); }   // woken by a brush in a bag or pocket: straight back to sleep
  if (cfg.deepMin && idle > (uint32_t)cfg.deepMin * 60000UL) deepSleep();
  else if (cfg.lightMin && idle > (uint32_t)cfg.lightMin * 60000UL) lightSleep();
}

void handleTouch() {
  int x, y; bool t = readTouch(x, y);
  Screen *s = current();
  if (t && !touching && asleep) { wakeTouch = true; displayWake(); }   // a touch on a dark screen only wakes it
  if (t && !touching && face::ambient) { wakeTouch = true; face::ambientEnd(); }   // and a touch on the screensaver only ends it
  if (wakeTouch) { if (!t) wakeTouch = false; return; }
  if (t) { lastActivity = millis(); strayWatch = false; }
  if (ccOpen) { ccTouch(t, x, y); return; }
  if (pullMode) { if (t) pullSeen = millis(); else if (millis() - pullSeen < PULL_DROP_MS) return; }   // the touch panel loses the finger for a moment near its top edge: do not take that as a release
  if (t && !touching) {
    touching = true; tx0 = txl = x; ty0 = tyl = y; tStart = millis(); dragging = false; edgeMode = false; pullMode = false;
    pullPending = y < PULL_ZONE;                                   // maybe a pull on the control centre: decide once the finger moves
    edgePending = !pullPending && (x < EDGE_W && navDepth > 1);   // maybe a back-swipe: decide once the finger moves
    if (!edgePending && !pullPending) s->onDown(x, y);
  } else if (t && touching) {
    int dx = x - txl, dy = y - tyl;
    if (pullPending) {
      if (y - ty0 > PULL_THRESH) { pullPending = false; pullMode = true; pullSeen = millis(); ccPrev = 0; ccPull(y); }
      else if (abs(x - tx0) > 16 && abs(x - tx0) > 3 * (y - ty0)) { pullPending = false; s->onDown(tx0, ty0); dragging = true; }   // clearly sideways: not a pull
    }
    if (pullMode) { if (y != tyl) ccPull(y); txl = x; tyl = y; return; }
    if (edgePending) {
      if (x - tx0 > 8) { edgePending = false; edgeMode = true; navDepth--; navStack[navDepth]->render(); navDepth++; } // underlying screen into frame
      else if (abs(y - ty0) > 12 || x - tx0 < -8) { edgePending = false; s->onDown(tx0, ty0); dragging = true; }
    }
    if (edgeMode) { if (x != txl) { lcd.setClipRect(0, 0, x, H); lcd.pushImage(0, 0, W, H, fbuf()); lcd.clearClipRect(); } }
    else if (!edgePending && !pullPending && (dx || dy)) { if (!dragging && abs(x - tx0) + abs(y - ty0) > 10) dragging = true; if (dragging) s->onMove(x, y, dx, dy); }
    txl = x; tyl = y;
  } else if (!t && touching) {
    touching = false;
    if (pullMode) { pullMode = false; ccSettle(ccReveal > 56); return; }   // far enough: it comes down and stays; else it goes back
    if (pullPending) { pullPending = false; s->onDown(tx0, ty0); }                                       // plain tap in the status bar
    if (edgeMode) {
      edgeMode = false;
      if (txl > BACK_THRESH || (txl > 60 && millis() - tStart < 250)) popScreenAnimated(txl);
      else if (navDepth > 1 && s->backHit(tx0, ty0) && millis() - tStart < 700) { s->render(); flushAll(); s->onBack(); }   // short tap on the chevron that slid a bit
      else { s->render(); flushAll(); s->dirty = false; }
    } else {
      if (edgePending) { edgePending = false; s->onDown(tx0, ty0); }
      s->onUp(txl, tyl);
      bool back = navDepth > 1 && s->backHit(tx0, ty0) && abs(txl - tx0) < 24 && abs(tyl - ty0) < 24;   // chevron: tolerate a slide
      if (millis() - tStart < 700 && (back || !dragging)) { if (back) s->onBack(); else s->onTap(tx0, ty0); }
    }
  }
}

// ---- last crash, kept in RTC memory across the reset so it can be read after the reboot (serial `P`) ----
struct PanicRec { uint32_t magic; char reason[48]; uint32_t pc; int core, n; uint32_t bt[32]; };
RTC_NOINIT_ATTR PanicRec panicRec;
void panicHook(arduino_panic_info_t *info, void *) {
  panicRec.magic = 0xC0FFEE01; strlcpy(panicRec.reason, info->reason ? info->reason : "?", sizeof panicRec.reason);
  panicRec.pc = (uint32_t)info->pc; panicRec.core = info->core; panicRec.n = min((int)info->backtrace_len, 32);
  for (int i = 0; i < panicRec.n; i++) panicRec.bt[i] = info->backtrace[i];
}
void panicPrint() {
  if (panicRec.magic != 0xC0FFEE01) { Serial.println("panic: none recorded"); return; }
  Serial.printf("panic: %s core %d pc 0x%08lx bt", panicRec.reason, panicRec.core, (unsigned long)panicRec.pc);
  for (int i = 0; i < panicRec.n; i++) Serial.printf(" 0x%08lx", (unsigned long)panicRec.bt[i]);
  Serial.println();
}

// ---- serial debug ----
void screenDump() {
  static uint16_t *buf = (uint16_t *)ps_malloc(W * H * 2); if (!buf) return;
  lcd.readRect(0, 0, W, H, buf);
  Serial.printf("SCR %d %d\n", W, H); Serial.write((uint8_t *)buf, W * H * 2); Serial.println("\nEND");
}
void handleSerial() {
  static char line[200]; static int n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (n == 0 && strchr("Sb12345pWTrizP", c)) {
      displayWake();
      if (c == 'S') screenDump();
      else if (c == 'z') displaySleep();   // display sleep now
      else if (c == 'P') panicPrint();     // the last crash since power-on, if any
      else if (c == 'b') popScreen();
      else if (c >= '1' && c <= '5') { if (navDepth == 1) { if (apps[c - '1'].launch) apps[c - '1'].launch(); else pushScreen(apps[c - '1'].screen); } }
      else if (c == 'W') { static int wn = 0; static const int wc[] = {2, 0, 61, 95, 3, 71}; wx.ok = true; wx.city = "Kalamassery"; wx.temp = 29; wx.code = wc[wn++ % 6]; wx.hum = 74; wx.wind = 12; wx.nDays = 5; for (int i = 0; i < 5; i++) wx.days[i] = {i % 3 == 0 ? 61 : 2, 31.f, 24.f, (1 + i) % 7}; wx.fetchedAt = millis(); current()->dirty = true; }  // fake weather for layout tests; each press steps to the next kind of sky
      else if (c == 'T') { applyTheme(!themeDark); current()->dirty = true; }   // preview the other theme (not saved)
      else if (c == 'p') { time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm); char tb[32]; strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", &tm);
        Serial.printf("time %s tz %ld bright %d wifi %d ssid %s wx ok %d city %s err %s home page %d storage %s ip %s gw %s mask %s bssid %s ch %d\n", tb, cfg.tzOff, cfg.bright, WiFi.status(), WiFi.SSID().c_str(), wx.ok, wx.city.c_str(), wx.err.c_str(), homeScreen.page, storageText().c_str(), WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.subnetMask().toString().c_str(), WiFi.BSSIDstr().c_str(), WiFi.channel());
        Serial.printf("sleep %d min asleep %d idle %lu s light %d min deep %d min host %d last light sleep %d s cause %d reset reason %d\n", cfg.sleepMin, asleep, (unsigned long)((millis() - lastActivity) / 1000), cfg.lightMin, cfg.deepMin, bat.host, lastSleepS, lastSleepCause, (int)esp_reset_reason());
        Serial.printf("deep sleeps %d touch wakes %d stray wakes %d\n", deepCount, wakeCount, strayCount);
        Serial.printf("sync on %d busy %d err %s book %s ch %d posT %lu\n", syncOn(), sy.busy, sy.err.c_str(), book.file.c_str(), book.ch, (unsigned long)book.posT);
        Serial.printf("notes: %d marks %d highlights %d tombs dirty %d\n", nmarks, nnotes, ntombs, notesDirty);
        Serial.printf("bat %d mV raw %d pct %d usb %d host %d charging %d full %d lift %d trend %d\n", bat.mv, bat.raw, bat.pct, bat.usb, bat.host, bat.charging, bat.full, bat.lift, bat.trend);
        Serial.printf("sp linked %d busy %d pending %d want %d lastPoll %lu have %d err %s access %d wxbusy %d mutex %s\n", sp.linked, sp.busy, sp.pending, sp.wantPolling, (unsigned long)sp.lastPoll, sp.have, sp.err.c_str(), sp.access.length(), wx.busy, xSemaphoreGetMutexHolder(netMutex) ? pcTaskGetName(xSemaphoreGetMutexHolder(netMutex)) : "free"); }
      else if (c == 'i') { Serial.print("i2c:"); for (int a = 1; a < 127; a++) { Wire.beginTransmission(a); if (!Wire.endTransmission()) Serial.printf(" 0x%02x", a); } Serial.println(); }   // bus scan
      else if (c == 'r') { storageTried = 0; storageRetrySD(); Serial.printf("storage: %s\n", storageText().c_str()); }
      continue;
    }
    if (c == '\n' || c == '\r') {
      line[n] = 0;
      int a, b, cc, d;
      if (sscanf(line, "t %d %d", &a, &b) == 2) { synth = {true, a, b, a, b, millis(), 80}; }
      else if (line[0] == 'y' && (!line[1] || line[1] == ' ')) lightSleep(atoi(line + 1));   // light sleep now; "y 40" also wakes after 40 s
      else if (line[0] == 'Y' && (!line[1] || line[1] == ' ')) deepSleep(atoi(line + 1));    // deep sleep now; "Y 15" also wakes after 15 s
      else if (line[0] == 'R' && line[1] == ' ') { cfg.rot = atoi(line + 2) & 3; applyRotation(cfg.rot); bookRelayout(); current()->enter(); current()->dirty = true; }   // rotation 0..3 quarter turns (not saved)
      else if (line[0] == 'm' && line[1] == ' ') { int v = atoi(line + 2); if (v >= 90) face::debug(v); else face::force = v; }   // hold Robo expression N; -1 releases; 90 and up are test hooks
      else if (line[0] == 'f' && line[1] == ' ') {   // font test: draw the text in every font
        const lgfx::IFont *fs[] = {F12, F12M, F16, F16B, RD12, RD16, RD20, F24B}; frame.fillScreen(C.BG); int y = 4;
        for (auto f : fs) { text(8, y, line + 2, C.TEXT, f); frame.setFont(f); y += frame.fontHeight() + 4; }
        flushAll(); current()->dirty = false;
      }
      else if (sscanf(line, "n %d.%d.%d.%d", &a, &b, &cc, &d) == 4) {   // reachability test: TCP connect to a LAN host (resolves ARP from this side)
        WiFiClient tc; uint32_t t0 = millis(); bool ok = tc.connect(IPAddress(a, b, cc, d), 80, 3000); tc.stop();
        Serial.printf("net: %d.%d.%d.%d:80 %s in %lu ms\n", a, b, cc, d, ok ? "connected" : "no connection", (unsigned long)(millis() - t0)); }
      else if (sscanf(line, "d %d %d %d %d", &a, &b, &cc, &d) == 4) { int ms = 350; sscanf(line, "d %*d %*d %*d %*d %d", &ms); synth = {true, a, b, cc, d, millis(), (uint32_t)ms}; }   // optional 5th value: duration ms
      else if (!strncmp(line, "ls", 2)) {   // list /books with sizes and the first bytes of each file
        File dir = bookFS ? bookFS->open(BOOK_DIR) : File(); if (dir) for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
          uint8_t h[48]; int r = f.read(h, sizeof h); Serial.printf("%s %u bytes:", f.name(), (unsigned)f.size()); for (int i = 0; i < r && i < 40; i++) Serial.printf(" %02x", h[i]); Serial.println(); f.close(); }
      }
      else if (line[0] == 'h' && line[1] == ' ') { String host = line + 2; host.trim(); /* reachability: DNS, TCP 443, TLS */ IPAddress ip; uint32_t t0 = millis(); bool ok = WiFi.hostByName(host.c_str(), ip);
        Serial.printf("dns %s -> %s (%d) in %lu ms\n", host.c_str(), ok ? ip.toString().c_str() : "fail", ok, (unsigned long)(millis() - t0));
        if (ok) { WiFiClient tc; t0 = millis(); bool c1 = tc.connect(ip, 443, 5000); Serial.printf("tcp 443 %d in %lu ms\n", c1, (unsigned long)(millis() - t0)); tc.stop();
          WiFiClientSecure sc; sc.setInsecure(); sc.setTimeout(8); t0 = millis(); bool c2 = sc.connect(host.c_str(), 443); Serial.printf("tls %d in %lu ms heap %u\n", c2, (unsigned long)(millis() - t0), ESP.getFreeHeap()); sc.stop(); } }
      else if (!strncmp(line, "dict ", 5)) {   // "dict make": a three-word test dictionary when the volume has none; "dict rm": remove it; "dict <word>": look it up
        String arg = line + 5; arg.trim();
        if (arg == "make" && bookFS && !bookFS->exists("/dict/zz-test.ifo")) {
          static const char *const W3[3] = {"agenda", "heal", "norm"}, *const D3[3] = {"noun\na list of matters to be taken up\ne.g. the meeting's agenda\nalso: docket, schedule", "verb\nget healthy again\nalso: mend", "noun\na standard regarded as typical\ne.g. the current middle-class norm"};
          bookFS->mkdir("/dict"); dict::close(); File i = bookFS->open("/dict/zz-test.idx", FILE_WRITE), d = bookFS->open("/dict/zz-test.dict", FILE_WRITE); uint32_t off = 0;
          for (int k = 0; k < 3; k++) { uint32_t n = strlen(D3[k]); d.write((const uint8_t *)D3[k], n); i.write((const uint8_t *)W3[k], strlen(W3[k]) + 1); uint8_t b[8] = {(uint8_t)(off >> 24), (uint8_t)(off >> 16), (uint8_t)(off >> 8), (uint8_t)off, 0, 0, (uint8_t)(n >> 8), (uint8_t)n}; i.write(b, 8); off += n; }
          i.close(); d.close(); File f = bookFS->open("/dict/zz-test.ifo", FILE_WRITE); f.print("StarDict's dict ifo file\nversion=2.4.2\nbookname=Test dictionary\nwordcount=3\nsametypesequence=m\n"); f.close(); Serial.println("dict: test dictionary written");
        } else if (arg == "rm" && bookFS) { dict::close(); for (const char *e : {".ifo", ".idx", ".dict"}) bookFS->remove(String("/dict/zz-test") + e); bookFS->rmdir("/dict"); Serial.println("dict: test dictionary removed"); }
        else { dict::Result r = dict::lookup(arg.c_str()); Serial.printf("dict: %s -> have %d found %d head %s | %.80s\n", arg.c_str(), r.haveDict, r.found, r.head.c_str(), r.text.c_str()); }
      }
      else if (!strncmp(line, "clip", 4)) {   // "clip": size of clippings.txt; "clip rm": delete it
        if (!bookFS) Serial.println("clip: no volume"); else if (!strcmp(line, "clip rm")) { bookFS->remove("/clippings.txt"); Serial.println("clip: removed"); }
        else { File f = bookFS->open("/clippings.txt"); Serial.printf("clip: %u bytes\n", f ? (unsigned)f.size() : 0); if (f) { while (f.available()) Serial.write(f.read()); f.close(); } }
      }
      else if (!strncmp(line, "sync ", 5)) {   // cloud sync: "sync <https address> <home key>", or "sync off"
        String v = line + 5; v.trim(); int sp1 = v.indexOf(' ');
        if (v == "off") { cfg.syncUrl = ""; cfg.syncKey = ""; } else if (sp1 > 0) { cfg.syncUrl = v.substring(0, sp1); cfg.syncKey = v.substring(sp1 + 1); cfg.syncKey.trim(); }
        cfgSave(); sy.lastPushed = ""; Serial.printf("sync: %s\n", syncOn() ? "on" : "off"); }
      else if (!strncmp(line, "spc ", 4)) { sp.clientId = line + 4; spSave(); Serial.println("spotify: client id saved"); }
      else if (!strncmp(line, "sp ", 3)) { sp.refresh = line + 3; sp.access = ""; spSave(); sp.linked = sp.refresh.length() && sp.clientId.length(); sp.err = ""; Serial.println("spotify: token saved"); }
      else if (line[0] == 'k' && line[1] == ' ') { // type text through the keyboard target
        if (kb.visible && kb.target) { *kb.target += (line + 2); if (kb.onChange) kb.onChange(); }
      }
      n = 0;
    } else if (n < 199) line[n++] = c;
  }
}

void setup() {
  pinMode(PIN_AMP_EN, OUTPUT); digitalWrite(PIN_AMP_EN, HIGH);   // amplifier off (active low) until Robo speaks
  Serial.begin(115200);
  Serial.println("\n=== Perch ===");
  set_arduino_panic_handler(panicHook, nullptr);
  esp_sleep_wakeup_cause_t wokeBy = esp_sleep_get_wakeup_cause();   // EXT0 = a touch ended deep sleep
  gpio_hold_dis(BL_PIN); gpio_hold_dis(TP_RST_PIN); gpio_hold_dis(AMP_PIN); gpio_deep_sleep_hold_dis(); rtc_gpio_deinit(WAKE_PIN);
  if (wokeBy == ESP_SLEEP_WAKEUP_EXT0) { wakeCount++; strayWatch = true; }
  Serial.printf("boot: reset reason %d\n", (int)esp_reset_reason());   // 1 power-on, 3 software, 4 panic, 6 task watchdog, 8 deep sleep
  if (wokeBy != ESP_SLEEP_WAKEUP_UNDEFINED) Serial.printf("power: woke from deep sleep (cause %d)\n", (int)wokeBy);
  cfgLoad(); spLoad();
  lcd.init(); applyAccent(cfg.accent); applyTheme(cfg.dark); applyRotation(cfg.rot); lcd.setBrightness(cfg.bright); lcd.fillScreen(C.BG);
  frame.setPsram(true); frame.setColorDepth(16);   // PSRAM: keeps ~150 KB of internal RAM free for TLS
  if (!frame.createSprite(W, H)) Serial.println("frame alloc FAILED");
  fontsBegin();
  face::micOk = face::micInit();
  randomSeed(esp_random());
  applyTz();
  netBegin();
  spBegin(); storageBegin(); bookBegin(); portalBegin(); syncBegin();
  navStack[0] = &homeScreen; navDepth = 1;
  homeScreen.render(); flushAll(); homeScreen.enter();
  neopixelWrite(PIN_LED, 0, 0, 0);
  lastActivity = millis(); wakeTouch = wokeBy == ESP_SLEEP_WAKEUP_EXT0;   // the touch that woke the board does nothing else
  Serial.printf("ready: mic %s, heap %lu, psram %lu\n", face::micOk ? "ok" : "off", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());
}

void loop() {
  static uint32_t lastLog = 0; static int frames = 0;
  handleSerial();
  netLoop(); batLoop(); spLoop(); portalLoop();
  handleTouch(); sleepLoop(); usbReattachLoop(); face::focusLoop();
  Screen *s = current();
  if (ccOpen && !touching && millis() - ccDrawn > 5000) ccShow();   // keeps its clock and Wi-Fi state current
  if (!edgeMode && !pullMode && !ccOpen) {   // a slide or the control centre owns the panel
    s->tick();
    if (s->dirty) { s->render(); flushAll(); s->dirty = false; }
  }
  frames++;
  if (millis() - lastLog > 10000) { Serial.printf("fps %.1f, screen %s, heap %lu\n", frames / 10.0f, s->name(), (unsigned long)ESP.getFreeHeap()); frames = 0; lastLog = millis(); }
  if (s != &face::faceScreen) delay(5); // idle screens don't need to spin
}
