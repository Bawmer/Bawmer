# Engine design notes

* Round 1 — exact lookahead and zero-delay feedback (§1–3 below)
* Round 2 — true peak, adaptive release, hold, smooth hybrid detector (§4–7)
* Round 3 — analog component models (§8)

---

# Round 1: exact lookahead and zero-delay feedback

This round implements sections 3.3, 6 and 7.2–7.4 of [`DSP_ENGINE_PROMPT.md`](DSP_ENGINE_PROMPT.md).
Every claim here has an automated test in `tests/TestMain.cpp`. The before/after numbers come
from the same measurement program, compiled against the previous engine and the current one
(48 kHz, stereo).

## 1. Exact-onset lookahead (`Lookahead.h`)

### Problem

The previous lookahead delayed the audio by L samples and passed the reduction target through a
sliding maximum of length L+1 before the attack ballistics. By the time a transient reached the
output, a one-pole attack had only covered `1 − e^(−L/τ_A)` of the needed reduction. With a slow
attack the lookahead barely helped.

### Design

```
t[n] → sliding max over L+1 → m[n] → triangular kernel h (length W ≤ L+1, h ≥ 0, Σh = 1) → s[n]
applied reduction  r[n] = max(ballistics(s)[n], s[n])
```

**Guarantee:** `s[n] ≥ t[n − L]`, which means the sample being output is never under-compressed.
For every tap `0 ≤ j < W ≤ L+1`, the window of `m[n−j]` is `[n−j−L, n−j]`, and that window
contains `n−L`. So every `m[n−j] ≥ t[n−L]`, and therefore `s[n] = Σ h[j]·m[n−j] ≥ t[n−L]`. ∎

The downstream stages only ever add reduction, so the guarantee survives all of them:
* the `max()` with the ballistics;
* hybrid blending, which is a convex combination of values that are each ≥ their own target;
* stereo linking, which only moves r toward the larger reduction.

The kernel is two cascaded moving averages (O(1) per sample, Kahan-compensated running sums).
Its step response is a C¹ S-curve. The kernel length comes from the attack control,
`W = 1.7·attack` capped at L+1, because the triangle's step response reaches 63 % at about
0.59·W.

*Classic* mode keeps the previous behaviour (lookahead just starts the attack earlier), for
users who want slow-attack punch with lookahead.

Lookahead changes now crossfade the old and new delay taps over 20 ms with a raised-cosine
blend (`CrossfadingDelay`), on both the wet and dry paths.

### Measurements

Maximum output level above the static curve. Test signal: 0.5 ms bursts at 0 dBFS over a
−40 dBFS bed. Settings: threshold −20 dB, 20:1, peak detection, release 50 ms.

| Attack | Lookahead | Before | Exact mode |
|---|---|---|---|
| 1 ms | 0 ms | +17.31 dB | +17.31 dB (no lookahead) |
| 1 ms | 1 ms | **+5.73 dB** | **0.00 dB** |
| 1 ms | 5 ms | +0.09 dB | 0.00 dB |
| 10 ms | 1 ms | **+16.53 dB** | **0.00 dB** |
| 10 ms | 5 ms | **+10.55 dB** | **0.00 dB** |
| 10 ms | 20 ms | +2.07 dB | 0.00 dB |

Tests:
* `lookaheadShaperGuarantee` — property test, 40 random L/W combinations
* `exactLookaheadZeroOvershoot` — all four modes, linked stereo, overshoot ≤ 1e-6 dB
* `lookaheadChangeIsClickFree`
* `movingAverageNoDrift` — 10 M samples, error < 1e-9

## 2. Exact feedback gain law (`GainComputer.h`)

### Problem, found by measurement

At 20:1 the settled feedback output sat 0.73 dB below feed-forward. The cause: at high ratios
the *output* level lies only `(L_x − T)/R` above threshold, which is inside the knee. The old law
reused the quadratic knee shape with slope `1 − R`, which is only exact outside the knee. The
original test checked only levels outside the knee, so it couldn't catch this.

### Design

To make the closed loop land exactly on the feed-forward curve, invert that curve:
`G_fb(y) = G_ff(x)` where `x + G_ff(x) = y`.

Inside the knee, with `t = x − T + W/2`, `q = y − T + W/2`, `s = 1/R − 1` and `a = s/(2W)`:

```
q = t + a·t²   ⇒   t = 2q / (1 + √(1 + 4aq))
```

The rationalised root avoids cancellation as a → 0. The discriminant equals `(1+s)² ≥ 0` at the
top of the knee, so it stays real over the whole knee.

| Ratio | FB − FF before | after |
|---|---|---|
| 2 | −0.0000 dB | −0.0000 dB |
| 10 | −0.2610 dB | −0.0000 dB |
| 20 | −0.7261 dB | −0.001 dB at 1 kHz, settled (−0.012 / −0.040 dB at 200 Hz, see §3) |

Tests:
* `feedbackCurveMatchesFeedForward` — now covers every level including the knee, for 6 ratios
  × 4 knee widths, to 1e-9 dB
* `inverseStaticCurveRoundTrip`

## 3. Zero-delay feedback loop (`FeedbackSolver.h`)

### Problem

The feedback detector read the previous output sample. The linearised loop is
`e[n] = (a − k(1−a))·e[n−1] + …` with `k = R − 1`, so it is only stable when `a > (k−1)/(k+1)`.
The engine enforced that by clamping the envelope coefficient, which made the attack control
meaningless at high ratios. At 20:1, a 10 ms attack measured 0.42 ms.

### Design

Solve the instantaneous loop each sample:

```
y(r) = u·10^(−(r0 + w·r)/20)
d(r) = A·y(r) + B
H(r) = envelope(−G_fb(level(d(r))))
f(r) = r − H(r) = 0
```

* u is the delayed input. r0 and w come from the hybrid blend (r0 = 0, w = 1 in Analog mode).
* The sidechain filters are linear, so A and B come from state-preserving `peek()` calls.

Every stage of H is non-increasing in r, so f is strictly increasing. The root is therefore
unique, and `[0, H(0)]` brackets it. It is solved with the Illinois variant of regula falsi,
seeded with the previous solution, with a hard cap of 24 iterations. The average is 4.4
evaluations per sample. Plain Newton isn't used because f is only piecewise smooth (attack/release
branch, knee edges).

Three further details, each found by measurement:

1. **Loop-gain timing compensation.** Above the knee the closed-loop pole is
   `c_cl = c/(1 + (1−c)k)`. Coefficients are pre-warped with `c = c_d(1+k)/(1+c_d·k)`, so the
   attack and release controls mean the same in FB as in FF. The program-dependent attack
   interpolates in the closed-loop domain and is warped per sample, because the warp is
   nonlinear.
2. **The RC rounding stage stays outside the loop.** Pre-warping the 0.35 ms RC pole as well
   turned the two loop poles into an underdamped pair (gain ringing, overshoot, a 7.5 dB static
   error at 20:1). The loop is now closed around the envelope *before* its RC stage, and the
   applied gain still gets the RC rounding. The RC stage has unity DC gain, so the static
   behaviour is unchanged.
3. **Overshoot normalisation.** The program-dependent attack reads the overshoot
   `target − r`. In the loop, that value is (1+k) times its feed-forward equivalent, so it is
   divided by (1+k). Before this fix, 0.05 ms attacks at 20:1 overshot 0.17 dB more than
   feed-forward.

### Measurements

63 % attack time on a DC step, analog mode, threshold −30 dB:

| Ratio | Attack | FF | FB before | FB after |
|---|---|---|---|---|
| 4 | 0.05 ms | 0.31 ms | 0.13 ms | 0.33 ms |
| 4 | 1 ms | 0.90 ms | 0.31 ms | 0.92 ms |
| 4 | 10 ms | 6.15 ms | 1.33 ms | 7.15 ms |
| 20 | 0.05 ms | 0.31 ms | 0.10 ms | 0.31 ms |
| 20 | 1 ms | 0.88 ms | **0.13 ms** | 0.90 ms |
| 20 | 10 ms | 5.90 ms | **0.42 ms** | 7.35 ms |

The remaining +1 ms at 10 ms attack is *not* a compensation error. With the detector tilt
flattened, FB and FF match to the sample at every ratio (test `zdfLoopCompensationExact`, within
1 sample). The analog detector's −3 dB shelf below 90 Hz settles in about 2 ms after a step.
In FB it sits inside the loop and in FF outside it. That is a genuine property of
frequency-dependent feedback detection.

The peak GR overshoot on a step in FB is now at or below FF. For example, at 20:1 with a 1 ms
attack: FB 20.51 dB, FF 20.94 dB; before this round FB reached 24.51 dB.

**Residual static error at low frequency** (20:1, settled): −0.012 dB at 200 Hz, rising to
−0.040 dB with symmetric timing. It comes from RMS-detector ripple, which the loop multiplies by
R−1 (19× at 20:1, against 0.95× in FF). It is inherent to feedback detection. It is below
0.01 dB at 1 kHz for every ratio, and that is what `zdfFeedbackStaticMatchesFeedForward` checks.

**Cost:** analog FB goes from about 0.8 % to about 3.6 % of one core at 48 kHz stereo.

**Approximation:** each channel's loop is solved with that channel's own gain. Stereo linking
is applied afterwards. A fully coupled 2-D solve for linked feedback is possible future work.

# Round 2: true peak, adaptive release, hold, smooth hybrid detector

This round implements sections 2.1, 2.4, 4.2, 4.4 and 4.5 of the prompt.

## 4. True-peak detection (`TruePeak.h`)

*Detection → True Peak*. A 4× polyphase windowed-sinc interpolator in the detector. Phase 0 is
a pure delay, so original samples pass through unchanged. The level is the maximum of the four
phases.

**Filter length was decided by measurement.** I first used 49 taps, the length of the
BS.1770-4 reference filter. A coarse sweep suggested it was within 0.024 dB of ideal, but a
dense scan (0.0005·fs steps × 64 phases, compared with an *ideal* interpolator evaluated at the
same 4× grid points) showed under-reads up to **0.34 dB** between 0.35·fs and 0.45·fs. That's
because 49 taps at 4× can't pass 0.45·fs and still reject the image at 0.55·fs. I scanned longer
filters:

| Taps (delay) | ≤ 0.35·fs | ≤ 0.40·fs | ≤ 0.45·fs |
|---|---|---|---|
| 49 (6), β 6.5 | 0.017 dB | 0.341 dB | 0.341 dB |
| 97 (12), β 7 | 0.001 | 0.002 | 0.169 |
| **145 (18), β 7** | **0.001** | **0.001** | **0.006** |
| 193 (24), β 7 | 0.001 | 0.001 | 0.003 |

I chose 145 taps. The extra latency is 18 samples (0.375 ms at 48 kHz), and the cost is about
+0.6–0.9 % of one core in stereo. The 4× grid itself can still miss a sine's crest by up to
`−20·log10(cos(2π·f/fs/8))`: 0.38 dB at 0.375·fs and 0.55 dB at 0.45·fs. That limit applies
to every 4× true-peak meter and is documented in `TruePeakInterpolator::gridBoundDb`.

**Alignment.** The detector reports the interval that began D = 18 samples earlier, so the
audio path is delayed by D in true-peak mode. Detection stays sample-aligned, the reported
latency grows by D, and Exact lookahead keeps its zero-overshoot guarantee (tested). True peak
needs future samples, so it can't run inside the zero-delay feedback loop. There, the
feedback path uses the sample peak.

**fs/4 sine at 45°:** sample peak −3.01 dB, true peak −0.004 dB. In the engine, with threshold
−1.5 dB at 20:1, sample-peak detection applies no reduction, while true peak applies 1.43 dB.

## 5. Smooth hybrid detector

`max(rms, peak − 6 dB)` had a kink in ∂L/∂x where peaks take over. It's now
`rms + softplus(peak − 6 − rms)` with `softplus(z) = ln(1 + e^{2z})/2`. That is
C^∞, never more than ln2/2 = 0.35 dB from the hard max, and below 1e-5 dB of deviation when
peaks are inside the crest allowance, so the sine calibration is unchanged.

## 6. Adaptive auto-release (`ReleaseShapeTable`, `CrestEstimator`)

All three auto shapes are dual-envelope designs. Their release time constants are multiplied by

```
m = clamp( (1 + d·r_s/12) · 2^(c·(12 − C)/6),  1/3, 3 )
```

* `r_s`: reduction smoothed over 200 ms.
* `C`: crest factor (peak-to-RMS, dB, 500 ms windows, smoothed over 300 ms).
* Weights `(d, c)`: Smooth (0.5, 0), Program (1, 1), Fast (0, 1.5).

Both inputs are slow, and m is recalculated every 32 samples, so the release time never
modulates at audio rate. The test confirms m never moves by more than 2 % per update on gated
noise.

Recovery to < 1 dB of reduction after a 1 kHz burst (−6 dBFS, threshold −30 dB, 4:1, release
400 ms, RMS detection):

| Release mode | 10 ms burst | 300 ms burst | 3 s burst |
|---|---|---|---|
| Manual | 1137 ms | 1185 ms | 1185 ms |
| Auto Smooth | 1176 ms | 1673 ms | 1803 ms |
| Auto Program | 491 ms | 3039 ms | 3619 ms |
| Auto Fast | 148 ms | 1337 ms | 2166 ms |

* **Duration:** Program recovers 7× faster after a 10 ms hit than after 3 s of compression.
* **Depth:** after a 3 s burst at about 3 dB of reduction, recovery takes 2.8 s; at about
  18 dB it takes 3.6 s.
* **Crest:** transient hits with the same peaks recover in 1.6 s; dense material takes 3.6 s.

A steady sine is the densest possible signal (crest 3 dB), which pushes Program to its 3×
limit. That's why Program is slowest in the 3 s column. On real masters (crest about 8–12 dB)
the crest term is close to 1.

## 7. Hold

After any sample in the attack branch, release is suspended for the hold time. This is
implemented in both envelopes. In the analog envelope the hold counter is part of the
explicit state, so the feedback solver's `H(r)` stays pure, continuous and non-increasing.

**Derivation:** a peak detector on a sine of frequency f sees peaks every 1/(2f) seconds. A hold
of at least 1/(2f) means no release ever starts between peaks, so the steady-state gain is
constant. At 30 Hz that is 16.7 ms.

THD+N on a 30 Hz sine (peak detection, attack 0.1 ms, release 30 ms, 4:1, −6 dBFS):

| Hold | 0 ms | 8 ms | 16.7 ms | 25 ms |
|---|---|---|---|---|
| THD+N | −24.9 dB | −29.4 dB | **−270 dB** (numerical floor) | −270 dB |

# Round 3: analog component models

This round implements section 7.1 of the prompt.

## 8. VCA / Opto / Vari-Mu / FET (`AnalogModel`)

*Analog Model* selects one of four **behavioural archetypes** in Analog and Hybrid modes. Each
one models widely documented behaviour of a family of gain-control circuits: timing, knee shape,
and how distortion relates to gain reduction. **None is a circuit simulation, and none models a
specific product.** All four share the engine's infrastructure: the exact feedback law, the
zero-delay loop, hold, auto-release and exact lookahead.

| Model | Static character | Timing | Distortion (AMP stage on) |
|---|---|---|---|
| VCA | knee ≥ 6 dB | program-dependent attack (0.5–3×), dual release, 0.35 ms RC | none beyond the amp setting |
| Opto | knee ≥ 12 dB | attack ×2, 1.5 ms RC; two-stage release with light memory | none (clean) |
| Vari-Mu | knee ≥ 18 dB: the ratio *rises* with level | attack ×1.5, 2 ms RC, gentler fast release | amp bias +0.25 at 20 dB GR → 2nd harmonic grows with compression |
| FET | knee as set | attack ×0.25, no gentle start, 0.05 ms RC | amp drive +10 dB at 20 dB GR → odd harmonics grow with compression |

**Opto light memory.** The slow branch charges over about 1.5 s of compression. Its decay
stretches by `1 + min(3, stored/6 dB)`, computed with the exp-free form
`c' = 1 − (1 − c)/(1 + mf)`. The output is `max(fast, 0.5·stored)`. The fast branch drops
quickly and the output then rides on half the stored light. In steady state the stored value
equals the fast branch, so the static curve is unchanged.

**Feedback compatibility.** The model state stays inside `evaluate()`'s explicit state, so
`H(r)` remains pure and non-increasing. The opto output is a max of non-decreasing functions
of the target, and the hold branch is target-independent. Every model also works in the
zero-delay feedback loop, and each settles on its static curve in both FF and FB.

**Distortion routing.** Model distortion goes through the oversampled *AMP* stage, using the
gain reduction averaged per 16-sample block and smoothed. Turning AMP or the colour stage off
removes it, so analog character is always something you choose.

### Measurements

48 kHz, 1 kHz sine, threshold −30 dB, 4:1, release 300 ms, attack control 10 ms.

| | VCA | Opto | Vari-Mu | FET |
|---|---|---|---|---|
| Worst settled error vs. own curve (−36 … −12 dB in) | 0.025 dB | 0.034 dB | 0.025 dB | 0.009 dB |
| Local ratio at threshold +2 / +8 / +16 dB | 2.67 / 4 / 4 | 2.00 / 4 / 4 | **1.85 / 3.43 / 4** | 4 / 4 / 4 |
| Attack 63 % (DC step) | 6.15 ms | 13.88 ms | 11.33 ms | **1.08 ms** |
| GR left 100 ms after a 0.5 s burst | 87 % | **22 %** | 89 % | 84 % |
| … after an 8 s burst | 92 % | **48 %** | 94 % | 89 % |
| Recovery to < 1 dB, 0.5 s / 8 s burst | 968 / 981 ms | **596 / 1526 ms** | 1015 / 1027 ms | 929 / 943 ms |

Harmonics relative to the VCA model with identical settings (AMP on, drive 6 dB, character
35 %). Absolute harmonic levels *fall* with more compression in every model, because the stage
sits after the gain and sees a quieter signal. So the model effect is reported as the excess
over VCA:

| | ~3 dB GR | 15 dB GR |
|---|---|---|
| Vari-Mu excess H2 | +2.0 dB | **+9.6 dB** |
| FET excess H3 | +1.4 dB | **+14.8 dB** |
| Opto excess H2 | ≈ 0 | ≈ 0 |

## Verification status

43 tests pass, clean under AddressSanitizer and UndefinedBehaviorSanitizer. The plugin builds
with zero warnings.

Round 3 adds 6 tests:
* `analogModelsStaticCurve`: 4 models × FF/FB × 4 levels, within 0.06 dB
* `variMuRatioRisesWithLevel`
* `optoTwoStageReleaseWithMemory`
* `analogModelAttackCharacter`
* `modelDistortionGrowsWithGainReduction`: includes the check that opto stays clean
* `analogModelsFeedbackStress`: every model in the zero-delay loop with hold and auto release

## Next rounds (remaining sections of the prompt)

* §3 C²-continuous knee
* §5 anti-modulation smoother selection by sideband measurement; gain-path oversampling
  decision for very fast attacks
* §5.4 link-law and image-shift bounds
