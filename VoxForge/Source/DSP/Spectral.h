#pragma once

#include "PitchDetector.h"

namespace vf
{

/** 40 log-spaced analysis bands between 80 Hz and 16 kHz. */
struct SpectralBands
{
    static constexpr int kNumBands = 40;
    static float centre (int band);
    static float lowEdge (int band);
    static float highEdge (int band);
};

/** Average spectral envelope (dB per band) over voiced, stable frames.
    Envelope points are taken at harmonic peaks so the result is largely
    independent of the sung pitch. */
std::vector<float> averageEnvelopeDb (const juce::AudioBuffer<float>& mono, double sampleRate, const F0Track& f0);

/** Applies a per-band gain curve (dB) with an STFT (Hann, 75 % overlap). */
void applySpectralCorrection (juce::AudioBuffer<float>& mono, double sampleRate, const std::vector<float>& bandDb);

/** Mean spectral flatness 2-6 kHz over voiced frames (breathiness indicator, 0..1). */
float highBandNoisiness (const juce::AudioBuffer<float>& mono, double sampleRate, const F0Track& f0);

} // namespace vf
