"""Minimal stand-in for Live's Python API + AbletonOSC server, so the real extension
(ext/autoproducer_handler.py) can be exercised over real UDP in tests."""
import importlib.util
import math
import sys
import threading
import types
from pathlib import Path

from pythonosc import dispatcher, osc_server, udp_client

EXT = Path(__file__).resolve().parent.parent / "ext" / "autoproducer_handler.py"


class Param:
    def __init__(self, name, value=0.0, lo=0.0, hi=1.0, fmt=None, quantized=False):
        self.name, self.value, self.min, self.max = name, value, lo, hi
        self.is_quantized = quantized
        self._fmt = fmt or (lambda v: f"{v:.2f}")

    def str_for_value(self, v):
        return self._fmt(v)


def volume_fmt(v):  # Live-like fader: 0.85 = 0 dB, log-ish curve
    if v <= 0:
        return "-inf dB"
    return f"{40 * math.log10(v / 0.85):.1f} dB"


class Mixer:
    def __init__(self, n_sends):
        self.volume = Param("Track Volume", 0.85, 0.0, 1.0, volume_fmt)
        self.panning = Param("Track Panning", 0.0, -1.0, 1.0)
        self.sends = [Param(f"Send {i}") for i in range(n_sends)]


class Device:
    def __init__(self, name, class_name, display=None, params=()):
        self.name, self.class_name = name, class_name
        self.class_display_name = display or name
        self.parameters = [Param("Device On", 1.0)] + list(params)


class Note:
    def __init__(self, **kw):
        self.__dict__.update(kw)


class Clip:
    def __init__(self, start, length, path=None):
        self.start_time, self.length = start, length
        self.end_time = start + length
        self.name, self.file_path, self._notes = "", path, []
        self.is_audio_clip, self.is_midi_clip = path is not None, path is None
        self.warping, self.warp_mode, self.pitch_coarse, self.gain = 0, 0, 0, 0.5
        self.start_marker, self._end_marker = 0.0, length
        self.warp_markers = [types.SimpleNamespace(beat_time=0.0, sample_time=0.0),
                             types.SimpleNamespace(beat_time=8.0, sample_time=4.0)] if path else []
        self.envelopes = {}

    @property
    def end_marker(self):
        return self._end_marker

    @end_marker.setter
    def end_marker(self, v):  # like Live: unwarped markers in seconds (tempo 120 -> 0.5 s/beat)
        self._end_marker = v
        beats = v / 0.5 if (self.file_path and not self.warping) else v
        self.end_time = self.start_time + beats

    @property
    def gain_display_string(self):
        return "-inf dB" if self.gain <= 0 else f"{40 * math.log10(self.gain / 0.4):.1f} dB"

    def remove_warp_marker(self, beat_time):
        self.warp_markers = [m for m in self.warp_markers if m.beat_time != beat_time]

    def add_warp_marker(self, m):
        self.warp_markers.append(m)
        self.warp_markers.sort(key=lambda m: m.beat_time)

    def clear_all_envelopes(self):
        self.envelopes = {}

    def add_new_notes(self, notes):
        self._notes += list(notes)

    def get_notes_extended(self, *a):
        return self._notes

    def automation_envelope(self, p):
        return self.envelopes.get(id(p))

    def create_automation_envelope(self, p):
        env = types.SimpleNamespace(steps=[])
        env.insert_step = lambda t, d, v: env.steps.append((t, d, v))
        self.envelopes[id(p)] = env
        return env


class Track:
    def __init__(self, name, midi, song):
        self.name, self.has_midi_input = name, midi
        self.devices, self.arrangement_clips = [], []
        self.mixer_device = Mixer(len(song.return_tracks))
        self.color_index = 0

    def create_midi_clip(self, start, length):
        self.arrangement_clips.append(Clip(start, length))
        self.arrangement_clips.sort(key=lambda c: c.start_time)

    def create_audio_clip(self, path, pos):
        self.arrangement_clips.append(Clip(pos, 8.0, path))
        self.arrangement_clips.sort(key=lambda c: c.start_time)

    def delete_device(self, i):
        del self.devices[i]

    def delete_clip(self, c):
        self.arrangement_clips.remove(c)


class Item:
    def __init__(self, name, children=(), loadable=False, payload=None):
        self.name, self.children = name, list(children)
        self.is_loadable, self.is_folder = loadable, bool(children) and not loadable
        self.payload = payload


class Song:
    def __init__(self):
        self.tracks, self.return_tracks = [], []
        self.master_track = types.SimpleNamespace(name="Master", devices=[], mixer_device=Mixer(0))
        self.view = types.SimpleNamespace(selected_track=None)
        self.tempo, self.signature_numerator, self.signature_denominator = 120.0, 4, 4
        self.song_length, self.last_event_time = 0.0, 0.0
        self.loop, self.loop_start, self.loop_length = 0, 0.0, 16.0

    def create_midi_track(self, i):
        self.tracks.append(Track("MIDI", True, self))

    def create_audio_track(self, i):
        self.tracks.append(Track("Audio", False, self))

    def create_return_track(self):
        self.return_tracks.append(Track("Return", False, self))
        for t in self.tracks:
            t.mixer_device.sends.append(Param("Send"))


def make_live_module(song, browser):
    live = types.ModuleType("Live")
    app = types.SimpleNamespace(browser=browser, get_major_version=lambda: 12, get_minor_version=lambda: 4,
                                get_bugfix_version=lambda: 3, view=types.SimpleNamespace(
                                    show_view=lambda n: None, focus_view=lambda n: None))
    live.Application = types.SimpleNamespace(get_application=lambda: app)
    live.Clip = types.SimpleNamespace(MidiNoteSpecification=Note,
                                      WarpMarker=lambda **kw: types.SimpleNamespace(**kw))
    return live


def make_browser(song):
    def plugin(name):
        return Device(name, "PluginDevice", name, [Param("Cutoff", 0.5)])

    def stock(name):
        if name == "EQ Eight":
            ps = [Param("1 Filter On A"), Param("1 Filter Type A", 3, 0, 7, quantized=True),
                  Param("1 Frequency A", 0.5, 0, 1, lambda v: f"{10 * (2000 ** v):.0f} Hz")]
        elif name == "Utility":
            ps = [Param("Gain", 0.5, 0, 1, lambda v: f"{(v - 0.5) * 70:.1f} dB")]
        else:
            ps = []
        return Device(name, {"EQ Eight": "Eq8", "Utility": "StereoGain"}.get(name, name.replace(" ", "")),
                      name, ps)

    browser = types.SimpleNamespace()
    browser.plugins = Item("Plug-Ins", [Item("VST3", [Item("Vendor", [
        Item("Good Synth", loadable=True, payload=lambda: plugin("Good Synth")),
        Item("Bad Synth", loadable=True, payload=lambda: plugin("Bad Synth"))])])])
    browser.user_library = Item("User Library", [Item("Presets", [Item("Instruments", [
        Item("Bass.adv", loadable=True, payload=lambda: plugin("Good Synth")),
        Item("Evil.adv", loadable=True, payload=lambda: plugin("Bad Synth"))])])])
    browser.audio_effects = Item("Audio Effects", [
        Item(n, loadable=True, payload=(lambda n=n: stock(n))) for n in ("EQ Eight", "Utility", "Auto Filter")
    ] + [Item("Dynamics", [Item(n, loadable=True, payload=(lambda n=n: stock(n)))
                           for n in ("Compressor", "Glue Compressor", "Limiter")])])

    def load_item(item):
        target = song.view.selected_track
        target.devices.append(item.payload())
    browser.load_item = load_item
    return browser


class FakeAbletonOSC:
    """Runs the real extension behind a python-osc UDP server."""

    def __init__(self, listen_port, reply_port):
        self.song = Song()
        browser = make_browser(self.song)
        sys.modules["Live"] = make_live_module(self.song, browser)
        pkg = types.ModuleType("fakeosc")
        pkg.__path__ = []
        sys.modules["fakeosc"] = pkg
        handler_mod = types.ModuleType("fakeosc.handler")
        callbacks = {}
        song = self.song

        class Server:
            def add_handler(self, address, fn):
                callbacks[address] = fn

        class AbletonOSCHandler:
            def __init__(self, manager):
                import logging
                self.logger = logging.getLogger("fake")
                self.osc_server = Server()
                self.song = song
                self.init_api()

        handler_mod.AbletonOSCHandler = AbletonOSCHandler
        sys.modules["fakeosc.handler"] = handler_mod
        spec = importlib.util.spec_from_file_location("fakeosc.autoproducer", EXT)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        self.handler = mod.AutoProducerHandler(None)
        self.reply = udp_client.SimpleUDPClient("127.0.0.1", reply_port)

        def on(address, *args):
            if address == "/live/test":
                self.reply.send_message("/live/test", ["ok"])
                return
            fn = callbacks.get(address)
            if fn is None:
                return
            rv = fn(tuple(args))
            if rv is not None:
                self.reply.send_message(address, list(rv))

        d = dispatcher.Dispatcher()
        d.set_default_handler(on)
        self.server = osc_server.ThreadingOSCUDPServer(("127.0.0.1", listen_port), d)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
