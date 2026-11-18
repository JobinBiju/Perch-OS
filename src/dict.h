#pragma once
// Dictionary lookup from StarDict files, the common offline format: name.ifo, name.idx and name.dict in a "dict" folder on
// the book volume (the SD card, or flash without one). A compressed name.dict.dz must be unpacked first. The .idx is a
// sorted list of "word\0, offset (4 bytes, big-endian), size (4)"; at the first lookup it is read once to keep every 32nd
// entry's place, so a lookup is a binary search over those and a short scan. A word not found as written is tried in its
// base forms: "running" -> "run", "studies" -> "study", "walked" -> "walk". Ported from the e-paper Perch.
#include <string>
#include <vector>
#include <strings.h>
#include "storage.h"

namespace dict {

struct Dict { fs::FS *fs = nullptr; String name; char types[8] = ""; File idx, dat; std::vector<uint32_t> marks; } d;
inline bool ok() { return d.idx && d.dat; }
inline void close() { if (d.idx) d.idx.close(); if (d.dat) d.dat.close(); d.marks.clear(); d.fs = nullptr; d.name = ""; }
static uint8_t buf[4096];
// one .idx entry out of a buffer; false when it does not hold the whole entry
inline bool parseEntry(const uint8_t *b, int n, int &at, std::string &w, uint32_t &off, uint32_t &size) {
  int e = at; while (e < n && b[e]) e++; if (e + 9 > n) return false;
  w.assign((const char *)b + at, e - at); const uint8_t *q = b + e + 1;
  off = (uint32_t)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3]; size = (uint32_t)q[4] << 24 | q[5] << 16 | q[6] << 8 | q[7]; at = e + 9; return true;
}
// open the first dictionary in /dict of the book volume; false when there is none
inline bool open() {
  if (ok() && d.fs == bookFS) return true;
  close(); if (!bookFS) return false;
  File dir = bookFS->open("/dict"); if (!dir || !dir.isDirectory()) return false;
  String ifo;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) { String n = f.name(); int sl = n.lastIndexOf('/'); if (sl >= 0) n = n.substring(sl + 1); if (n.endsWith(".ifo") && n[0] != '.' && (!ifo.length() || n < ifo)) ifo = n; f.close(); }
  dir.close(); if (!ifo.length()) return false;
  String base = "/dict/" + ifo.substring(0, ifo.length() - 4);
  File f = bookFS->open(base + ".ifo"); if (!f) return false;
  d.types[0] = 0;
  while (f.available()) { String s = f.readStringUntil('\n'); s.trim(); if (s.startsWith("bookname=")) d.name = s.substring(9); else if (s.startsWith("sametypesequence=")) strlcpy(d.types, s.c_str() + 17, sizeof d.types); }
  f.close();
  d.idx = bookFS->open(base + ".idx"); d.dat = bookFS->open(base + ".dict");
  if (!ok()) { close(); return false; }
  d.fs = bookFS; uint32_t t0 = millis(), pos = 0; int n = 0, have = 0, count = 0; std::string w; uint32_t o, sz;
  for (;;) {   // walk the whole index once, a block at a time
    int r = d.idx.read(buf + have, sizeof buf - have); if (r > 0) have += r;
    int at = 0, last = 0;
    while (parseEntry(buf, have, at, w, o, sz)) { if (count++ % 32 == 0) d.marks.push_back(pos + last); last = at; }
    if (r <= 0) break;
    memmove(buf, buf + last, have - last); have -= last; pos += last; n++;
  }
  Serial.printf("dict: %s, %d words indexed in %lu ms\n", d.name.c_str(), count, (unsigned long)(millis() - t0));
  return count > 0;
}
// the raw entry for exactly this word (case-insensitive), or false
inline bool find(const std::string &word, std::string &raw, std::string &head) {
  if (!open() || d.marks.empty()) return false;
  int lo = 0, hi = (int)d.marks.size() - 1; std::string w; uint32_t off, size;
  auto first = [&](int m) { d.idx.seek(d.marks[m]); int n = d.idx.read(buf, 300), at = 0; return n > 0 && parseEntry(buf, n, at, w, off, size); };
  while (lo < hi) {   // the last block whose first word is not after `word`
    int mid = (lo + hi + 1) / 2; if (!first(mid)) return false;
    if (strcasecmp(w.c_str(), word.c_str()) < 0) lo = mid; else hi = mid - 1;
  }
  d.idx.seek(d.marks[lo]); int n = d.idx.read(buf, sizeof buf), at = 0;
  for (int i = 0; i < 96 && parseEntry(buf, n, at, w, off, size); i++) {   // this block and the next: equal words may straddle
    int c = strcasecmp(w.c_str(), word.c_str());
    if (c > 0) break;
    if (c == 0) {
      if (size > 16000) size = 16000;
      raw.resize(size); d.dat.seek(off); if (d.dat.read((uint8_t *)&raw[0], size) != (int)size) return false;
      head = w; return true;
    }
  }
  return false;
}
// a definition as plain text: HTML and XDXF tags dropped, line breaks kept, common entities decoded
inline std::string plainDef(const std::string &raw, char type) {
  if (type != 'h' && type != 'g' && type != 'x') return raw;
  std::string o; bool bol = true;
  for (size_t i = 0; i < raw.size(); i++) {
    char c = raw[i];
    if (c == '<') {
      size_t e = raw.find('>', i); if (e == std::string::npos) break;
      std::string tag = raw.substr(i + 1, e - i - 1); for (auto &ch : tag) ch = tolower(ch);
      if (!tag.compare(0, 2, "br") || tag == "p" || tag == "/p" || !tag.compare(0, 3, "div") || tag == "/div" || tag == "li" || tag == "/k" || tag == "/li") { if (!bol) { o += '\n'; bol = true; } }
      i = e; continue;
    }
    if (c == '&') {
      static const char *const E[][2] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&#39;", "'"}, {"&nbsp;", " "}};
      bool hit = false; for (auto &e : E) if (!raw.compare(i, strlen(e[0]), e[0])) { o += e[1]; i += strlen(e[0]) - 1; hit = true; break; }
      if (hit) { bol = false; continue; }
    }
    if (c == '\n') { if (!bol) { o += '\n'; bol = true; } continue; }
    if (bol && c == ' ') continue;
    o += c; bol = false;
  }
  while (!o.empty() && (o.back() == '\n' || o.back() == ' ')) o.pop_back();
  return o;
}
// the forms of a word to try, as written first, then base forms by the common English endings
inline std::vector<std::string> forms(std::string w) {
  std::vector<std::string> out;
  auto strip = [](std::string &s) {   // what a selection carries round a word: punctuation, quotes
    while (!s.empty() && (ispunct((unsigned char)s.back()) || (unsigned char)s.back() >= 0x80)) {
      if ((unsigned char)s.back() >= 0x80) { size_t k = s.size() - 1; while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) k--; s.erase(k); } else s.pop_back();
    }
    while (!s.empty() && (ispunct((unsigned char)s[0]) || (unsigned char)s[0] >= 0x80)) { size_t k = 1; if ((unsigned char)s[0] >= 0x80) while (k < s.size() && ((unsigned char)s[k] & 0xC0) == 0x80) k++; s.erase(0, k); }
  };
  strip(w);
  for (const char *pos : {"'s", "\xe2\x80\x99s"}) { size_t n = strlen(pos); if (w.size() > n && !w.compare(w.size() - n, n, pos)) w.erase(w.size() - n); }
  if (w.empty()) return out;
  std::string l = w; for (auto &c : l) c = tolower(c);
  auto add = [&](const std::string &s) { if (s.empty()) return; for (auto &o : out) if (o == s) return; out.push_back(s); };
  auto ends = [&](const char *e) { size_t n = strlen(e); return l.size() > n + 1 && !l.compare(l.size() - n, n, e); };
  auto cut = [&](size_t n) { return l.substr(0, l.size() - n); };
  auto vowel = [](char c) { return strchr("aeiou", c) != nullptr; };
  auto undouble = [&](const std::string &s) { if (s.size() > 2 && s[s.size() - 1] == s[s.size() - 2] && !vowel(s.back())) add(s.substr(0, s.size() - 1)); };
  add(w); add(l);
  if (ends("ies")) add(cut(3) + "y");
  if (ends("es")) add(cut(2));
  if (ends("s") && !ends("ss")) add(cut(1));
  if (ends("ied")) add(cut(3) + "y");
  if (ends("ed")) { std::string s = cut(2); add(s); add(s + "e"); undouble(s); }
  if (ends("ing")) { std::string s = cut(3); add(s); add(s + "e"); undouble(s); }
  if (ends("ier")) add(cut(3) + "y");
  if (ends("iest")) add(cut(4) + "y");
  if (ends("er")) { add(cut(2)); add(cut(1)); }
  if (ends("est")) { add(cut(3)); add(cut(2)); }
  if (ends("ily")) add(cut(3) + "y");
  if (ends("ly")) add(cut(2));
  return out;
}
struct Result { bool found = false, haveDict = false; std::string head, text; };
// look a word up as written, then in its base forms
inline Result lookup(const std::string &word) {
  Result r; r.haveDict = open(); if (!r.haveDict) return r;
  for (auto &f : forms(word)) {
    std::string raw, head;
    if (find(f, raw, head)) { r.found = true; r.head = head; r.text = plainDef(raw, d.types[0] ? d.types[0] : 'm'); return r; }
  }
  r.head = word; return r;
}

}  // namespace dict
