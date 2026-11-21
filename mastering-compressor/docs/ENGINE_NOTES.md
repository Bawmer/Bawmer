# Engine design notes — round 1: exact lookahead and zero-delay feedback

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

## Verification status

31 tests pass. They are also clean under AddressSanitizer and UndefinedBehaviorSanitizer
(`-fsanitize=address,undefined`). The plugin builds with zero warnings.

## Next rounds (remaining sections of the prompt)

* §2 true-peak detection (BS.1770 4× interpolation)
* §2 crest-factor-weighted hybrid detector
* §3 C²-continuous knee
* §4 crest/depth-adaptive release, hold
* §5 anti-modulation smoother selection by sideband measurement; gain-path oversampling
  decision for very fast attacks
* §5.4 link-law and image-shift bounds
* §7.1 opto / vari-mu / FET component models
