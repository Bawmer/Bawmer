#include "VoiceAnalyzer.h"
#include "../DSP/AudioUtils.h"
#include "../DSP/PitchDetector.h"
#include "../DSP/Spectral.h"
#include "../Core/Timeline.h"
#include "../Core/Lyrics.h"

namespace vf
{

namespace
{
    float percentile (std::vector<float> v, float p)
    {
        if (v.empty()) return 0.0f;
        std::sort (v.begin(), v.end());
        return v[(size_t) juce::jlimit (0, (int) v.size() - 1, (int) (p * (float) (v.size() - 1)))];
    }

    SampleReport reportFor (const juce::File& file, const juce::AudioBuffer<float>& mono, double sr, const F0Track& f0)
    {
        SampleReport r;
        r.file = file;
        r.seconds = mono.getNumSamples() / sr;
        const float* x = mono.getReadPointer (0);
        int clipped = 0;
        float peak = 0.0f;
        for (int i = 0; i < mono.getNumSamples(); ++i)
        {
            const float a = std::abs (x[i]);
            peak = juce::jmax (peak, a);
            if (a > 0.995f) ++clipped;
        }
        r.peakDb = juce::Decibels::gainToDecibels (peak, -120.0f);
        r.clippingPercent = 100.0f * (float) clipped / (float) juce::jmax (1, mono.getNumSamples());

        std::vector<float> db, midi;
        int voiced = 0;
        for (int i = 0; i < f0.size(); ++i)
        {
            db.push_back (f0.rmsDb[(size_t) i]);
            if (f0.hz[(size_t) i] > 0.0f) { ++voiced; midi.push_back (hzToMidi (f0.hz[(size_t) i])); }
        }
        r.voicedSeconds = voiced / f0.frameRate;
        r.noiseFloorDb = percentile (db, 0.08f);
        r.minMidi = percentile (midi, 0.05f);
        r.maxMidi = percentile (midi, 0.95f);

        if (r.clippingPercent > 0.05f) r.warnings.add ("Clipping detected - record with more headroom.");
        if (r.noiseFloorDb > -50.0f)   r.warnings.add ("High noise floor (" + juce::String (r.noiseFloorDb, 1) + " dB) - use a quieter room.");
        if (r.peakDb < -30.0f)         r.warnings.add ("Very quiet recording.");
        if (r.voicedSeconds < 3.0)     r.warnings.add ("Very little singing detected in this file.");

        // reverb / doubling: low periodicity on loud frames
        double per = 0.0; int n = 0;
        for (int i = 0; i < f0.size(); ++i)
            if (f0.hz[(size_t) i] > 0.0f) { per += f0.periodicity[(size_t) i]; ++n; }
        if (n > 0 && per / n < 0.82) r.warnings.add ("Signal is not very clean (reverb, doubling or effects?). Use dry vocals.");
        r.usable = r.voicedSeconds >= 1.0 && r.clippingPercent < 2.0f;
        return r;
    }

    /** Envelope of the built-in synth singing a vowel set around a given pitch. */
    std::vector<float> referenceEnvelope (float medianMidi, float formantScale, const ExpressionSettings& ex)
    {
        VocalProject p;
        p.bpm = 60.0;   // 1 beat = 1 s
        static const char* vowels[] = { "aa", "iy", "uw", "eh", "ao", "ah", "er", "ae", "ow", "ih" };
        double beat = 0.2;
        for (int i = 0; i < 20; ++i)
        {
            Note n;
            n.startBeat = beat;
            n.lengthBeats = 0.8;
            n.pitch = juce::jlimit (36, 90, (int) std::round (medianMidi) + ((i % 5) - 2) * 2);
            n.lyric = "[" + juce::String (vowels[i % 10]) + "]";
            n.phonemes = vowels[i % 10];
            n.velocity = 0.8f;
            p.notes.push_back (n);
            beat += 1.0;
        }
        auto tl = buildTimeline (p);
        auto curves = buildCurves (p, ex, tl.endTime + 0.3);
        VoiceTimbre t;
        t.formantScale = formantScale;
        auto audio = SynthRenderer::render (tl, curves, t, ex);
        auto f0 = detectPitch (audio, kEngineSampleRate);
        return averageEnvelopeDb (audio, kEngineSampleRate, f0);
    }

    float bandValueShifted (const std::vector<float>& env, float bandPos)
    {
        const int NB = (int) env.size();
        const int b = juce::jlimit (0, NB - 2, (int) std::floor (bandPos));
        const float t = juce::jlimit (0.0f, 1.0f, bandPos - (float) b);
        return env[(size_t) b] + (env[(size_t) b + 1] - env[(size_t) b]) * t;
    }

    double correlation (const std::vector<float>& a, const std::vector<float>& b)
    {
        double ma = 0, mb = 0;
        for (size_t i = 0; i < a.size(); ++i) { ma += a[i]; mb += b[i]; }
        ma /= (double) a.size(); mb /= (double) b.size();
        double sab = 0, saa = 0, sbb = 0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            sab += (a[i] - ma) * (b[i] - mb);
            saa += (a[i] - ma) * (a[i] - ma);
            sbb += (b[i] - mb) * (b[i] - mb);
        }
        return sab / std::sqrt (juce::jmax (1.0e-9, saa * sbb));
    }
}

SampleReport inspectSample (const juce::File& file)
{
    juce::AudioBuffer<float> mono;
    double sr = 0;
    if (! loadAudioMono (file, mono, sr, 600.0))
    {
        SampleReport r; r.file = file; r.usable = false; r.warnings.add ("Cannot read this audio file.");
        return r;
    }
    auto f0 = detectPitch (mono, sr);
    return reportFor (file, mono, sr, f0);
}

VoiceAnalysisResult analyseVoiceSamples (const juce::Array<juce::File>& files, std::atomic<bool>* cancel,
                                         std::function<void (float, const juce::String&)> progress)
{
    VoiceAnalysisResult res;
    constexpr int NB = SpectralBands::kNumBands;
    std::vector<double> envAcc (NB, 0.0);
    double envWeight = 0.0, noisAcc = 0.0;
    std::vector<float> allMidi, vibRates, vibDepths;

    auto report = [&] (float p, const juce::String& m) { if (progress) progress (p, m); };

    for (int fi = 0; fi < files.size(); ++fi)
    {
        if (cancel != nullptr && cancel->load()) { res.error = "Cancelled"; return res; }
        report (0.7f * (float) fi / (float) files.size(), "Analysing " + files[fi].getFileName());

        juce::AudioBuffer<float> mono;
        double sr = 0;
        if (! loadAudioMono (files[fi], mono, sr, 1200.0))
        {
            SampleReport r; r.file = files[fi]; r.usable = false; r.warnings.add ("Cannot read this audio file.");
            res.reports.push_back (r);
            continue;
        }
        auto audio = resampleMono (mono, sr, kEngineSampleRate);
        auto f0 = detectPitch (audio, kEngineSampleRate, {}, cancel);
        auto rep = reportFor (files[fi], audio, kEngineSampleRate, f0);
        res.reports.push_back (rep);
        if (! rep.usable) continue;

        auto env = averageEnvelopeDb (audio, kEngineSampleRate, f0);
        for (int b = 0; b < NB; ++b) envAcc[(size_t) b] += env[(size_t) b] * rep.voicedSeconds;
        envWeight += rep.voicedSeconds;
        noisAcc += highBandNoisiness (audio, kEngineSampleRate, f0) * rep.voicedSeconds;

        for (auto h : f0.hz) if (h > 0.0f) allMidi.push_back (hzToMidi (h));

        // vibrato habits on long voiced runs
        const int n = f0.size();
        const int minRun = (int) (0.6 * f0.frameRate);
        const int avgWin = (int) (0.15 * f0.frameRate);
        for (int i = 0; i < n;)
        {
            if (f0.hz[(size_t) i] <= 0.0f) { ++i; continue; }
            int j = i; while (j < n && f0.hz[(size_t) j] > 0.0f) ++j;
            if (j - i >= minRun)
            {
                std::vector<float> cents;
                for (int k = i + avgWin; k < j - avgWin; ++k)
                {
                    double m = 0.0;
                    for (int q = k - avgWin; q <= k + avgWin; ++q) m += hzToMidi (f0.hz[(size_t) q]);
                    m /= (2 * avgWin + 1);
                    cents.push_back ((float) ((hzToMidi (f0.hz[(size_t) k]) - m) * 100.0));
                }
                if (cents.size() > 40)
                {
                    // rate: autocorrelation peak between 3.5 and 8.5 Hz
                    const int lagMin = (int) (f0.frameRate / 8.5), lagMax = (int) (f0.frameRate / 3.5);
                    double e0 = 0.0;
                    for (auto c : cents) e0 += (double) c * c;
                    int bestLag = -1; double bestR = 0.0;
                    for (int lag = lagMin; lag <= lagMax && lag < (int) cents.size() / 2; ++lag)
                    {
                        double r = 0.0;
                        for (size_t k = (size_t) lag; k < cents.size(); ++k) r += (double) cents[k] * cents[k - (size_t) lag];
                        r /= juce::jmax (1.0e-9, e0 * (double) (cents.size() - (size_t) lag) / (double) cents.size());
                        if (r > bestR) { bestR = r; bestLag = lag; }
                    }
                    // depth: RMS over the later part of the note (vibrato usually starts late)
                    double ss = 0.0; size_t cnt = 0;
                    for (size_t k = cents.size() * 2 / 5; k < cents.size(); ++k) { ss += (double) cents[k] * cents[k]; ++cnt; }
                    const float depth = (float) (std::sqrt (ss / juce::jmax ((size_t) 1, cnt)) * std::sqrt (2.0));
                    if (bestLag > 0 && bestR > 0.3 && depth > 8.0f && depth < 160.0f)
                    {
                        vibRates.push_back ((float) (f0.frameRate / bestLag));
                        vibDepths.push_back (depth);
                    }
                }
            }
            i = j;
        }
    }

    if (envWeight < 1.0)
    {
        res.error = "No usable singing found. Add clean, dry vocal recordings (WAV/FLAC/AIFF).";
        return res;
    }

    std::vector<float> userEnv (NB);
    for (int b = 0; b < NB; ++b) userEnv[(size_t) b] = (float) (envAcc[(size_t) b] / envWeight);

    res.stats.seconds = envWeight;
    res.stats.medianMidi = percentile (allMidi, 0.5f);
    res.stats.minMidi = percentile (allMidi, 0.03f);
    res.stats.maxMidi = percentile (allMidi, 0.97f);
    res.stats.vibratoRate = vibRates.empty() ? 5.5f : percentile (vibRates, 0.5f);
    res.stats.vibratoDepth = vibDepths.empty() ? 30.0f : percentile (vibDepths, 0.5f);

    res.suggested.vibratoRateHz = juce::jlimit (4.0f, 8.0f, res.stats.vibratoRate);
    res.suggested.vibratoDepthCents = juce::jlimit (10.0f, 90.0f, res.stats.vibratoDepth);
    const float flat = (float) (noisAcc / envWeight);
    res.suggested.breathiness = juce::jlimit (0.05f, 0.6f, (flat - 0.05f) / 0.35f * 0.6f);
    res.timbre.breathBase = juce::jlimit (0.0f, 0.3f, (flat - 0.1f) * 0.8f);

    // ---- formant scale search --------------------------------------------------
    report (0.75f, "Matching vocal-tract length");
    ExpressionSettings ex;
    ex.vibratoDepthCents = res.suggested.vibratoDepthCents;
    ex.vibratoRateHz = res.suggested.vibratoRateHz;
    ex.humanize = 0.2f;
    auto ref = referenceEnvelope (res.stats.medianMidi, 1.0f, ex);

    std::vector<int> fitBands;
    for (int b = 0; b < NB; ++b)
        if (SpectralBands::centre (b) > 250.0f && SpectralBands::centre (b) < 4500.0f) fitBands.push_back (b);

    float bestScale = 1.0f; double bestCorr = -2.0;
    const float logRange = std::log (16000.0f / 80.0f);
    for (float s = 0.84f; s <= 1.26f; s += 0.02f)
    {
        std::vector<float> a, b;
        const float shift = std::log (s) / logRange * NB;   // E_ref(f / s) => band position - shift
        for (int band : fitBands)
        {
            a.push_back (userEnv[(size_t) band]);
            b.push_back (bandValueShifted (ref, (float) band - shift));
        }
        const double c = correlation (a, b);
        if (c > bestCorr) { bestCorr = c; bestScale = s; }
    }
    res.timbre.formantScale = bestScale;

    // ---- residual spectral correction ------------------------------------------------
    report (0.88f, "Building spectral fingerprint");
    auto ref2 = referenceEnvelope (res.stats.medianMidi, bestScale, ex);
    std::vector<float> corr (NB);
    double mean = 0.0; int cnt = 0;
    for (int b = 0; b < NB; ++b)
    {
        corr[(size_t) b] = userEnv[(size_t) b] - ref2[(size_t) b];
        const float fc = SpectralBands::centre (b);
        if (fc > 150.0f && fc < 8000.0f) { mean += corr[(size_t) b]; ++cnt; }
    }
    mean /= juce::jmax (1, cnt);
    std::vector<float> smooth (NB);
    for (int b = 0; b < NB; ++b)
    {
        double s = 0.0; int c = 0;
        for (int k = juce::jmax (0, b - 1); k <= juce::jmin (NB - 1, b + 1); ++k) { s += corr[(size_t) k] - mean; ++c; }
        float v = (float) (s / c);
        const float fc = SpectralBands::centre (b);
        const float hiLimit = fc > 11000.0f ? 4.0f : 15.0f;
        smooth[(size_t) b] = juce::jlimit (-15.0f, hiLimit, v);
    }
    res.timbre.correctionDb = smooth;

    // ---- overall warnings ---------------------------------------------------------------
    if (res.stats.seconds < 30.0)
        res.warnings.add ("Only " + juce::String (res.stats.seconds, 0) + " s of singing. Lite clones work best with 1-3 minutes; neural training needs 10+ minutes.");
    else if (res.stats.seconds < 600.0)
        res.warnings.add ("Enough for a Lite clone. For neural training, 10-30 minutes of clean singing is recommended.");
    if (res.stats.maxMidi - res.stats.minMidi < 7.0f)
        res.warnings.add ("Narrow pitch range in the samples - include low and high phrases for a better clone.");

    report (1.0f, "Done");
    res.ok = true;
    return res;
}

} // namespace vf
