"""
AutoProducer extension for AbletonOSC (runs INSIDE Ableton Live's Python).

Installed by `autoproducer install-ext` as AbletonOSC/abletonosc/autoproducer.py
and registered in manager.py. Every handler lives under /live/ap/... and replies
on the same address with ("ok", ...) or ("error", "<message>") so the client
never has to guess whether a call worked.

Only documented Live Object Model calls are used. Nothing here writes files or
touches the .als directly. Tracks are addressed as (kind, index) where kind is
"track", "return" or "master".
"""
import traceback
from typing import Any, Tuple

import Live

from .handler import AbletonOSCHandler

API_VERSION = 1

# Clip properties the client may set / get on arrangement clips.
CLIP_PROPS_RW = (
    "name", "warping", "warp_mode", "pitch_coarse", "pitch_fine", "gain",
    "looping", "loop_start", "loop_end", "start_marker", "end_marker", "muted",
    "color_index",
)
CLIP_PROPS_R = ("length", "start_time", "end_time", "is_audio_clip", "is_midi_clip",
                "file_path", "gain_display_string")


class AutoProducerHandler(AbletonOSCHandler):
    def __init__(self, manager):
        super().__init__(manager)
        self.class_identifier = "ap"
        self._plugin_cache = None
        self._preset_cache = None

    # ------------------------------------------------------------------ helpers
    def _app(self):
        return Live.Application.get_application()

    def _track(self, kind, index):
        kind = str(kind)
        if kind == "track":
            return self.song.tracks[int(index)]
        if kind == "return":
            return self.song.return_tracks[int(index)]
        if kind == "master":
            return self.song.master_track
        raise ValueError("unknown track kind %r" % kind)

    def _arr_clip(self, track_index, clip_index):
        return self.song.tracks[int(track_index)].arrangement_clips[int(clip_index)]

    def _param(self, kind, index, device_index, param_index):
        track = self._track(kind, index)
        if str(device_index) == "mixer":
            mixer = track.mixer_device
            name = str(param_index)
            if name == "volume":
                return mixer.volume
            if name == "pan":
                return mixer.panning
            if name.startswith("send"):
                return mixer.sends[int(name[4:])]
            raise ValueError("unknown mixer parameter %r" % name)
        return track.devices[int(device_index)].parameters[int(param_index)]

    def _add(self, address, fn):
        def wrapped(params: Tuple[Any]):
            try:
                rv = fn(*params)
                if rv is None:
                    rv = ()
                return ("ok",) + tuple(rv)
            except Exception as e:  # report, never crash Live's tick
                self.logger.error("AutoProducer %s failed: %s" % (address, traceback.format_exc()))
                return ("error", "%s: %s" % (type(e).__name__, e))
        self.osc_server.add_handler(address, wrapped)

    @staticmethod
    def _walk(item, path, out, want, depth=0, max_depth=12):
        if depth > max_depth:
            return
        for child in item.children:
            child_path = path + "/" + child.name if path else child.name
            if want(child):
                out.append((child_path, child))
            if child.is_folder or (not child.is_loadable and child.children):
                AutoProducerHandler._walk(child, child_path, out, want, depth + 1, max_depth)

    @staticmethod
    def _page(items, page, page_size):
        page, page_size = int(page), int(page_size)
        chunk = items[page * page_size:(page + 1) * page_size]
        return (len(items),) + tuple(chunk)

    def _find_item(self, root, rel_path):
        node = root
        for part in [p for p in str(rel_path).replace("\\", "/").split("/") if p]:
            match = None
            for child in node.children:
                if child.name == part:
                    match = child
                    break
            if match is None:
                raise LookupError("browser item not found: %s (missing %r)" % (rel_path, part))
            node = match
        if not node.is_loadable:
            raise LookupError("browser item is not loadable: %s" % rel_path)
        return node

    def _select(self, track):
        self.song.view.selected_track = track

    def _device_info(self, track):
        return tuple(v for d in track.devices for v in (d.name, d.class_name, d.class_display_name))

    # ------------------------------------------------------------------ API
    def init_api(self):
        app = self._app

        # -- environment ------------------------------------------------------
        self._add("/live/ap/ping", lambda *_: (API_VERSION,))
        self._add("/live/ap/version", lambda *_: (
            app().get_major_version(), app().get_minor_version(), app().get_bugfix_version()))

        def song_info(*_):
            s = self.song
            return (s.tempo, s.signature_numerator, s.signature_denominator, s.song_length,
                    len(s.tracks), len(s.return_tracks), s.last_event_time)
        self._add("/live/ap/song/info", song_info)

        def song_set(prop, value):
            if prop not in ("tempo", "loop", "loop_start", "loop_length", "current_song_time",
                            "record_mode", "metronome", "root_note", "scale_name", "back_to_arranger"):
                raise ValueError("song property %r not allowed" % prop)
            setattr(self.song, prop, value)
            return (getattr(self.song, prop),)
        self._add("/live/ap/song/set", song_set)

        def song_transport(cmd):
            if cmd == "play_arrangement":
                self.song.back_to_arranger = False
                self.song.start_playing()
            elif cmd == "stop":
                self.song.stop_playing()
            elif cmd == "stop_all_clips":
                self.song.stop_all_clips()
            else:
                raise ValueError(cmd)
        self._add("/live/ap/song/transport", song_transport)

        def view_show(name):
            # "Arranger" or "Session"
            app().view.show_view(str(name))
            app().view.focus_view(str(name))
        self._add("/live/ap/view/show", view_show)

        # -- tracks -----------------------------------------------------------
        def create_track(kind, name):
            s = self.song
            if kind == "midi":
                s.create_midi_track(-1)
                t, idx = s.tracks[-1], len(s.tracks) - 1
            elif kind == "audio":
                s.create_audio_track(-1)
                t, idx = s.tracks[-1], len(s.tracks) - 1
            elif kind == "return":
                s.create_return_track()
                t, idx = s.return_tracks[-1], len(s.return_tracks) - 1
            else:
                raise ValueError(kind)
            t.name = str(name)
            return (idx,)
        self._add("/live/ap/track/create", create_track)

        def track_list(*_):
            out = []
            for i, t in enumerate(self.song.tracks):
                out += ["track", i, t.name, int(t.has_midi_input)]
            for i, t in enumerate(self.song.return_tracks):
                out += ["return", i, t.name, 0]
            return tuple(out)
        self._add("/live/ap/track/list", track_list)

        def track_set(kind, index, prop, value):
            if prop not in ("name", "mute", "solo", "arm", "color_index", "current_monitoring_state"):
                raise ValueError("track property %r not allowed" % prop)
            t = self._track(kind, index)
            setattr(t, prop, value)
            return (getattr(t, prop),)
        self._add("/live/ap/track/set", track_set)

        self._add("/live/ap/track/devices", lambda kind, index: self._device_info(self._track(kind, index)))

        def delete_device(kind, index, device_index):
            t = self._track(kind, index)
            t.delete_device(int(device_index))
            return self._device_info(t)
        self._add("/live/ap/track/delete_device", delete_device)

        def routing(kind, index, direction, type_name, channel_name=""):
            t = self._track(kind, index)
            avail = t.available_input_routing_types if direction == "input" else t.available_output_routing_types
            match = [r for r in avail if r.display_name == type_name]
            if not match:
                raise LookupError("routing %r not available; options: %s" % (
                    type_name, ", ".join(r.display_name for r in avail)))
            if direction == "input":
                t.input_routing_type = match[0]
            else:
                t.output_routing_type = match[0]
            if channel_name:
                chans = t.available_input_routing_channels if direction == "input" else t.available_output_routing_channels
                cm = [c for c in chans if c.display_name == channel_name]
                if not cm:
                    raise LookupError("routing channel %r not available" % channel_name)
                if direction == "input":
                    t.input_routing_channel = cm[0]
                else:
                    t.output_routing_channel = cm[0]
            return (type_name,)
        self._add("/live/ap/track/routing", routing)

        # -- browser: plugins & presets ---------------------------------------
        def refresh(*_):
            self._plugin_cache = None
            self._preset_cache = None
        self._add("/live/ap/browser/refresh", refresh)

        def list_plugins(page=0, page_size=40):
            if self._plugin_cache is None:
                found = []
                self._walk(app().browser.plugins, "", found, lambda c: c.is_loadable and not c.is_folder)
                self._plugin_cache = sorted(set(p for p, _ in found))
            return self._page(self._plugin_cache, page, page_size)
        self._add("/live/ap/browser/plugins", list_plugins)

        def list_presets(page=0, page_size=40):
            if self._preset_cache is None:
                found = []
                self._walk(app().browser.user_library, "", found,
                           lambda c: c.is_loadable and c.name.lower().endswith((".adv", ".adg")))
                self._preset_cache = sorted(p for p, _ in found)
            return self._page(self._preset_cache, page, page_size)
        self._add("/live/ap/browser/user_presets", list_presets)

        def load_user_preset(kind, index, rel_path):
            t = self._track(kind, index)
            item = self._find_item(app().browser.user_library, rel_path)
            before = len(t.devices)
            self._select(t)
            app().browser.load_item(item)
            return (before,) + self._device_info(t)
        self._add("/live/ap/browser/load_user_preset", load_user_preset)

        def load_native(kind, index, category, rel_path):
            # Stock Live devices for utility work only (EQ Eight, Compressor, ...).
            roots = {"audio_effects": app().browser.audio_effects}
            if category not in roots:
                raise ValueError("native category %r not allowed" % category)
            t = self._track(kind, index)
            try:
                item = self._find_item(roots[category], rel_path)
            except LookupError:
                # Live 12 may group stock devices into category folders: search by name.
                found = []
                self._walk(roots[category], "", found,
                           lambda c: c.is_loadable and c.name in (rel_path, rel_path + ".adv"))
                if not found:
                    raise
                item = found[0][1]
            before = len(t.devices)
            self._select(t)
            app().browser.load_item(item)
            return (before,) + self._device_info(t)
        self._add("/live/ap/browser/load_native", load_native)

        # -- device parameters -------------------------------------------------
        def params(kind, index, device_index, page=0, page_size=25):
            d = self._track(kind, index).devices[int(device_index)]
            rows = []
            for i, p in enumerate(d.parameters):
                rows.append((i, p.name, float(p.value), float(p.min), float(p.max), int(p.is_quantized)))
            page, page_size = int(page), int(page_size)
            chunk = rows[page * page_size:(page + 1) * page_size]
            return (len(rows),) + tuple(v for r in chunk for v in r)
        self._add("/live/ap/device/params", params)

        def set_param(kind, index, device_index, param_index, value):
            p = self._param(kind, index, device_index, param_index)
            p.value = max(p.min, min(p.max, float(value)))
            return (float(p.value), p.str_for_value(p.value))
        self._add("/live/ap/param/set", set_param)

        def get_param(kind, index, device_index, param_index):
            p = self._param(kind, index, device_index, param_index)
            return (float(p.value), float(p.min), float(p.max), p.str_for_value(p.value), p.name)
        self._add("/live/ap/param/get", get_param)

        def _display_number(p, v):
            s = p.str_for_value(v).replace("dB", "").replace("Hz", "").strip()
            if s in ("-inf", "-Inf"):
                return -1e9
            mult = 1.0
            if s.endswith("k"):
                s, mult = s[:-1], 1000.0
            return float(s) * mult

        def _edge(p, target, inclusive):
            """Smallest value whose display is >= target (inclusive) or > target, for a
            monotonic parameter; works on increasing and decreasing displays."""
            lo, hi = float(p.min), float(p.max)
            increasing = _display_number(p, hi) >= _display_number(p, lo)
            for _ in range(48):
                mid = (lo + hi) / 2.0
                d = _display_number(p, mid)
                below = (d < target) if inclusive else (d <= target)
                if below == increasing:
                    lo = mid
                else:
                    hi = mid
            return (lo + hi) / 2.0

        def _value_for(p, target):
            a, b = _edge(p, float(target), True), _edge(p, float(target), False)
            v = (a + b) / 2.0 if abs(b - a) > 1e-9 else a
            return max(float(p.min), min(float(p.max), v))

        def set_param_display(kind, index, device_index, param_index, target):
            """Set a parameter by its displayed value (dB fader, Hz cutoff, ms time...):
            the middle of the value range that displays as `target`."""
            p = self._param(kind, index, device_index, param_index)
            p.value = _value_for(p, target)
            return (float(p.value), p.str_for_value(p.value))
        self._add("/live/ap/param/set_display", set_param_display)

        def value_for_display(kind, index, device_index, param_index, target):
            """Same search as set_display but only returns the value (for envelopes)."""
            p = self._param(kind, index, device_index, param_index)
            v = _value_for(p, target)
            return (float(v), p.str_for_value(v))
        self._add("/live/ap/param/value_for_display", value_for_display)

        # -- arrangement clips --------------------------------------------------
        def arr_clips(track_index):
            t = self.song.tracks[int(track_index)]
            return tuple(v for c in t.arrangement_clips for v in (c.name, c.start_time, c.end_time))
        self._add("/live/ap/arr/list", arr_clips)

        def _index_of(track, start):
            for i, c in enumerate(track.arrangement_clips):
                if abs(c.start_time - start) < 1e-6:
                    return i
            raise LookupError("created clip not found at %s" % start)

        def create_audio_clip(track_index, path, position):
            t = self.song.tracks[int(track_index)]
            t.create_audio_clip(str(path), float(position))  # Live 12 LOM
            return (_index_of(t, float(position)),)
        self._add("/live/ap/arr/create_audio_clip", create_audio_clip)

        def create_midi_clip(track_index, start, length):
            t = self.song.tracks[int(track_index)]
            start, length = float(start), float(length)
            if hasattr(t, "create_midi_clip"):
                t.create_midi_clip(start, length)
                return (_index_of(t, start), "create_midi_clip")
            # Fallback: scratch session slot -> duplicate into arrangement -> clear slot.
            slot = None
            for s in t.clip_slots:
                if not s.has_clip:
                    slot = s
                    break
            if slot is None:
                raise RuntimeError("no free clip slot for fallback")
            slot.create_clip(length)
            t.duplicate_clip_to_arrangement(slot.clip, start)
            slot.delete_clip()
            return (_index_of(t, start), "duplicate_clip_to_arrangement")
        self._add("/live/ap/arr/create_midi_clip", create_midi_clip)

        def delete_arr_clip(track_index, clip_index):
            t = self.song.tracks[int(track_index)]
            t.delete_clip(t.arrangement_clips[int(clip_index)])
        self._add("/live/ap/arr/delete", delete_arr_clip)

        def add_notes(track_index, clip_index, *flat):
            clip = self._arr_clip(track_index, clip_index)
            notes = []
            for o in range(0, len(flat), 5):
                pitch, start, dur, vel, mute = flat[o:o + 5]
                notes.append(Live.Clip.MidiNoteSpecification(
                    start_time=float(start), duration=float(dur), pitch=int(pitch),
                    velocity=float(vel), mute=bool(mute)))
            clip.add_new_notes(tuple(notes))
            return (len(notes),)
        self._add("/live/ap/arr/add_notes", add_notes)

        def count_notes(track_index, clip_index):
            clip = self._arr_clip(track_index, clip_index)
            return (len(clip.get_notes_extended(0, 128, -8192, 16384)),)
        self._add("/live/ap/arr/count_notes", count_notes)

        def clip_set(track_index, clip_index, prop, value):
            if prop not in CLIP_PROPS_RW:
                raise ValueError("clip property %r not allowed" % prop)
            clip = self._arr_clip(track_index, clip_index)
            setattr(clip, prop, value)
            return (getattr(clip, prop),)
        self._add("/live/ap/arr/set", clip_set)

        def clip_get(track_index, clip_index, prop):
            if prop not in CLIP_PROPS_RW + CLIP_PROPS_R:
                raise ValueError("clip property %r not allowed" % prop)
            return (getattr(self._arr_clip(track_index, clip_index), prop),)
        self._add("/live/ap/arr/get", clip_get)

        def warp_markers(track_index, clip_index):
            clip = self._arr_clip(track_index, clip_index)
            return tuple(v for m in clip.warp_markers for v in (m.beat_time, m.sample_time))
        self._add("/live/ap/arr/warp_markers", warp_markers)

        def set_loop_tempo(track_index, clip_index, loop_seconds, loop_beats):
            """Make a warped clip play `loop_seconds` of audio over `loop_beats` beats:
            keep the first warp marker, drop the others, add one at the loop end."""
            clip = self._arr_clip(track_index, clip_index)
            markers = list(clip.warp_markers)
            if not markers:
                raise RuntimeError("clip has no warp markers (is warping on?)")
            first = markers[0]
            for m in markers[1:]:
                clip.remove_warp_marker(m.beat_time)
            clip.add_warp_marker(Live.Clip.WarpMarker(
                sample_time=first.sample_time + float(loop_seconds),
                beat_time=first.beat_time + float(loop_beats)))
            return tuple(v for m in clip.warp_markers for v in (m.beat_time, m.sample_time))
        self._add("/live/ap/arr/set_loop_tempo", set_loop_tempo)

        def envelope(track_index, clip_index, kind, index, device_index, param_index, *flat):
            """Write clip automation as steps: flat = t0, dur0, v0, t1, dur1, v1, ...
            Times are clip-relative beats; values in the parameter's native range."""
            clip = self._arr_clip(track_index, clip_index)
            p = self._param(kind, index, device_index, param_index)
            env = clip.automation_envelope(p)
            if env is None:
                env = clip.create_automation_envelope(p)
            if env is None:
                raise RuntimeError("parameter %s cannot be automated in this clip" % p.name)
            n = 0
            for o in range(0, len(flat), 3):
                t, d, v = flat[o:o + 3]
                env.insert_step(float(t), float(d), max(p.min, min(p.max, float(v))))
                n += 1
            return (n,)
        self._add("/live/ap/arr/envelope", envelope)

        def clear_envelopes(track_index, clip_index):
            self._arr_clip(track_index, clip_index).clear_all_envelopes()
        self._add("/live/ap/arr/clear_envelopes", clear_envelopes)

        # -- one-shot drum hits (drums.mode = audio_hits) ------------------------------
        def _db_of(clip):
            s = str(clip.gain_display_string).replace("dB", "").strip()
            return -1e9 if "inf" in s else float(s)

        gain_cache = {}

        def _gain_for_db(clip, db):
            """Clip.gain is normalised (0..1, not linear dB); find the value whose
            display reads `db` by bisection, cached per 0.5 dB step."""
            key = round(db * 2) / 2.0
            if key in gain_cache:
                return gain_cache[key]
            lo, hi = 0.0, 1.0
            for _ in range(30):
                mid = (lo + hi) / 2.0
                clip.gain = mid
                if _db_of(clip) < key:
                    lo = mid
                else:
                    hi = mid
            gain_cache[key] = (lo + hi) / 2.0
            return gain_cache[key]

        def place_hits(track_index, path, name, next_pos, *flat):
            """flat = pos0, db0, pos1, db1, ... (beats, clip gain dB), sorted by position;
            next_pos = position of the first hit of the following batch (-1 if none).
            Each hit: unwarped audio clip, gain set from velocity, end trimmed so it never
            overlaps the next hit on the same track."""
            t = self.song.tracks[int(track_index)]
            hits = [(float(flat[i]), float(flat[i + 1])) for i in range(0, len(flat), 2)]
            sec_per_beat = 60.0 / self.song.tempo
            placed, trim_failed = 0, 0
            for i, (pos, db) in enumerate(hits):
                t.create_audio_clip(str(path), pos)
                clip = t.arrangement_clips[_index_of(t, pos)]
                clip.warping = False
                clip.name = str(name)
                clip.gain = _gain_for_db(clip, db)
                nxt = hits[i + 1][0] if i + 1 < len(hits) else float(next_pos)
                if nxt > pos:
                    gap = nxt - pos
                    if clip.end_time - clip.start_time > gap + 1e-4:
                        # unwarped clip markers are in seconds; verify, else try beats
                        for unit in (sec_per_beat, 1.0):
                            try:
                                clip.end_marker = clip.start_marker + gap * unit
                            except Exception:
                                continue
                            if clip.end_time - clip.start_time <= gap + 1e-3:
                                break
                        else:
                            trim_failed += 1
                placed += 1
            return (placed, trim_failed)
        self._add("/live/ap/arr/place_hits", place_hits)
