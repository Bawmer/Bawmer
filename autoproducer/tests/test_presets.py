import pytest

from autoproducer import build, presets_pick
from autoproducer.project import Project
from tests.test_bridge import rig  # noqa: F401

LIB = ["Presets/Instruments/Sub Bass Deep.adv", "Presets/Instruments/Reese Bass.adv",
       "Presets/Instruments/Festival Supersaw Lead.adv", "Presets/Instruments/Pluck Lead Bright.adv",
       "Presets/Instruments/Big Room Chords.adv", "Presets/Instruments/Piano Keys.adv",
       "Presets/Instruments/Airy Pad.adv", "Presets/Instruments/Init.adv",
       "Presets/Instruments/Drum Kit 808.adv", "Presets/Audio Effects/Hall Reverb.adv",
       "Presets/Instruments/Kit.adg"]


def test_ranking_prefers_role_and_genre_words():
    p = presets_pick.pick_all(LIB, ["bass", "chords", "hook", "pad"], "progressive_house")
    assert p["bass"][0].endswith(("Sub Bass Deep.adv", "Reese Bass.adv"))
    assert p["chords"][0].endswith("Big Room Chords.adv")
    assert p["hook"][0].endswith(("Festival Supersaw Lead.adv", "Pluck Lead Bright.adv"))
    assert p["pad"][0].endswith("Airy Pad.adv")
    firsts = [v[0] for v in p.values()]
    assert len(set(firsts)) == 4                       # one preset per role
    flat = {x for v in p.values() for x in v}
    assert not any("Init" in x or "Drum Kit" in x or x.endswith(".adg") for x in flat)


def test_no_match_gives_empty_list():
    assert presets_pick.pick_all(["Presets/Instruments/Foo.adv"], ["pad"], "house") == {"pad": []}


def test_auto_loads_vst3_and_skips_stock_instrument(rig, tmp_path):  # noqa: F811
    fake, live, cfg = rig
    cfg.raw["plugin_whitelist"] = "all"
    cfg.raw["banned_plugins"] = []
    cfg.raw["presets"] = {"bass": "auto", "chords": "auto", "hook": "auto", "pad": "auto"}
    cfg.raw["allow_stock_utility"] = False
    proj = Project(cfg, tmp_path / "P")
    tracks = build.setup_tracks(live, proj, {"samples": {}})
    hook = tracks["hook"]
    assert hook["preset"].endswith("Lead Supersaw Festival.adv") and hook["auto_picked"]
    assert tracks["chords"]["preset"].endswith("Big Chords Supersaw.adv")
    assert tracks["pad"]["preset"].endswith("Warm Pad.adv")
    for t in tracks.values():
        if t.get("instrument"):
            assert t["instrument_class"] == "PluginDevice"


def test_explicit_stock_preset_is_refused(rig, tmp_path):  # noqa: F811
    fake, live, cfg = rig
    cfg.raw["plugin_whitelist"] = "all"
    t = live.create_track("midi", "Lead")
    from autoproducer.live import WhitelistViolation
    with pytest.raises(WhitelistViolation, match="not a VST3"):
        live.load_preset("track", t, "Presets/Instruments/Lead Wavetable.adv", None, require_plugin=True)
    assert fake.song.tracks[t].devices == []
