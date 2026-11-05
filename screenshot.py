# Grab the ES3C28P screen over USB serial: python screenshot.py [out.png] [delay_s]
import serial, sys, time, glob
from PIL import Image
out = sys.argv[1] if len(sys.argv) > 1 else 'screen.png'
delay = float(sys.argv[2]) if len(sys.argv) > 2 else 0
pre = sys.argv[3].encode() if len(sys.argv) > 3 else b''
port = glob.glob('/dev/cu.usbmodem*')[0]
s = serial.Serial(port, 115200, timeout=2)
if pre: s.write(pre)
time.sleep(delay)
s.reset_input_buffer(); s.write(b'S')
line = b''
t0 = time.time()
while not line.startswith(b'SCR') and time.time() - t0 < 5:
    line = s.readline()
_, w, h = line.split(); w, h = int(w), int(h)
raw = b''
while len(raw) < w * h * 2:
    chunk = s.read(w * h * 2 - len(raw))
    if not chunk: break
    raw += chunk
img = Image.new('RGB', (w, h))
px = img.load()
for i in range(w * h):
    v = raw[2*i] << 8 | raw[2*i+1]
    px[i % w, i // w] = ((v >> 11) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3)
img.save(out); print('saved', out, len(raw), 'bytes')
