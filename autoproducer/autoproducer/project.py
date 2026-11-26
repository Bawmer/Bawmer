"""Project folder, run state, versioned saves, sample copy-in and .als audits.

Layout (inside the folder Live creates on first save):
  <song> Project/
    <song>_v001.als, <song>_v002.als, ...
    Samples/Imported/AutoProducer/<role>/<file>   copies of library files (originals untouched)
    autoproducer/state.json    run state, track map, checkpoint approvals
    autoproducer/actions.jsonl full action log
    autoproducer/plan.json, plan.md, renders/, report.md
"""
from __future__ import annotations

import gzip
import hashlib
import json
import shutil
import xml.etree.ElementTree as ET
from pathlib import Path

from . import gui
from .actionlog import log
from .config import Config


class Project:
    def __init__(self, cfg: Config, root: Path):
        self.cfg = cfg
        self.root = Path(root)
        self.meta = self.root / "autoproducer"
        self.meta.mkdir(parents=True, exist_ok=True)
        self.state_path = self.meta / "state.json"
        self.state = json.loads(self.state_path.read_text()) if self.state_path.exists() else {
            "version": 0, "stages": {}, "approvals": {}, "tracks": {}, "copied": {}, "als": []}

    # -- discovery ---------------------------------------------------------------
    @staticmethod
    def locate(cfg: Config) -> "Project | None":
        base = Path(cfg.project_path)
        for cand in (base / f"{cfg.song_name} Project", base):
            if (cand / "autoproducer" / "state.json").exists():
                return Project(cfg, cand)
        return None

    def save_state(self) -> None:
        self.state_path.write_text(json.dumps(self.state, indent=2))

    @property
    def plan_path(self) -> Path:
        return self.meta / "plan.json"

    @property
    def renders(self) -> Path:
        p = self.meta / "renders"
        p.mkdir(exist_ok=True)
        return p

    # -- versioned saves (hard rule 3) ----------------------------------------------
    def save_version(self, stage: str) -> Path:
        self.state["version"] += 1
        name = f"{self.cfg.song_name}_v{self.state['version']:03d}"
        als = gui.save_as(self.root, name, self.cfg.gui["step_delay"])
        self.state["als"].append({"file": als.name, "stage": stage})
        self.save_state()
        log("save_version", file=str(als), stage=stage)
        return als

    def latest_als(self) -> Path | None:
        if not self.state["als"]:
            return None
        return self.root / self.state["als"][-1]["file"]

    # -- sample copy-in (never modifies originals) -----------------------------------
    def import_sample(self, src: str, role: str) -> Path:
        src_p = Path(src)
        if str(src_p) in self.state["copied"]:
            return self.root / self.state["copied"][str(src_p)]
        dest_dir = self.root / "Samples" / "Imported" / "AutoProducer" / role
        dest_dir.mkdir(parents=True, exist_ok=True)
        dest = dest_dir / src_p.name
        if dest.exists() and _sha1(dest) != _sha1(src_p):
            dest = dest_dir / f"{src_p.stem}_{_sha1(src_p)[:8]}{src_p.suffix}"
        if not dest.exists():
            shutil.copy2(src_p, dest)  # read-only access to the source
        rel = dest.relative_to(self.root).as_posix()
        self.state["copied"][str(src_p)] = rel
        self.save_state()
        log("sample_copied", src=str(src_p), dest=rel, role=role)
        return dest

    # -- checkpoints (hard rule 5) ------------------------------------------------------
    def approve(self, checkpoint: str, note: str = "") -> None:
        import time
        self.state["approvals"][checkpoint] = {"at": time.time(), "note": note}
        self.save_state()
        log("checkpoint_approved", checkpoint=checkpoint, note=note)

    def approved(self, checkpoint: str) -> bool:
        return checkpoint in self.state["approvals"]


def _sha1(p: Path) -> str:
    h = hashlib.sha1()
    with p.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def audit_als(als: Path, project_root: Path) -> dict:
    """Read a saved .als (gzipped XML) and list every referenced file and plugin.
    Used to prove the project is self-contained and only whitelisted plugins are used."""
    with gzip.open(als, "rb") as fh:
        tree = ET.parse(fh)
    root = tree.getroot()
    files, plugins = set(), set()
    for ref in root.iter("FileRef"):
        p = ref.find("Path")
        if p is not None and p.get("Value"):
            files.add(p.get("Value"))
    for tag, name_tag in (("Vst3PluginInfo", "Name"), ("VstPluginInfo", "PlugName"), ("AuPluginInfo", "Name")):
        for info in root.iter(tag):
            n = info.find(name_tag)
            if n is not None and n.get("Value"):
                plugins.add(n.get("Value"))
    pr = str(project_root.resolve()).replace("\\", "/").lower()
    outside = sorted(f for f in files if not f.replace("\\", "/").lower().startswith(pr)
                     and not f.lower().endswith((".adv", ".adg", ".asd")))
    return {"files": sorted(files), "outside_project": outside, "plugins": sorted(plugins)}
