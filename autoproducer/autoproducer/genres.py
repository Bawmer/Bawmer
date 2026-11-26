"""Genre profiles: tempo range, preferred mode, drum style, progressions, section lengths."""
from __future__ import annotations

PROFILES = {
    "house":  {"bpm": (120, 126), "mode": "minor", "drum": "four_floor", "swing": 0.10,
               "bass": "offbeat", "chords": "stab",
               "progressions": [[0, 5, 2, 6], [0, 3, 4, 3], [0, 6, 5, 6], [5, 3, 0, 4]]},
    "techno": {"bpm": (126, 134), "mode": "minor", "drum": "four_floor", "swing": 0.0,
               "bass": "rolling", "chords": "stab",
               "progressions": [[0, 0, 5, 6], [0, 5, 0, 3], [0, 3, 0, 6]]},
    "dnb":    {"bpm": (172, 176), "mode": "minor", "drum": "two_step", "swing": 0.0,
               "bass": "reese", "chords": "sustain",
               "progressions": [[0, 5, 2, 6], [0, 3, 5, 4], [5, 6, 0, 0]]},
    "trap":   {"bpm": (136, 150), "mode": "minor", "drum": "trap", "swing": 0.0,
               "bass": "808", "chords": "sustain",
               "progressions": [[0, 5, 3, 4], [0, 6, 5, 6], [0, 3, 5, 4]]},
    "pop":    {"bpm": (100, 124), "mode": "major", "drum": "backbeat", "swing": 0.05,
               "bass": "root8", "chords": "pulse",
               "progressions": [[0, 4, 5, 3], [5, 3, 0, 4], [0, 5, 3, 4], [3, 4, 0, 5]]},
    "lofi":   {"bpm": (70, 90), "mode": "minor", "drum": "boombap", "swing": 0.22,
               "bass": "root", "chords": "sustain",
               "progressions": [[1, 4, 0, 5], [3, 2, 1, 4], [0, 5, 3, 4]]},
}

# bars per section type (multiples of 8 so phrases line up)
SECTION_BARS = {"intro": 16, "verse": 16, "verse2": 16, "build": 8, "build2": 8, "drop": 16,
                "drop2": 16, "breakdown": 16, "bridge": 8, "outro": 16}
# base energy per section type (scaled by config.energy)
SECTION_ENERGY = {"intro": 0.30, "verse": 0.50, "verse2": 0.55, "build": 0.70, "build2": 0.75,
                  "drop": 1.00, "drop2": 1.00, "breakdown": 0.35, "bridge": 0.45, "outro": 0.30}
# which layers play in each section type
SECTION_LAYERS = {
    "intro":     {"kick", "hat", "perc", "pad", "top_loop"},
    "verse":     {"kick", "hat", "snare", "bass", "chords", "perc", "top_loop", "vocal"},
    "verse2":    {"kick", "hat", "snare", "bass", "chords", "perc", "top_loop", "tonal_loop", "vocal"},
    "build":     {"snare", "hat", "chords", "pad", "hook", "riser"},
    "build2":    {"snare", "hat", "chords", "pad", "hook", "riser"},
    "drop":      {"kick", "hat", "snare", "bass", "chords", "hook", "perc", "top_loop", "tonal_loop", "impact"},
    "drop2":     {"kick", "hat", "snare", "bass", "chords", "hook", "perc", "top_loop", "tonal_loop", "impact", "vocal"},
    "breakdown": {"pad", "chords", "hook", "vocal", "downlifter"},
    "bridge":    {"pad", "chords", "vocal", "hat"},
    "outro":     {"kick", "hat", "perc", "pad", "top_loop", "downlifter"},
}
