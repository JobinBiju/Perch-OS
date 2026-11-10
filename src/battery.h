#pragma once
// Battery monitor: BAT_ADC on GPIO 9 sits on a 200k/200k divider, so cell voltage = 2 x pin voltage.
// The TP4054 charger has no status line wired to the ESP32, so "charging" is inferred from the voltage.
#include <Arduino.h>
#include "soc/usb_serial_jtag_reg.h"

static constexpr int BAT_PIN = 9;
struct Battery { int mv = 0; int pct = 0; bool present = false, full = false, usb = false, host = true, charging = false; uint32_t lastSample = 0, lastFrame = 0;
  int hist[6] = {0}, nh = 0, trend = 0; uint32_t lastHist = 0;   // one reading a minute: trend = mV gained over the last (nh - 1) minutes
  int mv0 = 0, pct0 = 0, samples = 0; uint32_t t0 = 0;           // first settled reading after power-on, for "since power-on"
} bat;

void batSample() {
  static bool init = false;
  if (!init) { analogReadResolution(12); analogSetPinAttenuation(BAT_PIN, ADC_11db); init = true; }
  uint32_t sum = 0; for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BAT_PIN);
  int mv = (int)(sum / 16) * 2;
  bat.mv = bat.mv ? (bat.mv * 3 + mv) / 4 : mv;           // light smoothing
  bat.present = bat.mv > 3000;                             // below that nothing is on the divider
  bat.full = bat.mv >= 4150;                               // charger holding the cell at termination (or USB with no cell)
  // LiPo open-circuit curve, 3.3 V empty to 4.2 V full
  static const int pts[][2] = {{3300, 0}, {3500, 5}, {3600, 10}, {3700, 30}, {3800, 55}, {3900, 72}, {4000, 85}, {4100, 95}, {4200, 100}};
  int p = 0;
  if (bat.mv <= pts[0][0]) p = 0; else if (bat.mv >= pts[8][0]) p = 100;
  else for (int i = 1; i < 9; i++) if (bat.mv <= pts[i][0]) { p = pts[i - 1][1] + (pts[i][1] - pts[i - 1][1]) * (bat.mv - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]); break; }
  bat.pct = p; bat.lastSample = millis();
  // USB host present: the USB-Serial-JTAG frame counter advances only while a host sends SOF packets
  uint32_t fr = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & 0x7FF;
  static int still = 0; still = fr != bat.lastFrame ? 0 : still + 1;
  bat.host = still < 2;                                    // a computer is attached (two samples in a row without SOFs = none); gates battery sleep
  bat.usb = (fr != bat.lastFrame) || bat.full; bat.lastFrame = fr;
  if (++bat.samples == 4) { bat.mv0 = bat.mv; bat.pct0 = bat.pct; bat.t0 = millis(); }   // the smoothing has settled by the fourth sample
  if (bat.samples >= 4 && (!bat.nh || millis() - bat.lastHist > 60000)) {
    if (bat.nh == 6) { for (int i = 1; i < 6; i++) bat.hist[i - 1] = bat.hist[i]; bat.nh = 5; }
    bat.hist[bat.nh++] = bat.mv; bat.lastHist = millis(); bat.trend = bat.hist[bat.nh - 1] - bat.hist[0];
  }
  // no charger status line: a computer on USB always charges the cell; on a wall adapter a rising voltage gives it away
  bat.charging = bat.present && !bat.full && (bat.host || bat.trend > 5);
}
int batPercent() { return bat.pct; } bool batUsb() { return bat.usb; } bool batPresent() { return bat.present; }
void batLoop() { if (millis() - bat.lastSample > 5000 || !bat.lastSample) batSample(); }
