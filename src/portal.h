#pragma once
// Web portal: http://perch.local (mDNS) or the board's IP. Serves the drag-and-drop page, which converts
// EPUBs in the browser and uploads the plain-text result into /books on the SD card / flash.
#include <WebServer.h>
#include <ESPmDNS.h>
#include "net.h"
#include "storage.h"
#include "app_book.h"

extern const char portalHtml[] asm("_binary_portal_index_html_start");
extern const char portalHtmlEnd[] asm("_binary_portal_index_html_end");
extern const char portalJs[] asm("_binary_portal_convert_js_start");
extern const char portalJsEnd[] asm("_binary_portal_convert_js_end");
extern const char portalHyph[] asm("_binary_portal_hyph_en_us_txt_start");
extern const char portalHyphEnd[] asm("_binary_portal_hyph_en_us_txt_end");

WebServer portal(80);
bool portalUp = false, mdnsUp = false;

// ---- pairing: no accounts. While Settings > Portal is open the board shows a 6-digit code that changes every minute; a
// browser that sends it back gets a token, which every other API call must carry as ?t=. "Keep for 30 days" tokens are
// stored in flash, the others last until the board restarts. The same API is meant to be served by every Perch, so one
// page can hold a token for each device and work across them (hence CORS, and the token in the query, not a cookie).
struct PairTok { char t[17]; uint32_t exp; };   // exp: epoch seconds, 0 = until restart, 0xFFFFFFFF = no clock when issued
PairTok toks[8]; int ntoks = 0;
bool pairShowing = false; uint32_t pairCode = 0, pairPrev = 0, pairAt = 0, pairLockUntil = 0; int pairFails = 0;
void pairTick() { if (!pairAt || millis() - pairAt >= 60000) { pairPrev = pairAt ? pairCode : 1000000; pairCode = esp_random() % 1000000; pairAt = millis() ? millis() : 1; } }
int pairLeft() { return pairAt ? max(0, 60 - (int)((millis() - pairAt) / 1000)) : 0; }
int pairCount() { return ntoks; }
void toksSave() {
  String v; for (int i = 0; i < ntoks; i++) if (toks[i].exp) { if (v.length()) v += ','; v += toks[i].t; v += ':'; v += String(toks[i].exp); }
  prefs.begin("perch", false); prefs.putString("ptok", v); prefs.end();
}
void toksLoad() {
  prefs.begin("perch", true); String v = prefs.getString("ptok", ""); prefs.end(); ntoks = 0;
  while (v.length() && ntoks < 8) {
    int c = v.indexOf(','); String one = c < 0 ? v : v.substring(0, c); v = c < 0 ? String() : v.substring(c + 1);
    int k = one.indexOf(':'); if (k != 16) continue;
    strlcpy(toks[ntoks].t, one.substring(0, 16).c_str(), 17); toks[ntoks].exp = strtoul(one.c_str() + 17, nullptr, 10); ntoks++;
  }
}
void pairForgetAll() { ntoks = 0; toksSave(); }
bool tokOk(const String &t) {
  if (t.length() != 16) return false;
  for (int i = 0; i < ntoks; i++) if (t == toks[i].t) {
    if (toks[i].exp && toks[i].exp != 0xFFFFFFFF && clockValid() && (uint32_t)time(nullptr) > toks[i].exp) { toks[i] = toks[--ntoks]; toksSave(); return false; }   // the 30 days are up
    return true;
  }
  return false;
}
bool need() { if (tokOk(portal.arg("t"))) return true; portal.send(401, "text/plain", "pair this browser first"); return false; }
String portalId() { char b[13]; snprintf(b, sizeof b, "%012llx", (unsigned long long)ESP.getEfuseMac()); return b; }

// ---- other Perches on the network, found over mDNS by a background task (a query blocks for a few seconds)
struct Peer { String host, ip; }; Peer peers[6]; int npeers = 0; volatile bool peerWant = false, peerBusy = false; uint32_t peerAt = 0;
void peerTask(void *) {
  for (;;) {
    if (peerWant && mdnsUp && netConnected()) {
      peerBusy = true; peerWant = false;
      int n = MDNS.queryService("perch", "tcp"), k = 0; String me = WiFi.localIP().toString();
      for (int i = 0; i < n && k < 6; i++) { String ip = MDNS.address(i).toString(); if (ip == me || ip == "0.0.0.0") continue; peers[k].host = MDNS.hostname(i); peers[k].ip = ip; k++; }
      npeers = k; peerAt = millis(); peerBusy = false;
    }
    delay(200);
  }
}
File upFile; String upName; size_t upBytes = 0; bool upOk = false;

static String jsonStr(const String &s) { String o = "\""; for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if ((uint8_t)c < 32) o += ' '; else o += c; } return o + "\""; }
static bool safeName(const String &n) {   // slug.txt, slug.cov or slug.cvl only: no paths, no odd characters
  if (!(n.endsWith(".txt") || n.endsWith(".cov") || n.endsWith(".cvl")) || n.length() < 5 || n.length() > 48) return false;
  for (char c : n) if (!(isalnum((uint8_t)c) || c == '-' || c == '_' || c == '.')) return false;
  return n.indexOf("..") < 0;
}

void portalInfo() {   // public: what the pairing page and other Perches' pages need to show this device
  static bool scanned = false; if (!scanned) { libScan(); scanned = true; }
  String t = portal.arg("t");
  String j = "{\"id\":" + jsonStr(portalId()) + ",\"name\":" + jsonStr(cfg.name) + ",\"kind\":\"2.8\\u2033 LCD\",\"type\":\"lcd\",\"host\":\"perch.local\",\"ip\":" + jsonStr(WiFi.localIP().toString());
  j += ",\"battery\":{\"present\":" + String(bat.present ? "true" : "false") + ",\"pct\":" + String(bat.pct) + ",\"charging\":" + String(bat.charging ? "true" : "false") + ",\"usb\":" + String(bat.usb ? "true" : "false") + "}";
  j += ",\"storage\":{\"name\":" + jsonStr(storageName()) + ",\"sd\":" + String(storageSD ? "true" : "false") + ",\"free\":" + String((double)(storageTotal() - storageUsed()), 0) + ",\"total\":" + String((double)storageTotal(), 0) + "}";
  j += ",\"books\":" + String(nlib) + ",\"paired\":" + String(tokOk(t) ? "true" : "false") + ",\"sync\":" + String(syncOn() ? "true" : "false") + ",\"pairing\":" + String(pairShowing ? "true" : "false") + ",\"left\":" + String(pairLeft()) + "}";
  portal.send(200, "application/json", j);
}
void portalPair() {
  if (!pairShowing) { portal.send(409, "text/plain", "open Settings, then Portal on the device"); return; }
  if ((int32_t)(millis() - pairLockUntil) < 0) { portal.send(429, "text/plain", "too many wrong codes, wait a minute"); return; }
  pairTick(); uint32_t code = strtoul(portal.arg("code").c_str(), nullptr, 10);
  bool ok = portal.arg("code").length() == 6 && (code == pairCode || (code == pairPrev && millis() - pairAt < 10000));   // the code just replaced still counts for 10 s
  if (!ok) { if (++pairFails >= 5) { pairFails = 0; pairLockUntil = millis() + 60000; } portal.send(403, "text/plain", "that code is not the one on the device"); return; }
  pairFails = 0;
  if (ntoks == 8) { for (int i = 1; i < 8; i++) toks[i - 1] = toks[i]; ntoks = 7; }   // drop the oldest
  PairTok &k = toks[ntoks++]; snprintf(k.t, sizeof k.t, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  bool keep = portal.arg("keep") == "1"; k.exp = !keep ? 0 : clockValid() ? (uint32_t)time(nullptr) + 30 * 86400UL : 0xFFFFFFFF;
  toksSave(); pairCode = esp_random() % 1000000; pairAt = millis();   // a used code is spent
  portal.send(200, "application/json", String("{\"token\":\"") + k.t + "\"}");
}
void portalUnpair() { String t = portal.arg("t"); for (int i = 0; i < ntoks; i++) if (t == toks[i].t) { toks[i] = toks[--ntoks]; toksSave(); break; } portal.send(200, "text/plain", "ok"); }
void portalDevices() {
  if (!need()) return;
  if (portal.arg("scan") == "1" || !peerAt) peerWant = true;
  String j = String("{\"scanning\":") + (peerWant || peerBusy ? "true" : "false") + ",\"peers\":[";
  if (!peerBusy) for (int i = 0; i < npeers; i++) { if (i) j += ","; j += "{\"host\":" + jsonStr(peers[i].host + ".local") + ",\"ip\":" + jsonStr(peers[i].ip) + "}"; }
  portal.send(200, "application/json", j + "]}");
}
// One book: its chapter titles (read off the file), size, date added, and where the reader is.
void portalBook() {
  if (!need()) return;
  String f = portal.arg("file"); File fl;
  if (!safeName(f) || !bookFS || !(fl = bookFS->open(String(BOOK_DIR) + "/" + f))) { portal.send(404, "text/plain", "no such book"); return; }
  static uint8_t buf[2048]; String title, author, ch = "[", cur; int n = 0, state = 0;   // state 1: inside a chapter title
  int r = fl.read(buf, 200); if (r > 0) bookHeader((const char *)buf, (const char *)buf + r, title, author);
  fl.seek(0);
  while ((r = fl.read(buf, sizeof buf)) > 0) for (int i = 0; i < r; i++) {
    uint8_t c = buf[i];
    if (state) { if (c == '\n') { if (n < 400) { if (n) ch += ","; ch += jsonStr(cur); } n++; state = 0; } else if (c >= 32 && cur.length() < 90) cur += (char)c; }
    else if (c == 1) { state = 1; cur = ""; }
  }
  uint32_t size = fl.size(); time_t added = fl.getLastWrite(); fl.close();
  int chNow = -1, off = 0; uint32_t last = 0; bool opened = bookProgress(f, chNow, off, &last);
  String j = "{\"file\":" + jsonStr(f) + ",\"title\":" + jsonStr(title.length() ? title : f) + ",\"author\":" + jsonStr(author) + ",\"size\":" + String(size) + ",\"added\":" + String((uint32_t)added);
  j += ",\"pct\":" + String(opened && size ? constrain((int)(off * 100LL / size), 0, 100) : -1) + ",\"ch\":" + String(opened ? chNow : -1) + ",\"nch\":" + String(n) + ",\"chapters\":" + ch + "]}";
  portal.send(200, "application/json", j);
}
void portalBooks() {
  if (!need()) return;
  libScan();
  String j = "{\"storage\":" + jsonStr(storageName()) + ",\"free\":" + String((double)(storageTotal() - storageUsed()), 0) + ",\"books\":[";
  for (int i = 0; i < nlib; i++) {
    if (i) j += ",";
    j += "{\"file\":" + jsonStr(lib[i].file) + ",\"title\":" + jsonStr(lib[i].title) + ",\"author\":" + jsonStr(lib[i].author) + ",\"size\":" + String(lib[i].size) + ",\"pct\":" + String(lib[i].pct) + ",\"cover\":" + (lib[i].cover ? "true" : "false") + "}";
  }
  portal.send(200, "application/json", j + "]}");
}
void portalUpload() {   // multipart chunks arrive here; the response goes out in the route handler
  HTTPUpload &u = portal.upload();
  if (u.status == UPLOAD_FILE_START) {
    upName = u.filename; int sl = upName.lastIndexOf('/'); if (sl >= 0) upName = upName.substring(sl + 1);
    upBytes = 0; upOk = false;
    if (!tokOk(portal.arg("t"))) { Serial.println("portal: upload without a token"); return; }
    if (!bookFS || !safeName(upName)) { Serial.printf("portal: rejected upload '%s'\n", upName.c_str()); return; }
    if (book.file == upName && book.buf) { bookSave(); }   // replacing the open book: keep its position
    upFile = bookFS->open(String(BOOK_DIR) + "/" + upName + ".part", FILE_WRITE); upOk = (bool)upFile;
    Serial.printf("portal: upload %s\n", upName.c_str());
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (upOk && upFile.write(u.buf, u.currentSize) != u.currentSize) { upOk = false; upFile.close(); Serial.println("portal: write failed (storage full?)"); }
    upBytes += u.currentSize;
  } else if (u.status == UPLOAD_FILE_END || u.status == UPLOAD_FILE_ABORTED) {
    if (upFile) upFile.close();
    String part = String(BOOK_DIR) + "/" + upName + ".part", fin = String(BOOK_DIR) + "/" + upName;
    if (upOk && u.status == UPLOAD_FILE_END) { bookFS->remove(fin); upOk = bookFS->rename(part, fin); Serial.printf("portal: saved %s, %u bytes\n", upName.c_str(), (unsigned)upBytes); }
    else upOk = false;
    if (!upOk && bookFS) bookFS->remove(part);
    if (upOk && book.file == upName) bookLoad(upName);   // re-read a replaced open book
    if (current() == &libraryScreen) { libScan(); libraryScreen.dirty = true; }
  }
}
void portalBegin() {
  toksLoad(); portal.enableCORS(true);
  xTaskCreatePinnedToCore(peerTask, "peers", 4096, nullptr, 1, nullptr, 0);
  portal.on("/api/info", HTTP_GET, portalInfo);
  portal.on("/api/pair", HTTP_POST, portalPair);
  portal.on("/api/unpair", HTTP_POST, portalUnpair);
  portal.on("/api/devices", HTTP_GET, portalDevices);
  portal.on("/api/book", HTTP_GET, portalBook);
  portal.on("/api/sync", HTTP_GET, []() {   // cloud sync settings; the page copies them to every device so they share one home key
    if (!need()) return;
    portal.send(200, "application/json", "{\"url\":" + jsonStr(cfg.syncUrl) + ",\"key\":" + jsonStr(cfg.syncKey) + ",\"on\":" + String(syncOn() ? "true" : "false") + ",\"err\":" + jsonStr(sy.err) + "}");
  });
  portal.on("/api/sync", HTTP_POST, []() {
    if (!need()) return;
    String u = portal.arg("url"), k = portal.arg("key"); u.trim(); k.trim(); while (u.endsWith("/")) u.remove(u.length() - 1);
    if (u.length() && (!u.startsWith("https://") || k.length() < 24 || k.length() > 128)) { portal.send(400, "text/plain", "needs an https address and a key of 24 characters or more"); return; }
    cfg.syncUrl = u; cfg.syncKey = u.length() ? k : String(); cfgSave(); sy.lastPushed = ""; sy.err = "";
    portal.send(200, "text/plain", "ok");
  });
  // no-cache: a browser holding last week's page or converter would keep sending books in the old format
  portal.on("/", HTTP_GET, []() { portal.sendHeader("Cache-Control", "no-cache"); portal.send_P(200, "text/html", portalHtml, portalHtmlEnd - portalHtml - 1); });
  portal.on("/convert.js", HTTP_GET, []() { portal.sendHeader("Cache-Control", "no-cache"); portal.send_P(200, "application/javascript", portalJs, portalJsEnd - portalJs - 1); });
  portal.on("/hyph-en-us.txt", HTTP_GET, []() { portal.send_P(200, "text/plain", portalHyph, portalHyphEnd - portalHyph - 1); });   // hyphenation patterns for the converter
  portal.on("/api/books", HTTP_GET, portalBooks);
  portal.on("/api/upload", HTTP_POST, []() { if (upOk) portal.send(200, "text/plain", "ok"); else if (!tokOk(portal.arg("t"))) portal.send(401, "text/plain", "pair this browser first"); else portal.send(bookFS ? 400 : 507, "text/plain", bookFS ? "upload failed: bad name or storage full" : "no storage on the board"); }, portalUpload);
  portal.on("/api/cover", HTTP_GET, []() {   // the book's thumbnail sidecar, for the portal's list
    if (!need()) return;
    String f = portal.arg("file"); File c;
    if (!safeName(f) || !bookFS || !(c = bookFS->open(String(BOOK_DIR) + "/" + coverName(f)))) { portal.send(404, "text/plain", "no cover"); return; }
    portal.streamFile(c, "application/octet-stream"); c.close();
  });
  portal.on("/api/delete", HTTP_POST, []() {
    if (!need()) return;
    String f = portal.arg("file");
    if (!safeName(f) || !bookDelete(f)) { portal.send(400, "text/plain", "not deleted"); return; }
    if (current() == &libraryScreen) { libScan(); libraryScreen.dirty = true; }
    portal.send(200, "text/plain", "ok");
  });
  portal.onNotFound([]() { portal.send(404, "text/plain", "not found"); });
}
// Before Wi-Fi is switched off: WiFi.mode(WIFI_OFF) destroys the interface the mDNS responder points at, and stopping or
// restarting it afterwards read through the dead interface and crashed. portalLoop starts it again on reconnect.
void portalNetDown() {
  peerWant = false; for (int i = 0; i < 50 && peerBusy; i++) delay(100);   // let a query in flight finish first
  if (mdnsUp) { MDNS.end(); mdnsUp = false; }
}
void portalLoop() {
  bool c = netConnected();
  if (c && !portalUp) { portal.begin(); portalUp = true; Serial.printf("portal: http://%s\n", WiFi.localIP().toString().c_str()); }
  if (c && !mdnsUp) { mdnsUp = MDNS.begin("perch"); if (mdnsUp) { MDNS.addService("http", "tcp", 80); MDNS.addService("perch", "tcp", 80); } Serial.printf("mdns: %s\n", mdnsUp ? "perch.local" : "failed"); }
  if (!c && mdnsUp) { MDNS.end(); mdnsUp = false; }
  if (portalUp) portal.handleClient();
}
