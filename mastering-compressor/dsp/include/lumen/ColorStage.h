// Lumen MC-64 — optional analog coloration (transformer + discrete amplifier models).
//
// Design goals: subtle, level dependent, controllable, and *completely absent* when disabled.
//   * Amplifier: an asymmetric tanh soft clipper, normalised to unity small-signal gain.
//     The bias (Character) sets the 2nd/3rd harmonic balance: 0 = symmetric (odd/3rd only),
//     1 = strongly asymmetric (dominant 2nd). Harmonic level rises smoothly with Drive and with
//     signal level, as with real class-A stages.
//   * Transformer: low-frequency saturation. A 1st-order split isolates the band below ~110 Hz,
//     which is driven harder than the rest, mimicking core saturation that grows at low
//     frequencies and high levels. Only the *difference* (the distortion) is added back, so the
//     linear frequency response stays flat.
//   * Everything nonlinear runs inside the oversampler; a 4 Hz DC blocker follows.
//   * Drive is gain-compensated: at low levels the stage has exactly unity gain.
#pragma once

#include "DspCommon.h"
#include "Filters.h"
#include "Oversampler.h"

namespace lumen
{
struct ColorSettings
{
    bool transformer = true;
    bool amplifier = true;
    double driveDb = 6.0;   // 0..24 dB into the nonlinear stages
    double character = 0.35; // 0..1 : 3rd-heavy .. 2nd-heavy
    double amount = 1.0;     // 0..1 wet amount of the colour (hybrid blend)
};

/** Static curves, exposed for tests and the UI. */
struct Shapers
{
    /** Asymmetric soft clipper with f(0) = 0 and f'(0) = 1. */
    static double amp (double x, double k, double bias) noexcept
    {
        const double tb = std::tanh (k * bias);
        const double slope0 = k * (1.0 - tb * tb);
        return (std::tanh (k * (x + bias)) - tb) / slope0;
    }

    /** Symmetric rational soft saturation (odd harmonics, very gentle onset), f'(0) = 1. */
    static double core (double x) noexcept { return x / std::sqrt (1.0 + x * x); }
};

template <int MaxChannels>
class ColorStage
{
public:
    void prepare (double sampleRate, int maxBlock) noexcept
    {
        fs = sampleRate;
        os.prepare (maxBlock);
        driveSmoother.reset (fs, 30.0);
        charSmoother.reset (fs, 30.0);
        amountSmoother.reset (fs, 30.0);
        driveSmoother.setCurrentAndTarget (dbToGain (settings.driveDb));
        charSmoother.setCurrentAndTarget (settings.character);
        amountSmoother.setCurrentAndTarget (settings.amount);
        configureOversampling (osLog2, osQuality, true);
    }

    void reset() noexcept
    {
        os.reset();
        for (int ch = 0; ch < MaxChannels; ++ch)
        {
            lfSplit[ch].reset();
            dcBlock[ch].reset();
        }
    }

    void configureOversampling (int factorLog2, OversamplingQuality q, bool force = false) noexcept
    {
        if (! force && factorLog2 == osLog2 && q == osQuality)
            return;
        osLog2 = factorLog2;
        osQuality = q;
        os.configure (osLog2, osQuality);
        const double highRate = fs * os.getFactor();
        for (int ch = 0; ch < MaxChannels; ++ch)
        {
            lfSplit[ch].setup (110.0, highRate);
            lfSplit[ch].reset();
            dcBlock[ch].setup (4.0, fs);
            dcBlock[ch].reset();
        }
    }

    int getLatencySamples() const noexcept { return os.getLatencySamples(); }

    void setSettings (const ColorSettings& s) noexcept
    {
        settings = s;
        driveSmoother.setTarget (dbToGain (s.driveDb));
        charSmoother.setTarget (s.character);
        amountSmoother.setTarget (s.amount);
    }

    /** In-place; channels already at the base rate. Parameter smoothing is updated per base sample
        (held over the oversampled sub-samples). */
    void process (double* const* channels, int numChannels, int numSamples) noexcept
    {
        numChannels = std::min (numChannels, MaxChannels);
        const bool useTransformer = settings.transformer;
        const bool useAmp = settings.amplifier;

        for (int start = 0; start < numSamples; start += kSubBlock)
        {
            const int n = std::min (kSubBlock, numSamples - start);
            // parameters are smoothed at sub-block granularity (16 samples): click-free and cheap
            double drive = 1.0, character = 0.0, amount = 0.0;
            for (int i = 0; i < n; ++i)
            {
                drive = driveSmoother.next();
                character = charSmoother.next();
                amount = amountSmoother.next();
            }
            const double invDrive = 1.0 / drive;
            const double bias = 0.02 + 0.2 * character;
            const double k = 1.0;

            double* sub[MaxChannels];
            for (int ch = 0; ch < numChannels; ++ch)
                sub[ch] = channels[ch] + start;

            os.process (sub, numChannels, n, [&] (int ch, double x) noexcept {
                double y = x * drive;
                if (useTransformer)
                {
                    // low band driven ~6 dB harder than the rest; only the distortion is added back
                    const double lf = lfSplit[ch].lowpass (y);
                    const double lfDriven = 2.0 * lf;
                    y += 0.5 * (Shapers::core (lfDriven) - lfDriven);
                }
                if (useAmp)
                    y = Shapers::amp (y, k, bias);
                y *= invDrive;
                return x + amount * (y - x);
            });

            for (int ch = 0; ch < numChannels; ++ch)
                for (int i = 0; i < n; ++i)
                    sub[ch][i] = dcBlock[ch].highpass (sub[ch][i]);
        }
    }

private:
    static constexpr int kSubBlock = 16;
    double fs = 48000.0;
    ColorSettings settings;
    Oversampler<MaxChannels> os;
    OnePole lfSplit[MaxChannels];
    OnePole dcBlock[MaxChannels];
    LinearSmoother driveSmoother, charSmoother, amountSmoother;
    int osLog2 = 0;
    OversamplingQuality osQuality = OversamplingQuality::linearPhaseHQ;
};

} // namespace lumen
