"""Step 0: readiness checks. Prints a table; any FAIL in a 'blocking' row stops the run."""
from __future__ import annotations

import importlib
import os
import shutil
from pathlib import Path

from .config import Config
from .index_samples import iter_audio
from .osc import LiveError

EXPECTED_LIVE = (12, 4, 3)


def _row(rows, check, ok, detail, blocking=True):
    rows.append({"check": check, "status": "PASS" if ok else ("FAIL" if blocking else "WARN"),
                 "detail": detail, "blocking": blocking})
    return ok


def run(cfg: Config, live_factory) -> list[dict]:
    rows: list[dict] = []
    _row(rows, "config complete", not cfg.problems,
         "ok" if not cfg.problems else "; ".join(cfg.problems))
    for mod in ("librosa", "pyloudnorm", "pythonosc", "soundfile", "scipy", "yaml"):
        try:
            importlib.import_module(mod)
            _row(rows, f"python: {mod}", True, "importable")
        except ImportError as e:
            _row(rows, f"python: {mod}", False, str(e))
    try:
        importlib.import_module("pyautogui")
        _row(rows, "python: pyautogui (save/export)", True, "importable", blocking=False)
    except Exception as e:
        _row(rows, "python: pyautogui (save/export)", False,
             f"{e} -> resampling fallback only, no versioned saves", blocking=False)
    _row(rows, "ffmpeg (MP3 preview)", bool(shutil.which("ffmpeg")),
         shutil.which("ffmpeg") or "not on PATH; MP3 preview skipped", blocking=False)

    for root in cfg.sample_library:
        p = Path(root)
        ok = p.is_dir() and os.access(p, os.R_OK)
        n = 0
        if ok:
            for _ in iter_audio([str(p)]):
                n += 1
                if n >= 20000:
                    break
        _row(rows, f"samples readable: {root}", ok and n > 0, f"{n}{'+' if n >= 20000 else ''} audio files"
             if ok else "missing or unreadable")
    pp = Path(cfg.project_path) if cfg.project_path and "TODO" not in str(cfg.project_path) else None
    _row(rows, "project_path writable", bool(pp and pp.is_dir() and os.access(pp, os.W_OK)),
         str(pp) if pp else "not set")

    try:
        live = live_factory()
    except OSError as e:
        _row(rows, "OSC reply port", False, f"cannot bind {cfg.osc['recv_port']}: {e}")
        return rows
    try:
        _row(rows, "/live/test", live.test(), "AbletonOSC answered 'ok'")
    except LiveError as e:
        _row(rows, "/live/test", False, f"{e} -> is Live running with AbletonOSC as a Control Surface?")
        return rows
    v = live.version()
    vs = ".".join(str(x) for x in v if x >= 0)
    _row(rows, "/live/application/get/version", tuple(v[:2]) >= (12, 0),
         f"Live {vs}" + ("" if tuple(v) == EXPECTED_LIVE else f" (expected {'.'.join(map(str, EXPECTED_LIVE))})"))
    if not _row(rows, "AutoProducer OSC extension", live.ext_ok(),
                "/live/ap/ping ok" if live.ext_ok() else "not installed: run `autoproducer install-ext`"):
        return rows
    plugins = live.plugins(refresh=True)
    leaf = {p.split("/")[-1].lower(): p for p in plugins}
    if cfg.all_vst3:
        vst3 = live.vst3_names()
        _row(rows, "VST3 plugins scanned (whitelist: all)", bool(vst3),
             f"{len(vst3)} VST3 plugins in Live's browser" if vst3 else
             "no 'VST3' folder in Live's Plug-Ins browser -> enable 'Use VST3 Plug-In System Folders' "
             "(Preferences > Plug-Ins) and rescan")
    for name in ([] if cfg.all_vst3 else cfg.plugin_whitelist):
        hit = [p for k, p in leaf.items() if k == name.lower() or name.lower() in k]
        _row(rows, f"VST3 scanned: {name}", bool(hit), hit[0] if hit else
             "NOT FOUND in Live's Plug-Ins browser -> rescan (Preferences > Plug-Ins) and check the exact name")
    presets = set(live.user_presets())
    shown = sorted(p for p in presets if p.lower().endswith(".adv"))
    _row(rows, "User Library .adv presets found", bool(shown),
         (f"{len(shown)}: " + "; ".join(shown[:40]) + (" ..." if len(shown) > 40 else "")) if shown
         else "none: save your instrument presets (device title bar > Save Preset) first", blocking=False)
    for role, path in cfg.presets.items():
        if not path or "TODO" in path:
            continue
        norm = path.replace("\\", "/").strip("/")
        _row(rows, f"preset {role}", norm in presets or any(p.endswith(norm) for p in presets),
             norm if norm in presets else "not found in User Library")
    info = live.song_info()
    _row(rows, "empty set open", info["tracks"] <= 2 and info["song_length"] <= 8 * 4,
         f"{info['tracks']} tracks, length {info['song_length']} beats"
         " (open a NEW empty set before `init`)", blocking=False)
    return rows


def table(rows: list[dict]) -> str:
    out = ["| Check | Status | Detail |", "|---|---|---|"]
    for r in rows:
        mark = {"PASS": "✅", "FAIL": "❌", "WARN": "⚠️"}[r["status"]]
        out.append(f"| {r['check']} | {mark} {r['status']} | {r['detail']} |")
    return "\n".join(out)


def ready(rows: list[dict]) -> bool:
    return not any(r["status"] == "FAIL" and r["blocking"] for r in rows)
