"""Audio loading, resampling, slicing and f0 extraction shared by training and inference."""
from __future__ import annotations

import numpy as np

CONTENT_SR = 16000          # content encoder input rate
FRAME_HOP_16K = 320         # 50 frames per second
OUTPUT_SR = 48000           # voice model output rate
OUTPUT_HOP = 960            # output samples per frame (48000 / 50)
F0_MIN, F0_MAX = 50.0, 1100.0


def load_mono(path: str) -> tuple[np.ndarray, int]:
    import soundfile as sf
    data, sr = sf.read(path, dtype="float32", always_2d=True)
    return data.mean(axis=1), int(sr)


def resample(x: np.ndarray, sr_in: int, sr_out: int) -> np.ndarray:
    if sr_in == sr_out:
        return x.astype(np.float32)
    try:
        import torch
        import torchaudio.functional as AF
        y = AF.resample(torch.from_numpy(x.astype(np.float32))[None], sr_in, sr_out)[0]
        return y.numpy()
    except ImportError:
        from scipy.signal import resample_poly
        from math import gcd
        g = gcd(sr_in, sr_out)
        return resample_poly(x, sr_out // g, sr_in // g).astype(np.float32)


def normalize(x: np.ndarray, peak: float = 0.9) -> np.ndarray:
    m = float(np.max(np.abs(x))) if x.size else 0.0
    return x * (peak / m) if m > 1e-6 else x


def split_on_silence(x: np.ndarray, sr: int, top_db: float = 40.0, min_len: float = 1.0,
                     max_len: float = 8.0) -> list[np.ndarray]:
    """Cuts a recording into phrases (drops silence, limits segment length)."""
    hop = int(sr * 0.01)
    win = hop * 4
    if len(x) < win:
        return []
    frames = np.lib.stride_tricks.sliding_window_view(x, win)[::hop]
    rms_db = 20 * np.log10(np.sqrt(np.mean(frames ** 2, axis=1)) + 1e-9)
    voiced = rms_db > (rms_db.max() - top_db)

    segments, start = [], None
    for i, v in enumerate(np.append(voiced, False)):
        if v and start is None:
            start = i
        elif not v and start is not None:
            a, b = max(0, start * hop - hop * 5), min(len(x), i * hop + win + hop * 5)
            seg = x[a:b]
            step = int(max_len * sr)
            for k in range(0, len(seg), step):
                piece = seg[k:k + step]
                if len(piece) >= min_len * sr:
                    segments.append(piece)
            start = None
    return segments


def extract_f0(x16k: np.ndarray, n_frames: int, frames_per_second: int = 50) -> np.ndarray:
    """f0 in Hz at the given frame rate (0 = unvoiced). Uses WORLD (pyworld) when available."""
    hop = CONTENT_SR // frames_per_second
    try:
        import pyworld as pw
        x = x16k.astype(np.float64)
        f0, t = pw.harvest(x, CONTENT_SR, f0_floor=F0_MIN, f0_ceil=F0_MAX, frame_period=1000.0 * hop / CONTENT_SR)
        f0 = pw.stonemask(x, f0, t, CONTENT_SR)
    except ImportError:
        import librosa
        f0, _, _ = librosa.pyin(x16k, fmin=F0_MIN, fmax=F0_MAX, sr=CONTENT_SR,
                                frame_length=1024, hop_length=hop)
        f0 = np.nan_to_num(f0)
    f0 = np.asarray(f0, dtype=np.float32)
    if len(f0) < n_frames:
        f0 = np.pad(f0, (0, n_frames - len(f0)))
    return f0[:n_frames]
