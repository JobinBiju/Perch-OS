#pragma once
// Book reader: plain-text books (from book_convert.py or the web portal) on the SD card / flash,
// plus one embedded in the firmware. A book is loaded whole into PSRAM and paginated per chapter
// with word wrap. Tap right / swipe left for the next page, left for the previous one, centre for the menu.
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
  int ch = -1, fontIdx = 1; const char **pages = nullptr; int npages = 0, page = 0; uint32_t stamp = 0;   // stamp: open order, for the library sort
  // layout geometry
  int textX = MARGIN, textY = 30, textW = W - 2 * MARGIN, lineH = 22, lines = 8, lines0 = 6, headH = 0;   // lines0: first page, below the chapter heading
} book;

struct LibEntry { String file, title, author; uint32_t size; int pct; uint32_t last; };   // last: open order (0 = never opened)
static constexpr int MAXLIB = 40; LibEntry lib[MAXLIB]; int nlib = 0;

const lgfx::IFont *bookFont(int i) { return i == 0 ? RD12 : i == 1 ? RD16 : RD20; }

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
// progress value: "chapter offset stamp"; stamp counts book openings, so the library can sort by last read
bool bookProgress(const String &file, int &ch, int &off, uint32_t *stamp = nullptr) {
  prefs.begin("books", true); String v = prefs.getString(bookKey(file).c_str(), ""); prefs.end(); if (!v.length()) return false;
  ch = v.toInt(); int sp = v.indexOf(' '); off = sp > 0 ? v.substring(sp + 1).toInt() : 0;
  int sp2 = sp > 0 ? v.indexOf(' ', sp + 1) : -1; if (stamp) *stamp = sp2 > 0 ? (uint32_t)v.substring(sp2 + 1).toInt() : 0;
  return true;
}
uint32_t bookNextStamp() { prefs.begin("books", false); uint32_t n = prefs.getUInt("seq", 0) + 1; prefs.putUInt("seq", n); prefs.end(); return n; }

// greedy word wrap of [p, end): returns where the next line starts. `w` receives the line's text width.
const char *bookWrapLine(const char *p, const char *end, int maxw) {
  const char *nl = (const char *)memchr(p, '\n', end - p); const char *pe = nl ? nl : end; // paragraph end
  if (p == pe) return pe + 1;
  int used = 0; const char *q = p, *lastFit = nullptr;
  int spaceW = frame.textWidth(" ");
  while (q < pe) {
    const char *ws = q; while (ws < pe && *ws != ' ') ws++;
    char tmp[128]; int n = min((int)(ws - q), 127); memcpy(tmp, q, n); tmp[n] = 0;
    int ww = frame.textWidth(tmp);
    if (used && used + spaceW + ww > maxw) return lastFit;          // this word goes on the next line
    if (!used && ww > maxw) {                                         // single word wider than the line: split it
      int k = n; while (k > 1) { tmp[--k] = 0; if (frame.textWidth(tmp) <= maxw) break; } return q + k;
    }
    used += (used ? spaceW : 0) + ww; q = ws; while (q < pe && *q == ' ') q++; lastFit = q;
  }
  return pe < end ? pe + 1 : end; // end of paragraph: skip the newline
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
  book.lineH = frame.fontHeight() + (book.fontIdx == 0 ? 4 : 6);
  book.textY = 30; book.lines = (218 - book.textY) / book.lineH;
  const char *t; int l; book.headH = book.ch < 0 ? 0 : (bookChapterNum(book.ch, t, l) ? 20 : 0) + 24 + 8;   // "CHAPTER n" line, RD20 title, 8 px gap
  book.lines0 = (218 - book.textY - book.headH) / book.lineH;
}
// paginate the current chapter; pages[] holds the start of each page
void bookPaginate() {
  bookLayoutGeometry();
  if (book.pages) { free(book.pages); book.pages = nullptr; }
  book.npages = 0; if (book.ch < 0) return;
  int cap = 64; book.pages = (const char **)ps_malloc(cap * sizeof(char *));
  const char *p = book.chText[book.ch], *e = book.chEnd[book.ch];
  while (p < e) {
    if (book.npages == cap) { cap *= 2; book.pages = (const char **)ps_realloc(book.pages, cap * sizeof(char *)); }
    book.pages[book.npages++] = p;
    int lines = book.npages == 1 ? book.lines0 : book.lines;
    for (int l = 0; l < lines && p < e; l++) p = bookWrapLine(p, e, book.textW);
  }
  if (!book.npages) { book.pages[0] = e; book.npages = 1; }
}
int bookPageFor(const char *off) { int i = 0; while (i + 1 < book.npages && book.pages[i + 1] <= off) i++; return i; }
void bookSave() {
  if (book.ch < 0) return;
  prefs.begin("books", false);
  prefs.putString(bookKey(book.file).c_str(), String(book.ch) + " " + String((int)(book.pages[book.page] - book.data)) + " " + String(book.stamp));
  prefs.putString("last", book.file.length() ? book.file : "*"); prefs.putInt("font", book.fontIdx); prefs.end();
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
  int ch = 0, off = -1; bookProgress(file, ch, off, &book.stamp);
  bookOpen(ch, off >= 0 && book.data + off < book.dataEnd ? book.data + off : nullptr);
  return true;
}
void bookBegin() { prefs.begin("books", true); book.fontIdx = prefs.getInt("font", 1); prefs.end(); }
void bookSetFont(int f) { const char *off = book.pages ? book.pages[book.page] : nullptr; book.fontIdx = constrain(f, 0, 2); bookPaginate(); book.page = off ? bookPageFor(off) : 0; bookSave(); }
int bookPercent() { if (book.ch < 0) return 0; return (int)((book.pages[book.page] - book.data) * 100LL / (book.dataEnd - book.data)); }

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

// ---- chapters: text-size control on top, then the chapter list in one card ----
struct ChapterScreen : Screen {
  static constexpr int LIST_Y = 84, ROW = 30;
  int scroll = 0, pressed = -1;
  const char *name() override { return "Chapters"; }
  int maxScroll() { return max(0, book.nch * ROW - (H - LIST_Y)); }
  void enter() override { scroll = constrain(book.ch * ROW - 60, 0, maxScroll()); }
  static bool heading(const char *t, int n) { bool up = false; for (int i = 0; i < n; i++) { if (t[i] >= 'a' && t[i] <= 'z') return false; if (t[i] >= 'A' && t[i] <= 'Z') up = true; } return up; }   // part headings are all caps
  void render() override {
    frame.fillScreen(C.BG);
    frame.setClipRect(0, LIST_Y, W, H - LIST_Y);
    card(MARGIN, LIST_Y - scroll, W - 2 * MARGIN, book.nch * ROW, C.CARD, 12);
    for (int i = 0; i < book.nch; i++) {
      int y = LIST_Y + i * ROW - scroll; if (y + ROW < LIST_Y || y > H) continue;
      if (pressed == i) frame.fillRect(MARGIN + 2, y, W - 2 * MARGIN - 4, ROW, C.CARD2);
      char t[64]; int n = min(book.chTitleLen[i], 63); memcpy(t, book.chTitle[i], n); t[n] = 0;
      bool cur = i == book.ch, part = heading(t, n);
      const lgfx::IFont *f = cur || part ? F12M : F12;
      frame.setFont(f); while (n > 1 && frame.textWidth(t) > W - 2 * MARGIN - 24) t[--n] = 0;
      if (cur) frame.fillSmoothRoundRect(MARGIN + 4, y + 8, 3, 14, 2, C.BLUE);
      text(MARGIN + 12, y + ROW / 2, t, cur ? C.BLUE : part ? C.DIM : C.TEXT, f, textdatum_t::middle_left);
      if (i + 1 < book.nch) frame.drawFastHLine(MARGIN + 12, y + ROW - 1, W - 2 * MARGIN - 24, C.CARD2);
    }
    frame.clearClipRect();
    frame.fillRect(0, 0, W, LIST_Y, C.BG); drawStatusBar(); drawTitle("Chapters");
    // text size: segmented control 296x24 r12
    const char *lab[] = {"Small", "Medium", "Large"};
    card(MARGIN, CONTENT_Y, W - 2 * MARGIN, 24, C.CARD, 12);
    for (int i = 0; i < 3; i++) { int x = MARGIN + 2 + i * 97; bool on = book.fontIdx == i;
      if (on) frame.fillSmoothRoundRect(x, CONTENT_Y + 2, 98, 20, 10, C.BLUE);
      text(x + 49, CONTENT_Y + 12, lab[i], on ? C.WHITE : C.DIM, F12M, textdatum_t::middle_center); }
  }
  int rowAt(int x, int y) { if (y < LIST_Y) return -1; int i = (y - LIST_Y + scroll) / ROW; return i < book.nch ? i : -1; }
  void onDown(int x, int y) override { pressed = rowAt(x, y); if (pressed >= 0) dirty = true; }
  void onMove(int x, int y, int dx, int dy) override { pressed = -1; scroll = constrain(scroll - dy, 0, maxScroll()); dirty = true; }
  void onUp(int x, int y) override { if (pressed >= 0) { pressed = -1; dirty = true; } }
  void onTap(int x, int y) override {
    if (y >= CONTENT_Y - 4 && y < CONTENT_Y + 28 && x >= MARGIN && x < W - MARGIN) { bookSetFont((x - MARGIN) / 99); dirty = true; return; }
    int i = rowAt(x, y); if (i >= 0) { bookOpen(i); bookSave(); popScreens(2); }
  }
} chapterScreen;

// ---- reader menu: title, progress, text size chips, chapters row, continue / home ----
struct ReaderMenuScreen : Screen {
  static constexpr int CHIP_Y = 100, ROW_Y = 136, BTN_Y = 192, CHIP_X = 60, CHIP_W = 77, CHIP_P = 85;
  int pressed = -1; // 0..2 font chips, 3 chapters row, 4 continue, 5 home
  const char *name() override { return "Reading"; }
  void render() override {
    frame.fillScreen(C.BG); drawStatusBar(); drawTitle("Reading");
    String t = book.title; frame.setFont(F16B); if (frame.textWidth(t) > W - 2 * MARGIN) { while (t.length() > 1 && frame.textWidth(t + "...") > W - 2 * MARGIN) t.remove(t.length() - 1); t += "..."; }
    text(W / 2, 61, t.c_str(), C.TEXT, F16B, textdatum_t::middle_center);
    textf(W / 2, 81, C.DIM, F12, textdatum_t::middle_center, "%s \xC2\xB7 %d%% read \xC2\xB7 progress is saved", book.author.c_str(), bookPercent());
    const char *lab[] = {"Small", "Medium", "Large"};
    text(MARGIN, CHIP_Y + 12, "Text", C.DIM, F12M, textdatum_t::middle_left);
    for (int i = 0; i < 3; i++) { int x = CHIP_X + i * CHIP_P; bool on = book.fontIdx == i;
      card(x, CHIP_Y, CHIP_W + (i == 2 ? 1 : 0), 24, on ? C.BLUE : (pressed == i ? C.CARD2 : C.CARD), 12);
      text(x + CHIP_W / 2, CHIP_Y + 12, lab[i], on ? C.WHITE : C.TEXT, F12M, textdatum_t::middle_center); }
    card(MARGIN, ROW_Y, W - 2 * MARGIN, 36, pressed == 3 ? C.CARD2 : C.CARD, 10);
    char ct[64]; int n = min(book.chTitleLen[book.ch], 63); memcpy(ct, book.chTitle[book.ch], n); ct[n] = 0;
    frame.setFont(F12); while (n > 1 && frame.textWidth(ct) > 170) ct[--n] = 0;
    text(MARGIN + 12, ROW_Y + 18, "Chapters", C.TEXT, F16, textdatum_t::middle_left);
    text(CHEV_X - 16, ROW_Y + 18, ct, C.DIM, F12, textdatum_t::middle_right); chevron(CHEV_X, ROW_Y + 18, C.DIM);
    card(MARGIN, BTN_Y, 196, 36, pressed == 4 ? C.CARD2 : C.BLUE, 10);
    text(MARGIN + 98, BTN_Y + 18, "Continue reading", C.WHITE, F16B, textdatum_t::middle_center);
    card(216, BTN_Y, 92, 36, pressed == 5 ? C.CARD2 : C.CARD, 10);
    text(262, BTN_Y + 18, "Home", C.TEXT, F16, textdatum_t::middle_center);
  }
  int at(int x, int y) {
    if (y >= CHIP_Y - 4 && y < CHIP_Y + 28) for (int i = 0; i < 3; i++) if (x >= CHIP_X + i * CHIP_P - 4 && x < CHIP_X + i * CHIP_P + CHIP_W + 4) return i;
    if (hit(x, y, MARGIN, ROW_Y, W - 2 * MARGIN, 36)) return 3;
    if (hit(x, y, MARGIN, BTN_Y - 4, 196, 44)) return 4; if (hit(x, y, 216, BTN_Y - 4, 92, 44)) return 5; return -1;
  }
  void onDown(int x, int y) override { pressed = at(x, y); if (pressed >= 0) dirty = true; }
  void onUp(int x, int y) override { if (pressed >= 0) { pressed = -1; dirty = true; } }
  void onTap(int x, int y) override {
    int b = at(x, y); pressed = -1;
    if (b >= 0 && b < 3) { bookSetFont(b); dirty = true; }
    else if (b == 3) pushScreen(&chapterScreen);
    else if (b == 4) popScreen();
    else if (b == 5) { bookSave(); popToHome(); }
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
    const lgfx::IFont *f = bookFont(book.fontIdx); int y = book.textY;
    if (book.page == 0) {   // chapter heading on the first page
      if (num) { char c[24]; snprintf(c, sizeof c, "CHAPTER %d", num); text(book.textX, y + 7, c, C.DIM, F12M, textdatum_t::middle_left); y += 20; }
      char ht[64]; int hn = min(cl, 63); memcpy(ht, ct, hn); ht[hn] = 0; frame.setFont(RD20); while (hn > 1 && frame.textWidth(ht) > book.textW) ht[--hn] = 0;
      text(book.textX, y + 12, ht, C.TEXT, RD20, textdatum_t::middle_left); y = book.textY + book.headH;
    }
    frame.setFont(f);
    const char *p = book.pages[book.page], *e = book.chEnd[book.ch];
    int lines = book.page == 0 ? book.lines0 : book.lines;
    for (int l = 0; l < lines && p < e; l++) {
      const char *q = bookWrapLine(p, e, book.textW);
      const char *le = q; while (le > p && (le[-1] == '\n' || le[-1] == ' ')) le--;
      char line[200]; int m = min((int)(le - p), 199); memcpy(line, p, m); line[m] = 0;
      text(book.textX, y, line, C.TEXT, f);
      y += book.lineH; p = q;
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
