#pragma once
// Book storage: the microSD card (SDMMC, 1-bit: D1-D3 do not respond on this board) when one is inserted at boot, otherwise a LittleFS
// partition on the 16 MB flash (about 3.3 MB, enough for a few books). Books live in /books as the
// plain-text files book_convert.py / the web portal produce.
#include <FS.h>
#include <SD_MMC.h>
#include <LittleFS.h>

static constexpr int SD_CLK = 38, SD_CMD = 40, SD_D0 = 39, SD_D1 = 41, SD_D2 = 48, SD_D3 = 47;
static const char *BOOK_DIR = "/books";

fs::FS *bookFS = nullptr; bool storageSD = false, flashUp = false; uint32_t storageTried = 0;
int storageSDBooks = -1;   // books copied off flash at the last mount, -1 = card never mounted

// books uploaded while the board ran on flash move to the card the first time it mounts (copy, verify size, delete)
void storageMigrate() {
  if (!LittleFS.begin(false)) return;
  File d = LittleFS.open(BOOK_DIR); int n = 0;
  if (d) for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String name = f.name(); int sl = name.lastIndexOf('/'); if (sl >= 0) name = name.substring(sl + 1);
    if (f.isDirectory() || !(name.endsWith(".txt") || name.endsWith(".cov"))) continue;
    String dst = String(BOOK_DIR) + "/" + name; size_t len = f.size(), done = 0;
    File o = SD_MMC.open(dst + ".part", FILE_WRITE); if (!o) continue;
    static uint8_t buf[4096];
    while (done < len) { size_t r = f.read(buf, sizeof buf); if (!r || o.write(buf, r) != r) break; done += r; }
    o.close(); f.close();
    if (done == len && (SD_MMC.remove(dst), SD_MMC.rename(dst + ".part", dst))) { LittleFS.remove(String(BOOK_DIR) + "/" + name); n++; }
    else SD_MMC.remove(dst + ".part");
    Serial.printf("sd: moved %s (%u bytes)%s\n", name.c_str(), (unsigned)len, done == len ? "" : " FAILED");
  }
  LittleFS.end(); flashUp = false;
  storageSDBooks = n; if (n) Serial.printf("sd: %d book(s) moved from flash\n", n);
}
bool storageMountSD() {
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
  uint32_t t0 = millis(); bool ok = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5);
  Serial.printf("sd: %s (%lu ms)%s\n", ok ? "mounted" : "no card", (unsigned long)(millis() - t0), ok ? (", " + String((uint32_t)(SD_MMC.cardSize() >> 20)) + " MB").c_str() : "");
  if (!ok) { SD_MMC.end(); return false; }
  bookFS = &SD_MMC; storageSD = true;
  if (!SD_MMC.exists(BOOK_DIR)) SD_MMC.mkdir(BOOK_DIR);
  storageMigrate(); return true;
}
void storageBegin() {
  storageTried = millis();
  if (storageMountSD()) return; else if (LittleFS.begin(true)) { bookFS = &LittleFS; flashUp = true; Serial.printf("storage: flash, %lu KB free\n", (unsigned long)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024)); }
  else { Serial.println("storage: none"); return; }
  if (!bookFS->exists(BOOK_DIR)) bookFS->mkdir(BOOK_DIR);
}
// a card inserted after boot: try again (cheap when nothing is in the slot), at most every 10 s
void storageRetrySD() {
  if (storageSD || millis() - storageTried < 10000) return;
  storageTried = millis();
  storageMountSD();
}
// flash partition usage, mounting it briefly when the card is the active storage
bool storageFlash(uint64_t &total, uint64_t &used) {
  bool tmp = !flashUp; if (tmp && !LittleFS.begin(false)) return false;
  total = LittleFS.totalBytes(); used = LittleFS.usedBytes(); if (tmp) LittleFS.end(); return true;
}
String bytesText(uint64_t b) { return b >= (1ULL << 30) ? String(b / 1073741824.0f, 1) + " GB" : b >= (1ULL << 20) ? String(b / 1048576.0f, 1) + " MB" : String((uint32_t)(b / 1024)) + " KB"; }
const char *storageName() { return !bookFS ? "none" : storageSD ? "SD card" : "flash"; }
uint64_t storageTotal() { return !bookFS ? 0 : storageSD ? SD_MMC.totalBytes() : LittleFS.totalBytes(); }
uint64_t storageUsed() { return !bookFS ? 0 : storageSD ? SD_MMC.usedBytes() : LittleFS.usedBytes(); }
String storageText() {   // "SD card, 28.1 GB free" / "flash, 2.9 MB free"
  if (!bookFS) return "no storage";
  return String(storageName()) + ", " + bytesText(storageTotal() - storageUsed()) + " free";
}
