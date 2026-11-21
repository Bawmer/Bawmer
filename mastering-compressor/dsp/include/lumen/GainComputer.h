// Lumen MC-64 — static compression curves (log domain).
//
// Feed-forward curve: the quadratic soft knee of Giannoulis, Massberg & Reiss,
// "Digital Dynamic Range Compressor Design — A Tutorial and Analysis" (JAES 2012).
//
// Feedback curve: the detector sees the compressor's *output*. The gain law must make the closed
// loop settle exactly on the feed-forward static curve G_ff, for every level including the knee:
//     given output level y, find the input x with  x + G_ff(x) = y,  then  G_fb(y) = G_ff(x).
// The feed-forward output curve is strictly increasing, so the inverse exists; for the quadratic
// knee it has a closed form (see feedBack()). Above the knee this reduces to the classic
// G_fb(y) = -(R - 1)(y - T).
#pragma once

#include "DspCommon.h"

namespace lumen
{
struct GainComputer
{
    /** Gain (dB, <= 0) for an input level in dB. Returns exactly 0.0 below the knee and for R == 1. */
    static double feedForward (double levelDb, double thresholdDb, double ratio, double kneeDb) noexcept
    {
        const double slope = 1.0 / ratio - 1.0; // <= 0
        const double over = levelDb - thresholdDb;

        if (kneeDb <= 1.0e-9)
            return over > 0.0 ? slope * over : 0.0;

        const double halfKnee = 0.5 * kneeDb;
        if (over <= -halfKnee)
            return 0.0;
        if (over >= halfKnee)
            return slope * over;
        const double t = over + halfKnee;
        return slope * t * t / (2.0 * kneeDb);
    }

    /** Output level for an input level (what the transfer-curve display draws). */
    static double staticOutput (double levelDb, double thresholdDb, double ratio, double kneeDb) noexcept
    {
        return levelDb + feedForward (levelDb, thresholdDb, ratio, kneeDb);
    }

    /** Inverse of staticOutput(): the input level that produces a given output level. */
    static double inverseStaticOutput (double outputDb, double thresholdDb, double ratio, double kneeDb) noexcept
    {
        const double slope = 1.0 / ratio - 1.0; // s <= 0
        if (slope == 0.0)
            return outputDb;
        if (kneeDb <= 1.0e-9)
            return outputDb <= thresholdDb ? outputDb : thresholdDb + (outputDb - thresholdDb) * ratio;

        const double halfKnee = 0.5 * kneeDb;
        if (outputDb <= thresholdDb - halfKnee)
            return outputDb;
        const double kneeTopOut = thresholdDb + halfKnee / ratio; // output at x = T + W/2
        if (outputDb >= kneeTopOut)
            return thresholdDb + (outputDb - thresholdDb) * ratio;

        // Inside the knee, with t = x - T + W/2 in [0, W] and q = y - T + W/2:
        //     q = t + a·t²,  a = s / (2W) < 0   =>   t = 2q / (1 + sqrt(1 + 4aq))
        // (the rationalised root avoids cancellation as a -> 0). The discriminant at the knee
        // top is (1 + s)² >= 0, so it stays real over the whole knee.
        const double a = slope / (2.0 * kneeDb);
        const double q = outputDb - thresholdDb + halfKnee;
        const double disc = std::max (0.0, 1.0 + 4.0 * a * q);
        const double t = 2.0 * q / (1.0 + std::sqrt (disc));
        return thresholdDb - halfKnee + t;
    }

    /** Feedback-topology gain (dB, <= 0) from the *output* level; exact for every level. */
    static double feedBack (double outputLevelDb, double thresholdDb, double ratio, double kneeDb) noexcept
    {
        const double x = inverseStaticOutput (outputLevelDb, thresholdDb, ratio, kneeDb);
        return outputLevelDb - x; // = G_ff(x)
    }
};

} // namespace lumen
