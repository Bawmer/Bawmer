"""Stage 8: deliverables + report.md (everything measured, nothing claimed without a number)."""
from __future__ import annotations

import json
import shutil
from pathlib import Path

from .actionlog import log
from .plan import describe
from .project import Project, audit_als


def deliver(proj: Project, plan: dict, mp3: Path | None) -> Path:
    qc = proj.state.get("qc", {})
    final = qc.get("final", {})
    out = proj.root / "Deliverables"
    out.mkdir(exist_ok=True)
    if final.get("master"):
        shutil.copy2(final["master"], out / f"{proj.cfg.song_name}_master.wav")
        if mp3:
            shutil.copy2(mp3, out / f"{proj.cfg.song_name}_preview.mp3")
        stems_dir = out / "Stems"
        stems_dir.mkdir(exist_ok=True)
        for s in final.get("stems", []):
            shutil.copy2(s, stems_dir / Path(s).name)
    rep = write_report(proj, plan)
    shutil.copy2(rep, out / "report.md")
    log("delivered", folder=str(out))
    return out


def write_report(proj: Project, plan: dict) -> Path:
    st = proj.state
    qc = st.get("qc", {})
    lines = [f"# {proj.cfg.song_name}: production report", ""]
    lines += [describe(plan).split("\n", 2)[2], ""]  # plan body without its title

    lines += ["## Samples used (source → project copy)", ""]
    for src, rel in sorted(st.get("copied", {}).items()):
        lines.append(f"- `{src}` → `{rel}`")
    if not st.get("copied"):
        lines.append("- none")

    lines += ["", "## Plugins and devices", "", "| Track | Device | Role |", "|---|---|---|"]
    for part, t in st.get("tracks", {}).items():
        if t.get("instrument"):
            lines.append(f"| {part} | {t['instrument']} ({t.get('instrument_class', '')}) | instrument, preset "
                         f"`{t.get('preset', '')}` |")
        for d in t.get("devices", [])[1 if t.get("instrument") else 0:]:
            lines.append(f"| {part} | {d} | utility / processing |")
    for k in ("glue", "eq", "utility", "limiter"):
        if k in st.get("master", {}):
            lines.append(f"| master | {k} | master chain |")

    lines += ["", "## Mix decisions", ""]
    for part, ds in st.get("mix", {}).items():
        lines.append(f"- **{part}:** " + "; ".join(ds))
    if st.get("faders"):
        lines.append("- **final faders (dB):** " + ", ".join(f"{k} {v:+.1f}" for k, v in st["faders"].items()))
    m = st.get("master", {})
    if m:
        lim = m.get("limiter", {})
        lines.append(f"- **master:** Glue −12 dB 2:1 → EQ HPF 25 Hz → Utility bass mono "
                     f"< {m.get('mono_below_hz')} Hz → Limiter gain {lim.get('gain_db')} dB, "
                     f"ceiling {lim.get('ceiling_db')} dB")

    lines += ["", "## QC", ""]
    if not qc:
        lines.append("Not rendered yet.")
    else:
        lines.append(f"**Result: {'PASS' if qc.get('passed') else 'FAIL'}** after "
                     f"{len(qc['iterations'])} render(s).")
        lines += ["", "| # | Method | LUFS-I | True peak | Clipped | Corr | Mono loss | Low side | Failures | Actions |",
                  "|---|---|---|---|---|---|---|---|---|---|"]
        for it in qc["iterations"]:
            mm = it["measurements"]
            lines.append(f"| {it['iteration']} | {it['method']} | {mm['lufs_i']} | {mm['true_peak_dbtp']} dBTP | "
                         f"{mm['clipped_samples']} | {mm['lr_correlation']} | {mm['mono_loudness_loss_db']} dB | "
                         f"{mm['low_side_ratio']} | {', '.join(f['check'] for f in it['failures']) or '—'} | "
                         f"{'; '.join(it['actions']) or '—'} |")
        fm = qc["final"]["measurements"]
        lines += ["", f"Targets: LUFS-I {proj.cfg.loudness_target_lufs} ± 1, true peak ≤ "
                      f"{proj.cfg.true_peak_max_dbtp} dBTP, 0 clipped samples, no silence gaps, mono loss ≤ 3 dB.",
                  f"Final file: `{fm['file']}`: {fm['sr']} Hz, {fm['subtype']}, {fm['duration']} s."]
        if qc["final"]["stems"]:
            lines.append(f"Stems: {len(qc['final']['stems'])} files.")
        else:
            lines.append("Stems: none (resampling fallback renders the master only).")

    als = proj.latest_als()
    if als and als.exists():
        a = audit_als(als, proj.root)
        lines += ["", "## Project audit", "", f"- Set: `{als.name}`",
                  f"- Referenced files: {len(a['files'])}, outside project: {len(a['outside_project'])}"]
        for f in a["outside_project"][:20]:
            lines.append(f"  - ⚠️ `{f}`")
        if proj.cfg.all_vst3:
            banned = {b.lower() for b in proj.cfg.banned_plugins}
            bad = [p for p in a["plugins"] if a["plugin_formats"].get(p) != "VST3" or p.lower() in banned]
        else:
            bad = [p for p in a["plugins"] if not proj.cfg.plugin_allowed(p)]
        lines.append(f"- Plugins in .als: {', '.join(a['plugins']) or 'none'}"
                     + (f". ⚠️ NOT WHITELISTED: {bad}" if bad else ". All whitelisted."))
    lines += ["", "## Versions", ""] + [f"- `{v['file']}`: {v['stage']}" for v in st.get("als", [])]
    lines += ["", f"Seed: {plan['seed']}. Approvals: {json.dumps(st.get('approvals', {}))}.",
              f"Action log: `autoproducer/actions.jsonl`."]
    path = proj.meta / "report.md"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path
