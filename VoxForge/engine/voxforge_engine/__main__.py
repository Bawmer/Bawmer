"""VoxForge Engine - local, offline neural voice training and lyrics transcription.

Usage (normally launched by the plug-in):
    voxforge-engine check [--models-dir DIR]
    voxforge-engine train --voice-dir DIR --models-dir DIR [--samples a.wav b.wav ...] [--epochs N]
                          [--quality fast|standard|high] [--gpu] [--base base.pt]
    voxforge-engine train --pretrain --data-dir CORPUS --voice-dir WORKDIR --models-dir DIR --epochs N --gpu
    voxforge-engine transcribe --audio vocal.wav [--language en|auto] [--model small] [--gpu] [--models-dir DIR]
    voxforge-engine download-base --models-dir DIR          (pretrained RVC v2 48 kHz base, ~220 MB)
    voxforge-engine export-content-encoder --models-dir DIR
"""
from __future__ import annotations

import argparse
import importlib
import os
import platform
import sys
import traceback

from . import __version__
from . import protocol as P


def _check(args) -> dict:
    info, missing = {}, []
    for mod in ("numpy", "torch", "torchaudio", "soundfile", "pyworld", "onnx", "onnxruntime", "faster_whisper"):
        try:
            m = importlib.import_module(mod)
            info[mod] = getattr(m, "__version__", "ok")
        except Exception:  # noqa: BLE001
            info[mod] = None
            missing.append(mod)
    cuda = False
    if info.get("torch"):
        import torch
        cuda = torch.cuda.is_available()
        info["cuda_device"] = torch.cuda.get_device_name(0) if cuda else None
    enc = os.path.join(args.models_dir, "content_encoder.onnx") if args.models_dir else ""
    base_dir = os.path.join(args.models_dir, "base") if args.models_dir else ""
    has_base = bool(base_dir) and (os.path.exists(os.path.join(base_dir, "base.pt"))
                                   or os.path.exists(os.path.join(base_dir, "f0G48k.pth")))
    summary = (f"VoxForge Engine {__version__}, Python {platform.python_version()}, "
               f"GPU: {info.get('cuda_device') or 'none'}, "
               f"content encoder: {'yes' if enc and os.path.exists(enc) else 'no'}, "
               f"base model: {'yes' if has_base else 'no (downloaded on first training)'}"
               + (f", missing: {', '.join(missing)}" if missing else ""))
    return {"summary": summary, "modules": info, "cuda": cuda, "missing": missing}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="voxforge-engine", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("check")
    c.add_argument("--models-dir", default="")

    t = sub.add_parser("train")
    t.add_argument("--name", default="voice")
    t.add_argument("--voice-dir", required=True)
    t.add_argument("--models-dir", required=True)
    t.add_argument("--samples", nargs="*", default=[])
    t.add_argument("--data-dir", default="")
    t.add_argument("--epochs", type=int, default=200)
    t.add_argument("--quality", default="standard", choices=["fast", "standard", "high"])
    t.add_argument("--base", default="")
    t.add_argument("--gpu", action="store_true")
    t.add_argument("--pretrain", action="store_true", help="train a base model on a multi-singer corpus")
    t.add_argument("--no-download", action="store_true", help="do not download the pretrained base automatically")
    t.add_argument("--max-steps-per-epoch", type=int, default=0, help=argparse.SUPPRESS)

    b = sub.add_parser("download-base")
    b.add_argument("--models-dir", required=True)

    r = sub.add_parser("transcribe")
    r.add_argument("--audio", required=True)
    r.add_argument("--language", default="auto")
    r.add_argument("--model", default="small")
    r.add_argument("--models-dir", default="")
    r.add_argument("--gpu", action="store_true")

    e = sub.add_parser("export-content-encoder")
    e.add_argument("--models-dir", required=True)

    args = ap.parse_args(argv)
    try:
        if args.cmd == "check":
            P.result(**_check(args))
        elif args.cmd == "train":
            from .train import train
            P.result(**train(args))
        elif args.cmd == "transcribe":
            from .transcribe import transcribe
            P.result(**transcribe(args))
        elif args.cmd == "download-base":
            from .model import download_base
            files = download_base(args.models_dir, lambda p, m: P.progress(p, m))
            P.result(files=files, summary="RVC v2 48 kHz pretrained base installed")
        elif args.cmd == "export-content-encoder":
            from .content import export_onnx
            P.progress(0.1, "Exporting HuBERT content encoder", force=True)
            path = export_onnx(os.path.join(args.models_dir, "content_encoder.onnx"))
            P.result(path=path)
        return 0
    except KeyboardInterrupt:
        P.error("Cancelled")
        return 130
    except Exception as ex:  # noqa: BLE001
        for line in traceback.format_exc().splitlines():
            P.log(line)
        P.error(f"{type(ex).__name__}: {ex}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
