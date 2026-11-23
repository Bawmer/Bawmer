#include "VocalToMidi.h"
#include "../Core/Timeline.h"

namespace vf
{

static float median (std::vector<float> v)
{
    if (v.empty()) return 0.0f;
    std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
    return v[v.size() / 2];
}

VocalAnalysis vocalToMidi (const juce::AudioBuffer<float>& mono, double sampleRate,
                           const VocalToMidiOptions& opts, std::atomic<bool>* cancel)
{
    VocalAnalysis res;
    PitchOptions po;
    po.threshold = 0.10f + 0.12f * opts.sensitivity;
    po.voicingGateDb = -58.0f + 16.0f * (1.0f - opts.sensitivity);
    res.f0 = detectPitch (mono, sampleRate, po, cancel);

    const auto& f0 = res.f0;
    const int n = f0.size();
    const double fr = f0.frameRate;
    if (n == 0) return res;

    std::vector<float> midi ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i) midi[(size_t) i] = hzToMidi (f0.hz[(size_t) i]);

    // bridge tiny unvoiced gaps (<= 30 ms) inside sung notes
    const int bridge = (int) std::round (0.03 * fr);
    for (int i = 0; i < n;)
    {
        if (midi[(size_t) i] > 0) { ++i; continue; }
        int j = i;
        while (j < n && midi[(size_t) j] <= 0) ++j;
        if (i > 0 && j < n && j - i <= bridge)
            for (int k = i; k < j; ++k)
                midi[(size_t) k] = midi[(size_t) i - 1] + (midi[(size_t) j] - midi[(size_t) i - 1]) * (float) (k - i + 1) / (float) (j - i + 1);
        i = j;
    }

    struct Seg { int a, b; };   // [a, b)
    std::vector<Seg> segs;
    const int hold = juce::jmax (2, (int) std::round (opts.splitHoldMs * 0.001 * fr));
    const int onsetWin = juce::jmax (2, (int) std::round (0.03 * fr));

    for (int i = 0; i < n;)
    {
        if (midi[(size_t) i] <= 0) { ++i; continue; }
        int j = i;
        while (j < n && midi[(size_t) j] > 0) ++j;   // voiced run [i, j)

        // smoothed pitch (7-frame median)
        std::vector<float> m ((size_t) (j - i));
        for (int k = i; k < j; ++k)
        {
            std::vector<float> w;
            for (int q = juce::jmax (i, k - 3); q <= juce::jmin (j - 1, k + 3); ++q) w.push_back (midi[(size_t) q]);
            m[(size_t) (k - i)] = median (w);
        }

        int segStart = i;
        int deviating = 0, devStart = -1;
        for (int k = i; k < j; ++k)
        {
            // reference pitch: median of the last up to 25 frames of the current segment
            std::vector<float> w;
            for (int q = juce::jmax (segStart, k - 25); q < k; ++q) w.push_back (m[(size_t) (q - i)]);
            const float ref = w.empty() ? m[(size_t) (k - i)] : median (w);

            bool split = false;
            int splitAt = k;
            if (std::abs (m[(size_t) (k - i)] - ref) > opts.splitSemitones)
            {
                if (deviating == 0) devStart = k;
                if (++deviating >= hold) { split = true; splitAt = devStart; }
            }
            else deviating = 0;

            // energy onset (repeated syllable on the same pitch)
            if (! split && k - segStart > onsetWin * 3 && k >= onsetWin)
            {
                float minPrev = 0.0f;
                minPrev = f0.rmsDb[(size_t) (k - onsetWin)];
                for (int q = k - onsetWin; q < k; ++q) minPrev = juce::jmin (minPrev, f0.rmsDb[(size_t) q]);
                if (f0.rmsDb[(size_t) k] - minPrev > opts.onsetDb) { split = true; splitAt = k - onsetWin / 2; }
            }

            if (split && splitAt > segStart)
            {
                segs.push_back ({ segStart, splitAt });
                segStart = splitAt;
                deviating = 0;
            }
        }
        segs.push_back ({ segStart, j });
        i = j;
    }

    // build notes
    const int minFrames = juce::jmax (2, (int) std::round (opts.minNoteMs * 0.001 * fr));
    std::vector<DetectedNote> notes;
    float maxDb = -120.0f;
    for (auto v : f0.rmsDb) maxDb = juce::jmax (maxDb, v);

    for (auto& s : segs)
    {
        const int len = s.b - s.a;
        // central 70 % for the pitch estimate (skip scoops / falls)
        const int a = s.a + len * 15 / 100, b = juce::jmax (a + 1, s.b - len * 15 / 100);
        std::vector<float> w;
        double db = 0.0;
        for (int k = a; k < b; ++k) { w.push_back (midi[(size_t) k]); }
        for (int k = s.a; k < s.b; ++k) db += f0.rmsDb[(size_t) k];
        db /= juce::jmax (1, len);

        DetectedNote dn;
        dn.t0 = s.a / fr;
        dn.t1 = s.b / fr;
        dn.meanMidi = median (w);
        dn.pitch = juce::jlimit (0, 127, (int) std::round (dn.meanMidi));
        dn.velocity = juce::jlimit (0.2f, 1.0f, 1.0f + (float) (db - maxDb) / 40.0f);
        notes.push_back (dn);
    }

    // merge too-short fragments into the closest-pitched neighbour
    for (bool changed = true; changed;)
    {
        changed = false;
        for (size_t k = 0; k < notes.size(); ++k)
        {
            if ((notes[k].t1 - notes[k].t0) * fr >= minFrames) continue;
            const bool hasPrev = k > 0 && notes[k].t0 - notes[k - 1].t1 < 0.02;
            const bool hasNext = k + 1 < notes.size() && notes[k + 1].t0 - notes[k].t1 < 0.02;
            if (! hasPrev && ! hasNext)
            {
                if ((notes[k].t1 - notes[k].t0) < 0.05) { notes.erase (notes.begin() + (long) k); changed = true; break; }
                continue;
            }
            size_t into;
            if (hasPrev && hasNext)
                into = std::abs (notes[k - 1].meanMidi - notes[k].meanMidi) <= std::abs (notes[k + 1].meanMidi - notes[k].meanMidi) ? k - 1 : k + 1;
            else into = hasPrev ? k - 1 : k + 1;
            notes[into].t0 = juce::jmin (notes[into].t0, notes[k].t0);
            notes[into].t1 = juce::jmax (notes[into].t1, notes[k].t1);
            notes.erase (notes.begin() + (long) k);
            changed = true;
            break;
        }
    }

    // merge neighbours that round to the same pitch and were split only by pitch wobble
    for (size_t k = 0; k + 1 < notes.size();)
    {
        auto& a = notes[k];
        auto& b = notes[k + 1];
        const int fa = (int) std::round (a.t1 * fr), fb = (int) std::round (b.t0 * fr);
        bool onset = false;
        if (fb >= onsetWin && fb < n)
        {
            float minPrev = f0.rmsDb[(size_t) juce::jmax (0, fb - onsetWin)];
            for (int q = fb - onsetWin; q < fb; ++q) minPrev = juce::jmin (minPrev, f0.rmsDb[(size_t) juce::jmax (0, q)]);
            onset = f0.rmsDb[(size_t) juce::jmin (n - 1, fb + onsetWin / 2)] - minPrev > opts.onsetDb * 0.8f;
        }
        if (a.pitch == b.pitch && fb - fa <= 1 && ! onset)
        {
            a.t1 = b.t1;
            a.velocity = juce::jmax (a.velocity, b.velocity);
            notes.erase (notes.begin() + (long) k + 1);
        }
        else ++k;
    }

    // note-level octave sanity: a single note an octave away from both neighbours is
    // almost always a tracking error
    for (size_t k = 0; k < notes.size(); ++k)
    {
        const bool hasPrev = k > 0, hasNext = k + 1 < notes.size();
        if (! hasPrev && ! hasNext) continue;
        auto& n = notes[k];
        for (int dir : { -12, 12 })
        {
            const float cand = n.meanMidi + (float) dir;
            const float dPrev = hasPrev ? std::abs (n.meanMidi - notes[k - 1].meanMidi) : 0.0f;
            const float dNext = hasNext ? std::abs (n.meanMidi - notes[k + 1].meanMidi) : 0.0f;
            const float cPrev = hasPrev ? std::abs (cand - notes[k - 1].meanMidi) : 0.0f;
            const float cNext = hasNext ? std::abs (cand - notes[k + 1].meanMidi) : 0.0f;
            if ((! hasPrev || (dPrev >= 10.0f && cPrev <= 7.0f)) && (! hasNext || (dNext >= 10.0f && cNext <= 7.0f)))
            {
                n.meanMidi = cand;
                n.pitch = juce::jlimit (0, 127, (int) std::round (cand));
                break;
            }
        }
    }

    res.notes = std::move (notes);
    return res;
}

} // namespace vf
