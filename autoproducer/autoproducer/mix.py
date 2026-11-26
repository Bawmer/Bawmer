"""Stage 6: mix. Per-track EQ carving + compression, returns, master chain, gain staging.

Gain staging is measured, not guessed: after each render the per-stem peaks are read
and faders moved toward the role targets below (≈ -12 dBFS for the loudest element).
"""
from __future__ import annotations

from .actionlog import log
from .live import Live
from .project import Project

# high-pass corner per role (Hz); None = leave the low end alone
HPF = {"drums": None, "kick": None, "bass": 30, "chords": 180, "hook": 250, "pad": 200, "top_loop": 300,
       "tonal_loop": 180, "vocal": 120, "fx": 150, "snare": 120, "hat": 400, "perc": 200}
# target stem PEAK (dBFS) before the master bus
PEAK_TARGET = {"drums": -12.0, "kick": -12.0, "bass": -13.0, "snare": -15.0, "hat": -21.0, "perc": -20.0,
               "chords": -18.0, "hook": -16.0, "pad": -22.0, "top_loop": -20.0, "tonal_loop": -20.0,
               "vocal": -15.0, "fx": -18.0}
START_FADER_DB = {"drums": -3.0, "bass": -5.0, "chords": -9.0, "hook": -7.0, "pad": -12.0,
                  "top_loop": -12.0, "tonal_loop": -11.0, "vocal": -7.0, "fx": -10.0,
                  "kick": -3.0, "snare": -6.0, "hat": -12.0, "perc": -12.0}
SEND_REVERB = {"chords": 0.15, "hook": 0.18, "pad": 0.25, "vocal": 0.2, "snare": 0.08, "fx": 0.2}
SEND_DELAY = {"hook": 0.12, "vocal": 0.15}
COMPRESS = {"bass": (-18.0, 4.0), "vocal": (-20.0, 3.0), "chords": (-20.0, 2.0)}


def _eq_hpf(live: Live, kind: str, idx: int, hz: float, dev: int | None = None) -> str:
    if dev is None:
        dev = live.load_stock(kind, idx, "EQ Eight").index
    params = live.params(kind, idx, dev)
    on = live.find_param(params, "1 Filter On A")
    ftype = live.find_param(params, "1 Filter Type A")
    freq = live.find_param(params, "1 Frequency A")
    if not (on and ftype and freq):
        return "EQ Eight band-1 parameters not found"
    live.set_param(kind, idx, dev, on["index"], 1.0)
    live.set_param(kind, idx, dev, ftype["index"], 1.0)  # 1 = Low Cut 12 dB (0 = Low Cut 48 dB)
    _, s = live.set_param_display(kind, idx, dev, freq["index"], hz)
    return f"HPF {s}"


def _kick_bass_unmask(live: Live, kind: str, idx: int, part: str, dev: int) -> str | None:
    """Small bell cut so kick and bass don't fight: bass -3 dB @ 60 Hz (kick fundamental)
    when the kick is in a shared drum track; kick track gets -2 dB @ 120 Hz."""
    if part not in ("bass", "drums", "kick"):
        return None
    params = live.params(kind, idx, dev)
    on = live.find_param(params, "2 Filter On A")
    ftype = live.find_param(params, "2 Filter Type A")
    freq = live.find_param(params, "2 Frequency A")
    gain = live.find_param(params, "2 Gain A")
    if not (on and ftype and freq and gain):
        return "EQ Eight band-2 parameters not found"
    live.set_param(kind, idx, dev, on["index"], 1.0)
    live.set_param(kind, idx, dev, ftype["index"], 3.0)  # 3 = Bell
    hz, db = (60, -3.0) if part == "bass" else (120, -2.0)
    live.set_param_display(kind, idx, dev, freq["index"], hz)
    live.set_param_display(kind, idx, dev, gain["index"], db)
    return f"bell {db:+.0f} dB @ {hz} Hz"


def _compress(live: Live, kind: str, idx: int, thr: float, ratio: float) -> str:
    d = live.load_stock(kind, idx, "Compressor")
    params = live.params(kind, idx, d.index)
    for names, val in ((("Threshold",), thr), (("Ratio",), ratio), (("Attack",), 10.0), (("Release",), 80.0)):
        p = live.find_param(params, *names)
        if p:
            live.set_param_display(kind, idx, d.index, p["index"], val)
    return f"comp {thr:.0f} dB {ratio:.0f}:1"


def mix_tracks(live: Live, proj: Project) -> dict:
    tracks = proj.state["tracks"]
    decisions: dict = {}
    rev, dly = tracks.get("return_reverb"), tracks.get("return_delay")
    for part, t in tracks.items():
        if t["kind"] != "track":
            continue
        k, i = t["kind"], t["index"]
        d = decisions.setdefault(part, [])
        if live.cfg.allow_stock_utility:
            eq = live.load_stock(k, i, "EQ Eight").index
            if HPF.get(part):
                d.append(_eq_hpf(live, k, i, HPF[part], eq))
            r = _kick_bass_unmask(live, k, i, part, eq)
            if r:
                d.append(r)
            if part in COMPRESS:
                d.append(_compress(live, k, i, *COMPRESS[part]))
        d.append("fader " + live.set_volume_db(k, i, START_FADER_DB.get(part, -10.0)))
        if rev and part in SEND_REVERB:
            live.set_param(k, i, "mixer", f"send{rev['send']}", SEND_REVERB[part])
            d.append(f"reverb send {SEND_REVERB[part]:.2f}")
        if dly and part in SEND_DELAY:
            live.set_param(k, i, "mixer", f"send{dly['send']}", SEND_DELAY[part])
            d.append(f"delay send {SEND_DELAY[part]:.2f}")
        if part in ("bass", "kick", "drums"):
            d.append("ducking: bass/chords/pad/tonal loop duck -9 dB on each kick (Utility envelope)")
    for slot in ("return_reverb", "return_delay"):
        if slot in tracks and live.cfg.allow_stock_utility:
            r = tracks[slot]
            decisions[slot] = [_eq_hpf(live, "return", r["index"], 250)]
    proj.state["mix"] = decisions
    proj.save_state()
    log("mix_tracks", decisions=decisions)
    return decisions


def master_chain(live: Live, proj: Project) -> dict:
    """Glue comp -> EQ -> Utility (mono < 120 Hz) -> Limiter on the master track."""
    if not live.cfg.allow_stock_utility:
        raise RuntimeError("master chain needs stock utility devices (allow_stock_utility: true) "
                           "or whitelisted master presets; neither is configured")
    m = ("master", 0)
    out: dict = {}
    glue = live.load_stock(*m, "Glue Compressor")
    gp = live.params(*m, glue.index)
    for names, val in ((("Threshold",), -12.0), (("Ratio",), 2.0), (("Attack",), 10.0), (("Makeup",), 1.0)):
        p = live.find_param(gp, *names)
        if p:
            live.set_param_display(*m, glue.index, p["index"], val)
    p = live.find_param(gp, "Release")
    if p and p["quantized"]:
        live.set_param(*m, glue.index, p["index"], p["max"])  # "Auto"
    out["glue"] = {"index": glue.index, "threshold": -12.0, "ratio": 2.0}
    eq = live.load_stock(*m, "EQ Eight")
    ep = live.params(*m, eq.index)
    on, ft, fr = (live.find_param(ep, "1 Filter On A"), live.find_param(ep, "1 Filter Type A"),
                  live.find_param(ep, "1 Frequency A"))
    if on and ft and fr:
        live.set_param(*m, eq.index, on["index"], 1.0)
        live.set_param(*m, eq.index, ft["index"], 1.0)
        live.set_param_display(*m, eq.index, fr["index"], 25)
    out["eq"] = {"index": eq.index, "hpf_hz": 25}
    util = live.load_stock(*m, "Utility")
    up = live.params(*m, util.index)
    bm, bf = live.find_param(up, "Bass Mono"), live.find_param(up, "Bass Freq")
    if bm:
        live.set_param(*m, util.index, bm["index"], 1.0)
    if bf:
        live.set_param_display(*m, util.index, bf["index"], 120)
    out["mono_below_hz"] = 120 if bm else None
    out["utility"] = {"index": util.index}
    lim = live.load_stock(*m, "Limiter")
    lp = live.params(*m, lim.index)
    ceil = live.find_param(lp, "Ceiling")
    gain = live.find_param(lp, "Gain")
    if ceil:
        live.set_param_display(*m, lim.index, ceil["index"], -1.3)
    if gain:
        live.set_param_display(*m, lim.index, gain["index"], 4.0)
    out["limiter"] = {"index": lim.index, "ceiling_param": ceil["index"] if ceil else None,
                      "gain_param": gain["index"] if gain else None, "ceiling_db": -1.3, "gain_db": 4.0,
                      "param_names": [p["name"] for p in lp]}
    proj.state["master"] = out
    proj.save_state()
    log("master_chain", **out)
    return out


def gain_stage(live: Live, proj: Project, stem_peaks: dict[str, float]) -> dict:
    """Move faders so each stem's peak approaches its role target. Returns applied deltas."""
    tracks = proj.state["tracks"]
    faders = proj.state.setdefault("faders", {p: START_FADER_DB.get(p, -10.0) for p in tracks})
    applied = {}
    for part, peak in stem_peaks.items():
        if part not in tracks or tracks[part]["kind"] != "track" or peak <= -90:
            continue
        delta = max(-12.0, min(12.0, PEAK_TARGET.get(part, -18.0) - peak))
        if abs(delta) < 0.5:
            continue
        new = max(-40.0, min(6.0, faders.get(part, -10.0) + delta))
        live.set_volume_db("track", tracks[part]["index"], new)
        faders[part] = new
        applied[part] = round(delta, 2)
    proj.save_state()
    log("gain_stage", applied=applied)
    return applied
