# Convert an EPUB into the plain-text book format the Perch reader embeds in firmware.
# Usage: python book_convert.py "book.epub" [data/book.txt]
# Output format: chapters start with a line "\x01<title>", then one paragraph per line, ASCII only.
import sys, re, html, zipfile, posixpath, unicodedata

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

REPL = {"‘": "'", "’": "'", "“": '"', "”": '"', "–": "-", "—": "-", "…": "...",
        " ": " ", "­": "", "•": "*", "©": "(c)", "′": "'", "″": '"', "×": "x"}
def ascii(s):
    s = "".join(REPL.get(c, c) for c in s)
    s = unicodedata.normalize("NFKD", s)
    return "".join(c for c in s if ord(c) < 128 and (c >= " " or c == "\n"))

def paragraphs(doc):
    doc = re.sub(r"<(style|script|head)\b.*?</\1>", "", doc, flags=re.S | re.I)
    doc = re.sub(r"<br\s*/?>", "\n", doc, flags=re.I)
    doc = re.sub(r"</(p|div|h[1-6]|li|tr|blockquote|dd|dt)>", "\n", doc, flags=re.I)
    doc = re.sub(r"<[^>]+>", "", doc)
    out = []
    for p in html.unescape(doc).split("\n"):
        p = ascii(re.sub(r"\s+", " ", p)).strip()
        if p: out.append(p)
    return out

chapters = []   # [title, [paragraphs]]
for f in spine:
    if f not in z.namelist(): continue
    paras = paragraphs(z.read(f).decode("utf-8", "replace"))
    if f not in toc and paras and paras[0].lower() in ("contents", "table of contents"): continue  # skip the TOC page
    if f in toc: chapters.append([ascii(toc[f]), []])
    if not chapters:
        if not paras: continue
        chapters.append([ascii(title), []])
    chapters[-1][1].extend(paras)
chapters = [c for c in chapters if c[1]]

with open(dst, "w") as o:
    o.write("\x02" + ascii(title) + "\n" + ascii(author) + "\n")
    for t, ps in chapters:
        o.write("\x01" + t + "\n")
        for p in ps: o.write(p + "\n")
size = sum(len(p) + 1 for _, ps in chapters for p in ps)
print(f"{title} by {author}: {len(chapters)} chapters, {size} bytes -> {dst}")
for t, ps in chapters: print(f"  {t[:50]:50s} {sum(len(p) for p in ps):>7d}")
