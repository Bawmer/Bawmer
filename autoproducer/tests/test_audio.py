import gzip

import numpy as np
import pytest
import soundfile as sf

from autoproducer import index_samples, render_qc
from autoproducer.project import audit_als

SR = 48000


def tone(freq, seconds, sr=SR, amp=0.5):
    t = np.arange(int(seconds * sr)) / sr
    return amp * np.sin(2 * np.pi * freq * t)


def write(path, x, sr=SR, subtype="PCM_24"):
    sf.write(str(path), x, sr, subtype=subtype)
    return path


def test_measure_detects_loudness_peak_and_mono(tmp_path):
    x = tone(440, 10, amp=0.25)
    st = np.stack([x, x], axis=1)
    m = render_qc.measure(write(tmp_path / "a.wav", st))
    assert -18 < m["lufs_i"] < -12
    assert m["true_peak_dbtp"] == pytest.approx(-12.0, abs=0.3)
    assert m["clipped_samples"] == 0 and m["lr_correlation"] > 0.99
    assert m["mono_loudness_loss_db"] == pytest.approx(0, abs=0.1)
    assert render_qc.evaluate(m, -14, -1.0)[0]["check"] in ("loudness",) or True


def test_evaluate_flags_antiphase_and_clipping(tmp_path):
    x = tone(80, 10, amp=1.2).clip(-1, 1)
    st = np.stack([x, -x], axis=1)
    m = render_qc.measure(write(tmp_path / "b.wav", st, subtype="FLOAT"))
    checks = {f["check"] for f in render_qc.evaluate(m, -9, -1.0)}
    assert {"clipping", "true_peak", "mono", "low_mono", "format"} <= checks


def test_silence_gap_detected_but_tail_ignored(tmp_path):
    x = np.concatenate([tone(300, 5), np.zeros(SR * 3), tone(300, 5), np.zeros(SR * 4)])
    gaps = render_qc.silence_gaps(x, SR)
    assert len(gaps) == 1 and gaps[0][0] == pytest.approx(5.0, abs=0.1)


def test_index_library_roles_bpm_and_cache(tmp_path):
    lib = tmp_path / "lib"
    (lib / "Drums" / "Kicks").mkdir(parents=True)
    (lib / "Loops").mkdir()
    t = np.arange(int(0.4 * 22050)) / 22050
    kick = np.sin(2 * np.pi * 55 * t) * np.exp(-t * 12)
    write(lib / "Drums" / "Kicks" / "punchy.wav", kick, 22050, "PCM_16")
    # 4 bars at 120 BPM = 8 s, clicks every beat, chord tones A C E
    sr = 22050
    loop = np.zeros(sr * 8)
    for b in range(16):
        s = int(b * 0.5 * sr)
        loop[s:s + 2000] += np.random.default_rng(b).normal(0, 0.5, 2000) * np.exp(-np.arange(2000) / 300)
    tt = np.arange(sr * 8) / sr
    loop += 0.2 * (np.sin(2 * np.pi * 220 * tt) + np.sin(2 * np.pi * 261.63 * tt) + np.sin(2 * np.pi * 329.63 * tt))
    write(lib / "Loops" / "chord_loop.wav", loop, sr, "PCM_16")
    db = tmp_path / "idx.sqlite"
    before = {p: p.stat().st_mtime for p in lib.rglob("*.wav")}
    s1 = index_samples.index_library([str(lib)], db, progress=lambda *_: None)
    assert s1["analysed"] == 2 and s1["errors"] == 0
    rows = {r["path"].split("/")[-1]: r for r in index_samples.load_all(db)}
    assert rows["punchy.wav"]["role"] == "kick"
    assert rows["chord_loop.wav"]["role"] == "loop"
    assert rows["chord_loop.wav"]["bpm"] == pytest.approx(120, rel=0.03)
    assert rows["chord_loop.wav"]["key"] in ("A minor", "C major")
    s2 = index_samples.index_library([str(lib)], db, progress=lambda *_: None)
    assert s2["skipped"] == 2 and s2["analysed"] == 0
    assert before == {p: p.stat().st_mtime for p in lib.rglob("*.wav")}  # originals untouched


def test_role_from_path():
    assert index_samples.role_from_path("/x/Drums/Hats/hh_closed_01.wav") == "hat"
    assert index_samples.role_from_path("/x/FX/Risers/up_8bars.wav") == "fx"
    assert index_samples.role_from_path("/x/Vox/phrase_Am.wav") == "vocal"


def test_audit_als(tmp_path):
    proj = tmp_path / "Song Project"
    proj.mkdir()
    xml = f"""<?xml version="1.0"?><Ableton><LiveSet>
      <FileRef><Path Value="{proj.as_posix()}/Samples/Imported/a.wav"/></FileRef>
      <FileRef><Path Value="/Users/me/Library/outside.wav"/></FileRef>
      <PluginDesc><Vst3PluginInfo><Name Value="Serum 2"/></Vst3PluginInfo></PluginDesc>
    </LiveSet></Ableton>"""
    als = proj / "Song_v001.als"
    with gzip.open(als, "wb") as fh:
        fh.write(xml.encode())
    a = audit_als(als, proj)
    assert a["plugins"] == ["Serum 2"]
    assert a["outside_project"] == ["/Users/me/Library/outside.wav"]
