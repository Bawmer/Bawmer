// Lumen MC-64 — true-peak (inter-sample peak) estimation, after ITU-R BS.1770-4 Annex 2.
//
// The signal is interpolated 4x with a 145-tap linear-phase Kaiser-windowed sinc (cutoff at the
// original Nyquist, beta 7). In polyphase form:
//
//     y_p[n] = sum_j h[4j + p] · x[n - j],     p = 0..3,  centre c = 72
//
// h has zeros at every 4th tap from the centre, so phase 0 is a pure delay (y_0[n] = x[n - 18]):
// original samples are preserved exactly. Phases 1..3 are the points 1/4, 2/4, 3/4 of the way
// from x[n-18] to x[n-17]. The true-peak estimate at n is max(|y_0|..|y_3|), describing the
// interval that started D = 18 samples ago; the engine delays the audio by the same amount.
//
// Why 145 taps and not the 49 of the BS.1770 reference filter: measured against an *ideal*
// interpolator on the same 4x grid (dense frequency x phase scan), the 49-tap design under-reads
// by up to 0.34 dB between 0.35·fs and 0.45·fs, because a filter that short cannot pass 0.45·fs
// and still reject the image at 0.55·fs. 145 taps bring that to <= 0.006 dB up to 0.45·fs for
// 0.375 ms of extra latency at 48 kHz (193 taps gain < 0.003 dB more).
//
// Error bound. Even with an ideal interpolator, a 4x grid can miss the true crest of a sine by up
// to half a grid step (1/8 sample), i.e. it under-reads by at most
//     -20·log10(cos(2π·f/fs · 1/8))  dB      (0.38 dB at 0.375·fs, 0.55 dB at 0.45·fs).
// That is the documented limit of 4x true-peak meters; the test suite checks that this
// implementation adds no more than 0.15 dB to it (filter truncation) and never over-reads
// by more than 0.1 dB (passband ripple).
#pragma once

#include "DspCommon.h"

namespace lumen
{
class TruePeakInterpolator
{
public:
    static constexpr int kNumTaps = 145;
    static constexpr int kPhases = 4;
    static constexpr int kTapsPerPhase = (kNumTaps + kPhases - 1) / kPhases; // 37
    static constexpr int kDelay = (kNumTaps - 1) / 2 / kPhases;             // 18

    TruePeakInterpolator() noexcept { design (7.0); }

    /** Kaiser-windowed sinc; each phase normalised to unity DC gain. */
    void design (double kaiserBeta) noexcept
    {
        const int c = (kNumTaps - 1) / 2;
        double h[kNumTaps];
        const double i0b = besselI0 (kaiserBeta);
        for (int k = 0; k < kNumTaps; ++k)
        {
            const double t = (k - c) / static_cast<double> (kPhases);
            const double sinc = (k == c) ? 1.0 : std::sin (kPi * t) / (kPi * t);
            const double r = 2.0 * k / (kNumTaps - 1) - 1.0;
            h[k] = sinc * besselI0 (kaiserBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
        }
        for (int p = 0; p < kPhases; ++p)
        {
            double sum = 0.0;
            for (int j = 0; j < kTapsPerPhase; ++j)
            {
                const int k = 4 * j + p;
                coeffs[p][j] = k < kNumTaps ? h[k] : 0.0;
                sum += coeffs[p][j];
            }
            for (int j = 0; j < kTapsPerPhase; ++j)
                coeffs[p][j] /= sum;
        }
        reset();
    }

    void reset() noexcept
    {
        std::fill (std::begin (history), std::end (history), 0.0);
        pos = 0;
    }

    /** Push one sample; returns the true-peak magnitude of the interval starting kDelay samples ago. */
    inline double process (double x) noexcept
    {
        pos = (pos == 0) ? kTapsPerPhase - 1 : pos - 1;
        history[pos] = x;
        history[pos + kTapsPerPhase] = x;
        const double* hx = history + pos; // hx[j] = x[n - j]

        double peak = std::abs (hx[kDelay]); // phase 0: the original sample
        for (int p = 1; p < kPhases; ++p)
        {
            double acc = 0.0;
            for (int j = 0; j < kTapsPerPhase; ++j)
                acc += coeffs[p][j] * hx[j];
            peak = std::max (peak, std::abs (acc));
        }
        return peak;
    }

    /** Theoretical worst-case under-read of an ideal 4x grid for a sine at f (Hz), in dB (>= 0). */
    static double gridBoundDb (double f, double fs) noexcept
    {
        return -20.0 * std::log10 (std::cos (2.0 * kPi * f / fs * 0.125));
    }

private:
    static double besselI0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        const double y = 0.25 * x * x;
        for (int i = 1; i < 64; ++i)
        {
            term *= y / (static_cast<double> (i) * i);
            sum += term;
            if (term < sum * 1.0e-17)
                break;
        }
        return sum;
    }

    double coeffs[kPhases][kTapsPerPhase] {};
    double history[2 * kTapsPerPhase] {};
    int pos = 0;
};

} // namespace lumen
