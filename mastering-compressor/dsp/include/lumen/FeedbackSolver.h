// Lumen MC-64 — zero-delay feedback (ZDF) solver for the feedback compressor topology.
//
// In a feedback compressor the detector sees the output. Discretised naively, the loop needs a
// one-sample delay (detector reads y[n-1]), which makes the loop gain k = R - 1 act on stale
// data: it is only stable when the envelope coefficient satisfies a > (k - 1)/(k + 1), so fast
// attacks have to be clamped at high ratios, and the delay adds phase error that grows with
// sample period.
//
// Instead we solve the instantaneous loop each sample. With u = the (delayed) input sample and
// r the unknown reduction in dB:
//
//     y(r)  = u · 10^(-(r0 + w·r)/20)                 (w, r0 from hybrid blending; w = 1, r0 = 0 in Analog mode)
//     d(r)  = A·y(r) + B                              (sidechain filters are linear: A, B from peek())
//     L(r)  = level detector(d)                       (peak / RMS / hybrid, state not advanced)
//     H(r)  = envelope( -G_fb(L(r)) )                 (envelope evaluated, state not advanced)
//     f(r)  = r - H(r) = 0
//
// Every stage of H is non-increasing in r (more reduction -> quieter output -> lower level ->
// lower target -> lower envelope output), so f is strictly increasing: the root is unique, and
//     f(0) = -H(0) <= 0,      f(H(0)) = H(0) - H(H(0)) >= 0
// brackets it. We solve with the Illinois variant of regula falsi (super-linear, never leaves
// the bracket), seeded with the previous sample's solution, with a hard iteration cap for
// real-time safety. f is only piecewise smooth (attack/release branch, knee edges), which is
// why a bracketing method is used rather than plain Newton–Raphson.
#pragma once

#include "DspCommon.h"

namespace lumen
{
struct FeedbackSolver
{
    static constexpr int kMaxIterations = 24;
    static constexpr double kTolerance = 1.0e-9; // dB

    struct Result
    {
        double r = 0.0;
        int evaluations = 0;
    };

    /** H: callable double(double r) -> envelope output for candidate reduction r (non-increasing). */
    template <typename Fn>
    static Result solve (Fn&& H, double guess) noexcept
    {
        Result res;
        const double h0 = H (0.0);
        res.evaluations = 1;
        if (h0 <= kTolerance)
        {
            res.r = std::max (0.0, h0);
            return res;
        }

        double a = 0.0, fa = -h0;
        double b = h0, fb = b - H (b);
        ++res.evaluations;
        if (fb <= kTolerance)
        {
            res.r = b;
            return res;
        }

        // tighten the bracket with the previous solution (usually very close)
        if (guess > a && guess < b)
        {
            const double fg = guess - H (guess);
            ++res.evaluations;
            if (std::abs (fg) <= kTolerance)
            {
                res.r = guess;
                return res;
            }
            if (fg < 0.0) { a = guess; fa = fg; }
            else          { b = guess; fb = fg; }
        }

        int side = 0;
        double c = 0.5 * (a + b);
        for (int it = 0; it < kMaxIterations; ++it)
        {
            c = (a * fb - b * fa) / (fb - fa);
            if (! (c > a && c < b)) // numerical safety: fall back to bisection
                c = 0.5 * (a + b);
            const double fc = c - H (c);
            ++res.evaluations;
            if (std::abs (fc) <= kTolerance || (b - a) <= kTolerance)
                break;
            if (fc < 0.0)
            {
                a = c; fa = fc;
                if (side == -1) fb *= 0.5; // Illinois: halve the stale endpoint
                side = -1;
            }
            else
            {
                b = c; fb = fc;
                if (side == 1) fa *= 0.5;
                side = 1;
            }
        }
        res.r = c;
        return res;
    }
};

} // namespace lumen
