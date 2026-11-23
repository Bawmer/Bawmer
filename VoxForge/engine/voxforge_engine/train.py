"""Neural voice training: fine-tunes the pretrained RVC v2 48 kHz base model on the user's dry singing."""
from __future__ import annotations

import glob
import json
import os
import random
import time

import numpy as np

from . import protocol as P
from .audio import CONTENT_SR, extract_f0, load_mono, normalize, resample, split_on_silence

CROP = 96                      # training crop in 100 Hz frames (0.96 s), must be >= 36 (model slice)
QUALITY = {"fast": dict(batch=8), "standard": dict(batch=8), "high": dict(batch=12)}
CACHE_VERSION = 2              # bump when the cached features change (v2: HuBERT layer 12, f0 @ 100 fps)


def _device(use_gpu: bool) -> str:
    import torch
    if use_gpu and torch.cuda.is_available():
        return "cuda"
    return "cpu"


def prepare_dataset(samples: list[str], cache_dir: str, device: str) -> list[str]:
    """Slices samples into phrases and caches units (50 fps), f0 (100 fps) and 48 kHz audio per phrase."""
    import torch
    from .content import ContentEncoder
    from .model import HOP, SR

    os.makedirs(cache_dir, exist_ok=True)
    manifest = os.path.join(cache_dir, "manifest.json")
    key = [CACHE_VERSION] + sorted((os.path.basename(s), os.path.getsize(s)) for s in samples if os.path.exists(s))
    key = json.loads(json.dumps(key))
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
            x48 = resample(seg, sr, SR)
            units = enc.units(x16)                                   # [T50, 768]
            T = min(2 * len(units), len(x48) // HOP)                 # 100 Hz frames
            if T < CROP:
                continue
            f0 = extract_f0(x16, T, frames_per_second=100)
            out = os.path.join(cache_dir, f"{i:03d}_{j:04d}.pt")
            torch.save({"units": torch.from_numpy(units[: (T + 1) // 2]).half(),
                        "f0": torch.from_numpy(f0[:T]),
                        "wav": torch.from_numpy(x48[: T * HOP])}, out)
            items.append(out)
            total_sec += T / 100.0
    with open(manifest, "w") as f:
        json.dump({"key": key, "items": items, "seconds": total_sec}, f)
    P.log(f"Dataset: {len(items)} phrases, {total_sec / 60:.1f} min of singing")
    return items


def _batch(items, cache, batch, device):
    """Random equal-length crops: units x2 to 100 fps, f0, wav, linear spectrogram."""
    import torch
    import torch.nn.functional as F
    from .model import HOP, f0_to_coarse, spectrogram

    us, fs, ws = [], [], []
    for path in random.choices(items, k=batch):
        d = cache.get(path)
        if d is None:
            d = torch.load(path, weights_only=True)
            cache[path] = d
        T = d["f0"].shape[0]
        s = random.randint(0, (T - CROP) // 2) * 2                    # even start: aligned with 50 fps units
        u = d["units"][s // 2: s // 2 + CROP // 2].float()
        u = F.interpolate(u.T[None], scale_factor=2.0, mode="nearest")[0].T[:CROP]
        f0 = d["f0"][s: s + CROP].clone()
        if random.random() < 0.3:                                     # +-2 semitone pitch augmentation
            f0 = f0 * (2 ** (random.uniform(-2, 2) / 12))
        us.append(u); fs.append(f0)
        ws.append(d["wav"][s * HOP:(s + CROP) * HOP])
    units = torch.stack(us).to(device)
    pitchf = torch.stack(fs).to(device)
    wav = torch.stack(ws).to(device)
    spec = spectrogram(wav)
    lengths = torch.full((batch,), CROP, dtype=torch.long, device=device)
    return units, lengths, f0_to_coarse(pitchf), pitchf, spec, wav


def train(args) -> dict:
    import torch
    from . import model as M
    from .rvc import commons

    device = _device(args.gpu)
    P.log(f"Device: {device}" + ("" if device != "cpu" else "  (no GPU: training will be slow - use 'fast' quality)"))
    voice_dir, models_dir = args.voice_dir, args.models_dir
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

    G, D = M.build_generator().to(device), M.build_discriminator().to(device)

    # ---- initialise: resume > explicit/own base > official RVC v2 base (downloaded on demand) ----
    ckpt_dir = os.path.join(voice_dir, "checkpoints")
    os.makedirs(ckpt_dir, exist_ok=True)
    resume = os.path.join(ckpt_dir, "latest.pt")
    start_epoch = 0
    if os.path.exists(resume) and not args.pretrain:
        ck = torch.load(resume, map_location=device, weights_only=True)
        if ck.get("arch") == "rvc-v2-48k":
            G.load_state_dict(ck["G"]); D.load_state_dict(ck["D"])
            start_epoch = ck.get("epoch", 0)
            P.log(f"Resuming from epoch {start_epoch}")
        else:
            P.log("Ignoring checkpoint from an older VoxForge model version")
    if start_epoch == 0:
        src = M.load_base(G, D, models_dir, args.base)
        if not src and not args.no_download:
            P.progress(0.1, "Downloading the pretrained base model (once, ~220 MB)", force=True)
            M.download_base(models_dir, lambda p, m: P.progress(0.1 + 0.02 * p, m))
            src = M.load_base(G, D, models_dir, args.base)
        if src:
            P.log("Fine-tuning from base model: " + src)
        else:
            P.log("WARNING: no base model - training from scratch needs hours of singing.")
    G.to(device); D.to(device)

    lr = 1e-4
    optG = torch.optim.AdamW(G.parameters(), lr, betas=(0.8, 0.99), eps=1e-9)
    optD = torch.optim.AdamW(D.parameters(), lr, betas=(0.8, 0.99), eps=1e-9)
    sched_g = torch.optim.lr_scheduler.ExponentialLR(optG, 0.999875)
    sched_d = torch.optim.lr_scheduler.ExponentialLR(optD, 0.999875)

    q = QUALITY.get(args.quality, QUALITY["standard"])
    batch = q["batch"]
    steps_per_epoch = max(4, len(items) // batch)
    if args.max_steps_per_epoch:
        steps_per_epoch = min(steps_per_epoch, args.max_steps_per_epoch)
    epochs = int(args.epochs)
    total = max(1, (epochs - start_epoch) * steps_per_epoch)
    use_amp = device == "cuda"
    scaler = torch.cuda.amp.GradScaler(enabled=use_amp)
    cache: dict = {}
    t0, step = time.time(), 0
    seg_frames = M.SEGMENT // M.HOP
    loss_mel = loss_d = torch.zeros(())

    G.train(); D.train()
    for epoch in range(start_epoch, epochs):
        for _ in range(steps_per_epoch):
            units, lengths, pitch, pitchf, spec, wav = _batch(items, cache, batch, device)
            sid = torch.zeros(batch, dtype=torch.long, device=device)
            with torch.autocast(device_type="cuda", enabled=use_amp):
                y_hat, ids_slice, x_mask, z_mask, (z, z_p, m_p, logs_p, m_q, logs_q) = G(
                    units, lengths, pitch, pitchf, spec, lengths, sid)
                y = commons.slice_segments(wav[:, None], ids_slice * M.HOP, M.SEGMENT)
                y_d_r, y_d_g, _, _ = D(y, y_hat.detach())
                loss_d = M.discriminator_loss(y_d_r, y_d_g)
            optD.zero_grad(set_to_none=True)
            scaler.scale(loss_d).backward()
            scaler.unscale_(optD)
            torch.nn.utils.clip_grad_norm_(D.parameters(), 1000)
            scaler.step(optD)

            with torch.autocast(device_type="cuda", enabled=use_amp):
                y_d_r, y_d_g, fmap_r, fmap_g = D(y, y_hat)
            y_mel = commons.slice_segments(M.spec_to_mel(spec.float()), ids_slice, seg_frames)
            y_hat_mel = M.spec_to_mel(M.spectrogram(y_hat[:, 0].float()))
            loss_mel = torch.nn.functional.l1_loss(y_mel, y_hat_mel) * 45
            loss_kl = M.kl_loss(z_p.float(), logs_q.float(), m_p.float(), logs_p.float(), z_mask.float())
            loss_g = loss_mel + loss_kl + M.feature_loss(fmap_r, fmap_g) + M.generator_loss(y_d_g)
            optG.zero_grad(set_to_none=True)
            scaler.scale(loss_g).backward()
            scaler.unscale_(optG)
            torch.nn.utils.clip_grad_norm_(G.parameters(), 1000)
            scaler.step(optG)
            scaler.update()

            step += 1
            done = step / total
            eta = (time.time() - t0) / max(1e-6, done) * (1 - done)
            P.progress(0.12 + 0.82 * done, f"epoch {epoch + 1}/{epochs}  mel {loss_mel.item() / 45:.3f}  ETA {eta / 60:.0f} min")
        sched_g.step(); sched_d.step()

        if (epoch + 1) % 10 == 0 or epoch + 1 == epochs:
            torch.save({"arch": "rvc-v2-48k", "G": G.state_dict(), "D": D.state_dict(), "epoch": epoch + 1}, resume)
            P.log(f"epoch {epoch + 1}: mel {loss_mel.item() / 45:.4f}, d {loss_d.item():.3f} (checkpoint saved)")

    if args.pretrain:
        _, own = M.base_paths(models_dir)
        os.makedirs(os.path.dirname(own), exist_ok=True)
        torch.save({"arch": "rvc-v2-48k", "G": G.state_dict(), "D": D.state_dict()}, own)
        P.progress(1.0, "Base model saved", force=True)
        return {"base": own}

    P.progress(0.95, "Exporting voice.onnx", force=True)
    G_cpu = {k: v.detach().cpu() for k, v in G.state_dict().items()}
    out = os.path.join(voice_dir, "voice.onnx")
    M.export_voice_onnx(G_cpu, out)
    _verify_onnx(out)

    enc_path = os.path.join(models_dir, "content_encoder.onnx")
    from .content import is_current
    if not is_current(enc_path):          # missing, or an older (layer 9) export -> rebuild
        P.progress(0.98, "Exporting content encoder", force=True)
        from .content import export_onnx
        export_onnx(enc_path)

    P.progress(1.0, "Done", force=True)
    return {"model": "voice.onnx", "sampleRate": M.SR, "hop": 960, "contentEncoder": enc_path}


def _verify_onnx(path: str):
    try:
        import onnxruntime as ort
    except ImportError:
        P.log("onnxruntime not installed - skipped ONNX verification")
        return
    s = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    y = s.run(None, {"units": np.random.randn(1, 30, 768).astype(np.float32),
                     "f0": np.full((1, 30), 220.0, np.float32)})[0]
    assert y.shape == (1, 30 * 960), y.shape
    assert np.isfinite(y).all()
    P.log("voice.onnx verified with onnxruntime")
