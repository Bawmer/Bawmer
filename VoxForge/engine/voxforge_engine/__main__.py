"""VoxForge Engine - local, offline neural voice training and lyrics transcription.

Usage (normally launched by the plug-in):
    voxforge-engine check [--models-dir DIR]
    voxforge-engine train --voice-dir DIR --models-dir DIR [--samples a.wav b.wav ...] [--epochs N]
                          [--quality fast|standard|high] [--gpu] [--base base.pt]
    voxforge-engine train --pretrain --data-dir CORPUS --voice-dir WORKDIR --models-dir DIR --epochs N --gpu
    voxforge-engine transcribe --audio vocal.wav [--language en|auto] [--model small] [--prompt "lyrics hint"] [--gpu]
    voxforge-engine import-model --model my_voice.pth --out DIR/voice.onnx   (Applio / RVC v2 voice)
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


def _enc_state(enc: str) -> str:
    if not enc or not os.path.exists(enc):
        return "no"
    from .content import is_current
    return "yes" if is_current(enc) else "OUTDATED (run export-content-encoder)"


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
               f"content encoder: {_enc_state(enc)}, "
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

    im = sub.add_parser("import-model", help="convert an Applio / RVC v2 voice model (.pth) to voice.onnx")
    im.add_argument("--model", required=True)
    im.add_argument("--out", required=True, help="output voice.onnx path")

    b = sub.add_parser("download-base")
    b.add_argument("--models-dir", required=True)

    r = sub.add_parser("transcribe")
    r.add_argument("--audio", required=True)
    r.add_argument("--language", default="auto")
    r.add_argument("--model", default="small")
    r.add_argument("--models-dir", default="")
    r.add_argument("--gpu", action="store_true")
    r.add_argument("--prompt", default="", help="known lyrics / vocabulary hint for the recogniser")

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
        elif args.cmd == "import-model":
            from .model import import_rvc_model
            P.progress(0.1, "Reading model", force=True)
            info = import_rvc_model(args.model, args.out)
            from .train import _verify_onnx
            _verify_onnx(args.out, hop=info["hop"])
            P.progress(1.0, "Imported", force=True)
            P.result(model=os.path.basename(args.out), **info)
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
