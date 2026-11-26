import itertools

import pytest

from autoproducer import compose as comp
from autoproducer import config, plan
from autoproducer.theory import Key, compatible, estimate_key, semitone_shift
import numpy as np

LOOP = dict(path="/lib/loops/chords_Am_124.wav", duration=7.742, bpm=124.0, key="A minor", key_conf=0.5,
            role="loop", is_loop=1, transient_density=3.0, low_ratio=0.1)
TOP = dict(path="/lib/loops/top_124.wav", duration=7.742, bpm=124.0, key="C major", key_conf=0.05,
           role="loop", is_loop=1, transient_density=6.0, low_ratio=0.05)
RISER = dict(path="/lib/fx/riser_8bar.wav", duration=8.0, bpm=0, key="", key_conf=0, role="fx",
             is_loop=0, transient_density=1, low_ratio=0.1)


def shot(role, name, dur=0.4):
    return dict(path=f"/lib/drums/{name}.wav", duration=dur, bpm=0, key="", key_conf=0, role=role,
                is_loop=0, transient_density=2, low_ratio=0.6 if role == "kick" else 0.1)


SHOTS = [shot("kick", "kick_01"), shot("snare", "snare_01"), shot("clap", "clap_01"),
         shot("hat", "hat_01", 0.15), shot("perc", "rim_01", 0.2)]


def make(genre="house", samples=None, **kw):
    cfg = config.from_dict({"genre": genre, "sample_library": ["/lib"], "project_path": "/p", "seed": 7, **kw})
    return plan.make_plan(cfg, samples if samples is not None else [LOOP, TOP, RISER] + SHOTS)


def test_audio_hits_is_default_and_uses_one_shots():
    p = make()
    assert p["drum_mode"] == "audio_hits"
    assert p["samples"]["kick"][0]["path"].endswith("kick_01.wav")
    assert "kick" in next(s for s in p["sections"] if s["name"] == "drop")["layers"]


def test_audio_hits_without_kick_stops():
    with pytest.raises(plan.PlanError, match="kick"):
        make(samples=[LOOP, TOP, RISER] + SHOTS[1:])


def test_midi_drum_mode_needs_no_one_shots():
    p = make(samples=[LOOP, TOP, RISER], drums={"mode": "midi"})
    assert "kick" in next(s for s in p["sections"] if s["name"] == "drop")["layers"]


def test_key_parse_and_shift():
    assert str(Key.parse("Am")) == "A minor"
    assert str(Key.parse("F# major")) == "F# major"
    assert semitone_shift(Key.parse("A minor"), Key.parse("B minor")) == 2
    assert semitone_shift(Key.parse("C major"), Key.parse("A minor")) == 0  # relative
    assert compatible(Key.parse("G minor"), Key.parse("A minor"), 2) == (True, 2)


def test_estimate_key_from_triad_chroma():
    chroma = np.zeros(12)
    chroma[[9, 0, 4]] = [1.0, 0.8, 0.8]  # A C E
    chroma[[2, 7, 11, 5]] = 0.3
    k, conf = estimate_key(chroma)
    assert str(k) in ("A minor", "C major")


@pytest.mark.parametrize("genre", ["house", "techno", "dnb", "trap", "pop", "lofi"])
def test_plan_fits_length_and_is_deterministic(genre):
    p1, p2 = make(genre), make(genre)
    assert p1 == p2
    assert 180 - 20 <= p1["duration_seconds"] <= 210 + 20
    assert [s["name"] for s in p1["sections"]] == ["intro", "verse", "build", "drop", "verse2", "drop2", "outro"]


def test_plan_prefers_library_key_and_bpm():
    p = make()
    assert p["bpm"] == 124
    assert p["key"] == "A minor"
    assert p["samples"]["tonal_loop"][0]["transpose"] == 0
    assert p["samples"]["riser"]


def test_user_key_forces_transpose():
    p = make(key="B minor")
    assert p["samples"]["tonal_loop"][0]["transpose"] == -2 or p["samples"]["tonal_loop"][0]["transpose"] == 2


@pytest.mark.parametrize("genre", ["house", "techno", "dnb", "trap", "pop", "lofi"])
def test_all_tonal_notes_in_key(genre):
    p = make(genre)
    key = Key.parse(p["key"])
    parts = comp.compose(p)
    for part in ("bass", "chords", "pad"):
        for c in parts[part]:
            bad = [n.pitch for n in c.notes if not key.in_key(n.pitch)]
            assert not bad, (part, c.section, bad[:5])
    hook_bad = [n.pitch for c in parts["hook"] for n in c.notes if not key.in_key(n.pitch)]
    # grace notes may be chromatic neighbours only when the diatonic step is impossible
    assert len(hook_bad) <= 2


@pytest.mark.parametrize("genre", ["dnb", "trap", "lofi", "pop"])
def test_bass_locked_to_kick(genre):
    p = make(genre)
    parts = comp.compose(p)
    for c in parts["bass"]:
        drums = next(d for d in parts["drums"] if d.section == c.section)
        kicks = {round(n.start * 4) / 4 for n in drums.notes if n.tag == "kick"}
        onsets = {round(n.start * 4) / 4 for n in c.notes}
        if genre == "pop":  # 8th-note bass that starts on each kick
            assert kicks & onsets and len(kicks - onsets) <= len(kicks) * 0.1
        else:
            assert len(onsets - kicks) <= len(onsets) * 0.1, c.section


def test_house_bass_interlocks_offbeat():
    p = make("house")
    parts = comp.compose(p)
    for c in parts["bass"]:
        for n in c.notes:
            assert abs((n.start % 1) - 0.5) < 0.05


def test_no_static_loop_copy_paste():
    p = make()
    parts = comp.compose(p)
    drop = next(c for c in parts["drums"] if c.section == "drop")
    bars = [tuple(sorted((round(n.start - b * 4, 2), n.tag, n.vel) for n in drop.notes if b * 4 <= n.start < b * 4 + 4))
            for b in range(16)]
    blocks = [tuple(bars[i:i + 4]) for i in range(0, 16, 4)]
    assert len(set(blocks)) == len(blocks), "identical 4-bar drum blocks"


def test_fills_and_build_roll():
    p = make()
    parts = comp.compose(p)
    build = next(c for c in parts["drums"] if c.section == "build")
    last_bar = [n for n in build.notes if n.start >= build.length_beats - 4]
    first_bar = [n for n in build.notes if n.start < 4]
    assert len([n for n in last_bar if n.tag == "snare"]) > len([n for n in first_bar if n.tag == "snare"])
    assert not [n for n in build.notes if n.start >= build.length_beats - 1 + 0.02]  # silence before drop
    verse = next(c for c in parts["drums"] if c.section == "verse2")  # verse2 -> drop2 gets a fill
    tail = [n for n in verse.notes if n.start >= verse.length_beats - 2]
    assert len([n for n in tail if n.tag in ("snare", "perc")]) >= 4


def test_hook_repeats_motif_with_call_and_response():
    p = make()
    parts = comp.compose(p)
    drop = next(c for c in parts["hook"] if c.section == "drop")
    def phrase(b0):
        return [n.pitch for n in drop.notes if b0 * 4 <= n.start < (b0 + 2) * 4]
    call, response, call2 = phrase(0), phrase(2), phrase(4)
    assert call and response
    assert call != response           # answer differs
    assert call[:3] == call2[:3]      # motif comes back
    assert phrase(0)[:6] == phrase(8)[:6]  # second 8-bar phrase repeats the hook


def test_humanized_velocities_and_timing():
    p = make()
    parts = comp.compose(p)
    hats = [n for c in parts["drums"] for n in c.notes if n.tag == "hat_closed"]
    assert len({n.vel for n in hats}) > 5
    off_grid = [n for n in hats if abs(n.start * 4 - round(n.start * 4)) > 1e-4]
    assert len(off_grid) > len(hats) * 0.5


def test_sketch_is_eight_bars():
    p = make()
    sk = comp.sketch(p)
    for part, clips in sk.items():
        assert clips[0].length_beats == 32
        assert all(n.start < 32 for n in clips[0].notes)
    assert {"drums", "bass", "chords", "hook"} <= set(sk)


def test_note_tuples_valid():
    p = make("trap")
    for part, clips in comp.compose(p).items():
        if part.startswith("_"):
            continue
        for c in clips:
            for n in c.notes:
                pitch, start, dur, vel, mute = n.as_tuple()
                assert 0 <= start < c.length_beats and dur > 0 and 1 <= vel <= 127
