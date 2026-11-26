"""Stage 1: scan the (read-only) sample library and cache audio features in SQLite.

Per file: duration, BPM, key + confidence, brightness (spectral centroid), transient
density (onsets/second), low-end ratio, and a role guess (kick/snare/hat/perc/bass/
loop/pad/vocal/fx) from folder names first, audio features second.
Files are opened read-only; unchanged files (same size + mtime) are skipped on rescans.
"""
from __future__ import annotations

import os
import sqlite3
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

import numpy as np

from .actionlog import log
from .theory import Key, estimate_key

AUDIO_EXT = {".wav", ".aif", ".aiff", ".flac", ".mp3", ".ogg"}
ROLES = ("kick", "snare", "clap", "hat", "perc", "bass", "loop", "pad", "vocal", "fx")
# keyword -> role, checked against lowercase path parts (most specific first)
KEYWORDS = [
    ("kick", "kick"), ("bd_", "kick"), ("808", "bass"),
    ("snare", "snare"), ("snr", "snare"), ("clap", "clap"),
    ("hihat", "hat"), ("hi-hat", "hat"), ("hat", "hat"), ("hh_", "hat"), ("cymbal", "hat"), ("ride", "hat"),
    ("shaker", "perc"), ("perc", "perc"), ("tom", "perc"), ("rim", "perc"), ("conga", "perc"),
    ("vocal", "vocal"), ("vox", "vocal"), ("acapella", "vocal"), ("chant", "vocal"),
    ("riser", "fx"), ("uplifter", "fx"), ("downlifter", "fx"), ("sweep", "fx"), ("impact", "fx"),
    ("fx", "fx"), ("sfx", "fx"), ("noise", "fx"), ("crash", "fx"), ("reverse", "fx"),
    ("bass", "bass"), ("sub", "bass"),
    ("pad", "pad"), ("atmos", "pad"), ("ambien", "pad"), ("drone", "pad"), ("texture", "pad"),
    ("loop", "loop"), ("chord", "loop"), ("melod", "loop"), ("keys", "loop"), ("synth", "loop"),
    ("guitar", "loop"), ("piano", "loop"), ("top", "loop"), ("groove", "loop"), ("break", "loop"),
]
DRUM_ROLES = {"kick", "snare", "clap", "hat", "perc"}
TONAL_ROLES = {"bass", "loop", "pad", "vocal"}

SCHEMA = """
CREATE TABLE IF NOT EXISTS samples (
  path TEXT PRIMARY KEY, size INTEGER, mtime REAL, duration REAL, sr INTEGER,
  bpm REAL, key TEXT, key_conf REAL, brightness REAL, transient_density REAL,
  low_ratio REAL, rms_db REAL, role TEXT, role_source TEXT, is_loop INTEGER,
  error TEXT, indexed_at REAL
);
CREATE INDEX IF NOT EXISTS idx_role ON samples(role);
"""


@dataclass
class SampleInfo:
    path: str
    size: int
    mtime: float
    duration: float = 0.0
    sr: int = 0
    bpm: float = 0.0
    key: str = ""
    key_conf: float = 0.0
    brightness: float = 0.0
    transient_density: float = 0.0
    low_ratio: float = 0.0
    rms_db: float = -120.0
    role: str = "fx"
    role_source: str = "features"
    is_loop: int = 0
    error: str = ""
    indexed_at: float = 0.0


def role_from_path(path: str) -> str | None:
    parts = [p.lower() for p in Path(path).parts[-4:]]
    # filename first (most specific), then parent folders, innermost first
    for part in [parts[-1]] + parts[-2::-1]:
        for kw, role in KEYWORDS:
            if kw in part:
                return role
    return None


def role_from_features(dur: float, bright: float, dens: float, low: float, tonal_conf: float) -> str:
    if dur < 1.2:  # one-shots
        if low > 0.55 and bright < 1500:
            return "kick"
        if bright > 6000:
            return "hat"
        if bright > 2500:
            return "snare" if dens < 4 else "perc"
        return "perc"
    if low > 0.6 and bright < 900:
        return "bass"
    if dens < 0.8 and tonal_conf > 0.2:
        return "pad"
    if dens > 1.5:
        return "loop"
    return "fx"


def analyse(path: str, max_seconds: float = 60.0) -> dict:
    import librosa

    y, sr = librosa.load(path, sr=22050, mono=True, duration=max_seconds)
    full_dur = float(librosa.get_duration(path=path))
    if y.size == 0 or not np.any(y):
        return {"duration": full_dur, "sr": sr, "rms_db": -120.0}
    rms = float(np.sqrt(np.mean(y ** 2)))
    onset_env = librosa.onset.onset_strength(y=y, sr=sr)
    onsets = librosa.onset.onset_detect(onset_envelope=onset_env, sr=sr, units="time")
    dur = len(y) / sr
    dens = len(onsets) / max(dur, 1e-3)
    centroid = float(np.mean(librosa.feature.spectral_centroid(y=y, sr=sr)))
    spec = np.abs(np.fft.rfft(y[: sr * 10]))
    freqs = np.fft.rfftfreq(min(len(y), sr * 10), 1 / sr)
    low = float(spec[freqs < 150].sum() / max(spec.sum(), 1e-9))
    bpm = 0.0
    if dur >= 2.0:
        tempo, _ = librosa.beat.beat_track(onset_envelope=onset_env, sr=sr)
        bpm = float(np.atleast_1d(tempo)[0])
        bpm = _bpm_from_length(dur, bpm)
    key, conf = Key(0, "major"), 0.0
    if dur >= 1.0:
        chroma = librosa.feature.chroma_cqt(y=y, sr=sr)
        key, conf = estimate_key(chroma.mean(axis=1))
    return {"duration": full_dur, "sr": int(sr), "bpm": round(bpm, 2), "key": str(key),
            "key_conf": round(conf, 3), "brightness": round(centroid, 1),
            "transient_density": round(dens, 3), "low_ratio": round(low, 3),
            "rms_db": round(20 * np.log10(max(rms, 1e-9)), 2)}


def _bpm_from_length(dur: float, est: float) -> float:
    """Loops are usually whole bars: prefer the BPM that makes dur = 1,2,4,8,16 bars of 4/4
    when it is within 4% of the onset-based estimate (or of its double/half)."""
    best, best_err = est, 1.0
    for bars in (1, 2, 4, 8, 16, 32):
        cand = bars * 4 * 60.0 / dur
        if not 60 <= cand <= 200:
            continue
        for f in (1.0, 2.0, 0.5):
            if est <= 0:
                continue
            err = abs(cand - est * f) / cand + (0.0 if f == 1.0 else 0.015)  # prefer the direct reading
            if err < best_err:
                best, best_err = cand, err
    return best if best_err <= 0.055 else est


def iter_audio(roots: Iterable[str]) -> Iterable[Path]:
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if not d.startswith(".")]
            for f in filenames:
                if Path(f).suffix.lower() in AUDIO_EXT and not f.startswith("._"):
                    yield Path(dirpath) / f


def open_db(db_path: str | Path) -> sqlite3.Connection:
    con = sqlite3.connect(str(db_path))
    con.executescript(SCHEMA)
    con.row_factory = sqlite3.Row
    return con


def index_library(roots: list[str], db_path: str | Path, progress=print) -> dict:
    con = open_db(db_path)
    stats = {"seen": 0, "analysed": 0, "skipped": 0, "errors": 0}
    t0 = time.time()
    for p in iter_audio(roots):
        stats["seen"] += 1
        st = p.stat()
        row = con.execute("SELECT size, mtime FROM samples WHERE path=?", (str(p),)).fetchone()
        if row and row["size"] == st.st_size and abs(row["mtime"] - st.st_mtime) < 1e-3:
            stats["skipped"] += 1
            continue
        info = SampleInfo(path=str(p), size=st.st_size, mtime=st.st_mtime, indexed_at=time.time())
        try:
            feats = analyse(str(p))
            for k, v in feats.items():
                setattr(info, k, v)
            hinted = role_from_path(str(p))
            if hinted:
                info.role, info.role_source = hinted, "path"
            else:
                info.role = role_from_features(info.duration, info.brightness, info.transient_density,
                                               info.low_ratio, info.key_conf)
            info.is_loop = int(info.duration >= 1.5 and info.bpm > 0 and info.role != "fx")
            stats["analysed"] += 1
        except Exception as e:  # unreadable/corrupt file: record and continue
            info.error = f"{type(e).__name__}: {e}"
            stats["errors"] += 1
        d = asdict(info)
        con.execute(f"INSERT OR REPLACE INTO samples ({','.join(d)}) VALUES ({','.join('?' * len(d))})",
                    tuple(d.values()))
        if stats["seen"] % 50 == 0:
            con.commit()
            progress(f"  indexed {stats['seen']} files ({stats['analysed']} new, {stats['errors']} errors)")
    con.commit()
    stats["seconds"] = round(time.time() - t0, 1)
    stats["by_role"] = dict(con.execute(
        "SELECT role, COUNT(*) FROM samples WHERE error='' GROUP BY role").fetchall())
    log("index_samples", **stats)
    con.close()
    return stats


def load_all(db_path: str | Path) -> list[dict]:
    con = open_db(db_path)
    rows = [dict(r) for r in con.execute("SELECT * FROM samples WHERE error='' ORDER BY path")]
    con.close()
    return rows
