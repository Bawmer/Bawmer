"""Neural voice training (fine-tunes a base model on the user's dry singing)."""
from __future__ import annotations

import glob
import json
import os
import random
import time

import numpy as np

from . import protocol as P
from .audio import (CONTENT_SR, OUTPUT_HOP, OUTPUT_SR, extract_f0, load_mono, normalize, resample, split_on_silence)

SEGMENT_FRAMES = 36          # 0.72 s training crops
QUALITY = {"fast": dict(batch=8, lr=2e-4), "standard": dict(batch=8, lr=2e-4), "high": dict(batch=12, lr=1.5e-4)}


def _device(use_gpu: bool) -> str:
    import torch
    if use_gpu and torch.cuda.is_available():
        return "cuda"
    if use_gpu and getattr(torch.backends, "mps", None) and torch.backends.mps.is_available():
        return "mps"
    return "cpu"


def prepare_dataset(samples: list[str], cache_dir: str, device: str) -> list[str]:
    """Slices samples into phrases and caches (units, f0, wav48) per phrase."""
    import torch
    from .content import ContentEncoder

    os.makedirs(cache_dir, exist_ok=True)
    manifest = os.path.join(cache_dir, "manifest.json")
    key = sorted((os.path.basename(s), os.path.getsize(s)) for s in samples if os.path.exists(s))
    if os.path.exists(manifest):
        with open(manifest) as f:
            m = json.load(f)
        if m.get("key") == key and all(os.path.exists(p) for p in m["items"]):
            P.log(f"Using cached dataset ({len(m['items'])} phrases)")
            return m["items"]

    P.progress(0.01, "Loading content encoder (HuBERT)", force=True)
    enc = ContentEncoder(device)
    items, total_sec = [], 0.0
    for i, path in enumerate(samples):
        P.progress(0.02 + 0.08 * i / max(1, len(samples)), f"Preparing {os.path.basename(path)}")
        try:
            x, sr = load_mono(path)
        except Exception as e:  # noqa: BLE001
            P.log(f"Skipping {path}: {e}")
            continue
        x = normalize(x)
        for j, seg in enumerate(split_on_silence(x, sr)):
            x16 = resample(seg, sr, CONTENT_SR)
            x48 = resample(seg, sr, OUTPUT_SR)
            units = enc.units(x16)
            T = min(len(units), len(x48) // OUTPUT_HOP)
            if T < SEGMENT_FRAMES:
                continue
            f0 = extract_f0(x16, T)
            out = os.path.join(cache_dir, f"{i:03d}_{j:04d}.pt")
            torch.save({"units": torch.from_numpy(units[:T]).half(),
                        "f0": torch.from_numpy(f0[:T]),
                        "wav": torch.from_numpy(x48[:T * OUTPUT_HOP])}, out)
            items.append(out)
            total_sec += T / 50.0
    with open(manifest, "w") as f:
        json.dump({"key": key, "items": items, "seconds": total_sec}, f)
    P.log(f"Dataset: {len(items)} phrases, {total_sec / 60:.1f} min of singing")
    return items


def _batch(items, cache, batch, device):
    import torch
    us, fs, ws = [], [], []
    for path in random.choices(items, k=batch):
        d = cache.get(path)
        if d is None:
            d = torch.load(path)
            cache[path] = d
        T = d["units"].shape[0]
        s = random.randint(0, T - SEGMENT_FRAMES)
        us.append(d["units"][s:s + SEGMENT_FRAMES].float())
        f0 = d["f0"][s:s + SEGMENT_FRAMES]
        # light pitch augmentation keeps the model robust to new melodies (+-2 semitones)
        if random.random() < 0.3:
            f0 = f0 * (2 ** (random.uniform(-2, 2) / 12))
        fs.append(f0)
        ws.append(d["wav"][s * OUTPUT_HOP:(s + SEGMENT_FRAMES) * OUTPUT_HOP])
    return torch.stack(us).to(device), torch.stack(fs).to(device), torch.stack(ws).to(device)[:, None]


def export_voice_onnx(G, path: str, opset: int = 17):
    import copy
    import torch
    g = copy.deepcopy(G).cpu().eval()
    g.remove_weight_norm()
    units = torch.randn(1, 50, 768)
    f0 = torch.full((1, 50), 220.0)
    tmp = path + ".tmp"
    torch.onnx.export(g, (units, f0), tmp, input_names=["units", "f0"], output_names=["audio"],
                      dynamic_axes={"units": {1: "frames"}, "f0": {1: "frames"}, "audio": {1: "samples"}},
                      opset_version=opset, do_constant_folding=True)
    os.replace(tmp, path)
    try:  # verify with onnxruntime when available
        import onnxruntime as ort
        s = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
        y = s.run(None, {"units": np.random.randn(1, 30, 768).astype(np.float32),
                         "f0": np.full((1, 30), 200.0, np.float32)})[0]
        assert y.shape == (1, 30 * OUTPUT_HOP), y.shape
        P.log("voice.onnx verified with onnxruntime")
    except ImportError:
        P.log("onnxruntime not installed - skipped ONNX verification")


def train(args) -> dict:
    import torch
    from .model import Discriminator, Generator, MelLoss, discriminator_loss, feature_loss, generator_loss

    device = _device(args.gpu)
    P.log(f"Device: {device}" + ("" if device != "cpu" else "  (no GPU: training will be slow - use 'fast' quality)"))
    voice_dir = args.voice_dir
    models_dir = args.models_dir
    os.makedirs(voice_dir, exist_ok=True)
    os.makedirs(models_dir, exist_ok=True)

    samples = args.samples or sorted(glob.glob(os.path.join(voice_dir, "samples", "*")))
    if args.data_dir:
        for ext in ("wav", "flac", "aif", "aiff", "ogg", "mp3"):
            samples += glob.glob(os.path.join(args.data_dir, "**", f"*.{ext}"), recursive=True)
    if not samples:
        raise RuntimeError("No training samples given.")

    items = prepare_dataset(samples, os.path.join(voice_dir, ".cache"), device)
    if not items:
        raise RuntimeError("No usable phrases found in the samples (too short or silent?).")

    q = QUALITY.get(args.quality, QUALITY["standard"])
    G, D = Generator().to(device), Discriminator().to(device)
    optG = torch.optim.AdamW(G.parameters(), q["lr"], betas=(0.8, 0.99), eps=1e-9)
    optD = torch.optim.AdamW(D.parameters(), q["lr"], betas=(0.8, 0.99), eps=1e-9)
    mel_loss = MelLoss().to(device)

    # ---- initialise from a base model (strongly recommended) or resume -------------------
    ckpt_dir = os.path.join(voice_dir, "checkpoints")
    os.makedirs(ckpt_dir, exist_ok=True)
    resume = os.path.join(ckpt_dir, "latest.pt")
    base = args.base or os.path.join(models_dir, "base", "base.pt")
    start_epoch = 0
    if os.path.exists(resume) and not args.pretrain:
        ck = torch.load(resume, map_location=device)
        G.load_state_dict(ck["G"]); D.load_state_dict(ck["D"])
        start_epoch = ck.get("epoch", 0)
        P.log(f"Resuming from epoch {start_epoch}")
    elif os.path.exists(base):
        ck = torch.load(base, map_location=device)
        G.load_state_dict(ck["G"]); D.load_state_dict(ck["D"], strict=False)
        P.log("Fine-tuning from base model " + base)
    else:
        P.log("WARNING: no base model found (Models/base/base.pt). Training from scratch needs a lot more data "
              "and time; see the manual for building a base model.")

    steps_per_epoch = max(10, len(items) * 2 // q["batch"])
    epochs = int(args.epochs)
    total = max(1, (epochs - start_epoch) * steps_per_epoch)
    sched_g = torch.optim.lr_scheduler.ExponentialLR(optG, 0.999)
    sched_d = torch.optim.lr_scheduler.ExponentialLR(optD, 0.999)
    use_amp = device == "cuda"
    scaler = torch.cuda.amp.GradScaler(enabled=use_amp)
    cache: dict = {}
    t0, step = time.time(), 0

    G.train(); D.train()
    for epoch in range(start_epoch, epochs):
        for _ in range(steps_per_epoch):
            units, f0, wav = _batch(items, cache, q["batch"], device)
            with torch.autocast(device_type="cuda", enabled=use_amp):
                y_hat = G(units, f0)[:, None]
                r, g, _, _ = D(wav, y_hat.detach())
                loss_d = discriminator_loss(r, g)
            optD.zero_grad(set_to_none=True)
            scaler.scale(loss_d).backward()
            scaler.unscale_(optD)
            torch.nn.utils.clip_grad_norm_(D.parameters(), 1000)
            scaler.step(optD)

            with torch.autocast(device_type="cuda", enabled=use_amp):
                r, g, fr, fg = D(wav, y_hat)
                l_mel = mel_loss(wav[:, 0].float(), y_hat[:, 0].float()) * 45
                l_fm = feature_loss(fr, fg)
                l_adv = generator_loss(g)
                loss_g = l_mel + l_fm + l_adv
            optG.zero_grad(set_to_none=True)
            scaler.scale(loss_g).backward()
            scaler.unscale_(optG)
            torch.nn.utils.clip_grad_norm_(G.parameters(), 1000)
            scaler.step(optG)
            scaler.update()

            step += 1
            done = step / total
            eta = (time.time() - t0) / max(1e-6, done) * (1 - done)
            P.progress(0.1 + 0.85 * done, f"epoch {epoch + 1}/{epochs}  mel {l_mel.item() / 45:.3f}  ETA {eta / 60:.0f} min")
        sched_g.step(); sched_d.step()

        if (epoch + 1) % 10 == 0 or epoch + 1 == epochs:
            torch.save({"G": G.state_dict(), "D": D.state_dict(), "epoch": epoch + 1}, resume)
            P.log(f"epoch {epoch + 1}: mel {l_mel.item() / 45:.4f}, d {loss_d.item():.3f} (checkpoint saved)")

    # ---- export --------------------------------------------------------------------------------
    if args.pretrain:
        out = os.path.join(models_dir, "base", "base.pt")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        torch.save({"G": G.state_dict(), "D": D.state_dict()}, out)
        P.progress(1.0, "Base model saved", force=True)
        return {"base": out}

    P.progress(0.96, "Exporting voice.onnx", force=True)
    export_voice_onnx(G, os.path.join(voice_dir, "voice.onnx"))

    enc_path = os.path.join(models_dir, "content_encoder.onnx")
    if not os.path.exists(enc_path):
        P.progress(0.98, "Exporting content encoder", force=True)
        from .content import export_onnx
        export_onnx(enc_path)

    P.progress(1.0, "Done", force=True)
    return {"model": "voice.onnx", "sampleRate": OUTPUT_SR, "hop": OUTPUT_HOP, "contentEncoder": enc_path}
