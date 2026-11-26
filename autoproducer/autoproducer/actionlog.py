"""Append-only JSONL action log. Every OSC call, file copy, GUI step and QC result lands here."""
from __future__ import annotations

import json
import threading
import time
from pathlib import Path
from typing import Any

_lock = threading.Lock()
_path: Path | None = None


def open_log(path: str | Path) -> Path:
    global _path
    _path = Path(path)
    _path.parent.mkdir(parents=True, exist_ok=True)
    return _path


def log(kind: str, **data: Any) -> None:
    rec = {"t": round(time.time(), 3), "kind": kind, **data}
    line = json.dumps(rec, default=str, ensure_ascii=False)
    with _lock:
        if _path is not None:
            with _path.open("a", encoding="utf-8") as fh:
                fh.write(line + "\n")


def current() -> Path | None:
    return _path
