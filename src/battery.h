#pragma once
// Battery monitor: BAT_ADC on GPIO 9 sits on a 200k/200k divider, so cell voltage = 2 x pin voltage.
// The TP4054 charger has no status line wired to the ESP32, so everything about charging is inferred:
//  - plugged in: a computer is sending USB frames, or the voltage jumped up between two samples (the charger lifts the
//    cell by 100-200 mV the moment it starts); unplugged: the frames stop, or the voltage drops back by a step.
//  - level while charging: the lift measured at plug-in is subtracted before the open-circuit curve is applied, so the
//    figure does not leap when the cable goes in. Once the charger holds the cell at its plateau the voltage says
//    nothing more, and the level is walked from there to 100% ("Full") over the time a top-off takes.
#include <Arduino.h>
#include "soc/usb_serial_jtag_reg.h"

static constexpr int BAT_PIN = 9;
static constexpr int BAT_STEP = 60, BAT_LIFT_GUESS = 150, BAT_CV = 4130, BAT_TOPOFF_MIN = 40;   // mV, mV, mV (this board reads the charger's 4.2 V plateau as about 4.15), minutes
struct Battery {
  int mv = 0, raw = 0, pct = 0; uint32_t lastSample = 0;
  bool present = false, host = false, usb = false, charging = false, full = false;   // host: a computer; usb: any USB power
  bool byHost = false; int lift = 0, floorPct = 0, cvBase = 0; uint32_t cvSince = 0;             // lift: mV the charger adds; floorPct: level never falls while plugged
  int hist[6] = {0}, nh = 0, trend = 0; uint32_t lastHist = 0;   // one reading a minute: trend = mV gained over the last (nh - 1) minutes
  int mv0 = 0, pct0 = 0, samples = 0; uint32_t t0 = 0;           // first settled reading after power-on, for "since power-on"
} bat;

// LiPo open-circuit curve, 3.3 V empty to 4.2 V full
int batCurve(int mv) {
  static const int pts[][2] = {{3300, 0}, {3500, 5}, {3600, 10}, {3700, 30}, {3800, 55}, {3900, 72}, {4000, 85}, {4100, 95}, {4200, 100}};
  if (mv <= pts[0][0]) return 0;
  for (int i = 1; i < 9; i++) if (mv <= pts[i][0]) return pts[i - 1][1] + (pts[i][1] - pts[i - 1][1]) * (mv - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
  return 100;
}
void batSample() {
  static bool init = false;
  if (!init) { analogReadResolution(12); analogSetPinAttenuation(BAT_PIN, ADC_11db); init = true; }
  uint32_t sum = 0; for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BAT_PIN);
  int raw = (int)(sum / 16) * 2; uint32_t now = millis();
  bool first = !bat.samples, gap = !first && now - bat.lastSample > 15000;   // gap: the board slept, the last reading is stale
  int step = first || gap ? 0 : raw - bat.raw;
  // a computer sends a start-of-frame every millisecond; the USB-Serial-JTAG frame counter stands still without one
  uint32_t f0 = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & 0x7FF; delay(3);
  bat.host = (REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & 0x7FF) != f0;

  bool was = bat.usb;
  if (!bat.usb) {
    if (bat.host) { bat.usb = true; bat.byHost = true; }
    else if (step >= BAT_STEP || (first && raw >= 4150) || bat.trend >= 40) { bat.usb = true; bat.byHost = false; }   // wall adapter
    if (bat.usb) { bat.lift = step >= BAT_STEP && bat.mv ? constrain(raw - bat.mv, 40, 300) : BAT_LIFT_GUESS; bat.floorPct = first ? 0 : bat.pct; bat.cvSince = 0; }
  } else if (bat.host) bat.byHost = true;
  else if (bat.byHost || step <= -BAT_STEP || (bat.trend <= -25 && raw < 4100)) bat.usb = false;   // frames stopped, or the voltage fell back
  if (was != bat.usb || gap) { bat.mv = raw; bat.nh = 0; bat.trend = 0; }                           // start the smoothing and the trend afresh
  else bat.mv = bat.mv ? (bat.mv * 3 + raw) / 4 : raw;                                              // light smoothing
  bat.raw = raw; bat.lastSample = now;
  bat.present = bat.mv > 2000;                                                                      // a drained cell recovering on the charger starts near 2.5 V; with no cell the divider reads far lower

  if (!bat.usb) { bat.pct = batCurve(bat.mv); bat.full = false; bat.cvSince = 0; }
  else {
    int est;
    if (bat.mv < BAT_CV) { est = batCurve(bat.mv - bat.lift); bat.cvSince = 0; }
    else {   // plateau: walk from the level at its start to 100% over the top-off time
      if (!bat.cvSince) { bat.cvSince = now ? now : 1; bat.cvBase = max(bat.floorPct, batCurve(bat.mv - bat.lift)); }
      est = bat.cvBase + (100 - bat.cvBase) * min((int)((now - bat.cvSince) / 60000), BAT_TOPOFF_MIN) / BAT_TOPOFF_MIN;
    }
    bat.floorPct = max(bat.floorPct, est); bat.pct = bat.floorPct;
    bat.full = bat.cvSince && now - bat.cvSince >= BAT_TOPOFF_MIN * 60000UL;
  }
  bat.charging = bat.usb && bat.present && !bat.full;

  if (++bat.samples == 4) { bat.mv0 = bat.mv; bat.pct0 = bat.pct; bat.t0 = now; }   // the smoothing has settled by the fourth sample
  if (bat.samples >= 4 && (!bat.nh || now - bat.lastHist > 60000)) {
    if (bat.nh == 6) { for (int i = 1; i < 6; i++) bat.hist[i - 1] = bat.hist[i]; bat.nh = 5; }
    bat.hist[bat.nh++] = bat.mv; bat.lastHist = now; bat.trend = bat.hist[bat.nh - 1] - bat.hist[0];
  }
}
int batPercent() { return bat.pct; } bool batUsb() { return bat.usb; } bool batPresent() { return bat.present; }
void batLoop() { if (millis() - bat.lastSample > 5000 || !bat.samples) batSample(); }
