#include "KitAnalyzer.h"

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numeric>

namespace dkc
{
namespace
{
constexpr int kBands = 32;
constexpr int kDim = kBands + 2;            // spectrum + two envelope-shape terms
constexpr float kShapeWeight = 1.5f;
constexpr int kMaxClusterHits = 700;        // hits clustered directly; the rest are assigned afterwards
constexpr double kMaxOneShotSeconds = 2.0;

struct Hit
{
    int onset = 0;      // sample index
    int gap = 0;        // samples until the next hit (or end of file)
    int room = 0;       // samples since the previous hit (or start of file)
    float peak = 0.0f;
    std::array<float, kDim> feat {};
    int cluster = -1;
    float dist = 0.0f;  // distance to its cluster centroid
};

int msToSamples (double ms, double sr)      { return juce::jmax (1, (int) std::lround (ms * 0.001 * sr)); }

std::vector<int> makeBandMap (int fftSize, double sr, int numBands)
{
    auto mel = [] (double f) { return 2595.0 * std::log10 (1.0 + f / 700.0); };
    const double fLo = 40.0, fHi = juce::jmin (16000.0, sr * 0.48);
    const double mLo = mel (fLo), mHi = mel (fHi);

    std::vector<int> map ((size_t) fftSize / 2 + 1, -1);
    for (size_t bin = 0; bin < map.size(); ++bin)
    {
        const double f = (double) bin * sr / fftSize;
        if (f >= fLo && f < fHi)
            map[bin] = juce::jlimit (0, numBands - 1, (int) ((mel (f) - mLo) / (mHi - mLo) * numBands));
    }
    return map;
}

float featureDistance (const std::array<float, kDim>& a, const std::array<float, kDim>& b)
{
    // Bands that are quiet in both hits say little about the sound, so they count less.
    float sum = 0.0f, weights = 0.0f;
    for (int i = 0; i < kBands; ++i)
    {
        const float louder = juce::jmax (a[(size_t) i], b[(size_t) i]);          // 0 = loudest band, -1 = -50 dB
        const float w = juce::jlimit (0.05f, 1.0f, (louder + 0.6f) / 0.4f);
        const float d = a[(size_t) i] - b[(size_t) i];
        sum += w * d * d;
        weights += w;
    }
    for (int i = kBands; i < kDim; ++i)
    {
        const float d = a[(size_t) i] - b[(size_t) i];
        sum += kShapeWeight * d * d;
        weights += kShapeWeight;
    }
    // features are stored in units of 50 dB, so this is a weighted RMS difference in dB
    return 50.0f * std::sqrt (sum / weights);
}

//==============================================================================
// 1. Onset detection: log-magnitude spectral flux in mel bands + adaptive peak picking,
//    then each onset is pulled onto the actual attack in the time domain.
std::vector<int> detectOnsets (const std::vector<float>& mono, double sr, float sensitivity,
                               float globalPeak, const ProgressFn& progress)
{
    const int n = (int) mono.size();
    const int order = sr > 60000.0 ? 11 : 10;
    const int N = 1 << order, hop = N / 4;
    const int numFrames = n / hop + 1;
    const int numBands = 40;

    juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) N), data ((size_t) N * 2);
    for (int i = 0; i < N; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) N);

    const auto bandMap = makeBandMap (N, sr, numBands);
    const float magScale = 100.0f * 2.0f / ((float) N * 0.5f) / juce::jmax (1.0e-6f, globalPeak);

    std::vector<float> flux ((size_t) numFrames, 0.0f);
    std::vector<float> prev1 ((size_t) numBands, 0.0f), prev2 ((size_t) numBands, 0.0f), cur ((size_t) numBands);

    for (int t = 0; t < numFrames; ++t)
    {
        if ((t & 1023) == 0 && ! progress (0.02f + 0.38f * (float) t / (float) numFrames))
            return {};

        const int start = t * hop - N / 2;
        for (int i = 0; i < N; ++i)
        {
            const int s = start + i;
            data[(size_t) i] = (s >= 0 && s < n) ? mono[(size_t) s] * window[(size_t) i] : 0.0f;
        }
        std::fill (data.begin() + N, data.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (data.data(), true);

        std::fill (cur.begin(), cur.end(), 0.0f);
        for (size_t bin = 0; bin < bandMap.size(); ++bin)
            if (bandMap[bin] >= 0)
                cur[(size_t) bandMap[bin]] += data[bin];

        float f = 0.0f;
        for (int b = 0; b < numBands; ++b)
        {
            cur[(size_t) b] = std::log1p (cur[(size_t) b] * magScale);
            f += juce::jmax (0.0f, cur[(size_t) b] - juce::jmax (prev1[(size_t) b], prev2[(size_t) b]) * 0.5f
                                                   - juce::jmin (prev1[(size_t) b], prev2[(size_t) b]) * 0.5f);
        }
        flux[(size_t) t] = f;
        prev2.swap (prev1);
        prev1 = cur;
    }

    // adaptive threshold
    double mean = 0.0;
    float maxFlux = 0.0f;
    for (auto f : flux) { mean += f; maxFlux = juce::jmax (maxFlux, f); }
    mean /= (double) juce::jmax (1, numFrames);

    const float s = juce::jmap (juce::jlimit (0.0f, 1.0f, sensitivity), 2.6f, 0.35f);
    const float delta = s * ((float) mean + 0.04f * maxFlux);
    const float localMul = juce::jmap (juce::jlimit (0.0f, 1.0f, sensitivity), 1.6f, 1.1f);

    std::vector<int> frames;
    for (int t = 0; t < numFrames; ++t)
    {
        const float f = flux[(size_t) t];
        if (f <= delta)
            continue;

        bool isMax = true;
        for (int k = juce::jmax (0, t - 3); k <= juce::jmin (numFrames - 1, t + 3) && isMax; ++k)
            if (flux[(size_t) k] > f || (flux[(size_t) k] == f && k < t))
                isMax = false;
        if (! isMax)
            continue;

        float local = 0.0f;
        int count = 0;
        for (int k = juce::jmax (0, t - 16); k <= juce::jmin (numFrames - 1, t + 4); ++k)
            if (std::abs (k - t) > 1) { local += flux[(size_t) k]; ++count; }
        local = count > 0 ? local / (float) count : 0.0f;

        if (f > local * localMul + delta)
            frames.push_back (t);
    }

    // refine: find where the energy jumps the most around the detected frame
    const int w = msToSamples (4.0, sr);
    const float gate = globalPeak * 0.003f;     // ignore anything below -50 dB
    const double eps = (double) globalPeak * globalPeak * 1.0e-5 * w;
    const int minGap = msToSamples (30.0, sr);

    std::vector<int> onsets;
    std::vector<double> prefix;
    for (auto t : frames)
    {
        const int centre = t * hop;
        const int lo = juce::jmax (0, centre - N / 2 - w), hi = juce::jmin (n, centre + N / 4 + w);
        if (hi - lo < 2 * w + 2)
            continue;

        prefix.assign ((size_t) (hi - lo + 1), 0.0);
        for (int i = lo; i < hi; ++i)
            prefix[(size_t) (i - lo + 1)] = prefix[(size_t) (i - lo)] + (double) mono[(size_t) i] * mono[(size_t) i];

        int best = centre;
        double bestRatio = -1.0;
        for (int i = lo + w; i <= hi - w; ++i)
        {
            const double before = prefix[(size_t) (i - lo)] - prefix[(size_t) (i - lo - w)];
            const double after = prefix[(size_t) (i - lo + w)] - prefix[(size_t) (i - lo)];
            const double ratio = (after + eps) / (before + eps);
            if (ratio > bestRatio) { bestRatio = ratio; best = i; }
        }

        // The biggest jump sits a little way into a soft attack; walk back to where the
        // level first lifts clear of what came before.
        {
            const int shortW = juce::jmax (4, w / 4);
            const int refFrom = juce::jmax (lo, best - 2 * w), refTo = juce::jmax (refFrom + 1, best - w);
            const double reference = (prefix[(size_t) (refTo - lo)] - prefix[(size_t) (refFrom - lo)]) / (refTo - refFrom);
            const double lift = 4.0 * reference + eps / w;
            for (int i = juce::jmax (lo, best - w); i < best; ++i)
            {
                const int to = juce::jmin (hi, i + shortW);
                if ((prefix[(size_t) (to - lo)] - prefix[(size_t) (i - lo)]) / juce::jmax (1, to - i) > lift)
                {
                    best = i;
                    break;
                }
            }
        }

        float peak = 0.0f;
        for (int i = best; i < juce::jmin (n, best + msToSamples (30.0, sr)); ++i)
            peak = juce::jmax (peak, std::abs (mono[(size_t) i]));
        if (peak < gate)
            continue;

        if (onsets.empty() || best - onsets.back() >= minGap)
            onsets.push_back (best);
    }
    return onsets;
}

//==============================================================================
// 2. Features: level-independent mel spectrum of the hit plus a rough decay shape.
void computeFeatures (const std::vector<float>& mono, double sr, std::vector<Hit>& hits)
{
    const int n = (int) mono.size();
    const int order = sr > 60000.0 ? 13 : 12;
    const int N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> data ((size_t) N * 2);
    const auto bandMap = makeBandMap (N, sr, kBands);

    const int guard = msToSamples (2.0, sr);

    // short transforms for the decay shape
    const int shortOrder = sr > 60000.0 ? 12 : 11, shortN = 1 << shortOrder;
    juce::dsp::FFT shortFft (shortOrder);
    std::vector<float> shortData ((size_t) shortN * 2);
    const auto shortMap = makeBandMap (shortN, sr, kBands);

    auto bandsOf = [&] (int from, int to, std::array<float, kBands>& out)
    {
        out.fill (0.0f);
        const int count = juce::jmin (to - from, shortN);
        if (count < 8) return;
        std::fill (shortData.begin(), shortData.end(), 0.0f);
        double weight = 0.0;
        for (int i = 0; i < count; ++i)
        {
            const float g = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * ((float) i + 0.5f) / (float) count);
            shortData[(size_t) i] = mono[(size_t) (from + i)] * g;
            weight += g * g;
        }
        shortFft.performFrequencyOnlyForwardTransform (shortData.data(), true);
        for (size_t bin = 0; bin < shortMap.size(); ++bin)
            if (shortMap[bin] >= 0)
                out[(size_t) shortMap[bin]] += shortData[bin] * shortData[bin] / (float) weight;
    };

    for (auto& h : hits)
    {
        const int len = juce::jlimit (msToSamples (20.0, sr), juce::jmin (N, msToSamples (120.0, sr)), h.gap);
        const int avail = juce::jmin (len, n - h.onset);
        const int fade = juce::jmax (1, avail / 4);

        std::fill (data.begin(), data.end(), 0.0f);
        double weight = 0.0;
        for (int i = 0; i < avail; ++i)
        {
            float g = 1.0f;
            if (i >= avail - fade)
                g = 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (i - (avail - fade)) / (float) fade);
            data[(size_t) i] = mono[(size_t) (h.onset + i)] * g;
            weight += g * g;
        }
        fft.performFrequencyOnlyForwardTransform (data.data(), true);

        std::array<float, kBands> e {};
        for (size_t bin = 0; bin < bandMap.size(); ++bin)
            if (bandMap[bin] >= 0)
                e[(size_t) bandMap[bin]] += data[bin] * data[bin];

        // Subtract what was already sounding just before the hit (bass notes, the tail
        // of the previous drum...), so the same drum groups together whatever is under it.
        const int preLen = juce::jmin (avail, h.room - guard - msToSamples (3.0, sr));
        const int preStart = h.onset - guard - preLen;
        if (preLen >= msToSamples (12.0, sr) && preStart >= 0)
        {
            std::fill (data.begin(), data.end(), 0.0f);
            double preWeight = 0.0;
            for (int i = 0; i < preLen; ++i)
            {
                // the hit's window mirrored in time, so both leak across bands the same way
                const int preFade = juce::jmax (1, preLen / 4);
                const float g = i < preFade ? 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) i / (float) preFade) : 1.0f;
                data[(size_t) i] = mono[(size_t) (preStart + i)] * g;
                preWeight += g * g;
            }
            fft.performFrequencyOnlyForwardTransform (data.data(), true);

            std::array<float, kBands> before {};
            for (size_t bin = 0; bin < bandMap.size(); ++bin)
                if (bandMap[bin] >= 0)
                    before[(size_t) bandMap[bin]] += data[bin] * data[bin];

            const float scale = 2.0f * (float) (weight / preWeight);
            for (int b = 0; b < kBands; ++b)
                e[(size_t) b] = juce::jmax (e[(size_t) b] - scale * before[(size_t) b], 1.0e-4f * e[(size_t) b]);
        }

        float maxDb = -1000.0f;
        for (auto& v : e) { v = 10.0f * std::log10 (v + 1.0e-20f); maxDb = juce::jmax (maxDb, v); }
        for (int b = 0; b < kBands; ++b)
            h.feat[(size_t) b] = juce::jmax (-50.0f, e[(size_t) b] - maxDb) / 50.0f;

        // decay shape: level at 20-50 ms and 50-100 ms relative to the first 20 ms,
        // measured only in the bands this hit is loudest in so other sounds don't skew it
        const int a = msToSamples (20.0, sr), b = msToSamples (50.0, sr), c = msToSamples (100.0, sr);
        const int limit = juce::jmin (h.gap, n - h.onset);
        std::array<float, kBands> seg {};
        auto levelDb = [&] (int from, int to)
        {
            bandsOf (h.onset + from, h.onset + to, seg);
            double sum = 0.0;
            for (int k = 0; k < kBands; ++k)
                if (h.feat[(size_t) k] > -0.2f)     // within 10 dB of the loudest band
                    sum += seg[(size_t) k];
            return 10.0f * (float) std::log10 (sum + 1.0e-14);
        };
        const float base = levelDb (0, juce::jmin (a, limit));
        float r1 = -6.0f, r2 = -12.0f;
        if (limit >= b) r1 = levelDb (a, b) - base;
        r2 = limit >= c ? levelDb (b, c) - base : r1 - 6.0f;
        h.feat[(size_t) kBands]     = juce::jlimit (-40.0f, 6.0f, r1) / 50.0f;
        h.feat[(size_t) kBands + 1] = juce::jlimit (-40.0f, 6.0f, r2) / 50.0f;
    }
}

//==============================================================================
// 3. Grouping: average-linkage agglomerative clustering with a distance cut-off.
int clusterHits (std::vector<Hit>& hits, float threshold, const ProgressFn& progress)
{
    const int total = (int) hits.size();
    std::vector<int> subset;
    if (total <= kMaxClusterHits)
    {
        subset.resize ((size_t) total);
        std::iota (subset.begin(), subset.end(), 0);
    }
    else
    {
        for (int i = 0; i < kMaxClusterHits; ++i)
            subset.push_back ((int) ((juce::int64) i * total / kMaxClusterHits));
    }

    const int n = (int) subset.size();
    std::vector<float> d ((size_t) n * (size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            d[(size_t) i * (size_t) n + (size_t) j] = d[(size_t) j * (size_t) n + (size_t) i]
                = featureDistance (hits[(size_t) subset[(size_t) i]].feat, hits[(size_t) subset[(size_t) j]].feat);

    std::vector<int> size ((size_t) n, 1), label ((size_t) n);
    std::vector<bool> alive ((size_t) n, true);
    std::iota (label.begin(), label.end(), 0);

    for (int iter = 0; iter < n - 1; ++iter)
    {
        if ((iter & 31) == 0 && ! progress (0.55f + 0.2f * (float) iter / (float) n))
            return -1;

        int bi = -1, bj = -1;
        float best = threshold;
        for (int i = 0; i < n; ++i)
        {
            if (! alive[(size_t) i]) continue;
            const float* row = d.data() + (size_t) i * (size_t) n;
            for (int j = i + 1; j < n; ++j)
                if (alive[(size_t) j] && row[j] < best) { best = row[j]; bi = i; bj = j; }
        }
        if (bi < 0)
            break;

        const float wi = (float) size[(size_t) bi], wj = (float) size[(size_t) bj];
        for (int k = 0; k < n; ++k)
        {
            if (! alive[(size_t) k] || k == bi || k == bj) continue;
            const float v = (wi * d[(size_t) bi * (size_t) n + (size_t) k] + wj * d[(size_t) bj * (size_t) n + (size_t) k]) / (wi + wj);
            d[(size_t) bi * (size_t) n + (size_t) k] = d[(size_t) k * (size_t) n + (size_t) bi] = v;
        }
        size[(size_t) bi] += size[(size_t) bj];
        alive[(size_t) bj] = false;
        for (auto& l : label)
            if (l == bj) l = bi;
    }

    // compact labels
    std::vector<int> remap ((size_t) n, -1);
    int numClusters = 0;
    for (int i = 0; i < n; ++i)
    {
        auto& r = remap[(size_t) label[(size_t) i]];
        if (r < 0) r = numClusters++;
        hits[(size_t) subset[(size_t) i]].cluster = r;
    }

    // centroids
    std::vector<std::array<float, kDim>> centroid ((size_t) numClusters);
    std::vector<int> count ((size_t) numClusters, 0);
    for (auto& c : centroid) c.fill (0.0f);
    for (auto& h : hits)
        if (h.cluster >= 0)
        {
            for (int k = 0; k < kDim; ++k) centroid[(size_t) h.cluster][(size_t) k] += h.feat[(size_t) k];
            ++count[(size_t) h.cluster];
        }
    for (int c = 0; c < numClusters; ++c)
        for (auto& v : centroid[(size_t) c]) v /= (float) juce::jmax (1, count[(size_t) c]);

    // assign everything (including hits left out of the subset) a distance / nearest cluster
    for (auto& h : hits)
    {
        if (h.cluster >= 0)
        {
            h.dist = featureDistance (h.feat, centroid[(size_t) h.cluster]);
            continue;
        }
        float best = threshold * 1.25f;
        for (int c = 0; c < numClusters; ++c)
        {
            const float v = featureDistance (h.feat, centroid[(size_t) c]);
            if (v < best) { best = v; h.cluster = c; }
        }
        h.dist = best;
    }
    return numClusters;
}

//==============================================================================
// 4. Isolation. The exemplar hit is cut out, and where other hits of the same group
//    exist, a spectral mask keeps only what the group has in common (per time-frequency
//    cell: the median magnitude across the group), which removes bass notes, vocals and
//    other material that just happened to be playing under this particular hit.
juce::AudioBuffer<float> isolateOneShot (const juce::AudioBuffer<float>& src, const std::vector<float>& mono,
                                         double sr, const std::vector<const Hit*>& members)
{
    const int n = src.getNumSamples();
    const int numCh = src.getNumChannels();
    const Hit& ex = *members.front();

    const int pre = msToSamples (2.0, sr);
    const int start = juce::jmax (0, ex.onset - pre);
    const int lead = ex.onset - start;
    // stop just short of the next hit so none of its attack gets in
    const int usable = ex.onset + ex.gap >= n ? ex.gap : juce::jmax (msToSamples (15.0, sr), ex.gap - msToSamples (3.0, sr));
    const int length = juce::jlimit (1, n - start, juce::jmin (usable, (int) (kMaxOneShotSeconds * sr)) + lead);

    juce::AudioBuffer<float> out (numCh, length);

    {
        const int order = sr > 60000.0 ? 11 : 10;
        const int N = 1 << order, hop = N / 4, bins = N / 2 + 1;
        const int total = length + 2 * N;
        const int numFrames = (total - N) / hop + 1;
        const int numMembers = (int) members.size();

        juce::dsp::FFT fft (order);
        std::vector<float> window ((size_t) N);
        for (int i = 0; i < N; ++i)
            window[(size_t) i] = std::sqrt (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) N));

        using Cpx = juce::dsp::Complex<float>;
        std::vector<Cpx> in ((size_t) N), spec ((size_t) N);

        // magnitudes of every member, aligned on their onsets
        std::vector<float> mags ((size_t) numMembers * (size_t) numFrames * (size_t) bins);
        for (int m = 0; m < numMembers; ++m)
        {
            const int base = members[(size_t) m]->onset - lead - N;
            for (int f = 0; f < numFrames; ++f)
            {
                for (int i = 0; i < N; ++i)
                {
                    const int s = base + f * hop + i;
                    in[(size_t) i] = Cpx ((s >= 0 && s < n) ? mono[(size_t) s] * window[(size_t) i] : 0.0f, 0.0f);
                }
                fft.perform (in.data(), spec.data(), false);
                float* dst = mags.data() + ((size_t) m * (size_t) numFrames + (size_t) f) * (size_t) bins;
                for (int b = 0; b < bins; ++b)
                    dst[b] = std::abs (spec[(size_t) b]);
            }
        }

        // mask
        std::vector<float> mask ((size_t) numFrames * (size_t) bins, 1.0f);
        std::vector<int> valid;
        std::vector<float> vals;
        const int alwaysValid = msToSamples (30.0, sr);
        for (int f = 0; f < numFrames; ++f)
        {
            const int frameEnd = f * hop + N - N - lead;    // relative to the onset
            const int frameCentre = frameEnd - N / 2;
            valid.clear();
            for (int m = 0; m < numMembers; ++m)
                if (frameCentre <= alwaysValid || frameEnd <= members[(size_t) m]->gap)
                    valid.push_back (m);
            if (valid.size() < 3)
                continue;

            for (int b = 0; b < bins; ++b)
            {
                vals.clear();
                for (auto m : valid)
                    vals.push_back (mags[((size_t) m * (size_t) numFrames + (size_t) f) * (size_t) bins + (size_t) b]);
                auto mid = vals.begin() + (std::ptrdiff_t) (vals.size() / 2);
                std::nth_element (vals.begin(), mid, vals.end());
                const float exMag = mags[(size_t) f * (size_t) bins + (size_t) b];
                // Leave anything within 6 dB of the group's median alone (noisy sounds vary
                // that much from hit to hit); pull down what clearly isn't part of the drum.
                const float ratio = exMag > 1.0e-12f ? juce::jmin (1.0f, 2.0f * *mid / exMag) : 1.0f;
                mask[(size_t) f * (size_t) bins + (size_t) b] = ratio * ratio;
            }
        }

        // Second pass: spectral subtraction of whatever was already sounding right before
        // the hit (a held bass note, a pad, the tail of the previous drum).
        {
            std::vector<double> background ((size_t) bins, 0.0);
            int count = 0;
            const int guard = msToSamples (2.0, sr);
            for (int k = 0; k < 3; ++k)
            {
                const int from = ex.onset - guard - N - k * hop;
                if (from < 0)
                    break;
                for (int i = 0; i < N; ++i)
                    in[(size_t) i] = Cpx (mono[(size_t) (from + i)] * window[(size_t) i], 0.0f);
                fft.perform (in.data(), spec.data(), false);
                for (int b = 0; b < bins; ++b)
                    background[(size_t) b] += (double) std::norm (spec[(size_t) b]);
                ++count;
            }
            if (count > 0)
                for (int f = 0; f < numFrames; ++f)
                    for (int b = 0; b < bins; ++b)
                    {
                        const double exPower = (double) mags[(size_t) f * (size_t) bins + (size_t) b] * mags[(size_t) f * (size_t) bins + (size_t) b];
                        if (exPower > 1.0e-24)
                            mask[(size_t) f * (size_t) bins + (size_t) b] *= (float) std::sqrt (juce::jmax (0.0, 1.0 - 2.25 * background[(size_t) b] / count / exPower));
                    }
        }

        // resynthesise the exemplar through the mask
        std::vector<float> acc ((size_t) total);
        for (int ch = 0; ch < numCh; ++ch)
        {
            std::fill (acc.begin(), acc.end(), 0.0f);
            const float* x = src.getReadPointer (ch);
            const int base = start - N;
            for (int f = 0; f < numFrames; ++f)
            {
                for (int i = 0; i < N; ++i)
                {
                    const int s = base + f * hop + i;
                    in[(size_t) i] = Cpx ((s >= 0 && s < n) ? x[s] * window[(size_t) i] : 0.0f, 0.0f);
                }
                fft.perform (in.data(), spec.data(), false);
                const float* mk = mask.data() + (size_t) f * (size_t) bins;
                for (int b = 0; b < bins; ++b)
                {
                    spec[(size_t) b] *= mk[b];
                    if (b > 0 && b < N / 2)
                        spec[(size_t) (N - b)] *= mk[b];
                }
                fft.perform (spec.data(), in.data(), true);
                for (int i = 0; i < N; ++i)
                    acc[(size_t) (f * hop + i)] += in[(size_t) i].real() * window[(size_t) i] * 0.5f;
            }
            out.copyFrom (ch, 0, acc.data() + N, length);
        }
    }

    // Trim the tail once it has decayed to -55 dB, or - when the tail has flattened out
    // into a steady residue of other material - to where the drum sinks into that residue.
    const int block = msToSamples (5.0, sr);
    const float peak = out.getMagnitude (0, length);
    int end = length;
    if (peak > 0.0f)
    {
        std::vector<float> env;
        for (int pos = 0; pos < length; pos += block)
            env.push_back (out.getMagnitude (pos, juce::jmin (block, length - pos)));

        float threshold = peak * 0.0018f;
        const size_t quarter = env.size() / 4;
        if (quarter >= 3)
        {
            auto median = [&] (size_t from, size_t to)
            {
                std::vector<float> part (env.begin() + (std::ptrdiff_t) from, env.begin() + (std::ptrdiff_t) to);
                std::nth_element (part.begin(), part.begin() + (std::ptrdiff_t) (part.size() / 2), part.end());
                return part[part.size() / 2];
            };
            const float third = median (env.size() - 2 * quarter, env.size() - quarter);
            const float last = median (env.size() - quarter, env.size());
            if (last < peak * 0.1f && last > third * 0.8f)      // quiet and no longer decaying
                threshold = juce::jmax (threshold, last * 1.6f);
        }

        // Once the drum has been below -48 dB for 20 ms it is over; anything later is not it.
        size_t limit = env.size();
        for (size_t i = 0, quiet = 0; i < env.size(); ++i)
        {
            quiet = env[i] < peak * 0.004f ? quiet + 1 : 0;
            if (quiet >= 4) { limit = i - 3; break; }
        }

        int lastLoud = 0;
        for (size_t i = 0; i < limit; ++i)
            if (env[i] > threshold)
                lastLoud = (int) (i + 1) * block;
        end = juce::jlimit (juce::jmin (length, msToSamples (20.0, sr)), length, lastLoud + msToSamples (15.0, sr));
    }
    out.setSize (numCh, end, true);

    // fades + normalise
    const int fadeIn = juce::jmin (lead, end);
    if (fadeIn > 1)
        out.applyGainRamp (0, fadeIn, 0.0f, 1.0f);
    const int fadeOut = juce::jmin (end, juce::jmax (msToSamples (8.0, sr), end / 8));
    out.applyGainRamp (end - fadeOut, fadeOut, 1.0f, 0.0f);

    const float finalPeak = out.getMagnitude (0, end);
    if (finalPeak > 1.0e-6f)
        out.applyGain (0.89f / finalPeak);
    return out;
}

//==============================================================================
// 5. Naming: a coarse guess at what kind of drum this is.
int classify (const juce::AudioBuffer<float>& audio, double sr)
{
    const int order = sr > 60000.0 ? 13 : 12, N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> data ((size_t) N * 2, 0.0f);
    const int len = juce::jmin (N, audio.getNumSamples());
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        for (int i = 0; i < len; ++i)
            data[(size_t) i] += audio.getSample (ch, i);
    fft.performFrequencyOnlyForwardTransform (data.data(), true);

    double low = 0, lowMid = 0, mid = 0, high = 0, air = 0, total = 0;
    for (int bin = 1; bin <= N / 2; ++bin)
    {
        const double f = bin * sr / N, e = (double) data[(size_t) bin] * data[(size_t) bin];
        total += e;
        if (f < 150.0) low += e;
        else if (f < 300.0) lowMid += e;
        else if (f < 2000.0) mid += e;
        else if (f < 6000.0) high += e;
        else air += e;
    }
    if (total <= 0.0)
        return Perc;
    low /= total; lowMid /= total; mid /= total; high /= total; air /= total;
    const double seconds = audio.getNumSamples() / sr;

    if (high + air > 0.6 && low + lowMid < 0.1)
        return seconds < 0.18 ? Hat : (seconds < 0.7 ? OpenHat : Cymbal);
    if (low > 0.45 || (low + lowMid > 0.75 && low > lowMid))
        return Kick;
    if (high + air > 0.12 && low + lowMid + mid > 0.12)
        return (lowMid < 0.04 && low < 0.02) ? Clap : Snare;
    if (low + lowMid > 0.6)
        return Tom;
    return Perc;
}

std::vector<float> makeEnvelope (const float* x, int n, int points)
{
    std::vector<float> env ((size_t) points, 0.0f);
    if (n <= 0) return env;
    for (int p = 0; p < points; ++p)
    {
        const int a = (int) ((juce::int64) p * n / points);
        const int b = juce::jmax (a + 1, (int) ((juce::int64) (p + 1) * n / points));
        float m = 0.0f;
        for (int i = a; i < juce::jmin (b, n); ++i)
            m = juce::jmax (m, std::abs (x[i]));
        env[(size_t) p] = m;
    }
    return env;
}
} // namespace

//==============================================================================
const char* padTypeName (int type)
{
    static const char* names[] = { "Kick", "Snare", "Clap", "Hat", "Open Hat", "Tom", "Perc", "Cymbal" };
    return names[juce::jlimit (0, (int) numPadTypes - 1, type)];
}

void PadSample::rebuildThumb (int points)
{
    thumb.assign ((size_t) points, 0.0f);
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        const auto env = makeEnvelope (audio.getReadPointer (ch), audio.getNumSamples(), points);
        for (int i = 0; i < points; ++i)
            thumb[(size_t) i] = juce::jmax (thumb[(size_t) i], env[(size_t) i]);
    }
}

Kit::Ptr KitAnalyzer::analyse (const juce::AudioBuffer<float>& source, double sr,
                               const AnalysisSettings& settings, const ProgressFn& progress)
{
    Kit::Ptr kit = new Kit();
    const int n = source.getNumSamples();
    const int numCh = source.getNumChannels();
    kit->sourceSeconds = n / sr;
    if (n < 64 || numCh < 1)
        return kit;

    std::vector<float> mono ((size_t) n, 0.0f);
    for (int ch = 0; ch < numCh; ++ch)
        juce::FloatVectorOperations::addWithMultiply (mono.data(), source.getReadPointer (ch), 1.0f / (float) numCh, n);

    kit->overview = makeEnvelope (mono.data(), n, 1200);
    const float globalPeak = *std::max_element (kit->overview.begin(), kit->overview.end());
    if (globalPeak < 1.0e-5f)
        return kit;     // silence

    auto onsets = detectOnsets (mono, sr, settings.sensitivity, globalPeak, progress);
    if (! progress (0.4f))
        return nullptr;

    std::vector<Hit> hits (onsets.size());
    for (size_t i = 0; i < onsets.size(); ++i)
    {
        hits[i].onset = onsets[i];
        hits[i].gap = (i + 1 < onsets.size() ? onsets[i + 1] : n) - onsets[i];
        hits[i].room = onsets[i] - (i > 0 ? onsets[i - 1] : 0);
        const int end = juce::jmin (n, onsets[i] + juce::jmin (hits[i].gap, msToSamples (50.0, sr)));
        for (int s = onsets[i]; s < end; ++s)
            hits[i].peak = juce::jmax (hits[i].peak, std::abs (mono[(size_t) s]));
    }
    kit->totalHits = (int) hits.size();
    if (hits.empty())
        return kit;

    computeFeatures (mono, sr, hits);
    if (! progress (0.55f))
        return nullptr;

    const float threshold = juce::jmap (juce::jlimit (0.0f, 1.0f, settings.separation), 12.0f, 4.0f);
    const int numClusters = clusterHits (hits, threshold, progress);
    if (numClusters < 0)
        return nullptr;

    // rank groups: most hits first, louder first on ties
    struct Group { int id; int count = 0; float level = 0.0f; std::vector<const Hit*> members; };
    std::vector<Group> groups ((size_t) numClusters);
    for (int c = 0; c < numClusters; ++c) groups[(size_t) c].id = c;
    for (auto& h : hits)
        if (h.cluster >= 0)
        {
            auto& g = groups[(size_t) h.cluster];
            ++g.count;
            g.level = juce::jmax (g.level, h.peak);
            g.members.push_back (&h);
        }
    std::sort (groups.begin(), groups.end(), [] (const Group& a, const Group& b)
               { return a.count != b.count ? a.count > b.count : a.level > b.level; });
    if ((int) groups.size() > juce::jlimit (1, kMaxPads, settings.maxPads))
        groups.resize ((size_t) juce::jlimit (1, kMaxPads, settings.maxPads));

    struct Built { PadSample pad; int cluster; };
    std::vector<Built> built;
    for (size_t g = 0; g < groups.size(); ++g)
    {
        if (! progress (0.75f + 0.25f * (float) g / (float) groups.size()))
            return nullptr;

        auto members = groups[g].members;
        std::sort (members.begin(), members.end(), [] (const Hit* a, const Hit* b) { return a->dist < b->dist; });
        if (members.size() > 12)
            members.resize (12);

        // exemplar: among the most typical hits, the one with the most room to ring out
        const size_t pool = juce::jmax ((size_t) 1, (members.size() + 1) / 2);
        size_t best = 0;
        for (size_t i = 1; i < pool; ++i)
            if (juce::jmin (members[i]->gap, (int) (1.5 * sr)) > juce::jmin (members[best]->gap, (int) (1.5 * sr)))
                best = i;
        std::swap (members[0], members[best]);

        Built b;
        b.cluster = groups[g].id;
        b.pad.audio = isolateOneShot (source, mono, sr, members);
        b.pad.sampleRate = sr;
        b.pad.hitCount = groups[g].count;
        b.pad.type = classify (b.pad.audio, sr);
        b.pad.rebuildThumb();
        built.push_back (std::move (b));
    }

    std::stable_sort (built.begin(), built.end(), [] (const Built& a, const Built& b)
                      { return a.pad.type != b.pad.type ? a.pad.type < b.pad.type : a.pad.hitCount > b.pad.hitCount; });

    std::array<int, numPadTypes> typeTotals {}, typeSeen {};
    for (auto& b : built) ++typeTotals[(size_t) b.pad.type];

    std::vector<int> clusterToPad ((size_t) numClusters, -1);
    for (size_t i = 0; i < built.size(); ++i)
    {
        auto& p = built[i].pad;
        p.name = padTypeName (p.type);
        if (typeTotals[(size_t) p.type] > 1)
            p.name << " " << ++typeSeen[(size_t) p.type];
        clusterToPad[(size_t) built[i].cluster] = (int) i;
        kit->pads.push_back (std::move (p));
    }

    kit->hits.reserve (hits.size());
    for (auto& h : hits)
        kit->hits.push_back ({ (float) h.onset / (float) n, h.cluster >= 0 ? clusterToPad[(size_t) h.cluster] : -1 });

    progress (1.0f);
    return kit;
}

Kit::Ptr KitAnalyzer::analyseFile (const juce::File& file, const AnalysisSettings& settings,
                                   const ProgressFn& progress, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
    {
        error = "Couldn't read \"" + file.getFileName() + "\" as audio";
        return nullptr;
    }

    const auto maxSamples = (juce::int64) (600.0 * reader->sampleRate);
    const int length = (int) juce::jmin (reader->lengthInSamples, maxSamples);
    const int channels = (int) juce::jlimit (1u, 2u, reader->numChannels);

    juce::AudioBuffer<float> buffer (channels, length);
    if (! reader->read (&buffer, 0, length, 0, true, channels > 1))
    {
        error = "Error while reading \"" + file.getFileName() + "\"";
        return nullptr;
    }
    if (! progress (0.02f))
        return nullptr;

    auto kit = analyse (buffer, reader->sampleRate, settings, progress);
    if (kit != nullptr)
    {
        kit->sourceName = file.getFileNameWithoutExtension();
        if (kit->pads.empty())
            error = "No hits found in \"" + file.getFileName() + "\" - try raising Sensitivity";
    }
    return kit;
}
}
