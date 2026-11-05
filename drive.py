# Drive the firmware over serial: python drive.py 'cmd' 'sleep:1.5' 'shot:name.png' ...
# cmds: S=shot, b=back, 1-4=open app, 't x y' tap, 'd x0 y0 x1 y1' drag, 'k text' type into keyboard field
import serial, sys, time, glob
from PIL import Image
port = glob.glob('/dev/cu.usbmodem*')[0]
s = serial.Serial(port, 115200, timeout=2)
def shot(out):
    s.reset_input_buffer(); s.write(b'S')
    line = b''; t0 = time.time()
    while not line.startswith(b'SCR') and time.time() - t0 < 5: line = s.readline()
    _, w, h = line.split(); w, h = int(w), int(h)
    raw = b''
    while len(raw) < w * h * 2:
        chunk = s.read(w * h * 2 - len(raw))
        if not chunk: break
        raw += chunk
    img = Image.frombytes('RGB', (w, h), bytes(b for i in range(w*h) for b in ((raw[2*i] & 0xF8), ((raw[2*i] & 7) << 5) | ((raw[2*i+1] >> 3) & 0x1C), (raw[2*i+1] & 0x1F) << 3)))
    img.save(out); print('saved', out)
for a in sys.argv[1:]:
    if a.startswith('sleep:'): time.sleep(float(a[6:]))
    elif a.startswith('shot:'): shot(a[5:])
    elif a.startswith('log'): time.sleep(0.2); print(s.read_all().decode(errors='replace'))
    else: s.write((a + '\n').encode()); time.sleep(0.5)
