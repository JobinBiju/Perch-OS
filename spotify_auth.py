# One-time Spotify login for Perch (PKCE flow, no client secret).
# Usage: python spotify_auth.py
# Opens the Spotify consent page in your browser, catches the redirect on 127.0.0.1:8888,
# exchanges the code for a refresh token and sends it to the board over USB serial.
import base64, hashlib, secrets, urllib.parse, urllib.request, json, webbrowser, glob, sys, time
from http.server import HTTPServer, BaseHTTPRequestHandler

CLIENT_ID = "a862fb74b77a4705892db33f25e8838b"
REDIRECT = "http://127.0.0.1:8888/callback"
SCOPES = "user-read-playback-state user-modify-playback-state user-read-currently-playing"

verifier = base64.urlsafe_b64encode(secrets.token_bytes(64)).rstrip(b"=").decode()
challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
    "client_id": CLIENT_ID, "response_type": "code", "redirect_uri": REDIRECT, "scope": SCOPES,
    "code_challenge_method": "S256", "code_challenge": challenge})

code = None
class H(BaseHTTPRequestHandler):
    def do_GET(self):
        global code
        q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        code = q.get("code", [None])[0]
        self.send_response(200); self.end_headers()
        self.wfile.write(b"<h2>Perch: Spotify linked. You can close this tab.</h2>" if code else b"<h2>Login failed.</h2>")
    def log_message(self, *a): pass

print("Opening Spotify login in your browser...")
webbrowser.open(url)
srv = HTTPServer(("127.0.0.1", 8888), H)
while code is None: srv.handle_request()

data = urllib.parse.urlencode({"client_id": CLIENT_ID, "grant_type": "authorization_code", "code": code,
                               "redirect_uri": REDIRECT, "code_verifier": verifier}).encode()
tok = json.load(urllib.request.urlopen(urllib.request.Request("https://accounts.spotify.com/api/token", data)))
refresh = tok["refresh_token"]
print("Got refresh token. Sending to the board...")

ports = glob.glob("/dev/cu.usbmodem*")
if not ports: print("Board not found on USB. Token:", refresh); sys.exit(1)
import serial
s = serial.Serial(ports[0], 115200, timeout=3)
s.reset_input_buffer()
s.write(f"spc {CLIENT_ID}\n".encode()); time.sleep(0.3)
s.write(f"sp {refresh}\n".encode())
t0 = time.time()
while time.time() - t0 < 5:
    line = s.readline().decode(errors="replace").strip()
    if "spotify" in line: print("Board:", line); break
print("Done. Open the Spotify app on the board.")
