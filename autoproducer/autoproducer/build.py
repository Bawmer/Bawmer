"""Stages 3-5 in Live: tracks + sounds (load_sounds), MIDI clips (compose), arrangement + automation.

Track map is stored in project state so later stages (mix, render) find everything by role.
"""
from __future__ import annotations

import math
from pathlib import Path

from . import compose as comp
from .actionlog import log
from . import presets_pick
from .live import WARP_MODES, Live, WhitelistViolation, ramp
from .osc import LiveError
from .project import Project
from .theory import Key

MIDI_PARTS = ("drums", "bass", "chords", "hook", "pad")
AUDIO_PARTS = ("top_loop", "tonal_loop", "vocal", "fx")
HIT_PARTS = ("kick", "snare", "hat", "perc")
FILTERED = ("chords", "pad", "hook", "tonal_loop")   # get an Auto Filter for sweeps
DUCKED = ("bass", "chords", "pad", "tonal_loop")      # kick-synced ducking ("sidechain")
HITS_PER_MESSAGE = 150
COLORS = {"drums": 69, "bass": 1, "chords": 25, "hook": 13, "pad": 41, "top_loop": 59, "tonal_loop": 23,
          "vocal": 17, "fx": 4, "kick": 69, "snare": 70, "hat": 71, "perc": 72}


# ----------------------------------------------------------------------------- tracks & sounds
def setup_tracks(live: Live, proj: Project, plan: dict) -> dict:
    """Create tracks, load whitelisted presets (verified), add utility devices."""
    cfg = live.cfg
    tracks: dict = {}
    hit_mode = cfg.drums["mode"] == "audio_hits"
    midi_parts = [p for p in MIDI_PARTS if not (p == "drums" and hit_mode)]
    auto_roles = [p for p in midi_parts if str(cfg.presets.get(p, "")).strip().lower() == "auto"]
    ranked = presets_pick.pick_all(live.user_presets(), auto_roles, cfg.genre) if auto_roles else {}
    taken: set[str] = set()
    for part in midi_parts:
        idx = live.create_track("midi", part.replace("_", " ").title())
        if part in ranked:
            dev, preset, tried = None, None, []
            for cand in [c for c in ranked[part] if c not in taken] + [c for c in ranked[part] if c in taken]:
                try:
                    dev = live.load_preset("track", idx, cand, None, require_plugin=True)
                    preset = cand
                    break
                except (WhitelistViolation, LiveError) as e:
                    tried.append(f"{cand}: {e}")
                    if len(tried) >= 8:
                        break
            if dev is None:
                raise LiveError(f"presets.{part}: auto found no usable VST3 preset. Tried: "
                                + ("; ".join(tried) or "no preset name matched the role"))
            taken.add(preset)
            log("preset_auto_picked", role=part, preset=preset, rejected=tried)
        else:
            preset = cfg.presets[part]
            dev = live.load_preset("track", idx, preset, None, require_plugin=True)
        tracks[part] = {"kind": "track", "index": idx, "instrument": dev.name,
                        "instrument_class": dev.class_name, "preset": preset, "devices": [dev.name],
                        "auto_picked": part in ranked}
    audio_parts = list(AUDIO_PARTS) + (list(HIT_PARTS) if hit_mode else [])
    for part in audio_parts:
        idx = live.create_track("audio", part.replace("_", " ").title())
        tracks[part] = {"kind": "track", "index": idx, "devices": []}
    for name, slot in (("Reverb", "reverb"), ("Delay", "delay")):
        preset = cfg.presets.get(slot)
        if preset and "TODO" not in preset:
            ridx = live.create_track("return", name)
            dev = live.load_preset("return", ridx, preset, None)
            tracks[f"return_{slot}"] = {"kind": "return", "index": ridx, "send": ridx, "devices": [dev.name],
                                        "preset": preset}
    for part, t in tracks.items():
        if t["kind"] == "track":
            live.set_track("track", t["index"], "color_index", COLORS.get(part, 0))
    if cfg.allow_stock_utility:
        for part in FILTERED:
            if part in tracks:
                d = live.load_stock("track", tracks[part]["index"], "Auto Filter")
                tracks[part]["auto_filter"] = d.index
                tracks[part]["devices"].append("Auto Filter")
        for part in DUCKED:
            if part in tracks:
                d = live.load_stock("track", tracks[part]["index"], "Utility")
                tracks[part]["duck"] = d.index
                tracks[part]["devices"].append("Utility (duck)")
    proj.state["tracks"] = tracks
    proj.save_state()
    log("setup_tracks", tracks=tracks)
    return tracks


# ----------------------------------------------------------------------------- clips
def write_midi(live: Live, tracks: dict, parts: dict, note_map: dict) -> dict:
    counts = {}
    for part in MIDI_PARTS:
        if part not in tracks:
            continue
        clips = parts.get(part, [])
        if part == "drums":
            comp.map_drums(clips, note_map)
        n = 0
        for c in clips:
            live.midi_clip(tracks[part]["index"], c.start_beat, c.length_beats,
                           [x.as_tuple() for x in c.notes], f"{part} · {c.section}")
            n += len(c.notes)
        counts[part] = n
    log("write_midi", counts=counts)
    return counts


def write_hits(live: Live, proj: Project, tracks: dict, plan: dict, parts: dict) -> int:
    """drums.mode = audio_hits: every drum note becomes an unwarped one-shot clip of one of
    YOUR samples, at its humanized position, with velocity as clip gain (20·log10(vel/127) dB).
    Clap falls back to the snare sample (and vice versa); open hat/ride use the hat sample."""
    tag_to_part = {"kick": "kick", "snare": "snare", "clap": "snare", "hat_closed": "hat",
                   "hat_open": "hat", "ride": "hat", "perc": "perc"}
    files = {}
    for role in ("kick", "snare", "clap", "hat", "perc"):
        if plan["samples"].get(role):
            files[role] = str(proj.import_sample(plan["samples"][role][0]["path"], role))
    fallback = {"clap": "snare", "snare": "clap", "hat_closed": "hat", "hat_open": "hat", "ride": "hat"}
    by_track: dict[tuple[str, str], list[tuple[float, float]]] = {}
    for c in parts.get("drums", []):
        for note in c.notes:
            part = tag_to_part.get(note.tag, "perc")
            src = files.get(note.tag) or files.get(fallback.get(note.tag, part)) or files.get(part)
            if not src or part not in tracks:
                continue
            db = round(20 * math.log10(max(note.vel, 1) / 127.0), 2)
            by_track.setdefault((part, src), []).append((c.start_beat + note.start, db))
    n, trim_failed = 0, 0
    for (part, src), hits in by_track.items():
        hits.sort()
        dedup = [h for i, h in enumerate(hits) if i == 0 or h[0] - hits[i - 1][0] > 1e-3]
        t = tracks[part]["index"]
        for i in range(0, len(dedup), HITS_PER_MESSAGE):
            chunk = dedup[i:i + HITS_PER_MESSAGE]
            nxt = dedup[i + HITS_PER_MESSAGE][0] if i + HITS_PER_MESSAGE < len(dedup) else -1.0
            placed, failed = live.c.ap("/live/ap/arr/place_hits", t, src, part, float(nxt),
                                       *[v for h in chunk for v in h], timeout=60.0)
            n += int(placed)
            trim_failed += int(failed)
    log("write_hits", clips=n, trim_failed=trim_failed, tracks=sorted({p for p, _ in by_track}))
    return n


def _place_loop(live: Live, track: int, path: str, entry: dict, bpm: float, start: float, length: float,
                tonal: bool, name: str) -> list[int]:
    """Tile a warped loop across [start, start+length); verify tempo via warp markers."""
    factor = entry.get("bpm_factor", 1.0)
    src_bpm = (entry["bpm"] or bpm) * factor
    loop_beats = entry["duration"] * src_bpm / 60.0
    loop_beats = max(1.0, round(loop_beats))  # loops are whole beats
    made, pos = [], start
    while pos < start + length - 1e-6:
        ci = live.audio_clip(track, path, pos)
        live.clip_set(track, ci, "warping", 1)
        live.clip_set(track, ci, "warp_mode", WARP_MODES["complex_pro" if tonal else "beats"])
        if tonal and entry.get("transpose"):
            live.clip_set(track, ci, "pitch_coarse", int(entry["transpose"]))
        markers = live.warp_markers(track, ci)
        if len(markers) >= 2:
            (b0, s0), (b1, s1) = markers[0], markers[-1]
            implied = (b1 - b0) / max(s1 - s0, 1e-6) * 60.0
            if abs(implied - bpm) / bpm > 0.01:
                live.set_loop_tempo(track, ci, entry["duration"], loop_beats)
                log("warp_fixed", clip=name, implied_bpm=round(implied, 2), target=bpm)
        remaining = start + length - pos
        if remaining < loop_beats:
            live.clip_set(track, ci, "end_marker", float(remaining))
        live.clip_set(track, ci, "name", name)
        made.append(ci)
        pos += loop_beats
    return made


def write_audio(live: Live, proj: Project, tracks: dict, plan: dict) -> dict:
    bpm = float(plan["bpm"])
    placed: dict = {"top_loop": 0, "tonal_loop": 0, "vocal": 0, "fx": 0}
    files = {}
    for role in ("top_loop", "tonal_loop", "vocal", "riser", "impact", "downlifter"):
        if plan["samples"].get(role):
            e = plan["samples"][role][0]
            files[role] = (str(proj.import_sample(e["path"], role)), e)
    secs = plan["sections"]
    for si, s in enumerate(secs):
        start, length = s["start_bar"] * 4.0, s["bars"] * 4.0
        layers = set(s["layers"])
        for role, tonal in (("top_loop", False), ("tonal_loop", True), ("vocal", True)):
            if role in layers and role in files and role in tracks:
                path, e = files[role]
                if role == "vocal":  # vocal phrases on the 2nd half of each 8-bar phrase: call/response with hook
                    for ph in range(0, s["bars"], 8):
                        p0 = start + (ph + 4) * 4.0
                        if p0 < start + length:
                            placed[role] += len(_place_loop(live, tracks[role]["index"], path, e, bpm, p0,
                                                            min(16.0, start + length - p0), True,
                                                            f"vocal · {s['name']}"))
                    continue
                placed[role] += len(_place_loop(live, tracks[role]["index"], path, e, bpm, start, length,
                                                tonal, f"{role} · {s['name']}"))
        fx_t = tracks.get("fx", {}).get("index")
        if fx_t is None:
            continue
        if "riser" in layers and "riser" in files:  # ends exactly at the next section
            path, e = files["riser"]
            beats = e["duration"] * bpm / 60.0
            pos = max(start, start + length - beats)
            ci = live.audio_clip(fx_t, path, pos)
            live.clip_set(fx_t, ci, "warping", 0)
            placed["fx"] += 1
        if "impact" in layers and "impact" in files:
            ci = live.audio_clip(fx_t, files["impact"][0], start)
            live.clip_set(fx_t, ci, "warping", 0)
            placed["fx"] += 1
        if "downlifter" in layers and "downlifter" in files:
            ci = live.audio_clip(fx_t, files["downlifter"][0], start)
            live.clip_set(fx_t, ci, "warping", 0)
            placed["fx"] += 1
    log("write_audio", placed=placed)
    return placed


# ----------------------------------------------------------------------------- automation
def _clips_by_section(live: Live, track: int) -> dict[str, list[int]]:
    flat = live.c.ap("/live/ap/arr/list", track)
    out: dict[str, list[int]] = {}
    for i in range(0, len(flat), 3):
        name = str(flat[i])
        sec = name.split("·")[-1].strip() if "·" in name else ""
        out.setdefault(sec, []).append(i // 3)
    return out


def write_automation(live: Live, tracks: dict, plan: dict, parts: dict) -> dict:
    """Filter sweeps (intro open, build open, outro close), send rise into the drop,
    and kick-synced ducking on bass/chords/pad/tonal loop. Every clip of an automated
    track gets an envelope, so values never 'stick' from a previous section."""
    written = {"filter": 0, "send": 0, "duck": 0}
    secs = {s["name"]: s for s in plan["sections"]}
    kick_times: dict[str, list[float]] = {}
    for c in parts.get("drums", []):
        kick_times[c.section] = [n.start for n in c.notes if n.tag == "kick"]

    for part in FILTERED:
        t = tracks.get(part)
        if not t or "auto_filter" not in t:
            continue
        ti, dev = t["index"], t["auto_filter"]
        params = live.params("track", ti, dev)
        freq = live.find_param(params, "Frequency", "Filter Freq", "Cutoff")
        if not freq:
            log("automation_skipped", track=part, reason="Auto Filter frequency parameter not found")
            continue
        hz = {f: live.value_for_display("track", ti, dev, freq["index"], f) for f in (250, 600, 18000)}
        for sec, clip_ids in _clips_by_section(live, ti).items():
            if sec not in secs:
                continue
            L = secs[sec]["bars"] * 4.0
            if sec == "intro":
                steps = ramp(0, L, hz[250], hz[18000], 48, curve=2.0)
            elif sec.startswith("build"):
                steps = ramp(0, L, hz[600], hz[18000], 32, curve=1.5)
            elif sec == "outro":
                steps = ramp(0, L, hz[18000], hz[250], 48, curve=0.6)
            else:
                steps = [(0.0, L, hz[18000])]
            # audio loops are tiled: offset each tile's envelope by its position in the section
            offset = 0.0
            for ci in clip_ids:
                clip_len = float(live.clip_get(ti, ci, "length"))
                local = [(s0 - offset, d, v) for s0, d, v in steps if offset <= s0 < offset + clip_len]
                if not local:
                    nearest = min(steps, key=lambda s: abs(s[0] - offset))
                    local = [(0.0, clip_len, nearest[2])]
                written["filter"] += live.envelope(ti, ci, ("track", ti, dev, freq["index"]), local)
                offset += clip_len

    hook = tracks.get("hook")
    rev = tracks.get("return_reverb")
    if hook and rev:
        send = f"send{rev['send']}"
        for sec, clip_ids in _clips_by_section(live, hook["index"]).items():
            if sec not in secs:
                continue
            L = secs[sec]["bars"] * 4.0
            steps = (ramp(max(0, L - 16), L, 0.15, 0.7, 32) if sec.startswith("build")
                     else [(0.0, L, 0.25 if sec == "breakdown" else 0.12)])
            for ci in clip_ids:
                written["send"] += live.envelope(hook["index"], ci, ("track", hook["index"], "mixer", send), steps)

    for part in DUCKED:
        t = tracks.get(part)
        if not t or "duck" not in t:
            continue
        ti, dev = t["index"], t["duck"]
        params = live.params("track", ti, dev)
        gain = live.find_param(params, "Gain")
        if not gain:
            continue
        g0 = live.value_for_display("track", ti, dev, gain["index"], 0.0)
        g_lo = live.value_for_display("track", ti, dev, gain["index"], -9.0)
        g_mid = live.value_for_display("track", ti, dev, gain["index"], -3.0)
        offset_by_sec: dict[str, float] = {}
        for sec, clip_ids in _clips_by_section(live, ti).items():
            kicks = kick_times.get(sec, [])
            for ci in clip_ids:
                clip_len = float(live.clip_get(ti, ci, "length"))
                off = offset_by_sec.get(sec, 0.0)
                steps = []
                for k in kicks:
                    if off <= k < off + clip_len:
                        r = k - off
                        steps += [(r, 0.06, g_lo), (r + 0.06, 0.1, g_mid), (r + 0.16, 0.08, g0)]
                if not steps:
                    steps = [(0.0, clip_len, g0)]
                written["duck"] += live.envelope(ti, ci, ("track", ti, dev, gain["index"]), steps)
                offset_by_sec[sec] = off + clip_len
    log("write_automation", **written)
    return written


# ----------------------------------------------------------------------------- entry points
def build_sketch(live: Live, proj: Project, plan: dict) -> dict:
    live.set_song("tempo", float(plan["bpm"]))
    key = Key.parse(plan["key"])
    try:  # Live 12 scale awareness (cosmetic; not required)
        live.set_song("root_note", key.tonic)
        live.set_song("scale_name", "Minor" if key.mode == "minor" else "Major")
    except Exception:
        pass
    tracks = setup_tracks(live, proj, plan)
    parts = comp.sketch(plan, plan["sketch"]["bars"])
    counts = write_midi(live, tracks, parts, live.cfg.drums["note_map"])
    # audio layers for the sketch: same drop section, first 8 bars
    sk_plan = dict(plan)
    drop = next(s for s in plan["sections"] if s["name"] == plan["sketch"]["section"])
    sk_plan["sections"] = [dict(drop, start_bar=0, bars=plan["sketch"]["bars"], name="sketch",
                                layers=[l for l in drop["layers"] if l not in ("impact", "riser")])]
    placed = write_audio(live, proj, tracks, sk_plan)
    if live.cfg.drums["mode"] == "audio_hits":
        write_hits(live, proj, tracks, plan, parts)
    live.set_song("loop_start", 0.0)
    live.set_song("loop_length", plan["sketch"]["bars"] * 4.0)
    live.set_song("loop", 1)
    live.show("Arranger")
    return {"tracks": tracks, "notes": counts, "audio_clips": placed}


def clear_arrangement(live: Live, tracks: dict) -> None:
    for t in tracks.values():
        if t["kind"] != "track":
            continue
        while True:
            flat = live.c.ap("/live/ap/arr/list", t["index"])
            if not flat:
                break
            live.c.ap("/live/ap/arr/delete", t["index"], 0)


def build_full(live: Live, proj: Project, plan: dict) -> dict:
    tracks = proj.state["tracks"]
    clear_arrangement(live, tracks)
    parts = comp.compose(plan)
    counts = write_midi(live, tracks, parts, live.cfg.drums["note_map"])
    hits = write_hits(live, proj, tracks, plan, parts) if live.cfg.drums["mode"] == "audio_hits" else 0
    placed = write_audio(live, proj, tracks, plan)
    auto = write_automation(live, tracks, plan, parts)
    live.set_song("loop", 0)
    live.show("Arranger")
    return {"notes": counts, "hits": hits, "audio_clips": placed, "automation": auto}
