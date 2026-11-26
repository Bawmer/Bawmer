"""Stage 7: render (GUI export, resample fallback), measure, diagnose, fix, re-render (max 5).

Measurements (pyloudnorm, ITU-R BS.1770-4):
  integrated LUFS, true peak (4x oversampled), sample peak / clipped samples,
  unwanted silence, mono compatibility (L/R correlation, loudness lost when summed to
  mono, side energy below 120 Hz), file format (48 kHz / 24-bit), duration.
"""
from __future__ import annotations

import shutil
import subprocess
import time
from pathlib import Path

import numpy as np

from . import gui, mix
from .actionlog import log
from .live import Live
from .project import Project

MAX_ITERATIONS = 5


# ----------------------------------------------------------------------------- measuring
def true_peak_db(x: np.ndarray, sr: int) -> float:
    from scipy.signal import resample_poly
    up = resample_poly(x, 4, 1, axis=0)
    peak = float(np.max(np.abs(up))) if up.size else 0.0
    return 20 * np.log10(max(peak, 1e-12))


def silence_gaps(mono: np.ndarray, sr: int, thresh_db: float = -60.0, min_len: float = 2.0,
                 head: float = 0.5, tail: float = 6.0) -> list[tuple[float, float]]:
    """Runs of >= min_len seconds below thresh, ignoring the first `head` s and the
    reverb/fade `tail` after the last audible block."""
    hop = int(sr * 0.05)
    if len(mono) < hop:
        return []
    frames = mono[: len(mono) // hop * hop].reshape(-1, hop)
    rms_db = 20 * np.log10(np.sqrt(np.mean(frames ** 2, axis=1)) + 1e-12)
    loud = rms_db > thresh_db
    if not loud.any():
        return [(0.0, len(mono) / sr)]
    last = np.where(loud)[0][-1] * hop / sr
    gaps, start = [], None
    for i, ok in enumerate(loud):
        t = i * hop / sr
        if not ok and start is None:
            start = t
        elif ok and start is not None:
            if t - start >= min_len and start >= head:
                gaps.append((round(start, 2), round(t, 2)))
            start = None
    if start is not None and (len(mono) / sr - start) >= min_len and start < last - tail:
        gaps.append((round(start, 2), round(len(mono) / sr, 2)))
    return gaps


def measure(path: str | Path) -> dict:
    import pyloudnorm as pyln
    import soundfile as sf

    info = sf.info(str(path))
    x, sr = sf.read(str(path), always_2d=True, dtype="float64")
    if x.shape[1] == 1:
        x = np.repeat(x, 2, axis=1)
    meter = pyln.Meter(sr)
    lufs = float(meter.integrated_loudness(x))
    mono = x.mean(axis=1)
    lufs_mono = float(meter.integrated_loudness(np.stack([mono, mono], axis=1)))
    l, r = x[:, 0], x[:, 1]
    corr = float(np.corrcoef(l, r)[0, 1]) if np.std(l) > 0 and np.std(r) > 0 else 1.0
    from scipy.signal import butter, sosfilt
    sos = butter(4, 120, "lowpass", fs=sr, output="sos")
    side_low = sosfilt(sos, (l - r) / 2)
    mid_low = sosfilt(sos, (l + r) / 2)
    low_side_ratio = float(np.sqrt(np.mean(side_low ** 2)) / (np.sqrt(np.mean(mid_low ** 2)) + 1e-12))
    sample_peak = float(np.max(np.abs(x)))
    return {
        "file": str(path), "sr": sr, "subtype": info.subtype, "channels": info.channels,
        "duration": round(len(x) / sr, 2),
        "lufs_i": round(lufs, 2), "true_peak_dbtp": round(true_peak_db(x, sr), 2),
        "sample_peak_dbfs": round(20 * np.log10(max(sample_peak, 1e-12)), 2),
        "clipped_samples": int(np.sum(np.abs(x) >= 0.9999)),
        "silence_gaps": silence_gaps(mono, sr),
        "lr_correlation": round(corr, 3),
        "mono_loudness_loss_db": round(lufs - lufs_mono, 2),
        "low_side_ratio": round(low_side_ratio, 3),
    }


def peak_db(path: str | Path) -> float:
    import soundfile as sf
    x, _ = sf.read(str(path), always_2d=True)
    return round(20 * np.log10(max(float(np.max(np.abs(x))) if x.size else 0.0, 1e-12)), 2)


def evaluate(m: dict, target_lufs: float, tp_max: float, expect_sr: int = 48000,
             expect_subtype: str = "PCM_24", expect_seconds: float | None = None) -> list[dict]:
    """Return a list of failures; empty = pass."""
    f = []
    if abs(m["lufs_i"] - target_lufs) > 1.0:
        f.append({"check": "loudness", "value": m["lufs_i"], "target": f"{target_lufs}±1"})
    if m["true_peak_dbtp"] > tp_max:
        f.append({"check": "true_peak", "value": m["true_peak_dbtp"], "target": f"<= {tp_max}"})
    if m["clipped_samples"] > 0:
        f.append({"check": "clipping", "value": m["clipped_samples"], "target": 0})
    if m["silence_gaps"]:
        f.append({"check": "silence", "value": m["silence_gaps"], "target": "none"})
    if m["mono_loudness_loss_db"] > 3.0 or m["lr_correlation"] < 0.2:
        f.append({"check": "mono", "value": (m["mono_loudness_loss_db"], m["lr_correlation"]),
                  "target": "loss <= 3 dB, correlation >= 0.2"})
    if m["low_side_ratio"] > 0.1:
        f.append({"check": "low_mono", "value": m["low_side_ratio"], "target": "<= 0.1 below 120 Hz"})
    if m["sr"] != expect_sr or m["subtype"] != expect_subtype:
        f.append({"check": "format", "value": (m["sr"], m["subtype"]), "target": (expect_sr, expect_subtype)})
    if expect_seconds and m["duration"] < expect_seconds - 1:
        f.append({"check": "duration", "value": m["duration"], "target": f">= {expect_seconds:.1f}s"})
    return f


# ----------------------------------------------------------------------------- rendering
def _render_range(live: Live, plan: dict) -> float:
    beats = plan["total_bars"] * 4.0 + 8.0  # +2 bars for reverb/delay tails
    live.set_song("loop_start", 0.0)
    live.set_song("loop_length", beats)
    return beats * 60.0 / plan["bpm"]


def render_gui(live: Live, proj: Project, plan: dict, tag: str) -> dict:
    _render_range(live, plan)
    live.set_song("loop", 1)
    out = proj.renders / tag
    files = gui.export_audio(out, f"{proj.cfg.song_name}_{tag}", proj.cfg.export["stems"],
                             proj.cfg.gui["step_delay"], proj.cfg.gui["export_timeout"])
    live.set_song("loop", 0)
    master = [p for p in files if p.stem.endswith("Master") or p.stem == f"{proj.cfg.song_name}_{tag}"]
    stems = [p for p in files if p not in master]
    return {"master": master[0], "stems": stems, "method": "export_dialog"}


def render_resample(live: Live, proj: Project, plan: dict, tag: str) -> dict:
    """Fallback: record the master via a Resampling track in the arrangement."""
    seconds = _render_range(live, plan)
    rec_dir = proj.root / "Samples" / "Recorded"
    before = set(rec_dir.glob("*.wav")) if rec_dir.exists() else set()
    idx = live.create_track("audio", f"AP Resample {tag}")
    live.c.ap("/live/ap/track/routing", "track", idx, "input", "Resampling")
    live.set_track("track", idx, "current_monitoring_state", 2)  # Off: avoid feedback
    live.set_track("track", idx, "arm", 1)
    live.set_song("loop", 0)
    live.set_song("current_song_time", 0.0)
    live.set_song("record_mode", 1)
    live.transport("play_arrangement")
    time.sleep(seconds + 1.0)
    live.transport("stop")
    live.set_song("record_mode", 0)
    live.set_track("track", idx, "arm", 0)
    time.sleep(2.0)
    new = sorted(set(rec_dir.glob("*.wav")) - before, key=lambda p: p.stat().st_mtime)
    if not new:
        raise RuntimeError("resample recording produced no file in Samples/Recorded")
    dest = proj.renders / tag
    dest.mkdir(parents=True, exist_ok=True)
    master = dest / f"{proj.cfg.song_name}_{tag} Master.wav"
    shutil.copy2(new[-1], master)
    live.set_track("track", idx, "mute", 1)  # keep (non-destructive), but silent
    return {"master": master, "stems": [], "method": "resampling"}


def render(live: Live, proj: Project, plan: dict, tag: str) -> dict:
    if proj.cfg.gui["enabled"]:
        try:
            return render_gui(live, proj, plan, tag)
        except gui.GuiError as e:
            log("render_gui_failed", error=str(e))
    return render_resample(live, proj, plan, tag)


def stem_part(stem: Path, tracks: dict) -> str | None:
    s = stem.stem.lower()
    for part in sorted(tracks, key=len, reverse=True):
        label = part.replace("_", " ")
        if label in s or part in s:
            return part
    return None


# ----------------------------------------------------------------------------- fix loop
def apply_fixes(live: Live, proj: Project, failures: list[dict], m: dict) -> list[str]:
    master = proj.state["master"]
    lim = master["limiter"]
    actions = []
    target = proj.cfg.loudness_target_lufs
    for f in failures:
        c = f["check"]
        if c == "loudness" and lim.get("gain_param") is not None:
            delta = max(-4.0, min(4.0, target - m["lufs_i"]))
            lim["gain_db"] = round(max(0.0, min(18.0, lim["gain_db"] + delta)), 2)
            live.set_param_display("master", 0, lim["index"], lim["gain_param"], lim["gain_db"])
            actions.append(f"limiter gain -> {lim['gain_db']} dB (LUFS {m['lufs_i']} vs {target})")
        elif c in ("true_peak", "clipping") and lim.get("ceiling_param") is not None:
            over = max(0.3, m["true_peak_dbtp"] - proj.cfg.true_peak_max_dbtp + 0.3)
            lim["ceiling_db"] = round(max(-3.0, lim["ceiling_db"] - over), 2)
            live.set_param_display("master", 0, lim["index"], lim["ceiling_param"], lim["ceiling_db"])
            actions.append(f"limiter ceiling -> {lim['ceiling_db']} dB (TP {m['true_peak_dbtp']})")
        elif c in ("mono", "low_mono"):
            up = live.params("master", 0, master["utility"]["index"])
            w = live.find_param(up, "Width", "Stereo Width")
            if w:
                live.set_param_display("master", 0, master["utility"]["index"], w["index"], 80)
                actions.append("master width -> 80 %")
            bf = live.find_param(up, "Bass Freq")
            if c == "low_mono" and bf:
                live.set_param_display("master", 0, master["utility"]["index"], bf["index"], 160)
                actions.append("bass mono corner -> 160 Hz")
        elif c == "format":
            raise RuntimeError("export format wrong: open File > Export Audio/Video once and set WAV, "
                               "24-bit, 48000 Hz, then re-run `autoproducer render`")
        elif c in ("silence", "duration"):
            actions.append(f"UNFIXED {c}: {f['value']} (check the arrangement at those times)")
    proj.save_state()
    return actions


def render_and_qc(live: Live, proj: Project, plan: dict) -> dict:
    cfg = proj.cfg
    history = []
    expect = plan["total_bars"] * 240.0 / plan["bpm"]
    for it in range(1, MAX_ITERATIONS + 1):
        tag = f"r{it:02d}"
        r = render(live, proj, plan, tag)
        m = measure(r["master"])
        stem_peaks = {}
        for s in r["stems"]:
            part = stem_part(s, proj.state["tracks"])
            if part:
                stem_peaks[part] = peak_db(s)
        fails = evaluate(m, cfg.loudness_target_lufs, cfg.true_peak_max_dbtp,
                         cfg.export["sample_rate"], f"PCM_{cfg.export['bit_depth']}", expect)
        entry = {"iteration": it, "method": r["method"], "master": str(r["master"]),
                 "stems": [str(s) for s in r["stems"]], "measurements": m, "stem_peaks": stem_peaks,
                 "failures": fails, "actions": []}
        if it == 1 and stem_peaks:
            staged = mix.gain_stage(live, proj, stem_peaks)
            if staged:
                entry["actions"].append(f"gain staging: {staged}")
        if not fails and not entry["actions"]:
            history.append(entry)
            log("qc_pass", iteration=it, **m)
            break
        if fails:
            entry["actions"] += apply_fixes(live, proj, fails, m)
        history.append(entry)
        log("qc_iteration", iteration=it, failures=fails, actions=entry["actions"])
        if fails and all(a.startswith("UNFIXED") for a in entry["actions"]):
            break  # nothing left we can change automatically
    final = history[-1]
    result = {"passed": not final["failures"], "iterations": history, "final": final}
    proj.state["qc"] = result
    proj.save_state()
    return result


def mp3_preview(wav: Path) -> Path | None:
    if not shutil.which("ffmpeg"):
        log("mp3_skipped", reason="ffmpeg not on PATH")
        return None
    out = wav.with_suffix(".mp3")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(wav), "-b:a", "320k", str(out)], check=True)
    return out
