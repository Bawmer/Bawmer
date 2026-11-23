#include "Spectral.h"

namespace vf
{

static constexpr float kLo = 80.0f, kHi = 16000.0f;

float SpectralBands::lowEdge (int b)  { return kLo * std::pow (kHi / kLo, (float) b / kNumBands); }
float SpectralBands::highEdge (int b) { return kLo * std::pow (kHi / kLo, (float) (b + 1) / kNumBands); }
float SpectralBands::centre (int b)   { return std::sqrt (lowEdge (b) * highEdge (b)); }

namespace
{
    constexpr int kOrder = 11;            // 2048-point FFT
    constexpr int kN = 1 << kOrder;

    void hann (std::vector<float>& w)
    {
        w.resize (kN);
        for (int i = 0; i < kN; ++i)
            w[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) kN);
    }
}

std::vector<float> averageEnvelopeDb (const juce::AudioBuffer<float>& mono, double sr, const F0Track& f0)
{
    constexpr int NB = SpectralBands::kNumBands;
    std::vector<double> acc (NB, 0.0);
    std::vector<int> cnt (NB, 0);

    juce::dsp::FFT fft (kOrder);
    std::vector<float> win, buf ((size_t) kN * 2);
    hann (win);
    const float* x = mono.getReadPointer (0);
    const int n = mono.getNumSamples();
    const int hop = kN / 2;

    float maxDb = -120.0f;
    for (auto v : f0.rmsDb) maxDb = juce::jmax (maxDb, v);

    for (int start = 0; start + kN <= n; start += hop)
    {
        const double tc = (start + kN / 2) / sr;
        const int fi = (int) (tc * f0.frameRate);
        if (fi < 0 || fi >= f0.size()) continue;
        const float hz = f0.hz[(size_t) fi];
        if (hz <= 0.0f || f0.rmsDb[(size_t) fi] < maxDb - 30.0f) continue;

        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < kN; ++i) buf[(size_t) i] = x[start + i] * win[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);

        // harmonic-peak envelope: sample the spectrum at each harmonic of f0, then
        // interpolate (log-frequency) onto the band centres -> independent of pitch
        std::vector<float> hf, hdb;
        const float nyq = (float) sr * 0.5f;
        for (int h = 1; h * hz < juce::jmin (nyq * 0.95f, 17000.0f); ++h)
        {
            const float fc = h * hz;
            const int k0 = juce::jmax (1, (int) ((fc - 0.3f * hz) * kN / sr));
            const int k1 = juce::jmin (kN / 2, (int) ((fc + 0.3f * hz) * kN / sr));
            float m = 0.0f;
            for (int k = k0; k <= k1; ++k) m = juce::jmax (m, buf[(size_t) k]);
            hf.push_back (fc);
            hdb.push_back (juce::Decibels::gainToDecibels (m, -140.0f));
        }
        if (hf.size() < 2) continue;
        std::vector<float> bandDb (NB, -140.0f);
        size_t j = 0;
        for (int b = 0; b < NB; ++b)
        {
            const float fc = SpectralBands::centre (b);
            if (fc <= hf.front()) { bandDb[(size_t) b] = hdb.front(); continue; }
            if (fc >= hf.back())  { bandDb[(size_t) b] = hdb.back(); continue; }
            while (j + 1 < hf.size() && hf[j + 1] < fc) ++j;
            const float t = std::log (fc / hf[j]) / std::log (hf[j + 1] / hf[j]);
            bandDb[(size_t) b] = hdb[j] + (hdb[j + 1] - hdb[j]) * t;
        }
        for (int b = 0; b < NB; ++b) { acc[(size_t) b] += bandDb[(size_t) b]; ++cnt[(size_t) b]; }
    }

    std::vector<float> out (NB, 0.0f);
    for (int b = 0; b < NB; ++b) out[(size_t) b] = cnt[(size_t) b] > 0 ? (float) (acc[(size_t) b] / cnt[(size_t) b]) : -100.0f;
    return out;
}

void applySpectralCorrection (juce::AudioBuffer<float>& mono, double sr, const std::vector<float>& bandDb)
{
    constexpr int NB = SpectralBands::kNumBands;
    if ((int) bandDb.size() != NB) return;

    // per-bin gain (linear interpolation of band centres in log-frequency)
    std::vector<float> gain ((size_t) kN / 2 + 1, 1.0f);
    for (int k = 0; k <= kN / 2; ++k)
    {
        const float f = juce::jmax (1.0f, (float) (k * sr / kN));
        float db;
        if (f <= SpectralBands::centre (0)) db = bandDb[0];
        else if (f >= SpectralBands::centre (NB - 1)) db = bandDb[(size_t) NB - 1];
        else
        {
            const float pos = std::log (f / kLo) / std::log (kHi / kLo) * NB - 0.5f;
            const int b = juce::jlimit (0, NB - 2, (int) std::floor (pos));
            const float t = juce::jlimit (0.0f, 1.0f, pos - (float) b);
            db = bandDb[(size_t) b] + (bandDb[(size_t) b + 1] - bandDb[(size_t) b]) * t;
        }
        gain[(size_t) k] = juce::Decibels::decibelsToGain (db);
    }

    juce::dsp::FFT fft (kOrder);
    std::vector<float> win, buf ((size_t) kN * 2);
    hann (win);
    const int n = mono.getNumSamples();
    const int hop = kN / 4;
    juce::AudioBuffer<float> out (1, n + kN);
    out.clear();
    const float* x = mono.getReadPointer (0);
    float* y = out.getWritePointer (0);

    for (int start = -kN; start < n; start += hop)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < kN; ++i)
        {
            const int idx = start + i;
            buf[(size_t) i] = (idx >= 0 && idx < n) ? x[idx] * win[(size_t) i] : 0.0f;
        }
        fft.performRealOnlyForwardTransform (buf.data());
        for (int k = 0; k <= kN / 2; ++k)
        {
            buf[(size_t) 2 * k]     *= gain[(size_t) k];
            buf[(size_t) 2 * k + 1] *= gain[(size_t) k];
        }
        fft.performRealOnlyInverseTransform (buf.data());
        for (int i = 0; i < kN; ++i)
        {
            const int idx = start + i;
            if (idx >= 0 && idx < n) y[idx] += buf[(size_t) i] * win[(size_t) i];
        }
    }
    // Hann^2 at 75 % overlap sums to 1.5
    for (int i = 0; i < n; ++i) mono.setSample (0, i, y[i] / 1.5f);
}

float highBandNoisiness (const juce::AudioBuffer<float>& mono, double sr, const F0Track& f0)
{
    juce::dsp::FFT fft (kOrder);
    std::vector<float> win, buf ((size_t) kN * 2);
    hann (win);
    const float* x = mono.getReadPointer (0);
    const int n = mono.getNumSamples();
    double acc = 0.0; int cnt = 0;
    const int k0 = (int) (2000.0 * kN / sr), k1 = juce::jmin (kN / 2, (int) (6000.0 * kN / sr));

    for (int start = 0; start + kN <= n; start += kN)
    {
        const int fi = (int) ((start + kN / 2) / sr * f0.frameRate);
        if (fi < 0 || fi >= f0.size() || f0.hz[(size_t) fi] <= 0.0f) continue;
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < kN; ++i) buf[(size_t) i] = x[start + i] * win[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        double logSum = 0.0, linSum = 0.0;
        for (int k = k0; k <= k1; ++k)
        {
            const double p = (double) buf[(size_t) k] * buf[(size_t) k] + 1.0e-12;
            logSum += std::log (p); linSum += p;
        }
        const int m = k1 - k0 + 1;
        acc += std::exp (logSum / m) / (linSum / m);   // spectral flatness
        ++cnt;
    }
    return cnt > 0 ? (float) (acc / cnt) : 0.0f;
}

} // namespace vf
