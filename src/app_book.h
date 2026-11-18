#pragma once
// Book reader: UTF-8 text books (from book_convert.py or the web portal) on the SD card / flash,
// plus one embedded in the firmware. A book is loaded whole into PSRAM and each chapter is laid out with
// best-fit line breaks, soft-hyphen breaks and justification (see the layout engine below).
// Tap right / swipe left for the next page, left for the previous one, centre for the menu.
#include "ui.h"
#include "net.h"
#include "storage.h"
#include "sync.h"
#include "dict.h"

extern const char bookStart[] asm("_binary_data_book_txt_start");
extern const char bookEnd[] asm("_binary_data_book_txt_end");

struct Book {
  String title, author, file;              // file: "" for the embedded book
  const char *data = nullptr, *dataEnd = nullptr; char *buf = nullptr;   // buf: PSRAM copy of a file book
  static constexpr int MAXCH = 256;
  const char *chTitle[MAXCH]; int chTitleLen[MAXCH]; const char *chText[MAXCH]; const char *chEnd[MAXCH]; int nch = 0;
  // current chapter layout
  int ch = -1, fontIdx = 1, lineSp = 1 /* 0 tight, 1 normal, 2 loose */; int *pages = nullptr; int npages = 0, page = 0; uint32_t stamp = 0, posT = 0;   // posT: when this position was read (epoch seconds), for cloud sync. pages: first line index of each page; stamp: open order, for the library sort
  // layout geometry
  int textX = MARGIN, textY = 30, textW = W - 2 * MARGIN, lineH = 22, lines = 8, lines0 = 6, headH = 0;   // lines0: first page, below the chapter heading
  uint16_t *cover = nullptr, *coverBig = nullptr;   // 32x48 list thumbnail and the 48x72 one of the portrait reading menu, scaled from the sidecar, in PSRAM
} book;

struct LibEntry { String file, title, author; uint32_t size; int pct; uint32_t last; uint16_t *cover; };   // last: open order (0 = never opened); cover: 32x48 RGB565 in PSRAM or null
// cover sidecar next to the book ("name.cov" for "name.txt"): "PC", width, height, then RGB565 little-endian pixels, corners in COV_KEY
static constexpr int COV_W = 32, COV_H = 48; static constexpr uint16_t COV_KEY = 0xF81F;
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
bool bookProgress(const String &file, int &ch, int &off, uint32_t *stamp = nullptr, int *rel = nullptr, int *len = nullptr, uint32_t *t = nullptr) {
  prefs.begin("books", true); String v = prefs.getString(bookKey(file).c_str(), ""); prefs.end(); if (!v.length()) return false;
  long f[6] = {0, 0, 0, -1, 0, 0}; int n = 0; const char *q = v.c_str();
  while (n < 6 && *q) { char *e; f[n++] = strtoul(q, &e, 10); q = e; while (*q == ' ') q++; }
  ch = f[0]; off = f[1]; if (stamp) *stamp = f[2]; if (rel) *rel = f[3]; if (len) *len = f[4]; if (t) *t = f[5];
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
// Kerning pairs for the reader faces (fonts_data.h, from the fonts' own tables): px to add between two codepoints.
struct KernTab { const KernPair *p; int n; };
#define KT(x) {kern_##x, (int)(sizeof kern_##x / sizeof *kern_##x)}
static const KernTab KERN[3][4] = {{KT(rd12), KT(rd12i), KT(rd12b), KT(rd12z)}, {KT(rd16), KT(rd16i), KT(rd16b), KT(rd16z)}, {KT(rd20), KT(rd20i), KT(rd20b), KT(rd20z)}};
#undef KT
int kernOf(int a, int b, int style = 0) {
  if (a > 0xFFFF || b > 0xFFFF) return 0;
  const KernTab &t = KERN[constrain(book.fontIdx, 0, 2)][style & 3]; uint32_t key = (uint32_t)a << 16 | b; int lo = 0, hi = t.n - 1;
  while (lo <= hi) { int mid = (lo + hi) / 2; uint32_t m = (uint32_t)t.p[mid].a << 16 | t.p[mid].b; if (m == key) return t.p[mid].k; if (m < key) lo = mid + 1; else hi = mid - 1; }
  return 0;
}
// the width of a run of text as it will be drawn: advances plus kerning, soft hyphens skipped
int runWidth(const char *p, const char *e, int style = 0) { int w = 0, prev = 0; while (p < e) { int cp = utf8Next(p, e); if (cp == 0xAD) continue; w += glyphAdv(cp, style) + (prev ? kernOf(prev, cp, style) : 0); prev = cp; } return w; }
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
  book.textY = 30; book.textX = MARGIN; book.textW = W - 2 * MARGIN; book.lines = (H - 22 - book.textY) / book.lineH;   // the page follows the screen shape
  const char *t; int l; book.headH = book.ch < 0 ? 0 : (bookChapterNum(book.ch, t, l) ? 20 : 0) + 24 + 8;   // "CHAPTER n" line, RD20 title, 8 px gap
  book.lines0 = (H - 22 - book.textY - book.headH) / book.lineH;
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
                  String((int)(ps - book.chText[book.ch])) + " " + String((int)(book.chEnd[book.ch] - book.chText[book.ch])) + " " + String(book.posT));
  prefs.putString("last", book.file.length() ? book.file : "*"); prefs.putInt("font", book.fontIdx); prefs.putInt("lsp", book.lineSp); prefs.end();
}
void bookOpen(int ch, const char *off = nullptr) {
  book.ch = constrain(ch, 0, book.nch - 1); bookPaginate(); book.page = off ? bookPageFor(off) : 0;
}
String coverName(const String &file) { return file.endsWith(".txt") ? file.substring(0, file.length() - 4) + ".cov" : String(); }
// Sidecar: "PC", width, height, then RGB565. The portal sends 160x240; older uploads are 96x144, or 32x48 with keyed corners.
// Scaled by averaging the source pixels under each target pixel, then the
// corners outside radius r take the key colour so the tile is rounded on any background.
uint16_t *coverScale(const uint16_t *src, int sw, int sh, int dw, int dh, int r) {
  uint16_t *out = (uint16_t *)ps_malloc(dw * dh * 2); if (!out) return nullptr;
  for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
    int x0 = x * sw / dw, x1 = max(x0 + 1, (x + 1) * sw / dw), y0 = y * sh / dh, y1 = max(y0 + 1, (y + 1) * sh / dh), rr = 0, gg = 0, bb = 0, n = 0;
    for (int v = y0; v < y1; v++) for (int u = x0; u < x1; u++) { uint16_t c = src[v * sw + u]; if (c == COV_KEY) continue; rr += c >> 11; gg += (c >> 5) & 63; bb += c & 31; n++; }
    float cx = x < r ? r - 0.5f - x : x >= dw - r ? x - (dw - r - 0.5f) : 0, cy = y < r ? r - 0.5f - y : y >= dh - r ? y - (dh - r - 0.5f) : 0;
    uint16_t c = n ? ((rr / n) << 11) | ((gg / n) << 5) | (bb / n) : COV_KEY; if (c == COV_KEY && n) c = 0xF81E;
    out[y * dw + x] = cx * cx + cy * cy > r * r ? COV_KEY : c;
  }
  return out;
}
uint16_t *libCoverLoad(const String &file, int dw = COV_W, int dh = COV_H, int r = 4) {
  String cn = coverName(file); if (!bookFS || !cn.length()) return nullptr;
  File f = bookFS->open(String(BOOK_DIR) + "/" + cn); if (!f) return nullptr;
  uint8_t h[4]; uint16_t *px = nullptr, *out = nullptr;
  if (f.read(h, 4) == 4 && h[0] == 'P' && h[1] == 'C' && h[2] >= 16 && h[3] >= 16) {
    int n = h[2] * h[3] * 2; px = (uint16_t *)ps_malloc(n);
    if (px && f.read((uint8_t *)px, n) == n) out = coverScale(px, h[2], h[3], dw, dh, r);
    free(px);
  }
  f.close(); return out;
}
// cover placeholder: initials on a colour from the title
uint16_t coverColor(const String &t) { uint32_t h = 0; for (char c : t) h = h * 31 + (uint8_t)c; const uint16_t cols[] = {rgb(255, 150, 40), C.INDIGO, C.SKY, C.GREEN, C.RED}; return cols[h % 5]; }
String initials(const String &t) { String r; bool start = true; for (char c : t) { if (c == ' ' || c == ':') start = true; else if (start && isalnum((uint8_t)c)) { r += (char)toupper(c); start = false; if (r.length() == 2) break; } } return r; }
void drawCover(int x, int y, const uint16_t *cover, const String &title, uint16_t bg) {
  if (cover) {
    frame.pushImage(x, y, COV_W, COV_H, (const lgfx::rgb565_t *)cover);   // host-order 565: a plain uint16_t* would be read byte-swapped
    for (int cy = 0; cy < COV_H; cy++) if (cy < 4 || cy >= COV_H - 4) for (int cx = 0; cx < COV_W; cx++)   // keyed corner pixels take the background
      if ((cx < 4 || cx >= COV_W - 4) && cover[cy * COV_W + cx] == COV_KEY) frame.drawPixel(x + cx, y + cy, bg);
    frame.drawRoundRect(x, y, COV_W, COV_H, 4, C.LINE);   // hairline keeps light covers off the card
  }
  else { frame.fillSmoothRoundRect(x, y, COV_W, COV_H, 4, coverColor(title)); text(x + COV_W / 2, y + COV_H / 2, initials(title).c_str(), C.WHITE, F12M, textdatum_t::middle_center); }
}
// wrap a title onto at most two lines of width w: break at the last space that fits, cut the second line with an ellipsis
int wrap2(const String &t, const lgfx::IFont *f, int w, String &l1, String &l2) {
  frame.setFont(f); l1 = t; l2 = "";
  if (frame.textWidth(t) <= w) return 1;
  int cut = 0; for (int k = 1; k < (int)t.length(); k++) if (t[k] == ' ') { if (frame.textWidth(t.substring(0, k)) <= w) cut = k; else break; }
  if (!cut) { cut = t.length(); while (cut > 1 && frame.textWidth(t.substring(0, cut)) > w) cut--; }
  l1 = t.substring(0, cut); int s0 = cut; while (s0 < (int)t.length() && t[s0] == ' ') s0++; l2 = t.substring(s0);
  if (frame.textWidth(l2) > w) { while (l2.length() > 1 && frame.textWidth(l2 + "...") > w) l2.remove(l2.length() - 1); l2 += "..."; }
  return 2;
}
// load a book (file name under /books, or "" for the embedded one) and restore its position
void marksLoad(); String marksKey(const String &file); String noteKey(const String &file, char kind);
bool bookLoad(const String &file) {
  if (book.buf) { free(book.buf); book.buf = nullptr; }
  book.ch = -1; book.nch = 0; book.file = file;
  free(book.cover); book.cover = libCoverLoad(file); free(book.coverBig); book.coverBig = libCoverLoad(file, 48, 72, 6);
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
  int ch = 0, off = -1, rel = -1, len = 0; book.posT = 0; bookProgress(file, ch, off, &book.stamp, &rel, &len, &book.posT);
  ch = constrain(ch, 0, book.nch - 1); const char *pos = nullptr;
  if (rel >= 0 && len > 0) { int now = book.chEnd[ch] - book.chText[ch]; pos = book.chText[ch] + min((long long)now, (long long)rel * now / len); }   // chapter-relative, scaled to the current text
  else if (off >= 0 && book.data + off < book.dataEnd) pos = book.data + off;   // older saves: absolute offset
  bookOpen(ch, pos); marksLoad();
  return true;
}
// ---- cloud sync hooks (sync.h)
int bookPercent();
void bookTouched() { if (clockValid()) book.posT = (uint32_t)time(nullptr); }   // the reader moved: this position is now the newest
void bookSyncPush() {
  if (!syncOn() || book.ch < 0 || !book.file.length() || !book.posT) return;
  int rel = bookPageStart(book.page) - book.chText[book.ch], len = book.chEnd[book.ch] - book.chText[book.ch];
  String key = book.file + " " + String(book.ch) + " " + String(rel) + " " + String(book.posT); if (key == sy.lastPushed) return;   // already sent
  sy.push.ch = book.ch; sy.push.rel = rel; sy.push.len = len; sy.push.pct = bookPercent(); sy.push.t = book.posT; sy.push.dev = cfg.name;
  sy.pushFile = book.file; sy.lastPushed = key; sy.pushWant = true;
}
void bookSyncPull() { if (!syncOn() || book.ch < 0 || !book.file.length()) return; sy.pullDone = false; sy.pullFile = book.file; sy.pullWant = true; }
// A newer position arrived from another device: go there. Returns the device's name, or "" when this one is already current.
String bookSyncApply() {
  if (!sy.pullDone || sy.pullFile != book.file || book.ch < 0) return "";
  sy.pullDone = false; const SyncPos &g = sy.got;
  if (!sy.gotOk || g.ch < 0 || g.ch >= book.nch || g.t <= book.posT + 2) return "";
  int now = book.chEnd[g.ch] - book.chText[g.ch]; const char *pos = book.chText[g.ch] + (g.len > 0 ? min((long long)now, (long long)g.rel * now / g.len) : 0);
  bool same = g.ch == book.ch && abs((int)(pos - bookPageStart(book.page))) < 200;   // the same page, give or take
  book.posT = g.t;
  if (!same) { bookOpen(g.ch, pos); bookSave(); return g.dev.length() ? g.dev : String("another device"); }
  bookSave(); return "";
}
// ---- notes: bookmarks and highlights. Both are places in the text like the saved place (chapter, bytes into it, the
// chapter's length), kept per book in NVS in reading order: bookmarks as "ch rel len pct id t,..." under "m<hash>",
// highlights as "ch rel end len pct id t,..." under "h<hash>". id and t (when it was made) are for cloud sync, which
// merges the sets of several devices; an item removed here leaves a tombstone "id t" under "x<hash>" so the removal
// reaches the others too.
struct Mark { int ch, rel, len, pct; uint32_t id, t; };
struct Note { int ch, rel, end, len, pct; uint32_t id, t; };
struct Tomb { uint32_t id, t; };
static constexpr int MAX_MARKS = 32, MAX_NOTES = 100, MAX_TOMBS = 64;
Mark marks[MAX_MARKS]; int nmarks = 0; Note notes[MAX_NOTES]; int nnotes = 0; Tomb tombs[MAX_TOMBS]; int ntombs = 0;
bool notesDirty = false;   // changed since the last cloud merge
String noteKey(const String &file, char kind) { String k = bookKey(file); k.setCharAt(0, kind); return k; }
String marksKey(const String &file) { return noteKey(file, 'm'); }
uint32_t noteNow() { return clockValid() ? (uint32_t)time(nullptr) : 0; }
uint32_t noteId() {   // random, non-zero, and not one this book already uses for a note or a removal
  for (;;) {
    uint32_t i = esp_random(); bool taken = !i;
    for (int k = 0; k < nmarks && !taken; k++) taken = marks[k].id == i;
    for (int k = 0; k < nnotes && !taken; k++) taken = notes[k].id == i;
    for (int k = 0; k < ntombs && !taken; k++) taken = tombs[k].id == i;
    if (!taken) return i;
  }
}
void marksLoad() {
  prefs.begin("books", true); String m = prefs.getString(noteKey(book.file, 'm').c_str(), ""), h = prefs.getString(noteKey(book.file, 'h').c_str(), ""), x = prefs.getString(noteKey(book.file, 'x').c_str(), ""); prefs.end();
  nmarks = nnotes = ntombs = 0; notesDirty = false;
  for (const char *q = m.c_str(); *q && nmarks < MAX_MARKS;) {
    Mark k{0, 0, 0, 0, 0, 0}; unsigned long id = 0, t = 0;
    if (sscanf(q, "%d %d %d %d %lu %lu", &k.ch, &k.rel, &k.len, &k.pct, &id, &t) >= 4 && k.ch >= 0 && k.ch < book.nch) { k.id = id ? id : noteId(); k.t = t; marks[nmarks++] = k; }
    const char *c = strchr(q, ','); if (!c) break; q = c + 1;
  }
  for (const char *q = h.c_str(); *q && nnotes < MAX_NOTES;) {
    Note k{0, 0, 0, 0, 0, 0, 0}; unsigned long id = 0, t = 0;
    if (sscanf(q, "%d %d %d %d %d %lu %lu", &k.ch, &k.rel, &k.end, &k.len, &k.pct, &id, &t) >= 5 && k.ch >= 0 && k.ch < book.nch && k.end > k.rel) { k.id = id ? id : noteId(); k.t = t; notes[nnotes++] = k; }
    const char *c = strchr(q, ','); if (!c) break; q = c + 1;
  }
  for (const char *q = x.c_str(); *q && ntombs < MAX_TOMBS;) {
    unsigned long id = 0, t = 0; if (sscanf(q, "%lu %lu", &id, &t) == 2 && id) tombs[ntombs++] = {(uint32_t)id, (uint32_t)t};
    const char *c = strchr(q, ','); if (!c) break; q = c + 1;
  }
}
void marksSave() {
  String m, h, x; char b[80];
  for (int i = 0; i < nmarks; i++) { snprintf(b, sizeof b, "%s%d %d %d %d %lu %lu", i ? "," : "", marks[i].ch, marks[i].rel, marks[i].len, marks[i].pct, (unsigned long)marks[i].id, (unsigned long)marks[i].t); m += b; }
  for (int i = 0; i < nnotes; i++) { snprintf(b, sizeof b, "%s%d %d %d %d %d %lu %lu", i ? "," : "", notes[i].ch, notes[i].rel, notes[i].end, notes[i].len, notes[i].pct, (unsigned long)notes[i].id, (unsigned long)notes[i].t); h += b; }
  for (int i = 0; i < ntombs; i++) { snprintf(b, sizeof b, "%s%lu %lu", i ? "," : "", (unsigned long)tombs[i].id, (unsigned long)tombs[i].t); x += b; }
  prefs.begin("books", false);
  auto put = [&](char kind, const String &v) { String k = noteKey(book.file, kind); if (v.length()) prefs.putString(k.c_str(), v); else prefs.remove(k.c_str()); };
  put('m', m); put('h', h); put('x', x); prefs.end(); notesDirty = true; sy.notesGen++;
}
void tombAdd(uint32_t id) { if (ntombs == MAX_TOMBS) { for (int i = 1; i < ntombs; i++) tombs[i - 1] = tombs[i]; ntombs--; } tombs[ntombs++] = {id, noteNow()}; }
const char *markAt(const Mark &m) {   // where a mark points in this copy of the book
  long long now = book.chEnd[m.ch] - book.chText[m.ch]; return book.chText[m.ch] + (m.len > 0 ? min(now, (long long)m.rel * now / m.len) : 0);
}
int markHere() {   // the mark on the page being read, or -1
  if (book.ch < 0) return -1;
  const char *a = bookPageStart(book.page), *b = book.page + 1 < book.npages ? bookPageStart(book.page + 1) : book.chEnd[book.ch];
  for (int i = 0; i < nmarks; i++) if (marks[i].ch == book.ch) { const char *p = markAt(marks[i]); if (p >= a && p < b) return i; }
  return -1;
}
void markInsert(const Mark &m) {   // in reading order
  int at = 0; while (at < nmarks && (marks[at].ch < m.ch || (marks[at].ch == m.ch && markAt(marks[at]) <= markAt(m)))) at++;
  for (int k = nmarks; k > at; k--) marks[k] = marks[k - 1];
  marks[at] = m; nmarks++;
}
void markRemove(int i) { if (i < 0 || i >= nmarks) return; tombAdd(marks[i].id); for (int k = i + 1; k < nmarks; k++) marks[k - 1] = marks[k]; nmarks--; marksSave(); }
// add a mark for this page (1), take it away (-1), or 0 when the list is full
int markToggle() {
  int i = markHere(); if (i >= 0) { markRemove(i); return -1; }
  if (nmarks >= MAX_MARKS || book.ch < 0) return 0;
  markInsert({book.ch, (int)(bookPageStart(book.page) - book.chText[book.ch]), (int)(book.chEnd[book.ch] - book.chText[book.ch]), bookPercent(), noteId(), noteNow()});
  marksSave(); return 1;
}
// plain text of a stretch of the book: markers and soft hyphens dropped, paragraph ends as spaces
String plainText(const char *p, const char *e, size_t maxLen = 4000) {
  String o;
  while (p < e && o.length() < maxLen) {
    uint8_t c = *p++;
    if (c == '\n') { if (o.length() && o[o.length() - 1] != ' ') o += ' '; continue; }
    if (c < 0x20) continue;
    if (c == 0xC2 && p < e && (uint8_t)*p == 0xAD) { p++; continue; }
    o += (char)c;
  }
  o.trim(); return o;
}
String markText(const Mark &m, size_t maxLen = 60) {   // the first words at a mark, for the list
  const char *p = markAt(m); while (p > book.chText[m.ch] && p[-1] != ' ' && p[-1] != '\n') p--;   // a page may start mid-word: show it whole
  return plainText(p, book.chEnd[m.ch], maxLen);
}

// ---- the page as pieces. A line is laid out as the runs of text between spaces and style markers, each with its x and
// width, and the drawn hyphen at the end as an empty piece at L.e. Drawing, the selection and finding the word under a
// finger all use the same layout.
struct Piece { const char *p, *q; int x, w; uint8_t st; };
struct Range { const char *a = nullptr, *b = nullptr; bool empty() const { return a >= b; } };
int layLine(const Line &L, Piece *out, int cap) {
  int spaceW = glyphAdv(' '), hyphW = glyphAdv('-'), gaps = 0, width = L.hyph ? hyphW : 0, n = 0; uint8_t st = L.style;
  for (const char *p = L.s; p < L.e;) {
    if (*p == ' ') { gaps++; p++; continue; } if (isToggle(*p)) { st ^= *p == 3 ? 1 : 2; p++; continue; }
    const char *q = p; while (q < L.e && *q != ' ' && !isToggle(*q)) q++; width += runWidth(p, q, st); p = q;
  }
  int indent = L.first ? INDENT : 0, slack = book.textW - indent - width - gaps * spaceW;
  bool just = !L.last && gaps > 0 && slack > 0 && slack <= book.textW / 3;   // a very loose line reads better ragged
  int extra = just ? slack / gaps : 0, rem = just ? slack % gaps : 0, x = book.textX + indent, g = 0; st = L.style;
  for (const char *p = L.s; p < L.e;) {
    if (*p == ' ') { x += spaceW + extra + (g < rem ? 1 : 0); g++; p++; continue; }
    if (isToggle(*p)) { st ^= *p == 3 ? 1 : 2; p++; continue; }
    const char *q = p; while (q < L.e && *q != ' ' && !isToggle(*q)) q++;
    int w = runWidth(p, q, st); if (n < cap) out[n++] = {p, q, x, w, st}; x += w; p = q;
  }
  if (L.hyph && n < cap) out[n++] = {L.e, L.e, x, hyphW, st};
  return n;
}
bool covers(const Range &r, const Piece &pc) { return pc.p == pc.q ? pc.p > r.a && pc.p <= r.b : pc.p < r.b && pc.q > r.a; }
// every line of the page being read, with its y
template <class F> void eachLine(F fn) {
  if (book.ch < 0 || !book.pages) return;
  int y = book.textY + (book.page == 0 ? book.headH : 0), first = book.pages[book.page], last = min(nlines, first + (book.page == 0 ? book.lines0 : book.lines));
  for (int i = first; i < last; i++) { fn(lines[i], y); y += book.lineH; }
}
// the word under a point of the page (from space to space, markers and all), or an empty range
Range wordAt(int x, int y) {
  Range r; static Piece ps[64]; int spaceW = glyphAdv(' ');
  eachLine([&](const Line &L, int ly) {
    if (!r.empty() || y < ly || y >= ly + book.lineH) return;
    int n = layLine(L, ps, 64), best = -1, bd = spaceW * 3;   // the nearest word, so a touch in a wide justified gap still lands
    for (int i = 0; i < n; i++) if (ps[i].p < ps[i].q) { int d = x < ps[i].x ? ps[i].x - x : x >= ps[i].x + ps[i].w ? x - (ps[i].x + ps[i].w) + 1 : 0; if (d < bd) { bd = d; best = i; } }
    if (best < 0) return;
    const char *a = ps[best].p, *b = ps[best].q, *c0 = book.chText[book.ch], *c1 = book.chEnd[book.ch];
    while (a > c0 && a[-1] != ' ' && a[-1] != '\n') a--;
    while (b < c1 && *b != ' ' && *b != '\n') b++;
    r.a = a; r.b = b;
  });
  return r;
}
int rangeY(const Range &r) { int y = -1; eachLine([&](const Line &L, int ly) { if (y < 0 && r.a < L.e && r.a >= L.s) y = ly; }); return y; }   // the line a range starts on

// ---- highlights
Range rangeOf(const Note &n) {   // scaled to this copy of the chapter
  long long now = book.chEnd[n.ch] - book.chText[n.ch], len = n.len > 0 ? n.len : now;
  Range r; r.a = book.chText[n.ch] + min(now, n.rel * now / len); r.b = book.chText[n.ch] + min(now, n.end * now / len); return r;
}
int noteAt(const Range &r) {   // the highlight a range lies inside, or -1
  for (int i = 0; i < nnotes; i++) if (notes[i].ch == book.ch) { Range h = rangeOf(notes[i]); if (r.a >= h.a && r.b <= h.b) return i; }
  return -1;
}
void noteInsert(const Note &n) {
  int at = 0; while (at < nnotes && (notes[at].ch < n.ch || (notes[at].ch == n.ch && rangeOf(notes[at]).a <= rangeOf(n).a))) at++;
  for (int k = nnotes; k > at; k--) notes[k] = notes[k - 1];
  notes[at] = n; nnotes++;
}
// Every new highlight is also appended to clippings.txt on the book volume, in the Kindle's "My Clippings" layout.
void addClipping(const String &words, int ch, int pct) {
  if (!bookFS) return; File f = bookFS->open("/clippings.txt", FILE_APPEND); if (!f) return;
  const char *ct; int cl; int num = bookChapterNum(ch, ct, cl); char where[80], when[48] = "";
  if (num) snprintf(where, sizeof where, "Chapter %d", num); else snprintf(where, sizeof where, "%.*s", min(cl, 60), ct);
  if (clockValid()) { time_t t = time(nullptr); struct tm tmv; gmtime_r(&t, &tmv); strftime(when, sizeof when, " | Added on %Y-%m-%d %H:%M UTC", &tmv); }
  f.printf("%s (%s)\n- Highlight in %s | %d%%%s\n\n%s\n==========\n", book.title.c_str(), book.author.c_str(), where, pct, when, words.c_str());
  f.close();
}
void noteRemove(int i, bool save = true) { if (i < 0 || i >= nnotes) return; tombAdd(notes[i].id); for (int k = i + 1; k < nnotes; k++) notes[k - 1] = notes[k]; nnotes--; if (save) marksSave(); }
// highlight a range of the open chapter; false when the list is full. Overlapping highlights merge into one.
bool noteAdd(const Range &r) {
  if (r.empty() || book.ch < 0) return false;
  const char *c0 = book.chText[book.ch];
  Note n{book.ch, (int)(r.a - c0), (int)(r.b - c0), (int)(book.chEnd[book.ch] - c0), (int)((r.a - book.data) * 100LL / max(1, (int)(book.dataEnd - book.data))), noteId(), noteNow()};
  for (int i = nnotes - 1; i >= 0; i--) if (notes[i].ch == book.ch) { Range h = rangeOf(notes[i]); if (h.a < r.b && h.b > r.a) { n.rel = min(n.rel, (int)(h.a - c0)); n.end = max(n.end, (int)(h.b - c0)); noteRemove(i, false); } }
  if (nnotes >= MAX_NOTES) { marksSave(); return false; }
  noteInsert(n); marksSave(); addClipping(plainText(r.a, r.b), book.ch, n.pct); return true;
}
String chapterLabel(int ch) {   // "3 \xC2\xB7 The Human Spark", or the bare title when it has no number
  const char *ct; int cl; int num = bookChapterNum(ch, ct, cl); char t[80];
  if (num) snprintf(t, sizeof t, "%d \xC2\xB7 %.*s", num, min(cl, 60), ct); else snprintf(t, sizeof t, "%.*s", min(cl, 60), ct);
  return t;
}
void bookGoTo(int ch, const char *at) { bookOpen(ch, at); bookTouched(); bookSave(); }
int chapterAtPercent(int pct) {   // the chapter that holds pct percent of the book's bytes, as the percent shown counts them
  const char *t = book.data + (long long)(book.dataEnd - book.data) * constrain(pct, 0, 100) / 100; int ch = 0;
  while (ch + 1 < book.nch && book.chText[ch + 1] <= t) ch++;
  return ch;
}
void bookGoPercent(int pct) {
  const char *t = book.data + (long long)(book.dataEnd - book.data) * constrain(pct, 0, 100) / 100; int ch = chapterAtPercent(pct);
  bookGoTo(ch, t < book.chText[ch] ? book.chText[ch] : t >= book.chEnd[ch] ? book.chEnd[ch] - 1 : t);
}
// ---- notes in the cloud: post the whole set, take the merged set back (Perch-Cloud README, "Notes")
uint32_t notesSentGen = 0;
void bookNotesSync() {
  if (!syncOn() || book.ch < 0 || !book.file.length()) return;
  JsonDocument d; d["dev"] = cfg.name; JsonArray a = d["items"].to<JsonArray>();
  for (int i = 0; i < nmarks; i++) { JsonObject o = a.add<JsonObject>(); o["id"] = marks[i].id; o["k"] = "m"; o["ch"] = marks[i].ch; o["rel"] = marks[i].rel; o["len"] = marks[i].len; o["pct"] = marks[i].pct; o["t"] = marks[i].t; }
  for (int i = 0; i < nnotes; i++) { JsonObject o = a.add<JsonObject>(); o["id"] = notes[i].id; o["k"] = "h"; o["ch"] = notes[i].ch; o["rel"] = notes[i].rel; o["end"] = notes[i].end; o["len"] = notes[i].len; o["pct"] = notes[i].pct; o["t"] = notes[i].t; }
  for (int i = 0; i < ntombs; i++) { JsonObject o = a.add<JsonObject>(); o["id"] = tombs[i].id; o["del"] = 1; o["t"] = tombs[i].t; }
  String body; serializeJson(d, body);
  sy.notesDone = false; sy.notesFile = book.file; sy.notesBody = body; notesSentGen = sy.notesGen; sy.notesWant = true;
}
// The merged set has come back: make it this device's. True when something on screen may have changed.
bool notesSynced() {
  if (!sy.notesDone) return false;
  sy.notesDone = false; if (!sy.notesOk || sy.notesFile != book.file || book.ch < 0) return false;
  if (sy.notesGen != notesSentGen) { bookNotesSync(); return false; }   // a note changed here while the request was out: send again
  JsonDocument d; if (deserializeJson(d, sy.notesOut)) return false;
  uint32_t before = 0; for (int i = 0; i < nmarks; i++) before += marks[i].id; for (int i = 0; i < nnotes; i++) before += notes[i].id * 3;
  nmarks = nnotes = ntombs = 0;
  for (JsonObject o : d["items"].as<JsonArray>()) {
    uint32_t id = o["id"] | 0u, t = o["t"] | 0u; int ch = o["ch"] | -1; if (!id) continue;
    if (o["del"] | 0) { if (ntombs < MAX_TOMBS) tombs[ntombs++] = {id, t}; continue; }
    if (ch < 0 || ch >= book.nch) continue;
    const char *k = o["k"] | "";
    if (*k == 'm' && nmarks < MAX_MARKS) markInsert({ch, o["rel"] | 0, o["len"] | 0, o["pct"] | 0, id, t});
    else if (*k == 'h' && nnotes < MAX_NOTES && (o["end"] | 0) > (o["rel"] | 0)) noteInsert({ch, o["rel"] | 0, o["end"] | 0, o["len"] | 0, o["pct"] | 0, id, t});
  }
  uint32_t after = 0; for (int i = 0; i < nmarks; i++) after += marks[i].id; for (int i = 0; i < nnotes; i++) after += notes[i].id * 3;
  marksSave(); notesDirty = false; notesSentGen = sy.notesGen;
  return after != before;
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
  for (int i = 0; i < nlib; i++) { free(lib[i].cover); lib[i].cover = nullptr; }
  nlib = 0; bookMoveEmbedded();
  if (bookEnd > bookStart && !embMoved()) { LibEntry &e = lib[nlib++]; e.file = ""; bookHeader(bookStart, bookEnd, e.title, e.author); e.size = bookEnd - bookStart; e.pct = libPercent("", e.size, &e.last); e.cover = nullptr; }
  if (!bookFS) return;
  File d = bookFS->open(BOOK_DIR); if (!d) return;
  for (File f = d.openNextFile(); f && nlib < MAXLIB; f = d.openNextFile()) {
    String n = f.name(); int sl = n.lastIndexOf('/'); if (sl >= 0) n = n.substring(sl + 1);
    if (f.isDirectory() || !n.endsWith(".txt")) { f.close(); continue; }
    char h[200]; int r = f.read((uint8_t *)h, sizeof h - 1); if (r < 0) r = 0; h[r] = 0;
    LibEntry &e = lib[nlib++]; e.file = n; bookHeader(h, h + r, e.title, e.author); if (!e.title.length()) e.title = n; e.size = f.size(); e.pct = libPercent(n, e.size, &e.last);
    f.close(); e.cover = libCoverLoad(n);
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
  if (book.file == file && book.buf) { free(book.buf); book.buf = nullptr; free(book.cover); book.cover = nullptr; free(book.coverBig); book.coverBig = nullptr; book.ch = -1; book.nch = 0; book.data = book.dataEnd = nullptr; }
  prefs.begin("books", false); prefs.remove(bookKey(file).c_str()); prefs.remove(marksKey(file).c_str()); prefs.remove(noteKey(file, 'h').c_str()); prefs.remove(noteKey(file, 'x').c_str()); if (prefs.getString("last", "") == file) prefs.remove("last"); prefs.end();
  bookFS->remove(String(BOOK_DIR) + "/" + coverName(file));
  { String big = coverName(file); if (big.length()) { big.setCharAt(big.length() - 1, 'l'); big.setCharAt(big.length() - 2, 'v'); bookFS->remove(String(BOOK_DIR) + "/" + big); } }   // name.cvl, the e-paper Perch's large cover, if one was ever sent here
  return bookFS->remove(String(BOOK_DIR) + "/" + file);
}

// segmented control: a pill with three equal segments, the selected one filled blue
void segmented(int x, int y, int w, const char *const lab[3], int on, int pressed = -1, bool onCard = false) {   // onCard: the control sits on a card, so its track is one step lighter
  card(x, y, w, 24, onCard ? C.CARD2 : C.CARD, 12);
  for (int i = 0; i < 3; i++) { int sx = x + i * w / 3, sw = (i + 1) * w / 3 - i * w / 3;
    if (on == i) frame.fillSmoothRoundRect(sx, y, sw, 24, 12, C.ACCENT); else if (pressed == i) frame.fillSmoothRoundRect(sx, y, sw, 24, 12, onCard ? C.LINE : C.CARD2);
    text(sx + sw / 2, y + 12, lab[i], on == i ? C.WHITE : C.DIM, F12M, textdatum_t::middle_center); }
}
int segmentAt(int x, int y, int sx, int sy, int w) { return hit(x, y, sx - 4, sy - 4, w + 8, 32) ? constrain((x - sx) * 3 / w, 0, 2) : -1; }
static const char *const SIZE_LAB[3] = {"Small", "Medium", "Large"}, *const LINES_LAB[3] = {"Tight", "Normal", "Loose"};

// ---- chapters: text-size control on top, then the chapter list in one card; long titles wrap to two lines ----
struct ChapterScreen : Screen {
  static constexpr int LIST_Y = 84, ROW1 = 30, ROW2 = 44, TX = MARGIN + 12;
  static int TW() { return W - 2 * MARGIN - 24; }
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
    if (frame.textWidth(t) <= TW()) { strcpy(l1, t); l2[0] = 0; return 1; }
    int cut = 0; for (int k = 1; k < n; k++) if (t[k] == ' ') { char c = t[k]; t[k] = 0; bool fits = frame.textWidth(t) <= TW(); t[k] = c; if (fits) cut = k; else break; }
    if (!cut) { cut = n; while (cut > 1) { t[cut] = 0; if (frame.textWidth(t) <= TW()) break; cut--; } title(i, t); }
    memcpy(l1, t, cut); l1[cut] = 0; int s = cut; while (t[s] == ' ') s++; strcpy(l2, t + s);
    int m = strlen(l2); while (m > 1 && frame.textWidth(l2) > TW()) l2[--m] = 0;
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
      const lgfx::IFont *f = rowFont(i); uint16_t col = cur ? C.ACCENT : part ? C.DIM : C.TEXT;
      if (cur) frame.fillSmoothRoundRect(MARGIN + 4, y + 8, 3, h - 16, 2, C.ACCENT);
      if (n == 1) text(TX, y + h / 2, l1, col, f, textdatum_t::middle_left);
      else { text(TX, y + 15, l1, col, f, textdatum_t::middle_left); text(TX, y + 29, l2, col, f, textdatum_t::middle_left); }
      if (i + 1 < book.nch) frame.drawFastHLine(TX, y + h - 1, TW(), C.CARD2);
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
    int i = rowAt(x, y); if (i >= 0) { bookOpen(i); bookTouched(); bookSave(); popScreens(2); }
  }
} chapterScreen;

// ---- notes: bookmarks and highlights in reading order, one row each (what and where, then the words) with a bin; a tap
// opens the page ----
String fitText(String v, const lgfx::IFont *f, int w) { frame.setFont(f); if (frame.textWidth(v) <= w) return v; while (v.length() > 1 && frame.textWidth(v + "...") > w) v.remove(v.length() - 1); return v + "..."; }
struct NotesScreen : Screen {
  static constexpr int ROW = 44, PITCH = 48, BIN_W = 44;
  struct Item { bool hl; int i; };   // a highlight (notes[i]) or a bookmark (marks[i])
  Item items[MAX_MARKS + MAX_NOTES]; int n = 0, scroll = 0;
  const char *name() override { return "Notes"; }
  void collect() {   // both lists are in reading order: merge them
    n = 0; int m = 0, h = 0;
    while (m < nmarks || h < nnotes) {
      bool takeNote = m == nmarks || (h < nnotes && (notes[h].ch < marks[m].ch || (notes[h].ch == marks[m].ch && rangeOf(notes[h]).a < markAt(marks[m]))));
      items[n++] = takeNote ? Item{true, h++} : Item{false, m++};
    }
  }
  void enter() override { collect(); scroll = constrain(scroll, 0, maxScroll()); }
  int maxScroll() { return max(0, n * PITCH - 4 + MARGIN - (H - CONTENT_Y)); }
  void render() override {
    frame.fillScreen(C.BG);
    if (!n) {
      text(W / 2, H / 2 - 12, "No notes yet", C.TEXT, F16B, textdatum_t::middle_center);
      text(W / 2, H / 2 + 10, "Tap a page's top right corner to bookmark", C.DIM, F12, textdatum_t::middle_center);
      text(W / 2, H / 2 + 26, "Hold a word to highlight", C.DIM, F12, textdatum_t::middle_center);
    }
    frame.setClipRect(0, CONTENT_Y, W, H - CONTENT_Y);
    for (int k = 0; k < n; k++) {
      int y = CONTENT_Y + k * PITCH - scroll; if (y + ROW < CONTENT_Y || y > H) continue;
      const Item &it = items[k]; int ch, pct; String words;
      if (it.hl) { Range g = rangeOf(notes[it.i]); ch = notes[it.i].ch; pct = notes[it.i].pct; words = "\xE2\x80\x9C" + plainText(g.a, g.b, 80) + "\xE2\x80\x9D"; }
      else { ch = marks[it.i].ch; pct = marks[it.i].pct; words = markText(marks[it.i]); }
      card(MARGIN, y, ROW_W, ROW, C.CARD, 10); char pc[8]; snprintf(pc, sizeof pc, "%d%%", pct);
      int rx = W - MARGIN - BIN_W; frame.setFont(F12); int pw = frame.textWidth(pc);
      text(rx, y + 14, pc, C.DIM, F12, textdatum_t::middle_right);
      icon(MARGIN + 18, y + 14, it.hl ? IC_HIGHLIGHTER : IC_BOOKMARK, IC12, it.hl ? C.YELLOW : C.ACCENT);
      text(MARGIN + 30, y + 14, fitText(chapterLabel(ch), F12M, rx - pw - 8 - MARGIN - 30).c_str(), C.TEXT, F12M, textdatum_t::middle_left);
      text(MARGIN + 12, y + 30, fitText(words, it.hl ? RD12I : F12, rx - MARGIN - 12).c_str(), C.DIM, it.hl ? RD12I : F12, textdatum_t::middle_left);
      icon(W - MARGIN - BIN_W / 2, y + ROW / 2, IC_TRASH_2, IC16, C.DIM);
    }
    frame.clearClipRect();
    frame.fillRect(0, 0, W, CONTENT_Y, C.BG); drawStatusBar(); drawTitle("Notes");
  }
  void onMove(int x, int y, int dx, int dy) override { scroll = constrain(scroll - dy, 0, maxScroll()); dirty = true; }
  void onTap(int x, int y) override {
    if (y < CONTENT_Y) return; int k = (y - CONTENT_Y + scroll) / PITCH; if (k < 0 || k >= n) return; const Item it = items[k];
    if (x >= W - MARGIN - BIN_W) { if (it.hl) noteRemove(it.i); else markRemove(it.i); collect(); scroll = constrain(scroll, 0, maxScroll()); dirty = true; return; }
    if (it.hl) bookGoTo(notes[it.i].ch, rangeOf(notes[it.i]).a); else bookGoTo(marks[it.i].ch, markAt(marks[it.i]));
    popScreens(2);   // closes onto the page
  }
} notesScreen;

// ---- a definition: the headword as the title, then the dictionary's text wrapped into pages (swipe, or tap the lower
// half for more and the upper half to go back) ----
struct DefineScreen : Screen {
  struct Ln { String t; char kind; };   // 'p' part of speech, 'a' examples or synonyms (set in), 'n' the rest
  String word; dict::Result res; std::vector<Ln> lines; int page = 0, laidW = 0, downY = 0;
  const char *name() override { return "Define"; }
  void look(const String &w) { word = w; res = dict::lookup(std::string(w.c_str())); laidW = 0; page = 0; }
  static constexpr int LH = 16;
  int top() { return CONTENT_Y + 4; }
  int perPage() { return max(1, (H - top() - 24) / LH); }
  int pages() { int k = lines.size(), per = perPage(); return k ? (k + per - 1) / per : 1; }
  void layout() {
    lines.clear(); laidW = W; String t = res.text.c_str(); int i = 0;
    while (i <= (int)t.length()) {   // each line of the text, greedily wrapped at spaces
      int nl = t.indexOf('\n', i); String para = nl < 0 ? t.substring(i) : t.substring(i, nl);
      char kind = para == "noun" || para == "verb" || para == "adjective" || para == "adverb" ? 'p' : para.startsWith("e.g. ") || para.startsWith("also: ") ? 'a' : 'n';
      if (kind == 'p') para.toUpperCase();
      const lgfx::IFont *f = kind == 'p' ? F12M : F12; int w = W - 2 * MARGIN - (kind == 'a' ? 12 : 0); frame.setFont(f);
      String line; int k = 0;
      while (k < (int)para.length()) {
        int sp = para.indexOf(' ', k); String wd = sp < 0 ? para.substring(k) : para.substring(k, sp), trial = line.length() ? line + " " + wd : wd;
        if (line.length() && frame.textWidth(trial) > w) { lines.push_back({line, kind}); line = wd; } else line = trial;
        if (sp < 0) break; k = sp + 1;
      }
      lines.push_back({line, kind});
      if (nl < 0) break; i = nl + 1;
    }
  }
  void render() override {
    if (laidW != W) layout();
    frame.fillScreen(C.BG); drawStatusBar(); drawTitle(fitText(res.found ? String(res.head.c_str()) : plainWord(), F16B, W - 100).c_str());
    if (!res.haveDict || !res.found) {
      text(W / 2, H / 2 - 8, res.haveDict ? "Not in the dictionary" : "No dictionary", C.TEXT, F16B, textdatum_t::middle_center);
      text(W / 2, H / 2 + 14, res.haveDict ? fitText("\xE2\x80\x9C" + word + "\xE2\x80\x9D", F12, W - 2 * MARGIN).c_str() : "Put a StarDict dictionary in /dict", C.DIM, F12, textdatum_t::middle_center);
      return;
    }
    int per = perPage(), y = top(); if (page >= pages()) page = pages() - 1;
    for (int i = page * per; i < (int)lines.size() && i < (page + 1) * per; i++, y += LH) {
      const Ln &l = lines[i];
      if (l.kind == 'p') text(MARGIN, y + LH / 2, l.t.c_str(), C.ACCENT, F12M, textdatum_t::middle_left);
      else text(MARGIN + (l.kind == 'a' ? 12 : 0), y + LH / 2, l.t.c_str(), l.kind == 'a' ? C.DIM : C.TEXT, F12, textdatum_t::middle_left);
    }
    text(MARGIN, H - 11, fitText(dict::d.name, F12, W / 2).c_str(), C.DIM, F12, textdatum_t::middle_left);
    if (pages() > 1) textf(W - MARGIN, H - 11, C.DIM, F12, textdatum_t::middle_right, "%d / %d", page + 1, pages());
  }
  String plainWord() { return word; }
  void turn(int d) { if (page + d >= 0 && page + d < pages()) { page += d; dirty = true; } }
  void onDown(int x, int y) override { downY = y; }
  void onUp(int x, int y) override { if (abs(y - downY) > 40) turn(y < downY ? 1 : -1); }
  void onTap(int x, int y) override { if (y > CONTENT_Y) turn(y > (H + CONTENT_Y) / 2 ? 1 : -1); }
} defineScreen;

// ---- go to a percentage: the number, the chapter it falls in, steps of 1 and 10, Go ----
struct GoToScreen : Screen {
  int pct = 0, pressed = -1;
  const char *name() override { return "Go to"; }
  void enter() override { pct = bookPercent(); pressed = -1; }
  static int stepW() { return portrait() ? 48 : 64; } static int stepX(int i) { return (W - 4 * stepW() - 24) / 2 + i * (stepW() + 8); }
  static int numY() { return portrait() ? 136 : 104; } static int stepY() { return portrait() ? 192 : 140; } static int goY() { return H - 48; }
  void render() override {
    static const char *const L[] = {"-10", "-1", "+1", "+10"};
    frame.fillScreen(C.BG); drawStatusBar(); drawTitle("Go to");
    char b[8]; snprintf(b, sizeof b, "%d%%", pct);
    text(W / 2, numY(), b, C.TEXT, F40B, textdatum_t::baseline_center);
    text(W / 2, numY() + 16, fitText(chapterLabel(chapterAtPercent(pct)), F12, W - 2 * MARGIN).c_str(), C.DIM, F12, textdatum_t::middle_center);
    for (int i = 0; i < 4; i++) { card(stepX(i), stepY(), stepW(), 36, pressed == i ? C.CARD2 : C.CARD, 10); text(stepX(i) + stepW() / 2, stepY() + 18, L[i], C.TEXT, F16, textdatum_t::middle_center); }
    card(MARGIN, goY(), ROW_W, 36, pressed == 4 ? C.CARD2 : C.ACCENT, 10); text(W / 2, goY() + 18, "Go", C.WHITE, F16B, textdatum_t::middle_center);
  }
  int at(int x, int y) { for (int i = 0; i < 4; i++) if (hit(x, y, stepX(i) - 4, stepY() - 4, stepW() + 8, 44)) return i; return hit(x, y, MARGIN, goY() - 4, ROW_W, 44) ? 4 : -1; }
  void onDown(int x, int y) override { pressed = at(x, y); if (pressed >= 0) dirty = true; }
  void onUp(int x, int y) override { if (pressed >= 0) { pressed = -1; dirty = true; } }
  void onTap(int x, int y) override {
    static const int D[] = {-10, -1, 1, 10}; int b = at(x, y); pressed = -1; dirty = true;
    if (b >= 0 && b < 4) pct = constrain(pct + D[b], 0, 100);
    else if (b == 4) { bookGoPercent(pct); popScreens(2); }   // closes onto the page
  }
} goToScreen;

// ---- reader menu: title, author · chapter, text size and line spacing, continue / chapters ----
struct ReaderMenuScreen : Screen {
  static constexpr int COV_Y = 56, HEAD_X = MARGIN + COV_W + 12, SEG_X = 60, SEG1_Y = 120, SEG2_Y = 152;
  // landscape: Continue (172) and Chapters (116) side by side at the bottom; portrait: stacked, full width
  static int SEG_W() { return W - MARGIN - SEG_X; }
  static int BTN_Y() { return portrait() ? H - 92 : H - 40; }
  static int BTN1_W() { return portrait() ? W - 2 * MARGIN : 148; }
  static int BTN2_X() { return portrait() ? MARGIN : 168; }
  static int BTN2_Y() { return portrait() ? H - 48 : H - 40; }
  static int BTN2_W() { return portrait() ? W - 2 * MARGIN : 92; }
  static constexpr int BM_X = 268, BM_W = 40;   // landscape: the bookmarks button at the end of the row
  int pressed = -1; // 0..2 text size, 6..8 line spacing, 4 continue, 5 chapters, 9 go to (the book block), 10 notes
  const char *name() override { return "Reading"; }
  // portrait: cover 48x72 with title, author and a progress bar beside it; both controls in one card (12,140) 216x72;
  // a Chapters row (12,220) showing the current chapter; Continue (12,268) as the primary action
  static constexpr int P_SEG_X = 58, P_SEG_W = 162, P_SEG1_Y = 148, P_SEG2_Y = 180, P_CH_Y = 220, P_GO_Y = 268;
  void coverBig(int x, int y) {   // the sidecar at 48x72, or initials on the title's colour
    if (book.coverBig) {
      for (int dy = 0; dy < 72; dy++) for (int dx = 0; dx < 48; dx++) { uint16_t c = book.coverBig[dy * 48 + dx]; if (c != COV_KEY) frame.drawPixel(x + dx, y + dy, c); }
      frame.drawRoundRect(x, y, 48, 72, 6, C.LINE);
    } else { frame.fillSmoothRoundRect(x, y, 48, 72, 6, coverColor(book.title)); text(x + 24, y + 36, initials(book.title).c_str(), C.WHITE, F16B, textdatum_t::middle_center); }
  }
  void renderPortrait() {
    coverBig(MARGIN, COV_Y);
    const int tx = 72, tw = W - MARGIN - tx; String l1, l2; bool two = wrap2(book.title, F16B, tw, l1, l2) == 2;
    text(tx, COV_Y + 9, l1.c_str(), C.TEXT, F16B, textdatum_t::middle_left);
    if (two) text(tx, COV_Y + 27, l2.c_str(), C.TEXT, F16B, textdatum_t::middle_left);
    String au = book.author; frame.setFont(F12); while (au.length() > 1 && frame.textWidth(au) > tw) au.remove(au.length() - 1);
    text(tx, COV_Y + (two ? 46 : 30), au.c_str(), C.DIM, F12, textdatum_t::middle_left);
    int pct = bookPercent(); frame.fillSmoothRoundRect(tx, COV_Y + 63, 104, 4, 2, C.LINE);   // the progress line: a tap on the book block opens Go to
    if (pct > 0) frame.fillSmoothRoundRect(tx, COV_Y + 63, max(4, 104 * pct / 100), 4, 2, C.ACCENT);
    textf(W - MARGIN - 14, COV_Y + 65, C.DIM, F12M, textdatum_t::middle_right, "%d%%", pct); icon(W - MARGIN - 5, COV_Y + 65, IC_CHEVRON_RIGHT, IC12, C.DIM);
    card(MARGIN, 140, W - 2 * MARGIN, 72, C.CARD, 12);
    text(MARGIN + 10, P_SEG1_Y + 12, "Text", C.DIM, F12M, textdatum_t::middle_left); segmented(P_SEG_X, P_SEG1_Y, P_SEG_W, SIZE_LAB, book.fontIdx, pressed >= 0 && pressed < 3 ? pressed : -1, true);
    text(MARGIN + 10, P_SEG2_Y + 12, "Lines", C.DIM, F12M, textdatum_t::middle_left); segmented(P_SEG_X, P_SEG2_Y, P_SEG_W, LINES_LAB, book.lineSp, pressed >= 6 ? pressed - 6 : -1, true);
    int hw = (W - 2 * MARGIN - 8) / 2, bx = MARGIN + hw + 8;   // Chapters and Bookmarks share the row
    card(MARGIN, P_CH_Y, hw, 36, pressed == 5 ? C.CARD2 : C.CARD, 10);
    text(MARGIN + 12, P_CH_Y + 18, "Chapters", C.TEXT, F16, textdatum_t::middle_left); icon(MARGIN + hw - 10, P_CH_Y + 18, IC_CHEVRON_RIGHT, IC12, C.DIM);
    card(bx, P_CH_Y, hw, 36, pressed == 10 ? C.CARD2 : C.CARD, 10);
    icon(bx + 16, P_CH_Y + 18, IC_BOOKMARK, IC16, nmarks + nnotes ? C.ACCENT : C.TEXT); text(bx + 30, P_CH_Y + 18, "Notes", C.TEXT, F16, textdatum_t::middle_left);
    if (nmarks + nnotes) textf(bx + hw - 10, P_CH_Y + 18, C.DIM, F12, textdatum_t::middle_right, "%d", nmarks + nnotes); else icon(bx + hw - 10, P_CH_Y + 18, IC_CHEVRON_RIGHT, IC12, C.DIM);
    card(MARGIN, P_GO_Y, W - 2 * MARGIN, 36, pressed == 4 ? C.CARD2 : C.ACCENT, 10);
    text(W / 2, P_GO_Y + 18, "Continue reading", C.WHITE, F16B, textdatum_t::middle_center);
  }
  void render() override {
    frame.fillScreen(C.BG); drawStatusBar(); drawTitle("Reading");
    if (portrait()) { renderPortrait(); return; }
    drawCover(MARGIN, COV_Y, book.cover, book.title, C.BG);
    const int tw = W - MARGIN - HEAD_X - 52;   // the right end holds the percent and a chevron: a tap on the book block opens Go to
    textf(W - MARGIN - 14, COV_Y + 24, C.DIM, F12M, textdatum_t::middle_right, "%d%%", bookPercent()); icon(W - MARGIN - 5, COV_Y + 24, IC_CHEVRON_RIGHT, IC12, C.DIM);
    String l1, l2; bool two = wrap2(book.title, F16B, tw, l1, l2) == 2;   // one line: title and author centred on the cover; two: stacked from its top
    text(HEAD_X, COV_Y + (two ? 10 : 16), l1.c_str(), C.TEXT, F16B, textdatum_t::middle_left);
    if (two) text(HEAD_X, COV_Y + 28, l2.c_str(), C.TEXT, F16B, textdatum_t::middle_left);
    const char *ct; int cl; int num = bookChapterNum(book.ch, ct, cl);
    char sub[120]; if (num) snprintf(sub, sizeof sub, "%s \xC2\xB7 Chapter %d", book.author.c_str(), num); else snprintf(sub, sizeof sub, "%s \xC2\xB7 %.*s", book.author.c_str(), min(cl, 60), ct);
    frame.setFont(F12); int n = strlen(sub); while (n > 1 && frame.textWidth(sub) > tw) sub[--n] = 0;
    text(HEAD_X, COV_Y + (two ? 46 : 34), sub, C.DIM, F12, textdatum_t::middle_left);
    text(MARGIN, SEG1_Y + 12, "Text", C.DIM, F12M, textdatum_t::middle_left); segmented(SEG_X, SEG1_Y, SEG_W(), SIZE_LAB, book.fontIdx, pressed >= 0 && pressed < 3 ? pressed : -1);
    text(MARGIN, SEG2_Y + 12, "Lines", C.DIM, F12M, textdatum_t::middle_left); segmented(SEG_X, SEG2_Y, SEG_W(), LINES_LAB, book.lineSp, pressed >= 6 ? pressed - 6 : -1);
    card(MARGIN, BTN_Y(), BTN1_W(), 36, pressed == 4 ? C.CARD2 : C.ACCENT, 10);
    text(MARGIN + BTN1_W() / 2, BTN_Y() + 18, "Continue reading", C.WHITE, F16B, textdatum_t::middle_center);
    card(BTN2_X(), BTN2_Y(), BTN2_W(), 36, pressed == 5 ? C.CARD2 : C.CARD, 10);
    text(BTN2_X() + BTN2_W() / 2, BTN2_Y() + 18, "Chapters", C.TEXT, F16, textdatum_t::middle_center);
    card(BM_X, BTN2_Y(), BM_W, 36, pressed == 10 ? C.CARD2 : C.CARD, 10); icon(BM_X + BM_W / 2, BTN2_Y() + 18, IC_BOOKMARK, IC16, nmarks + nnotes ? C.ACCENT : C.TEXT);
  }
  int at(int x, int y) {
    if (portrait()) {
      int sg = segmentAt(x, y, P_SEG_X, P_SEG1_Y, P_SEG_W); if (sg >= 0) return sg;
      sg = segmentAt(x, y, P_SEG_X, P_SEG2_Y, P_SEG_W); if (sg >= 0) return 6 + sg;
      if (hit(x, y, MARGIN, P_CH_Y - 4, W - 2 * MARGIN, 44)) return x < W / 2 ? 5 : 10; if (hit(x, y, MARGIN, P_GO_Y - 4, W - 2 * MARGIN, 48)) return 4;
      return hit(x, y, 0, COV_Y - 4, W, 80) ? 9 : -1;
    }
    int sg = segmentAt(x, y, SEG_X, SEG1_Y, SEG_W()); if (sg >= 0) return sg;
    sg = segmentAt(x, y, SEG_X, SEG2_Y, SEG_W()); if (sg >= 0) return 6 + sg;
    if (hit(x, y, MARGIN, BTN_Y() - 4, BTN1_W(), 44)) return 4; if (hit(x, y, BTN2_X(), BTN2_Y() - 4, BTN2_W(), 44)) return 5; if (hit(x, y, BM_X - 4, BTN2_Y() - 4, BM_W + 8, 44)) return 10;
    return hit(x, y, 0, COV_Y - 4, W, 56) ? 9 : -1;
  }
  void onDown(int x, int y) override { pressed = at(x, y); if (pressed >= 0) dirty = true; }
  void onUp(int x, int y) override { if (pressed >= 0) { pressed = -1; dirty = true; } }
  void onTap(int x, int y) override {
    int b = at(x, y); pressed = -1;
    if (b >= 0 && b < 3) { bookSetFont(b); dirty = true; }
    else if (b >= 6 && b < 9) { bookSetLineSp(b - 6); dirty = true; }
    else if (b == 4) popScreen();
    else if (b == 5) pushScreen(&chapterScreen);
    else if (b == 9) pushScreen(&goToScreen);
    else if (b == 10) pushScreen(&notesScreen);
    else dirty = true;
  }
} readerMenuScreen;

// ---- reader ----
struct BookScreen : Screen {
  int downX = 0, downY = 0; uint32_t lastClock = 0, noteUntil = 0, downAt = 0; String note; bool isDown = false, held = false, moved = false;
  // ---- selecting: hold a word to select it, tap another word to stretch the selection to it, then Highlight (or Define,
  // for a single word). Holding a highlighted word selects the whole highlight, with Remove instead. The buttons sit at
  // the bottom, or at the top when the selection starts in the lower half of the page. No page turns meanwhile.
  Range sel, anchor; int existing = -1;
  bool selecting() const { return !sel.empty(); }
  bool oneWord() const { return existing < 0 && sel.a == anchor.a && sel.b == anchor.b; }
  bool barTop() { return rangeY(sel) > H / 2; }
  int barY() { return barTop() ? 0 : H - 44; }
  int btnN() const { return oneWord() ? 3 : 2; }
  int btnW() { return (W - 2 * MARGIN - 8 * (btnN() - 1)) / btnN(); }
  int btnX(int i) { return MARGIN + i * (btnW() + 8); }
  void drawBar() {
    int y = barY(); frame.fillRect(0, y, W, 44, C.BG); frame.drawFastHLine(0, barTop() ? y + 43 : y, W, C.LINE);
    const lgfx::IFont *f = portrait() ? F12M : F16; const char *lab[3] = {"Cancel", oneWord() ? "Define" : (existing >= 0 ? "Remove" : "Highlight"), existing >= 0 ? "Remove" : "Highlight"};
    for (int i = 0; i < btnN(); i++) { bool pri = i == btnN() - 1; card(btnX(i), y + 6, btnW(), 32, pri ? C.ACCENT : C.CARD, 10); text(btnX(i) + btnW() / 2, y + 22, lab[i], pri ? C.WHITE : C.TEXT, f, textdatum_t::middle_center); }
  }
  void clearSel() { sel = anchor = Range(); existing = -1; dirty = true; }
  void say(const char *t) { note = t; noteUntil = millis() + 2500; dirty = true; }
  void selectTap(int x, int y) {
    int by = barY();
    if (y >= by && y < by + 44) {
      int k = -1; for (int i = 0; i < btnN(); i++) if (x >= btnX(i) - 4 && x < btnX(i) + btnW() + 4) k = i;
      if (k == 0) clearSel();
      else if (k == btnN() - 1) { if (existing >= 0) { noteRemove(existing); say("Highlight removed"); } else say(noteAdd(sel) ? "Highlighted" : "No room for more highlights"); clearSel(); bookNotesSync(); }
      else if (k == 1) { String w = plainText(sel.a, sel.b, 80); clearSel(); defineScreen.look(w); pushScreen(&defineScreen); }
      return;
    }
    Range w = wordAt(x, y);
    if (!w.empty() && existing < 0) { if (w.a < anchor.a) { sel.a = w.a; sel.b = anchor.b; } else { sel.a = anchor.a; sel.b = w.b; } dirty = true; }
  }
  const char *name() override { return "Book"; }
  void enter() override { isDown = held = false; sel = anchor = Range(); existing = -1; bookSyncPull(); bookNotesSync(); }     // has another device read further, or made notes?
  void leave() override { bookSyncPush(); }
  bool titleBack() override { return false; }   // reader has its own 20 px header
  bool backHit(int x, int y) override { return y < 36 && x < 100; }   // generous zone around the 20 px header's chevron
  void onBack() override { bookSave(); popScreen(); }
  void render() override {
    frame.fillScreen(C.BG);
    if (book.ch < 0) { text(W / 2, 100, "No book open", C.DIM, F16, textdatum_t::top_center); return; }
    // header: chevron, "n · chapter" dim, clock + battery
    const char *ct; int cl; int num = bookChapterNum(book.ch, ct, cl);
    char t[80]; if (num) snprintf(t, sizeof t, "%d \xC2\xB7 %.*s", num, min(cl, 60), ct); else snprintf(t, sizeof t, "%.*s", min(cl, 60), ct);
    frame.setFont(F12); int n = strlen(t); while (n > 1 && frame.textWidth(t) > W - 146) t[--n] = 0;
    if (markHere() >= 0) { int cw = frame.textWidth(clockValid() ? clockHHMM().c_str() : ""), rx = W - 36 - cw - 16;   // a bookmarked page: a ribbon hangs from the top edge, left of the clock (tap that corner to add or remove it)
      frame.fillRect(rx, 0, 8, 17, C.ACCENT); frame.fillTriangle(rx, 17, rx + 7, 17, rx + 4, 12, C.BG); frame.fillTriangle(rx, 17, rx + 8, 17, rx + 4, 12, C.BG); }
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
    // highlights of this chapter, then every line as pieces: highlight and selection backgrounds first, the words over them
    static Range shown[MAX_NOTES]; int nshown = 0; for (int i = 0; i < nnotes; i++) if (notes[i].ch == book.ch) shown[nshown++] = rangeOf(notes[i]);
    static Piece ps[64]; frame.setFont(bookFont(book.fontIdx)); int fh = frame.fontHeight();
    const uint16_t HL = themeDark ? rgb(96, 78, 10) : rgb(255, 232, 130);
    eachLine([&](const Line &L, int ly) {
      int np = layLine(L, ps, 64);
      auto band = [&](const Range &r, int pad, uint16_t col) {   // one rounded band behind the words of this line that the range covers
        int a = -1, b = -1; for (int i = 0; i < np; i++) if (covers(r, ps[i])) { if (a < 0) a = i; b = i; }
        if (a >= 0) frame.fillSmoothRoundRect(ps[a].x - pad, ly, ps[b].x + ps[b].w - ps[a].x + 2 * pad, fh, 4, col);
      };
      for (int h = 0; h < nshown; h++) band(shown[h], 2, HL);
      if (selecting()) band(sel, 2, C.ACCENT);
      for (int i = 0; i < np; i++) {
        const Piece &pc = ps[i]; uint16_t col = selecting() && covers(sel, pc) ? C.WHITE : C.TEXT; const lgfx::IFont *f = bookFont(book.fontIdx, pc.st);
        if (pc.p == pc.q) { text(pc.x, ly, "-", col, f); continue; }
        // The library draws a string without kerning, so a word goes out in pieces, split wherever a pair is kerned,
        // each piece placed where runWidth() measured it.
        char seg[256]; int n = 0, sx = pc.x, sw = 0, prev = 0;
        for (const char *c = pc.p; c < pc.q;) {
          const char *c0 = c; int cp = utf8Next(c, pc.q); if (cp == 0xAD) continue;
          int k = prev ? kernOf(prev, cp, pc.st) : 0;
          if (k && n) { seg[n] = 0; text(sx, ly, seg, col, f); sx += sw + k; sw = 0; n = 0; }
          if (n + (c - c0) < 250) { memcpy(seg + n, c0, c - c0); n += c - c0; }
          sw += glyphAdv(cp, pc.st); prev = cp;
        }
        if (n) { seg[n] = 0; text(sx, ly, seg, col, f); }
      }
    });
    if (selecting()) { drawBar(); return; }
    // footer: page in chapter, book progress
    if (noteUntil && (int32_t)(millis() - noteUntil) < 0) { text(MARGIN, H - 13, note.c_str(), C.ACCENT, F12, textdatum_t::middle_left); }   // a sync note takes the page counter's place for a few seconds
    else textf(MARGIN, H - 13, C.DIM, F12, textdatum_t::middle_left, "%d / %d", book.page + 1, book.npages);
    textf(W - MARGIN, H - 13, C.DIM, F12, textdatum_t::middle_right, "%d%%", bookPercent());
  }
  void next() { if (book.page + 1 < book.npages) book.page++; else if (book.ch + 1 < book.nch) bookOpen(book.ch + 1); else return; bookTouched(); bookSave(); dirty = true; }
  void prev() { if (book.page > 0) book.page--; else if (book.ch > 0) { bookOpen(book.ch - 1); book.page = book.npages - 1; } else return; bookTouched(); bookSave(); dirty = true; }
  void onDown(int x, int y) override { downX = x; downY = y; isDown = true; held = moved = false; downAt = millis(); }
  void onMove(int x, int y, int dx, int dy) override { moved = true; }
  void onUp(int x, int y) override { isDown = false; if (held || selecting()) return; int dx = x - downX; if (abs(dx) > 50 && abs(y - downY) < 60) { if (dx < 0) next(); else prev(); } }
  void onTap(int x, int y) override {
    if (held) return;
    if (selecting()) { selectTap(x, y); return; }
    if (y < 36 && x > W - 96) { int r = markToggle(); say(r > 0 ? "Bookmarked" : r < 0 ? "Bookmark removed" : "No room for more bookmarks"); bookNotesSync(); return; }   // the top right corner
    if (x < W / 3) prev(); else if (x > 2 * W / 3) next(); else pushScreen(&readerMenuScreen);
  }
  void tick() override {
    if (isDown && !held && !moved && millis() - downAt > 550) {   // hold a word: select it, or the highlight it is part of
      held = true; Range w = wordAt(downX, downY);
      if (!w.empty()) { existing = noteAt(w); if (existing >= 0) sel = anchor = rangeOf(notes[existing]); else sel = anchor = w; dirty = true; }
    }
    if (notesSynced()) dirty = true;   // notes arrived from another device
    if (sy.pullDone) { String from = bookSyncApply(); if (from.length()) { note = "Synced from " + from; noteUntil = millis() + 4000; dirty = true; } }
    if (noteUntil && (int32_t)(millis() - noteUntil) >= 0) { noteUntil = 0; dirty = true; }
    if (millis() - lastClock > 30000) { lastClock = millis(); dirty = true; }
  }
} bookScreen;

// ---- library screen: entry point of the Book app ----
struct LibraryScreen : Screen {
  static constexpr int LIST_Y = CONTENT_Y, ROW = 56, PITCH = 60, FOOT = 24;
  int scroll = 0, pressed = -1, opening = -1;   // opening: row whose author line reads "Opening..." while the file loads
  const char *name() override { return "Library"; }
  int maxScroll() { return max(0, nlib * PITCH - 4 - (H - LIST_Y - FOOT)); }
  void enter() override { storageRetrySD(); libScan(); scroll = constrain(scroll, 0, maxScroll()); dirty = true; }   // progress may have changed
  void render() override {
    frame.fillScreen(C.BG);
    if (!nlib) {
      text(W / 2, 96, "No books yet", C.TEXT, F16B, textdatum_t::middle_center);
      text(W / 2, 120, "Drop EPUBs on the portal:", C.DIM, F12, textdatum_t::middle_center);
      text(W / 2, 136, netConnected() ? ("http://" + WiFi.localIP().toString() + "  (perch.local)").c_str() : "connect Wi-Fi first", C.ACCENT, F12, textdatum_t::middle_center);
      text(W / 2, 160, storageText().c_str(), C.DIM, F12, textdatum_t::middle_center);
    }
    frame.setClipRect(0, LIST_Y, W, H - LIST_Y - FOOT);
    for (int i = 0; i < nlib; i++) {
      int y = LIST_Y + i * PITCH - scroll; if (y + ROW < LIST_Y || y > H - FOOT) continue;
      card(MARGIN, y, W - 2 * MARGIN, ROW, pressed == i ? C.CARD2 : C.CARD, 10);
      // cover: the sidecar thumbnail, else initials on a colour from the title
      drawCover(MARGIN + 8, y + 4, lib[i].cover, lib[i].title, pressed == i ? C.CARD2 : C.CARD);
      bool cur = lib[i].file == book.file && book.ch >= 0;
      const int tw = portrait() ? W - 62 - MARGIN - 12 : 186;   // landscape leaves the right 60 px to the percent and its bar
      String l1, l2; bool two = wrap2(lib[i].title, F12M, tw, l1, l2) == 2;
      text(62, y + (two ? 13 : 18), l1.c_str(), cur ? C.ACCENT : C.TEXT, F12M, textdatum_t::middle_left);
      if (two) text(62, y + 27, l2.c_str(), cur ? C.ACCENT : C.TEXT, F12M, textdatum_t::middle_left);
      String sub = opening == i ? String("Opening...") : lib[i].author.length() ? lib[i].author : String(lib[i].file.length() ? "" : "built in");
      int rx = W - MARGIN - 12; char pc[8]; if (lib[i].pct >= 0) snprintf(pc, sizeof pc, "%d%%", lib[i].pct); else strcpy(pc, "new");
      if (portrait()) {   // no bar: the percent sits at the right end of the author line
        frame.setFont(F12M); int pw = frame.textWidth(pc);
        frame.setFont(F12); while (sub.length() > 1 && frame.textWidth(sub) > tw - pw - 8) sub.remove(sub.length() - 1);
        text(62, y + (two ? 43 : 38), sub.c_str(), C.DIM, F12, textdatum_t::middle_left);
        text(rx, y + (two ? 43 : 38), pc, C.DIM, F12M, textdatum_t::middle_right);
      } else {
        frame.setFont(F12); while (sub.length() > 1 && frame.textWidth(sub) > 186) sub.remove(sub.length() - 1);
        text(62, y + (two ? 43 : 38), sub.c_str(), C.DIM, F12, textdatum_t::middle_left);
        text(rx, y + 21, pc, C.DIM, F12M, textdatum_t::middle_right);
        frame.fillSmoothRoundRect(rx - 40, y + 34, 40, 4, 2, C.LINE);
        if (lib[i].pct > 0) frame.fillSmoothRoundRect(rx - 40, y + 34, max(4, 40 * lib[i].pct / 100), 4, 2, C.ACCENT);
      }
    }
    frame.clearClipRect();
    frame.fillRect(0, 0, W, LIST_Y, C.BG); drawStatusBar(); drawTitle("Library");
    if (nlib) {
      frame.fillRect(0, H - FOOT, W, FOOT, C.BG);
      text(W / 2, portrait() ? H - 13 : H - 11, "Add EPUBs at http://perch.local", C.DIM, F12, textdatum_t::middle_center);
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
