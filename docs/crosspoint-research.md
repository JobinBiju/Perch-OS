# What Perch's Book app can take from CrossPoint Reader and FreeInk SDK

Research notes, 2026-11-09. Repos read: `../crosspoint-reader` (v2.0.0, commit 86bde44) and
`../freeink-sdk` (commit ac146e4; the submodule pinned inside crosspoint-reader is 8 small commits
behind it and is not checked out).

## The two projects in one paragraph each

**CrossPoint Reader** is a complete e-reader firmware for e-ink boards, built for the ESP32-C3 first:
380 KB of RAM, no PSRAM, 1-bit framebuffer. Everything about it follows from that budget: EPUBs are
unzipped and parsed in 1 KB chunks, laid-out pages are cached on the SD card as binary files, fonts
are pre-rasterised bitmaps streamed from the card, and reading positions are anchored to text offsets
so the cache can be rebuilt after a settings change. Its own code is MIT; the combined firmware is
GPLv3 because of wolfSSL. About 300 source files plus 42k lines in the EPUB library alone.

**FreeInk SDK** is the hardware layer CrossPoint sits on, plus two things that matter for us:
**FreeInkBook**, a freestanding C++17 EPUB layout engine (zip, XML, CSS, UAX #14 line breaking,
hyphenation, justification, kerning, ligatures, TTF rasterising through stb_truetype, page cache),
and **FreeInkUI**, a 1-bit immediate-mode UI toolkit. MIT licensed. FreeInkBook never calls Arduino
or ESP-IDF; it takes memory arenas and byte-source callbacks and hands back pages as lists of
positioned text runs. Only its last stage, `PageRenderer`, assumes grey or 1-bit output.

## Where Perch stands

Perch's Book app is 388 lines. The browser portal converts EPUBs to ASCII text with chapter markers,
the board loads the whole book into PSRAM, wraps lines greedily with a bitmap Literata face in three
sizes, and remembers chapter + byte offset per book. That design is the right one for a board with
8 MB PSRAM and a colour LCD: most of CrossPoint's machinery exists to work around memory we have.

What Perch lacks, in the order a reader notices: ragged right edges with no justification or
hyphenation, no bold or italic, ASCII only (curly quotes and accented names are already lost), no
bookmarks, no go-to-percent, no search in the library, and a tap model that is less forgiving than
CrossPoint's.

## Findings by area

### Typography (the biggest visible gap)

- CrossPoint breaks lines with a minimum-raggedness dynamic program, `ParsedText::computeLineBreaks`
  at `lib/Epub/Epub/ParsedText.cpp:1099`, about 100 lines of integer math, and justifies by
  spreading spare space over word gaps (`computeJustifyExtra`, line 146). Greedy breaking is used
  only when hyphenation is on. Pure CPU, no e-ink dependency.
- Hyphenation is Liang patterns in a compiled trie. The English trie is 27 KB binary;
  German is 1.3 MB as a header. Runtime is about 1.1k lines.
- FreeInkBook does the same with libunibreak (77 KB of tables) plus a 71 KB US-English
  hyphenation blob, with widow/orphan control, text indent, and line spacing knobs.
- Cheapest route for Perch: hyphenate in the browser during conversion and emit a soft-hyphen
  marker byte. The device then only needs "split here if the word overflows", about 60 lines, and
  zero tables in flash.

### Fonts

- CrossPoint has three font paths: compiled bitmap fonts (NotoSans 12 to 18, four styles, 2-bit
  anti-aliased, kerning classes, 1/16 px advances), `.cpfont` files on SD, and runtime TTF through
  FreeType (PSRAM boards only, about 110 KB flash).
- FreeInkBook's `TtfFont` rasterises any TTF with stb_truetype into 8-bit coverage with kerning from
  `kern` and GPOS tables and fi/fl ligatures. A 32 to 64 KB glyph arena per face; the TTF itself
  must sit in PSRAM or mapped flash (Literata is already in `fonts/`).
- Perch's VLW fonts are already anti-aliased, so the real missing pieces are a bold and italic face
  and kerning. Adding Literata 600 and the italic to `fontgen.py` is a one-line change each; kerning
  would need a pair table emitted by `fontgen.py` and a lookup in the wrap loop.

### Reading position, bookmarks, progress

- CrossPoint saves 10 bytes per book: spine, page, page count, and a "visible text offset" that
  survives re-layout. Perch's chapter + byte offset is the same design.
- Bookmarks (`src/BookmarkEntry.h`, `src/util/BookmarkFile.cpp`): offset, 72-character summary of
  the page's first words, optional name, percent. One JSON file per book. About 110 lines plus a
  list screen.
- Go-to-percent maps a percentage to a byte position using cumulative chapter sizes
  (`Epub::calculateProgress`, `EpubReaderActivity::jumpToPercent`). Perch has the chapter sizes
  already since the book is in RAM.
- Status bar options: chapter page X/Y, book percent, progress bar, clock, battery. Perch shows
  page X/Y and percent today.

### Touch model

`src/activities/reader/ReaderUtils.h`, `detectTouchPageTurn` and `isTouchMenuTap`: centre third of
the screen in both axes opens the menu, left third goes back, the rest goes forward, swipes left and
right turn pages, long-press is reserved for selection or bookmark, with settings for inverted and
tap-only or swipe-only. Perch's reader already does tap right/left and swipes; the centre-third
menu zone and the long-press bookmark are the missing bits.

### Library

- CrossPoint keeps an on-disk index of up to 4096 books with fixed 128-byte records, folded titles
  for search (lowercase, diacritics stripped), author grouping, and tabs Recent / Added / Title /
  Author. The index exists because the C3 cannot hold it in RAM.
- Perch can do the same search and sort tabs in RAM in about 100 lines. Covers: the portal could
  emit a small RGB565 thumbnail per book, which would look good on a colour LCD and costs the
  board nothing.

### Plain text handling

CrossPoint's `.txt` path is weaker than Perch's: it turns every line feed into a `<br>`, has no
paragraph or chapter detection, and assumes UTF-8. Nothing to take here except the companion-cover
lookup (`<book>.jpg` next to the file) and the encoding fallback its browser-side EPUB optimiser
uses (utf-8, then declared charset, then windows-1252).

### Wireless

- WebSocket uploads on a second port (`CrossPointWebServer.cpp`, `onWebSocketEvent`): a text
  `START:name:size:path` frame, binary chunks, `PROGRESS` every 64 KB, `DONE`. About 170 lines over
  the links2004 WebSockets library (LGPL). Much faster than multipart POST on ESP32, which is
  what Perch's portal uses now.
- UDP "hello" discovery on port 8134 so a desktop app can find the device without mDNS: 20 lines.
- WebDAV (834 lines) and OPDS (1k lines plus SDK) are not useful for Perch because every book
  must pass through the browser converter anyway.
- Web settings API driven by one declarative table (`src/SettingsList.cpp`) that also builds the
  on-device menu. A nice pattern if Perch's portal ever gets a settings page.

### Dictionary

StarDict lookup (`src/util/Dictionary.cpp`, `DictZip.cpp`) streams from SD with a sampled sidecar
index, about 40 KB of working memory, with a small English stemmer. Plain-text definitions only
would be a medium job on Perch: long-press a word, show a popup. Needs a dictionary on the card.

### Power

CrossPoint drops the CPU to 80 MHz after 3 s idle and sleeps the loop in 10 ms slices that break
early on input (`lib/hal/HalPowerManager.cpp`). Its sleep screens rely on e-ink holding an image
unpowered, so they do not apply; Perch's new display sleep covers the backlight. The idle clock drop
would help battery life while reading and is small.

### Not worth adapting

The SD page cache and incremental chapter builds, the on-device zip/expat/CSS stack, the XTC
pre-rendered format, e-ink refresh and dithering, FreeInkUI (1-bit, grey-only colour model), BLE
page turners, tilt, OPDS, and runtime FreeType.

## FreeInkBook as a replacement engine

It fits technically. Perch already compiles with GCC 14 and C++20 on espressif32 8.0, so the
toolchain concern in the SDK docs does not apply. The engine's memory plan (about 256 KB book arena,
256 KB scratch, 64 KB glyph arena, the TTF in PSRAM) is small next to 8 MB. The work to run real
EPUBs on the board is: a `BookSource` over a file (30 lines), a `CacheStorage` over the SD card
(80 lines), an RGB565 text renderer in place of `PageRenderer::renderText` (80 lines, blend 8-bit
coverage into the frame sprite), and new library and reader flows. Images would come out grey
unless the JPEG and PNG decoders are forked to pass colour through. Tables stack as blocks, CSS
colour is ignored, footnotes jump instead of popping up. No speed numbers exist for the S3; first
open of a chapter means inflate plus parse plus layout plus an SD cache write.

The cheaper middle path, which I recommend over a full switch: keep the browser conversion but have
it emit UTF-8 text with light markup per chapter, and run only FreeInkBook's layout stage over the
book already in PSRAM (`ChapterLayout::layoutPlainText` or `ZipEntryReader::rawEntry` with a
memory-backed source and no zip). That gains UAX #14 breaking, hyphenation, justification, kerning,
ligatures, bold and italic, and offset-anchored positions, with no SD cache and no on-device EPUB
parsing. Medium effort, low risk.

## Licensing

Everything relevant is MIT, zlib, BSD or Apache, all fine inside Perch's GPL-3.0. Two things to
avoid: the Russian, Swedish and Ukrainian hyphenation tries in CrossPoint are LPPL, which the FSF
considers GPL-incompatible, and the EPDiy font struct layout in `lib/EpdFont/EpdFontData.h` is
LGPL-3.0 (fine to link, but keep its notice if code is copied). FreeType is FTL or GPLv2; we do not
need it.

## Ranked shortlist

| # | Feature | Source | Effort |
|---|---|---|---|
| 1 | Justified text with minimum-raggedness line breaking | `ParsedText::computeLineBreaks`, `computeJustifyExtra` | S |
| 2 | Hyphenation done in the portal, soft-hyphen byte honoured on device | CrossPoint's `hyphenateWordAtIndex` for the device half | S |
| 3 | UTF-8 text with curly quotes and accents (fonts already have the glyphs) | FreeInkBook's UTF-8 handling as reference | S |
| 4 | Bold and italic Literata faces, kerning pairs from `fontgen.py` | CrossPoint font converter design | S/M |
| 5 | Bookmarks: long-press adds one, list screen, first-words summary | `BookmarkEntry.h`, `BookmarkFile.cpp` | S |
| 6 | Go-to-percent and a book progress bar in the reader menu | `jumpToPercent`, status bar settings | S |
| 7 | Reader touch zones: centre third opens the menu | `ReaderUtils.h` | S |
| 8 | Line spacing and margin settings with live preview | `ReaderLineSpacing.h`, `TextSettingsPreview` | S/M |
| 9 | Library search and Recent / Title / Author sort tabs, in RAM | `library::fold`, `matchesQuery` | S/M |
| 10 | WebSocket uploads with progress in the portal | `onWebSocketEvent` | S/M |
| 11 | Cover thumbnails emitted by the portal | HomeCoverCache idea | M |
| 12 | Idle CPU clock drop while reading | `HalPowerManager.cpp` | S |
| 13 | StarDict dictionary on long-press | `Dictionary.cpp`, `DictZip.cpp` | M |
| 14 | FreeInkBook layout stage over portal text (replaces 1, 2, 4 in one go) | `ChapterLayout`, `TtfFont` | M |

Items 1 to 8 are each a day or less and change what reading feels like. Item 14 is the structural
alternative: one medium job instead of four small ones, with a far better engine underneath, at the
cost of a dependency on a young library.
