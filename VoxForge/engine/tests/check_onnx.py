"""CI check: content encoder + trained voice.onnx produce sensible audio with onnxruntime."""
import sys
import numpy as np
import onnxruntime as ort
import soundfile as sf
from voxforge_engine.audio import load_mono, resample

enc_path, voice_path, wav_path, out_path = sys.argv[1:5]
x, sr = load_mono(wav_path)
x16 = resample(x, sr, 16000)[None].astype(np.float32)
enc = ort.InferenceSession(enc_path, providers=["CPUExecutionProvider"])
units = enc.run(None, {"audio": x16})[0]
print("units", units.shape)
assert units.shape[2] == 768 and units.shape[1] > 100

T = units.shape[1]
f0 = np.full((1, T), 261.6, np.float32)
voice = ort.InferenceSession(voice_path, providers=["CPUExecutionProvider"])
y = voice.run(None, {"units": units.astype(np.float32), "f0": f0})[0]
print("audio", y.shape, "peak", float(np.abs(y).max()), "rms", float(np.sqrt(np.mean(y ** 2))))
assert y.shape == (1, T * 960)
assert np.isfinite(y).all() and np.abs(y).max() > 1e-3
sf.write(out_path, y[0], 48000)
print("OK")
