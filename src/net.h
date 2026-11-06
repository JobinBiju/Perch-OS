#pragma once
// Wi-Fi, saved settings (NVS), NTP clock, IP geolocation and Open-Meteo weather.
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>

Preferences prefs;
SemaphoreHandle_t netMutex = xSemaphoreCreateMutex();   // one TLS session at a time (heap + reliability)
struct Settings { String ssid, pass; int bright = 220; bool fahrenheit = false, dark = true, flip = false; int sleepMin = 0 /* 0 = never */; long tzOff = 0; String notes, place = "Kalamassery, Kerala"; } cfg;

// Settings live in NVS namespace "perch". Boards flashed before the rename kept them under "robo":
// copy the known keys across once, then drop the old namespace.
void cfgMigrate() {
  prefs.begin("perch", true); bool have = prefs.isKey("ssid") || prefs.isKey("dark"); prefs.end();
  if (have) return;
  Preferences old; if (!old.begin("robo", true)) return;
  if (!old.isKey("ssid") && !old.isKey("dark")) { old.end(); return; }
  prefs.begin("perch", false);
  const char *str[] = {"ssid", "pass", "notes", "place", "wnets", "spcid", "sprt"};
  for (const char *k : str) if (old.isKey(k)) prefs.putString(k, old.getString(k, ""));
  if (old.isKey("bright")) prefs.putInt("bright", old.getInt("bright", 220));
  const char *bools[] = {"fahr", "dark", "flip"};
  for (const char *k : bools) if (old.isKey(k)) prefs.putBool(k, old.getBool(k, false));
  if (old.isKey("tzoff")) prefs.putLong("tzoff", old.getLong("tzoff", 0));
  prefs.end(); old.end();
  old.begin("robo", false); old.clear(); old.end();
  Serial.println("nvs: settings moved from robo to perch");
}
void cfgLoad() {
  cfgMigrate();
  prefs.begin("perch", true);
  cfg.ssid = prefs.getString("ssid", ""); cfg.pass = prefs.getString("pass", "");
  cfg.bright = prefs.getInt("bright", 220); cfg.fahrenheit = prefs.getBool("fahr", false); cfg.dark = prefs.getBool("dark", true); cfg.flip = prefs.getBool("flip", false); cfg.sleepMin = prefs.getInt("sleep", 0);
  cfg.tzOff = prefs.getLong("tzoff", 0); cfg.notes = prefs.getString("notes", ""); cfg.place = prefs.getString("place", "Kalamassery, Kerala");
  prefs.end();
}
void cfgSave() {
  prefs.begin("perch", false);
  prefs.putString("ssid", cfg.ssid); prefs.putString("pass", cfg.pass); prefs.putInt("bright", cfg.bright);
  prefs.putBool("fahr", cfg.fahrenheit); prefs.putBool("dark", cfg.dark); prefs.putBool("flip", cfg.flip); prefs.putInt("sleep", cfg.sleepMin); prefs.putLong("tzoff", cfg.tzOff); prefs.putString("notes", cfg.notes); prefs.putString("place", cfg.place);
  prefs.end();
}

// ---- wifi: a list of known networks; the board scans and joins the strongest one it knows ----
struct KnownNet { String ssid, pass; };
static constexpr int MAXNETS = 8; KnownNet nets[MAXNETS]; int nnets = 0;
int netFind(const String &ssid) { for (int i = 0; i < nnets; i++) if (nets[i].ssid == ssid) return i; return -1; }
void netsSave() {
  JsonDocument doc; JsonArray a = doc.to<JsonArray>();
  for (int i = 0; i < nnets; i++) { JsonObject o = a.add<JsonObject>(); o["s"] = nets[i].ssid; o["p"] = nets[i].pass; }
  String out; serializeJson(doc, out); prefs.begin("perch", false); prefs.putString("wnets", out); prefs.end();
}
void netsLoad() {
  prefs.begin("perch", true); String in = prefs.getString("wnets", ""); prefs.end(); nnets = 0;
  JsonDocument doc;
  if (in.length() && !deserializeJson(doc, in)) for (JsonObject o : doc.as<JsonArray>()) { if (nnets >= MAXNETS) break; nets[nnets].ssid = (const char *)o["s"]; nets[nnets].pass = (const char *)(o["p"] | ""); nnets++; }
  if (!nnets && cfg.ssid.length()) { nets[0] = {cfg.ssid, cfg.pass}; nnets = 1; netsSave(); }   // migrate the single saved network
}
void netRemember(const String &ssid, const String &pass) {
  int i = netFind(ssid);
  if (i < 0) { if (nnets == MAXNETS) { for (int k = 1; k < nnets; k++) nets[k - 1] = nets[k]; nnets--; } i = nnets++; }  // drop the oldest when full
  nets[i] = {ssid, pass}; netsSave();
}
void netForget(const String &ssid) { int i = netFind(ssid); if (i < 0) return; for (int k = i + 1; k < nnets; k++) nets[k - 1] = nets[k]; nnets--; netsSave(); }

bool netConnected() { return WiFi.status() == WL_CONNECTED; }
int wifiBars() {
  if (!netConnected()) return 0;
  int r = WiFi.RSSI(); return r > -60 ? 3 : r > -75 ? 2 : 1;
}
enum NetState { NS_IDLE, NS_SCANNING, NS_CONNECTING }; NetState netState = NS_IDLE;
uint32_t connectStarted = 0, netNextTry = 0; bool wasConnected = false;
int scanCount = 0; uint32_t scanSeq = 0;   // last scan results (WiFi.SSID(i) etc. stay valid until the next scan)
void netConnectTo(const String &ssid, const String &pass) {
  cfg.ssid = ssid; cfg.pass = pass; cfgSave();
  WiFi.disconnect(); WiFi.begin(ssid.c_str(), pass.c_str()); connectStarted = millis(); netState = NS_CONNECTING;
  Serial.printf("wifi: connecting to %s\n", ssid.c_str());
}
void netConnect() { if (cfg.ssid.length()) netConnectTo(cfg.ssid, cfg.pass); }
bool netStartScan() {
  if (netState == NS_SCANNING) return true;
  if (!netConnected()) { WiFi.disconnect(); delay(50); }   // the SDK refuses to scan while connecting
  WiFi.scanDelete(); int r = WiFi.scanNetworks(true, false); Serial.printf("wifi: scan start %d\n", r);
  if (r == WIFI_SCAN_FAILED) { netState = NS_IDLE; netNextTry = millis() + 5000; return false; }
  netState = NS_SCANNING; return true;
}
void netBegin() {
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(false); WiFi.setSleep(true);   // reconnection is handled in netLoop
  netsLoad();
  if (cfg.ssid.length() && netFind(cfg.ssid) >= 0) netConnect();            // try the last network first, scan if it fails
}
const char *netStatusText() {
  if (netConnected()) return "Connected";
  if (!nnets) return "Not configured";
  if (netState == NS_CONNECTING) return "Connecting...";
  if (netState == NS_SCANNING) return "Searching...";
  return "Not connected";
}
// after a scan: join the strongest network we know
void netPickFromScan() {
  int best = -1, bestRssi = -1000;
  for (int i = 0; i < scanCount; i++) { if (netFind(WiFi.SSID(i)) < 0) continue; if (WiFi.RSSI(i) > bestRssi) { bestRssi = WiFi.RSSI(i); best = i; } }
  if (best >= 0) { int k = netFind(WiFi.SSID(best)); netConnectTo(nets[k].ssid, nets[k].pass); }
  else { netState = NS_IDLE; netNextTry = millis() + 20000; }
}
void netManage() {
  if (netConnected()) { netState = NS_IDLE; return; }
  if (netState == NS_SCANNING) {
    int r = WiFi.scanComplete();
    if (r >= 0) { scanCount = r; scanSeq++; Serial.printf("wifi: scan found %d\n", r); netPickFromScan(); }
    else if (r == WIFI_SCAN_FAILED) { scanCount = 0; scanSeq++; netState = NS_IDLE; netNextTry = millis() + 5000; Serial.println("wifi: scan failed"); }
  } else if (netState == NS_CONNECTING) {
    if (millis() - connectStarted > 15000 || WiFi.status() == WL_CONNECT_FAILED) { Serial.println("wifi: connect failed"); netState = NS_IDLE; netNextTry = millis() + 3000; }
  } else if (nnets && (int32_t)(millis() - netNextTry) >= 0) netStartScan();
}

// ---- clock ----
void applyTz() { configTime(cfg.tzOff, 0, "pool.ntp.org", "time.google.com"); }
bool clockValid() { time_t t = time(nullptr); return t > 1700000000; }
// 12-hour clock: "3:07" and "PM"
String clock12(String *ampm = nullptr) {
  time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm);
  int h = tm.tm_hour % 12; if (h == 0) h = 12; char b[8]; snprintf(b, sizeof b, "%d:%02d", h, tm.tm_min);
  if (ampm) *ampm = tm.tm_hour < 12 ? "AM" : "PM";
  return String(b);
}
String clockHHMM() { String ap; String t = clock12(&ap); return t + " " + ap; }

// ---- geo + weather ----
struct Day { int code; float hi, lo; int wday; };
struct Weather {
  volatile bool busy = false; bool ok = false; uint32_t fetchedAt = 0; String err, city;
  float lat = 0, lon = 0, temp = 0, wind = 0; int hum = 0, code = 0; Day days[5]; int nDays = 0;
} wx;

const char *wxText(int c) {
  if (c == 0) return "Clear"; if (c == 1) return "Mostly clear"; if (c == 2) return "Partly cloudy"; if (c == 3) return "Overcast";
  if (c == 45 || c == 48) return "Fog"; if (c >= 51 && c <= 57) return "Drizzle"; if (c >= 61 && c <= 67) return "Rain";
  if (c >= 71 && c <= 77) return "Snow"; if (c >= 80 && c <= 82) return "Showers"; if (c >= 85 && c <= 86) return "Snow showers";
  if (c >= 95) return "Thunderstorm"; return "Unknown";
}
float toUnit(float c) { return cfg.fahrenheit ? c * 9 / 5 + 32 : c; }

bool fetchGeo() {
  HTTPClient http; http.setTimeout(6000);
  if (!http.begin("http://ip-api.com/json/?fields=status,city,lat,lon")) return false;
  int code = http.GET(); if (code != 200) { wx.err = "geo http " + String(code); http.end(); return false; }
  String body = http.getString(); http.end();
  JsonDocument doc; if (deserializeJson(doc, body)) { wx.err = "geo json"; return false; }
  if (String((const char *)doc["status"]) != "success") { wx.err = "geo failed"; return false; }
  wx.lat = doc["lat"]; wx.lon = doc["lon"]; wx.city = (const char *)doc["city"];
  return true;
}
// Saved places (Settings > Location) carry their coordinates, so they never depend on the geocoder:
// "Kochi" alone resolves to Kochi, Japan there, and the Thiruvananthapuram Kumarapuram is not in it at all.
struct Place { const char *name; float lat, lon; };
const Place PLACES[] = {{"Kalamassery, Kerala", 10.0614f, 76.3263f}, {"Kochi, Kerala", 9.9399f, 76.2602f},
                        {"Kanjiramkulam, Kerala", 8.3598f, 77.0525f}, {"Kumarapuram, Kerala", 8.5241f, 76.9366f}};
static constexpr int NPLACES = sizeof(PLACES) / sizeof(PLACES[0]);
int placeIndex(const String &p) { for (int i = 0; i < NPLACES; i++) if (p.equalsIgnoreCase(PLACES[i].name)) return i; return -1; }
String placeCity(const String &p) { int c = p.indexOf(','); String q = c > 0 ? p.substring(0, c) : p; q.trim(); return q; }

// Open-Meteo geocoding for a typed place: "City, Region" - the region (after the comma) picks among same-named cities
bool fetchGeocode() {
  int pi = placeIndex(cfg.place);
  if (pi >= 0) { wx.lat = PLACES[pi].lat; wx.lon = PLACES[pi].lon; wx.city = placeCity(cfg.place); return true; }
  String q = placeCity(cfg.place), region = ""; int c = cfg.place.indexOf(','); if (c > 0) { region = cfg.place.substring(c + 1); region.trim(); region.toLowerCase(); }
  String enc; for (size_t i = 0; i < q.length(); i++) { char ch = q[i]; if (isalnum(ch)) enc += ch; else { char b[4]; snprintf(b, 4, "%%%02X", (uint8_t)ch); enc += b; } }
  WiFiClientSecure client; client.setInsecure(); HTTPClient http; http.setTimeout(8000);
  if (!http.begin(client, "https://geocoding-api.open-meteo.com/v1/search?count=5&language=en&name=" + enc)) { wx.err = "geocode begin"; return false; }
  int code = http.GET(); if (code != 200) { wx.err = "geocode http " + String(code); http.end(); return false; }
  String body = http.getString(); http.end();
  JsonDocument doc; if (deserializeJson(doc, body)) { wx.err = "geocode json"; return false; }
  JsonArray r = doc["results"]; if (r.isNull() || r.size() == 0) { wx.err = "place not found"; return false; }
  int best = 0;
  if (region.length()) for (size_t i = 0; i < r.size(); i++) {   // first hit whose state or country mentions the region
    String a1 = (const char *)(r[i]["admin1"] | ""), cc = (const char *)(r[i]["country"] | ""); a1.toLowerCase(); cc.toLowerCase();
    if (a1.indexOf(region) >= 0 || cc.indexOf(region) >= 0 || region.indexOf(a1) >= 0) { best = i; break; }
  }
  wx.lat = r[best]["latitude"]; wx.lon = r[best]["longitude"]; wx.city = (const char *)r[best]["name"];
  Serial.printf("geocode: %s -> %.4f, %.4f\n", wx.city.c_str(), wx.lat, wx.lon);
  return true;
}
bool fetchForecast() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(10000);
  char url[300];
  snprintf(url, sizeof url, "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=5", wx.lat, wx.lon);
  if (!http.begin(client, url)) { wx.err = "https begin"; return false; }
  int code = http.GET(); if (code != 200) { wx.err = "wx http " + String(code); http.end(); return false; }
  String body = http.getString(); http.end();
  JsonDocument doc; DeserializationError e = deserializeJson(doc, body);
  if (e) { wx.err = String("wx json: ") + e.c_str(); Serial.printf("wx body (%d): %.200s\n", body.length(), body.c_str()); return false; }
  long off = doc["utc_offset_seconds"] | 0L; Serial.printf("wx: tz offset %ld (%s)\n", off, (const char *)(doc["timezone"] | "?"));
  if (off != cfg.tzOff) { cfg.tzOff = off; cfgSave(); applyTz(); }
  wx.temp = doc["current"]["temperature_2m"]; wx.hum = doc["current"]["relative_humidity_2m"];
  wx.code = doc["current"]["weather_code"]; wx.wind = doc["current"]["wind_speed_10m"];
  JsonArray t = doc["daily"]["time"], wc = doc["daily"]["weather_code"], hi = doc["daily"]["temperature_2m_max"], lo = doc["daily"]["temperature_2m_min"];
  wx.nDays = 0;
  for (size_t i = 0; i < t.size() && wx.nDays < 5; i++) {
    struct tm tm = {}; int y, m, d; sscanf((const char *)t[i], "%d-%d-%d", &y, &m, &d);
    tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d; tm.tm_hour = 12; mktime(&tm);
    wx.days[wx.nDays++] = {(int)wc[i], (float)hi[i], (float)lo[i], tm.tm_wday};
  }
  return true;
}
void wxTask(void *) {
  xSemaphoreTake(netMutex, portMAX_DELAY);
  wx.err = "";
  bool ok = (wx.city.length() || (cfg.place.length() ? fetchGeocode() : fetchGeo())) && fetchForecast();
  if (ok) { wx.ok = true; wx.fetchedAt = millis(); Serial.printf("wx: %s %.1fC code %d\n", wx.city.c_str(), wx.temp, wx.code); }
  else Serial.printf("wx: failed: %s\n", wx.err.c_str());
  wx.busy = false; xSemaphoreGive(netMutex); vTaskDelete(nullptr);
}
void wxInvalidate() { wx.city = ""; wx.ok = false; }
bool spBusyNow(); // spotify.h
void wxRequest() {
  if (wx.busy || !netConnected()) return;
  wx.busy = true;
  xTaskCreatePinnedToCore(wxTask, "wx", 12288, nullptr, 1, nullptr, 0);
}
void netLoop() {
  netManage();
  bool c = netConnected();
  if (c && !wasConnected) { Serial.printf("wifi: connected, ip %s\n", WiFi.localIP().toString().c_str()); applyTz(); wxRequest(); }
  if (!c && wasConnected) { Serial.println("wifi: dropped"); netNextTry = 0; }
  wasConnected = c;
  if (c && wx.ok && millis() - wx.fetchedAt > 15 * 60 * 1000UL) wxRequest();
}
