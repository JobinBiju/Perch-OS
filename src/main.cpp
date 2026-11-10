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

// ---- navigation with transitions ----
static inline float easeOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
const lgfx::swap565_t *fbuf() { return (const lgfx::swap565_t *)frame.getBuffer(); }

// Screens get enter() before their first render, so any work it starts (scans, reads) lands before the
// slide instead of freezing its end; the slide's last step pushes only the strip not yet shown.
void pushScreen(Screen *s) {
  if (navDepth >= 6) return;
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
bool touching = false, edgeMode = false, edgePending = false, dragging = false; int tx0, ty0, txl, tyl; uint32_t tStart = 0;
static constexpr int EDGE_W = 28, BACK_THRESH = 110;

// ---- pull-down brightness: a touch that starts in the top band and moves down opens a card at the top;
// sliding left / right then changes the brightness relative to where it was (the slider's width is the whole
// range), and lifting saves it ----
bool pullPending = false, pullMode = false; int pullStart = 0, pullX0 = 0; uint32_t pullSeen = 0;
static constexpr int PULL_ZONE = 36, PULL_THRESH = 12, SLIDER_X = 56, SLIDER_W = 172, PULL_DROP_MS = 2000;   // start in the top 36 px
void drawBrightCard() {
  card(MARGIN, 12, W - 2 * MARGIN, 48, C.CARD, 12);
  icon(36, 36, IC_SUN, IC16, C.ORANGE);
  int sx = SLIDER_X, sw = SLIDER_W; frame.fillSmoothRoundRect(sx, 32, sw, 8, 4, C.LINE);
  int fill = (cfg.bright - BRIGHT_MIN) * sw / (BRIGHT_MAX - BRIGHT_MIN);
  frame.fillSmoothRoundRect(sx, 32, fill, 8, 4, C.ORANGE); frame.fillSmoothCircle(sx + constrain(fill, 8, sw - 8), 36, 8, C.WHITE);
  textf(W - MARGIN - 12, 36, C.TEXT, F12M, textdatum_t::middle_right, "%d%%", (cfg.bright - BRIGHT_MIN) * 100 / (BRIGHT_MAX - BRIGHT_MIN));
  flushRows(8, 56);
}
void pullUpdate(int x) {
  cfg.bright = constrain(pullStart + (x - pullX0) * (BRIGHT_MAX - BRIGHT_MIN) / SLIDER_W, BRIGHT_MIN, BRIGHT_MAX);
  lcd.setBrightness(cfg.bright); drawBrightCard();
}

// Display sleep: after cfg.sleepMin minutes without a touch the backlight goes off; the next touch only wakes it.
uint32_t lastActivity = 0; bool asleep = false, wakeTouch = false;
void displaySleep() { if (asleep) return; asleep = true; lcd.setBrightness(0); Serial.println("display: sleep"); }
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
static constexpr gpio_num_t WAKE_PIN = GPIO_NUM_17, BL_PIN = GPIO_NUM_45, TP_RST_PIN = GPIO_NUM_18;
void deepSleep(int secs = 0) {
  Serial.printf("power: deep sleep%s\n", secs ? " (timed)" : ""); Serial.flush();
  bookSave();
  lcd.setBrightness(0); lcd.sleep(); portalNetDown(); WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  gpio_hold_en(BL_PIN); gpio_hold_en(TP_RST_PIN); gpio_deep_sleep_hold_en();   // backlight stays off, the touch chip stays out of reset (released in setup)
  rtc_gpio_pullup_en(WAKE_PIN); rtc_gpio_pulldown_dis(WAKE_PIN);
  esp_sleep_enable_ext0_wakeup(WAKE_PIN, 0);
  if (secs) esp_sleep_enable_timer_wakeup(secs * 1000000ULL);
  esp_deep_sleep_start();
}
void lightSleep(int secs = 0) {
  Serial.printf("power: light sleep%s\n", secs ? " (timed)" : "");
  xSemaphoreTake(netMutex, portMAX_DELAY);   // let a request in flight finish; the Spotify and weather workers park on this
  lcd.setBrightness(0); lcd.sleep(); asleep = true;
  portalNetDown(); WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
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
  WiFi.mode(WIFI_STA); WiFi.setSleep(true); netState = NS_IDLE; netNextTry = 0; netConnect();
  xSemaphoreGive(netMutex);
  lcd.wakeup(); delay(120);                   // the panel needs 120 ms after sleep-out
  Screen *s = current(); s->render(); flushAll(); s->dirty = false;
  lcd.setBrightness(cfg.bright); asleep = false; lastActivity = millis(); wakeTouch = true;   // the touch that woke it does nothing else
  lastSleepS = (millis() - t0) / 1000; Serial.printf("power: awake after %d s (cause %d)\n", lastSleepS, lastSleepCause);
}
void sleepLoop() {
  if (touching) return;
  uint32_t idle = millis() - lastActivity;
  if (cfg.sleepMin && !asleep && idle > (uint32_t)cfg.sleepMin * 60000UL) displaySleep();
  if (bat.host) return;                       // plugged into a computer: stay reachable for serial and flashing
  if (cfg.deepMin && idle > (uint32_t)cfg.deepMin * 60000UL) deepSleep();
  else if (cfg.lightMin && idle > (uint32_t)cfg.lightMin * 60000UL) lightSleep();
}

void handleTouch() {
  int x, y; bool t = readTouch(x, y);
  Screen *s = current();
  if (t && !touching && asleep) { wakeTouch = true; displayWake(); }   // a touch on a dark screen only wakes it
  if (wakeTouch) { if (!t) wakeTouch = false; return; }
  if (t) lastActivity = millis();
  if (pullMode) { if (t) pullSeen = millis(); else if (millis() - pullSeen < PULL_DROP_MS) return; }   // the panel loses the finger briefly near its top edge: keep the card up
  if (t && !touching) {
    touching = true; tx0 = txl = x; ty0 = tyl = y; tStart = millis(); dragging = false; edgeMode = false; pullMode = false;
    pullPending = y < PULL_ZONE;                                   // maybe a brightness pull: decide once the finger moves
    edgePending = !pullPending && (x < EDGE_W && navDepth > 1);   // maybe a back-swipe: decide once the finger moves
    if (!edgePending && !pullPending) s->onDown(x, y);
  } else if (t && touching) {
    int dx = x - txl, dy = y - tyl;
    if (pullPending) {
      if (y - ty0 > PULL_THRESH) { pullPending = false; pullMode = true; pullStart = cfg.bright; pullX0 = x; pullSeen = millis(); pullUpdate(x); }
      else if (abs(x - tx0) > 16 && abs(x - tx0) > 3 * (y - ty0)) { pullPending = false; s->onDown(tx0, ty0); dragging = true; }   // clearly sideways: not a pull
    }
    if (pullMode) { if (x != txl) pullUpdate(x); txl = x; tyl = y; return; }
    if (edgePending) {
      if (x - tx0 > 8) { edgePending = false; edgeMode = true; navDepth--; navStack[navDepth]->render(); navDepth++; } // underlying screen into frame
      else if (abs(y - ty0) > 12 || x - tx0 < -8) { edgePending = false; s->onDown(tx0, ty0); dragging = true; }
    }
    if (edgeMode) { if (x != txl) { lcd.setClipRect(0, 0, x, H); lcd.pushImage(0, 0, W, H, fbuf()); lcd.clearClipRect(); } }
    else if (!edgePending && !pullPending && (dx || dy)) { if (!dragging && abs(x - tx0) + abs(y - ty0) > 10) dragging = true; if (dragging) s->onMove(x, y, dx, dy); }
    txl = x; tyl = y;
  } else if (!t && touching) {
    touching = false;
    if (pullMode) { pullMode = false; cfgSave(); s->render(); flushAll(); s->dirty = false; return; }   // card away, level kept
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
      else if (c == 'W') { wx.ok = true; wx.city = "Kalamassery"; wx.temp = 29; wx.code = 2; wx.hum = 74; wx.wind = 12; wx.nDays = 5; for (int i = 0; i < 5; i++) wx.days[i] = {i % 3 == 0 ? 61 : 2, 31.f, 24.f, (1 + i) % 7}; wx.fetchedAt = millis(); current()->dirty = true; }  // fake weather for layout tests
      else if (c == 'T') { applyTheme(!themeDark); current()->dirty = true; }   // preview the other theme (not saved)
      else if (c == 'p') { time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm); char tb[32]; strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", &tm);
        Serial.printf("time %s tz %ld bright %d wifi %d ssid %s wx ok %d city %s err %s home page %d storage %s ip %s gw %s mask %s bssid %s ch %d\n", tb, cfg.tzOff, cfg.bright, WiFi.status(), WiFi.SSID().c_str(), wx.ok, wx.city.c_str(), wx.err.c_str(), homeScreen.page, storageText().c_str(), WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.subnetMask().toString().c_str(), WiFi.BSSIDstr().c_str(), WiFi.channel());
        Serial.printf("sleep %d min asleep %d idle %lu s light %d min deep %d min host %d last light sleep %d s cause %d reset reason %d\n", cfg.sleepMin, asleep, (unsigned long)((millis() - lastActivity) / 1000), cfg.lightMin, cfg.deepMin, bat.host, lastSleepS, lastSleepCause, (int)esp_reset_reason());
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
  pinMode(PIN_AMP_EN, OUTPUT); digitalWrite(PIN_AMP_EN, HIGH);
  Serial.begin(115200);
  Serial.println("\n=== Perch ===");
  set_arduino_panic_handler(panicHook, nullptr);
  esp_sleep_wakeup_cause_t wokeBy = esp_sleep_get_wakeup_cause();   // EXT0 = a touch ended deep sleep
  gpio_hold_dis(BL_PIN); gpio_hold_dis(TP_RST_PIN); gpio_deep_sleep_hold_dis(); rtc_gpio_deinit(WAKE_PIN);
  Serial.printf("boot: reset reason %d\n", (int)esp_reset_reason());   // 1 power-on, 3 software, 4 panic, 6 task watchdog, 8 deep sleep
  if (wokeBy != ESP_SLEEP_WAKEUP_UNDEFINED) Serial.printf("power: woke from deep sleep (cause %d)\n", (int)wokeBy);
  cfgLoad(); spLoad();
  lcd.init(); applyAccent(cfg.accent); applyTheme(cfg.dark); applyRotation(cfg.flip); lcd.setBrightness(cfg.bright); lcd.fillScreen(C.BG);
  frame.setPsram(true); frame.setColorDepth(16);   // PSRAM: keeps ~150 KB of internal RAM free for TLS
  if (!frame.createSprite(W, H)) Serial.println("frame alloc FAILED");
  fontsBegin();
  face::micOk = face::micInit();
  randomSeed(esp_random());
  applyTz();
  netBegin();
  spBegin(); storageBegin(); bookBegin(); portalBegin();
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
  handleTouch(); sleepLoop(); usbReattachLoop();
  Screen *s = current();
  if (!edgeMode && !pullMode) {   // a slide or the brightness card owns the panel
    s->tick();
    if (s->dirty) { s->render(); flushAll(); s->dirty = false; }
  }
  frames++;
  if (millis() - lastLog > 10000) { Serial.printf("fps %.1f, screen %s, heap %lu\n", frames / 10.0f, s->name(), (unsigned long)ESP.getFreeHeap()); frames = 0; lastLog = millis(); }
  if (s != &face::faceScreen) delay(5); // idle screens don't need to spin
}
