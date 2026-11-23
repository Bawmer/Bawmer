#include "PitchDetector.h"
#include "AudioUtils.h"

namespace vf
{

float F0Track::hzAt (double seconds) const
{
    if (hz.empty()) return 0.0f;
    const double pos = seconds * frameRate;
    const int i = (int) std::floor (pos);
    if (i < 0 || i >= (int) hz.size()) return 0.0f;
    const float a = hz[(size_t) i];
    const float b = i + 1 < (int) hz.size() ? hz[(size_t) i + 1] : a;
    if (a <= 0.0f || b <= 0.0f) return a;
    const float t = (float) (pos - i);
    return a * std::pow (b / a, t);
}

F0Track detectPitch (const juce::AudioBuffer<float>& monoIn, double sampleRate, const PitchOptions& opts,
                     std::atomic<bool>* cancel)
{
    constexpr double sr = 16000.0;
    auto mono = resampleMono (monoIn, sampleRate, sr);
    const float* x = mono.getReadPointer (0);
    const int n = mono.getNumSamples();

    F0Track track;
    track.frameRate = 1.0 / opts.hopSeconds;
    const int hop = (int) std::round (opts.hopSeconds * sr);
    const int tauMin = (int) std::floor (sr / opts.fmaxHz);
    const int tauMax = (int) std::ceil (sr / opts.fminHz);
    const int W = 1024;
    const int L = W + tauMax + 2;
    int order = 1;
    while ((1 << order) < 2 * L) ++order;
    const int N = 1 << order;
    juce::dsp::FFT fft (order);

    std::vector<float> A ((size_t) N * 2), B ((size_t) N * 2), seg ((size_t) L);
    std::vector<double> prefix ((size_t) L + 1), d ((size_t) tauMax + 2), dn ((size_t) tauMax + 2);

    const int frames = n / hop + 1;
    track.hz.assign ((size_t) frames, 0.0f);
    track.periodicity.assign ((size_t) frames, 0.0f);
    track.rmsDb.assign ((size_t) frames, -120.0f);

    for (int fr = 0; fr < frames; ++fr)
    {
        if (cancel != nullptr && (fr & 255) == 0 && cancel->load()) break;
        const int centre = fr * hop;
        const int start = centre - L / 2;
        for (int j = 0; j < L; ++j)
        {
            const int idx = start + j;
            seg[(size_t) j] = (idx >= 0 && idx < n) ? x[idx] : 0.0f;
        }

        // energy of the analysis window
        double e = 0.0;
        for (int j = 0; j < W; ++j) e += (double) seg[(size_t) j] * seg[(size_t) j];
        const float rmsDb = (float) juce::Decibels::gainToDecibels (std::sqrt (e / W), -120.0);
        track.rmsDb[(size_t) fr] = rmsDb;
        if (rmsDb < opts.voicingGateDb) continue;

        // cross-correlation via FFT: r(tau) = sum_{j<W} x_j x_{j+tau}
        std::fill (A.begin(), A.end(), 0.0f);
        std::fill (B.begin(), B.end(), 0.0f);
        for (int j = 0; j < W; ++j) A[(size_t) j] = seg[(size_t) j];
        for (int j = 0; j < L; ++j) B[(size_t) j] = seg[(size_t) j];
        fft.performRealOnlyForwardTransform (A.data());
        fft.performRealOnlyForwardTransform (B.data());
        for (int k = 0; k <= N / 2; ++k)
        {
            const float ar = A[(size_t) 2 * k], ai = A[(size_t) 2 * k + 1];
            const float br = B[(size_t) 2 * k], bi = B[(size_t) 2 * k + 1];
            // conj(A) * B
            A[(size_t) 2 * k]     = ar * br + ai * bi;
            A[(size_t) 2 * k + 1] = ar * bi - ai * br;
        }
        fft.performRealOnlyInverseTransform (A.data());   // A[tau] = r(tau) (JUCE scales by 1/N)

        prefix[0] = 0.0;
        for (int j = 0; j < L; ++j) prefix[(size_t) j + 1] = prefix[(size_t) j] + (double) seg[(size_t) j] * seg[(size_t) j];

        const double e0 = prefix[(size_t) W];
        double running = 0.0;
        dn[0] = 1.0;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            const double et = prefix[(size_t) (tau + W)] - prefix[(size_t) tau];
            d[(size_t) tau] = juce::jmax (0.0, e0 + et - 2.0 * (double) A[(size_t) tau]);
            running += d[(size_t) tau];
            dn[(size_t) tau] = running > 0.0 ? d[(size_t) tau] * tau / running : 1.0;
        }

        int best = -1;
        for (int tau = tauMin; tau <= tauMax; ++tau)
        {
            if (dn[(size_t) tau] < opts.threshold)
            {
                while (tau + 1 <= tauMax && dn[(size_t) tau + 1] < dn[(size_t) tau]) ++tau;
                best = tau;
                break;
            }
        }
        if (best < 0)
        {
            double m = 1.0e9;
            for (int tau = tauMin; tau <= tauMax; ++tau)
                if (dn[(size_t) tau] < m) { m = dn[(size_t) tau]; best = tau; }
        }
        // sub-octave check: a strong 2nd harmonic (F1 close to 2 x f0) makes YIN pick
        // half the period; prefer the doubled lag when it is clearly more periodic
        if (2 * best <= tauMax)
        {
            int b2 = 2 * best;
            for (int k = juce::jmax (tauMin, b2 - 2); k <= juce::jmin (tauMax, b2 + 2); ++k)
                if (dn[(size_t) k] < dn[(size_t) b2]) b2 = k;
            if (dn[(size_t) b2] < dn[(size_t) best] - 0.03) best = b2;
        }
        const double dmin = dn[(size_t) best];
        track.periodicity[(size_t) fr] = (float) juce::jlimit (0.0, 1.0, 1.0 - dmin);
        if (dmin > 0.35) continue;

        double t = best;
        if (best > tauMin && best < tauMax)
        {
            const double a = dn[(size_t) best - 1], b = dn[(size_t) best], c = dn[(size_t) best + 1];
            const double den = a - 2.0 * b + c;
            if (std::abs (den) > 1.0e-12) t = best + 0.5 * (a - c) / den;
        }
        track.hz[(size_t) fr] = (float) (sr / t);
    }

    // relative gate: drop frames far below the loudest part (bleed, room noise)
    float maxDb = -120.0f;
    for (auto v : track.rmsDb) maxDb = juce::jmax (maxDb, v);
    for (size_t i = 0; i < track.hz.size(); ++i)
        if (track.rmsDb[i] < maxDb - 42.0f) track.hz[i] = 0.0f;

    // octave-error repair + 5-point median on voiced frames
    auto h = track.hz;
    for (size_t i = 2; i + 2 < h.size(); ++i)
    {
        if (h[i] <= 0.0f) continue;
        float w[5]; int c = 0;
        for (int k = -2; k <= 2; ++k) if (h[i + (size_t) k] > 0.0f) w[c++] = h[i + (size_t) k];
        if (c < 3) continue;
        std::sort (w, w + c);
        const float med = w[c / 2];
        float v = h[i];
        const float ratio = v / med;
        if (ratio > 1.8f && ratio < 2.2f) v *= 0.5f;
        else if (ratio > 0.45f && ratio < 0.55f) v *= 2.0f;
        track.hz[i] = (std::abs (v - med) / med > 0.06f) ? med : v;
    }
    // remove isolated voiced blips (< 3 frames)
    for (size_t i = 0; i < track.hz.size();)
    {
        if (track.hz[i] <= 0.0f) { ++i; continue; }
        size_t j = i;
        while (j < track.hz.size() && track.hz[j] > 0.0f) ++j;
        if (j - i < 3) for (size_t k = i; k < j; ++k) track.hz[k] = 0.0f;
        i = j;
    }
    return track;
}

} // namespace vf
