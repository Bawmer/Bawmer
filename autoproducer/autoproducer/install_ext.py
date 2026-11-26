"""Install the AutoProducer handler into an existing AbletonOSC remote script folder.

Copies ext/autoproducer_handler.py -> AbletonOSC/abletonosc/autoproducer.py and adds one
import line + one handler line. Backups (*.bak) are written before any edit; re-running
is a no-op. Restart Live (or send /live/api/reload) afterwards.
"""
from __future__ import annotations

import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent / "ext" / "autoproducer_handler.py"
IMPORT_LINE = "from .autoproducer import AutoProducerHandler\n"
HANDLER_LINE = "                abletonosc.AutoProducerHandler(self),\n"
ANCHOR = "abletonosc.MidiMapHandler(self),"


def install(abletonosc_dir: str | Path) -> list[str]:
    root = Path(abletonosc_dir)
    pkg, manager, init = root / "abletonosc", root / "manager.py", root / "abletonosc" / "__init__.py"
    if not (pkg.is_dir() and manager.exists() and init.exists()):
        raise FileNotFoundError(f"{root} does not look like an AbletonOSC folder (need manager.py, abletonosc/)")
    done = []
    shutil.copy2(HERE, pkg / "autoproducer.py")
    done.append(f"copied handler -> {pkg / 'autoproducer.py'}")
    text = init.read_text(encoding="utf-8")
    if IMPORT_LINE.strip() not in text:
        shutil.copy2(init, init.with_suffix(".py.bak"))
        init.write_text(text.rstrip("\n") + "\n" + IMPORT_LINE, encoding="utf-8")
        done.append("registered import in abletonosc/__init__.py")
    m = manager.read_text(encoding="utf-8")
    if "AutoProducerHandler" not in m:
        if ANCHOR not in m:
            raise RuntimeError("manager.py layout not recognised; add "
                               "'abletonosc.AutoProducerHandler(self),' to the handlers list by hand")
        shutil.copy2(manager, manager.with_suffix(".py.bak"))
        i = m.index(ANCHOR) + len(ANCHOR)
        nl = m.index("\n", i) + 1
        m = m[:nl] + HANDLER_LINE + m[nl:]
        manager.write_text(m, encoding="utf-8")
        done.append("registered handler in manager.py")
    return done
