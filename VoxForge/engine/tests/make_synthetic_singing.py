"""Writes a few seconds of synthetic 'singing' (glottal-like harmonics through vowel formants,
vibrato, phrase gaps) for the CI smoke test of the training pipeline. Not a voice - just
realistic enough signal statistics to exercise slicing, f0, HuBERT, training and export."""
import sys
import numpy as np
import soundfile as sf

SR = 48000
VOWELS = [(730, 1090, 2440), (270, 2290, 3010), (300, 870, 2240), (530, 1840, 2480), (570, 840, 2410)]


def resonate(x, f, bw):
    r = np.exp(-np.pi * bw / SR); c = -r * r; b = 2 * r * np.cos(2 * np.pi * f / SR); a = 1 - b - c
    y = np.zeros_like(x); y1 = y2 = 0.0
    for i, v in enumerate(x):
        y0 = a * v + b * y1 + c * y2; y[i] = y0; y2, y1 = y1, y0
    return y


def phrase(notes, seed):
    rng = np.random.default_rng(seed)
    out = []
    for k, midi in enumerate(notes):
        n = int(SR * 0.45)
        t = np.arange(n) / SR
        f0 = 440 * 2 ** ((midi - 69) / 12) * (1 + 0.012 * np.sin(2 * np.pi * 5.5 * t) * np.clip(t / 0.25, 0, 1))
        ph = np.cumsum(f0 / SR) % 1.0
        src = (2 * ph - 3 * ph ** 2) * (ph < 0.66) + 0.02 * rng.standard_normal(n)
        F = VOWELS[(k + seed) % len(VOWELS)]
        y = src
        for f, bw in zip(F, (80, 100, 140)):
            y = resonate(y, f, bw)
        env = np.minimum(1, np.minimum(t / 0.02, (t[-1] - t) / 0.03))
        out.append(y * env)
    y = np.concatenate(out)
    return 0.6 * y / (np.max(np.abs(y)) + 1e-9)


if __name__ == "__main__":
    out = sys.argv[1]
    import os
    os.makedirs(out, exist_ok=True)
    melodies = [[60, 62, 64, 65, 67, 69, 67, 65], [57, 60, 64, 67, 64, 60, 57, 55], [64, 66, 68, 69, 71, 69, 68, 66]]
    for i, m in enumerate(melodies):
        y = np.concatenate([phrase(m, i), np.zeros(int(SR * 0.5)), phrase(m[::-1], i + 3)])
        sf.write(os.path.join(out, f"take{i}.wav"), y.astype(np.float32), SR)
    print("wrote", len(melodies), "files to", out)
