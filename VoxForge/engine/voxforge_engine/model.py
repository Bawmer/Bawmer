"""VoxForge neural voice model.

Generator  (exported to voice.onnx):
    units [B, T, 768] + f0 [B, T] (Hz, 0 = unvoiced)  ->  audio [B, T * 960] @ 48 kHz

    units ─ Linear ─┐
    f0 ── log-bin ──┴─ Embedding ─ +  ─ dilated conv stack ─ HiFi-GAN upsampler (x8 x6 x4 x5) ─ tanh
                                                  ▲          ▲  (neural source-filter excitation
    f0 ── sample-rate sine harmonics + noise ─────┴──────────┘   injected at every resolution)

Discriminators (training only): multi-period (2,3,5,7,11) + multi-scale.
This is the same family of architecture used by modern singing-voice
conversion systems (NSF-HiFiGAN), sized for consumer GPUs.
"""
from __future__ import annotations

import math

import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.nn.utils import remove_weight_norm, weight_norm

SR = 48000
HOP = 960
UPSAMPLE = (8, 6, 4, 5)
UPSAMPLE_K = (16, 12, 8, 11)
F0_BINS = 256
F0_MEL_MIN = 1127 * math.log(1 + 50 / 700)
F0_MEL_MAX = 1127 * math.log(1 + 1100 / 700)


def f0_to_coarse(f0: torch.Tensor) -> torch.Tensor:
    """Hz -> integer bin 1..255 on a mel scale, 0 = unvoiced."""
    mel = 1127 * torch.log(1 + f0 / 700)
    b = (mel - F0_MEL_MIN) * (F0_BINS - 2) / (F0_MEL_MAX - F0_MEL_MIN) + 1
    b = torch.clamp(b, 1, F0_BINS - 1)
    return torch.where(f0 > 0, b, torch.zeros_like(b)).round().long()


def init_weights(m, std=0.01):
    if isinstance(m, (nn.Conv1d, nn.ConvTranspose1d)):
        m.weight.data.normal_(0.0, std)


class ResBlock(nn.Module):
    def __init__(self, ch, k, dilations=(1, 3, 5)):
        super().__init__()
        self.c1 = nn.ModuleList([weight_norm(nn.Conv1d(ch, ch, k, dilation=d, padding=(k * d - d) // 2)) for d in dilations])
        self.c2 = nn.ModuleList([weight_norm(nn.Conv1d(ch, ch, k, padding=(k - 1) // 2)) for _ in dilations])
        self.c1.apply(init_weights)
        self.c2.apply(init_weights)

    def forward(self, x):
        for a, b in zip(self.c1, self.c2):
            x = x + b(F.leaky_relu(a(F.leaky_relu(x, 0.1)), 0.1))
        return x

    def remove_wn(self):
        for c in list(self.c1) + list(self.c2):
            remove_weight_norm(c)


class SineSource(nn.Module):
    """Neural source-filter excitation: harmonic sines for voiced, noise for unvoiced."""

    def __init__(self, harmonics=8, amp=0.1, noise=0.003):
        super().__init__()
        self.h = harmonics
        self.amp = amp
        self.noise = noise
        self.merge = nn.Linear(harmonics, 1)

    def forward(self, f0_up):                          # [B, N] Hz at 48 kHz
        k = torch.arange(1, self.h + 1, device=f0_up.device, dtype=f0_up.dtype)
        inc = f0_up[..., None] * k / SR                # cycles per sample per harmonic
        phase = torch.cumsum(inc, dim=1)
        phase = phase - torch.floor(phase)             # keep precision
        sines = torch.sin(2 * math.pi * phase) * self.amp
        uv = (f0_up > 0).to(f0_up.dtype)[..., None]
        n = torch.randn_like(sines)
        sig = sines * uv + n * (uv * self.noise + (1 - uv) * self.amp / 3)
        sig = sig * (k <= (SR / 2) / torch.clamp(f0_up[..., None], min=1.0)).to(sig.dtype)   # no aliasing
        return torch.tanh(self.merge(sig)).transpose(1, 2)   # [B, 1, N]


class Generator(nn.Module):
    def __init__(self, unit_dim=768, hidden=256, ch=512, resblock_k=(3, 7, 11)):
        super().__init__()
        self.unit_proj = nn.Linear(unit_dim, hidden)
        self.f0_emb = nn.Embedding(F0_BINS, hidden)
        self.enc = nn.ModuleList([nn.Conv1d(hidden, hidden, 5, padding=2 * d, dilation=d) for d in (1, 2, 4, 1, 2, 4)])
        self.enc_norm = nn.ModuleList([nn.GroupNorm(1, hidden) for _ in range(6)])
        self.pre = weight_norm(nn.Conv1d(hidden, ch, 7, padding=3))
        self.source = SineSource()

        self.ups = nn.ModuleList()
        self.noise_convs = nn.ModuleList()
        self.res = nn.ModuleList()
        c = ch
        for i, (u, k) in enumerate(zip(UPSAMPLE, UPSAMPLE_K)):
            c_out = ch // (2 ** (i + 1))
            self.ups.append(weight_norm(nn.ConvTranspose1d(c, c_out, k, u, padding=(k - u) // 2)))
            stride = int(math.prod(UPSAMPLE[i + 1:]))
            if stride > 1:
                self.noise_convs.append(nn.Conv1d(1, c_out, stride * 2, stride=stride, padding=(stride + 1) // 2))
            else:
                self.noise_convs.append(nn.Conv1d(1, c_out, 1))
            self.res.append(nn.ModuleList([ResBlock(c_out, rk) for rk in resblock_k]))
            c = c_out
        self.post = weight_norm(nn.Conv1d(c, 1, 7, padding=3))
        self.ups.apply(init_weights)
        self.post.apply(init_weights)

    def forward(self, units, f0):
        """units [B, T, C], f0 [B, T] -> audio [B, T * HOP]."""
        T = units.shape[1]
        x = self.unit_proj(units) + self.f0_emb(f0_to_coarse(f0))
        x = x.transpose(1, 2)
        for conv, norm in zip(self.enc, self.enc_norm):
            x = x + F.gelu(norm(conv(x)))
        x = self.pre(x)

        f0_up = F.interpolate(f0[:, None], scale_factor=float(HOP), mode="nearest")[:, 0]
        exc = self.source(f0_up)                        # [B, 1, T*HOP]

        for up, nc, rbs in zip(self.ups, self.noise_convs, self.res):
            x = up(F.leaky_relu(x, 0.1))
            e = nc(exc)
            n = min(x.shape[-1], e.shape[-1])
            x = x[..., :n] + e[..., :n]
            x = sum(rb(x) for rb in rbs) / len(rbs)
        x = torch.tanh(self.post(F.leaky_relu(x)))
        x = x[:, 0, : T * HOP]
        if x.shape[-1] < T * HOP:
            x = F.pad(x, (0, T * HOP - x.shape[-1]))
        return x

    def remove_weight_norm(self):
        remove_weight_norm(self.pre)
        remove_weight_norm(self.post)
        for u in self.ups:
            remove_weight_norm(u)
        for rbs in self.res:
            for rb in rbs:
                rb.remove_wn()


#--------------------------------------------------------------------------- discriminators
class PeriodD(nn.Module):
    def __init__(self, period):
        super().__init__()
        self.p = period
        chs = [1, 32, 128, 512, 1024]
        self.convs = nn.ModuleList([weight_norm(nn.Conv2d(chs[i], chs[i + 1], (5, 1), (3, 1), padding=(2, 0))) for i in range(4)])
        self.convs.append(weight_norm(nn.Conv2d(1024, 1024, (5, 1), 1, padding=(2, 0))))
        self.out = weight_norm(nn.Conv2d(1024, 1, (3, 1), 1, padding=(1, 0)))

    def forward(self, x):
        fmap = []
        b, c, t = x.shape
        if t % self.p:
            x = F.pad(x, (0, self.p - t % self.p), "reflect")
            t = x.shape[-1]
        x = x.view(b, c, t // self.p, self.p)
        for l in self.convs:
            x = F.leaky_relu(l(x), 0.1)
            fmap.append(x)
        x = self.out(x)
        fmap.append(x)
        return torch.flatten(x, 1, -1), fmap


class ScaleD(nn.Module):
    def __init__(self):
        super().__init__()
        self.convs = nn.ModuleList([
            weight_norm(nn.Conv1d(1, 16, 15, 1, padding=7)),
            weight_norm(nn.Conv1d(16, 64, 41, 4, groups=4, padding=20)),
            weight_norm(nn.Conv1d(64, 256, 41, 4, groups=16, padding=20)),
            weight_norm(nn.Conv1d(256, 1024, 41, 4, groups=64, padding=20)),
            weight_norm(nn.Conv1d(1024, 1024, 41, 4, groups=256, padding=20)),
            weight_norm(nn.Conv1d(1024, 1024, 5, 1, padding=2)),
        ])
        self.out = weight_norm(nn.Conv1d(1024, 1, 3, 1, padding=1))

    def forward(self, x):
        fmap = []
        for l in self.convs:
            x = F.leaky_relu(l(x), 0.1)
            fmap.append(x)
        x = self.out(x)
        fmap.append(x)
        return torch.flatten(x, 1, -1), fmap


class Discriminator(nn.Module):
    def __init__(self):
        super().__init__()
        self.ds = nn.ModuleList([ScaleD()] + [PeriodD(p) for p in (2, 3, 5, 7, 11)])

    def forward(self, y, y_hat):
        rs, gs, frs, fgs = [], [], [], []
        for d in self.ds:
            r, fr = d(y)
            g, fg = d(y_hat)
            rs.append(r); gs.append(g); frs.append(fr); fgs.append(fg)
        return rs, gs, frs, fgs


#--------------------------------------------------------------------------- losses
def feature_loss(fmap_r, fmap_g):
    loss = 0
    for dr, dg in zip(fmap_r, fmap_g):
        for r, g in zip(dr, dg):
            loss = loss + torch.mean(torch.abs(r.detach() - g))
    return loss * 2


def discriminator_loss(real, fake):
    loss = 0
    for r, g in zip(real, fake):
        loss = loss + torch.mean((1 - r) ** 2) + torch.mean(g ** 2)
    return loss


def generator_loss(fake):
    return sum(torch.mean((1 - g) ** 2) for g in fake)


class MelLoss(nn.Module):
    def __init__(self):
        super().__init__()
        import torchaudio
        self.mels = nn.ModuleList([
            torchaudio.transforms.MelSpectrogram(SR, n_fft=n, win_length=n, hop_length=n // 4, n_mels=m,
                                                 f_min=40, f_max=SR // 2, power=1.0, center=True)
            for n, m in ((512, 64), (1024, 100), (2048, 128))])

    def forward(self, y, y_hat):
        loss = 0
        for mel in self.mels:
            loss = loss + F.l1_loss(torch.log(mel(y_hat) + 1e-5), torch.log(mel(y) + 1e-5))
        return loss / len(self.mels)
