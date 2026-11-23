"""VoxForge neural voice = RVC v2 48 kHz (SynthesizerTrnMs768NSFsid) fine-tuned on the user's singing.

Why RVC v2: it is the most widely used open singing-voice-conversion architecture,
it is MIT licensed, and an official pretrained 48 kHz base model exists (trained on
many speakers) - so a few minutes of the user's voice give a natural result instead
of training from scratch.

Shapes (all at 48 kHz):
    content units  : HuBERT-Base layer 12, 768-dim, 50 frames/s  -> interpolated x2 to 100 frames/s
    f0             : Hz at 100 frames/s (hop 480 @ 48 kHz), 0 = unvoiced
    audio          : 480 samples per 100 Hz frame

Plug-in contract (voice.onnx), unchanged for the C++ side:
    units [1, T, 768] @ 50 fps, f0 [1, T] (Hz) @ 50 fps  ->  audio [1, T * 960]
"""
from __future__ import annotations

import math
import os

import torch
import torch.nn as nn
import torch.nn.functional as F

SR = 48000
HOP = 480                      # model frame = 10 ms
SEGMENT = 17280                # training slice in samples (36 frames), as in RVC 48k_v2
N_FFT = 2048
N_MELS = 128
BASE_URLS = {
    "f0G48k.pth": "https://huggingface.co/lj1995/VoiceConversionWebUI/resolve/main/pretrained_v2/f0G48k.pth",
    "f0D48k.pth": "https://huggingface.co/lj1995/VoiceConversionWebUI/resolve/main/pretrained_v2/f0D48k.pth",
}

# RVC configs/v2/48k.json "model" section, as positional args of SynthesizerTrnMs768NSFsid
MODEL_ARGS = dict(
    spec_channels=N_FFT // 2 + 1, segment_size=SEGMENT // HOP, inter_channels=192, hidden_channels=192,
    filter_channels=768, n_heads=2, n_layers=6, kernel_size=3, p_dropout=0, resblock="1",
    resblock_kernel_sizes=[3, 7, 11], resblock_dilation_sizes=[[1, 3, 5], [1, 3, 5], [1, 3, 5]],
    upsample_rates=[12, 10, 2, 2], upsample_initial_channel=512, upsample_kernel_sizes=[24, 20, 4, 4],
    spk_embed_dim=109, gin_channels=256, sr=SR,
)

F0_MEL_MIN = 1127 * math.log(1 + 50 / 700)
F0_MEL_MAX = 1127 * math.log(1 + 1100 / 700)


def f0_to_coarse(f0: torch.Tensor) -> torch.Tensor:
    """Hz -> RVC pitch bins 1..255 (0 for unvoiced handled by the 1-floor like RVC)."""
    mel = 1127 * torch.log(1 + f0 / 700)
    mel = torch.where(mel > 0, (mel - F0_MEL_MIN) * 254 / (F0_MEL_MAX - F0_MEL_MIN) + 1, mel)
    mel = torch.clamp(mel, 1, 255)
    return torch.round(mel).long()


def build_generator():
    from .rvc.models import SynthesizerTrnMs768NSFsid
    return SynthesizerTrnMs768NSFsid(**MODEL_ARGS, is_half=False)


def build_discriminator():
    from .rvc.models import MultiPeriodDiscriminatorV2
    return MultiPeriodDiscriminatorV2(False)


#----------------------------------------------------------------------------- base model
def base_paths(models_dir: str):
    d = os.path.join(models_dir, "base")
    return {k: os.path.join(d, k) for k in BASE_URLS}, os.path.join(d, "base.pt")


def download_base(models_dir: str, progress=None) -> dict:
    """Downloads the official RVC v2 48 kHz pretrained base (MIT) into Models/base/."""
    import urllib.request
    files, _ = base_paths(models_dir)
    os.makedirs(os.path.dirname(next(iter(files.values()))), exist_ok=True)
    for i, (name, path) in enumerate(files.items()):
        if os.path.exists(path) and os.path.getsize(path) > 1_000_000:
            continue
        tmp = path + ".part"
        with urllib.request.urlopen(BASE_URLS[name], timeout=60) as r, open(tmp, "wb") as f:
            total = int(r.headers.get("Content-Length", 0)) or 1
            done = 0
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                done += len(chunk)
                if progress:
                    progress((i + done / total) / len(files), f"Downloading {name} ({done >> 20} MB)")
        os.replace(tmp, path)
    # sanity check: loadable and shaped like RVC v2 48k
    g = torch.load(files["f0G48k.pth"], map_location="cpu", weights_only=False)
    sd = g["model"] if "model" in g else g
    assert sd["enc_p.emb_phone.weight"].shape[1] == 768, "not an RVC v2 (768-dim) base model"
    return files


def load_base(G, D, models_dir: str, explicit: str = "", log=print) -> str:
    """Initialise from (1) an explicit checkpoint, (2) a VoxForge-trained base, (3) the official RVC v2 base."""
    files, own = base_paths(models_dir)

    def sd_of(path):
        ck = torch.load(path, map_location="cpu", weights_only=False)
        return ck

    if explicit:
        ck = sd_of(explicit)
        if "G" in ck:
            G.load_state_dict(ck["G"]); D.load_state_dict(ck["D"])
        else:
            G.load_state_dict(ck.get("model", ck))
        return explicit
    if os.path.exists(own):
        ck = sd_of(own)
        G.load_state_dict(ck["G"]); D.load_state_dict(ck["D"])
        return own
    if os.path.exists(files["f0G48k.pth"]):
        G.load_state_dict(sd_of(files["f0G48k.pth"])["model"])          # strict: architecture must match exactly
        if os.path.exists(files["f0D48k.pth"]):
            D.load_state_dict(sd_of(files["f0D48k.pth"])["model"])
        return files["f0G48k.pth"]
    return ""


#----------------------------------------------------------------------------- spectra & losses
_windows: dict = {}
_mel_fb: dict = {}


def spectrogram(y: torch.Tensor) -> torch.Tensor:
    """[B, N] -> magnitude [B, 1025, N / 480] (RVC/VITS convention: reflect pad, center=False)."""
    key = (y.device, y.dtype)
    if key not in _windows:
        _windows[key] = torch.hann_window(N_FFT, device=y.device, dtype=y.dtype)
    pad = (N_FFT - HOP) // 2
    y = F.pad(y[:, None], (pad, pad), mode="reflect")[:, 0]
    s = torch.stft(y, N_FFT, hop_length=HOP, win_length=N_FFT, window=_windows[key],
                   center=False, return_complex=True)
    return torch.sqrt(s.real ** 2 + s.imag ** 2 + 1e-6)


def spec_to_mel(spec: torch.Tensor) -> torch.Tensor:
    import torchaudio.functional as AF
    key = (spec.device, spec.dtype)
    if key not in _mel_fb:
        fb = AF.melscale_fbanks(N_FFT // 2 + 1, 0.0, SR / 2, N_MELS, SR, norm="slaney", mel_scale="slaney")
        _mel_fb[key] = fb.T.to(spec.device, spec.dtype)          # [128, 1025]
    return torch.log(torch.clamp(torch.matmul(_mel_fb[key], spec), min=1e-5))


def kl_loss(z_p, logs_q, m_p, logs_p, z_mask):
    kl = logs_p - logs_q - 0.5 + 0.5 * ((z_p - m_p) ** 2) * torch.exp(-2.0 * logs_p)
    return torch.sum(kl * z_mask) / torch.sum(z_mask)


def feature_loss(fmap_r, fmap_g):
    loss = 0
    for dr, dg in zip(fmap_r, fmap_g):
        for rl, gl in zip(dr, dg):
            loss = loss + torch.mean(torch.abs(rl.float().detach() - gl.float()))
    return loss * 2


def discriminator_loss(real, fake):
    loss = 0
    for dr, dg in zip(real, fake):
        loss = loss + torch.mean((1 - dr.float()) ** 2) + torch.mean(dg.float() ** 2)
    return loss


def generator_loss(fake):
    return sum(torch.mean((1 - dg.float()) ** 2) for dg in fake)


#----------------------------------------------------------------------------- ONNX export wrapper
def onnx_export(*args, **kwargs):
    """torch.onnx.export with the TorchScript exporter (torch >= 2.9 defaults to dynamo)."""
    try:
        return torch.onnx.export(*args, dynamo=False, **kwargs)
    except TypeError:                     # older torch: no 'dynamo' argument
        return torch.onnx.export(*args, **kwargs)


class VoiceOnnx(nn.Module):
    """Adapts RVC's inference graph to the plug-in contract (50 fps in, 960 samples per frame out)."""

    def __init__(self, net):
        super().__init__()
        self.net = net          # rvc.models_onnx.SynthesizerTrnMsNSFsidM

    def forward(self, units, f0):
        u = F.interpolate(units.transpose(1, 2), scale_factor=2.0, mode="nearest").transpose(1, 2)   # [1, 2T, 768]
        f = F.interpolate(f0[:, None], scale_factor=2.0, mode="nearest")[:, 0]                       # [1, 2T]
        lengths = torch.ones_like(f).sum(dim=1).long()                                                # dynamic length
        pitch = f0_to_coarse(f)
        sid = torch.zeros_like(lengths)
        rnd = torch.randn_like(u[:, :, :192].transpose(1, 2)) * 0.66666
        o = self.net(u, lengths, pitch, f, sid, rnd)                                                  # [1, 1, 2T*480]
        return o[:, 0, :]


def export_voice_onnx(G_state: dict, path: str, opset: int = 17):
    from .rvc.models_onnx import SynthesizerTrnMsNSFsidM
    net = SynthesizerTrnMsNSFsidM(**MODEL_ARGS, version="v2", is_half=False)
    net.load_state_dict(G_state, strict=False)
    net.eval()
    net.remove_weight_norm()
    model = VoiceOnnx(net).eval()
    units = torch.randn(1, 50, 768)
    f0 = torch.full((1, 50), 220.0)
    tmp = path + ".tmp"
    with torch.no_grad():
        onnx_export(model, (units, f0), tmp, input_names=["units", "f0"], output_names=["audio"],
                    dynamic_axes={"units": {1: "frames"}, "f0": {1: "frames"}, "audio": {1: "samples"}},
                    opset_version=opset, do_constant_folding=True)
    os.replace(tmp, path)
    return path
