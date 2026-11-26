"""High-level Live operations on top of the OSC client (one place for every LOM call)."""
from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Iterable

from .actionlog import log
from .config import Config
from .osc import LiveError, OSCClient

WARP_MODES = {"beats": 0, "tones": 1, "texture": 2, "repitch": 3, "complex": 4, "complex_pro": 6}
NOTES_PER_MESSAGE = 120  # keeps each UDP datagram well under 64 KB


class WhitelistViolation(LiveError):
    pass


@dataclass
class Device:
    index: int
    name: str
    class_name: str
    display: str

    @property
    def is_plugin(self) -> bool:
        return self.class_name in ("PluginDevice", "AuPluginDevice", "VstPluginDevice", "Vst3PluginDevice")


class Live:
    def __init__(self, cfg: Config, client: OSCClient | None = None):
        self.cfg = cfg
        o = cfg.osc
        self.c = client or OSCClient(o["host"], o["send_port"], o["recv_port"], o["timeout"])

    # -- environment -----------------------------------------------------------
    def test(self) -> bool:
        return self.c.query("/live/test", timeout=3.0)[0] == "ok"

    def version(self) -> tuple[int, int, int]:
        try:
            return tuple(int(v) for v in self.c.ap("/live/ap/version"))  # type: ignore[return-value]
        except LiveError:
            major, minor = self.c.query("/live/application/get/version")
            return int(major), int(minor), -1

    def ext_ok(self) -> bool:
        try:
            return int(self.c.ap("/live/ap/ping", timeout=3.0)[0]) >= 1
        except LiveError:
            return False

    def song_info(self) -> dict:
        tempo, num, den, length, ntracks, nreturns, last = self.c.ap("/live/ap/song/info")
        return {"tempo": tempo, "signature": f"{num}/{den}", "song_length": length,
                "tracks": ntracks, "returns": nreturns, "last_event_time": last}

    def set_song(self, prop: str, value) -> None:
        self.c.ap("/live/ap/song/set", prop, value)

    def plugins(self, refresh: bool = False) -> list[str]:
        if refresh:
            self.c.ap("/live/ap/browser/refresh")
        return self.c.ap_paged("/live/ap/browser/plugins", page_size=40)

    def vst3_names(self, refresh: bool = False) -> set[str]:
        """Lowercased names of every plugin Live lists under a 'VST3' folder of the Plug-Ins browser."""
        if refresh or getattr(self, "_vst3", None) is None:
            self._vst3 = {p.split("/")[-1].strip().lower() for p in self.plugins(refresh)
                          if p.split("/")[0].strip().lower() in ("vst3", "vst 3", "vst3 plug-ins")}
        return self._vst3

    def plugin_ok(self, d: "Device") -> bool:
        names = self.vst3_names() if self.cfg.all_vst3 else None
        return self.cfg.plugin_allowed(d.name, names) or self.cfg.plugin_allowed(d.display, names)

    def user_presets(self) -> list[str]:
        return self.c.ap_paged("/live/ap/browser/user_presets", page_size=40)

    # -- tracks ----------------------------------------------------------------
    def tracks(self) -> list[dict]:
        flat = self.c.ap("/live/ap/track/list")
        return [{"kind": flat[i], "index": flat[i + 1], "name": flat[i + 2], "midi": bool(flat[i + 3])}
                for i in range(0, len(flat), 4)]

    def create_track(self, kind: str, name: str) -> int:
        return int(self.c.ap("/live/ap/track/create", kind, name)[0])

    def set_track(self, kind: str, index: int, prop: str, value) -> None:
        self.c.ap("/live/ap/track/set", kind, index, prop, value)

    def devices(self, kind: str, index: int) -> list[Device]:
        flat = self.c.ap("/live/ap/track/devices", kind, index)
        return [Device(i // 3, flat[i], flat[i + 1], flat[i + 2]) for i in range(0, len(flat), 3)]

    # -- device loading with whitelist enforcement (hard rule 2) -------------------
    def load_preset(self, kind: str, index: int, preset_path: str, expect_plugin: str | None) -> Device:
        """Load a User Library preset, then VERIFY what actually got inserted.
        Any inserted plugin that is not whitelisted is deleted and the run stops."""
        reply = self.c.ap("/live/ap/browser/load_user_preset", kind, index, preset_path, timeout=30.0)
        before = int(reply[0])
        time.sleep(0.5)
        devs = self.devices(kind, index)
        new = devs[before:] if len(devs) > before else []
        if not new:
            raise LiveError(f"loading {preset_path!r} added no device to {kind} {index}")
        self._enforce(kind, index, devs)
        main = new[0]
        if expect_plugin:
            names = [d.name for d in new] + [d.display for d in new]
            if not any(expect_plugin.lower() in n.lower() for n in names):
                log("preset_unexpected_device", preset=preset_path, expected=expect_plugin,
                    got=[(d.name, d.class_name) for d in new])
                raise LiveError(f"{preset_path!r} loaded {[d.name for d in new]}, expected {expect_plugin!r}")
        log("device_loaded", track=(kind, index), preset=preset_path,
            devices=[(d.name, d.class_name, d.display) for d in new])
        return main

    def load_stock(self, kind: str, index: int, device_name: str) -> Device:
        if not self.cfg.stock_allowed(device_name):
            raise WhitelistViolation(f"stock device {device_name!r} not allowed by config")
        before = int(self.c.ap("/live/ap/browser/load_native", kind, index, "audio_effects", device_name,
                               timeout=20.0)[0])
        time.sleep(0.3)
        devs = self.devices(kind, index)
        if len(devs) <= before:
            raise LiveError(f"stock device {device_name!r} did not load on {kind} {index}")
        dev = devs[before]
        if dev.is_plugin or dev.name != device_name and dev.display != device_name:
            raise LiveError(f"expected stock {device_name!r}, got {dev.name!r} ({dev.class_name})")
        log("device_loaded", track=(kind, index), stock=device_name)
        return dev

    def _enforce(self, kind: str, index: int, devs: Iterable[Device]) -> None:
        for d in devs:
            if d.is_plugin and not self.plugin_ok(d):
                self.c.ap("/live/ap/track/delete_device", kind, index, d.index)
                log("whitelist_violation_removed", track=(kind, index), device=d.name, cls=d.class_name)
                raise WhitelistViolation(
                    f"plugin {d.name!r} on {kind} {index} is not whitelisted; it was removed. Stopping.")
            if not d.is_plugin and d.class_name not in _ALLOWED_NATIVE_CLASSES:
                if not self.cfg.stock_allowed(d.display or d.name):
                    # a preset rack/stock device we did not ask for
                    log("non_plugin_device_in_preset", track=(kind, index), device=d.name, cls=d.class_name)

    def audit_all_devices(self) -> list[str]:
        problems = []
        for t in self.tracks() + [{"kind": "master", "index": 0, "name": "Master"}]:
            for d in self.devices(t["kind"], t["index"]):
                if d.is_plugin and not self.plugin_ok(d):
                    problems.append(f"{t['name']}: {d.name} ({d.class_name})")
        return problems

    # -- parameters --------------------------------------------------------------
    def params(self, kind: str, index: int, device_index: int) -> list[dict]:
        rows = self.c.ap_paged("/live/ap/device/params", kind, index, device_index, page_size=25, stride=6)
        return [{"index": r[0], "name": r[1], "value": r[2], "min": r[3], "max": r[4], "quantized": bool(r[5])}
                for r in rows]

    def find_param(self, params: list[dict], *names: str) -> dict | None:
        lowered = {p["name"].lower(): p for p in params}
        for n in names:
            if n.lower() in lowered:
                return lowered[n.lower()]
        for n in names:
            for pn, p in lowered.items():
                if n.lower() in pn:
                    return p
        return None

    def set_param(self, kind: str, index: int, device, param, value: float) -> tuple[float, str]:
        v, s = self.c.ap("/live/ap/param/set", kind, index, device, param, float(value))
        return float(v), s

    def set_param_display(self, kind: str, index: int, device, param, target: float) -> tuple[float, str]:
        v, s = self.c.ap("/live/ap/param/set_display", kind, index, device, param, float(target))
        return float(v), s

    def value_for_display(self, kind: str, index: int, device, param, target: float) -> float:
        return float(self.c.ap("/live/ap/param/value_for_display", kind, index, device, param, float(target))[0])

    def set_volume_db(self, kind: str, index: int, db: float) -> str:
        return self.set_param_display(kind, index, "mixer", "volume", db)[1]

    def get_param(self, kind: str, index: int, device, param) -> dict:
        v, lo, hi, s, name = self.c.ap("/live/ap/param/get", kind, index, device, param)
        return {"value": v, "min": lo, "max": hi, "display": s, "name": name}

    # -- arrangement clips ---------------------------------------------------------
    def midi_clip(self, track: int, start: float, length: float, notes: list[tuple], name: str) -> int:
        idx, method = self.c.ap("/live/ap/arr/create_midi_clip", track, float(start), float(length))
        idx = int(idx)
        for i in range(0, len(notes), NOTES_PER_MESSAGE):
            flat = [v for n in notes[i:i + NOTES_PER_MESSAGE] for v in n]
            self.c.ap("/live/ap/arr/add_notes", track, idx, *flat)
        self.c.ap("/live/ap/arr/set", track, idx, "name", name)
        count = int(self.c.ap("/live/ap/arr/count_notes", track, idx)[0])
        if count != len(notes):
            raise LiveError(f"clip {name!r}: wrote {len(notes)} notes, Live reports {count}")
        return idx

    def audio_clip(self, track: int, path: str, position: float) -> int:
        return int(self.c.ap("/live/ap/arr/create_audio_clip", track, path, float(position), timeout=30.0)[0])

    def clip_set(self, track: int, clip: int, prop: str, value) -> None:
        self.c.ap("/live/ap/arr/set", track, clip, prop, value)

    def clip_get(self, track: int, clip: int, prop: str):
        return self.c.ap("/live/ap/arr/get", track, clip, prop)[0]

    def warp_markers(self, track: int, clip: int) -> list[tuple[float, float]]:
        flat = self.c.ap("/live/ap/arr/warp_markers", track, clip)
        return [(flat[i], flat[i + 1]) for i in range(0, len(flat), 2)]

    def set_loop_tempo(self, track: int, clip: int, seconds: float, beats: float) -> None:
        self.c.ap("/live/ap/arr/set_loop_tempo", track, clip, float(seconds), float(beats))

    def envelope(self, track: int, clip: int, target: tuple, steps: list[tuple[float, float, float]]) -> int:
        kind, index, device, param = target
        flat = [float(v) for s in steps for v in s]
        n = 0
        for i in range(0, len(flat), 3 * 100):
            n += int(self.c.ap("/live/ap/arr/envelope", track, clip, kind, index, device, param,
                               *flat[i:i + 300])[0])
        return n

    def transport(self, cmd: str) -> None:
        self.c.ap("/live/ap/song/transport", cmd)

    def show(self, view: str) -> None:
        self.c.ap("/live/ap/view/show", view)


_ALLOWED_NATIVE_CLASSES = {"InstrumentGroupDevice", "DrumGroupDevice", "AudioEffectGroupDevice",
                           "MidiEffectGroupDevice"}


def ramp(t0: float, t1: float, value0: float, value1: float,
         steps: int = 32, curve: float = 1.0) -> list[tuple[float, float, float]]:
    """Automation as `steps` stair-steps from (t0, value0) to (t1, value1)."""
    out = []
    dt = (t1 - t0) / steps
    for i in range(steps):
        x = (i / max(steps - 1, 1)) ** curve
        out.append((t0 + i * dt, dt, value0 + (value1 - value0) * x))
    return out
