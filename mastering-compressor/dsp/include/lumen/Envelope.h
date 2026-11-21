// Lumen MC-64 — level detection and gain-reduction ballistics.
//
// All envelopes run on gain *reduction* in dB (r >= 0). Smoothing in the log domain gives
// release curves that fall at a constant dB/s rate — the behaviour engineers expect from
// mastering compressors — and keeps the static curve exact once the envelope settles.
#pragma once

#include "DspCommon.h"

namespace lumen
{
enum class DetectionMode { peak = 0, rms = 1, hybrid = 2 };

/** Converts a (sidechain-filtered) sample into a level in dB.
    RMS is AES-17 calibrated (+3.01 dB) so a steady sine reads the same in peak and RMS modes,
    which keeps the threshold meaning identical when the user switches detectors. */
class LevelDetector
{
public:
    void prepare (double sampleRate, double rmsWindowMs = 10.0) noexcept
    {
        rmsCoeff = timeConstantToCoeff (rmsWindowMs, sampleRate);
        reset();
    }

    void reset() noexcept { meanSquare = 0.0; }

    void setMode (DetectionMode m) noexcept { mode = m; }

    inline double process (double x) noexcept
    {
        const double sq = x * x;
        meanSquare = flushDenormal (rmsCoeff * meanSquare + (1.0 - rmsCoeff) * sq);
        return levelFrom (sq, meanSquare);
    }

    /** Level for input x without advancing the RMS state. */
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
                return 10.0 * std::log10 (sq + 1.0e-30);
            case DetectionMode::rms:
                return 10.0 * std::log10 (2.0 * ms + 1.0e-30);
            case DetectionMode::hybrid:
            {
                // RMS body; peaks only take over when they exceed the body by more than 6 dB
                // of crest factor, so transients are caught without RMS-style overshoot.
                const double rmsDb = 10.0 * std::log10 (2.0 * ms + 1.0e-30);
                const double peakDb = 10.0 * std::log10 (sq + 1.0e-30) - 6.0;
                return std::max (rmsDb, peakDb);
            }
        }
        return kMinusInfDb;
    }

    DetectionMode mode = DetectionMode::peak;
    double rmsCoeff = 0.0, meanSquare = 0.0;
};

/** Timing for an envelope. */
struct BallisticsSettings
{
    double attackMs = 10.0;
    double releaseMs = 200.0;
    bool autoRelease = false;
};

/** Digital ballistics: smooth branching one-pole in the dB domain (Giannoulis et al.), with an
    optional program-dependent dual release:
      - a fast branch recovers quickly after short transients,
      - a slow branch charges only under sustained compression and then sets the recovery,
    output = max(fast, slow). Short peaks release fast, dense passages release slowly — minimal
    pumping on transients, no breathing on sustained material. */
class DigitalEnvelope
{
public:
    void prepare (double sampleRate) noexcept
    {
        fs = sampleRate;
        reset();
    }

    void reset() noexcept { fast = slow = 0.0; }

    void configure (const BallisticsSettings& s) noexcept
    {
        attackCoeff = timeConstantToCoeff (s.attackMs, fs);
        auto_ = s.autoRelease;
        if (auto_)
        {
            fastReleaseCoeff = timeConstantToCoeff (std::max (5.0, s.releaseMs * 0.1), fs);
            slowAttackCoeff = timeConstantToCoeff (std::max (100.0, s.releaseMs * 0.5), fs);
            slowReleaseCoeff = timeConstantToCoeff (s.releaseMs, fs);
        }
        else
        {
            fastReleaseCoeff = timeConstantToCoeff (s.releaseMs, fs);
        }
    }

    inline double process (double target) noexcept
    {
        const double c = target > fast ? attackCoeff : fastReleaseCoeff;
        fast = flushDenormal (c * fast + (1.0 - c) * target);

        if (! auto_)
            return fast;

        const double cs = fast > slow ? slowAttackCoeff : slowReleaseCoeff;
        slow = flushDenormal (cs * slow + (1.0 - cs) * fast);
        return std::max (fast, slow);
    }

    double getState() const noexcept { return auto_ ? std::max (fast, slow) : fast; }

private:
    double fs = 48000.0;
    double attackCoeff = 0.0, fastReleaseCoeff = 0.0, slowAttackCoeff = 0.0, slowReleaseCoeff = 0.0;
    double fast = 0.0, slow = 0.0;
    bool auto_ = false;
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
    };

    void prepare (double sampleRate) noexcept
    {
        fs = sampleRate;
        reset();
        configure (BallisticsSettings {}, 0.0);
    }

    void reset() noexcept { state = State {}; }

    /** loopGain = R - 1 when the envelope sits inside a feedback loop, else 0. */
    void configure (const BallisticsSettings& s, double loopGain) noexcept
    {
        overshootScale = 1.0 / (1.0 + std::max (0.0, loopGain));
        auto c = [&] (double ms) {
            const double cd = timeConstantToCoeff (ms, fs);
            return loopGain > 0.0 ? cd * (1.0 + loopGain) / (1.0 + cd * loopGain) : cd;
        };
        // The RC rounding pole is *not* pre-warped: inside a loop of gain k it must stay much
        // faster than the envelope pole, or the pair becomes underdamped (ringing gain).
        rcCoeff = timeConstantToCoeff (0.35, fs);
        fastReleaseCoeff = c (std::max (8.0, s.releaseMs * (s.autoRelease ? 0.1 : 0.25)));
        slowAttackCoeff = c (std::max (120.0, s.releaseMs * 0.6));
        slowReleaseCoeff = c (s.releaseMs);
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
        }
        else
        {
            out.fast = fastReleaseCoeff * in.fast + (1.0 - fastReleaseCoeff) * target;
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
        return r;
    }

    double getOutput() const noexcept { return state.rc; }

private:
    double fs = 48000.0, overshootScale = 1.0, loopK = 0.0;
    double attackCoeffSlow = 0.0, attackCoeffFast = 0.0;
    double fastReleaseCoeff = 0.0, slowAttackCoeff = 0.0, slowReleaseCoeff = 0.0, rcCoeff = 0.0;
    State state;
};

} // namespace lumen
