"""Dry run of sketch -> full arrangement -> mix -> master against the fake Live."""
import numpy as np
import soundfile as sf

from autoproducer import build, mix, plan as planmod
from autoproducer.project import Project
from tests.test_bridge import rig  # noqa: F401  (fixture)


def _wav(path, seconds):
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(path), np.zeros((int(seconds * 22050), 2)), 22050)
    return str(path)


def test_full_build(rig, tmp_path):  # noqa: F811
    fake, live, cfg = rig
    lib = tmp_path / "lib"
    samples = [
        dict(path=_wav(lib / "loops/chords_Am_124.wav", 7.742), duration=7.742, bpm=124.0, key="A minor",
             key_conf=0.5, role="loop", is_loop=1, transient_density=3.0, low_ratio=0.1),
        dict(path=_wav(lib / "loops/top_124.wav", 7.742), duration=7.742, bpm=124.0, key="C major",
             key_conf=0.05, role="loop", is_loop=1, transient_density=6.0, low_ratio=0.05),
        dict(path=_wav(lib / "fx/riser_8.wav", 8.0), duration=8.0, bpm=0, key="", key_conf=0, role="fx",
             is_loop=0, transient_density=1, low_ratio=0.1),
    ]
    cfg.raw["presets"] = {p: "Presets/Instruments/Bass.adv" for p in ("drums", "bass", "chords", "hook", "pad")}
    p = planmod.make_plan(cfg, samples)
    proj = Project(cfg, tmp_path / "Song Project")

    sk = build.build_sketch(live, proj, p)
    assert set(sk["tracks"]) >= {"drums", "bass", "chords", "hook", "pad", "top_loop", "tonal_loop", "fx"}
    assert sum(sk["notes"].values()) > 100
    assert fake.song.loop_length == 32.0
    # originals untouched, copies inside the project
    assert all(str(proj.root) in str(proj.root / v) for v in proj.state["copied"].values())

    full = build.build_full(live, proj, p)
    tr = proj.state["tracks"]
    drums = fake.song.tracks[tr["drums"]["index"]].arrangement_clips
    assert [c.name.split("· ")[1] for c in drums] == [s["name"] for s in p["sections"]]
    fx = fake.song.tracks[tr["fx"]["index"]].arrangement_clips
    build_sec = next(s for s in p["sections"] if s["name"] == "build")
    riser = fx[0]
    assert abs(riser.start_time + 8.0 * p["bpm"] / 60 - (build_sec["start_bar"] + build_sec["bars"]) * 4) < 1e-3
    assert full["automation"]["duck"] > 0
    tonal = fake.song.tracks[tr["tonal_loop"]["index"]].arrangement_clips
    assert tonal and all(c.warping == 1 and c.warp_mode == 6 for c in tonal)

    decisions = mix.mix_tracks(live, proj)
    assert any("HPF" in d for d in decisions["chords"])
    m = mix.master_chain(live, proj)
    names = [d.name for d in fake.song.master_track.devices]
    assert names == ["Glue Compressor", "EQ Eight", "Utility", "Limiter"]
    assert live.audit_all_devices() == []
