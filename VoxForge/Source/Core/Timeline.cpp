#include "Timeline.h"
#include "Lyrics.h"
#include "Phonemes.h"

namespace vf
{

namespace
{
    struct Syl
    {
        juce::StringArray onset, coda;
        int nucleus = -1;
        double start = 0, end = 0;   // note-defined span (vowel starts at 'start')
        int note = -1;
        double onsetDur = 0, codaDur = 0;
    };

    double typical (const juce::String& p)
    {
        return phonemeAt (juce::jmax (0, phonemeIndex (p))).typicalMs * 0.001;
    }
}

Timeline buildTimeline (const VocalProject& project, const std::vector<bool>* include)
{
    Timeline tl;
    const auto& notes = project.notes;
    std::vector<Syl> syls;

    // ---- 1. group notes into syllables (a note + following "-" hold notes) ---
    for (size_t i = 0; i < notes.size(); ++i)
    {
        if (include != nullptr && (i >= include->size() || ! (*include)[i])) continue;
        const auto& n = notes[i];
        const double t0 = project.beatsToSeconds (n.startBeat);
        double t1 = project.beatsToSeconds (n.endBeat());

        auto ph = phonemesForNote (n);
        if (ph.isEmpty())
        {
            // hold: extend previous syllable if it is adjacent, otherwise sing "aa"
            if (! syls.empty() && t0 - syls.back().end < 0.06)
            {
                syls.back().end = juce::jmax (syls.back().end, t1);
                continue;
            }
            ph.add ("aa");
        }

        // also extend over following holds
        for (size_t j = i + 1; j < notes.size(); ++j)
        {
            if (include != nullptr && (j >= include->size() || ! (*include)[j])) break;
            if (! phonemesForNote (notes[j]).isEmpty()) break;
            const double h0 = project.beatsToSeconds (notes[j].startBeat);
            if (h0 - t1 > 0.06) break;
            t1 = project.beatsToSeconds (notes[j].endBeat());
            i = j;
        }

        auto parts = syllabifyPhonemes (ph);
        const double span = (t1 - t0) / (double) parts.size();
        for (size_t k = 0; k < parts.size(); ++k)
        {
            Syl s;
            s.note = (int) (&n - notes.data());
            s.start = t0 + span * (double) k;
            s.end = s.start + span;
            auto& p = parts[k];
            int v = -1;
            for (int q = 0; q < p.size(); ++q)
                if (isVowel (phonemeIndex (p[q]))) { v = q; break; }
            if (v < 0)
            {
                // no vowel: sustain the last voiced continuant (e.g. "mm") or add schwa
                for (int q = p.size(); --q >= 0;)
                {
                    auto c = phonemeAt (juce::jmax (0, phonemeIndex (p[q]))).cls;
                    if (c == PhClass::Nasal || c == PhClass::Liquid || c == PhClass::Glide) { v = q; break; }
                }
                if (v < 0) { p.add ("ax"); v = p.size() - 1; }
            }
            for (int q = 0; q < v; ++q)          s.onset.add (p[q]);
            for (int q = v + 1; q < p.size(); ++q) s.coda.add (p[q]);
            s.nucleus = phonemeIndex (p[v]);
            syls.push_back (s);
        }
    }

    // ---- 2. consonant durations --------------------------------------------------
    for (size_t i = 0; i < syls.size(); ++i)
    {
        auto& s = syls[i];
        for (auto& p : s.onset) s.onsetDur += typical (p) * 0.85;
        for (auto& p : s.coda)  s.codaDur  += typical (p) * 0.85;

        double budget;
        if (i == 0) budget = juce::jmin (0.25, s.start);
        else
        {
            const auto& prev = syls[i - 1];
            const double gap = juce::jmax (0.0, s.start - prev.end);
            budget = juce::jmin (0.25, gap + 0.35 * (prev.end - prev.start));
        }
        if (s.onsetDur > budget) s.onsetDur = budget;
    }

    // ---- 3. place segments --------------------------------------------------------
    for (size_t i = 0; i < syls.size(); ++i)
    {
        auto& s = syls[i];
        double end = s.end;
        if (i + 1 < syls.size())
        {
            const auto& nx = syls[i + 1];
            end = juce::jmin (end, nx.start - nx.onsetDur);
        }
        end = juce::jmax (end, s.start + 0.03);

        // onset (before the beat)
        double t = s.start - s.onsetDur;
        double totalTyp = 0;
        for (auto& p : s.onset) totalTyp += typical (p);
        for (auto& p : s.onset)
        {
            const double d = totalTyp > 0 ? s.onsetDur * typical (p) / totalTyp : 0.0;
            tl.segments.push_back ({ juce::jmax (0, phonemeIndex (p)), t, t + d, s.note, false });
            t += d;
        }

        // coda (end of the syllable), max 40 % of vowel span
        const double avail = end - s.start;
        const double codaDur = juce::jmin (s.codaDur, avail * 0.4);
        const double vowelEnd = end - codaDur;
        tl.segments.push_back ({ s.nucleus, s.start, vowelEnd, s.note, true });

        double tc = vowelEnd;
        double codaTyp = 0;
        for (auto& p : s.coda) codaTyp += typical (p);
        for (auto& p : s.coda)
        {
            const double d = codaTyp > 0 ? codaDur * typical (p) / codaTyp : 0.0;
            tl.segments.push_back ({ juce::jmax (0, phonemeIndex (p)), tc, tc + d, s.note, false });
            tc += d;
        }
        tl.endTime = juce::jmax (tl.endTime, end);
    }
    return tl;
}

//==============================================================================
namespace
{
    inline float logistic (double x) { return (float) (1.0 / (1.0 + std::exp (-x))); }

    struct NoteSpan { double t0, t1; float pitch; float vel; double vibPhase; float vibRate; };
}

ControlCurves buildCurves (const VocalProject& project, const ExpressionSettings& s,
                           double durationSec, const std::vector<bool>* include,
                           const PitchNuanceFn& nuance)
{
    ControlCurves c;
    const int frames = juce::jmax (1, (int) std::ceil (durationSec * c.rate));
    c.f0Hz.assign ((size_t) frames, 0.0f);
    c.amp.assign ((size_t) frames, 0.0f);
    c.breath.assign ((size_t) frames, s.breathiness);
    c.tension.assign ((size_t) frames, s.tension);

    std::vector<NoteSpan> spans;
    juce::Random rng (1234);   // deterministic -> identical re-renders
    for (size_t i = 0; i < project.notes.size(); ++i)
    {
        if (include != nullptr && (i >= include->size() || ! (*include)[i])) continue;
        const auto& n = project.notes[i];
        NoteSpan sp;
        sp.t0 = project.beatsToSeconds (n.startBeat);
        sp.t1 = project.beatsToSeconds (n.endBeat());
        sp.pitch = (float) n.pitch + n.pitchOffsetCents * 0.01f;
        sp.vel = n.velocity;
        sp.vibPhase = rng.nextDouble() * juce::MathConstants<double>::twoPi;
        sp.vibRate = s.vibratoRateHz * (1.0f + (rng.nextFloat() - 0.5f) * 0.08f * (0.5f + s.humanize));
        spans.push_back (sp);
    }
    if (spans.empty()) return c;

    const double porta = juce::jmax (0.005, s.portamentoMs * 0.001);
    const double driftA = 8.0 * s.humanize;   // cents
    const double ph1 = rng.nextDouble() * 6.28, ph2 = rng.nextDouble() * 6.28;

    // boundaries for pitch transitions
    struct Boundary { double centre, width; float delta; };
    std::vector<Boundary> bounds;
    for (size_t k = 0; k + 1 < spans.size(); ++k)
    {
        const auto& a = spans[k];
        const auto& b = spans[k + 1];
        const bool legato = b.t0 - a.t1 < 0.05;
        Boundary bd;
        bd.delta = b.pitch - a.pitch;
        if (legato) { bd.centre = b.t0 - porta * 0.25; bd.width = porta / 6.0; }
        else        { bd.centre = b.t0 - 0.08;         bd.width = 0.004; }   // jump during the onset consonant
        bounds.push_back (bd);
    }

    size_t spanIdx = 0;
    size_t fullyApplied = 0;
    float base = spans.front().pitch;

    for (int f = 0; f < frames; ++f)
    {
        const double t = f / c.rate;
        const double beat = project.secondsToBeats (t);

        while (spanIdx + 1 < spans.size() && t >= spans[spanIdx + 1].t0 - 0.25
               && (t >= spans[spanIdx].t1 || t >= spans[spanIdx + 1].t0 - 0.001))
            ++spanIdx;
        while (fullyApplied < bounds.size() && bounds[fullyApplied].centre + 8 * bounds[fullyApplied].width < t)
            base += bounds[fullyApplied++].delta;

        float pitch = base;
        for (size_t k = fullyApplied; k < bounds.size(); ++k)
        {
            const auto& bd = bounds[k];
            if (bd.centre - 8 * bd.width > t) break;
            pitch += bd.delta * logistic ((t - bd.centre) / bd.width);
        }

        const auto& sp = spans[spanIdx];

        // ---- vibrato ---------------------------------------------------------------
        const double delay = s.vibratoDelayMs * 0.001;
        const double inNote = t - sp.t0;
        float vib = 0.0f;
        if (inNote > delay && sp.t1 - sp.t0 > delay + 0.1)
        {
            const double fadeIn = juce::jlimit (0.0, 1.0, (inNote - delay) / 0.25);
            const double fadeOut = juce::jlimit (0.0, 1.0, (sp.t1 - t) / 0.06 + 0.3);
            const float mod = evalControl (project.modWheel, beat, 0.0f);
            const double depth = s.vibratoDepthCents * (1.0 + 1.5 * mod) * fadeIn * fadeOut;
            const double rate = sp.vibRate * (1.0 + 0.04 * std::sin (t * 1.3 + ph2) * s.humanize);
            vib = (float) (depth * std::sin (sp.vibPhase + juce::MathConstants<double>::twoPi * rate * (inNote - delay)));
        }

        // ---- drift + onset scoop -------------------------------------------------
        float drift = (float) (driftA * (0.6 * std::sin (t * 2 * juce::MathConstants<double>::pi * 0.7 + ph1)
                                       + 0.4 * std::sin (t * 2 * juce::MathConstants<double>::pi * 1.9 + ph2)));
        const bool phraseStart = spanIdx == 0 || sp.t0 - spans[spanIdx - 1].t1 > 0.05;
        if (phraseStart && inNote > -0.1 && inNote < 0.12)
            drift -= (float) (30.0 * s.humanize * (1.0 - juce::jlimit (0.0, 1.0, (inNote + 0.02) / 0.12)));

        // ---- pitch bend from DAW --------------------------------------------------
        const float bend = evalControl (project.pitchBend, beat, 0.0f) * s.pitchBendRange;

        float cents = vib + drift;
        if (nuance) cents += nuance (t);
        c.f0Hz[(size_t) f] = midiToHz (pitch + bend + cents * 0.01f);

        // ---- dynamics -----------------------------------------------------------------
        float amp = 0.0f;
        {
            // in a note (including onset consonants just before it)?
            const bool before = t < sp.t0 - 0.25;
            const bool after = t > sp.t1 + 0.002;
            if (! before && ! after)
            {
                const double attack = phraseStart ? juce::jlimit (0.0, 1.0, (t - (sp.t0 - 0.25)) / 0.03) : 1.0;
                const bool phraseEnd = spanIdx + 1 >= spans.size() || spans[spanIdx + 1].t0 - sp.t1 > 0.05;
                const double release = phraseEnd ? juce::jlimit (0.0, 1.0, (sp.t1 - t) / 0.06) : 1.0;
                const float cc11 = evalControl (project.expressionCC, beat, 1.0f);
                const float level = (0.35f + 0.65f * sp.vel) * (0.4f + 0.6f * s.expression) * cc11;
                // slight swell on long notes
                const double swell = 1.0 + 0.06 * juce::jlimit (0.0, 1.0, inNote / 0.8);
                amp = (float) (level * attack * release * swell);
            }
        }
        c.amp[(size_t) f] = amp;

        const float bc = evalControl (project.breathCC, beat, 0.0f);
        c.breath[(size_t) f] = juce::jlimit (0.0f, 1.0f, s.breathiness + 0.6f * bc + (1.0f - sp.vel) * 0.08f);
        c.tension[(size_t) f] = juce::jlimit (0.0f, 1.0f, s.tension + (sp.vel - 0.7f) * 0.3f + s.expression * 0.1f - 0.05f);
    }
    return c;
}

} // namespace vf
