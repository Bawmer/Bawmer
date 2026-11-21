// Lumen MC-64 — level detection and gain-reduction ballistics.
//
// All envelopes run on gain *reduction* in dB (r >= 0). Smoothing in the log domain gives
// release curves that fall at a constant dB/s rate — the behaviour engineers expect from
// mastering compressors — and keeps the static curve exact once the envelope settles.
#pragma once

#include "DspCommon.h"
#include "TruePeak.h"

namespace lumen
{
enum class DetectionMode { peak = 0, rms = 1, hybrid = 2, truePeak = 3 };
enum class AutoReleaseShape { smooth = 0, program = 1, fast = 2 };

/** Converts a (sidechain-filtered) sample into a level in dB.
    RMS is AES-17 calibrated (+3.01 dB) so a steady sine reads the same in peak and RMS modes,
    which keeps the threshold meaning identical when the user switches detectors.
    True peak (TruePeak.h) reads inter-sample peaks and has a group delay of
    TruePeakInterpolator::kDelay samples, which the engine compensates on the audio path. */
class LevelDetector
{
public:
    void prepare (double sampleRate, double rmsWindowMs = 10.0) noexcept
    {
        rmsCoeff = timeConstantToCoeff (rmsWindowMs, sampleRate);
        reset();
    }

    void reset() noexcept
    {
        meanSquare = 0.0;
        truePeak.reset();
    }

    static constexpr int kTruePeakDelay = TruePeakInterpolator::kDelay;

    void setMode (DetectionMode m) noexcept { mode = m; }

    inline double process (double x) noexcept
    {
        const double sq = x * x;
        meanSquare = flushDenormal (rmsCoeff * meanSquare + (1.0 - rmsCoeff) * sq);
        if (mode == DetectionMode::truePeak)
        {
            const double tp = truePeak.process (x);
            return 20.0 * std::log10 (tp + 1.0e-15);
        }
        return levelFrom (sq, meanSquare);
    }

    /** Level for input x without advancing the state. True peak is non-causal within a sample
        (it needs future samples), so peek() — used only inside the zero-delay feedback loop —
        falls back to the sample peak in that mode. */
    inline double peek (double x) const noexcept
    {
        const double sq = x * x;
        return levelFrom (sq, rmsCoeff * meanSquare + (1.0 - rmsCoeff) * sq);
    }

private:
    inline double levelFrom (double sq, double ms) const noexcept
    {
        switch (mode)
        {
            case DetectionMode::peak:
            case DetectionMode::truePeak:
                return 10.0 * std::log10 (sq + 1.0e-30);
            case DetectionMode::rms:
                return 10.0 * std::log10 (2.0 * ms + 1.0e-30);
            case DetectionMode::hybrid:
            {
                // RMS body; peaks only take over when they exceed the body by more than 6 dB of
                // crest factor, so transients are caught without RMS-style overshoot. A soft
                // maximum  a + softplus(b - a)  replaces max(a, b): the level is C-infinity in
                // the input (no kink where peaks take over), equals the RMS level to < 1e-5 dB
                // when peaks are well below the 6 dB crest allowance, and is within 0.35 dB of
                // max() at the crossover (softplus(0) = ln 2 / k).
                const double rmsDb = 10.0 * std::log10 (2.0 * ms + 1.0e-30);
                const double peakDb = 10.0 * std::log10 (sq + 1.0e-30) - 6.0;
                return rmsDb + softplus (peakDb - rmsDb);
            }
        }
        return kMinusInfDb;
    }

public:
    /** ln(1 + e^(k·z)) / k with k = 2 per dB, evaluated without overflow. */
    static double softplus (double z) noexcept
    {
        constexpr double k = 2.0;
        return z > 0.0 ? z + std::log1p (std::exp (-k * z)) / k : std::log1p (std::exp (k * z)) / k;
    }

private:

    DetectionMode mode = DetectionMode::peak;
    double rmsCoeff = 0.0, meanSquare = 0.0;
    TruePeakInterpolator truePeak;
};

/** Smoothed crest factor (peak-to-RMS, dB) of the sidechain, used to adapt release timing.
    Peak: instant attack, 500 ms release on the squared signal; RMS: 500 ms mean square; the
    ratio is then smoothed over 300 ms, so it changes slowly and never modulates the release
    time at audio rate. A steady sine reads 3 dB, dense masters ~8–12 dB, drums 15–20 dB. */
class CrestEstimator
{
public:
    void prepare (double sampleRate) noexcept
    {
        peakCoeff = timeConstantToCoeff (500.0, sampleRate);
        msCoeff = timeConstantToCoeff (500.0, sampleRate);
        smoothCoeff = timeConstantToCoeff (300.0, sampleRate);
        reset();
    }

    void reset() noexcept
    {
        peakPow = meanSquare = 0.0;
        crestDb = 12.0;
    }

    inline void process (double x) noexcept
    {
        const double sq = x * x;
        peakPow = std::max (sq, flushDenormal (peakCoeff * peakPow));
        meanSquare = flushDenormal (msCoeff * meanSquare + (1.0 - msCoeff) * sq);
        const double c = (peakPow > 1.0e-12 && meanSquare > 1.0e-14)
                             ? clampValue (10.0 * std::log10 (peakPow / meanSquare), 0.0, 30.0)
                             : 12.0; // silence: neutral
        crestDb = smoothCoeff * crestDb + (1.0 - smoothCoeff) * c;
    }

    double getCrestDb() const noexcept { return crestDb; }

private:
    double peakCoeff = 0.0, msCoeff = 0.0, smoothCoeff = 0.0;
    double peakPow = 0.0, meanSquare = 0.0, crestDb = 12.0;
};

/** Timing for an envelope. */
struct BallisticsSettings
{
    double attackMs = 10.0;
    double releaseMs = 200.0;
    bool autoRelease = false;
    AutoReleaseShape shape = AutoReleaseShape::program;
    double holdMs = 0.0;
};

/** Auto-release shapes. All are dual-envelope designs (fast branch + slow branch, output = max);
    they differ in the branch time constants and in how the release adapts to the programme:

        release multiplier  m = clamp( (1 + depthWeight · r_s / 12) · 2^(crestWeight · (12 - C) / 6),  1/3, 3 )

    r_s = reduction smoothed over 200 ms (deep, sustained reduction -> slower recovery), C = smoothed
    crest factor in dB (transient material -> faster recovery, dense material -> slower). Both
    inputs are slow, smoothed quantities, so m itself never moves at audio rate; it is applied
    every 32 samples. Manual release (autoRelease off) uses m = 1. */
struct ReleaseShapeTable
{
    double fastReleaseFactor, fastReleaseMinMs;
    double slowAttackFactor, slowAttackMinMs;
    double slowReleaseFactor;
    double depthWeight, crestWeight;

    static ReleaseShapeTable get (AutoReleaseShape s) noexcept
    {
        switch (s)
        {
            case AutoReleaseShape::smooth:  return { 0.30, 10.0, 0.5, 150.0, 1.0, 0.5, 0.0 };
            case AutoReleaseShape::program: return { 0.10, 5.0, 0.5, 100.0, 1.0, 1.0, 1.0 };
            case AutoReleaseShape::fast:    return { 0.05, 3.0, 1.0, 300.0, 0.6, 0.0, 1.5 };
        }
        return { 0.10, 5.0, 0.5, 100.0, 1.0, 1.0, 1.0 };
    }

    double multiplier (double smoothedReductionDb, double crestDb) const noexcept
    {
        const double depth = 1.0 + depthWeight * smoothedReductionDb / 12.0;
        const double crest = std::exp2 (crestWeight * (12.0 - crestDb) / 6.0);
        return clampValue (depth * crest, 1.0 / 3.0, 3.0);
    }
};

/** Digital ballistics: smooth branching one-pole in the dB domain (Giannoulis et al.), with an
    optional program-dependent dual release (see ReleaseShapeTable):
      - a fast branch recovers quickly after short transients,
      - a slow branch charges only under sustained compression and then sets the recovery,
    output = max(fast, slow). Short peaks release fast, dense passages release slowly — minimal
    pumping on transients, no breathing on sustained material.

    Hold: after any sample in the attack branch, release is suspended for holdMs. With peak
    detection the target of a sine of frequency f peaks every 1/(2f) s, so a hold of at least
    1/(2f) (16.7 ms at 30 Hz) removes release between the peaks of the rectified waveform —
    the dominant source of low-frequency gain ripple and therefore of LF distortion. */
class DigitalEnvelope
{
public:
    void prepare (double sampleRate) noexcept
    {
        fs = sampleRate;
        reset();
        updateCoefficients();
    }

    void reset() noexcept
    {
        fast = slow = 0.0;
        holdCount = 0;
    }

    void configure (const BallisticsSettings& s) noexcept
    {
        settings = s;
        holdSamples = static_cast<int> (std::lround (std::max (0.0, s.holdMs) * 0.001 * fs));
        updateCoefficients();
    }

    /** Programme-dependent release multiplier (auto-release); cheap no-op if unchanged. */
    void setReleaseScale (double m) noexcept
    {
        if (std::abs (m - releaseScale) <= 0.005 * releaseScale)
            return;
        releaseScale = m;
        updateCoefficients();
    }

    inline double process (double target) noexcept
    {
        if (target > fast)
        {
            fast = flushDenormal (attackCoeff * fast + (1.0 - attackCoeff) * target);
            holdCount = holdSamples;
        }
        else if (holdCount > 0)
        {
            --holdCount;
        }
        else
        {
            fast = flushDenormal (fastReleaseCoeff * fast + (1.0 - fastReleaseCoeff) * target);
        }

        if (! settings.autoRelease)
            return fast;

        const double cs = fast > slow ? slowAttackCoeff : slowReleaseCoeff;
        slow = flushDenormal (cs * slow + (1.0 - cs) * fast);
        return std::max (fast, slow);
    }

    double getState() const noexcept { return settings.autoRelease ? std::max (fast, slow) : fast; }
    double getReleaseScale() const noexcept { return releaseScale; }

private:
    void updateCoefficients() noexcept
    {
        attackCoeff = timeConstantToCoeff (settings.attackMs, fs);
        if (settings.autoRelease)
        {
            const auto t = ReleaseShapeTable::get (settings.shape);
            const double m = releaseScale;
            fastReleaseCoeff = timeConstantToCoeff (std::max (t.fastReleaseMinMs, settings.releaseMs * t.fastReleaseFactor) * m, fs);
            slowAttackCoeff = timeConstantToCoeff (std::max (t.slowAttackMinMs, settings.releaseMs * t.slowAttackFactor), fs);
            slowReleaseCoeff = timeConstantToCoeff (settings.releaseMs * t.slowReleaseFactor * m, fs);
        }
        else
        {
            fastReleaseCoeff = timeConstantToCoeff (settings.releaseMs, fs);
        }
    }

    double fs = 48000.0;
    BallisticsSettings settings;
    double releaseScale = 1.0;
    double attackCoeff = 0.0, fastReleaseCoeff = 0.0, slowAttackCoeff = 0.0, slowReleaseCoeff = 0.0;
    double fast = 0.0, slow = 0.0;
    int holdSamples = 0, holdCount = 0;
};

/** Analog-inspired ballistics.
    - Program-dependent attack: small overshoots are caught gently (up to 3x slower), large ones
      quickly (down to 0.5x) — like a capacitor charged through a level-dependent current.
    - Always uses the dual (program-dependent) release.
    - A final ~0.35 ms RC stage rounds off the onset of gain changes, so the gain trajectory has
      a continuous slope (fewer sidebands than a one-pole corner).

    The state is explicit so the zero-delay feedback solver can evaluate the envelope for a
    candidate input (evaluate() is pure) and commit only the converged solution.

    Loop-gain timing compensation (feedback topology). Above the knee the feedback detector
    produces target = k·(L_y - T) with k = R - 1, and L_y = L_x - r. Substituting into a one-pole
    r[n] = c·r[n-1] + (1 - c)·target gives the closed-loop pole
          c_cl = c / (1 + (1 - c)·k),
    i.e. a feedback loop speeds every time constant up by a ratio-dependent amount. To make the
    attack/release controls mean the same in FB as in FF, each coefficient is pre-warped so that
    c_cl equals the coefficient c_d derived from the user time:
          c = c_d·(1 + k) / (1 + c_d·k).
    (Exact above the knee; inside the knee the local slope is lower, so timing there is slightly
    slower than set, as in hardware.) */
class AnalogEnvelope
{
public:
    struct State
    {
        double fast = 0.0, slow = 0.0, rc = 0.0;
        int hold = 0;
    };

    void prepare (double sampleRate) noexcept
    {
        fs = sampleRate;
        reset();
        configure (BallisticsSettings {}, 0.0);
    }

    /** Programme-dependent release multiplier (auto-release); cheap no-op if unchanged. */
    void setReleaseScale (double m) noexcept
    {
        if (std::abs (m - releaseScale) <= 0.005 * releaseScale)
            return;
        releaseScale = m;
        configure (settings, loopK);
    }

    void reset() noexcept { state = State {}; }

    /** loopGain = R - 1 when the envelope sits inside a feedback loop, else 0. */
    void configure (const BallisticsSettings& s, double loopGain) noexcept
    {
        settings = s;
        holdSamples = static_cast<int> (std::lround (std::max (0.0, s.holdMs) * 0.001 * fs));
        const double m = s.autoRelease ? releaseScale : 1.0;
        overshootScale = 1.0 / (1.0 + std::max (0.0, loopGain));
        auto c = [&] (double ms) {
            const double cd = timeConstantToCoeff (ms, fs);
            return loopGain > 0.0 ? cd * (1.0 + loopGain) / (1.0 + cd * loopGain) : cd;
        };
        // The RC rounding pole is *not* pre-warped: inside a loop of gain k it must stay much
        // faster than the envelope pole, or the pair becomes underdamped (ringing gain).
        rcCoeff = timeConstantToCoeff (0.35, fs);
        if (s.autoRelease)
        {
            const auto t = ReleaseShapeTable::get (s.shape);
            fastReleaseCoeff = c (std::max (std::max (8.0, t.fastReleaseMinMs), s.releaseMs * t.fastReleaseFactor) * m);
            slowAttackCoeff = c (std::max (std::max (120.0, t.slowAttackMinMs), s.releaseMs * t.slowAttackFactor));
            slowReleaseCoeff = c (s.releaseMs * t.slowReleaseFactor * m);
        }
        else
        {
            fastReleaseCoeff = c (std::max (8.0, s.releaseMs * 0.25));
            slowAttackCoeff = c (std::max (120.0, s.releaseMs * 0.6));
            slowReleaseCoeff = c (s.releaseMs);
        }
        // attack endpoints are kept as *closed-loop* (desired) coefficients: the program-dependent
        // interpolation happens there, and the result is pre-warped per sample (the warp is
        // nonlinear, so warping the endpoints and interpolating would not give the same timing)
        attackCoeffSlow = timeConstantToCoeff (s.attackMs * 3.0, fs);
        attackCoeffFast = timeConstantToCoeff (s.attackMs * 0.5, fs);
        loopK = std::max (0.0, loopGain);
    }

    /** Output and successor state for an input, without modifying this envelope. */
    inline double evaluate (double target, const State& in, State& out) const noexcept
    {
        if (target > in.fast)
        {
            // map overshoot onto slow..fast attack (smooth, monotonic): 4 dB -> halfway.
            // Inside a feedback loop the target is k·(L_y - T); the equivalent feed-forward
            // overshoot is (target - r)/(1 + k) (exact above the knee), so program dependence
            // reacts to the same musical overshoot in both topologies.
            const double overshoot = (target - in.fast) * overshootScale;
            const double w = overshoot / (overshoot + 4.0);
            const double cd = attackCoeffSlow + (attackCoeffFast - attackCoeffSlow) * w;
            const double c = loopK > 0.0 ? cd * (1.0 + loopK) / (1.0 + cd * loopK) : cd;
            out.fast = c * in.fast + (1.0 - c) * target;
            out.hold = holdSamples;
        }
        else if (in.hold > 0)
        {
            // hold: output independent of the target here, so H(r) in the feedback solver stays
            // continuous and non-increasing (the branches meet at target == fast)
            out.fast = in.fast;
            out.hold = in.hold - 1;
        }
        else
        {
            out.fast = fastReleaseCoeff * in.fast + (1.0 - fastReleaseCoeff) * target;
            out.hold = 0;
        }
        const double cs = out.fast > in.slow ? slowAttackCoeff : slowReleaseCoeff;
        out.slow = cs * in.slow + (1.0 - cs) * out.fast;
        out.rc = rcCoeff * in.rc + (1.0 - rcCoeff) * std::max (out.fast, out.slow);
        return out.rc;
    }

    inline double peek (double target) const noexcept
    {
        State tmp;
        return evaluate (target, state, tmp);
    }

    /** Envelope output *before* the RC rounding stage, without advancing the state. The
        zero-delay feedback loop is closed around this value: keeping the 0.35 ms RC pole outside
        the loop leaves a single-pole loop (no overshoot from loop gain), while the applied gain
        still gets the RC rounding. The RC stage has unity DC gain, so statics are unaffected. */
    inline double peekBeforeRc (double target) const noexcept
    {
        State tmp;
        evaluate (target, state, tmp);
        return std::max (tmp.fast, tmp.slow);
    }

    double getOutputBeforeRc() const noexcept { return std::max (state.fast, state.slow); }

    inline double process (double target) noexcept
    {
        State next;
        const double r = evaluate (target, state, next);
        state.fast = flushDenormal (next.fast);
        state.slow = flushDenormal (next.slow);
        state.rc = flushDenormal (next.rc);
        state.hold = next.hold;
        return r;
    }

    double getOutput() const noexcept { return state.rc; }

private:
    double fs = 48000.0, overshootScale = 1.0, loopK = 0.0, releaseScale = 1.0;
    BallisticsSettings settings;
    int holdSamples = 0;
    double attackCoeffSlow = 0.0, attackCoeffFast = 0.0;
    double fastReleaseCoeff = 0.0, slowAttackCoeff = 0.0, slowReleaseCoeff = 0.0, rcCoeff = 0.0;
    State state;
};

} // namespace lumen
