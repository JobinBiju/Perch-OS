# Perch

Phone-style launcher for the LCDWiki ES3C28P (ESP32-S3, 2.8" 320x240 capacitive touch).

Apps: **Robo** (animated companion face, mic-reactive), **Weather** (Open-Meteo, location from IP),
**Spotify** (Connect remote: now playing with album art, play/pause/next/prev, volume; controls your phone), **Settings** (Wi-Fi join with password keyboard,
brightness, °C/°F, light/dark appearance, location for weather and timezone, About).
The board has no GPS: weather uses the Location setting (default Kalamassery, Kerala, geocoded by
Open-Meteo), or IP geolocation when it is left blank. Swipe in from the left edge to go back.
The home screen is an app drawer: page one keeps the clock and weather with one row of five icons, every
further page holds two rows of five; swipe left/right to page (add apps to `apps[]` in `src/home.h`). Clock syncs over NTP once Wi-Fi
is connected; the timezone comes from the weather lookup.

Build and flash (venv in the parent folder):

    source ../.venv/bin/activate
    pio run -t upload

Debug over USB serial (115200): `S` screen dump, `b` back, `1`-`5` open app, `p` status, `W` fake weather, `T` preview other theme, `t x y` tap,
`d x0 y0 x1 y1` drag, `k text` type into the open keyboard field, `f text` render text in every font, `n a.b.c.d` TCP-connect to a LAN host from the board (reachability test), `h host` resolve a host and test TCP 443 and a TLS handshake to it (`p` also prints the Spotify worker state and who holds the network mutex), Settings > Rotation flips the screen and touch 180° for a cable-on-the-left stand. Pull down from the top edge (the status bar or the title) on any screen to adjust the brightness (then slide left or right; lift to keep the level). `d x0 y0 x1 y1 [ms]` drags over an optional duration. `i` scan the I2C bus, `r` retry the SD card mount (Settings > Storage shows the card and the flash partition side by side), `ls` list `/books` with each file's size and first bytes. Books on flash move to the card automatically when it first mounts, and the book built into the firmware is written to the card once (as a portal-style file, so it can be deleted there; its reading position moves with it). The library lists the most recently opened book first.
`python drive.py 3 'k hello' shot:out.png` scripts those; `python screenshot.py out.png` grabs the screen.

Fonts: the UI uses Plus Jakarta Sans and the reader Literata (both OFL, TTFs in `fonts/`), rendered to antialiased
VLW bitmap fonts by `python fontgen.py` into `src/fonts_data.h` (about 160 KB of flash for nine text faces plus
the Lucide icon font at 12-20 px: `fonts/Lucide.ttf` from lucide-static, ISC; the status bar, back/row chevrons and
Settings tiles draw its glyphs as text, codepoints listed in `fontgen.py`).
Sizes and weights are the `SPECS` table in that script; `src/fonts.h` names them (F12, F16B, RD16...).

Spotify setup (once): create an app at developer.spotify.com with redirect URI `http://127.0.0.1:8888/callback`,
put its Client ID in `spotify_auth.py`, then with the board plugged in run `python spotify_auth.py` and approve
in the browser. The refresh token is stored on the board; playback control needs Spotify Premium.

## Books

The Book app is a library of plain-text books. One is embedded in the firmware from `data/book.txt` (not in the repo: put any plain-text book there, title on the first line, author on the second, before building); the rest live in
`/books` on the microSD card (1-bit SDMMC, FAT32, mounted at boot or when the Library is opened), or, with no
card inserted, in a 3.3 MB LittleFS partition on the flash. Progress is saved per book; the Book icon opens the
last book you read, and its header chevron goes back to the Library.

Adding books: open **http://perch.local** (or the IP shown in Settings > About) on a phone or computer on the
same Wi-Fi and drop EPUB files on the page. The EPUB is converted in the browser (`portal/convert.js`, a port of
`book_convert.py`) and only the text is uploaded, so the board never parses EPUBs. The page also lists and
deletes books. Without the board, `python book_convert.py "book.epub" out.txt` makes the same file, which you
can copy into `/books` on the card by hand.

Book format: `\x02Title\nAuthor\n`, then chapters starting with `\x01Title\n`, one ASCII paragraph per line.
Up to 256 chapters; a book is loaded whole into PSRAM, so keep each under a few MB. Uploads to flash are slow
(about 50 KB/s); the card is much faster.
