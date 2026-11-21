// Lumen MC-64 — static compression curves (log domain).
//
// Feed-forward curve: the quadratic soft knee of Giannoulis, Massberg & Reiss,
// "Digital Dynamic Range Compressor Design — A Tutorial and Analysis" (JAES 2012).
//
// Feedback curve: the detector sees the compressor's *output*. For the closed loop to settle on
// the same static curve as the feed-forward version, the gain applied for an output overshoot o
// must be -(R - 1)·o instead of -(1 - 1/R)·o_in:
//     y = x - (R - 1)(y - T)  =>  y = T + (x - T) / R.
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

    /** Feedback-topology gain (dB, <= 0) from the *output* level. */
    static double feedBack (double outputLevelDb, double thresholdDb, double ratio, double kneeDb) noexcept
    {
        const double slope = 1.0 - ratio; // <= 0
        const double over = outputLevelDb - thresholdDb;

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
};

} // namespace lumen
