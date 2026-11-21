// Lumen MC-64 — numerically stable filters.
//
// The state-variable filter is the trapezoidal (TPT / zero-delay-feedback) topology described by
// A. Simper (Cytomic). Unlike direct-form biquads it stays well conditioned at very low cutoff
// relative to the sample rate (20 Hz at 384 kHz) and tolerates per-sample coefficient changes.
#pragma once

#include "DspCommon.h"

namespace lumen
{
class Svf
{
public:
    enum class Type { lowpass, highpass, bandpass };

    void setup (Type t, double cutoffHz, double q, double sampleRate) noexcept
    {
        type = t;
        const double fc = clampValue (cutoffHz, 1.0, sampleRate * 0.49);
        const double g = std::tan (kPi * fc / sampleRate);
        k = 1.0 / q;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    void reset() noexcept { ic1 = ic2 = 0.0; }

    /** Returns all three responses at once; process() picks the configured one. */
    inline void tick (double v0, double& lp, double& bp, double& hp) noexcept
    {
        const double v3 = v0 - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = flushDenormal (2.0 * v1 - ic1);
        ic2 = flushDenormal (2.0 * v2 - ic2);
        lp = v2;
        bp = v1;
        hp = v0 - k * v1 - v2;
    }

    /** Output for input x *without* advancing the state (used by the zero-delay feedback solver;
        the filter is linear, so peek(x) = peek(0) + x·(peek(1) - peek(0))). */
    inline double peek (double v0) const noexcept
    {
        const double v3 = v0 - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        switch (type)
        {
            case Type::lowpass:  return v2;
            case Type::highpass: return v0 - k * v1 - v2;
            case Type::bandpass: return v1;
        }
        return v2;
    }

    inline double process (double x) noexcept
    {
        double lp, bp, hp;
        tick (x, lp, bp, hp);
        switch (type)
        {
            case Type::lowpass:  return lp;
            case Type::highpass: return hp;
            case Type::bandpass: return bp;
        }
        return lp;
    }

private:
    Type type = Type::lowpass;
    double k = 1.4142135623730951, a1 = 0.0, a2 = 0.0, a3 = 0.0;
    double ic1 = 0.0, ic2 = 0.0;
};

/** First-order TPT filter (lowpass/highpass outputs). */
class OnePole
{
public:
    void setup (double cutoffHz, double sampleRate) noexcept
    {
        const double fc = clampValue (cutoffHz, 0.1, sampleRate * 0.49);
        const double g = std::tan (kPi * fc / sampleRate);
        G = g / (1.0 + g);
    }

    void reset() noexcept { s = 0.0; }

    inline double lowpass (double x) noexcept
    {
        const double v = (x - s) * G;
        const double y = v + s;
        s = flushDenormal (y + v);
        return y;
    }

    inline double highpass (double x) noexcept { return x - lowpass (x); }

private:
    double G = 0.0, s = 0.0;
};

/** Shelving tilt built from an SVF: y = x + (gLow - 1)·LP(x) + (gHigh - 1)·HP(x).
    Used as the frequency-dependent detector of the analog engine (less LF, slightly more HF
    sensitivity), which avoids bass-driven pumping on full mixes. */
class DetectorTilt
{
public:
    void setup (double lowShelfHz, double lowGainDb, double highShelfHz, double highGainDb, double sampleRate) noexcept
    {
        low.setup (Svf::Type::lowpass, lowShelfHz, 0.7071067811865476, sampleRate);
        high.setup (Svf::Type::highpass, highShelfHz, 0.7071067811865476, sampleRate);
        gLow = dbToGain (lowGainDb) - 1.0;
        gHigh = dbToGain (highGainDb) - 1.0;
    }

    void reset() noexcept
    {
        low.reset();
        high.reset();
    }

    inline double process (double x) noexcept
    {
        return x + gLow * low.process (x) + gHigh * high.process (x);
    }

    inline double peek (double x) const noexcept { return x + gLow * low.peek (x) + gHigh * high.peek (x); }

private:
    Svf low, high;
    double gLow = 0.0, gHigh = 0.0;
};

/** Approximate ITU-R BS.1770 K-weighting (high-pass ~60 Hz + high shelf +4 dB above ~1.7 kHz).
    Only used for the loudness-matched bypass comparison. */
class KWeighting
{
public:
    void setup (double sampleRate) noexcept
    {
        hp.setup (Svf::Type::highpass, 60.0, 0.5, sampleRate);
        shelf.setup (Svf::Type::highpass, 1700.0, 0.7071067811865476, sampleRate);
        shelfGain = dbToGain (4.0) - 1.0;
    }

    void reset() noexcept
    {
        hp.reset();
        shelf.reset();
    }

    inline double process (double x) noexcept
    {
        const double h = hp.process (x);
        return h + shelfGain * shelf.process (h);
    }

private:
    Svf hp, shelf;
    double shelfGain = 0.0;
};

} // namespace lumen
