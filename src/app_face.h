#pragma once
// Robo Face app: the animated companion face, drawing into the shared frame.
#include <Wire.h>
#include <driver/i2s.h>
#include "ui.h"

namespace face {
static constexpr int FACE_Y = 30, FACE_H = 160, CAP_Y = 196, CAP_H = 30;
static constexpr uint8_t ES8311_ADDR = 0x18; static constexpr int SAMPLE_RATE = 16000;
static const uint16_t BG = lcd.color565(8, 12, 22), EYE_BLUE = lcd.color565(40, 150, 255), EYE_WHITE = lcd.color565(235, 240, 245);
static const uint16_t MINT = lcd.color565(190, 255, 220), TEXT_BLUE = lcd.color565(150, 200, 255);
float dtk = 1.0f;

enum Mood { IDLE, HAPPY, WINK_L, WINK_R, SURPRISED, ANGRY, STARTLED };
Mood mood = IDLE; uint32_t moodUntil = 0; const char *caption = ""; uint32_t captionStart = 0;
struct Anim { float v, t; void to(float x) { t = x; } void step(float k) { k *= dtk; if (k > 1) k = 1; v += (t - v) * k; } };
Anim openL{1, 1}, openR{1, 1}, gazeX{0, 0}, gazeY{0, 0}, mouthCurve{1, 1}, mouthO{0, 0}, eyeScale{1, 1};
uint32_t nextBlink = 0; int blinkPhase = 0; uint32_t blinkT = 0;
bool touching = false; float dragDist = 0; uint32_t pokeTimes[8]; int pokeIdx = 0;
bool micOk = false; float micAvg = 200;

bool esWrite(uint8_t reg, uint8_t val) { Wire.beginTransmission(ES8311_ADDR); Wire.write(reg); Wire.write(val); return Wire.endTransmission() == 0; }
bool micInit() {
  Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);
  Wire.beginTransmission(ES8311_ADDR); if (Wire.endTransmission() != 0) return false;
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
  cfg.sample_rate = SAMPLE_RATE; cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT; cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1; cfg.dma_buf_count = 4; cfg.dma_buf_len = 256;
  cfg.tx_desc_auto_clear = true; cfg.fixed_mclk = SAMPLE_RATE * 256; cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) return false;
  i2s_pin_config_t pins = {}; pins.mck_io_num = PIN_I2S_MCLK; pins.bck_io_num = PIN_I2S_BCLK; pins.ws_io_num = PIN_I2S_WS;
  pins.data_out_num = PIN_I2S_DOUT; pins.data_in_num = PIN_I2S_DIN;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) return false;
  bool ok = true;
  ok &= esWrite(0x00, 0x1F); delay(20); ok &= esWrite(0x00, 0x00); ok &= esWrite(0x00, 0x80);
  ok &= esWrite(0x01, 0x3F); ok &= esWrite(0x06, 0x03); ok &= esWrite(0x02, 0x00); ok &= esWrite(0x03, 0x10);
  ok &= esWrite(0x04, 0x10); ok &= esWrite(0x05, 0x00); ok &= esWrite(0x07, 0x00); ok &= esWrite(0x08, 0xFF);
  ok &= esWrite(0x00, 0x80); ok &= esWrite(0x09, 0x0C); ok &= esWrite(0x0A, 0x0C);
  ok &= esWrite(0x0D, 0x01); ok &= esWrite(0x0E, 0x02); ok &= esWrite(0x12, 0x00); ok &= esWrite(0x13, 0x10);
  ok &= esWrite(0x1C, 0x6A); ok &= esWrite(0x37, 0x08); ok &= esWrite(0x17, 0xC8); ok &= esWrite(0x14, 0x1A);
  ok &= esWrite(0x16, 0x05); ok &= esWrite(0x32, 0x00);
  return ok;
}
int micPeak() {
  static int16_t buf[256]; size_t r = 0; int peak = 0;
  i2s_read(I2S_NUM_0, buf, sizeof buf, &r, 0);
  for (size_t i = 0; i < r / 2; i++) { int v = abs(buf[i]); if (v > peak) peak = v; }
  return peak;
}

void setMood(Mood m, const char *t, uint32_t ms) { mood = m; moodUntil = millis() + ms; caption = t; captionStart = millis(); }
void led(Mood m) {
  switch (m) {
    case ANGRY: neopixelWrite(PIN_LED, 60, 0, 0); break;
    case HAPPY: neopixelWrite(PIN_LED, 0, 40, 20); break;
    case SURPRISED: case STARTLED: neopixelWrite(PIN_LED, 40, 30, 0); break;
    case WINK_L: case WINK_R: neopixelWrite(PIN_LED, 20, 0, 40); break;
    default: neopixelWrite(PIN_LED, 0, 10, 40); break;
  }
}

void drawEyeNormal(float cx, float cy, float w, float h, float open, uint16_t col) {
  float hh = max(4.0f, h * open); float r = min(w, hh) * 0.35f;
  frame.fillSmoothRoundRect((int)(cx - w / 2), (int)(cy - hh / 2), (int)w, (int)hh, (int)r, col);
}
void drawEyeHappy(float cx, float cy, float w, uint16_t col) { int r = (int)(w / 2); frame.fillArc((int)cx, (int)(cy + r * 0.35f), r, r - 7, 190, 350, col); }
void drawEyeAngry(float cx, float cy, float w, float h, bool left, uint16_t col) {
  int x0 = (int)(cx - w / 2), x1 = (int)(cx + w / 2), yb = (int)(cy + h / 2);
  int yOuter = (int)(cy - h / 2 + 6), yInner = (int)(cy - h / 2 + h * 0.55f);
  int ytl = left ? yOuter : yInner, ytr = left ? yInner : yOuter;
  frame.fillTriangle(x0, ytl, x1, ytr, x1, yb, col); frame.fillTriangle(x0, ytl, x1, yb, x0, yb, col);
}
void drawMouth(float cx, float cy, uint16_t col) {
  if (mouthO.v > 0.5f) { int r = (int)(6 + 6 * mouthO.v); frame.fillSmoothCircle((int)cx, (int)cy + 4, r, col); frame.fillSmoothCircle((int)cx, (int)cy + 4, r - 4, BG); return; }
  float c = mouthCurve.v; int r = 16;
  if (c > 0.15f) frame.fillArc((int)cx, (int)(cy - r * 0.5f), r, r - 4, 30, 150, col);
  else if (c < -0.15f) frame.fillArc((int)cx, (int)(cy + r * 0.8f), r, r - 4, 210, 330, col);
  else frame.fillSmoothRoundRect((int)cx - 10, (int)cy, 20, 4, 2, col);
}
void drawCaption() {
  frame.fillRect(0, CAP_Y, W, CAP_H, BG);
  if (caption[0]) {
    uint32_t shown = (millis() - captionStart) / 35 + 1; size_t len = strlen(caption); if (shown > len) shown = len;
    char buf[64]; strncpy(buf, caption, shown); buf[shown] = 0;
    text(W / 2, CAP_Y + CAP_H - 8, buf, mood == ANGRY ? EYE_WHITE : TEXT_BLUE, F12, textdatum_t::bottom_center);
  }
  flushRows(CAP_Y, CAP_H);
}
void drawFace() {
  frame.fillRect(0, FACE_Y, W, FACE_H, BG);
  const float eyeW = 46 * eyeScale.v, eyeH = 58 * eyeScale.v, gap = 92;
  const float cx = W / 2 + gazeX.v, cy = (H / 2 - 28) + gazeY.v;
  uint16_t col = (mood == ANGRY) ? EYE_WHITE : (mood == HAPPY) ? MINT : EYE_BLUE;
  for (int side = -1; side <= 1; side += 2) {
    float ex = cx + side * gap / 2; bool left = side < 0; float open = left ? openL.v : openR.v;
    if (mood == HAPPY) drawEyeHappy(ex, cy, eyeW, col);
    else if (mood == ANGRY) drawEyeAngry(ex, cy, eyeW, eyeH * open, left, col);
    else drawEyeNormal(ex, cy, eyeW, eyeH, open, col);
  }
  drawMouth(cx, cy + eyeH / 2 + 22, col);
  flushRows(FACE_Y, FACE_H);
  static const char *lastCap = nullptr; static size_t lastLen = 0;
  size_t shown = caption[0] ? min(strlen(caption), (size_t)((millis() - captionStart) / 35 + 1)) : 0;
  if (caption != lastCap || shown != lastLen) { drawCaption(); lastCap = caption; lastLen = shown; }
}

bool inEye(int x, int y, bool left) { float ex = W / 2 + (left ? -46 : 46), ey = H / 2 - 28; return fabsf(x - ex) < 40 && fabsf(y - ey) < 48; }
int recentPokes() { uint32_t now = millis(); int n = 0; for (int i = 0; i < 8; i++) if (pokeTimes[i] && now - pokeTimes[i] < 4000) n++; return n; }
const char *pick(const char *const *arr, int n) { return arr[random(n)]; }
const char *HELLO[] = {"Oh! Hello there.", "Hi! Nice to see you.", "Hey, that's my face.", "Boop received."};
const char *TICKLE[] = {"That tickles!", "Hehe, stop it!", "Tickle tickle!"};
const char *WINK[] = {"A wink just for you.", ";) you saw that.", "Wink wink."};
const char *ANGRYL[] = {"Hey! Easy on the pokes.", "Too many pokes. Grr.", "Personal space, please."};
const char *STARTLE[] = {"What was that?!", "Whoa! Loud!", "Did you hear that?"};

void onTapFace(int x, int y) {
  if (mood == ANGRY) return;
  pokeTimes[pokeIdx++ & 7] = millis();
  if (recentPokes() >= 5) { setMood(ANGRY, pick(ANGRYL, 3), 3500); return; }
  if (inEye(x, y, true)) setMood(WINK_L, pick(WINK, 3), 1800);
  else if (inEye(x, y, false)) setMood(WINK_R, pick(WINK, 3), 1800);
  else setMood(SURPRISED, pick(HELLO, 4), 2200);
}
void onTickle() { if (mood != ANGRY && mood != HAPPY) setMood(HAPPY, pick(TICKLE, 3), 2500); }

void updateAnim() {
  uint32_t now = millis();
  if (mood != IDLE && now > moodUntil) { mood = IDLE; caption = ""; }
  float oL = 1, oR = 1, curve = 1, o = 0, scale = 1;
  switch (mood) {
    case WINK_L: oL = 0.08f; break; case WINK_R: oR = 0.08f; break;
    case SURPRISED: o = 1; scale = 1.08f; curve = 0; break;
    case STARTLED: o = 1; scale = 1.18f; curve = 0; break;
    case ANGRY: curve = -1; break; case HAPPY: curve = 1; break; default: break;
  }
  if (mood == IDLE || mood == SURPRISED || mood == HAPPY) {
    if (blinkPhase == 0 && now > nextBlink) { blinkPhase = 1; blinkT = now; }
    if (blinkPhase == 1) { oL = oR = 0.05f; if (now - blinkT > 90) { blinkPhase = 2; blinkT = now; } }
    if (blinkPhase == 2) { if (now - blinkT > 110) { blinkPhase = 0; nextBlink = now + random(2500, 6000); } }
  }
  if (!touching) { float t = now / 1000.0f; gazeX.to(sinf(t * 0.7f) * 6); gazeY.to(cosf(t * 0.5f) * 3); }
  openL.to(oL); openR.to(oR); mouthCurve.to(curve); mouthO.to(o); eyeScale.to(scale);
  openL.step(0.35f); openR.step(0.35f); mouthCurve.step(0.2f); mouthO.step(0.3f); eyeScale.step(0.2f); gazeX.step(0.15f); gazeY.step(0.15f);
}

struct FaceScreen : Screen {
  uint32_t lastFrame = 0;
  const char *name() override { return "Robo"; }
  bool titleBack() override { return false; }
  void render() override { frame.fillScreen(BG); }
  void enter() override { nextBlink = millis() + 800; setMood(SURPRISED, pick(HELLO, 4), 2200); lastFrame = millis(); touching = false; }
  void leave() override { neopixelWrite(PIN_LED, 0, 0, 0); }
  void tick() override {
    if (micOk) { int p = micPeak(); if (p > micAvg * 6 + 1500 && mood == IDLE) setMood(STARTLED, pick(STARTLE, 3), 2000); micAvg += (p - micAvg) * 0.02f; }
    updateAnim(); drawFace(); led(mood);
    uint32_t now = millis(); dtk = (now - lastFrame) / 33.0f; lastFrame = now;
  }
  void onDown(int x, int y) override { touching = true; dragDist = 0; }
  void onMove(int x, int y, int dx, int dy) override {
    dragDist += abs(dx) + abs(dy); if (dragDist > 60) onTickle();
    gazeX.to(constrain((x - W / 2) * 0.12f, -14.f, 14.f)); gazeY.to(constrain((y - (H / 2 - 28)) * 0.1f, -8.f, 8.f));
  }
  void onTap(int x, int y) override { onTapFace(x, y); }
  void onUp(int x, int y) override { touching = false; }
} faceScreen;
} // namespace face
