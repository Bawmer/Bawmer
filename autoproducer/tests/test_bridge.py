import socket

import pytest

from autoproducer import config, step0
from autoproducer.live import Live, WhitelistViolation
from autoproducer.osc import LiveError, OSCClient
from tests.fake_live import FakeAbletonOSC


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


@pytest.fixture()
def rig(tmp_path):
    send, recv = free_port(), free_port()
    fake = FakeAbletonOSC(send, recv)
    lib = tmp_path / "lib"
    lib.mkdir()
    (lib / "kick.wav").write_bytes(b"RIFF")
    cfg = config.from_dict({
        "genre": "house", "sample_library": [str(lib)], "project_path": str(tmp_path),
        "plugin_whitelist": ["Good Synth"], "banned_plugins": ["Bad Synth"],
        "presets": {"bass": "Presets/Instruments/Bass.adv"},
        "osc": {"host": "127.0.0.1", "send_port": send, "recv_port": recv, "timeout": 2.0}}, need_live_assets=False)
    client = OSCClient("127.0.0.1", send, recv, 2.0)
    live = Live(cfg, client)
    yield fake, live, cfg
    client.close()
    fake.close()


def test_environment_calls(rig):
    fake, live, _ = rig
    assert live.test()
    assert live.version() == (12, 4, 3)
    assert live.ext_ok()
    assert live.plugins() == ["VST3/Vendor/Bad Synth", "VST3/Vendor/Good Synth"]
    assert live.user_presets() == ["Presets/Instruments/Bass.adv", "Presets/Instruments/Evil.adv"]


def test_whitelisted_preset_loads_and_is_verified(rig):
    fake, live, _ = rig
    t = live.create_track("midi", "Bass")
    dev = live.load_preset("track", t, "Presets/Instruments/Bass.adv", "Good Synth")
    assert dev.name == "Good Synth" and dev.is_plugin
    assert [d.name for d in fake.song.tracks[t].devices] == ["Good Synth"]


def test_non_whitelisted_plugin_is_removed_and_stops(rig):
    fake, live, _ = rig
    t = live.create_track("midi", "Lead")
    with pytest.raises(WhitelistViolation):
        live.load_preset("track", t, "Presets/Instruments/Evil.adv", None)
    assert fake.song.tracks[t].devices == []


def test_missing_preset_reports_error(rig):
    _, live, _ = rig
    t = live.create_track("midi", "X")
    with pytest.raises(LiveError, match="not found"):
        live.load_preset("track", t, "Presets/Nope.adv", None)


def test_midi_clip_notes_roundtrip_in_chunks(rig):
    fake, live, _ = rig
    t = live.create_track("midi", "Drums")
    notes = [(36, i * 0.25, 0.2, 100, 0) for i in range(300)]  # > one OSC message
    ci = live.midi_clip(t, 16.0, 75.0, notes, "drums · drop")
    clip = fake.song.tracks[t].arrangement_clips[ci]
    assert len(clip._notes) == 300 and clip.name == "drums · drop" and clip.start_time == 16.0


def test_volume_db_binary_search(rig):
    fake, live, _ = rig
    t = live.create_track("audio", "Loop")
    shown = live.set_volume_db("track", t, -12.0)
    assert shown == "-12.0 dB"


def test_stock_device_params_and_envelope(rig):
    fake, live, _ = rig
    t = live.create_track("audio", "Pad")
    eq = live.load_stock("track", t, "EQ Eight")
    params = live.params("track", t, eq.index)
    freq = live.find_param(params, "1 Frequency A")
    _, s = live.set_param_display("track", t, eq.index, freq["index"], 180)
    assert s in ("179 Hz", "180 Hz", "181 Hz")
    fake.song.tracks[t].create_audio_clip("/p/a.wav", 0.0)
    v = live.value_for_display("track", t, eq.index, freq["index"], 1000)
    n = live.envelope(t, 0, ("track", t, eq.index, freq["index"]), [(0.0, 1.0, v), (1.0, 1.0, v)])
    assert n == 2


def test_stock_device_refused_when_not_allowed(rig):
    fake, live, cfg = rig
    cfg.raw["allow_stock_utility"] = False
    t = live.create_track("audio", "Pad")
    with pytest.raises(WhitelistViolation):
        live.load_stock("track", t, "EQ Eight")


def test_step0_table_reports_missing_plugin(rig):
    fake, live, cfg = rig
    cfg.raw["plugin_whitelist"] = ["Good Synth", "Missing Synth"]
    rows = step0.run(cfg, lambda: live)
    by = {r["check"]: r for r in rows}
    assert by["/live/test"]["status"] == "PASS"
    assert by["VST3 scanned: Good Synth"]["status"] == "PASS"
    assert by["VST3 scanned: Missing Synth"]["status"] == "FAIL"
    assert not step0.ready(rows)
    assert "Missing Synth" in step0.table(rows)


def test_install_ext_is_idempotent(tmp_path):
    from autoproducer.install_ext import install
    root = tmp_path / "AbletonOSC"
    (root / "abletonosc").mkdir(parents=True)
    (root / "abletonosc" / "__init__.py").write_text("from .song import SongHandler\n")
    (root / "manager.py").write_text("handlers = [\n                abletonosc.MidiMapHandler(self),\n            ]\n")
    install(root)
    install(root)
    m = (root / "manager.py").read_text()
    assert m.count("AutoProducerHandler") == 1
    assert (root / "abletonosc" / "__init__.py").read_text().count("AutoProducerHandler") == 1
    assert (root / "abletonosc" / "autoproducer.py").exists()
