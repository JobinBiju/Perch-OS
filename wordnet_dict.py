# Build an English dictionary for Perch from WordNet (Princeton University, free to use and share), as StarDict files.
# Usage:
#   curl -LO https://wordnetcode.princeton.edu/3.0/WordNet-3.0.tar.gz
#   python wordnet_dict.py WordNet-3.0.tar.gz dict
# (the full package, not WNdb-3.0.tar.gz: only the full one has the lists of irregular forms)
# then copy the dict folder to the root of the device's SD card. The input may also be a folder holding WordNet's
# database files (index.noun, data.noun, ... and the .exc lists). Each word gets its senses by part of speech, most
# common first, with synonyms; irregular forms ("went", "mice") point to their base word and carry its senses.
import os, re, struct, sys, tarfile

POS = [("noun", "noun"), ("verb", "verb"), ("adj", "adjective"), ("adv", "adverb")]

def reader(src):
    if os.path.isdir(src):
        def get(name):
            for root, _, files in os.walk(src):
                if name in files: return open(os.path.join(root, name), encoding="latin-1").read()
            return ""
        return get
    tf = tarfile.open(src); members = {os.path.basename(m.name): m for m in tf.getmembers() if m.isfile()}
    return lambda name: tf.extractfile(members[name]).read().decode("latin-1") if name in members else ""

def main(src, out):
    get = reader(src); senses = {}   # word -> [(pos label, gloss, synonyms)]
    for pos, label in POS:
        data = {}
        for line in get("data." + pos).splitlines():
            if not line or line.startswith("  "): continue   # the licence header
            head, _, gloss = line.partition(" | "); f = head.split()
            n = int(f[3], 16); words = [f[4 + 2 * i].replace("_", " ") for i in range(n)]
            words = [re.sub(r"\(\w+\)$", "", w) for w in words]   # adjective markers like (a)
            data[f[0]] = (words, gloss.strip())
        for line in get("index." + pos).splitlines():
            if not line or line.startswith("  "): continue
            f = line.split(); lemma = f[0].replace("_", " "); cnt = int(f[2])
            for off in f[-cnt:]:
                if off not in data: continue
                words, gloss = data[off]
                senses.setdefault(lemma, []).append((label, gloss, [w for w in words if w.lower() != lemma.lower()]))
    def text(word):
        out, last = [], None; n = 0
        for label, gloss, syn in senses[word]:
            if label != last: out.append(label); last = label; n = 0
            n += 1; m = re.match(r'(.*?)(?:;\s*("[^"]*".*))?$', gloss)   # the definition, then "examples"
            out.append(f"{n}. {m.group(1).strip()}")
            if m.group(2): out.append("e.g. " + "; ".join(e.strip() for e in re.findall(r'"[^"]*"', m.group(2))))
            if syn: out.append("also: " + ", ".join(syn[:4]))
        return "\n".join(out)
    entries = {w: text(w) for w in senses}
    for pos, _ in POS:   # irregular forms: "mice mouse"
        for line in get(pos + ".exc").splitlines():
            f = line.split()
            if len(f) < 2: continue
            form, base = f[0].replace("_", " "), f[1].replace("_", " ")
            if form not in entries and base in entries: entries[form] = f"see {base}\n" + entries[base]
    os.makedirs(out, exist_ok=True)
    words = sorted(entries, key=lambda w: (w.encode().lower(), w.encode()))   # StarDict's order
    idx, dat = bytearray(), bytearray()
    for w in words:
        t = entries[w].encode("utf-8"); idx += w.encode("utf-8") + b"\0" + struct.pack(">II", len(dat), len(t)); dat += t
    base = os.path.join(out, "wordnet")
    open(base + ".idx", "wb").write(idx); open(base + ".dict", "wb").write(dat)
    open(base + ".ifo", "w").write(f"StarDict's dict ifo file\nversion=2.4.2\nwordcount={len(words)}\nidxfilesize={len(idx)}\n"
                                   f"bookname=WordNet 3.0\nsametypesequence=m\n")
    print(f"wrote {base}.ifo/.idx/.dict: {len(words)} words, {(len(idx) + len(dat)) // 1024} KB")

if __name__ == "__main__":
    if len(sys.argv) != 3: sys.exit(__doc__ or "usage: wordnet_dict.py <WNdb-3.0.tar.gz or folder> <output folder>")
    main(sys.argv[1], sys.argv[2])
