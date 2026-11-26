"""Keys, scales, chords and the Krumhansl-Schmuckler key finder."""
from __future__ import annotations

import numpy as np

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
_ALIASES = {"DB": 1, "EB": 3, "GB": 6, "AB": 8, "BB": 10, "CB": 11, "FB": 4, "E#": 5, "B#": 0}
SCALES = {
    "major": [0, 2, 4, 5, 7, 9, 11],
    "minor": [0, 2, 3, 5, 7, 8, 10],
}
# Krumhansl-Kessler key profiles
_MAJOR_PROFILE = np.array([6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88])
_MINOR_PROFILE = np.array([6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17])


def pc(name: str) -> int:
    n = name.strip().upper()
    if n in _ALIASES:
        return _ALIASES[n]
    n = n.replace("♯", "#")
    if n in NOTE_NAMES:
        return NOTE_NAMES.index(n)
    raise ValueError(f"unknown note {name!r}")


class Key:
    def __init__(self, tonic: int, mode: str):
        if mode not in SCALES:
            raise ValueError(mode)
        self.tonic, self.mode = tonic % 12, mode

    @classmethod
    def parse(cls, text: str) -> "Key":
        parts = text.replace("m", " minor").split() if text.endswith("m") and " " not in text else text.split()
        tonic = pc(parts[0])
        mode = parts[1].lower() if len(parts) > 1 else "major"
        mode = {"maj": "major", "min": "minor"}.get(mode, mode)
        return cls(tonic, mode)

    def __str__(self) -> str:
        return f"{NOTE_NAMES[self.tonic]} {self.mode}"

    def __eq__(self, other: object) -> bool:
        return isinstance(other, Key) and (self.tonic, self.mode) == (other.tonic, other.mode)

    def __hash__(self) -> int:
        return hash((self.tonic, self.mode))

    @property
    def pitch_classes(self) -> set[int]:
        return {(self.tonic + i) % 12 for i in SCALES[self.mode]}

    def relative(self) -> "Key":
        return Key(self.tonic + 9, "minor") if self.mode == "major" else Key(self.tonic + 3, "major")

    def degree_pitch(self, degree: int, octave: int = 4) -> int:
        """MIDI pitch of scale degree (0-based, may exceed 6 / be negative)."""
        steps = SCALES[self.mode]
        o, d = divmod(degree, 7)
        return 12 * (octave + 1 + o) + self.tonic + steps[d]

    def triad(self, degree: int, octave: int = 4, seventh: bool = False) -> list[int]:
        idx = [degree, degree + 2, degree + 4] + ([degree + 6] if seventh else [])
        return [self.degree_pitch(i, octave) for i in idx]

    def in_key(self, pitch: int) -> bool:
        return pitch % 12 in self.pitch_classes


def semitone_shift(src: Key, dst: Key) -> int:
    """Smallest transposition (-6..+5) that maps src onto dst (or dst's relative)."""
    target = dst if src.mode == dst.mode else dst.relative()
    d = (target.tonic - src.tonic) % 12
    return d - 12 if d > 5 else d


def compatible(src: Key, dst: Key, max_shift: int = 2) -> tuple[bool, int]:
    shift = semitone_shift(src, dst)
    return abs(shift) <= max_shift, shift


def estimate_key(chroma_mean: np.ndarray) -> tuple[Key, float]:
    """Correlate a 12-bin mean chroma with all 24 key profiles. Returns (key, confidence)."""
    c = np.asarray(chroma_mean, dtype=float)
    if c.sum() <= 0:
        return Key(0, "major"), 0.0
    scores = []
    for t in range(12):
        for mode, prof in (("major", _MAJOR_PROFILE), ("minor", _MINOR_PROFILE)):
            r = np.corrcoef(c, np.roll(prof, t))[0, 1]
            scores.append((r, Key(t, mode)))
    scores.sort(key=lambda x: x[0], reverse=True)
    best, second = scores[0][0], scores[1][0]
    return scores[0][1], float(max(0.0, best - second) + max(0.0, best) * 0.5)


def bpm_ratio_match(sample_bpm: float, target: float, tol: float = 0.06) -> float | None:
    """Return the factor (0.5, 1, 2) that brings sample_bpm within tol of target, else None."""
    if not sample_bpm:
        return None
    for f in (1.0, 2.0, 0.5):
        if abs(sample_bpm * f - target) / target <= tol:
            return f
    return None
