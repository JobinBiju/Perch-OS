#pragma once
// Cloud sync of reading positions through a Cloudflare Worker (github.com/JobinBiju/Perch-Cloud, whose README is the contract). Every Perch of one owner holds the same
// "home key"; a device pushes its position when the reader is left or the device sleeps, and asks for the newest one when
// a book is opened. Positions are chapter + bytes into the chapter (with the chapter's length, so copies converted
// differently still land on the same paragraph) and carry the time they were read; the newest wins.
#include "net.h"

struct SyncPos { int ch = -1, rel = 0, len = 0, pct = 0; uint32_t t = 0; String dev; };
struct SyncState {
  String pushFile, pullFile, lastPushed, err; SyncPos push, got;
  volatile bool pushWant = false, pullWant = false, busy = false, pullDone = false; bool gotOk = false; uint32_t lastOk = 0;
  // notes (bookmarks and highlights): the whole set goes up as JSON and the merged set comes back
  String notesFile, notesBody, notesOut; volatile bool notesWant = false, notesDone = false; bool notesOk = false; uint32_t notesGen = 0;
} sy;
bool syncOn() { return cfg.syncUrl.startsWith("https://") && cfg.syncKey.length() >= 24; }
int syncHttp(const char *method, const String &file, const String &body, String &out, const char *what = "pos") {
  WiFiClientSecure c; c.setInsecure(); c.setHandshakeTimeout(10); HTTPClient h; h.setTimeout(6000); h.setConnectTimeout(6000);
  if (!h.begin(c, cfg.syncUrl + "/v1/" + what + "/" + file)) return -1;
  h.addHeader("Authorization", "Bearer " + cfg.syncKey); if (body.length()) h.addHeader("Content-Type", "application/json");
  int code = h.sendRequest(method, (uint8_t *)body.c_str(), body.length()); if (code > 0) out = h.getString();
  h.end(); return code;
}
// one background worker, sharing the TLS lock with the Spotify and weather workers
void syncTask(void *) {
  for (;;) {
    if (!(sy.pushWant || sy.pullWant || sy.notesWant) || !syncOn() || !netConnected()) { delay(50); continue; }
    xSemaphoreTake(netMutex, portMAX_DELAY); sy.busy = true;
    if (sy.pushWant) {
      String f = sy.pushFile, out; SyncPos p = sy.push; sy.pushWant = false;
      JsonDocument d; d["ch"] = p.ch; d["rel"] = p.rel; d["len"] = p.len; d["pct"] = p.pct; d["t"] = p.t; d["dev"] = p.dev; String body; serializeJson(d, body);
      uint32_t t0 = millis(); int code = syncHttp("PUT", f, body, out);
      Serial.printf("sync: push %s ch %d -> %d in %lu ms\n", f.c_str(), p.ch, code, (unsigned long)(millis() - t0));
      if (code == 200) { sy.lastOk = millis(); sy.err = ""; } else { sy.err = "push " + String(code); sy.lastPushed = ""; }   // try again at the next chance
    }
    if (sy.pullWant) {
      String f = sy.pullFile, out; sy.pullWant = false; uint32_t t0 = millis(); int code = syncHttp("GET", f, "", out);
      JsonDocument d; bool ok = code == 200 && !deserializeJson(d, out) && d["t"].is<uint32_t>();
      if (ok) { sy.got.ch = d["ch"] | -1; sy.got.rel = d["rel"] | 0; sy.got.len = d["len"] | 0; sy.got.pct = d["pct"] | 0; sy.got.t = d["t"] | 0; sy.got.dev = (const char *)(d["dev"] | ""); }
      Serial.printf("sync: pull %s -> %d%s in %lu ms\n", f.c_str(), code, ok ? "" : " (nothing stored)", (unsigned long)(millis() - t0));
      if (code == 200) { sy.lastOk = millis(); sy.err = ""; } else sy.err = "pull " + String(code);
      if (f == sy.pullFile) { sy.gotOk = ok; sy.pullDone = true; }
    }
    if (sy.notesWant) {
      String f = sy.notesFile, body = sy.notesBody, out; sy.notesWant = false; uint32_t t0 = millis(); int code = syncHttp("POST", f, body, out, "notes");
      Serial.printf("sync: notes %s -> %d, %u bytes in %lu ms\n", f.c_str(), code, (unsigned)out.length(), (unsigned long)(millis() - t0));
      if (code == 200) { sy.lastOk = millis(); sy.err = ""; } else sy.err = "notes " + String(code);
      if (f == sy.notesFile && !sy.notesWant) { sy.notesOut = out; sy.notesOk = code == 200; sy.notesDone = true; }
    }
    sy.busy = false; xSemaphoreGive(netMutex);
  }
}
void syncBegin() { xTaskCreatePinnedToCore(syncTask, "sync", 10240, nullptr, 1, nullptr, 0); }
// Before the radio goes down for sleep: give a queued push the few seconds it needs.
void syncFlush(uint32_t ms) { uint32_t t0 = millis(); while ((sy.pushWant || sy.notesWant || sy.busy) && syncOn() && netConnected() && millis() - t0 < ms) delay(20); }
