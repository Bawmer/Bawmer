// Lumen MC-64 — 2x/4x/8x/16x oversampling with cascaded polyphase half-band FIR filters.
//
// Each stage is a linear-phase Kaiser-windowed half-band (N = 4k + 3 taps). Half of the taps are
// exactly zero, so the polyphase form costs (k + 1) multiplies per output sample and the
// "centre" phase is a pure delay. All designs for both quality settings are computed once in
// prepare(), so switching factor/quality on the audio thread only resets state (no allocation).
//
// Latency: every stage adds (2k + 1) samples at its input rate. Stages above the first run at
// 2x, 4x... the base rate, so their latency can be fractional at the base rate. A small integer
// delay is inserted at the highest rate so the *total* latency is always a whole number of base
// samples — the plugin can then report it exactly and the dry path stays sample-aligned.
#pragma once

#include "DspCommon.h"

namespace lumen
{
enum class OversamplingQuality { linearPhaseHQ = 0, lowLatency = 1 };

class HalfbandDesign
{
public:
    static constexpr int kMaxTaps = 64; // non-zero polyphase taps (k + 1)

    /** Kaiser-windowed half-band with numTaps = 4k + 3. */
    void design (int numTaps, double kaiserBeta) noexcept
    {
        const int N = numTaps;
        const int centre = (N - 1) / 2;
        k = (N - 3) / 4;
        polyTaps = (N + 1) / 2; // even-index taps = 2k + 2
        polyTaps = clampValue (polyTaps, 1, kMaxTaps);

        double sum = 0.0;
        const double i0b = besselI0 (kaiserBeta);
        for (int q = 0; q < polyTaps; ++q)
        {
            const int n = 2 * q; // even indices hold the non-zero sinc taps (centre is odd)
            const double t = static_cast<double> (n - centre);
            const double sinc = std::sin (0.5 * kPi * t) / (kPi * t); // 0.5·sinc(t/2)
            const double r = 2.0 * n / (N - 1) - 1.0;
            const double w = besselI0 (kaiserBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
            taps[q] = sinc * w;
            sum += taps[q];
        }
        // normalise so DC gain is exactly 1: non-centre taps must sum to 0.5
        const double scale = 0.5 / sum;
        for (int q = 0; q < polyTaps; ++q)
            taps[q] *= scale;
        latencyAtInputRate = 2 * k + 1;
    }

    double taps[kMaxTaps] {};
    int polyTaps = 0;
    int k = 0;
    int latencyAtInputRate = 0;

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
};

/** Ring-buffer history with mirrored storage so FIR loops never wrap. */
template <int Size>
struct FirHistory
{
    double data[2 * Size] {};
    int pos = 0;

    void clear() noexcept
    {
        std::fill (std::begin (data), std::end (data), 0.0);
        pos = 0;
    }

    /** push newest sample; returns pointer p where p[0] is newest, p[1] previous, ... */
    inline const double* push (double x) noexcept
    {
        pos = (pos == 0) ? Size - 1 : pos - 1;
        data[pos] = x;
        data[pos + Size] = x;
        return data + pos;
    }

    inline double at (int age) const noexcept { return data[pos + age]; } // age < Size
};

class HalfbandStage
{
public:
    void setDesign (const HalfbandDesign* d) noexcept
    {
        design = d;
        reset();
    }

    void reset() noexcept
    {
        upHist.clear();
        downEven.clear();
        downOdd.clear();
    }

    /** One input sample -> two output samples (gain-corrected interpolation). */
    inline void upsample (double x, double& out0, double& out1) noexcept
    {
        const double* h = upHist.push (x);
        double acc = 0.0;
        for (int q = 0; q < design->polyTaps; ++q)
            acc += design->taps[q] * h[q];
        out0 = 2.0 * acc;
        out1 = h[design->k]; // centre phase: 2 · 0.5 · x[m - k]
    }

    /** Two input samples (even, odd) -> one output sample. */
    inline double downsample (double even, double odd) noexcept
    {
        const double* e = downEven.push (even);
        downOdd.push (odd);
        double acc = 0.0;
        for (int q = 0; q < design->polyTaps; ++q)
            acc += design->taps[q] * e[q];
        return acc + 0.5 * downOdd.at (design->k + 1);
    }

private:
    const HalfbandDesign* design = nullptr;
    FirHistory<HalfbandDesign::kMaxTaps + 2> upHist, downEven, downOdd;
};

/** Oversampler for up to MaxChannels channels and blocks of up to maxBlockSize samples.
    process() upsamples, calls the supplied per-sample function at the high rate, then decimates. */
template <int MaxChannels>
class Oversampler
{
public:
    static constexpr int kMaxStages = 4; // 16x
    static constexpr int kMaxPad = 16;

    void prepare (int maxBlock) noexcept
    {
        maxBlockSize = maxBlock;
        // Quality 0: linear-phase, high stop-band rejection (~120 dB), steep first stage.
        designs[0][0].design (127, 12.0);
        designs[0][1].design (35, 11.0);
        designs[0][2].design (19, 10.0);
        designs[0][3].design (15, 10.0);
        // Quality 1: shorter filters, lower latency (~85-90 dB rejection).
        designs[1][0].design (51, 8.5);
        designs[1][1].design (19, 8.0);
        designs[1][2].design (11, 8.0);
        designs[1][3].design (11, 8.0);
        configure (0, OversamplingQuality::linearPhaseHQ);
    }

    /** factorLog2: 0 = off, 1 = 2x ... 4 = 16x. Resets filter state. */
    void configure (int factorLog2, OversamplingQuality q) noexcept
    {
        numStages = clampValue (factorLog2, 0, kMaxStages);
        quality = q;
        const int qi = static_cast<int> (q);
        for (int ch = 0; ch < MaxChannels; ++ch)
            for (int s = 0; s < kMaxStages; ++s)
                stages[ch][s].setDesign (&designs[qi][s]);

        // latency in units of the highest-rate sample
        const int factor = 1 << numStages;
        int latHigh = 0;
        for (int s = 0; s < numStages; ++s)
        {
            const int inputRateMultiplier = factor >> s; // high-rate samples per stage-input sample
            latHigh += designs[qi][s].latencyAtInputRate * inputRateMultiplier;
        }
        pad = (factor - (latHigh % factor)) % factor;
        latencyBase = (latHigh + pad) / factor;
        reset();
    }

    void reset() noexcept
    {
        for (int ch = 0; ch < MaxChannels; ++ch)
        {
            for (int s = 0; s < kMaxStages; ++s)
                stages[ch][s].reset();
            padLine[ch].clear();
            padLine[ch].setDelay (pad);
        }
    }

    int getFactor() const noexcept { return 1 << numStages; }
    int getLatencySamples() const noexcept { return latencyBase; }

    /** In-place processing. fn(channel, sample) -> sample, evaluated at the oversampled rate. */
    template <typename Fn>
    void process (double* const* channels, int numChannels, int numSamples, Fn&& fn) noexcept
    {
        numChannels = std::min (numChannels, MaxChannels);
        if (numStages == 0)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                for (int i = 0; i < numSamples; ++i)
                    channels[ch][i] = fn (ch, channels[ch][i]);
            return;
        }

        for (int ch = 0; ch < numChannels; ++ch)
        {
            double* data = channels[ch];
            for (int i = 0; i < numSamples; ++i)
                data[i] = processSample (ch, data[i], 0, fn);
        }
    }

private:
    /** Recursive per-sample cascade (depth <= 4). */
    template <typename Fn>
    double processSample (int ch, double x, int stage, Fn& fn) noexcept
    {
        double a, b;
        stages[ch][stage].upsample (x, a, b);
        if (stage + 1 < numStages)
        {
            a = processSample (ch, a, stage + 1, fn);
            b = processSample (ch, b, stage + 1, fn);
        }
        else
        {
            a = padLine[ch].process (fn (ch, a));
            b = padLine[ch].process (fn (ch, b));
        }
        return stages[ch][stage].downsample (a, b);
    }

    HalfbandDesign designs[2][kMaxStages];
    HalfbandStage stages[MaxChannels][kMaxStages];
    DelayLine<kMaxPad + 1> padLine[MaxChannels];
    OversamplingQuality quality = OversamplingQuality::linearPhaseHQ;
    int numStages = 0, pad = 0, latencyBase = 0, maxBlockSize = 0;
};

} // namespace lumen
