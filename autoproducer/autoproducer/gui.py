"""OS-level GUI automation for the two things the Live API cannot do: save and render.

Fragile by nature, so every action is: focus Live -> send keys -> WAIT FOR THE RESULT
ON DISK. If the expected file never appears, we raise; nothing proceeds on hope.
Requires `pip install autoproducer[gui]` and (macOS) Accessibility permission for the
terminal running this. Do not touch mouse/keyboard while a GUI step runs.
"""
from __future__ import annotations

import platform
import subprocess
import time
from pathlib import Path

from .actionlog import log


class GuiError(RuntimeError):
    pass


def _pg():
    try:
        import pyautogui
    except ImportError as e:
        raise GuiError("pyautogui not installed: pip install 'autoproducer[gui]'") from e
    pyautogui.FAILSAFE = True  # slam the mouse into a screen corner to abort
    return pyautogui


def _mod() -> str:
    return "command" if platform.system() == "Darwin" else "ctrl"


def focus_live() -> None:
    system = platform.system()
    if system == "Darwin":
        script = ('tell application "System Events" to set frontmost of '
                  '(first process whose name starts with "Live") to true')
        r = subprocess.run(["osascript", "-e", script], capture_output=True, text=True)
        if r.returncode != 0:
            raise GuiError(f"could not focus Ableton Live: {r.stderr.strip()}")
    elif system == "Windows":
        try:
            import pygetwindow as gw
        except ImportError as e:
            raise GuiError("pygetwindow not installed: pip install 'autoproducer[gui]'") from e
        wins = [w for w in gw.getAllWindows() if "Ableton Live" in w.title]
        if not wins:
            raise GuiError("no window titled 'Ableton Live' found")
        w = wins[0]
        if w.isMinimized:
            w.restore()
        w.activate()
    else:
        raise GuiError("GUI automation supports Windows and macOS only")
    time.sleep(0.6)
    log("gui_focus_live")


def _wait_file(candidates: list[Path], timeout: float, newer_than: float) -> Path:
    t0 = time.time()
    while time.time() - t0 < timeout:
        for c in candidates:
            if c.exists() and c.stat().st_mtime >= newer_than - 1:
                if _stable(c):
                    return c
        time.sleep(0.5)
    raise GuiError("expected file did not appear: " + " | ".join(str(c) for c in candidates))


def _stable(p: Path, wait: float = 1.0) -> bool:
    s1 = p.stat().st_size
    time.sleep(wait)
    return p.exists() and p.stat().st_size == s1 and s1 > 0


def _type_path_in_dialog(pg, folder: Path, filename: str, delay: float) -> None:
    if platform.system() == "Darwin":
        pg.hotkey("command", "shift", "g")       # "Go to folder"
        time.sleep(delay)
        pg.write(str(folder), interval=0.01)
        pg.press("enter")
        time.sleep(delay)
        pg.hotkey("command", "a")                # name field has focus again
        pg.write(filename, interval=0.01)
    else:
        pg.hotkey("ctrl", "a")
        pg.write(str(folder / filename), interval=0.01)
    time.sleep(delay)
    pg.press("enter")
    time.sleep(delay)
    pg.press("enter")  # confirm overwrite if asked; harmless otherwise on Live's dialogs


def save_as(folder: Path, name: str, delay: float = 0.8) -> Path:
    """File > Save Live Set As... Returns the .als path Live actually wrote.
    Live puts a first save outside a project into '<name> Project/'."""
    pg = _pg()
    folder.mkdir(parents=True, exist_ok=True)
    t = time.time()
    focus_live()
    pg.hotkey(_mod(), "shift", "s")
    time.sleep(delay * 1.5)
    _type_path_in_dialog(pg, folder, name, delay)
    als = _wait_file([folder / f"{name}.als", folder / f"{name} Project" / f"{name}.als"], 60, t)
    log("gui_save_as", path=str(als))
    return als


def export_audio(out_dir: Path, name: str, expect_stems: bool, delay: float = 0.8,
                 timeout: float = 900) -> list[Path]:
    """File > Export Audio/Video (Ctrl/Cmd+Shift+R) using the settings last used in the dialog.
    The render range is the arrangement loop brace (set via the API before calling).
    One-time manual setup (see README): Rendered Track = 'All Individual Tracks',
    WAV, 24-bit, 48 kHz, Normalize off, Create Analysis File off."""
    pg = _pg()
    out_dir.mkdir(parents=True, exist_ok=True)
    t = time.time()
    focus_live()
    pg.hotkey(_mod(), "shift", "r")
    time.sleep(delay * 2)
    pg.press("enter")                 # "Export" is the dialog's default button
    time.sleep(delay * 2)
    _type_path_in_dialog(pg, out_dir, name, delay)
    log("gui_export_started", out_dir=str(out_dir), name=name)
    # Individual-track export writes a folder/files prefixed with the name; master file
    # is '<name> Master.wav' (Live 12) or '<name>.wav' for a master-only export.
    deadline = t + timeout
    found: list[Path] = []
    while time.time() < deadline:
        found = [p for p in out_dir.rglob("*.wav") if p.stat().st_mtime >= t - 1]
        master = [p for p in found if p.stem in (name, f"{name} Master", f"{name}-Master")
                  or p.stem.endswith(" Master")]
        if master and all(_stable(p, 0.5) for p in found):
            time.sleep(3)  # Live writes stems sequentially; make sure it finished
            again = [p for p in out_dir.rglob("*.wav") if p.stat().st_mtime >= t - 1]
            if len(again) == len(found):
                log("gui_export_done", files=[str(p) for p in found])
                return found
        time.sleep(2)
    raise GuiError(f"export did not finish within {timeout}s; files so far: {[p.name for p in found]}")
