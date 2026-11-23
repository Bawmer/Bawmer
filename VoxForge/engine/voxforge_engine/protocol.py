"""JSON-lines protocol between the VoxForge plug-in and this engine.

Every message is one JSON object on its own line on stdout:
    {"type": "progress", "value": 0.42, "message": "epoch 12/200"}
    {"type": "log", "message": "..."}
    {"type": "result", ...}
    {"type": "error", "message": "..."}
Anything else printed by third-party libraries is shown as a plain log line.
"""
import json
import sys
import time

_last_progress = 0.0


def emit(kind: str, **payload):
    payload["type"] = kind
    sys.stdout.write(json.dumps(payload, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def log(message: str):
    emit("log", message=str(message))


def progress(value: float, message: str = "", force: bool = False):
    """Rate-limited progress (max ~4 messages per second)."""
    global _last_progress
    now = time.time()
    if force or now - _last_progress > 0.25 or value >= 1.0:
        _last_progress = now
        emit("progress", value=max(0.0, min(1.0, float(value))), message=message)


def result(**payload):
    emit("result", **payload)


def error(message: str):
    emit("error", message=str(message))
