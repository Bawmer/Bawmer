"""Load and validate config.yaml. Missing values are collected and reported together."""
from __future__ import annotations

import copy
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

GENRES = ("house", "progressive_house", "techno", "dnb", "trap", "pop", "lofi")
SECTION_TYPES = ("intro", "verse", "verse2", "build", "build2", "drop", "drop2", "breakdown", "bridge", "outro")
STOCK_UTILITY = ("EQ Eight", "Compressor", "Glue Compressor", "Utility", "Limiter", "Auto Filter")

DEFAULTS: dict[str, Any] = {
    "song_name": "untitled", "os": "windows", "seed": 42, "genre": None, "subgenre": "",
    "reference_tracks": [], "bpm": "auto", "key": "auto", "mood": "", "energy": 0.7,
    "length_seconds": [180, 210],
    "structure": ["intro", "verse", "build", "drop", "verse2", "drop2", "outro"],
    "sample_library": [], "sample_folders": {}, "plugin_whitelist": [], "banned_plugins": [],
    "allow_stock_utility": True, "presets": {},
    "drums": {"mode": "audio_hits", "note_map": {"kick": 36, "snare": 38, "clap": 39, "hat_closed": 42,
                                           "hat_open": 46, "perc": 37, "ride": 51}},
    "loudness_target_lufs": -9.0, "true_peak_max_dbtp": -1.0,
    "export": {"sample_rate": 48000, "bit_depth": 24, "stems": True, "mp3_preview": True},
    "project_path": None,
    "osc": {"host": "127.0.0.1", "send_port": 11000, "recv_port": 11001, "timeout": 5.0},
    "gui": {"enabled": True, "step_delay": 0.8, "export_timeout": 900},
}


class ConfigError(ValueError):
    pass


@dataclass
class Config:
    raw: dict
    path: Path | None = None
    problems: list[str] = field(default_factory=list)

    def __getattr__(self, name: str) -> Any:
        try:
            return self.raw[name]
        except KeyError:
            raise AttributeError(name) from None

    # --- whitelist gate (hard rule 2) ------------------------------------
    @property
    def all_vst3(self) -> bool:
        return isinstance(self.raw["plugin_whitelist"], str) and self.raw["plugin_whitelist"].lower() == "all"

    def plugin_allowed(self, name: str, vst3_names: set[str] | None = None) -> bool:
        """Named whitelist: exact (case-insensitive) match. `all`: any plugin Live lists
        under VST3 in its Plug-Ins browser (vst3_names, lowercased). Banned always wins."""
        n = name.strip().lower()
        if any(n == b.strip().lower() for b in self.raw["banned_plugins"]):
            return False
        if self.all_vst3:
            return vst3_names is not None and n in vst3_names
        return any(n == w.strip().lower() for w in self.raw["plugin_whitelist"])

    def stock_allowed(self, name: str) -> bool:
        return bool(self.raw["allow_stock_utility"]) and name in STOCK_UTILITY


def _merge(base: dict, over: dict) -> dict:
    out = copy.deepcopy(base)
    for k, v in (over or {}).items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = _merge(out[k], v)
        else:
            out[k] = v
    return out


def _todo(v: Any) -> bool:
    return v is None or (isinstance(v, str) and (not v.strip() or "TODO" in v))


def validate(raw: dict, need_live_assets: bool = True) -> list[str]:
    p: list[str] = []
    if raw["genre"] not in GENRES:
        p.append(f"genre: must be one of {', '.join(GENRES)} (got {raw['genre']!r})")
    if raw["os"] not in ("windows", "macos"):
        p.append("os: windows or macos")
    if raw["bpm"] != "auto" and not (isinstance(raw["bpm"], (int, float)) and 60 <= raw["bpm"] <= 200):
        p.append("bpm: number 60-200 or 'auto'")
    lo, hi = raw["length_seconds"]
    if not (30 <= lo <= hi <= 900):
        p.append("length_seconds: [min, max] in seconds, 30-900")
    for s in raw["structure"]:
        if s not in SECTION_TYPES:
            p.append(f"structure: unknown section {s!r} (allowed: {', '.join(SECTION_TYPES)})")
    if not raw["sample_library"] or any(_todo(x) for x in raw["sample_library"]):
        p.append("sample_library: list at least one absolute folder")
    if _todo(raw["project_path"]):
        p.append("project_path: absolute folder where the new project is created")
    if raw["drums"]["mode"] not in ("midi", "audio_hits"):
        p.append("drums.mode: midi or audio_hits")
    if need_live_assets:
        wl = raw["plugin_whitelist"]
        if isinstance(wl, str) and wl.lower() != "all" or not wl:
            p.append("plugin_whitelist: 'all' (every scanned VST3) or a list of exact VST3 names")
        roles = ["bass", "chords", "hook", "pad"] + (["drums"] if raw["drums"]["mode"] == "midi" else [])
        for r in roles:
            if _todo(raw["presets"].get(r)):
                p.append(f"presets.{r}: User Library preset path (.adv/.adg)")
        listed = [] if isinstance(wl, str) else wl
        overlap = {w.lower() for w in listed} & {b.lower() for b in raw["banned_plugins"]}
        if overlap:
            p.append(f"plugin on both whitelist and banned list: {sorted(overlap)}")
    if not -30 <= float(raw["loudness_target_lufs"]) <= -5:
        p.append("loudness_target_lufs: between -30 and -5")
    return p


def load(path: str | Path, need_live_assets: bool = True, strict: bool = True) -> Config:
    path = Path(path)
    if not path.exists():
        raise ConfigError(f"{path} not found. Copy config.example.yaml to {path.name} and fill it in.")
    raw = _merge(DEFAULTS, yaml.safe_load(path.read_text(encoding="utf-8")) or {})
    problems = validate(raw, need_live_assets)
    if strict and problems:
        raise ConfigError("config incomplete:\n  - " + "\n  - ".join(problems))
    return Config(raw=raw, path=path, problems=problems)


def from_dict(d: dict, need_live_assets: bool = False) -> Config:
    raw = _merge(DEFAULTS, d)
    return Config(raw=raw, problems=validate(raw, need_live_assets))
