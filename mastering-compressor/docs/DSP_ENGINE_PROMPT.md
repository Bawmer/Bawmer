# Prompt: Mastering-Grade Compressor DSP Engine — Mathematical Design & Implementation

> Paste everything below the line into your AI assistant or hand it to a DSP engineer.
> Section 0 describes the existing Lumen MC-64 code. Delete it if you want a design from scratch.

---

You are a senior audio DSP engineer. Design, derive, implement and verify the **mathematical
engine** of a mastering compressor: detection, gain computation, adaptive ballistics, analog
feedback modeling, gain smoothing and lookahead. Don't spend time on UI, presets or the plugin
wrapper. The work is the math, the reasoning behind each choice, a real-time C++17
implementation and the evidence that it behaves as claimed.

Everything runs in 64-bit floating point. Treat that as a precondition, not as the source of
quality. Transparency comes from the gain-control law and the shape of the gain trajectory, so
spend your effort there.

## 0. Existing code (context)

The current engine lives in `mastering-compressor/dsp/include/lumen/`. It is header-only, has no
JUCE dependency, and is verified by `tests/TestMain.cpp`.

- `GainComputer.h` — quadratic soft knee (Giannoulis/Massberg/Reiss 2012) for feed-forward, and
  a feedback law `-(R-1)·overshoot` whose closed-loop fixed point matches the FF curve.
- `Envelope.h` — peak / RMS (AES-17 calibrated, 10 ms) / hybrid detectors. Digital ballistics use
  a smooth-branching one-pole in the dB domain with optional dual-envelope auto-release. Analog
  ballistics add overshoot-dependent attack (0.5×–3×), dual release and a 0.35 ms RC stage.
- `CompressorEngine.h` — lookahead (delay plus sliding max of the reduction target), stereo link
  (blend toward the louder channel's reduction), M/S, a detector tilt (−3 dB below 90 Hz,
  +1.5 dB above 5 kHz) and a one-pole anti-modulation stage in Ultra mode.
  Feedback stability currently comes from clamping the envelope coefficient to `a ≥ k/(k+1)`.

Keep the public API (`CompressorParams`, `CompressorEngine::process`) and all existing tests
passing. Where you replace an algorithm, show measurements that the new one is better.

## 1. Signal model and notation

Define and use consistently:

- `x[n]` main input, `s[n]` sidechain, `y[n]` output, sample rate `fs` (44.1–384 kHz).
- Level `L[n]` (dB), static gain `G_s(L)` (dB, ≤ 0), applied gain `g[n]` (dB),
  reduction `r[n] = −g[n] ≥ 0`.
- Threshold `T`, ratio `R`, knee width `W`, attack `τ_A`, release `τ_R`.
- State every time constant's definition explicitly: 1−1/e, 10–90 %, or dB/s.

Write each block as an equation first, then as code. For every nonlinear or recursive block,
state its fixed points, its steady-state behaviour and its stability conditions.

## 2. Level detection

Derive and implement:

1. **Peak detection** on the rectified signal, plus an optional **true-peak** detector:
   4× polyphase interpolation following ITU-R BS.1770-4 Annex 2. Give the residual
   intersample error bound for the chosen filter.
2. **RMS / mean-square** detection. Compare a one-pole mean square with a rectangular
   (moving-sum) window. Use running-sum-with-correction or Kahan compensation so the window
   cannot drift over hours of audio. Calibrate so a sine reads identically in all modes.
3. **Crest-factor estimation** `C[n] = L_peak − L_rms` (dB), smoothed. It drives the adaptive
   timing in section 4.
4. **Hybrid detection**. Replace the current `max(rms, peak − 6 dB)` with a continuous blend
   weighted by crest factor, with no discontinuity in `∂L/∂x`.
5. **Frequency-dependent detection**. Specify the sidechain filters (HPF 20–500 Hz, optional
   LPF, analog-mode tilt) as transfer functions. Show the magnitude responses. Explain the
   effect of detector phase on the timing of low-frequency gain reduction.
6. **Detector ripple**. Derive the steady-state ripple of `L[n]` for a sine of frequency `f` as
   a function of the window or release time. Bound the resulting gain ripple. This ripple is the
   main source of low-frequency distortion in compressors.

## 3. Static gain computer

1. Generalise the knee. Provide the existing quadratic knee (C¹) and a **C²-continuous** knee
   (cubic Hermite or a smooth log-sum-exp). Derive both. Prove that the slope stays monotonic
   between 1 and 1/R.
2. Specify optional **upward / parallel behaviour** only where it follows from the mix law.
   Derive the effective static curve of parallel compression at mix `m`. Note that it is not
   the same as a lower ratio.
3. **Feedback static curve**. Derive the FB gain law for any FF knee shape: given
   `y = x + G_fb(y)`, find `G_fb` such that the closed loop equals `G_s`. Show the solution for
   the C² knee too. Where no closed form exists, solve it by implicit inversion.
4. **Auto-makeup**. Compare compensating the static curve at a reference level with
   compensating by long-term average reduction (the current approach). Recommend one, with
   reasons, and state its loudness-matching error on pink noise and on a dynamic music proxy.

## 4. Adaptive attack / release (the core of this task)

Design the ballistics in the **log (dB) domain**. Justify that choice against linear-domain
smoothing using sideband and pumping arguments.

1. **Smooth-branching baseline**. Derive the branching one-pole and its level-corrected variant
   (Giannoulis). Show exactly which "attack time" the user parameter represents.
2. **Program-dependent release**. Derive a release whose time constant adapts to:
   - depth of reduction, so deep reduction recovers more slowly;
   - duration of reduction, which is what the dual-envelope design models — formalise it as a
     two-state linear system and give its step and burst responses in closed form;
   - crest factor, so transient material releases faster and dense material slower.

   Give the adaptation law explicitly, for example `τ_R,eff = τ_R · f(r, C, ṙ)`, with bounded
   range, monotonic behaviour and no discontinuities. Make it depend only on smoothed quantities,
   so the time constant itself never modulates at audio rate.
3. **Program-dependent attack**. Formalise the current overshoot-dependent attack. Analyse its
   effect on transient preservation, measured as the change in crest factor between input and
   output on drum hits, against a fixed attack.
4. **Hysteresis / hold**. Add an optional hold of 0–50 ms before release. Show how hold avoids
   gain ripple on low-frequency periodic material, and derive the hold needed to suppress
   ripple at 30 Hz.
5. **Auto-release presets**. Produce three named parameter sets (Smooth, Program, Fast) with
   measured recovery curves for a 10 ms burst, a 300 ms burst and a 3 s tone burst.

For every ballistic you design, provide its step response, its burst response at 5, 50, 500 and
5000 ms, and its response to a 40 Hz sine (ripple). Show plots or tables.

## 5. Gain-trajectory shaping and modulation distortion

Applying a time-varying gain is amplitude modulation. Make this explicit:

1. Derive the output spectrum `Y = X * 𝓕{10^{g/20}}`. Show how the corners of a one-pole
   envelope widen the gain spectrum and therefore the sidebands.
2. Design an **anti-modulation smoother** that bandlimits the gain trajectory without adding
   lag to attack onset. Candidates: critically damped 2nd-order, a Bessel-like 2-pole, and
   a slew-rate limit in dB/ms. Compare sideband levels on a 1 kHz tone under 6 dB of 40 Hz
   gain modulation and pick one.
3. **Gain-path aliasing**. When attack is below about 0.1 ms, the gain's bandwidth can approach
   `fs/2`, and multiplying it with HF content aliases. Decide whether the gain multiplication
   needs to run at the oversampled rate in fast settings. Back the decision with a measurement
   (alias level vs attack time at 44.1 kHz). If it does, implement it as an option that adds no
   cost when it is off.
4. **Stereo / M/S image stability**. Derive interchannel gain mismatch as a function of link
   amount. Bound how far the image can shift: for a source panned by `p`, the apparent pan
   moves by `Δp`. Define and implement the link law, comparing a max-of-reductions link with a
   detector-domain link (shared level). Recommend one for mastering.

## 6. Lookahead

1. The current method (sliding max of the target, then one-pole attack) reaches only
   `1 − e^{−L/τ_A}` of the target by the time the transient arrives. Design a lookahead whose
   gain reaches **100 % of the required reduction exactly at the transient onset**, with a
   smooth (C¹) approach. One option: sliding max followed by a length-L FIR smoothing window
   (Hann or raised cosine) whose integral is normalised so the onset is fully attenuated. Prove
   it cannot overshoot, which makes it suitable as a limiter-safe option.
2. Give the latency as an exact integer. Show the behaviour when lookahead changes at runtime
   (crossfade or reset), with no audible click.
3. Measure the peak overshoot above the static curve on a 0.5 ms click train for lookahead of
   0, 1, 5 and 20 ms.

## 7. Analog feedback and component modeling

Model the gain element and loop as continuous-time systems, then discretise them properly:

1. **Topologies**. Write ODE / block models for:
   - VCA feed-forward and feedback;
   - opto: an LDR with a level- and history-dependent resistance. Use a two-time-constant
     memory model (fast plus slow "light memory") to produce the classic program-dependent
     release;
   - vari-mu: the ratio rises with level, giving a level-dependent knee;
   - FET: fast attack, with ratio and distortion set by the operating point.

   For each, give the governing equations, the parameters, and the behaviour that should be
   audible. Keep "analog-inspired" honest: say what is modelled and what is only approximated.
2. **Zero-delay feedback**. The current FB loop has a one-sample delay and relies on coefficient
   clamping for stability. Replace it with an **implicit solution** of the instantaneous loop
   `y[n] = x[n] · 10^{g(y[n])/20}` combined with the envelope state update. Solve it per sample
   with Newton–Raphson in the dB domain:
   - derive the residual and its Jacobian;
   - give an initial guess from the previous sample;
   - bound the iterations (typically 2–4), and give a convergence proof or empirical
     convergence rate across the full parameter range;
   - add a fallback (damped Newton or bisection) for pathological inputs.

   Compare the result with the 1-sample-delay loop: static curve error, stability margin and
   transient overshoot.
3. **Stability analysis**. Linearise every feedback configuration around its operating point,
   give the loop gain and pole locations, and show stability for R up to 20:1, all attack times
   and every sample rate in range. Where stability needs a constraint, derive the constraint
   and enforce it smoothly, not as a hard clamp.
4. **Discretisation**. Use TPT / trapezoidal integration, with pre-warping where it applies. Show
   that the time constants are identical at 44.1 and 384 kHz (sample-rate invariance) within a
   stated tolerance.

## 8. Numerical robustness

- Show each recursive state is bounded, or saturate it with a reason.
- `log` and `exp` guards: define level floors (−300 dB internal). Show that exact unity gain is
  preserved where it must be: below threshold, at 1:1, and on the bypass path.
- State the denormal strategy. FTZ/DAZ plus state flushing at a level below −600 dBFS must not
  affect any signal above −200 dBFS.
- If you use fast `exp`/`log` approximations, give their maximum error in dB and prove it can't
  produce audible gain error (< 0.001 dB) or zipper noise.
- Coefficients must stay well conditioned at 20 Hz with fs = 384 kHz.

## 9. Real-time constraints

- No allocation, locks, system calls or unbounded loops in `process()`. The Newton solver must
  have a hard iteration cap.
- Cost budget, stereo at 48 kHz on a modern x86 core: transparent modes ≤ 2 %, analog modes
  with the ZDF loop ≤ 6 % excluding oversampling. Report measured numbers.
- Parameter changes are click-free: smooth every value that enters the gain path, and never
  recompute coefficients in a way that steps the gain.

## 10. Verification and acceptance criteria

Add tests to the existing suite. Every criterion below needs an automated pass/fail check.

| Area | Acceptance criterion |
|---|---|
| Static curve | engine steady state on the static curve within ±0.05 dB (RMS) and ±0.1 dB (peak), all knees, FF and FB |
| C² knee | second derivative continuous (finite-difference jump < 1e-6) |
| ZDF feedback | closed-loop static error < 0.01 dB vs FF curve. No limit cycles for R ≤ 20, attack ≥ 0.01 ms, 44.1–384 kHz |
| Ballistics | measured τ within 2 % of the specification. Adaptive release monotonic in depth and duration |
| Ripple | 40 Hz sine at 10 dB GR, release 100 ms: THD+N ≤ −60 dB (Ultra mode with hold/smoothing) |
| Modulation | 1 kHz tone under 6 dB of 40 Hz gain modulation: sidebands beyond ±200 Hz ≤ −90 dBc after the anti-modulation smoother |
| Lookahead | with exact-onset lookahead, the output never exceeds the static-curve target on a click train (0 dB overshoot) |
| True peak | intersample-peak detection error ≤ 0.5 dB on worst-case test signals |
| Image | 100 % link: interchannel gain difference exactly 0. Pan shift ≤ 0.1 dB at 50 % link for 12 dB GR |
| Sample-rate invariance | step and burst responses at 44.1 and 384 kHz agree within 0.1 dB |
| Transparency | Ultra mode below threshold: bit-exact null (keep the existing test). At 1:1: bit-exact |
| Stress | 10 minutes of random noise, square waves, DC steps, silence ↔ +24 dBFS bursts: no NaN/Inf, no state blow-up |

## 11. Deliverables

1. **Design document**: equations, derivations, stability proofs, design decisions with the
   alternatives considered and why they were rejected, and all plots and tables from sections
   2–7.
2. **Implementation**: modular C++17 headers in `dsp/include/lumen/` (for example
   `Detector.h`, `GainLaw.h`, `Ballistics.h`, `LookaheadShaper.h`, `FeedbackSolver.h`,
   `AnalogModels.h`), integrated into `CompressorEngine`. Every algorithm gets a comment block
   citing its source or derivation.
3. **Tests**: every criterion in section 10, plus regression tests for the existing behaviour.
4. **Measurement report**: CPU numbers, measured sideband and THD+N figures, and
   before/after comparisons against the current engine.
5. **Limitations**: an honest list of what the analog models do not capture.

Work incrementally. Derive first, then implement, then measure. When a measurement contradicts
a derivation, say so, investigate, and fix the derivation or the code. Don't loosen the test to
make it pass.

### References to build on

- D. Giannoulis, M. Massberg, J. D. Reiss — *Digital Dynamic Range Compressor Design — A Tutorial
  and Analysis*, JAES 60(6), 2012.
- U. Zölzer (ed.) — *DAFX: Digital Audio Effects*, 2nd ed., ch. 4 (dynamics processing).
- V. Zavalishin — *The Art of VA Filter Design* (TPT / ZDF, nonlinear feedback solving).
- ITU-R BS.1770-4 — loudness and true-peak measurement.
- J. S. Abel, D. P. Berners — work on analog compressor modeling (opto / vari-mu behaviour).
