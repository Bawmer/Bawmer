"""Stage 3 (pure part): generate every MIDI note of the song from the plan.

Nothing here talks to Live; `build.py` writes the result into clips. That keeps the
music testable: tests check scale membership, kick/bass lock, variation, fills.

Conventions: times in beats (quarter notes), 4/4, one clip per part per section,
note times relative to the clip start.
"""
from __future__ import annotations

import random
from dataclasses import dataclass, field

from .theory import Key

DRUMS = ("kick", "snare", "clap", "hat_closed", "hat_open", "perc", "ride")


@dataclass
class Note:
    pitch: int
    start: float
    dur: float
    vel: int
    tag: str = ""  # drum instrument name for drum notes

    def as_tuple(self) -> tuple:
        return (int(self.pitch), round(float(self.start), 5), round(float(self.dur), 5),
                int(max(1, min(127, self.vel))), 0)


@dataclass
class ClipSpec:
    part: str
    section: str
    start_beat: float
    length_beats: float
    notes: list[Note] = field(default_factory=list)


# ----------------------------------------------------------------------------- helpers
def _humanize(notes: list[Note], rng: random.Random, bpm: float, vel_jitter: int,
              ms_sigma: float, length: float) -> None:
    beat_per_ms = bpm / 60000.0
    for n in notes:
        n.vel = int(max(1, min(127, n.vel + rng.randint(-vel_jitter, vel_jitter))))
        if n.start > 0:  # never push the downbeat of a clip before its start
            shift = max(-12.0, min(12.0, rng.gauss(0.0, ms_sigma))) * beat_per_ms
            n.start = min(max(0.0, n.start + shift), length - 1e-3)


def _swing(t: float, amount: float) -> float:
    sixteenth = round(t * 4)
    if abs(t * 4 - sixteenth) < 1e-6 and sixteenth % 2 == 1:
        return t + amount * 0.25
    return t


def chord_at(prog: list[int], bar: int, bars_per_chord: int) -> int:
    return prog[(bar // bars_per_chord) % len(prog)]


def voice_lead(key: Key, degrees: list[int], lo: int, hi: int, seventh: bool) -> list[list[int]]:
    """Pick the inversion/octave of each chord closest to the previous voicing."""
    voicings, prev = [], None
    for d in degrees:
        base = key.triad(d, 3, seventh)
        options = []
        for inv in range(len(base)):
            v = sorted(base[inv:] + [p + 12 for p in base[:inv]])
            for octv in (-12, 0, 12, 24):
                w = [p + octv for p in v]
                if lo <= w[0] and w[-1] <= hi:
                    options.append(w)
        if not options:
            options = [base]
        if prev is None:
            centre = (lo + hi) / 2
            best = min(options, key=lambda w: abs(sum(w) / len(w) - centre))
        else:
            best = min(options, key=lambda w: sum(abs(a - b) for a, b in zip(w, prev)) + abs(len(w) - len(prev)))
        voicings.append(best)
        prev = best
    return voicings


# ----------------------------------------------------------------------------- drums
def kick_pattern(style: str, bar_in_phrase: int, rng: random.Random, energy: float) -> list[float]:
    if style == "four_floor":
        return [0.0, 1.0, 2.0, 3.0]
    if style == "two_step":
        return [0.0, 2.5] if bar_in_phrase % 2 == 0 else [0.0, 1.75, 2.5]
    if style == "trap":
        pats = [[0.0, 0.75, 2.5], [0.0, 1.5, 2.75], [0.0, 0.75, 1.75, 3.5]]
        return pats[bar_in_phrase % len(pats)] if energy > 0.6 else [0.0, 2.5]
    if style == "backbeat":
        return [0.0, 2.0] + ([2.5] if bar_in_phrase % 4 == 3 else [])
    if style == "boombap":
        return [0.0, 1.75, 2.5] if bar_in_phrase % 2 == 0 else [0.0, 2.25, 2.5]
    raise ValueError(style)


def snare_pattern(style: str) -> list[float]:
    return [2.0] if style == "trap" else [1.0, 3.0]


def drum_bar(style: str, bar: int, bars: int, energy: float, rng: random.Random,
             perc_cells: list[list[float]], layers: set[str], section: str, next_section: str | None) -> list[Note]:
    notes: list[Note] = []
    phrase_bar = bar % 8
    last_bar = bar == bars - 1
    building = section.startswith("build")
    if "kick" in layers and not building:
        for t in kick_pattern(style, phrase_bar, rng, energy):
            notes.append(Note(0, t, 0.25, 118 if t == 0 else 108, "kick"))
    if "snare" in layers:
        if building:
            # snare roll: density doubles every quarter of the build
            q = min(3, int(4 * bar / max(bars, 1)))
            step = [1.0, 0.5, 0.25, 0.125][q]
            t = 0.0
            while t < 4.0 - 1e-6:
                notes.append(Note(0, t, step * 0.9, int(60 + 60 * (bar + t / 4) / bars), "snare"))
                t += step
        else:
            inst = "clap" if style == "four_floor" else "snare"
            for t in snare_pattern(style):
                notes.append(Note(0, t, 0.25, 112, inst))
            if energy > 0.5 and style != "four_floor" and phrase_bar in (3, 7):
                notes.append(Note(0, 3.75, 0.2, 70, "snare"))  # ghost
    if "hat" in layers:
        alt = phrase_bar >= 4  # pattern B for bars 5-8 of each phrase
        if style == "trap":
            t = 0.0
            while t < 4.0 - 1e-6:
                roll = alt and (t in (1.5, 3.5)) and energy > 0.6
                if roll:
                    for k in range(3):
                        notes.append(Note(0, t + k / 6.0, 0.08, 70 + k * 10, "hat_closed"))
                    t += 0.5
                    continue
                notes.append(Note(0, t, 0.1, 96 if t % 1 == 0 else 78, "hat_closed"))
                t += 0.5
        else:
            grid = 0.25 if (energy > 0.8 or (alt and energy > 0.6)) else 0.5
            t = 0.0
            while t < 4.0 - 1e-6:
                offbeat = abs(t % 1 - 0.5) < 1e-6
                if style == "four_floor" and not offbeat and grid == 0.5:
                    t += grid
                    continue
                vel = 100 if offbeat else (84 if t % 1 == 0 else 66)
                notes.append(Note(0, _swing(t, 0.0), 0.1, vel, "hat_closed"))
                t += grid
            if energy >= 0.9 and style in ("four_floor",):
                for b in (0.5, 1.5, 2.5, 3.5):
                    notes.append(Note(0, b, 0.2, 90, "hat_open"))
    if "perc" in layers and energy >= 0.4:
        cell = perc_cells[(bar // 4) % len(perc_cells)]
        for t in cell:
            notes.append(Note(0, t, 0.15, rng.randint(64, 92), "perc"))
    # fill into the next section (not into a build: the build roll is its own fill)
    if last_bar and next_section and not building and not next_section.startswith("build"):
        notes = [n for n in notes if n.start < 2.0 or n.tag == "kick" and n.start < 3.0]
        for i, t in enumerate([2.0, 2.5, 3.0, 3.25, 3.5, 3.75]):
            notes.append(Note(0, t, 0.2, 80 + i * 7, "snare" if i % 2 else "perc"))
    if building and last_bar:
        notes = [n for n in notes if n.start < 3.0]  # one beat of silence before the drop
    return notes


# ----------------------------------------------------------------------------- tonal parts
def bass_bar(style: str, key: Key, degree: int, kicks: list[float], bar_in_phrase: int,
             energy: float) -> list[Note]:
    root = key.degree_pitch(degree, 1)  # octave 1 => roughly E1..D#2
    if root < 28:
        root += 12
    notes: list[Note] = []
    if style == "offbeat":  # interlocks with four-on-the-floor: every offbeat between kicks
        for k in kicks:
            notes.append(Note(root + (12 if bar_in_phrase % 4 == 3 and k == kicks[-1] else 0),
                              k + 0.5, 0.4, 104))
    elif style == "rolling":  # 16ths that avoid the kick positions
        for k in kicks:
            for off in (0.25, 0.5, 0.75):
                notes.append(Note(root, k + off, 0.2, 100 if off == 0.5 else 86))
    else:  # kick-locked: a bass note starts on every kick
        ends = kicks[1:] + [4.0]
        for i, (k, e) in enumerate(zip(kicks, ends)):
            p = root
            if style == "808" and i == len(kicks) - 1 and bar_in_phrase % 2 == 1:
                p = root + 7 if key.in_key(root + 7) else root + 12
            if style == "root8":
                t = k
                while t < e - 1e-6:
                    notes.append(Note(p, t, 0.45, 104 if t == k else 88))
                    t += 0.5
                continue
            notes.append(Note(p, k, max(0.2, e - k - 0.05), 110))
    if energy < 0.6:
        notes = [n for n in notes if n.start % 1 in (0.0, 0.5)]
    return notes


def chord_rhythm(style: str, bar_in_phrase: int, rng_cell: list[tuple[float, float]]) -> list[tuple[float, float]]:
    if style == "sustain":
        return [(0.0, 3.9)]
    if style == "pulse":
        return [(t * 0.5, 0.4) for t in range(8)]
    cell = list(rng_cell)
    if bar_in_phrase == 7:  # phrase-end push
        cell = [(t, d) for t, d in cell if t < 3.0] + [(3.5, 0.5)]
    return cell


@dataclass
class Motif:
    rhythm: list[tuple[float, float]]      # (start, dur) over 2 bars
    call: list[int]                        # scale degrees (absolute, 0 = tonic)
    response: list[int]


def make_motif(rng: random.Random) -> Motif:
    cells = [
        [(0.0, 0.5), (0.75, 0.25), (1.0, 0.5), (1.5, 0.5), (2.5, 1.0), (4.0, 0.75), (5.0, 0.5), (5.5, 1.5)],
        [(0.0, 0.75), (0.75, 0.25), (1.5, 0.5), (2.0, 1.0), (3.5, 0.5), (4.0, 1.5), (6.0, 1.0)],
        [(0.5, 0.5), (1.0, 0.5), (1.5, 0.25), (2.0, 0.75), (3.0, 0.5), (4.5, 0.5), (5.0, 1.0), (6.5, 0.5)],
        [(0.0, 0.25), (0.25, 0.25), (0.5, 0.5), (1.5, 0.5), (2.0, 1.5), (4.0, 0.5), (4.5, 0.5), (5.0, 2.0)],
    ]
    rhythm = rng.choice(cells)
    n = len(rhythm)
    start = rng.choice([2, 4, 7])
    steps = [rng.choice([-2, -1, 1, 1, 2, 3]) for _ in range(n - 1)]
    call = [start]
    for s in steps:
        call.append(max(-3, min(11, call[-1] + s)))
    # response: same rhythm and first half, answer the contour and land on the tonic
    half = n // 2
    response = call[:half] + [call[half - 1] - (c - call[half - 1]) for c in call[half:]]
    response[-1] = 0 if abs(response[-2]) < abs(response[-2] - 7) else 7
    return Motif(rhythm, call, response)


def hook_phrase(key: Key, motif: Motif, prog: list[int], bars_per_chord: int, start_bar: int,
                variant: str) -> list[Note]:
    """8 bars: call, response, call (varied tail), resolving response."""
    notes: list[Note] = []
    plan = [("call", 0), ("response", 2), ("call2", 4), ("response", 6)]
    for kind, bar in plan:
        degrees = motif.response if kind == "response" else list(motif.call)
        if kind == "call2":
            degrees = degrees[:-2] + [degrees[-2] + 1, degrees[-1] + 2]
        for (t, d), deg in zip(motif.rhythm, degrees):
            abs_bar = start_bar + bar + int(t // 4)
            chord = chord_at(prog, abs_bar, bars_per_chord)
            strong = t % 1 == 0
            if strong:  # snap strong-beat notes to the nearest chord tone
                tones = [chord, chord + 2, chord + 4]
                deg = min((tt + 7 * o for tt in tones for o in (-1, 0, 1)), key=lambda x: abs(x - deg))
            pitch = key.degree_pitch(deg, 5)
            notes.append(Note(pitch, bar * 4 + t, d * 0.95, 108 if strong else 92))
            if variant == "octave":
                notes.append(Note(pitch - 12, bar * 4 + t, d * 0.95, 84))
    if variant == "thin":
        notes = [n for n in notes if n.start % 1 == 0 or n.dur >= 1.0]
    if variant == "ornament":  # grace notes on the last bar
        for n in [n for n in notes if n.start >= 28 and n.dur >= 0.5]:
            notes.append(Note(n.pitch + (2 if key.in_key(n.pitch + 2) else 1), n.start + n.dur * 0.5,
                              n.dur * 0.45, 80))
            n.dur *= 0.5
    return notes


# ----------------------------------------------------------------------------- song
def compose(plan: dict) -> dict[str, list[ClipSpec]]:
    rng = random.Random(plan["seed"] * 7919 + 1)
    key = Key.parse(plan["key"])
    bpm = float(plan["bpm"])
    style = plan["drum_style"]
    bars_per_chord = 2 if plan["genre"] in ("dnb", "trap", "lofi") else 1
    seventh = plan["genre"] in ("house", "lofi")
    motif = make_motif(rng)
    stab_cells = [[(0.0, 0.25), (0.75, 0.25), (1.5, 0.5), (2.5, 0.25), (3.0, 0.5)],
                  [(0.5, 0.25), (1.5, 0.25), (2.5, 0.25), (3.5, 0.5)],
                  [(0.0, 0.5), (1.0, 0.25), (1.75, 0.25), (2.5, 0.5), (3.25, 0.25)]]
    stab = rng.choice(stab_cells)
    perc_cells = [sorted(rng.sample([i * 0.25 for i in range(16) if i % 4], 3)) for _ in range(2)]
    swing = float(plan.get("swing", 0.0))

    parts: dict[str, list[ClipSpec]] = {p: [] for p in ("drums", "bass", "chords", "hook", "pad")}
    sections = plan["sections"]
    for si, sec in enumerate(sections):
        name, bars, energy = sec["name"], sec["bars"], sec["energy"]
        layers = set(sec["layers"])
        nxt = sections[si + 1]["name"] if si + 1 < len(sections) else None
        start = sec["start_bar"] * 4.0
        length = bars * 4.0
        prog = plan["alt_progression"] if name in ("verse2", "drop2", "bridge") else plan["progression"]
        building = name.startswith("build")

        drums = ClipSpec("drums", name, start, length)
        bass = ClipSpec("bass", name, start, length)
        chords = ClipSpec("chords", name, start, length)
        hook = ClipSpec("hook", name, start, length)
        pad = ClipSpec("pad", name, start, length)
        degrees = [chord_at(prog, b, bars_per_chord) for b in range(bars)]
        chord_v = voice_lead(key, degrees, 52, 76, seventh)
        pad_v = voice_lead(key, degrees, 60, 86, False)

        for b in range(bars):
            off = b * 4.0
            dn = drum_bar(style, b, bars, energy, rng, perc_cells, layers, name, nxt)
            for n in dn:
                n.start = _swing(n.start, swing) + off
            drums.notes += dn
            kicks = kick_pattern(style, b % 8, rng, energy)
            if "bass" in layers and not building:
                for n in bass_bar(plan["bass_style"], key, degrees[b], kicks, b % 8, energy):
                    n.start += off
                    bass.notes.append(n)
            if "chords" in layers:
                rhythm = chord_rhythm(plan["chord_style"], b % 8, stab)
                if building:
                    rhythm = [(t * 0.5, 0.4) for t in range(8)] if b >= bars - 4 else [(0.0, 3.9)]
                if b == bars - 1 and nxt and nxt.startswith("drop"):
                    rhythm = [(t, d) for t, d in rhythm if t < 3.0]
                for t, d in rhythm:
                    for p in chord_v[b]:
                        chords.notes.append(Note(p, off + _swing(t, swing), d, int(70 + 30 * energy)))
            if "pad" in layers:
                for p in pad_v[b]:
                    pad.notes.append(Note(p, off, 3.98, int(55 + 25 * energy)))
        if "hook" in layers:
            variant = {"drop": "full", "drop2": "octave", "breakdown": "thin"}.get(name, "thin")
            for phrase in range(0, bars, 8):
                variant_p = "ornament" if (name == "drop2" and phrase + 8 >= bars) else variant
                for n in hook_phrase(key, motif, prog, bars_per_chord, phrase, variant_p):
                    if n.start + phrase * 4 < length:
                        n.start += phrase * 4
                        hook.notes.append(n)
            if building:  # only the call, repeated: tension, no resolution
                hook.notes = [n for n in hook.notes if (n.start % 16) < 8]
                hook.notes = [n for n in hook.notes if n.start < length - 4]

        _humanize(drums.notes, rng, bpm, 6, 3.0, length)
        for n in drums.notes:  # keep the kick tight
            if n.tag == "kick":
                n.start = round(n.start * 4) / 4 if abs(n.start * 4 - round(n.start * 4)) < 0.05 else n.start
        for clip, j, ms in ((bass, 4, 2.0), (chords, 6, 4.0), (hook, 8, 5.0), (pad, 4, 0.0)):
            _humanize(clip.notes, rng, bpm, j, ms, length)
        for clip in (drums, bass, chords, hook, pad):
            if clip.notes:
                clip.notes.sort(key=lambda n: (n.start, n.pitch))
                parts[clip.part].append(clip)
    parts["_motif"] = motif  # type: ignore[assignment]
    return parts


def map_drums(clips: list[ClipSpec], note_map: dict[str, int]) -> None:
    for c in clips:
        for n in c.notes:
            n.pitch = int(note_map.get(n.tag, note_map.get("perc", 37)))


def sketch(plan: dict, bars: int = 8) -> dict[str, list[ClipSpec]]:
    """The 8-bar checkpoint-A loop: the first `bars` of the first drop, as standalone clips."""
    full = compose(plan)
    target = plan["sketch"]["section"]
    out: dict[str, list[ClipSpec]] = {}
    for part, clips in full.items():
        if part.startswith("_"):
            continue
        for c in clips:
            if c.section == target:
                cut = ClipSpec(part, "sketch", 0.0, bars * 4.0,
                               [Note(n.pitch, n.start, min(n.dur, bars * 4.0 - n.start), n.vel, n.tag)
                                for n in c.notes if n.start < bars * 4.0])
                out[part] = [cut]
    return out
