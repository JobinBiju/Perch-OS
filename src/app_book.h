#pragma once
// Book reader: UTF-8 text books (from book_convert.py or the web portal) on the SD card / flash,
// plus one embedded in the firmware. A book is loaded whole into PSRAM and each chapter is laid out with
// best-fit line breaks, soft-hyphen breaks and justification (see the layout engine below).
// Tap right / swipe left for the next page, left for the previous one, centre for the menu.
#include "ui.h"
#include "net.h"
#include "storage.h"

extern const char bookStart[] asm("_binary_data_book_txt_start");
extern const char bookEnd[] asm("_binary_data_book_txt_end");

struct Book {
  String title, author, file;              // file: "" for the embedded book
  const char *data = nullptr, *dataEnd = nullptr; char *buf = nullptr;   // buf: PSRAM copy of a file book
  static constexpr int MAXCH = 256;
  const char *chTitle[MAXCH]; int chTitleLen[MAXCH]; const char *chText[MAXCH]; const char *chEnd[MAXCH]; int nch = 0;
  // current chapter layout
  int ch = -1, fontIdx = 1, lineSp = 1 /* 0 tight, 1 normal, 2 loose */; int *pages = nullptr; int npages = 0, page = 0; uint32_t stamp = 0;   // pages: first line index of each page; stamp: open order, for the library sort
  // layout geometry
  int textX = MARGIN, textY = 30, textW = W - 2 * MARGIN, lineH = 22, lines = 8, lines0 = 6, headH = 0;   // lines0: first page, below the chapter heading
} book;

struct LibEntry { String file, title, author; uint32_t size; int pct; uint32_t last; };   // last: open order (0 = never opened)
static constexpr int MAXLIB = 40; LibEntry lib[MAXLIB]; int nlib = 0;

// reader faces by text size (0..2) and style bits (1 italic, 2 bold)
const lgfx::IFont *bookFont(int i, int style = 0) {
  static const lgfx::IFont *const faces[3][4] = {{RD12, RD12I, RD12B, RD12Z}, {RD16, RD16I, RD16B, RD16Z}, {RD20, RD20I, RD20B, RD20Z}};
  return faces[constrain(i, 0, 2)][style & 3];
}

// parse the "\x02Title\nAuthor\n" header; returns the byte after it
const char *bookHeader(const char *p, const char *e, String &title, String &author) {
  title = ""; author = "";
  if (p < e && *p == 2) {
    p++; const char *nl = (const char *)memchr(p, '\n', e - p); if (!nl) return e; title = String(p, nl - p); p = nl + 1;
    nl = (const char *)memchr(p, '\n', e - p); if (!nl) return e; author = String(p, nl - p); p = nl + 1;
  }
  return p;
}
void bookIndex() {
  book.nch = 0;
  const char *p = bookHeader(book.data, book.dataEnd, book.title, book.author), *e = book.dataEnd;
  while (p < e && book.nch < Book::MAXCH) {
    if (*p != 1) { p++; continue; }
    const char *nl = (const char *)memchr(p + 1, '\n', e - p - 1); if (!nl) break;
    int i = book.nch++; book.chTitle[i] = p + 1; book.chTitleLen[i] = nl - p - 1; book.chText[i] = nl + 1;
    const char *q = (const char *)memchr(nl + 1, 1, e - nl - 1); book.chEnd[i] = q ? q : e; p = book.chEnd[i];
  }
}

// per-book progress lives in NVS namespace "books": key = hash of the file name -> "chapter offset"
String bookKey(const String &file) { uint32_t h = 2166136261u; for (char c : file) { h ^= (uint8_t)c; h *= 16777619u; } char k[12]; snprintf(k, sizeof k, "b%08lx", (unsigned long)h); return k; }
// progress value: "chapter offset stamp rel len". offset is absolute (the library's percent); rel/len are the offset
// within the chapter and the chapter's length when saved, so a re-converted book (more bytes: hyphens, accents)
// reopens at the same place by scaling rel. stamp counts book openings, so the library can sort by last read.
bool bookProgress(const String &file, int &ch, int &off, uint32_t *stamp = nullptr, int *rel = nullptr, int *len = nullptr) {
  prefs.begin("books", true); String v = prefs.getString(bookKey(file).c_str(), ""); prefs.end(); if (!v.length()) return false;
  long f[5] = {0, 0, 0, -1, 0}; int n = 0; const char *q = v.c_str();
  while (n < 5 && *q) { char *e; f[n++] = strtol(q, &e, 10); q = e; while (*q == ' ') q++; }
  ch = f[0]; off = f[1]; if (stamp) *stamp = f[2]; if (rel) *rel = f[3]; if (len) *len = f[4];
  return true;
}
uint32_t bookNextStamp() { prefs.begin("books", false); uint32_t n = prefs.getUInt("seq", 0) + 1; prefs.putUInt("seq", n); prefs.end(); return n; }

// ---- layout engine ----
// Text is UTF-8. A soft hyphen (U+00AD, bytes C2 AD) marks where a word may break; it is never drawn.
// \x03 toggles italic and \x04 toggles bold (set by the converters for <em> and <strong>); both are paragraph-local.
// Each paragraph is split into fragments (words, or word parts at soft hyphens) and the line breaks are chosen
// by a minimum-raggedness dynamic program (as CrossPoint Reader does): the sum over the paragraph's lines of
// the squared spare space is minimised, the last line costs nothing and a hyphenated break pays HYPH_PEN.
// Lines are then justified at draw time by spreading the spare space over the word gaps.
static int16_t advCache[3][4][0x180 + 0x100];   // glyph advances per text size and style: codepoints < U+0180 and U+2000..U+20FF
static inline int utf8Next(const char *&p, const char *e) {
  uint8_t c = (uint8_t)*p++; if (c < 0x80) return c;
  int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1, cp = c & (0x3F >> n);
  while (n-- > 0 && p < e) cp = (cp << 6) | ((uint8_t)*p++ & 0x3F);
  return cp;
}
int glyphAdv(int cp, int style = 0) {
  int slot = cp < 0x180 ? cp : (cp >= 0x2000 && cp < 0x2100) ? 0x180 + (cp - 0x2000) : -1;
  int16_t *c = slot >= 0 ? &advCache[book.fontIdx][style & 3][slot] : nullptr; if (c && *c) return *c;
  char u[5]; int n = 0;
  if (cp < 0x80) u[n++] = cp; else if (cp < 0x800) { u[n++] = 0xC0 | (cp >> 6); u[n++] = 0x80 | (cp & 0x3F); }
  else { u[n++] = 0xE0 | (cp >> 12); u[n++] = 0x80 | ((cp >> 6) & 0x3F); u[n++] = 0x80 | (cp & 0x3F); }
  u[n] = 0; char uu[9]; memcpy(uu, u, n); memcpy(uu + n, u, n + 1);
  frame.setFont(bookFont(book.fontIdx, style));
  int w = frame.textWidth(uu) - frame.textWidth(u);   // textWidth of one glyph is its ink extent (italics overhang): "cc" - "c" is the advance
  if (c) *c = w; return w;
}
int runWidth(const char *p, const char *e, int style = 0) { int w = 0; while (p < e) { int cp = utf8Next(p, e); if (cp != 0xAD) w += glyphAdv(cp, style); } return w; }
static inline bool isToggle(char c) { return c == 3 || c == 4; }
int stripShy(const char *p, const char *e, char *out, int cap) {   // copy [p, e) without soft hyphens; returns the length
  int n = 0; while (p < e && n < cap - 1) { if ((uint8_t)p[0] == 0xC2 && p + 1 < e && (uint8_t)p[1] == 0xAD) { p += 2; continue; } out[n++] = *p++; } out[n] = 0; return n;
}

struct Frag { const char *s, *e; int16_t w; bool word, hyph; uint8_t style; };   // word: a space precedes it; hyph: a soft hyphen follows it
struct Line { const char *s, *e; bool hyph, last, first; uint8_t style; };       // hyph: draw a '-' at the end; first/last line of its paragraph; style at the line start
static constexpr int INDENT = 18;                                   // first-line indent of a paragraph
static Frag *frags = nullptr; static int nfrags = 0, fragCap = 0;
static Line *lines = nullptr; static int nlines = 0, lineCap = 0;
static int64_t *lcost = nullptr; static int *lnext = nullptr; static int costCap = 0, nextCap = 0;
template <class T> void grow(T *&a, int &cap, int need) { if (need <= cap) return; cap = max(need, cap ? cap * 2 : 256); a = (T *)ps_realloc(a, cap * sizeof(T)); }
void pushFrag(const char *s, const char *e, bool word, bool hyph, uint8_t style, int maxw) {
  int w = runWidth(s, e, style);
  while (w > maxw && e - s > 1) {   // a word wider than the line: cut it at a codepoint boundary
    const char *q = s, *fit = s + 1; int ww = 0;
    while (q < e) { const char *n = q; int cp = utf8Next(n, e); int a = cp == 0xAD ? 0 : glyphAdv(cp, style); if (ww + a > maxw && q > s) break; ww += a; q = n; fit = q; }
    grow(frags, fragCap, nfrags + 1); frags[nfrags++] = {s, fit, (int16_t)ww, word, false, style}; word = false; s = fit; w = runWidth(s, e, style);
  }
  grow(frags, fragCap, nfrags + 1); frags[nfrags++] = {s, e, (int16_t)w, word, hyph, style};
}
// split a paragraph into fragments at spaces, soft hyphens and style toggles
void bookFragments(const char *p, const char *pe, int maxw) {
  nfrags = 0; bool word = true; uint8_t style = 0; const char *s = p;
  auto flush = [&](const char *e, bool hy) { if (e > s) { pushFrag(s, e, word, hy, style, maxw); word = false; } };
  while (p < pe) {
    uint8_t c = (uint8_t)*p;
    if (c == ' ') { flush(p, false); p++; s = p; word = true; }
    else if (isToggle(c)) { flush(p, false); style ^= (c == 3 ? 1 : 2); p++; s = p; }
    else if (c == 0xC2 && p + 1 < pe && (uint8_t)p[1] == 0xAD) { flush(p, true); p += 2; s = p; }
    else p++;
  }
  flush(pe, false);
}
void bookBreakParagraph(int maxw, int spaceW, int hyphW) {
  int n = nfrags; if (!n) return;
  const int64_t HYPH_PEN = (int64_t)(maxw / 7) * (maxw / 7);   // a hyphen is worth about 42 px of raggedness on a 296 px line
  grow(lcost, costCap, n + 1); grow(lnext, nextCap, n + 1); int *nxt = lnext;
  lcost[n] = 0;
  for (int i = n - 1; i >= 0; i--) {
    int64_t best = INT64_MAX; int bestJ = i, w = 0, lim = i == 0 ? maxw - INDENT : maxw;   // the first line is indented
    for (int j = i; j < n; j++) {
      if (j > i && frags[j].word) w += spaceW;
      w += frags[j].w;
      bool wordEnd = j == n - 1 || frags[j + 1].word, hy = !wordEnd && frags[j].hyph;
      int lw = w + (hy ? hyphW : 0);
      if (lw > lim) { if (best == INT64_MAX) { best = lcost[j + 1] + 1000000; bestJ = j; } break; }
      if (!wordEnd && !hy) continue;
      int slack = lim - lw; int64_t c = (j == n - 1 ? 0 : (int64_t)slack * slack) + lcost[j + 1];
      if (hy) c += nxt[j + 1] == n - 1 ? 3 * HYPH_PEN : HYPH_PEN;   // like TeX, a hyphen right before the last line costs extra
      if (c < best) { best = c; bestJ = j; }
    }
    lcost[i] = best; nxt[i] = bestJ;
  }
  for (int i = 0; i < n; i = nxt[i] + 1) {
    int j = nxt[i]; bool wordEnd = j == n - 1 || frags[j + 1].word;
    grow(lines, lineCap, nlines + 1); lines[nlines++] = {frags[i].s, frags[j].e, !wordEnd, j == n - 1, i == 0, frags[i].style};
  }
}

// chapter heading split: "2  The Anthropocene" -> number 2, title "The Anthropocene"; no number -> num = 0
int bookChapterNum(int ch, const char *&title, int &len) {
  const char *p = book.chTitle[ch]; len = book.chTitleLen[ch]; title = p; int n = 0, i = 0;
  while (i < len && p[i] >= '0' && p[i] <= '9') { n = n * 10 + (p[i] - '0'); i++; }
  if (i && i < len && p[i] == ' ') { while (i < len && p[i] == ' ') i++; title = p + i; len -= i; return n; }
  return 0;
}
void bookLayoutGeometry() {
  frame.setFont(bookFont(book.fontIdx));
  static const int gap[3][3] = {{1, 4, 10}, {3, 6, 12}, {3, 6, 12}};   // [text size][line spacing] px between lines
  book.lineH = frame.fontHeight() + gap[book.fontIdx][book.lineSp];
  book.textY = 30; book.lines = (218 - book.textY) / book.lineH;
  const char *t; int l; book.headH = book.ch < 0 ? 0 : (bookChapterNum(book.ch, t, l) ? 20 : 0) + 24 + 8;   // "CHAPTER n" line, RD20 title, 8 px gap
  book.lines0 = (218 - book.textY - book.headH) / book.lineH;
}
// lay out the current chapter: lines[] for the whole chapter, pages[] = index of each page's first line
void bookPaginate() {
  bookLayoutGeometry();
  if (book.pages) { free(book.pages); book.pages = nullptr; }
  book.npages = 0; nlines = 0; if (book.ch < 0) return;
  uint32_t t0 = millis();
  int spaceW = glyphAdv(' '), hyphW = glyphAdv('-');
  const char *p = book.chText[book.ch], *e = book.chEnd[book.ch];
  while (p < e) {
    const char *nl = (const char *)memchr(p, '\n', e - p), *pe = nl ? nl : e;
    if (pe > p) { bookFragments(p, pe, book.textW); bookBreakParagraph(book.textW, spaceW, hyphW); }
    p = pe + 1;
  }
  int cap = 64; book.pages = (int *)ps_malloc(cap * sizeof(int));
  for (int l = 0; l < nlines; l += book.npages == 1 ? book.lines0 : book.lines) {
    if (book.npages == cap) { cap *= 2; book.pages = (int *)ps_realloc(book.pages, cap * sizeof(int)); }
    book.pages[book.npages++] = l;
  }
  if (!book.npages) { book.pages[0] = 0; book.npages = 1; }
  Serial.printf("book: chapter %d laid out, %d lines, %d pages in %lu ms\n", book.ch, nlines, book.npages, (unsigned long)(millis() - t0));
}
const char *bookPageStart(int page) { return nlines ? lines[book.pages[page]].s : book.chText[book.ch]; }
int bookPageFor(const char *off) { int i = 0; while (i + 1 < book.npages && bookPageStart(i + 1) <= off) i++; return i; }
void bookSave() {
  if (book.ch < 0) return;
  prefs.begin("books", false);
  const char *ps = bookPageStart(book.page);
  prefs.putString(bookKey(book.file).c_str(), String(book.ch) + " " + String((int)(ps - book.data)) + " " + String(book.stamp) + " " +
                  String((int)(ps - book.chText[book.ch])) + " " + String((int)(book.chEnd[book.ch] - book.chText[book.ch])));
  prefs.putString("last", book.file.length() ? book.file : "*"); prefs.putInt("font", book.fontIdx); prefs.putInt("lsp", book.lineSp); prefs.end();
}
void bookOpen(int ch, const char *off = nullptr) {
  book.ch = constrain(ch, 0, book.nch - 1); bookPaginate(); book.page = off ? bookPageFor(off) : 0;
}
// load a book (file name under /books, or "" for the embedded one) and restore its position
bool bookLoad(const String &file) {
  if (book.buf) { free(book.buf); book.buf = nullptr; }
  book.ch = -1; book.nch = 0; book.file = file;
  if (!file.length()) { book.data = bookStart; book.dataEnd = bookEnd; }
  else {
    if (!bookFS) return false;
    File f = bookFS->open(String(BOOK_DIR) + "/" + file); if (!f || f.isDirectory()) return false;
    size_t n = f.size(); book.buf = (char *)ps_malloc(n + 1); if (!book.buf) { f.close(); return false; }
    size_t got = 0; uint32_t t0 = millis(); while (got < n) { int r = f.read((uint8_t *)book.buf + got, min((size_t)16384, n - got)); if (r <= 0) break; got += r; }
    f.close(); book.buf[got] = 0; book.data = book.buf; book.dataEnd = book.buf + got;
    Serial.printf("book: loaded %s, %u bytes in %lu ms\n", file.c_str(), (unsigned)got, (unsigned long)(millis() - t0));
  }
  bookIndex(); if (!book.nch) return false;
  int ch = 0, off = -1, rel = -1, len = 0; bookProgress(file, ch, off, &book.stamp, &rel, &len);
  ch = constrain(ch, 0, book.nch - 1); const char *pos = nullptr;
  if (rel >= 0 && len > 0) { int now = book.chEnd[ch] - book.chText[ch]; pos = book.chText[ch] + min((long long)now, (long long)rel * now / len); }   // chapter-relative, scaled to the current text
  else if (off >= 0 && book.data + off < book.dataEnd) pos = book.data + off;   // older saves: absolute offset
  bookOpen(ch, pos);
  return true;
}
void bookBegin() { prefs.begin("books", true); book.fontIdx = prefs.getInt("font", 1); book.lineSp = constrain(prefs.getInt("lsp", 1), 0, 2); prefs.end(); }
void bookRelayout() { const char *off = book.pages ? bookPageStart(book.page) : nullptr; bookPaginate(); book.page = off ? bookPageFor(off) : 0; bookSave(); }   // same text stays on screen
void bookSetFont(int f) { book.fontIdx = constrain(f, 0, 2); bookRelayout(); }
void bookSetLineSp(int l) { book.lineSp = constrain(l, 0, 2); bookRelayout(); }
int bookPercent() { if (book.ch < 0) return 0; return (int)((bookPageStart(book.page) - book.data) * 100LL / (book.dataEnd - book.data)); }

// ---- library: the embedded book plus every .txt under /books ----
int libPercent(const String &file, uint32_t size, uint32_t *last = nullptr) { int ch, off; if (!bookProgress(file, ch, off, last) || !size) return -1; return constrain((int)(off * 100LL / size), 0, 100); }   // -1: never opened
// The book built into the firmware moves to the card (or flash) once, as a portal-style file that can be deleted there;
// its reading position moves with it. Afterwards the embedded copy is no longer listed.
bool embMoved() { prefs.begin("books", true); bool m = prefs.getBool("emb", false); prefs.end(); return m; }
void bookMoveEmbedded() {
  if (!bookFS || bookEnd <= bookStart) return;
  String title, author; bookHeader(bookStart, bookEnd, title, author);
  String name; for (char c : title) { if (isalnum((uint8_t)c)) name += (char)tolower(c); else if (!name.endsWith("-")) name += '-'; } while (name.endsWith("-")) name.remove(name.length() - 1); name = name.substring(0, 40) + ".txt";
  String path = String(BOOK_DIR) + "/" + name;
  bool have = bookFS->exists(path), good = false;
  if (have) { File f = bookFS->open(path); uint8_t b = 0; good = f && f.read(&b, 1) == 1 && b == 2; f.close(); }
  if (embMoved() && (good || !have)) return;   // done earlier; a missing file was deleted on purpose
  if (!good) {
    File o = bookFS->open(path + ".part", FILE_WRITE); if (!o) return;
    static uint8_t buf[4096];   // the SD driver's DMA cannot read memory-mapped flash: stage each chunk in RAM
    size_t len = bookEnd - bookStart, done = 0;
    while (done < len) { size_t n = min(sizeof buf, len - done); memcpy(buf, bookStart + done, n); if (o.write(buf, n) != n) break; done += n; }
    o.close(); if (done != len) { bookFS->remove(path + ".part"); return; }
    if (have) bookFS->remove(path);
    bookFS->rename(path + ".part", path);
  }
  prefs.begin("books", false);
  String prog = prefs.getString(bookKey("").c_str(), ""); if (prog.length()) { prefs.putString(bookKey(name).c_str(), prog); prefs.remove(bookKey("").c_str()); }
  if (prefs.getString("last", "") == "*") prefs.putString("last", name);
  prefs.putBool("emb", true); prefs.end();
  if (!book.file.length() && book.ch >= 0) book.file = name;   // currently open: keep saving under the new name
  Serial.printf("book: embedded book moved to %s\n", path.c_str());
}
void libScan() {
  nlib = 0; bookMoveEmbedded();
  if (bookEnd > bookStart && !embMoved()) { LibEntry &e = lib[nlib++]; e.file = ""; bookHeader(bookStart, bookEnd, e.title, e.author); e.size = bookEnd - bookStart; e.pct = libPercent("", e.size, &e.last); }
  if (!bookFS) return;
  File d = bookFS->open(BOOK_DIR); if (!d) return;
  for (File f = d.openNextFile(); f && nlib < MAXLIB; f = d.openNextFile()) {
    String n = f.name(); int sl = n.lastIndexOf('/'); if (sl >= 0) n = n.substring(sl + 1);
    if (f.isDirectory() || !n.endsWith(".txt")) { f.close(); continue; }
    char h[200]; int r = f.read((uint8_t *)h, sizeof h - 1); if (r < 0) r = 0; h[r] = 0;
    LibEntry &e = lib[nlib++]; e.file = n; bookHeader(h, h + r, e.title, e.author); if (!e.title.length()) e.title = n; e.size = f.size(); e.pct = libPercent(n, e.size, &e.last);
    f.close();
  }
  d.close();
  // most recently opened first, then unread books alphabetically
  auto before = [](const LibEntry &a, const LibEntry &b) { return a.last != b.last ? a.last > b.last : a.title.compareTo(b.title) < 0; };
  for (int i = 1; i < nlib; i++) for (int j = i; j > 0 && before(lib[j], lib[j - 1]); j--) { LibEntry t = lib[j]; lib[j] = lib[j - 1]; lib[j - 1] = t; }
}
bool libHas(const String &file) { for (int i = 0; i < nlib; i++) if (lib[i].file == file) return true; return false; }
String libLast() { prefs.begin("books", true); String l = prefs.getString("last", ""); prefs.end(); return l == "*" ? String("") : l; }
bool bookDelete(const String &file) {
  if (!bookFS || !file.length()) return false;
  if (book.file == file && book.buf) { free(book.buf); book.buf = nullptr; book.ch = -1; book.nch = 0; book.data = book.dataEnd = nullptr; }
  prefs.begin("books", false); prefs.remove(bookKey(file).c_str()); if (prefs.getString("last", "") == file) prefs.remove("last"); prefs.end();
  return bookFS->remove(String(BOOK_DIR) + "/" + file);
}

// segmented control: a pill with three equal segments, the selected one filled blue
void segmented(int x, int y, int w, const char *const lab[3], int on, int pressed = -1) {
  card(x, y, w, 24, C.CARD, 12);
  for (int i = 0; i < 3; i++) { int sx = x + i * w / 3, sw = (i + 1) * w / 3 - i * w / 3;
    if (on == i) frame.fillSmoothRoundRect(sx, y, sw, 24, 12, C.BLUE); else if (pressed == i) frame.fillSmoothRoundRect(sx, y, sw, 24, 12, C.CARD2);
    text(sx + sw / 2, y + 12, lab[i], on == i ? C.WHITE : C.DIM, F12M, textdatum_t::middle_center); }
}
int segmentAt(int x, int y, int sx, int sy, int w) { return hit(x, y, sx - 4, sy - 4, w + 8, 32) ? constrain((x - sx) * 3 / w, 0, 2) : -1; }
static const char *const SIZE_LAB[3] = {"Small", "Medium", "Large"}, *const LINES_LAB[3] = {"Tight", "Normal", "Loose"};

// ---- chapters: text-size control on top, then the chapter list in one card; long titles wrap to two lines ----
struct ChapterScreen : Screen {
  static constexpr int LIST_Y = 84, ROW1 = 30, ROW2 = 44, TX = MARGIN + 12, TW = W - 2 * MARGIN - 24;
  int scroll = 0, pressed = -1, segPressed = -1; uint16_t top[Book::MAXCH + 1]; uint8_t twoLine[Book::MAXCH]; int total = 0;
  const char *name() override { return "Chapters"; }
  // part headings ("Part I ...", or all caps) are shown dim, in capitals, with a dot after the numeral
  static bool heading(const char *t, int n) {
    if (n > 5 && !strncasecmp(t, "part ", 5)) return true;
    bool up = false; for (int i = 0; i < n; i++) { if (t[i] >= 'a' && t[i] <= 'z') return false; if (t[i] >= 'A' && t[i] <= 'Z') up = true; } return up;
  }
  int title(int i, char *t) {   // the row's text (part headings capitalised, "PART I \xC2\xB7 ..."); returns its length
    int n = min(book.chTitleLen[i], 120); memcpy(t, book.chTitle[i], n); t[n] = 0;
    if (!heading(t, n)) return n;
    for (int k = 0; k < n; k++) if (t[k] >= 'a' && t[k] <= 'z') t[k] -= 32;
    if (!strncmp(t, "PART ", 5)) { int sp = 5; while (sp < n && t[sp] != ' ') sp++; if (sp < n && t[sp + 1] != '\xC2') { memmove(t + sp + 3, t + sp, n - sp + 1); memcpy(t + sp, " \xC2\xB7", 3); n += 3; } }
    return n;
  }
  const lgfx::IFont *rowFont(int i) { return i == book.ch || heading(book.chTitle[i], book.chTitleLen[i]) ? F12M : F12; }
  // split a title at a space so the first line fits; the second line is cut to fit (returns the number of lines)
  int split(int i, char *l1, char *l2) {
    char t[128]; int n = title(i, t); frame.setFont(rowFont(i));
    if (frame.textWidth(t) <= TW) { strcpy(l1, t); l2[0] = 0; return 1; }
    int cut = 0; for (int k = 1; k < n; k++) if (t[k] == ' ') { char c = t[k]; t[k] = 0; bool fits = frame.textWidth(t) <= TW; t[k] = c; if (fits) cut = k; else break; }
    if (!cut) { cut = n; while (cut > 1) { t[cut] = 0; if (frame.textWidth(t) <= TW) break; cut--; } title(i, t); }
    memcpy(l1, t, cut); l1[cut] = 0; int s = cut; while (t[s] == ' ') s++; strcpy(l2, t + s);
    int m = strlen(l2); while (m > 1 && frame.textWidth(l2) > TW) l2[--m] = 0;
    return 2;
  }
  void enter() override {
    total = 0; char a[128], b[128];
    for (int i = 0; i < book.nch; i++) { top[i] = total; twoLine[i] = split(i, a, b) == 2; total += twoLine[i] ? ROW2 : ROW1; }
    top[book.nch] = total;
    scroll = constrain(book.ch < book.nch ? top[book.ch] - 60 : 0, 0, maxScroll());
  }
  int maxScroll() { return max(0, total - (H - LIST_Y)); }
  void render() override {
    frame.fillScreen(C.BG);
    frame.setClipRect(0, LIST_Y, W, H - LIST_Y);
    card(MARGIN, LIST_Y - scroll, W - 2 * MARGIN, total, C.CARD, 12);
    for (int i = 0; i < book.nch; i++) {
      int y = LIST_Y + top[i] - scroll, h = top[i + 1] - top[i]; if (y + h < LIST_Y || y > H) continue;
      if (pressed == i) frame.fillRect(MARGIN + 2, y, W - 2 * MARGIN - 4, h, C.CARD2);
      char l1[128], l2[128]; int n = split(i, l1, l2); bool cur = i == book.ch, part = heading(book.chTitle[i], book.chTitleLen[i]);
      const lgfx::IFont *f = rowFont(i); uint16_t col = cur ? C.BLUE : part ? C.DIM : C.TEXT;
      if (cur) frame.fillSmoothRoundRect(MARGIN + 4, y + 8, 3, h - 16, 2, C.BLUE);
      if (n == 1) text(TX, y + h / 2, l1, col, f, textdatum_t::middle_left);
      else { text(TX, y + 15, l1, col, f, textdatum_t::middle_left); text(TX, y + 29, l2, col, f, textdatum_t::middle_left); }
      if (i + 1 < book.nch) frame.drawFastHLine(TX, y + h - 1, TW, C.CARD2);
    }
    frame.clearClipRect();
    frame.fillRect(0, 0, W, LIST_Y, C.BG); drawStatusBar(); drawTitle("Chapters");
    segmented(MARGIN, CONTENT_Y, W - 2 * MARGIN, SIZE_LAB, book.fontIdx, segPressed);
  }
  int rowAt(int x, int y) { if (y < LIST_Y) return -1; int v = y - LIST_Y + scroll; for (int i = 0; i < book.nch; i++) if (v < top[i + 1]) return i; return -1; }
  void onDown(int x, int y) override { pressed = rowAt(x, y); segPressed = segmentAt(x, y, MARGIN, CONTENT_Y, W - 2 * MARGIN); if (pressed >= 0 || segPressed >= 0) dirty = true; }
  void onMove(int x, int y, int dx, int dy) override { pressed = -1; segPressed = -1; scroll = constrain(scroll - dy, 0, maxScroll()); dirty = true; }
  void onUp(int x, int y) override { if (pressed >= 0 || segPressed >= 0) { pressed = -1; segPressed = -1; dirty = true; } }
  void onTap(int x, int y) override {
    int sg = segmentAt(x, y, MARGIN, CONTENT_Y, W - 2 * MARGIN); if (sg >= 0) { bookSetFont(sg); dirty = true; return; }
    int i = rowAt(x, y); if (i >= 0) { bookOpen(i); bookSave(); popScreens(2); }
  }
} chapterScreen;

// ---- reader menu: title, author · chapter, text size and line spacing, continue / chapters ----
struct ReaderMenuScreen : Screen {
  static constexpr int SEG_X = 60, SEG_W = W - MARGIN - SEG_X, SEG1_Y = 100, SEG2_Y = 136, BTN_Y = 192, BTN1_W = 172, BTN2_X = 192, BTN2_W = 116;   // the design: 172 + 8 + 116
  int pressed = -1; // 0..2 text size, 6..8 line spacing, 4 continue, 5 chapters
  const char *name() override { return "Reading"; }
  void render() override {
    frame.fillScreen(C.BG); drawStatusBar(); drawTitle("Reading");
    String t = book.title; frame.setFont(F16B); if (frame.textWidth(t) > W - 2 * MARGIN) { while (t.length() > 1 && frame.textWidth(t + "...") > W - 2 * MARGIN) t.remove(t.length() - 1); t += "..."; }
    text(W / 2, 61, t.c_str(), C.TEXT, F16B, textdatum_t::middle_center);
    const char *ct; int cl; int num = bookChapterNum(book.ch, ct, cl);
    char sub[120]; if (num) snprintf(sub, sizeof sub, "%s \xC2\xB7 Chapter %d", book.author.c_str(), num); else snprintf(sub, sizeof sub, "%s \xC2\xB7 %.*s", book.author.c_str(), min(cl, 60), ct);
    frame.setFont(F12); int n = strlen(sub); while (n > 1 && frame.textWidth(sub) > W - 2 * MARGIN) sub[--n] = 0;
    text(W / 2, 81, sub, C.DIM, F12, textdatum_t::middle_center);
    text(MARGIN, SEG1_Y + 12, "Text", C.DIM, F12M, textdatum_t::middle_left); segmented(SEG_X, SEG1_Y, SEG_W, SIZE_LAB, book.fontIdx, pressed >= 0 && pressed < 3 ? pressed : -1);
    text(MARGIN, SEG2_Y + 12, "Lines", C.DIM, F12M, textdatum_t::middle_left); segmented(SEG_X, SEG2_Y, SEG_W, LINES_LAB, book.lineSp, pressed >= 6 ? pressed - 6 : -1);
    card(MARGIN, BTN_Y, BTN1_W, 36, pressed == 4 ? C.CARD2 : C.BLUE, 10);
    text(MARGIN + BTN1_W / 2, BTN_Y + 18, "Continue reading", C.WHITE, F16B, textdatum_t::middle_center);
    card(BTN2_X, BTN_Y, BTN2_W, 36, pressed == 5 ? C.CARD2 : C.CARD, 10);
    text(BTN2_X + 12, BTN_Y + 18, "Chapters", C.TEXT, F16, textdatum_t::middle_left); chevron(BTN2_X + BTN2_W - 16, BTN_Y + 18, C.DIM);
  }
  int at(int x, int y) {
    int sg = segmentAt(x, y, SEG_X, SEG1_Y, SEG_W); if (sg >= 0) return sg;
    sg = segmentAt(x, y, SEG_X, SEG2_Y, SEG_W); if (sg >= 0) return 6 + sg;
    if (hit(x, y, MARGIN, BTN_Y - 4, BTN1_W, 44)) return 4; if (hit(x, y, BTN2_X, BTN_Y - 4, BTN2_W, 44)) return 5; return -1;
  }
  void onDown(int x, int y) override { pressed = at(x, y); if (pressed >= 0) dirty = true; }
  void onUp(int x, int y) override { if (pressed >= 0) { pressed = -1; dirty = true; } }
  void onTap(int x, int y) override {
    int b = at(x, y); pressed = -1;
    if (b >= 0 && b < 3) { bookSetFont(b); dirty = true; }
    else if (b >= 6 && b < 9) { bookSetLineSp(b - 6); dirty = true; }
    else if (b == 4) popScreen();
    else if (b == 5) pushScreen(&chapterScreen);
    else dirty = true;
  }
} readerMenuScreen;

// ---- reader ----
struct BookScreen : Screen {
  int downX = 0, downY = 0; uint32_t lastClock = 0;
  const char *name() override { return "Book"; }
  bool titleBack() override { return false; }   // reader has its own 20 px header
  bool backHit(int x, int y) override { return y < 36 && x < 100; }   // generous zone around the 20 px header's chevron
  void onBack() override { bookSave(); popScreen(); }
  void render() override {
    frame.fillScreen(C.BG);
    if (book.ch < 0) { text(W / 2, 100, "No book open", C.DIM, F16, textdatum_t::top_center); return; }
    // header: chevron, "n · chapter" dim, clock + battery
    const char *ct; int cl; int num = bookChapterNum(book.ch, ct, cl);
    char t[80]; if (num) snprintf(t, sizeof t, "%d \xC2\xB7 %.*s", num, min(cl, 60), ct); else snprintf(t, sizeof t, "%.*s", min(cl, 60), ct);
    frame.setFont(F12); int n = strlen(t); while (n > 1 && frame.textWidth(t) > 190) t[--n] = 0;
    chevronLeft(12, 10, C.DIM);
    text(24, 10, t, C.DIM, F12, textdatum_t::middle_left);
    text(W - 36, 10, clockValid() ? clockHHMM().c_str() : "", C.DIM, F12, textdatum_t::middle_right); batteryIcon(W - 21, 10);
    // body
    int y = book.textY;
    if (book.page == 0) {   // chapter heading on the first page
      if (num) { char c[24]; snprintf(c, sizeof c, "CHAPTER %d", num); text(book.textX, y + 7, c, C.DIM, F12M, textdatum_t::middle_left); y += 20; }
      char ht[64]; int hn = min(cl, 63); memcpy(ht, ct, hn); ht[hn] = 0; frame.setFont(RD20); while (hn > 1 && frame.textWidth(ht) > book.textW) ht[--hn] = 0;
      text(book.textX, y + 12, ht, C.TEXT, RD20, textdatum_t::middle_left); y = book.textY + book.headH;
    }
    int spaceW = glyphAdv(' '), hyphW = glyphAdv('-');
    int first = book.pages[book.page], last = min(nlines, first + (book.page == 0 ? book.lines0 : book.lines));
    for (int li = first; li < last; li++) {
      const Line &L = lines[li];
      // runs are the pieces between spaces and style toggles: measure the line, then draw it justified
      int gaps = 0, width = L.hyph ? hyphW : 0; uint8_t st = L.style;
      for (const char *p = L.s; p < L.e;) {
        if (*p == ' ') { gaps++; p++; continue; } if (isToggle(*p)) { st ^= *p == 3 ? 1 : 2; p++; continue; }
        const char *q = p; while (q < L.e && *q != ' ' && !isToggle(*q)) q++; width += runWidth(p, q, st); p = q;
      }
      int indent = L.first ? INDENT : 0, slack = book.textW - indent - width - gaps * spaceW;
      bool just = !L.last && gaps > 0 && slack > 0 && slack <= book.textW / 3;   // a very loose line reads better ragged
      int extra = just ? slack / gaps : 0, rem = just ? slack % gaps : 0;
      int x = book.textX + indent, g = 0; st = L.style;
      for (const char *p = L.s; p < L.e;) {
        if (*p == ' ') { x += spaceW + extra + (g < rem ? 1 : 0); g++; p++; continue; }
        if (isToggle(*p)) { st ^= *p == 3 ? 1 : 2; p++; continue; }
        const char *q = p; while (q < L.e && *q != ' ' && !isToggle(*q)) q++;
        char word[256]; int n = stripShy(p, q, word, 254); bool hy = q == L.e && L.hyph;
        if (hy) { word[n++] = '-'; word[n] = 0; }
        text(x, y, word, C.TEXT, bookFont(book.fontIdx, st)); x += runWidth(p, q, st) + (hy ? hyphW : 0); p = q;
      }
      y += book.lineH;
    }
    // footer: page in chapter, book progress
    textf(MARGIN, 227, C.DIM, F12, textdatum_t::middle_left, "%d / %d", book.page + 1, book.npages);
    textf(W - MARGIN, 227, C.DIM, F12, textdatum_t::middle_right, "%d%%", bookPercent());
  }
  void next() { if (book.page + 1 < book.npages) book.page++; else if (book.ch + 1 < book.nch) bookOpen(book.ch + 1); else return; bookSave(); dirty = true; }
  void prev() { if (book.page > 0) book.page--; else if (book.ch > 0) { bookOpen(book.ch - 1); book.page = book.npages - 1; } else return; bookSave(); dirty = true; }
  void onDown(int x, int y) override { downX = x; downY = y; }
  void onUp(int x, int y) override { int dx = x - downX; if (abs(dx) > 50 && abs(y - downY) < 60) { if (dx < 0) next(); else prev(); } }
  void onTap(int x, int y) override {
    if (x < W / 3) prev(); else if (x > 2 * W / 3) next(); else pushScreen(&readerMenuScreen);
  }
  void tick() override { if (millis() - lastClock > 30000) { lastClock = millis(); dirty = true; } }
} bookScreen;

// ---- library screen: entry point of the Book app ----
struct LibraryScreen : Screen {
  static constexpr int LIST_Y = CONTENT_Y, ROW = 48, PITCH = 52, FOOT = 24;
  int scroll = 0, pressed = -1, opening = -1;   // opening: row whose author line reads "Opening..." while the file loads
  const char *name() override { return "Library"; }
  int maxScroll() { return max(0, nlib * PITCH - 4 - (H - LIST_Y - FOOT)); }
  void enter() override { storageRetrySD(); libScan(); scroll = constrain(scroll, 0, maxScroll()); dirty = true; }   // progress may have changed
  static uint16_t coverColor(const String &t) { uint32_t h = 0; for (char c : t) h = h * 31 + (uint8_t)c; const uint16_t cols[] = {rgb(255, 150, 40), C.INDIGO, C.SKY, C.GREEN, C.RED}; return cols[h % 5]; }
  static String initials(const String &t) { String r; bool start = true; for (char c : t) { if (c == ' ' || c == ':') start = true; else if (start && isalnum((uint8_t)c)) { r += (char)toupper(c); start = false; if (r.length() == 2) break; } } return r; }
  void render() override {
    frame.fillScreen(C.BG);
    if (!nlib) {
      text(W / 2, 96, "No books yet", C.TEXT, F16B, textdatum_t::middle_center);
      text(W / 2, 120, "Drop EPUBs on the portal:", C.DIM, F12, textdatum_t::middle_center);
      text(W / 2, 136, netConnected() ? ("http://" + WiFi.localIP().toString() + "  (perch.local)").c_str() : "connect Wi-Fi first", C.BLUE, F12, textdatum_t::middle_center);
      text(W / 2, 160, storageText().c_str(), C.DIM, F12, textdatum_t::middle_center);
    }
    frame.setClipRect(0, LIST_Y, W, H - LIST_Y - FOOT);
    for (int i = 0; i < nlib; i++) {
      int y = LIST_Y + i * PITCH - scroll; if (y + ROW < LIST_Y || y > H - FOOT) continue;
      card(MARGIN, y, W - 2 * MARGIN, ROW, pressed == i ? C.CARD2 : C.CARD, 10);
      // cover tile: initials on a colour from the title
      uint16_t cc = coverColor(lib[i].title); frame.fillSmoothRoundRect(MARGIN + 8, y + 8, 32, 32, 6, cc);
      text(MARGIN + 24, y + 24, initials(lib[i].title).c_str(), C.WHITE, F12M, textdatum_t::middle_center);
      bool cur = lib[i].file == book.file && book.ch >= 0;
      String t = lib[i].title; frame.setFont(F12M); while (t.length() > 1 && frame.textWidth(t) > 186) t.remove(t.length() - 1);
      text(62, y + 15, t.c_str(), cur ? C.BLUE : C.TEXT, F12M, textdatum_t::middle_left);
      String sub = opening == i ? String("Opening...") : lib[i].author.length() ? lib[i].author : String(lib[i].file.length() ? "" : "built in");
      frame.setFont(F12); while (sub.length() > 1 && frame.textWidth(sub) > 186) sub.remove(sub.length() - 1);
      text(62, y + 33, sub.c_str(), C.DIM, F12, textdatum_t::middle_left);
      int rx = W - MARGIN - 12;
      textf(rx, y + 17, C.DIM, F12M, textdatum_t::middle_right, lib[i].pct >= 0 ? "%d%%" : "new", lib[i].pct);
      frame.fillSmoothRoundRect(rx - 40, y + 30, 40, 4, 2, C.LINE);
      if (lib[i].pct > 0) frame.fillSmoothRoundRect(rx - 40, y + 30, max(4, 40 * lib[i].pct / 100), 4, 2, C.BLUE);
    }
    frame.clearClipRect();
    frame.fillRect(0, 0, W, LIST_Y, C.BG); drawStatusBar(); drawTitle("Library");
    if (nlib) {
      frame.fillRect(0, H - FOOT, W, FOOT, C.BG);
      text(W / 2, 229, "Add EPUBs at http://perch.local", C.DIM, F12, textdatum_t::middle_center);
    }
  }
  int rowAt(int x, int y) { if (y < LIST_Y || y >= H - FOOT) return -1; int i = (y - LIST_Y + scroll) / PITCH; return i < nlib ? i : -1; }
  void onDown(int x, int y) override { pressed = rowAt(x, y); if (pressed >= 0) dirty = true; }
  void onMove(int x, int y, int dx, int dy) override { pressed = -1; scroll = constrain(scroll - dy, 0, maxScroll()); dirty = true; }
  void onUp(int x, int y) override { if (pressed >= 0) { pressed = -1; dirty = true; } }
  void open(int i) {
    if (lib[i].file != book.file || book.ch < 0) {
      opening = i; render(); flushAll(); opening = -1;   // a 1 MB file takes a moment: say so on the row itself
      if (!bookLoad(lib[i].file)) { dirty = true; return; }
    }
    book.stamp = bookNextStamp(); bookSave();   // this is now the most recently read book
    pushScreen(&bookScreen);
  }
  void onTap(int x, int y) override { int i = rowAt(x, y); pressed = -1; if (i >= 0) open(i); else dirty = true; }
} libraryScreen;

// Book app icon: the library, and straight into the last book when it is still there
void bookLaunch() {
  pushScreen(&libraryScreen);
  String last = libLast();
  for (int i = 0; i < nlib; i++) if (lib[i].file == last && (last.length() || bookEnd > bookStart)) { libraryScreen.open(i); return; }
}
