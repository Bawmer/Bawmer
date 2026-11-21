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

        switch (mode)
        {
            case DetectionMode::peak:
                return 10.0 * std::log10 (sq + 1.0e-30);
            case DetectionMode::rms:
                return 10.0 * std::log10 (2.0 * meanSquare + 1.0e-30);
            case DetectionMode::hybrid:
            {
                // RMS body; peaks only take over when they exceed the body by more than 6 dB
                // of crest factor, so transients are caught without RMS-style overshoot.
                const double rmsDb = 10.0 * std::log10 (2.0 * meanSquare + 1.0e-30);
                const double peakDb = 10.0 * std::log10 (sq + 1.0e-30) - 6.0;
                return std::max (rmsDb, peakDb);
            }
        }
        return kMinusInfDb;
    }

private:
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
      a continuous slope (fewer sidebands than a one-pole corner). */
class AnalogEnvelope
{
public:
    void prepare (double sampleRate) noexcept
    {
        fs = sampleRate;
        rcCoeff = timeConstantToCoeff (0.35, fs);
        reset();
    }

    void reset() noexcept
    {
        fast = slow = rc = 0.0;
    }

    void configure (const BallisticsSettings& s, double minCoeffForStability = 0.0) noexcept
    {
        attackMs = s.attackMs;
        minCoeff = minCoeffForStability;
        fastReleaseCoeff = timeConstantToCoeff (std::max (8.0, s.releaseMs * (s.autoRelease ? 0.1 : 0.25)), fs);
        slowAttackCoeff = timeConstantToCoeff (std::max (120.0, s.releaseMs * 0.6), fs);
        slowReleaseCoeff = timeConstantToCoeff (s.releaseMs, fs);
        fastReleaseCoeff = std::max (fastReleaseCoeff, minCoeff);
        // attack coefficients for the program-dependent range, interpolated per sample
        attackCoeffSlow = std::max (minCoeff, timeConstantToCoeff (attackMs * 3.0, fs));
        attackCoeffFast = std::max (minCoeff, timeConstantToCoeff (attackMs * 0.5, fs));
    }

    inline double process (double target) noexcept
    {
        if (target > fast)
        {
            // map overshoot 0..12 dB onto slow..fast attack (smooth, monotonic)
            const double overshoot = target - fast;
            const double w = overshoot / (overshoot + 4.0); // 0 -> 0, 4 dB -> 0.5, 12 dB -> 0.75
            const double c = attackCoeffSlow + (attackCoeffFast - attackCoeffSlow) * w;
            fast = flushDenormal (c * fast + (1.0 - c) * target);
        }
        else
        {
            fast = flushDenormal (fastReleaseCoeff * fast + (1.0 - fastReleaseCoeff) * target);
        }

        const double cs = fast > slow ? slowAttackCoeff : slowReleaseCoeff;
        slow = flushDenormal (cs * slow + (1.0 - cs) * fast);

        const double combined = std::max (fast, slow);
        rc = flushDenormal (rcCoeff * rc + (1.0 - rcCoeff) * combined);
        return rc;
    }

private:
    double fs = 48000.0, attackMs = 10.0, minCoeff = 0.0;
    double attackCoeffSlow = 0.0, attackCoeffFast = 0.0;
    double fastReleaseCoeff = 0.0, slowAttackCoeff = 0.0, slowReleaseCoeff = 0.0, rcCoeff = 0.0;
    double fast = 0.0, slow = 0.0, rc = 0.0;
};

} // namespace lumen
