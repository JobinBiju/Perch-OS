// EPUB -> Perch book text, a port of book_convert.py. Runs in the browser (portal) or in node (tests).
// convertEpub(zip) -> {title, author, text, chapters:[{title, bytes}]}; `zip` is a loaded JSZip.
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
  function ascii(s) {
    s = Array.from(s, c => REPL[c] !== undefined ? REPL[c] : c).join("").normalize("NFKD");
    let o = ""; for (const c of s) { const k = c.codePointAt(0); if (k < 128 && (k >= 32 || c === "\n")) o += c; } return o;
  }
  function paragraphs(doc) {
    doc = doc.replace(/<(style|script|head)\b[\s\S]*?<\/\1>/gi, "");
    doc = doc.replace(/<br\s*\/?>/gi, "\n");
    doc = doc.replace(/<\/(p|div|h[1-6]|li|tr|blockquote|dd|dt)>/gi, "\n");
    doc = doc.replace(/<[^>]+>/g, "");
    const out = [];
    for (let p of unescape(doc).split("\n")) { p = ascii(p.replace(/\s+/g, " ")).trim(); if (p) out.push(p); }
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
        if (nav) for (const m of nav[0].matchAll(/<a\b[^>]*href="([^"#]+)[^"]*"[^>]*>([\s\S]*?)<\/a>/g)) { const f = join(d, m[1]); if (!(f in toc)) toc[f] = ascii(unescape(m[2].replace(/<[^>]+>/g, ""))).replace(/\s+/g, " ").trim(); }
      }
    }
    const chapters = [];   // [title, paragraphs[]]
    for (const f of spine) {
      const doc = await text(zip, f); if (doc === null) continue;
      const paras = paragraphs(doc);
      if (!(f in toc) && paras.length && ["contents", "table of contents"].includes(paras[0].toLowerCase())) continue;   // skip the TOC page
      if (f in toc) chapters.push([ascii(toc[f]), []]);
      if (!chapters.length) { if (!paras.length) continue; chapters.push([ascii(title), []]); }
      chapters[chapters.length - 1][1].push(...paras);
    }
    const kept = chapters.filter(c => c[1].length);
    let out = "\x02" + ascii(title) + "\n" + ascii(author) + "\n";
    for (const [t, ps] of kept) { out += "\x01" + t + "\n"; for (const p of ps) out += p + "\n"; }
    return {title: ascii(title), author: ascii(author), text: out, chapters: kept.map(c => ({title: c[0], bytes: c[1].reduce((a, p) => a + p.length, 0)}))};
  }
  function slug(title) { const s = ascii(title).toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 40); return (s || "book") + ".txt"; }
  const api = {convertEpub, slug, ascii, paragraphs};
  if (typeof module !== "undefined" && module.exports) module.exports = api; else root.BookConvert = api;
})(typeof globalThis !== "undefined" ? globalThis : this);
