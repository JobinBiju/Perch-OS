// Perch sync: reading positions shared between a household's devices.
// There are no accounts. A "home key" (a long random string every device of one owner holds) is the only credential;
// everything is stored under a hash of it, so one home never sees another's data.
//   PUT /v1/pos/<book>   {ch, rel, len, pct, t, dev}   keep it if it is newer than what is stored
//   GET /v1/pos/<book>   the newest position, or {}
//   GET /v1/pos          every book's newest position
// <book> is the book's file name on the devices. ch = chapter index, rel/len = bytes into the chapter and its length
// (devices scale by len, so differently converted copies still land on the same paragraph), t = epoch seconds.
const CORS = {"access-control-allow-origin": "*", "access-control-allow-methods": "GET, PUT, OPTIONS", "access-control-allow-headers": "authorization, content-type"};
const json = (body, status = 200) => new Response(JSON.stringify(body), {status, headers: {"content-type": "application/json", "cache-control": "no-store", ...CORS}});
async function homeOf(key) {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(key));
  return [...new Uint8Array(d)].slice(0, 16).map(b => b.toString(16).padStart(2, "0")).join("");
}
const int = (v, lo, hi) => Number.isInteger(v) && v >= lo && v <= hi;

export default {
  async fetch(req, env) {
    if (req.method === "OPTIONS") return new Response(null, {status: 204, headers: CORS});
    const url = new URL(req.url);
    const key = (req.headers.get("authorization") || "").replace(/^Bearer\s+/i, "") || url.searchParams.get("k") || "";
    if (key.length < 24 || key.length > 128) return json({error: "home key required"}, 401);
    const m = /^\/v1\/pos(?:\/([A-Za-z0-9._-]{1,64}))?$/.exec(url.pathname);
    if (!m) return json({error: "not found"}, 404);
    const prefix = (await homeOf(key)) + ":pos:";

    if (!m[1]) {   // the positions live in each key's metadata, so listing needs no further reads
      if (req.method !== "GET") return json({error: "method"}, 405);
      const out = {}; let cursor;
      do { const l = await env.PERCH.list({prefix, cursor}); for (const k of l.keys) if (k.metadata) out[k.name.slice(prefix.length)] = k.metadata; cursor = l.list_complete ? null : l.cursor; } while (cursor);
      return json(out);
    }
    const name = prefix + m[1];
    if (req.method === "GET") { const cur = await env.PERCH.getWithMetadata(name); return json(cur.metadata || {}); }
    if (req.method !== "PUT") return json({error: "method"}, 405);
    let b; try { b = await req.json(); } catch (e) { return json({error: "bad json"}, 400); }
    if (!int(b.ch, 0, 4096) || !int(b.rel, 0, 1 << 30) || !int(b.len, 0, 1 << 30) || !int(b.t, 1600000000, 4102444800)) return json({error: "bad position"}, 400);
    const pos = {ch: b.ch, rel: b.rel, len: b.len, pct: int(b.pct, 0, 100) ? b.pct : 0, t: b.t, dev: String(b.dev || "").slice(0, 32)};
    const cur = (await env.PERCH.getWithMetadata(name)).metadata;
    if (cur && cur.t >= pos.t) return json({stored: false, pos: cur});   // someone read further along in time: keep theirs
    await env.PERCH.put(name, "", {metadata: pos});
    return json({stored: true, pos});
  },
};
