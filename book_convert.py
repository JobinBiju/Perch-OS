# Convert an EPUB into the text format the Perch reader uses (the portal does the same in the browser).
# Usage: python book_convert.py "book.epub" [data/book.txt]
# Output: "\x02Title\nAuthor\n", then chapters starting with a line "\x01<title>", one paragraph per line.
# UTF-8 limited to the board's fonts (ASCII, Latin-1, Latin Extended-A, common punctuation); English text gets
# soft hyphens (U+00AD) at the break points of the hyph-utf8 en-US patterns in portal/hyph-en-us.txt.
import sys, re, html, zipfile, posixpath, unicodedata, os

src = sys.argv[1]
dst = sys.argv[2] if len(sys.argv) > 2 else "data/book.txt"
z = zipfile.ZipFile(src)

container = z.read("META-INF/container.xml").decode()
opf_path = re.search(r'full-path="([^"]+)"', container).group(1)
base = posixpath.dirname(opf_path)
opf = z.read(opf_path).decode("utf-8", "replace")
title = html.unescape(re.search(r"<dc:title[^>]*>(.*?)</dc:title>", opf, re.S).group(1)).strip()
author = html.unescape((re.search(r"<dc:creator[^>]*>(.*?)</dc:creator>", opf, re.S) or [None, ""])[1]).strip()

items = {m.group(1): m.group(2) for m in re.finditer(r'<item[^>]*?id="([^"]+)"[^>]*?href="([^"]+)"', opf)}
items.update({m.group(2): m.group(1) for m in re.finditer(r'<item[^>]*?href="([^"]+)"[^>]*?id="([^"]+)"', opf)})
spine = [items[i] for i in re.findall(r'<itemref[^>]*idref="([^"]+)"', opf) if i in items]
spine = [posixpath.normpath(posixpath.join(base, h)) for h in spine]

# chapter titles from the NCX, keyed by file
toc = {}
ncx = next((n for n in z.namelist() if n.endswith(".ncx")), None)
if ncx:
    for m in re.finditer(r"<navPoint.*?<text>(.*?)</text>.*?<content src=\"([^\"#]+)", z.read(ncx).decode("utf-8", "replace"), re.S):
        f = posixpath.normpath(posixpath.join(posixpath.dirname(ncx), m.group(2)))
        toc.setdefault(f, re.sub(r"\s+", " ", html.unescape(m.group(1))).strip())

MAP = {"\u00A0": " ", "\u2011": "-", "\u2010": "-", "\u2212": "-", "\u00AD": "", "\u200B": "", "\u200C": "", "\u200D": "", "\uFEFF": "", "\t": " "}
KEEP = set("‘’“”‚„‹›–—…•′″€£")
def clean(s):
    o = []
    for c in unicodedata.normalize("NFC", s):
        if c in MAP: o.append(MAP[c]); continue
        k = ord(c)
        if 32 <= k < 127 or 0xA1 <= k < 0x180 or c in KEEP or c in "\n\x03\x04": o.append(c); continue
        if k < 0xA1: continue
        o.extend(b for b in unicodedata.normalize("NFKD", c) if 32 <= ord(b) < 127)
    return "".join(o)

# Liang hyphenation, en-US patterns; left min 2, right min 3
PATS, PMAX = {}, 0
for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "portal", "hyph-en-us.txt")):
    p = line.strip()
    if not re.fullmatch(r"[.a-z0-9]+", p): continue
    letters = re.sub(r"\d", "", p); v = [0] * (len(letters) + 1); li = 0
    for ch in p:
        if ch.isdigit(): v[li] = max(v[li], int(ch))
        else: li += 1
    PATS[letters] = v; PMAX = max(PMAX, len(letters))
def hyphen_points(w):
    s = "." + w + "."; n = len(s); pts = [0] * (n + 1)
    for i in range(n):
        for j in range(i + 1, min(n, i + PMAX) + 1):
            v = PATS.get(s[i:j])
            if v:
                for k, d in enumerate(v):
                    if d > pts[i + k]: pts[i + k] = d
    return [m for m in range(2, len(w) - 2) if pts[m + 1] % 2]
def hyphenate(p):
    def word(m):
        w = m.group(0); o = ""; last = 0
        for k in hyphen_points(w.lower()): o += w[last:k] + "\u00AD"; last = k
        return o + w[last:]
    return re.sub(r"[A-Za-z]{5,}", word, p)
lang = (re.search(r"<dc:language[^>]*>(.*?)</dc:language>", opf, re.S) or [None, "en"])[1].strip().lower()
hyph = hyphenate if lang.startswith("en") else (lambda p: p)

def paragraphs(doc):
    doc = re.sub(r"<(style|script|head)\b.*?</\1>", "", doc, flags=re.S | re.I)
    doc = re.sub(r"<br\s*/?>", "\n", doc, flags=re.I)
    doc = re.sub(r"</?(em|i|cite)\b[^>]*>", "\x03", doc, flags=re.I)                  # italic on/off
    doc = re.sub(r"</?(strong|b)\b[^>]*>", "\x04", doc, flags=re.I)                   # bold on/off
    doc = re.sub(r"<h[1-6]\b[^>]*>", "\n\x04", doc, flags=re.I); doc = re.sub(r"</h[1-6]>", "\x04\n", doc, flags=re.I)   # headings inside a chapter
    doc = re.sub(r"</(p|div|li|tr|blockquote|dd|dt)>", "\n", doc, flags=re.I)
    doc = re.sub(r"<[^>]+>", "", doc)
    out = []
    for p in html.unescape(doc).split("\n"):
        p = re.sub(r"\s+", " ", clean(p)).strip()
        if not re.sub(r"[\x03\x04]", "", p).strip(): continue
        p = p.replace("\x03\x03", "").replace("\x04\x04", "")
        for t in "\x03\x04":
            if p.count(t) % 2: p = p[:-1] if p.endswith(t) else p + t   # styles end with the paragraph
        out.append(p)
    return out

chapters = []   # [title, [paragraphs]]
for f in spine:
    if f not in z.namelist(): continue
    paras = paragraphs(z.read(f).decode("utf-8", "replace"))
    if f not in toc and paras and re.sub(r"[\x03\x04]", "", paras[0]).lower() in ("contents", "table of contents"): continue  # skip the TOC page
    if f in toc: chapters.append([re.sub(r"[\x03\x04]", "", clean(toc[f])), []])
    if not chapters:
        if not paras: continue
        chapters.append([clean(title), []])
    chapters[-1][1].extend(hyph(p) for p in paras)
chapters = [c for c in chapters if c[1]]

with open(dst, "w", encoding="utf-8") as o:
    o.write("\x02" + clean(title) + "\n" + clean(author) + "\n")
    for t, ps in chapters:
        o.write("\x01" + t + "\n")
        for p in ps: o.write(p + "\n")
# cover thumbnail sidecar (the board's library tile): "PC", width, height, then 32x48 RGB565 little-endian pixels,
# corners outside a 4 px radius in the key colour 0xF81F that the board draws transparent. Same lookup as convert.js.
def first_image(page):
    try: x = z.read(page).decode("utf-8", "replace")
    except KeyError: return None
    m = re.search(r'<(?:img|image)\b[^>]*?\b(?:src|xlink:href|href)="([^"]+)"', x, re.I)
    return posixpath.normpath(posixpath.join(posixpath.dirname(page), html.unescape(m.group(1)))) if m else None
def cover_path():
    IMG = re.compile(r"\.(jpe?g|png|gif|webp|bmp)$", re.I)
    m = re.search(r'<item\b[^>]*\bproperties="[^"]*\bcover-image\b[^"]*"[^>]*\bhref="([^"]+)"', opf) or re.search(r'<item\b[^>]*\bhref="([^"]+)"[^>]*\bproperties="[^"]*\bcover-image\b', opf)
    if m: return posixpath.normpath(posixpath.join(base, html.unescape(m.group(1))))
    m = re.search(r'<meta\b[^>]*\bname="cover"[^>]*\bcontent="([^"]+)"', opf) or re.search(r'<meta\b[^>]*\bcontent="([^"]+)"[^>]*\bname="cover"', opf)
    if m and m.group(1) in items:
        p = posixpath.normpath(posixpath.join(base, html.unescape(items[m.group(1)])))
        if IMG.search(p): return p
        if (i := first_image(p)): return i
    m = re.search(r'<reference\b[^>]*\btype="cover"[^>]*\bhref="([^"#]+)', opf) or re.search(r'<reference\b[^>]*\bhref="([^"#]+)[^>]*\btype="cover"', opf)
    if m:
        p = posixpath.normpath(posixpath.join(base, html.unescape(m.group(1))))
        if (i := p if IMG.search(p) else first_image(p)): return i
    return first_image(spine[0]) if spine else None
def write_cover(path, out):
    from PIL import Image, ImageOps
    import io
    W, H, R, KEY = 32, 48, 4, 0xF81F
    im = ImageOps.fit(Image.open(io.BytesIO(z.read(path))).convert("RGB"), (W, H), Image.LANCZOS)
    px = im.load(); data = bytearray([0x50, 0x43, W, H])
    for y in range(H):
        for x in range(W):
            cx = R - 0.5 - x if x < R else x - (W - R - 0.5) if x >= W - R else 0
            cy = R - 0.5 - y if y < R else y - (H - R - 0.5) if y >= H - R else 0
            if cx * cx + cy * cy > R * R: v = KEY
            else:
                r, g, b = px[x, y]; v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                if v == KEY: v = 0xF81E
            data += bytes((v & 0xFF, v >> 8))
    open(out, "wb").write(data)
cov = None
try:
    cov = cover_path()
    if cov and cov in z.namelist():
        write_cover(cov, re.sub(r"\.txt$", "", dst) + ".cov")
    else: cov = None
except Exception as e:
    print(f"cover skipped: {e}"); cov = None

size = sum(len(p) + 1 for _, ps in chapters for p in ps)
print(f"{title} by {author}: {len(chapters)} chapters, {size} bytes -> {dst}" + (f", cover -> {re.sub(r'.txt$', '', dst)}.cov" if cov else ", no cover"))
for t, ps in chapters: print(f"  {t[:50]:50s} {sum(len(p) for p in ps):>7d}")
