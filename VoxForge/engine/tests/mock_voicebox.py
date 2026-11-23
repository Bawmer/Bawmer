"""Minimal stand-in for the Voicebox REST API (CI only).

    python mock_voicebox.py [--port 17493] [--wav fixed.wav]

/generate/stream speaks the text with espeak-ng when it is installed, otherwise returns --wav
(or a synthetic vowel). The audio is sent with chunked transfer encoding, like FastAPI's
StreamingResponse in the real Voicebox.
"""
import argparse
import io
import json
import shutil
import subprocess
import sys
import tempfile
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROFILES = [{"id": "p-test-1", "name": "Test Voice", "language": "en", "voice_type": "cloned"}]
ARGS = None
REQUESTS = []


def synth(text: str, seed: int) -> bytes:
    if shutil.which("espeak-ng"):
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
            path = f.name
        subprocess.run(["espeak-ng", "-s", str(140 + seed % 20), "-w", path, text], check=True)
        return open(path, "rb").read()
    if ARGS.wav:
        return open(ARGS.wav, "rb").read()
    import numpy as np
    sr = 24000
    t = np.arange(int(sr * max(1.0, 0.07 * len(text)))) / sr
    x = 0.3 * np.sign(np.sin(2 * np.pi * 140 * t)) * (0.5 + 0.5 * np.sin(2 * np.pi * 3 * t))
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1), w.setsampwidth(2), w.setframerate(sr)
        w.writeframes((x * 32767).astype("<i2").tobytes())
    return buf.getvalue()


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            return self._json({"status": "healthy", "model_loaded": True, "backend_type": "mock"})
        if self.path == "/profiles":
            return self._json(PROFILES)
        self._json({"detail": "Not Found"}, 404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n) or b"{}")
        if self.path != "/generate/stream":
            return self._json({"detail": "Not Found"}, 404)
        if req.get("profile_id") not in [p["id"] for p in PROFILES]:
            return self._json({"detail": "Profile not found"}, 404)
        if not req.get("text"):
            return self._json({"detail": "text required"}, 422)
        REQUESTS.append(req)
        print("generate:", json.dumps(req), flush=True)
        data = synth(req["text"], int(req.get("seed") or 0))
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for i in range(0, len(data), 8192):
            chunk = data[i:i + 8192]
            self.wfile.write(f"{len(chunk):X}\r\n".encode() + chunk + b"\r\n")
        self.wfile.write(b"0\r\n\r\n")

    def log_message(self, *a):
        pass


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=17493)
    ap.add_argument("--wav", default="")
    ARGS = ap.parse_args()
    srv = ThreadingHTTPServer(("127.0.0.1", ARGS.port), H)
    print(f"mock Voicebox on http://127.0.0.1:{ARGS.port}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
