// EPUB -> Perch book text, a port of book_convert.py. Runs in the browser (portal) or in node (tests).
// convertEpub(zip) -> {title, author, text, chapters:[{title, bytes}]}; `zip` is a loaded JSZip.
// Text format: "\x02Title\nAuthor\n", chapters start with "\x01Title\n", one paragraph per line; \x03 toggles italic,
// \x04 toggles bold; U+00AD marks a hyphenation point.
// Call setHyphenationPatterns(text of hyph-en-us.txt) first to get soft hyphens in English books.
(function (root) {
  const REPL = {"‘": "'", "’": "'", "“": '"', "”": '"', "–": "-", "—": "-", "…": "...",
    " ": " ", "­": "", "•": "*", "©": "(c)", "′": "'", "″": '"', "×": "x"};
  const ENT = {amp: "&", lt: "<", gt: ">", quot: '"', apos: "'", nbsp: " ", shy: "­", mdash: "—", ndash: "–",
    hellip: "…", lsquo: "‘", rsquo: "’", ldquo: "“", rdquo: "”", copy: "©", reg: "®", trade: "™",
    bull: "•", middot: "·", times: "×", deg: "°", eacute: "é", egrave: "è", agrave: "à", aacute: "á",
    ouml: "ö", uuml: "ü", auml: "ä", ccedil: "ç", ntilde: "ñ", iacute: "í", oacute: "ó", uacute: "ú", euro: "€", pound: "£"};
  function unescape(s) {
    return s.replace(/&(#x[0-9a-f]+|#\d+|[a-z][a-z0-9]*);/gi, (m, e) => {
      if (e[0] === "#") { const n = e[1] === "x" || e[1] === "X" ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10); return isNaN(n) ? m : String.fromCodePoint(n); }
      const v = ENT[e]; if (v !== undefined) return v;
      if (typeof document !== "undefined") { const t = document.createElement("textarea"); t.innerHTML = m; return t.value; }
      return m;
    });
  }
  function ascii(s) {   // for file names only
    s = Array.from(s, c => REPL[c] !== undefined ? REPL[c] : c).join("").normalize("NFKD");
    let o = ""; for (const c of s) { const k = c.codePointAt(0); if (k < 128 && (k >= 32 || c === "\n")) o += c; } return o;
  }
  // Book text is UTF-8 limited to what the board's fonts have: ASCII, Latin-1 Supplement, Latin Extended-A and a few
  // punctuation marks. Anything else is reduced to its base letters (NFKD) or dropped.
  const MAP = {"\u00A0": " ", "\u2011": "-", "\u2010": "-", "\u2212": "-", "\u00AD": "", "\u200B": "", "\u200C": "", "\u200D": "", "\uFEFF": "", "\t": " "};
  const KEEP = new Set("‘’“”‚„‹›–—…•′″€£");
  function clean(s) {
    let o = "";
    for (const c of s.normalize("NFC")) {
      const m = MAP[c]; if (m !== undefined) { o += m; continue; }
      const k = c.codePointAt(0);
      if ((k >= 32 && k < 127) || (k >= 0xA1 && k < 0x180) || KEEP.has(c) || c === "\n" || k === 3 || k === 4) { o += c; continue; }
      if (k < 32 || k < 0xA1) continue;
      for (const b of c.normalize("NFKD")) { const j = b.codePointAt(0); if (j >= 32 && j < 127) o += b; }
    }
    return o;
  }
  // Liang hyphenation with the hyph-utf8 en-US patterns (served by the board at /hyph-en-us.txt). A soft hyphen
  // (U+00AD) goes into every break point; the reader breaks there only when the word would not fit.
  let HYPH = null;
  function setHyphenationPatterns(text) {
    const pats = new Map(); let max = 0;
    for (const line of text.split("\n")) {
      const p = line.trim(); if (!/^[.a-z0-9]+$/.test(p)) continue;
      const letters = p.replace(/\d/g, ""), v = new Array(letters.length + 1).fill(0); let li = 0;
      for (const ch of p) { if (ch >= "0" && ch <= "9") v[li] = Math.max(v[li], +ch); else li++; }
      pats.set(letters, v); max = Math.max(max, letters.length);
    }
    HYPH = {pats, max};
  }
  function hyphenPoints(w) {   // w lowercase; returns the indexes a hyphen may precede (left min 2, right min 3)
    const s = "." + w + ".", n = s.length, pts = new Array(n + 1).fill(0);
    for (let i = 0; i < n; i++) for (let j = i + 1; j <= Math.min(n, i + HYPH.max); j++) {
      const v = HYPH.pats.get(s.slice(i, j)); if (!v) continue;
      for (let k = 0; k < v.length; k++) if (v[k] > pts[i + k]) pts[i + k] = v[k];
    }
    const out = []; for (let m = 2; m <= w.length - 3; m++) if (pts[m + 1] % 2) out.push(m); return out;
  }
  function hyphenate(p) {
    if (!HYPH) return p;
    return p.replace(/[A-Za-z]{5,}/g, w => { const pts = hyphenPoints(w.toLowerCase()); let o = "", last = 0; for (const m of pts) { o += w.slice(last, m) + "\u00AD"; last = m; } return o + w.slice(last); });
  }
  function paragraphs(doc) {
    doc = doc.replace(/<(style|script|head)\b[\s\S]*?<\/\1>/gi, "");
    doc = doc.replace(/<br\s*\/?>/gi, "\n");
    doc = doc.replace(/<\/?(em|i|cite)\b[^>]*>/gi, "\x03");                 // italic on/off
    doc = doc.replace(/<\/?(strong|b)\b[^>]*>/gi, "\x04");                  // bold on/off
    doc = doc.replace(/<h[1-6]\b[^>]*>/gi, "\n\x04").replace(/<\/h[1-6]>/gi, "\x04\n");   // headings inside a chapter: bold paragraphs
    doc = doc.replace(/<\/(p|div|li|tr|blockquote|dd|dt)>/gi, "\n");
    doc = doc.replace(/<[^>]+>/g, "");
    const out = [];
    for (let p of unescape(doc).split("\n")) {
      p = clean(p).replace(/\s+/g, " ").trim(); if (!p.replace(/[\x03\x04]/g, "").trim()) continue;
      p = p.replace(/\x03\x03|\x04\x04/g, "");                                 // empty runs
      for (const t of ["\x03", "\x04"]) if ((p.split(t).length - 1) % 2) p = p.endsWith(t) ? p.slice(0, -1) : p + t;   // styles end with the paragraph
      out.push(p);
    }
    return out;
  }
  function dirname(p) { const i = p.lastIndexOf("/"); return i < 0 ? "" : p.slice(0, i); }
  function normpath(p) {   // posixpath.normpath for zip paths
    const out = []; for (const seg of p.split("/")) { if (seg === "" || seg === ".") continue; if (seg === "..") out.pop(); else out.push(seg); } return out.join("/");
  }
  function join(base, rel) { return normpath(base ? base + "/" + rel : rel); }
  async function text(zip, path) { const f = zip.file(path); return f ? f.async("string") : null; }

  async function convertEpub(zip) {
    const names = Object.keys(zip.files);
    const container = await text(zip, "META-INF/container.xml"); if (!container) throw new Error("not an EPUB (no container.xml)");
    const opfPath = /full-path="([^"]+)"/.exec(container)[1]; const base = dirname(opfPath);
    const opf = await text(zip, opfPath); if (!opf) throw new Error("missing " + opfPath);
    const tm = /<dc:title[^>]*>([\s\S]*?)<\/dc:title>/.exec(opf); const title = tm ? unescape(tm[1]).trim() : "Untitled";
    const am = /<dc:creator[^>]*>([\s\S]*?)<\/dc:creator>/.exec(opf); const author = am ? unescape(am[1]).trim() : "";
    const lm = /<dc:language[^>]*>([\s\S]*?)<\/dc:language>/.exec(opf); const lang = lm ? lm[1].trim().toLowerCase() : "en";
    const hyph = lang.startsWith("en") ? hyphenate : p => p;   // the patterns are English only
    const items = {};
    for (const m of opf.matchAll(/<item\b[^>]*?\bid="([^"]+)"[^>]*?\bhref="([^"]+)"/g)) items[m[1]] = m[2];
    for (const m of opf.matchAll(/<item\b[^>]*?\bhref="([^"]+)"[^>]*?\bid="([^"]+)"/g)) items[m[2]] = m[1];
    const spine = []; for (const m of opf.matchAll(/<itemref\b[^>]*\bidref="([^"]+)"/g)) if (items[m[1]]) spine.push(join(base, items[m[1]]));
    // chapter titles: NCX (EPUB 2), else the nav document (EPUB 3)
    const toc = {};
    const ncx = names.find(n => n.endsWith(".ncx"));
    if (ncx) {
      const x = await text(zip, ncx), d = dirname(ncx);
      for (const m of x.matchAll(/<navPoint[\s\S]*?<text>([\s\S]*?)<\/text>[\s\S]*?<content src="([^"#]+)/g)) { const f = join(d, m[2]); if (!(f in toc)) toc[f] = unescape(m[1]).replace(/\s+/g, " ").trim(); }
    } else {
      const navId = Object.keys(items).find(id => new RegExp('<item\\b[^>]*\\bid="' + id + '"[^>]*\\bproperties="[^"]*\\bnav\\b').test(opf));
      if (navId) {
        const navPath = join(base, items[navId]), x = await text(zip, navPath), d = dirname(navPath);
        const nav = /<nav\b[^>]*epub:type="toc"[\s\S]*?<\/nav>/.exec(x);
        if (nav) for (const m of nav[0].matchAll(/<a\b[^>]*href="([^"#]+)[^"]*"[^>]*>([\s\S]*?)<\/a>/g)) { const f = join(d, m[1]); if (!(f in toc)) toc[f] = clean(unescape(m[2].replace(/<[^>]+>/g, ""))).replace(/[\x03\x04]/g, "").replace(/\s+/g, " ").trim(); }
      }
    }
    const chapters = [];   // [title, paragraphs[]]
    for (const f of spine) {
      const doc = await text(zip, f); if (doc === null) continue;
      const paras = paragraphs(doc);
      if (!(f in toc) && paras.length && ["contents", "table of contents"].includes(paras[0].replace(/[\x03\x04]/g, "").toLowerCase())) continue;   // skip the TOC page
      if (f in toc) chapters.push([clean(toc[f]), []]);
      if (!chapters.length) { if (!paras.length) continue; chapters.push([clean(title), []]); }
      chapters[chapters.length - 1][1].push(...paras.map(hyph));
    }
    const kept = chapters.filter(c => c[1].length);
    let out = "\x02" + clean(title) + "\n" + clean(author) + "\n";
    for (const [t, ps] of kept) { out += "\x01" + t + "\n"; for (const p of ps) out += p + "\n"; }
    return {title: clean(title), author: clean(author), text: out, chapters: kept.map(c => ({title: c[0], bytes: c[1].reduce((a, p) => a + p.length, 0)}))};
  }
  function slug(title) { const s = ascii(title).toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 40); return (s || "book") + ".txt"; }
  const api = {convertEpub, slug, ascii, clean, title: s => clean(s).replace(/[\x03\x04]/g, ""), paragraphs, setHyphenationPatterns, hyphenate};
  if (typeof module !== "undefined" && module.exports) module.exports = api; else root.BookConvert = api;
})(typeof globalThis !== "undefined" ? globalThis : this);
