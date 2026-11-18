# Copy files to the board's dict folder over USB: python put.py wordnet.ifo wordnet.idx wordnet.dict
# The firmware's serial command "write /dict/<name> <bytes>" receives each one, 4 KB at a time with an acknowledgement.
import glob, os, sys, time, serial
s = serial.Serial(glob.glob("/dev/cu.usbmodem*")[0], 115200, timeout=5)
def wait(token, secs=20):
    buf = b""; t0 = time.time()
    while time.time() - t0 < secs:
        buf += s.read(max(1, s.in_waiting))
        if token in buf: return buf
    raise SystemExit(f"timed out waiting for {token!r}; got {buf[-200:]!r}")
for path in sys.argv[1:]:
    data = open(path, "rb").read(); name = os.path.basename(path); t0 = time.time()
    s.reset_input_buffer(); s.write(f"write /dict/{name} {len(data)}\n".encode()); wait(b"put: ready")
    for i in range(0, len(data), 4096):
        s.write(data[i:i + 4096]); wait(b"+")
        if i % (1024 * 1024) < 4096: print(f"\r{name}: {i // 1024} / {len(data) // 1024} KB", end="", flush=True)
    out = wait(b"bytes\n"); secs = time.time() - t0
    print(f"\r{name}: {out.decode(errors='replace').strip().splitlines()[-1]} in {secs:.0f} s ({len(data) / 1024 / max(secs, 0.1):.0f} KB/s)")
