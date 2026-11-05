#pragma once
// Spotify Connect remote: token refresh (PKCE, no secret), player polling and commands,
// all run on a background task so the UI never blocks on TLS. Every TLS client gets a 5 s connect and
// 10 s handshake cap (the core defaults to 120 s), so a dropped hotspot packet cannot wedge the worker.
#include "net.h"

struct SpState {
  String clientId, refresh, access; uint32_t accessUntil = 0;
  volatile bool busy = false; bool linked = false; String err;
  // now playing
  bool have = false, playing = false; String title, artist, album, device, deviceId, artUrl;
  int durMs = 0, progMs = 0, volume = -1; uint32_t progAt = 0;
  // album art (jpeg bytes in PSRAM)
  uint8_t *art = nullptr; size_t artLen = 0; String artLoaded; volatile bool artNew = false;
  // pending command
  String cmd; int cmdArg = 0; volatile bool pending = false;   // pending: a command was sent, state not yet confirmed
  uint32_t lastPoll = 0; bool wantPolling = false;
} sp;

void spLoad() {
  prefs.begin("perch", true);
  sp.clientId = prefs.getString("spcid", ""); sp.refresh = prefs.getString("sprt", "");
  prefs.end(); sp.linked = sp.refresh.length() && sp.clientId.length();
}
void spSave() { prefs.begin("perch", false); prefs.putString("spcid", sp.clientId); prefs.putString("sprt", sp.refresh); prefs.end(); }

// Read a response body with a loop that yields. HTTPClient::getString spins at this task's priority while the
// server is slow with the body, which starves IDLE0 and trips the task watchdog (the board rebooted mid-poll).
// Handles a Content-Length body and a chunked one (Spotify's HTTP/1.1 replies on the keep-alive connection).
struct BodyReader {
  WiFiClient *s; uint32_t last = millis(), maxMs;
  BodyReader(WiFiClient *c, uint32_t ms) : s(c), maxMs(ms) {}
  int get() {   // next byte, -1 on timeout or close
    while (millis() - last < maxMs) {
      if (s->available() > 0) { int c = s->read(); if (c >= 0) { last = millis(); return c; } }
      else if (!s->connected()) return -1;
      else delay(2);
    }
    return -1;
  }
  bool take(String &out, int n) { while (n-- > 0) { int c = get(); if (c < 0) return false; out += (char)c; } return true; }
  bool line(String &l) { l = ""; for (;;) { int c = get(); if (c < 0) return false; if (c == '\n') { l.trim(); return true; } l += (char)c; } }
};
bool spReadBody(HTTPClient &http, String &out, uint32_t maxMs = 4000) {
  out = ""; int len = http.getSize(); WiFiClient *s = http.getStreamPtr(); if (!s) return false;
  BodyReader r(s, maxMs);
  if (len >= 0) return r.take(out, len);
  for (;;) {   // chunked: "<hex>\r\n" data "\r\n" ... "0\r\n\r\n"
    String l; if (!r.line(l)) return false;
    int n = (int)strtol(l.c_str(), nullptr, 16);
    if (n <= 0) { r.line(l); return true; }   // trailing CRLF (or trailers) after the last chunk
    if (!r.take(out, n) || !r.line(l)) return false;
  }
}

bool spRefreshToken() {
  WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(10); HTTPClient http; http.setTimeout(8000); http.setConnectTimeout(5000);
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) { sp.err = "token begin"; return false; }
  http.useHTTP10(true);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "grant_type=refresh_token&refresh_token=" + sp.refresh + "&client_id=" + sp.clientId;
  int code = http.POST(body); String resp; if (code > 0 && !spReadBody(http, resp)) code = HTTPC_ERROR_READ_TIMEOUT;
  if (code < 0) { char eb[100] = ""; client.lastError(eb, sizeof eb); IPAddress ip; bool dns = WiFi.hostByName("accounts.spotify.com", ip);
    Serial.printf("spotify: connect failed: %s | tls: %s | dns %d %s | heap %u max %u\n", HTTPClient::errorToString(code).c_str(), eb, dns, ip.toString().c_str(), ESP.getFreeHeap(), ESP.getMaxAllocHeap()); }
  http.end();
  if (code != 200) { sp.err = code < 0 ? "Spotify: " + HTTPClient::errorToString(code) : "token http " + String(code); Serial.printf("spotify: token refresh failed %d: %.200s\n", code, resp.c_str()); return false; }
  JsonDocument doc; if (deserializeJson(doc, resp)) { sp.err = "token json"; return false; }
  sp.access = (const char *)doc["access_token"]; sp.accessUntil = millis() + ((int)(doc["expires_in"] | 3600) - 60) * 1000UL;
  if (doc["refresh_token"].is<const char *>()) { sp.refresh = (const char *)doc["refresh_token"]; spSave(); } // PKCE rotates it
  sp.err = ""; return true;
}
bool spEnsureToken() { if (sp.access.length() && millis() < sp.accessUntil) return true; return spRefreshToken(); }

// generic authorised request; returns HTTP code, body in `out` (may be empty).
// One TLS connection to api.spotify.com is kept alive between calls (saves ~1 s handshake each).
WiFiClientSecure *apiClient = nullptr; HTTPClient apiHttp;
int spRequest(const char *method, const String &url, String &out, const String &payload = "") {
  if (!spEnsureToken()) return -1;
  if (!apiClient) { apiClient = new WiFiClientSecure(); apiClient->setInsecure(); apiClient->setHandshakeTimeout(10); }
  HTTPClient &http = apiHttp; http.setReuse(true); http.setTimeout(4000); http.setConnectTimeout(5000);
  uint32_t t0 = millis();
  if (!http.begin(*apiClient, url)) return -2;
  http.addHeader("Authorization", "Bearer " + sp.access);
  if (payload.length()) http.addHeader("Content-Type", "application/json");
  else if (strcmp(method, "GET") != 0) http.addHeader("Content-Length", "0"); // Spotify answers 411 without it
  int code = http.sendRequest(method, (uint8_t *)payload.c_str(), payload.length());
  if (code > 0 && !spReadBody(http, out)) code = HTTPC_ERROR_READ_TIMEOUT;
  http.end();
  if (code == 401) { sp.access = ""; }
  if (code < 0) { apiClient->stop(); delete apiClient; apiClient = nullptr; }   // drop a broken keep-alive socket
  Serial.printf("sp: %s %s -> %d in %lu ms\n", method, url.c_str() + 23, code, (unsigned long)(millis() - t0));
  return code;
}

bool spPoll() {
  String body; int code = spRequest("GET", "https://api.spotify.com/v1/me/player", body);
  if (code == 204 || (code == 200 && body.length() < 10)) { sp.have = false; sp.err = ""; return true; }
  static int fails = 0;
  if (code != 200) { fails++; if (fails >= 2 || code == 403) sp.err = code == 403 ? "Spotify Premium required" : "player http " + String(code); return false; }
  fails = 0;
  JsonDocument filter;
  filter["is_playing"] = true; filter["progress_ms"] = true; filter["device"]["name"] = true; filter["device"]["id"] = true; filter["device"]["volume_percent"] = true;
  filter["item"]["name"] = true; filter["item"]["duration_ms"] = true; filter["item"]["artists"][0]["name"] = true;
  filter["item"]["album"]["name"] = true; filter["item"]["album"]["images"][0]["url"] = true; filter["item"]["album"]["images"][0]["height"] = true;
  JsonDocument doc; if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) { sp.err = "player json"; return false; }
  sp.playing = doc["is_playing"] | false; sp.progMs = doc["progress_ms"] | 0; sp.progAt = millis();
  sp.device = (const char *)(doc["device"]["name"] | ""); sp.deviceId = (const char *)(doc["device"]["id"] | ""); sp.volume = doc["device"]["volume_percent"] | -1;
  if (doc["item"].isNull()) { sp.have = false; return true; }
  sp.title = (const char *)(doc["item"]["name"] | ""); sp.durMs = doc["item"]["duration_ms"] | 0;
  sp.artist = (const char *)(doc["item"]["artists"][0]["name"] | ""); sp.album = (const char *)(doc["item"]["album"]["name"] | "");
  // smallest image (64x64 is last in the list)
  String url; int best = 100000;
  for (JsonObject im : doc["item"]["album"]["images"].as<JsonArray>()) { int h = im["height"] | 640; if (h < best) { best = h; url = (const char *)im["url"]; } }
  sp.artUrl = url; sp.have = true; sp.err = ""; return true;
}

bool spFetchArt() {
  if (!sp.artUrl.length() || sp.artUrl == sp.artLoaded) return true;
  WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(10); HTTPClient http; http.setTimeout(8000); http.setConnectTimeout(5000);
  if (!http.begin(client, sp.artUrl)) return false;
  int code = http.GET(); if (code != 200) { http.end(); return false; }
  int len = http.getSize(); if (len <= 0 || len > 60000) { http.end(); return false; }
  uint8_t *buf = (uint8_t *)ps_malloc(len); if (!buf) { http.end(); return false; }
  WiFiClient *s = http.getStreamPtr(); int got = 0; uint32_t t0 = millis();
  while (got < len && millis() - t0 < 8000) { int n = s->read(buf + got, len - got); if (n > 0) got += n; else delay(2); }
  http.end();
  if (got != len) { free(buf); return false; }
  uint8_t *old = sp.art; sp.art = buf; sp.artLen = len; sp.artLoaded = sp.artUrl; sp.artNew = true; if (old) free(old);
  return true;
}

void spRunCommand() {
  String c = sp.cmd; sp.cmd = ""; String body; int code = 0;
  String dev = sp.deviceId.length() ? "device_id=" + sp.deviceId : ""; // target the last seen device, so an idle phone wakes up
  if (c == "play") code = spRequest("PUT", "https://api.spotify.com/v1/me/player/play?" + dev, body, "{}");
  else if (c == "pause") code = spRequest("PUT", "https://api.spotify.com/v1/me/player/pause?" + dev, body);
  else if (c == "next") code = spRequest("POST", "https://api.spotify.com/v1/me/player/next?" + dev, body);
  else if (c == "prev") code = spRequest("POST", "https://api.spotify.com/v1/me/player/previous?" + dev, body);
  else if (c == "vol") code = spRequest("PUT", "https://api.spotify.com/v1/me/player/volume?volume_percent=" + String(sp.cmdArg) + "&" + dev, body);
  if (code == 403 || code == 404) sp.err = body.indexOf("PREMIUM") >= 0 ? "Spotify Premium required" : "No active device";
  delay(250); // let Spotify apply it before we poll
}
// one long-lived worker: runs commands as soon as they arrive, polls while the app is open
void spTask(void *) {
  for (;;) {
    bool linked = sp.linked && netConnected();
    if (linked && sp.cmd.length()) {
      xSemaphoreTake(netMutex, portMAX_DELAY); sp.busy = true;
      spRunCommand(); if (spPoll()) spFetchArt(); sp.pending = false; sp.lastPoll = millis();
      sp.busy = false; xSemaphoreGive(netMutex);
    } else if (linked && sp.wantPolling && millis() - sp.lastPoll > 2000) {
      xSemaphoreTake(netMutex, portMAX_DELAY); sp.busy = true;
      bool ok = spPoll(); if (ok) spFetchArt();
      static int fails = 0; fails = ok ? 0 : fails + 1;
      sp.lastPoll = ok ? millis() : millis() + (fails < 3 ? -1000 : 3000);   // retry after 1 s, then every 5 s once the link looks dead
      sp.busy = false; xSemaphoreGive(netMutex);
    } else delay(20);
  }
}
void spBegin() { xTaskCreatePinnedToCore(spTask, "sp", 12288, nullptr, 1, nullptr, 0); }
void spKick() { sp.lastPoll = 0; }
void spCommand(const char *c, int arg = 0) { sp.cmd = c; sp.cmdArg = arg; sp.pending = true; }
void spLoop() {}
bool spBusyNow() { return sp.busy; }
int spProgressMs() { if (!sp.have) return 0; int p = sp.progMs + (sp.playing ? (int)(millis() - sp.progAt) : 0); return min(p, sp.durMs); }
