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

WebServer portal(80);
bool portalUp = false, mdnsUp = false;
File upFile; String upName; size_t upBytes = 0; bool upOk = false;

static String jsonStr(const String &s) { String o = "\""; for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if ((uint8_t)c < 32) o += ' '; else o += c; } return o + "\""; }
static bool safeName(const String &n) {   // slug.txt only: no paths, no odd characters
  if (!n.endsWith(".txt") || n.length() < 5 || n.length() > 48) return false;
  for (char c : n) if (!(isalnum((uint8_t)c) || c == '-' || c == '_' || c == '.')) return false;
  return n.indexOf("..") < 0;
}

void portalBooks() {
  libScan();
  String j = "{\"storage\":" + jsonStr(storageName()) + ",\"free\":" + String((double)(storageTotal() - storageUsed()), 0) + ",\"books\":[";
  for (int i = 0; i < nlib; i++) {
    if (i) j += ",";
    j += "{\"file\":" + jsonStr(lib[i].file) + ",\"title\":" + jsonStr(lib[i].title) + ",\"author\":" + jsonStr(lib[i].author) + ",\"size\":" + String(lib[i].size) + ",\"pct\":" + String(lib[i].pct) + "}";
  }
  portal.send(200, "application/json", j + "]}");
}
void portalUpload() {   // multipart chunks arrive here; the response goes out in the route handler
  HTTPUpload &u = portal.upload();
  if (u.status == UPLOAD_FILE_START) {
    upName = u.filename; int sl = upName.lastIndexOf('/'); if (sl >= 0) upName = upName.substring(sl + 1);
    upBytes = 0; upOk = false;
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
  portal.on("/", HTTP_GET, []() { portal.send_P(200, "text/html", portalHtml, portalHtmlEnd - portalHtml - 1); });
  portal.on("/convert.js", HTTP_GET, []() { portal.send_P(200, "application/javascript", portalJs, portalJsEnd - portalJs - 1); });
  portal.on("/api/books", HTTP_GET, portalBooks);
  portal.on("/api/upload", HTTP_POST, []() { if (upOk) portal.send(200, "text/plain", "ok"); else portal.send(bookFS ? 400 : 507, "text/plain", bookFS ? "upload failed: bad name or storage full" : "no storage on the board"); }, portalUpload);
  portal.on("/api/delete", HTTP_POST, []() {
    String f = portal.arg("file");
    if (!safeName(f) || !bookDelete(f)) { portal.send(400, "text/plain", "not deleted"); return; }
    if (current() == &libraryScreen) { libScan(); libraryScreen.dirty = true; }
    portal.send(200, "text/plain", "ok");
  });
  portal.onNotFound([]() { portal.send(404, "text/plain", "not found"); });
}
void portalLoop() {
  bool c = netConnected();
  if (c && !portalUp) { portal.begin(); portalUp = true; Serial.printf("portal: http://%s\n", WiFi.localIP().toString().c_str()); }
  if (c && !mdnsUp) { mdnsUp = MDNS.begin("perch"); if (mdnsUp) MDNS.addService("http", "tcp", 80); Serial.printf("mdns: %s\n", mdnsUp ? "perch.local" : "failed"); }
  if (!c && mdnsUp) { MDNS.end(); mdnsUp = false; }
  if (portalUp) portal.handleClient();
}
