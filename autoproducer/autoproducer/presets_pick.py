"""Pick User Library instrument presets by name when config says `presets.<role>: auto`.

Only names are available before loading, so this ranks candidates; the load step then
verifies each one really is a VST3 instrument (not a stock instrument, not VST2/AU) and
falls through to the next candidate if not. Picks are shown at checkpoint A.
"""
from __future__ import annotations

import re

# role -> (keyword, weight). Word matches on the preset file name and its folders.
ROLE_WORDS = {
    "bass": [("bass", 10), ("sub", 6), ("reese", 6), ("808", 5), ("low", 2), ("growl", 3)],
    "chords": [("chord", 10), ("supersaw", 8), ("saw", 4), ("stab", 7), ("keys", 6), ("piano", 6),
               ("poly", 4), ("organ", 3), ("brass", 3)],
    "hook": [("lead", 10), ("pluck", 8), ("hook", 9), ("arp", 4), ("melody", 6), ("bell", 4),
             ("supersaw", 3), ("synth", 1)],
    "pad": [("pad", 10), ("atmos", 7), ("ambient", 6), ("string", 6), ("choir", 5), ("drone", 4),
            ("evolv", 4), ("air", 3), ("warm", 2)],
}
# genre flavour: small boosts so festival house prefers bright, big sounds
GENRE_WORDS = {
    "progressive_house": [("supersaw", 3), ("festival", 3), ("big", 2), ("room", 2), ("future", 2),
                          ("anthem", 2), ("euphor", 2), ("bright", 1), ("pluck", 2), ("edm", 2)],
    "house": [("deep", 2), ("organ", 2), ("stab", 2), ("warm", 1)],
    "techno": [("acid", 2), ("dark", 2), ("rumble", 2)],
}
NEGATIVE = [("drum", 12), ("kit", 10), ("kick", 12), ("snare", 12), ("hat", 10), ("perc", 10),
            ("fx", 8), ("riser", 10), ("vocal", 6), ("init", 15), ("default", 8), ("template", 8)]


def _words(path: str) -> str:
    return re.sub(r"[^a-z0-9]+", " ", path.lower())


def score(path: str, role: str, genre: str) -> int:
    w = _words(path)
    s = sum(weight for kw, weight in ROLE_WORDS[role] if kw in w)
    if s == 0:
        return 0  # never pick a preset with no hint of the role
    s += sum(weight for kw, weight in GENRE_WORDS.get(genre, []) if kw in w)
    s -= sum(weight for kw, weight in NEGATIVE if kw in w)
    for other, kws in ROLE_WORDS.items():  # names that clearly belong to another role
        if other != role and any(kw in w for kw, wt in kws if wt >= 10):
            s -= 6
    return s


def rank(presets: list[str], role: str, genre: str) -> list[tuple[int, str]]:
    cands = [p for p in presets if p.lower().endswith(".adv")]
    scored = [(score(p, role, genre), p) for p in cands]
    return sorted([x for x in scored if x[0] > 0], key=lambda x: (-x[0], x[1]))


def pick_all(presets: list[str], roles: list[str], genre: str) -> dict[str, list[str]]:
    """Ranked candidate lists per role; the top choice of each role is unique when possible."""
    ranked = {r: [p for _, p in rank(presets, r, genre)] for r in roles}
    used: set[str] = set()
    out: dict[str, list[str]] = {}
    # most constrained role first
    for r in sorted(roles, key=lambda r: len(ranked[r])):
        lst = [p for p in ranked[r] if p not in used] + [p for p in ranked[r] if p in used]
        out[r] = lst
        if lst:
            used.add(lst[0])
    return {r: out[r] for r in roles}
