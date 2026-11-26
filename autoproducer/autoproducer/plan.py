"""Stage 2: choose key, BPM, progression, section map + energy curve, and shortlist samples.

Every choice is driven by `random.Random(seed)` so the same config + library index
gives the same plan. The plan is plain JSON (plan.json) and is what checkpoint A approves.
"""
from __future__ import annotations

import json
import random
from collections import Counter
from pathlib import Path

from . import genres
from .config import Config
from .theory import Key, bpm_ratio_match, compatible

# per role: how many candidates to keep, and whether key / BPM must match
ROLE_RULES = {
    "kick": {"n": 3, "key": False, "bpm": False, "one_shot": True},
    "snare": {"n": 3, "key": False, "bpm": False, "one_shot": True},
    "clap": {"n": 3, "key": False, "bpm": False, "one_shot": True},
    "hat": {"n": 3, "key": False, "bpm": False, "one_shot": True},
    "perc": {"n": 3, "key": False, "bpm": False, "one_shot": True},
    "top_loop": {"n": 3, "key": False, "bpm": True, "one_shot": False},
    "tonal_loop": {"n": 3, "key": True, "bpm": True, "one_shot": False},
    "vocal": {"n": 3, "key": True, "bpm": True, "one_shot": False},
    "riser": {"n": 3, "key": False, "bpm": False, "one_shot": False},
    "impact": {"n": 3, "key": False, "bpm": False, "one_shot": False},
    "downlifter": {"n": 3, "key": False, "bpm": False, "one_shot": False},
}
FX_KEYWORDS = {
    "riser": ("riser", "uplift", "build", "sweep up", "rise"),
    "impact": ("impact", "hit", "boom", "crash"),
    "downlifter": ("downlift", "down", "fall", "reverse"),
}
KEY_CONF_MIN = 0.15


class PlanError(ValueError):
    pass  # below this, a sample's key is treated as unknown


def _fx_kind(path: str) -> str | None:
    p = Path(path).name.lower()
    for kind, kws in FX_KEYWORDS.items():
        if any(k in p for k in kws):
            return kind
    return None


def _candidates(role: str, samples: list[dict]) -> list[dict]:
    if role in ("kick", "snare", "clap", "hat", "perc"):
        return [s for s in samples if s["role"] == role and s["duration"] < 2.5]
    if role == "top_loop":
        return [s for s in samples if s["is_loop"] and s["role"] in ("loop", "perc", "hat")
                and s["transient_density"] >= 2.0 and s["low_ratio"] < 0.35]
    if role == "tonal_loop":
        return [s for s in samples if s["is_loop"] and s["role"] in ("loop", "pad")
                and s["key_conf"] >= KEY_CONF_MIN]
    if role == "vocal":
        return [s for s in samples if s["role"] == "vocal" and s["duration"] >= 1.0]
    if role in FX_KEYWORDS:
        return [s for s in samples if s["role"] == "fx" and _fx_kind(s["path"]) == role]
    return []


def choose_bpm(cfg: Config, samples: list[dict], rng: random.Random) -> int:
    if cfg.bpm != "auto":
        return int(round(cfg.bpm))
    lo, hi = genres.PROFILES[cfg.genre]["bpm"]
    loops = [s for s in samples if s["is_loop"] and s["bpm"]]
    def closeness(sample_bpm: float, b: int) -> float:
        f = bpm_ratio_match(sample_bpm, b, 0.03)
        return 0.0 if f is None else 1.0 - abs(sample_bpm * f - b) / (0.03 * b)

    scores = {b: round(sum(closeness(s["bpm"], b) for s in loops), 6) for b in range(lo, hi + 1)}
    best = max(scores.values()) if scores else 0
    options = [b for b, v in scores.items() if v == best]
    return rng.choice(options)


def choose_key(cfg: Config, samples: list[dict], rng: random.Random) -> Key:
    if cfg.key != "auto":
        return Key.parse(cfg.key)
    mode = genres.PROFILES[cfg.genre]["mode"]
    tonal = [Key.parse(s["key"]) for s in samples
             if s["role"] in ("loop", "pad", "vocal", "bass") and s["key_conf"] >= KEY_CONF_MIN and s["key"]]
    scores = Counter()
    for t in range(12):
        k = Key(t, mode)
        scores[k] = sum(3 if compatible(sk, k, 0)[0] else 1 if compatible(sk, k, 1)[0] else 0
                        for sk in tonal)
    best = max(scores.values()) if scores else 0
    options = sorted([k for k, v in scores.items() if v == best], key=lambda k: k.tonic)
    return rng.choice(options)


def section_map(cfg: Config, bpm: int) -> list[dict]:
    lo, hi = cfg.length_seconds
    secs_per_bar = 4 * 60.0 / bpm
    bars = {s: genres.SECTION_BARS[s] for s in cfg.structure}
    order = list(cfg.structure)

    def total(b):
        return sum(b[s] for s in order) * secs_per_bar

    # grow / shrink in 8-bar steps on the most flexible sections until inside the window
    grow = [s for s in ("drop", "drop2", "verse", "verse2", "breakdown") if s in bars]
    shrink = [s for s in ("verse2", "verse", "intro", "outro", "breakdown") if s in bars]
    for _ in range(40):
        t = total(bars)
        if t < lo and grow:
            s = min(grow, key=lambda x: bars[x])
            bars[s] += 8
        elif t > hi and shrink:
            cands = [s for s in shrink if bars[s] > 8]
            if not cands:
                break
            s = max(cands, key=lambda x: bars[x])
            bars[s] -= 8
        else:
            break
    out, bar = [], 0
    for name in order:
        e = genres.SECTION_ENERGY[name] * (0.6 + 0.4 * float(cfg.energy))
        out.append({"name": name, "start_bar": bar, "bars": bars[name],
                    "energy": round(min(1.0, e), 2),
                    "layers": sorted(genres.SECTION_LAYERS[name])})
        bar += bars[name]
    return out


def shortlist(samples: list[dict], key: Key, bpm: int, rng: random.Random,
              skip_roles: tuple = ()) -> dict:
    out: dict = {}
    used: set[str] = set()
    for role, rule in ROLE_RULES.items():
        if role in skip_roles:
            out[role] = []
            continue
        picked = []
        for s in _candidates(role, samples):
            if s["path"] in used:
                continue
            entry = {"path": s["path"], "duration": s["duration"], "bpm": s["bpm"], "key": s["key"]}
            if rule["bpm"]:
                f = bpm_ratio_match(s["bpm"], bpm, 0.08)
                if f is None:
                    continue
                entry["bpm_factor"] = f
            if rule["key"]:
                ok, shift = compatible(Key.parse(s["key"]), key, 2)
                if not ok:
                    continue
                entry["transpose"] = shift
            picked.append(entry)
        rng.shuffle(picked)
        # prefer no-transpose / exact-tempo first, keep order otherwise seeded
        picked.sort(key=lambda e: (abs(e.get("transpose", 0)), abs(e.get("bpm_factor", 1) - 1)))
        out[role] = picked[: rule["n"]]
        used.update(e["path"] for e in out[role][:1])
    return out


def make_plan(cfg: Config, samples: list[dict]) -> dict:
    rng = random.Random(cfg.seed)
    prof = genres.PROFILES[cfg.genre]
    bpm = choose_bpm(cfg, samples, rng)
    key = choose_key(cfg, samples, rng)
    prog = rng.choice(prof["progressions"])
    alt = rng.choice([p for p in prof["progressions"] if p != prog] or [prog])
    sections = section_map(cfg, bpm)
    total_bars = sum(s["bars"] for s in sections)
    drum_one_shots = ("kick", "snare", "clap", "hat", "perc")
    skip = drum_one_shots if cfg.drums["mode"] == "midi" else ()
    sl = shortlist(samples, key, bpm, rng, skip)
    missing = [r for r, v in sl.items() if not v and r not in skip]
    if cfg.drums["mode"] == "audio_hits":
        need = [r for r in ("kick", "hat") if not sl[r]]
        if not sl["snare"] and not sl["clap"]:
            need.append("snare or clap")
        if need:
            raise PlanError("drums.mode audio_hits needs one-shots in your library for: " + ", ".join(need)
                            + ". Add them (or name the folders so the indexer finds them) and re-run index.")
    # layers we cannot fill from the library are removed (never substituted)
    def available(layer: str) -> bool:
        if layer in ("bass", "chords", "hook", "pad"):
            return True  # MIDI parts on whitelisted instrument presets
        if layer in ("kick", "snare", "hat", "perc"):
            if cfg.drums["mode"] == "midi":
                return True
            return bool(sl[layer]) or (layer == "snare" and bool(sl["clap"]))
        return layer not in missing

    for s in sections:
        s["layers"] = [l for l in s["layers"] if available(l)]
    plan = {
        "song_name": cfg.song_name, "seed": cfg.seed, "genre": cfg.genre, "subgenre": cfg.subgenre,
        "bpm": bpm, "key": str(key), "progression": prog, "alt_progression": alt,
        "progression_names": [_roman(d, key) for d in prog],
        "swing": prof["swing"], "drum_style": prof["drum"], "bass_style": prof["bass"],
        "chord_style": prof["chords"], "sections": sections, "total_bars": total_bars,
        "duration_seconds": round(total_bars * 240.0 / bpm, 1),
        "samples": sl, "unfilled_roles": missing,
        "drum_mode": cfg.drums["mode"],
        "sketch": {"section": next((s["name"] for s in sections if s["name"].startswith("drop")),
                                   sections[0]["name"]), "bars": 8},
    }
    return plan


def _roman(degree: int, key: Key) -> str:
    numerals = ["I", "II", "III", "IV", "V", "VI", "VII"]
    major_q = [1, 0, 0, 1, 1, 0, -1]  # 1 major, 0 minor, -1 dim (for major mode)
    minor_q = [0, -1, 1, 0, 0, 1, 1]
    q = (major_q if key.mode == "major" else minor_q)[degree % 7]
    n = numerals[degree % 7]
    return n if q == 1 else (n.lower() + ("°" if q == -1 else ""))


def save(plan: dict, path: str | Path) -> None:
    Path(path).write_text(json.dumps(plan, indent=2), encoding="utf-8")


def load(path: str | Path) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def describe(plan: dict) -> str:
    lines = [f"# Plan: {plan['song_name']}  (seed {plan['seed']})", "",
             f"- Genre: {plan['genre']} {plan['subgenre']}".rstrip(),
             f"- BPM {plan['bpm']}, key {plan['key']}, length {plan['total_bars']} bars "
             f"= {int(plan['duration_seconds'] // 60)}:{int(plan['duration_seconds'] % 60):02d}",
             f"- Progression: {' – '.join(plan['progression_names'])} (alt for verse2/drop2)",
             "", "| Section | Bars | Start bar | Energy | Layers |", "|---|---|---|---|---|"]
    for s in plan["sections"]:
        lines.append(f"| {s['name']} | {s['bars']} | {s['start_bar'] + 1} | {'█' * round(s['energy'] * 10)} "
                     f"{s['energy']:.2f} | {', '.join(s['layers'])} |")
    lines += ["", "## Shortlisted samples"]
    for role, items in plan["samples"].items():
        if not items and plan.get("drum_mode") == "midi" and role in ("kick", "snare", "clap", "hat", "perc"):
            continue
        if not items:
            lines.append(f"- **{role}:** none compatible in library (layer dropped)")
            continue
        first = items[0]
        extra = []
        if "transpose" in first:
            extra.append(f"transpose {first['transpose']:+d} st")
        if "bpm_factor" in first and first["bpm_factor"] != 1:
            extra.append(f"tempo ×{first['bpm_factor']}")
        lines.append(f"- **{role}:** `{first['path']}`" + (f" ({', '.join(extra)})" if extra else "")
                     + (f" +{len(items) - 1} alternates" if len(items) > 1 else ""))
    return "\n".join(lines)
