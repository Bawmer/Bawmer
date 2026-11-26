"""Command line: one command per stage, two human checkpoints.

  autoproducer install-ext --abletonosc <path to AbletonOSC remote script>
  autoproducer step0                 readiness table
  autoproducer index                 scan the sample library -> SQLite
  autoproducer init                  save the open empty set as a new project (v000)
  autoproducer plan                  key/BPM/structure/shortlist -> plan.md
  autoproducer sketch                tracks + sounds + 8-bar loop, save v001  -> CHECKPOINT A
  autoproducer approve A             (after listening)
  autoproducer produce               compose, arrange, mix, master, render + QC loop -> CHECKPOINT B
  autoproducer approve B             -> deliver: master, stems, MP3, report.md
"""
from __future__ import annotations

import argparse
import sys
import traceback
from pathlib import Path

from . import actionlog, build, index_samples, mix, plan as planmod, render_qc, report, step0
from .config import ConfigError, load
from .project import Project


def _cfg(args, need_live_assets=True, strict=True):
    return load(args.config, need_live_assets=need_live_assets, strict=strict)


def _live(cfg):
    from .live import Live
    return Live(cfg)


def _index_db(cfg) -> Path:
    return Path(cfg.project_path) / ".autoproducer_index.sqlite"


def _project(cfg) -> Project:
    p = Project.locate(cfg)
    if p is None:
        raise SystemExit("no project yet: open a new empty Live set and run `autoproducer init`")
    actionlog.open_log(p.meta / "actions.jsonl")
    return p


def cmd_install_ext(args):
    from .install_ext import install
    for line in install(args.abletonosc):
        print(" -", line)
    print("Done. Restart Live (AbletonOSC must be selected under Preferences > Link, Tempo & MIDI).")


def cmd_step0(args):
    cfg = _cfg(args, strict=False)
    actionlog.open_log(Path(cfg.project_path or ".") / ".autoproducer_step0.jsonl"
                       if cfg.project_path and "TODO" not in str(cfg.project_path) else "step0.jsonl")
    rows = step0.run(cfg, lambda: _live(cfg))
    print(step0.table(rows))
    print("\nREADY" if step0.ready(rows) else "\nNOT READY: fix every ❌ row, then re-run step0.")
    return 0 if step0.ready(rows) else 2


def cmd_index(args):
    cfg = _cfg(args, need_live_assets=False)
    actionlog.open_log(Path(cfg.project_path) / ".autoproducer_index.jsonl")
    stats = index_samples.index_library(cfg.sample_library, _index_db(cfg))
    print(stats)


def cmd_init(args):
    cfg = _cfg(args)
    from . import gui
    base = Path(cfg.project_path)
    if (base / f"{cfg.song_name} Project").exists():
        raise SystemExit(f"{base / (cfg.song_name + ' Project')} already exists: pick a new song_name "
                         "(projects are never overwritten)")
    live = _live(cfg)
    info = live.song_info()
    if info["tracks"] > 2 or info["song_length"] > 32:
        raise SystemExit("the open set is not empty; File > New Live Set first")
    als = gui.save_as(base, cfg.song_name, cfg.gui["step_delay"])
    proj = Project(cfg, als.parent)
    actionlog.open_log(proj.meta / "actions.jsonl")
    proj.state["als"].append({"file": als.name, "stage": "init (empty set)"})
    proj.save_state()
    print(f"project created: {proj.root}")


def cmd_plan(args):
    cfg = _cfg(args)
    proj = _project(cfg)
    samples = index_samples.load_all(_index_db(cfg))
    if not samples:
        raise SystemExit("sample index is empty: run `autoproducer index` first")
    p = planmod.make_plan(cfg, samples)
    planmod.save(p, proj.plan_path)
    text = planmod.describe(p)
    (proj.meta / "plan.md").write_text(text, encoding="utf-8")
    proj.state["stages"]["plan"] = "done"
    proj.save_state()
    print(text)


def cmd_sketch(args):
    cfg = _cfg(args)
    proj = _project(cfg)
    p = planmod.load(proj.plan_path)
    live = _live(cfg)
    result = build.build_sketch(live, proj, p)
    als = proj.save_version("checkpoint A sketch")
    proj.state["stages"]["sketch"] = "done"
    proj.save_state()
    print(planmod.describe(p))
    print(f"\nSketch built: {sum(result['notes'].values())} MIDI notes, audio clips {result['audio_clips']}.")
    print(f"Saved {als.name}. The 8-bar loop is under the loop brace: press play in Live.")
    print("\nCHECKPOINT A: approve with `autoproducer approve A`, or edit config/plan and re-run plan+sketch.")


def cmd_approve(args):
    cfg = _cfg(args)
    proj = _project(cfg)
    cp = args.checkpoint.upper()
    if cp == "B" and not proj.state.get("qc"):
        raise SystemExit("nothing to approve: run `autoproducer produce` first")
    if cp == "B" and not proj.state["qc"]["passed"]:
        if not args.force:
            raise SystemExit("QC did not pass. Approving anyway needs --force (the report will say so).")
    proj.approve(cp, args.note or "")
    if cp == "B":
        final = Path(proj.state["qc"]["final"]["master"])
        mp3 = render_qc.mp3_preview(final) if cfg.export["mp3_preview"] else None
        out = report.deliver(proj, planmod.load(proj.plan_path), mp3)
        print(f"Delivered to {out}")
    else:
        print("Checkpoint A approved. Run `autoproducer produce`.")


def cmd_produce(args):
    cfg = _cfg(args)
    proj = _project(cfg)
    if not proj.approved("A"):
        raise SystemExit("checkpoint A not approved: listen to the sketch, then `autoproducer approve A`")
    p = planmod.load(proj.plan_path)
    live = _live(cfg)
    stages = [
        ("arrange", lambda: build.build_full(live, proj, p)),
        ("mix", lambda: mix.mix_tracks(live, proj)),
        ("master", lambda: mix.master_chain(live, proj)),
    ]
    for name, fn in stages:
        if proj.state["stages"].get(name) == "done" and not args.redo:
            print(f"- {name}: already done (use --redo to rebuild)")
            continue
        proj.save_version(f"before {name}")
        print(f"- {name} ...")
        out = fn()
        proj.state["stages"][name] = "done"
        proj.save_state()
        actionlog.log("stage_done", stage=name, result=out)
    violations = live.audit_all_devices()
    if violations:
        raise SystemExit("non-whitelisted plugins present: " + "; ".join(violations))
    proj.save_version("before render")
    print("- render + QC ...")
    qc = render_qc.render_and_qc(live, proj, p)
    proj.save_version("after QC")
    rep = report.write_report(proj, p)
    fm = qc["final"]["measurements"]
    print(f"\nQC {'PASSED' if qc['passed'] else 'FAILED'} after {len(qc['iterations'])} render(s): "
          f"LUFS-I {fm['lufs_i']}, TP {fm['true_peak_dbtp']} dBTP, clipped {fm['clipped_samples']}, "
          f"mono loss {fm['mono_loudness_loss_db']} dB")
    if not qc["passed"]:
        print("Remaining failures: " + ", ".join(f"{f['check']}={f['value']}" for f in qc["final"]["failures"]))
    print(f"Master: {qc['final']['master']}\nReport: {rep}")
    print("\nCHECKPOINT B: listen, then `autoproducer approve B` to deliver.")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="autoproducer")
    ap.add_argument("-c", "--config", default="config.yaml")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("install-ext")
    s.add_argument("--abletonosc", required=True)
    s.set_defaults(fn=cmd_install_ext)
    for name, fn in (("step0", cmd_step0), ("index", cmd_index), ("init", cmd_init), ("plan", cmd_plan),
                     ("sketch", cmd_sketch)):
        sub.add_parser(name).set_defaults(fn=fn)
    s = sub.add_parser("produce")
    s.add_argument("--redo", action="store_true")
    s.set_defaults(fn=cmd_produce)
    s = sub.add_parser("approve")
    s.add_argument("checkpoint", choices=["A", "B", "a", "b"])
    s.add_argument("--note")
    s.add_argument("--force", action="store_true")
    s.set_defaults(fn=cmd_approve)
    args = ap.parse_args(argv)
    try:
        return args.fn(args) or 0
    except ConfigError as e:
        print(e, file=sys.stderr)
        return 2
    except SystemExit:
        raise
    except Exception as e:
        actionlog.log("failure", command=args.cmd, error=f"{type(e).__name__}: {e}",
                      trace=traceback.format_exc())
        print(f"FAILED in `{args.cmd}`: {type(e).__name__}: {e}\n(full trace in the action log)", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
