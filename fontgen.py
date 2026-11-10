# Generate antialiased bitmap fonts (Processing VLW format, read by LovyanGFX) from TTF files.
# Usage: python fontgen.py            -> writes src/fonts_data.h from the SPECS table below
# Each glyph is an 8-bit alpha bitmap; LovyanGFX blends it over the frame, so text looks smooth on any background.
import struct, sys, os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(HERE, "fonts")
# ASCII, Latin-1 Supplement and Latin Extended-A (accented names and words survive conversion), plus the punctuation the
# converters keep: quotes, dashes, ellipsis, bullet, primes, currency. The soft hyphen (U+00AD) is a break hint and is never drawn.
ASCII = [chr(c) for c in range(32, 127)]
CHARS = ASCII + [chr(c) for c in range(0xA1, 0x180) if c != 0xAD] + list("‘’“”‚„‹›–—…•′″€£")

# Lucide icon glyphs (private-use codepoints from lucide-static's lucide.css), drawn like text in the firmware
ICONS = {"wifi": 0xE1AE, "battery-charging": 0xE054, "battery-full": 0xE055, "battery-medium": 0xE057, "battery-low": 0xE056,
         "chevron-left": 0xE06E, "chevron-right": 0xE06F, "lock": 0xE10B, "map-pin": 0xE111, "sun": 0xE178, "thermometer": 0xE186,
         "moon": 0xE11E, "rotate-cw": 0xE149, "hard-drive": 0xE0ED, "info": 0xE0F9, "check": 0xE06C, "timer": 0xE1E0, "palette": 0xE1DD, "moon-star": 0xE410, "power": 0xE140}
ICON_CHARS = [chr(c) for c in ICONS.values()]

# name, file, pixel size, weight, optical size[, chars]
SPECS = [   # sizes are multiples of 4 px
    ("ui12",  "PlusJakartaSans.ttf",    12, 400, 14),
    ("ui12m", "PlusJakartaSans.ttf",    12, 500, 14),
    ("ui16",  "PlusJakartaSans.ttf",    16, 400, 14),
    ("ui16b", "PlusJakartaSans.ttf",    16, 600, 14),
    ("ui24b", "PlusJakartaSans.ttf",    24, 700, 24, ASCII),   # clock and temperature only
    ("ui40b", "PlusJakartaSans.ttf",    40, 700, 32, ASCII),
    ("rd12",  "Literata.ttf", 12, 400, 12),
    ("rd16",  "Literata.ttf", 16, 400, 12),
    ("rd20",  "Literata.ttf", 20, 400, 12),
    ("rd12i", "Literata-Italic.ttf", 12, 400, 12),   # reader italic, bold and bold italic: the converters mark <em> and <strong>
    ("rd16i", "Literata-Italic.ttf", 16, 400, 12),
    ("rd20i", "Literata-Italic.ttf", 20, 400, 12),
    ("rd12b", "Literata.ttf", 12, 700, 12),
    ("rd16b", "Literata.ttf", 16, 700, 12),
    ("rd20b", "Literata.ttf", 20, 700, 12),
    ("rd12z", "Literata-Italic.ttf", 12, 700, 12),
    ("rd16z", "Literata-Italic.ttf", 16, 700, 12),
    ("rd20z", "Literata-Italic.ttf", 20, 700, 12),
    ("ic12",  "Lucide.ttf", 12, 400, 0, ICON_CHARS),   # icon sizes follow the design: 12 lock, 14 tiles, 16 rows, 18 status, 20 back
    ("ic14",  "Lucide.ttf", 14, 400, 0, ICON_CHARS),
    ("ic16",  "Lucide.ttf", 16, 400, 0, ICON_CHARS),
    ("ic18",  "Lucide.ttf", 18, 400, 0, ICON_CHARS),
    ("ic20",  "Lucide.ttf", 20, 400, 0, ICON_CHARS),
]

# The battery glyphs are redrawn on the pixel grid: at these sizes the outline's 1.5 px stroke and three bars smear together.
# px: body w, body h, bar w, bar gap, nub w, nub h, corner radius. The stroke and the bars' clearance from the wall are 1 px.
BATTERY = {14: (12, 8, 2, 1, 1, 2, 2), 16: (14, 9, 2, 2, 1, 3, 2.5), 18: (14, 10, 2, 2, 2, 4, 2.5), 20: (15, 11, 3, 1, 2, 5, 3)}
BARS = {ICONS["battery-full"]: 3, ICONS["battery-medium"]: 2, ICONS["battery-low"]: 1, ICONS["battery-charging"]: 0}
SS = 8   # supersampling for the drawn glyphs

def battery(px, bars, bolt):
    bw, bh, barw, gap, nubw, nubh, r = BATTERY[px]
    w, h = bw + 1 + nubw, bh
    im = Image.new("L", (w * SS, h * SS), 0); d = ImageDraw.Draw(im)
    def rect(x0, y0, x1, y1, rad=0, fill=255): d.rounded_rectangle([x0 * SS, y0 * SS, x1 * SS - 1, y1 * SS - 1], rad * SS, fill=fill)
    rect(0, 0, bw, bh, r); rect(1, 1, bw - 1, bh - 1, r - 1, 0)
    rect(bw + 1, (bh - nubh) / 2, w, (bh + nubh) / 2, nubw / 2)
    for i in range(bars): x = 2 + i * (barw + gap); rect(x, 2, x + barw, bh - 2, 0.5)
    if bolt:   # Lucide's bolt (m11 7 -3 5 h4 l-3 5), through a break in the outline
        cx, k, t = bw / 2, bh / 12, 1.4
        rect(cx - bw * 0.22, 0, cx + bw * 0.22, bh, 0, 0)
        pts = [(cx + (x - 10) * k, bh / 2 + (y - 12) * k) for x, y in ((11, 7), (8, 12), (12, 12), (9, 17))]
        d.line([(x * SS, y * SS) for x, y in pts], fill=255, width=round(t * SS), joint="curve")
        for x, y in (pts[0], pts[-1]): d.ellipse([(x - t / 2) * SS, (y - t / 2) * SS, (x + t / 2) * SS, (y + t / 2) * SS], fill=255)
    return im.resize((w, h), Image.LANCZOS)

def icon_fix(g, px):
    code, w, h, adv, dY, dX, bm = g
    if code not in BARS or px not in BATTERY: return g
    im = battery(px, BARS[code], code == ICONS["battery-charging"])
    nw, nh = im.size   # keep the old glyph's centre
    return (code, nw, nh, adv, round(dY - h / 2 + nh / 2), round(dX + w / 2 - nw / 2), im.tobytes())

def build(file, px, weight, opsz, chars=CHARS):
    font = ImageFont.truetype(os.path.join(FONTS, file), px, layout_engine=ImageFont.Layout.BASIC)   # hinted advances, matching the hinted bitmaps
    try: axes = font.get_variation_axes()
    except OSError: axes = []   # static font (the icon font)
    if axes:
        vals = [weight if a["name"] == b"Weight" else max(a["minimum"], min(a["maximum"], opsz)) if a["name"] == b"Optical size" else a["default"] for a in axes]
        font.set_variation_by_axes(vals)
    pad = px * 2; W = px * 4
    glyphs = []   # (code, w, h, advance, dY, dX, bitmap)
    for ch in chars:
        img = Image.new("L", (W, W), 0)
        ImageDraw.Draw(img).text((pad, pad * 1.5), ch, font=font, fill=255, anchor="ls")
        adv = max(1, round(font.getlength(ch)))
        if ch == " ": adv = max(adv, -(-px * 27 // 100))   # hinting rounds small spaces down; keep at least 0.27 em
        box = img.getbbox()
        if not box: glyphs.append((ord(ch), 0, 0, adv, 0, 0, b"")); continue
        x0, y0, x1, y1 = box
        bm = img.crop(box).tobytes()
        glyphs.append((ord(ch), x1 - x0, y1 - y0, adv, int(pad * 1.5) - y0, x0 - pad, bm))
    if file == "Lucide.ttf": glyphs = [icon_fix(g, px) for g in glyphs]
    glyphs.sort(key=lambda g: g[0])   # the VLW loader finds codepoints by binary search
    by = {g[0]: g for g in glyphs}
    if ord("d") in by: ascent = by[ord("d")][4]; descent = by[ord("p")][2] - by[ord("p")][4]
    else: ascent = max(g[4] for g in glyphs); descent = max(0, max(g[2] - g[4] for g in glyphs))   # icons: box = union of the glyph inks
    out = struct.pack(">6i", len(glyphs), 11, px, 0, ascent, descent)
    for code, w, h, adv, dY, dX, _ in glyphs: out += struct.pack(">7i", code, h, w, adv, dY, dX, 0)
    for g in glyphs: out += g[6]
    return out

def main():
    lines = ["// Generated by fontgen.py from the TTFs in fonts/ (OFL; Lucide icons ISC). Do not edit.", "#pragma once", "#include <pgmspace.h>", ""]
    lines.append("// Lucide icon glyphs as UTF-8 strings (codepoints from lucide-static/font/lucide.css)")
    for n, c in ICONS.items(): lines.append(f'#define IC_{n.upper().replace("-", "_")} "' + "".join(f"\\x{b:02X}" for b in chr(c).encode()) + '"')
    lines.append("")
    total = 0
    for spec in SPECS:
        name, file, px, weight, opsz = spec[:5]
        data = build(file, px, weight, opsz, *spec[5:]); total += len(data)
        lines.append(f"// {file} {px}px weight {weight} opsz {opsz}: {len(data)} bytes")
        lines.append(f"static const uint8_t PROGMEM font_{name}[] = {{")
        for i in range(0, len(data), 32): lines.append("  " + ",".join(str(b) for b in data[i:i + 32]) + ",")
        lines.append("};\n")
        print(f"{name:6s} {file:14s} {px:2d}px w{weight} {len(data):7d} bytes")
    with open(os.path.join(HERE, "src", "fonts_data.h"), "w") as f: f.write("\n".join(lines))
    print(f"total {total} bytes -> src/fonts_data.h")

if __name__ == "__main__": main()
