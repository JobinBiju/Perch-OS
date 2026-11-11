#pragma once
// Robo Face app: the animated companion face, drawing into the shared frame.
#include <Wire.h>
#include <driver/i2s.h>
#include "ui.h"
#include "spotify.h"

extern uint32_t lastActivity; extern bool asleep; void displayWake();   // main.cpp: last touch, display asleep
namespace face {
static constexpr int FACE_Y = 30, FACE_H = 160, CAP_Y = 196, CAP_H = 30;
static constexpr uint8_t ES8311_ADDR = 0x18; static constexpr int SAMPLE_RATE = 16000;
static const uint16_t BG = lcd.color565(8, 12, 22), EYE_BLUE = lcd.color565(40, 150, 255), EYE_WHITE = lcd.color565(235, 240, 245);
static const uint16_t MINT = lcd.color565(190, 255, 220), TEXT_BLUE = lcd.color565(150, 200, 255);
float dtk = 1.0f;

// One row per expression: eye openness (left, right), top lid drop at the inner and outer corner (0..1 of the eye),
// eye scale, mouth curve (-1 frown .. 1 smile), mouth "o", eye shape, colour, LED. updateAnim() eases towards the row.
enum Mood { IDLE, HAPPY, WINK_L, WINK_R, SURPRISED, ANGRY, STARTLED, SLEEPY, ASLEEP, GROGGY, SAD, LOVE, EXCITED, DANCE, COLD, HOT, RAIN, DIZZY, SUSPICIOUS, BORED, MOOD_N };
enum Shape { S_RECT, S_ARC, S_HEART, S_SHUT, S_SPIRAL };
struct Expr { float oL, oR, lidIn, lidOut, scale, curve, o; Shape shape; uint16_t col; uint8_t r, g, b; };
static const uint16_t PINK = lcd.color565(255, 96, 150), SAD_BLUE = lcd.color565(70, 130, 220), Z_DIM = lcd.color565(60, 90, 130), ICE = lcd.color565(170, 215, 255);
static const Expr EXPR[MOOD_N] = {
  /* IDLE      */ {1, 1, 0, 0, 1, 1, 0, S_RECT, EYE_BLUE, 0, 10, 40},
  /* HAPPY     */ {1, 1, 0, 0, 1, 1, 0, S_ARC, MINT, 0, 40, 20},
  /* WINK_L    */ {0.08f, 1, 0, 0, 1, 1, 0, S_RECT, EYE_BLUE, 20, 0, 40},
  /* WINK_R    */ {1, 0.08f, 0, 0, 1, 1, 0, S_RECT, EYE_BLUE, 20, 0, 40},
  /* SURPRISED */ {1, 1, 0, 0, 1.08f, 0, 1, S_RECT, EYE_BLUE, 40, 30, 0},
  /* ANGRY     */ {1, 1, 0.55f, 0.1f, 1, -1, 0, S_RECT, EYE_WHITE, 60, 0, 0},
  /* STARTLED  */ {1, 1, 0, 0, 1.18f, 0, 1, S_RECT, EYE_BLUE, 40, 30, 0},
  /* SLEEPY    */ {0.8f, 0.8f, 0.45f, 0.45f, 1, 0, 0, S_RECT, EYE_BLUE, 0, 4, 16},
  /* ASLEEP    */ {1, 1, 0, 0, 1, 0, 0, S_SHUT, EYE_BLUE, 0, 0, 4},
  /* GROGGY    */ {0.55f, 0.8f, 0.5f, 0.5f, 1, 0, 0, S_RECT, EYE_BLUE, 0, 4, 16},
  /* SAD       */ {0.9f, 0.9f, 0, 0.45f, 1, -1, 0, S_RECT, SAD_BLUE, 8, 0, 30},
  /* LOVE      */ {1, 1, 0, 0, 1, 1, 0, S_HEART, PINK, 50, 6, 20},
  /* EXCITED   */ {1, 1, 0, 0, 1.12f, 1, 0, S_RECT, EYE_BLUE, 0, 50, 30},
  /* DANCE     */ {1, 1, 0, 0, 1, 1, 0, S_RECT, MINT, 30, 0, 40},
  /* COLD      */ {0.9f, 0.9f, 0, 0, 0.94f, 0, 0, S_RECT, ICE, 10, 20, 40},
  /* HOT       */ {0.9f, 0.9f, 0.12f, 0.12f, 1, 0, 0.7f, S_RECT, EYE_BLUE, 40, 15, 0},
  /* RAIN      */ {1, 1, 0, 0, 1, 0, 0, S_RECT, EYE_BLUE, 0, 10, 40},
  /* DIZZY     */ {1, 1, 0, 0, 1, 0, 0.6f, S_SPIRAL, EYE_BLUE, 30, 20, 40},
  /* SUSPICIOUS*/ {0.42f, 0.9f, 0.3f, 0.3f, 1, 0, 0, S_RECT, EYE_BLUE, 30, 20, 0},
  /* BORED     */ {0.85f, 0.85f, 0.32f, 0.32f, 1, 0, 0, S_RECT, EYE_BLUE, 0, 6, 20},
};
// `react` is a timed reaction; when it runs out the face falls back to the base mood (idle, sleepy, asleep or sad). `mood` is what is drawn.
Mood mood = IDLE, react = IDLE, lastBase = IDLE; uint32_t reactUntil = 0, reactStart = 0; const char *caption = ""; uint32_t captionStart = 0, captionUntil = 0;
int force = -1;   // serial `m N`: hold expression N (-1 releases)
struct Anim { float v, t; void to(float x) { t = x; } void step(float k) { k *= dtk; if (k > 1) k = 1; v += (t - v) * k; } };
Anim openL{1, 1}, openR{1, 1}, gazeX{0, 0}, gazeY{0, 0}, mouthCurve{1, 1}, mouthO{0, 0}, eyeScale{1, 1}, lidIn{0, 0}, lidOut{0, 0};
uint32_t nextBlink = 0; int blinkPhase = 0; uint32_t blinkT = 0;
uint32_t nextLook = 0; float lookX = 0, lookY = 0; uint32_t nextYawn = 0, yawnUntil = 0, nextSigh = 0, lastNoise = 0;
bool touching = false, tickled = false; float dragDist = 0; int reversals = 0, lastDx = 0; uint32_t downAt = 0; uint32_t pokeTimes[8]; int pokeIdx = 0;
bool micOk = false; float micAvg = 200, beat = 1;   // beat: how hard the dance bounces, from the mic level
float noiseFloor = 200, voice = 0, heard = 0; bool wasLoud = false; uint32_t lastClap = 0, lastSus = 0, deafUntil = 0; int susDir = 1;   // voice: 0..1 level above the room's floor; heard: how steadily someone is talking
float segX = 0, segY = 0, lastAng = 0, turn = 0; bool hasAng = false;   // circular drags: the turning of the finger's path adds up in `turn`
char dyn[64]; String lastTitle; uint32_t nextRemark = 0; bool prevUsb = false, prevFull = false; int lastHour = -1;

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
  ok &= esWrite(0x04, 0x20); ok &= esWrite(0x05, 0x00); ok &= esWrite(0x07, 0x00); ok &= esWrite(0x08, 0xFF);
  ok &= esWrite(0x00, 0x80); ok &= esWrite(0x09, 0x0C); ok &= esWrite(0x0A, 0x0C);
  ok &= esWrite(0x0D, 0x01); ok &= esWrite(0x0E, 0x02); ok &= esWrite(0x12, 0x00); ok &= esWrite(0x13, 0x10);
  ok &= esWrite(0x1C, 0x6A); ok &= esWrite(0x37, 0x08); ok &= esWrite(0x17, 0xC8); ok &= esWrite(0x14, 0x1A);
  ok &= esWrite(0x16, 0x05); ok &= esWrite(0x31, 0x00); ok &= esWrite(0x32, 0xBF);   // 0x32: DAC volume, 0xBF = 0 dB
  return ok;
}
int micPeak() {
  static int16_t buf[256]; size_t r = 0; int peak = 0;
  i2s_read(I2S_NUM_0, buf, sizeof buf, &r, 0);
  for (size_t i = 0; i < r / 2; i++) { int v = abs(buf[i]); if (v > peak) peak = v; }
  return peak;
}

// ---- voice: short chirps through the speaker. A sound is a list of sine sweeps {from Hz, to Hz, ms}; 0 Hz is a pause, 0 ms ends it.
struct Seg { uint16_t f0, f1, ms; };
static const Seg S_HELLO[] = {{700, 1000, 80}, {0, 0, 30}, {1000, 1500, 110}, {0, 0, 0}};
static const Seg S_HAPPY[] = {{900, 1200, 60}, {0, 0, 30}, {1100, 1400, 60}, {0, 0, 30}, {1300, 1700, 80}, {0, 0, 0}};
static const Seg S_WINK[] = {{1500, 1800, 70}, {0, 0, 0}};
static const Seg S_ANGRY[] = {{420, 330, 160}, {0, 0, 40}, {380, 280, 220}, {0, 0, 0}};
static const Seg S_STARTLE[] = {{1400, 2300, 120}, {0, 0, 0}};
static const Seg S_YAWN[] = {{850, 400, 600}, {0, 0, 0}};
static const Seg S_GROGGY[] = {{480, 400, 180}, {0, 0, 60}, {440, 360, 220}, {0, 0, 0}};
static const Seg S_SAD[] = {{650, 480, 220}, {0, 0, 60}, {520, 380, 320}, {0, 0, 0}};
static const Seg S_LOVE[] = {{900, 1300, 120}, {1300, 1000, 100}, {1000, 1500, 160}, {0, 0, 0}};
static const Seg S_EXCITED[] = {{800, 800, 70}, {1000, 1000, 70}, {1200, 1200, 70}, {1600, 1600, 140}, {0, 0, 0}};
static const Seg S_COLD[] = {{1200, 1200, 40}, {0, 0, 40}, {1200, 1200, 40}, {0, 0, 40}, {1200, 1200, 40}, {0, 0, 0}};
static const Seg S_HOT[] = {{750, 450, 350}, {0, 0, 0}};
static const Seg S_RAIN[] = {{1000, 1200, 80}, {0, 0, 0}};
static const Seg S_DIZZY[] = {{600, 1200, 150}, {1200, 600, 150}, {600, 1200, 150}, {1200, 500, 200}, {0, 0, 0}};
static const Seg S_SUS[] = {{550, 850, 180}, {0, 0, 0}};
static const Seg S_BORED[] = {{560, 420, 450}, {0, 0, 0}};
static const Seg *const SND[MOOD_N] = {nullptr, S_HAPPY, S_WINK, S_WINK, S_HELLO, S_ANGRY, S_STARTLE, nullptr, nullptr, S_GROGGY, S_SAD, S_LOVE, S_EXCITED, nullptr, S_COLD, S_HOT, S_RAIN, S_DIZZY, S_SUS, S_BORED};
static constexpr int AMP_ON = LOW;   // the amplifier's enable pin is active low on this board
const Seg *snd = nullptr; int sndSeg = 0, sndAt = 0, pendN = 0; float sndPhase = 0; bool ampOn = false; uint32_t ampOffAt = 0; int16_t pend[256 * 2];
void play(const Seg *s) {
  if (!s || !micOk || !cfg.roboSound || asleep) return;
  snd = s; sndSeg = sndAt = 0; sndPhase = 0;
  if (!ampOn) { digitalWrite(PIN_AMP_EN, AMP_ON); ampOn = true; }
}
// Feed the I2S buffer without blocking: synthesise a block, write what fits, keep the rest for the next frame.
void soundLoop() {
  if (!snd && !pendN) { if (ampOn && millis() > ampOffAt) { digitalWrite(PIN_AMP_EN, !AMP_ON); ampOn = false; } return; }
  for (int round = 0; round < 4; round++) {
    if (!pendN && snd) {
      for (; pendN < 256; pendN++) {
        const Seg &g = snd[sndSeg]; int v = 0;
        if (!g.ms) { snd = nullptr; break; }
        int n = g.ms * (SAMPLE_RATE / 1000);
        if (g.f0) { float f = g.f0 + (float)(g.f1 - g.f0) * sndAt / n, env = min(1.0f, min(sndAt, n - sndAt) / 80.0f); sndPhase += 6.2832f * f / SAMPLE_RATE; if (sndPhase > 6.2832f) sndPhase -= 6.2832f; v = (int)(sinf(sndPhase) * 9000 * env); }
        pend[pendN * 2] = pend[pendN * 2 + 1] = (int16_t)v;
        if (++sndAt >= n) { sndAt = 0; sndSeg++; }
      }
    }
    if (!pendN) break;
    size_t w = 0; i2s_write(I2S_NUM_0, pend, pendN * 4, &w, 0); int done = w / 4;
    if (done < pendN) { memmove(pend, pend + done * 2, (pendN - done) * 4); pendN -= done; break; }
    pendN = 0;
  }
  ampOffAt = millis() + 200; deafUntil = millis() + 350;   // the mic hears the speaker: ignore it until the sound has died away
}
// Serial `m 98`: a 1 kHz tone with the amp pin low, then high, and what the mic hears each time.
void ampTest() {
  static int16_t b[256 * 2];
  for (int lvl = 0; lvl < 2; lvl++) {
    digitalWrite(PIN_AMP_EN, lvl); delay(60); int peak = 0; float ph = 0;
    for (int k = 0; k < 30; k++) {
      for (int i = 0; i < 256; i++) { ph += 6.2832f * 1000 / SAMPLE_RATE; b[i * 2] = b[i * 2 + 1] = (int16_t)(sinf(ph) * 9000); }
      size_t w; i2s_write(I2S_NUM_0, b, sizeof b, &w, portMAX_DELAY); if (k > 8) peak = max(peak, micPeak());
    }
    Serial.printf("amp pin %d: mic peak %d\n", lvl, peak); delay(250); micPeak(); micPeak();
  }
  digitalWrite(PIN_AMP_EN, !AMP_ON);
}

void say(const char *t, uint32_t ms) { caption = t; captionStart = millis(); captionUntil = captionStart + ms; }
void setMood(Mood m, const char *t, uint32_t ms) { react = m; reactStart = millis(); reactUntil = reactStart + ms; say(t, ms); play(SND[m]); }
int ledShown = -1;
void led(Mood m) { if (m == ledShown) return; ledShown = m; neopixelWrite(PIN_LED, EXPR[m].r, EXPR[m].g, EXPR[m].b); }

// A rounded eye; the top lid is a background wedge laid over it, so a slant reads as angry (inner corner low) or sad (outer corner low).
void drawEye(float cx, float cy, float w, float h, float open, bool left, uint16_t col) {
  float hh = max(4.0f, h * open); float r = min(w, hh) * 0.35f; int x0 = (int)(cx - w / 2), y0 = (int)(cy - hh / 2);
  frame.fillSmoothRoundRect(x0, y0, (int)w, (int)hh, (int)r, col);
  if (lidIn.v < 0.02f && lidOut.v < 0.02f) return;
  float lo = min(0.15f, max(lidIn.v, lidOut.v)), li = max(lidIn.v, lo), lu = max(lidOut.v, lo);   // the high end still clears the rounded corner
  int yl = y0 + (int)(hh * (left ? lu : li)), yr = y0 + (int)(hh * (left ? li : lu)), xa = x0 - 2, xb = x0 + (int)w + 2, yt = y0 - 2;
  frame.fillTriangle(xa, yt, xb, yt, xb, yr, BG); frame.fillTriangle(xa, yt, xb, yr, xa, yl, BG);
}
void drawEyeHappy(float cx, float cy, float w, uint16_t col) { int r = (int)(w / 2); frame.fillArc((int)cx, (int)(cy + r * 0.35f), r, r - 7, 190, 350, col); }
void drawEyeShut(float cx, float cy, float w, uint16_t col) { int r = (int)(w / 2); frame.fillArc((int)cx, (int)(cy - r * 0.2f), r, r - 5, 20, 160, col); }
void drawEyeHeart(float cx, float cy, float w, uint16_t col) {
  float r = w * 0.27f * (1 + 0.08f * sinf(millis() / 160.0f)); int y = (int)(cy - r * 0.55f);
  frame.fillSmoothCircle((int)(cx - r * 0.95f), y, (int)r, col); frame.fillSmoothCircle((int)(cx + r * 0.95f), y, (int)r, col);
  frame.fillRect((int)cx - 3, y - 2, 6, (int)r, col);
  frame.fillTriangle((int)(cx - r * 1.85f), (int)(y + r * 0.45f), (int)(cx + r * 1.85f), (int)(y + r * 0.45f), (int)cx, (int)(y + r * 2.5f), col);
}
void drawEyeSpiral(float cx, float cy, float w, bool left, uint16_t col) {
  float rot = millis() / 110.0f * (left ? 1 : -1);
  for (float a = 0.6f; a < 15.7f; a += 0.22f) { float r = a * w * 0.03f, t = left ? a + rot : -a + rot; frame.fillCircle((int)(cx + cosf(t) * r), (int)(cy + sinf(t) * r), 2, col); }
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
    text(W / 2, CAP_Y + CAP_H - 8, buf, mood == ANGRY ? EYE_WHITE : mood == LOVE ? PINK : TEXT_BLUE, F12, textdatum_t::bottom_center);
  }
  flushRows(CAP_Y, CAP_H);
}
void drawFace() {
  frame.fillRect(0, FACE_Y, W, FACE_H, BG);
  const Expr &e = EXPR[mood];
  const float eyeW = 46 * eyeScale.v, eyeH = 58 * eyeScale.v, gap = 92;
  const float cx = W / 2 + gazeX.v, cy = (H / 2 - 28) + gazeY.v;
  for (int side = -1; side <= 1; side += 2) {
    float ex = cx + side * gap / 2; bool left = side < 0;
    if (e.shape == S_ARC) drawEyeHappy(ex, cy, eyeW, e.col);
    else if (e.shape == S_HEART) drawEyeHeart(ex, cy, eyeW, e.col);
    else if (e.shape == S_SHUT) drawEyeShut(ex, cy, eyeW, e.col);
    else if (e.shape == S_SPIRAL) drawEyeSpiral(ex, cy, eyeW, left, e.col);
    else drawEye(ex, cy, eyeW, eyeH, left ? openL.v : openR.v, left, e.col);
  }
  drawMouth(cx, cy + eyeH / 2 + 22, e.col);
  if (mood == ASLEEP) for (int i = 0; i < 3; i++) {   // three z's drifting up beside the face
    float p = fmodf(millis() / 2700.0f + i / 3.0f, 1.0f);
    text((int)(cx + 72 + p * 24 + sinf(p * 6.28f) * 3), (int)(cy - 22 - p * 30), "z", (p < 0.15f || p > 0.8f) ? Z_DIM : TEXT_BLUE, p > 0.5f ? F16 : F12, textdatum_t::middle_center);
  }
  uint32_t ms = millis();
  if (mood == EXCITED && openL.v > 0.7f) for (int side = -1; side <= 1; side += 2) {   // sparkles in the eyes
    float ex = cx + side * gap / 2; frame.fillSmoothCircle((int)(ex + eyeW * 0.18f), (int)(cy - eyeH * 0.2f), 5, EYE_WHITE); frame.fillSmoothCircle((int)(ex - eyeW * 0.14f), (int)(cy + eyeH * 0.14f), 2, EYE_WHITE);
  }
  if (mood == DANCE) for (int i = 0; i < 2; i++) {   // a note drifting up on each side
    float p = fmodf(ms / 2400.0f + i * 0.5f, 1.0f); int x = (int)(W / 2 + (i ? 112 : -120) + sinf(p * 6.28f) * 4), y = (int)(FACE_Y + 96 - p * 60); uint16_t c = (p < 0.12f || p > 0.85f) ? Z_DIM : MINT;
    frame.fillSmoothCircle(x, y, 4, c); frame.fillRect(x + 2, y - 14, 2, 14, c); frame.fillRect(x + 2, y - 14, 8, 3, c);
  }
  if (mood == HOT) {   // a sweat drop sliding down beside the right eye
    float p = fmodf(ms / 1500.0f, 1.0f); int x = (int)(cx + gap / 2 + eyeW / 2 + 12), y = (int)(cy - 22 + p * 26);
    frame.fillSmoothCircle(x, y, 5, TEXT_BLUE); frame.fillTriangle(x - 5, y - 1, x + 5, y - 1, x, y - 12, TEXT_BLUE);
  }
  if (mood == RAIN) for (int i = 0; i < 10; i++) { int x = 24 + i * 30 + (i * 7) % 11, y = FACE_Y + 2 + (int)fmodf(ms * 0.07f + i * 13, 22); frame.drawLine(x, y, x - 3, y + 8, TEXT_BLUE); frame.drawLine(x + 1, y, x - 2, y + 8, TEXT_BLUE); }
  flushRows(FACE_Y, FACE_H);
  static const char *lastCap = nullptr; static size_t lastLen = 0; static Mood lastMood = IDLE;
  size_t shown = caption[0] ? min(strlen(caption), (size_t)((millis() - captionStart) / 35 + 1)) : 0;
  if (caption != lastCap || shown != lastLen || mood != lastMood) { drawCaption(); lastCap = caption; lastLen = shown; lastMood = mood; }
}

bool inEye(int x, int y, bool left) { float ex = W / 2 + (left ? -46 : 46), ey = H / 2 - 28; return fabsf(x - ex) < 40 && fabsf(y - ey) < 48; }
int recentPokes() { uint32_t now = millis(); int n = 0; for (int i = 0; i < 8; i++) if (pokeTimes[i] && now - pokeTimes[i] < 4000) n++; return n; }
const char *pick(const char *const *arr, int n) { return arr[random(n)]; }
const char *HELLO[] = {"Oh! Hello there.", "Hi! Nice to see you.", "Hey, that's my face.", "Boop received."};
const char *TICKLE[] = {"That tickles!", "Hehe, stop it!", "Tickle tickle!"};
const char *WINK[] = {"A wink just for you.", ";) you saw that.", "Wink wink."};
const char *ANGRYL[] = {"Hey! Easy on the pokes.", "Too many pokes. Grr.", "Personal space, please."};
const char *STARTLE[] = {"What was that?!", "Whoa! Loud!", "Did you hear that?"};

const char *WAKE[] = {"Mm... five more minutes.", "Huh? I was dreaming.", "Oh. I dozed off."};
const char *LOVEL[] = {"Aww, that's nice.", "I like you too.", "More pets, please."};
const char *LOWBAT[] = {"I'm running low...", "Feeling a bit drained.", "Could use a charge."};
const char *DIZZYL[] = {"Whoa... spinning...", "Stop the room, please.", "Round and round..."};
const char *SUS[] = {"Hm? What was that?", "Did something move?", "I heard that."};
const char *BOREDL[] = {"So... what now?", "*sigh*", "I spy... a wall."};
const char *PLUG[] = {"Yum, power!", "Ahh, that's the stuff.", "Charging. Thank you!"};
const char *FULL[] = {"All charged up!", "I'm full. 100%."};
const char *NOWIFI[] = {"I lost the Wi-Fi...", "Where did the internet go?"};

bool clockNow(struct tm &tm) { time_t t = time(nullptr); localtime_r(&t, &tm); return tm.tm_year > 120; }
bool dancing() { return sp.linked && sp.have && sp.playing; }
const char *greeting() {
  struct tm tm; if (!clockNow(tm) || random(3) == 0) return pick(HELLO, 4);
  int h = tm.tm_hour; return h >= 5 && h < 12 ? "Good morning!" : h < 17 ? "Good afternoon!" : h < 23 ? "Good evening." : "It's late. Still up?";
}
// Something to say about the weather; the notable kinds also get a face.
void weatherRemark() {
  if (!wx.ok) return;
  int c = wx.code, t = (int)lroundf(toUnit(wx.temp)); bool rain = (c >= 51 && c <= 67) || (c >= 80 && c <= 82) || c >= 95;
  if (rain) { snprintf(dyn, sizeof dyn, c >= 95 ? "Thunder out there. Yikes." : "Sounds like rain outside."); setMood(RAIN, dyn, 4500); }
  else if (wx.temp <= 12) { snprintf(dyn, sizeof dyn, "Brr. %d\xC2\xB0 outside.", t); setMood(COLD, dyn, 4500); }
  else if (wx.temp >= 32) { snprintf(dyn, sizeof dyn, "Phew. %d\xC2\xB0 out there.", t); setMood(HOT, dyn, 4500); }
  else { snprintf(dyn, sizeof dyn, "%s, %d\xC2\xB0 outside.", wxText(c), t); say(dyn, 4000); }
}
// Things that happen around Robo while it is open: power, the clock, the weather, music.
void events() {
  uint32_t now = millis(); bool awake = mood != ASLEEP && react != ANGRY;
  if (bat.usb != prevUsb) {
    prevUsb = bat.usb;
    if (bat.usb) setMood(EXCITED, pick(PLUG, 3), 3000);
    else if (bat.present) { snprintf(dyn, sizeof dyn, "Unplugged. %d%% left.", bat.pct); setMood(SURPRISED, dyn, 3000); }
  }
  if (bat.full != prevFull) { prevFull = bat.full; if (bat.full) setMood(HAPPY, pick(FULL, 2), 3000); }
  struct tm tm;
  if (clockNow(tm) && tm.tm_hour != lastHour) {
    bool first = lastHour < 0; lastHour = tm.tm_hour;
    if (!first && awake && tm.tm_min == 0) { snprintf(dyn, sizeof dyn, "It's %d o'clock.", (tm.tm_hour + 11) % 12 + 1); setMood(SURPRISED, dyn, 3000); }
  }
  if (dancing()) {
    if (sp.title != lastTitle && awake && react == IDLE) { lastTitle = sp.title; snprintf(dyn, sizeof dyn, "%.34s - %.20s", sp.title.c_str(), sp.artist.c_str()); say(dyn, 6000); }
  } else lastTitle = "";
  if (mood == IDLE && !caption[0] && now > nextRemark) {
    nextRemark = now + (wx.ok ? random(45000, 80000) : 10000);
    if (now - lastActivity > 20000 && random(100) < 35) setMood(BORED, pick(BOREDL, 3), 4000); else weatherRemark();
  }
}

void onTapFace(int x, int y) {
  if (mood == ANGRY || mood == GROGGY) return;
  pokeTimes[pokeIdx++ & 7] = millis();
  if (recentPokes() >= 5) { setMood(ANGRY, pick(ANGRYL, 3), 3500); return; }
  if (inEye(x, y, true)) setMood(WINK_L, pick(WINK, 3), 1800);
  else if (inEye(x, y, false)) setMood(WINK_R, pick(WINK, 3), 1800);
  else setMood(SURPRISED, pick(HELLO, 4), 2200);
}
void onTickle() { if (mood != ANGRY && mood != HAPPY && mood != GROGGY && react != DIZZY) setMood(HAPPY, pick(TICKLE, 3), 2500); }

// What the face does when nothing is happening to it. It dozes off ahead of the display-sleep timer, so the screen goes dark on a sleeping face.
int sadWhy() {   // 1 low battery, 2 Wi-Fi lost after having had it
  static bool sawWifi = false; bool up = WiFi.status() == WL_CONNECTED; sawWifi |= up;
  if (bat.present && !bat.usb && bat.pct <= 20) return 1;
  return sawWifi && !up ? 2 : 0;
}
Mood baseMood() {
  uint32_t now = millis(), idle = now - lastActivity; if (lastNoise && now - lastNoise < idle) idle = now - lastNoise;
  uint32_t nap = cfg.sleepMin ? (uint32_t)cfg.sleepMin * 60000UL : 240000UL;
  time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm);
  if (tm.tm_year > 120 && (tm.tm_hour >= 23 || tm.tm_hour < 6) && nap > 120000UL) nap = 120000UL;   // late at night it tires sooner
  if (idle > nap / 5 * 4) return ASLEEP;
  if (idle > nap / 2) return SLEEPY;
  return sadWhy() ? SAD : dancing() ? DANCE : IDLE;
}

void updateAnim() {
  uint32_t now = millis();
  if (react != IDLE && now > reactUntil) react = IDLE;
  if (caption[0] && now > captionUntil) caption = "";
  Mood base = baseMood();
  if (base != lastBase) {
    if (lastBase == ASLEEP && react == IDLE) setMood(GROGGY, pick(WAKE, 3), 2600);
    if (base == SLEEPY) nextYawn = now + 1500;
    if (base == SAD) nextSigh = now + 600;
    lastBase = base;
  }
  mood = force >= 0 && force < MOOD_N ? (Mood)force : react != IDLE ? react : base;
  const Expr &e = EXPR[mood];
  float oL = e.oL, oR = e.oR, o = e.o;
  if (mood == SLEEPY) {
    if (now > nextYawn) { yawnUntil = now + 1700; nextYawn = now + random(9000, 15000); say("*yawn*", 1700); play(S_YAWN); }
    if (now < yawnUntil) { o = 1.6f; oL = oR = 0.12f; }
  }
  if (mood == SAD && now > nextSigh) { nextSigh = now + random(18000, 26000); if (sadWhy() == 2) say(pick(NOWIFI, 2), 3500); else say(pick(LOWBAT, 3), 3500); }
  if (mood == IDLE && heard > 0.4f) o = 0.42f + voice * 1.3f;   // someone is talking: the mouth follows their voice
  if (caption[0] && caption[0] != '*' && o < 0.5f && mood != ASLEEP && now - captionStart < strlen(caption) * 35) o = ((now / 100) & 1) ? 0.95f : 0.2f;   // its own words: the mouth flaps while the caption types out
  bool slow = mood == SLEEPY || mood == GROGGY || mood == BORED;
  if (e.shape == S_RECT && mood != WINK_L && mood != WINK_R && mood != ANGRY && mood != STARTLED && now > yawnUntil) {
    if (blinkPhase == 0 && now > nextBlink) { blinkPhase = 1; blinkT = now; }
    if (blinkPhase == 1) { oL = oR = 0.05f; if (now - blinkT > (slow ? 220u : 90u)) { blinkPhase = 2; blinkT = now; } }
    if (blinkPhase == 2) { if (now - blinkT > (slow ? 240u : 110u)) { blinkPhase = 0; nextBlink = now + (!slow && random(100) < 20 ? 140 : random(2500, 6000)); } }   // one in five is a double blink
  } else blinkPhase = 0;
  if (!touching) {
    bool wander = mood == IDLE || mood == SAD || mood == SLEEPY;
    if (!wander) { lookX = lookY = 0; nextLook = now + 600; }
    else if (now > nextLook) {
      if (!slow && random(100) < 18) { lookX = random(2) ? 13 : -13; lookY = random(-2, 3); nextLook = now + random(600, 1300); }   // a glance to the side
      else { lookX = random(-6, 7) * (slow ? 0.5f : 1); lookY = random(-3, 4); nextLook = now + random(1200, 3800) * (slow ? 2 : 1); }
    }
    float breath = mood == ASLEEP ? sinf(now / 720.0f) * 3 : sinf(now / 480.0f) * 1.5f;
    float roll = (now - reactStart) / 1400.0f;
    if (mood == BORED && roll < 1) { gazeX.to(-cosf(roll * 3.1416f) * 12); gazeY.to(-sinf(roll * 3.1416f) * 10); }   // an eye roll, up and over
    else if (mood == BORED) { gazeX.to(11); gazeY.to(3); }
    else if (mood == SUSPICIOUS) { gazeX.to(susDir * 12); gazeY.to(0); }
    else if (mood == DIZZY) { gazeX.to(sinf(now / 300.0f) * 6); gazeY.to(cosf(now / 300.0f) * 3); }
    else if (mood == DANCE) { gazeX.to(sinf(now / 500.0f * 3.1416f) * 7); gazeY.to(2 - fabsf(sinf(now / 500.0f * 3.1416f)) * 7 * beat); }   // sway and bounce at about 120 a minute
    else gazeX.to(lookX + (mood == COLD ? ((now / 45) & 1 ? 2.5f : -2.5f) : 0)), gazeY.to(lookY + breath + (mood == SAD ? 5 : mood == RAIN ? -8 : 0));
  }
  openL.to(oL); openR.to(oR); mouthCurve.to(e.curve); mouthO.to(o); eyeScale.to(e.scale); lidIn.to(e.lidIn); lidOut.to(e.lidOut);
  openL.step(0.35f); openR.step(0.35f); mouthCurve.step(0.2f); mouthO.step(0.3f); eyeScale.step(0.2f); lidIn.step(0.2f); lidOut.step(0.2f);
  float gk = touching ? 0.15f : (mood == DANCE || mood == COLD) ? 0.5f : 0.3f; gazeX.step(gk); gazeY.step(gk);
}

struct FaceScreen : Screen {
  uint32_t lastFrame = 0;
  const char *name() override { return "Robo"; }
  bool titleBack() override { return false; }
  void render() override { frame.fillScreen(BG); }
  void enter() override { nextBlink = millis() + 800; lastBase = IDLE; lastNoise = 0; setMood(SURPRISED, greeting(), 2200);
    prevUsb = bat.usb; prevFull = bat.full; lastHour = -1; lastTitle = ""; nextRemark = millis() + 9000;
    sp.pollMs = 10000; sp.wantPolling = true; /* a slow poll is enough to know whether music is playing */ lastFrame = millis(); touching = false; }
  void leave() override { neopixelWrite(PIN_LED, 0, 0, 0); ledShown = -1; sp.wantPolling = false; snd = nullptr; pendN = 0; digitalWrite(PIN_AMP_EN, !AMP_ON); ampOn = false; }
  void tick() override {
    uint32_t now = millis();
    soundLoop();
    if (micOk) {
      int p = micPeak(); bool loud = p > micAvg * 6 + 1500, deaf = now < deafUntil;
      noiseFloor += (p < noiseFloor ? 0.2f : 0.003f) * (p - noiseFloor);
      beat += ((p > micAvg * 1.5f ? 1.4f : 0.7f) - beat) * 0.3f;
      if (deaf || dancing()) { voice = 0; heard *= 0.85f; }
      else {
        if (loud && !wasLoud) {
          if (lastClap && now - lastClap > 150 && now - lastClap < 700) { lastClap = 0; lastNoise = now; displayWake(); setMood(HAPPY, "You called?", 2500); }   // two claps: wakes the screen too
          else { lastClap = now; if (react == IDLE) { setMood(STARTLED, pick(STARTLE, 3), 2000); lastNoise = now; } }
        } else if (!loud && mood == IDLE && p > micAvg * 3 + 600 && now - lastSus > 20000) { lastSus = now; susDir = random(2) ? 1 : -1; setMood(SUSPICIOUS, pick(SUS, 3), 2500); }
        voice = constrain((p - noiseFloor * 2 - 400) / 4000.0f, 0.0f, 1.0f); heard = heard * 0.85f + (voice > 0.1f ? 0.15f : 0);
        if (heard > 0.4f) lastNoise = now;
      }
      wasLoud = loud; micAvg += (p - micAvg) * 0.02f;
    }
    if (touching && !tickled && react != ANGRY && react != GROGGY && now - downAt > 700) {   // a long press or a slow stroke: petting
      if (react != LOVE) setMood(LOVE, pick(LOVEL, 3), 1500); else reactUntil = captionUntil = now + 1500;
    }
    if (asleep) { led(ASLEEP); delay(30); return; }   // the panel is off: nothing to draw
    events(); updateAnim(); drawFace(); led(mood);
    now = millis(); dtk = (now - lastFrame) / 33.0f; lastFrame = now;
  }
  void onDown(int x, int y) override {
    touching = true; tickled = false; dragDist = 0; reversals = 0; lastDx = 0; downAt = millis(); segX = segY = turn = 0; hasAng = false;
    if (mood == ASLEEP) setMood(GROGGY, pick(WAKE, 3), 2600);
  }
  void onMove(int x, int y, int dx, int dy) override {
    dragDist += abs(dx) + abs(dy);
    segX += dx; segY += dy;
    if (fabsf(segX) + fabsf(segY) >= 10) {
      float a = atan2f(segY, segX), d = a - lastAng; if (d > 3.1416f) d -= 6.2832f; if (d < -3.1416f) d += 6.2832f;
      if (hasAng && fabsf(d) < 2.2f) turn += d;   /* a sharper turn is a reversal, not a circle */
      lastAng = a; hasAng = true; segX = segY = 0;
    }
    if (fabsf(turn) > 12.5f && react != ANGRY && react != GROGGY && react != DIZZY) { turn = 0; tickled = true; setMood(DIZZY, pick(DIZZYL, 3), 3500); }   /* two full circles */
    if (abs(dx) >= 3) { if (lastDx && (dx > 0) != (lastDx > 0)) reversals++; lastDx = dx; }
    if (dragDist > 60 && react != LOVE && (reversals >= 2 || dragDist > 0.25f * (millis() - downAt))) { tickled = true; onTickle(); }   // quick or back-and-forth: a tickle
    gazeX.to(constrain((x - W / 2) * 0.12f, -14.f, 14.f)); gazeY.to(constrain((y - (H / 2 - 28)) * 0.1f, -8.f, 8.f));
  }
  void onTap(int x, int y) override { onTapFace(x, y); }
  void onUp(int x, int y) override { touching = false; }
} faceScreen;
// Serial `m 99`: draw three circles on the face with a pretend finger.
void spinTest() { faceScreen.onDown(200, 110); float px = 200, py = 110; for (int i = 1; i <= 90; i++) { float a = i * 0.21f, x = 160 + cosf(a) * 40, y = 110 + sinf(a) * 40; faceScreen.onMove((int)x, (int)y, (int)(x - px), (int)(y - py)); px = (int)x; py = (int)y; } faceScreen.onUp(0, 0); }
} // namespace face
