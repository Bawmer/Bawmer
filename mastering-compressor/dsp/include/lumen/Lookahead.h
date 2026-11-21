// Lumen MC-64 — lookahead gain-reduction shaping and click-free delay changes.
//
// Exact-onset lookahead
// ---------------------
// Audio is delayed by L samples, so the gain computed at time n is applied to x[n - L].
// Let t[n] >= 0 be the per-sample reduction target (dB) of the *undelayed* sidechain.
//
//   1. Sliding maximum over L + 1 samples:   m[n] = max_{0<=i<=L} t[n - i]
//   2. Smoothing kernel h of length W <= L + 1, h[j] >= 0, sum h = 1:
//                                            s[n] = sum_{j<W} h[j]·m[n - j]
//
// Claim: s[n] >= t[n - L] for every n (the sample being output is never under-compressed).
// Proof: for every 0 <= j < W <= L + 1, the window of m[n - j] is [n - j - L, n - j], which
// contains n - L because 0 <= j <= L. Hence m[n - j] >= t[n - L] for all j in the kernel, and
// s[n] = sum h[j]·m[n - j] >= t[n - L]·sum h[j] = t[n - L].                                    ∎
//
// Downstream ballistics may only *add* reduction (the engine applies r = max(envelope, s)), so
// the guarantee survives release smoothing, analog RC stages, hybrid blending (a convex
// combination of values >= t) and stereo linking (moves r towards the larger reduction).
//
// h is a triangle built from two cascaded moving averages of lengths a and b (a + b - 1 = W).
// Its step response is a piecewise-quadratic S-curve (C¹), each moving average costs O(1) per
// sample, and a running sum with Kahan compensation keeps it drift-free indefinitely.
#pragma once

#include "DspCommon.h"

namespace lumen
{
/** Moving average with fixed capacity, O(1) per sample, compensated running sum. */
template <int MaxLength>
class MovingAverage
{
public:
    void setLength (int n) noexcept
    {
        length = clampValue (n, 1, MaxLength);
        reset();
    }

    void reset() noexcept
    {
        std::fill (std::begin (buffer), std::end (buffer), 0.0);
        pos = 0;
        sum = comp = 0.0;
        primed = false;
    }

    /** First call fills the history with x, so a held signal passes through unchanged. */
    inline double process (double x) noexcept
    {
        if (length == 1)
            return x;
        if (! primed)
        {
            for (int i = 0; i < length; ++i)
                buffer[static_cast<size_t> (i)] = x;
            sum = x * length;
            comp = 0.0;
            primed = true;
        }
        const double old = buffer[static_cast<size_t> (pos)];
        buffer[static_cast<size_t> (pos)] = x;
        pos = (pos + 1 == length) ? 0 : pos + 1;
        // Kahan-compensated sum += (x - old)
        const double y = (x - old) - comp;
        const double t = sum + y;
        comp = (t - sum) - y;
        sum = t;
        return sum / length;
    }

private:
    double buffer[MaxLength] {};
    int length = 1, pos = 0;
    double sum = 0.0, comp = 0.0;
    bool primed = false;
};

/** Reduction-target shaper for lookahead. classic: sliding max only (the attack ballistics then
    start up to L samples early). exact: sliding max + triangular kernel (see proof above). */
template <int MaxLookahead>
class LookaheadShaper
{
public:
    void configure (int lookaheadSamples, int kernelLength, bool exactMode) noexcept
    {
        lookahead = clampValue (lookaheadSamples, 0, MaxLookahead - 1);
        exact = exactMode && lookahead > 0;
        const int W = clampValue (kernelLength, 1, lookahead + 1);
        const int a = (W + 1) / 2;
        const int b = W - a + 1; // a + b - 1 = W
        slidingMax.setWindow (lookahead + 1);
        slidingMax.reset();
        boxA.setLength (a);
        boxB.setLength (b);
        kernel = W;
    }

    void reset() noexcept
    {
        slidingMax.reset();
        boxA.reset();
        boxB.reset();
    }

    bool isExact() const noexcept { return exact; }
    int getKernelLength() const noexcept { return kernel; }

    inline double process (double target) noexcept
    {
        const double m = slidingMax.process (target);
        if (! exact)
            return m;
        return boxB.process (boxA.process (m));
    }

private:
    SlidingMax<MaxLookahead> slidingMax;
    MovingAverage<MaxLookahead> boxA, boxB;
    int lookahead = 0, kernel = 1;
    bool exact = false;
};

/** Delay line whose delay can change while running: the old and new taps are crossfaded with an
    equal-gain raised-cosine over fadeSamples, so lookahead / latency changes do not click. */
template <int MaxSamples>
class CrossfadingDelay
{
public:
    void clear() noexcept
    {
        std::fill (std::begin (buffer), std::end (buffer), 0.0);
        writePos = 0;
        fadePos = fadeLength = 0;
    }

    void setFadeLength (int samples) noexcept { fadeSamples = std::max (1, samples); }

    void setDelay (int samples, bool immediate = false) noexcept
    {
        samples = clampValue (samples, 0, MaxSamples - 1);
        if (samples == delay && fadeLength == 0)
            return;
        if (immediate)
        {
            delay = oldDelay = samples;
            fadeLength = fadePos = 0;
            return;
        }
        oldDelay = currentReadDelay();
        delay = samples;
        fadeLength = fadeSamples;
        fadePos = 0;
    }

    int getDelay() const noexcept { return delay; }

    inline double process (double x) noexcept
    {
        buffer[static_cast<size_t> (writePos)] = x;
        double y = read (delay);
        if (fadeLength > 0)
        {
            const double t = static_cast<double> (fadePos) / fadeLength;
            const double w = 0.5 - 0.5 * std::cos (kPi * t); // 0 -> 1
            y = read (oldDelay) + w * (y - read (oldDelay));
            if (++fadePos >= fadeLength)
                fadeLength = fadePos = 0;
        }
        writePos = (writePos + 1 == MaxSamples) ? 0 : writePos + 1;
        return y;
    }

private:
    inline double read (int d) const noexcept
    {
        int p = writePos - d;
        if (p < 0)
            p += MaxSamples;
        return buffer[static_cast<size_t> (p)];
    }

    int currentReadDelay() const noexcept { return fadeLength > 0 && fadePos * 2 < fadeLength ? oldDelay : delay; }

    double buffer[MaxSamples] {};
    int writePos = 0, delay = 0, oldDelay = 0, fadePos = 0, fadeLength = 0, fadeSamples = 960;
};

} // namespace lumen
