#include "FormantSynth.h"
#include "../Core/Lyrics.h"

namespace vf
{

static inline float smoothstep (float x) { x = juce::jlimit (0.0f, 1.0f, x); return x * x * (3.0f - 2.0f * x); }

VoiceTargets targetsForPhoneme (const PhonemeInfo& cur, const PhonemeInfo* next, double u, double segDur,
                                float f0, float amp, float breath, float tension, float formantFactor)
{
    VoiceTargets t;
    t.f0 = f0;
    t.tension = tension;
    for (int i = 0; i < 5; ++i) { t.F[i] = cur.f[i]; t.B[i] = cur.bw[i]; }

    // diphthong glide in the last ~30 % (max 160 ms) of the vowel
    if (cur.cls == PhClass::Diphthong && cur.fEnd[0] > 0)
    {
        const double glide = juce::jmin (0.16, segDur * 0.3);
        const float g = smoothstep ((float) ((u * segDur - (segDur - glide)) / juce::jmax (1.0e-3, glide)));
        for (int i = 0; i < 3; ++i) t.F[i] = cur.f[i] + (cur.fEnd[i] - cur.f[i]) * g;
    }

    const PhonemeInfo* shape = &cur;   // whose formants the aspiration is filtered through
    switch (cur.cls)
    {
        case PhClass::Vowel:
        case PhClass::Diphthong:
            t.av = 1.0f; t.ah = 0.08f + breath * 0.7f; break;
        case PhClass::Nasal:
            t.av = 0.75f; t.nasal = cur.nasal; t.ah = breath * 0.2f; break;
        case PhClass::Liquid:
        case PhClass::Glide:
            t.av = 0.85f; t.ah = breath * 0.3f; break;
        case PhClass::Fricative:
            t.av = cur.voiced ? 0.45f : 0.0f;
            t.af = cur.fricGain;
            t.fricF = cur.fricCenter; t.fricBw = cur.fricBw;
            t.ah = cur.voiced ? 0.0f : 0.04f;
            break;
        case PhClass::Aspirate:
            t.av = 0.0f; t.ah = 0.9f;
            if (next != nullptr) shape = next;
            break;
        case PhClass::Affricate:
            if (u < 0.35) { t.av = cur.voiced ? 0.1f : 0.0f; }
            else { t.af = cur.fricGain; t.av = cur.voiced ? 0.4f : 0.0f; t.fricF = cur.fricCenter; t.fricBw = cur.fricBw; }
            break;
        case PhClass::Plosive:
            if (u < 0.6)       { t.av = cur.voiced ? 0.12f : 0.0f; }
            else if (u < 0.75) { t.af = cur.fricGain * 1.6f; t.fricF = cur.fricCenter; t.fricBw = cur.fricBw; t.av = cur.voiced ? 0.3f : 0.0f; }
            else
            {
                if (cur.voiced) t.av = 0.6f;
                else { t.ah = 0.6f; }
                if (next != nullptr) shape = next;
            }
            break;
        case PhClass::Silence:
        default:
            break;
    }

    if (shape != &cur)
        for (int i = 0; i < 5; ++i) { t.F[i] = shape->f[i]; t.B[i] = shape->bw[i]; }

    for (int i = 0; i < 5; ++i) t.F[i] *= formantFactor;

    // formant tuning: sopranos raise F1 to follow f0 on high notes
    if (isVowelClass (cur.cls) && t.F[0] < f0 * 1.1f)
        t.F[0] = f0 * 1.1f;

    t.av *= amp * (1.0f - 0.35f * breath);
    t.ah *= amp;
    t.af *= juce::jmax (0.25f, amp);
    return t;
}

//==============================================================================
void VoiceDSP::Reso::set (double f, double bw, double s)
{
    f = juce::jlimit (20.0, s * 0.45, f);
    const double T = 1.0 / s;
    c = -std::exp (-2.0 * juce::MathConstants<double>::pi * bw * T);
    b = 2.0 * std::exp (-juce::MathConstants<double>::pi * bw * T) * std::cos (2.0 * juce::MathConstants<double>::pi * f * T);
    a = 1.0 - b - c;
}

void VoiceDSP::AntiReso::set (double f, double bw, double s)
{
    Reso r; r.set (f, bw, s);
    a = 1.0 / r.a; b = -r.b / r.a; c = -r.c / r.a;
}

void VoiceDSP::Biquad::bandpass (double f, double q, double s)
{
    f = juce::jlimit (20.0, s * 0.45, f);
    const double w = 2.0 * juce::MathConstants<double>::pi * f / s;
    const double alpha = std::sin (w) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    b0 = alpha / a0; b1 = 0.0; b2 = -alpha / a0;
    a1 = -2.0 * std::cos (w) / a0; a2 = (1.0 - alpha) / a0;
}

void VoiceDSP::Biquad::peak (double f, double q, double gainDb, double s)
{
    f = juce::jlimit (20.0, s * 0.45, f);
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w = 2.0 * juce::MathConstants<double>::pi * f / s;
    const double alpha = std::sin (w) / (2.0 * q);
    const double a0 = 1.0 + alpha / A;
    b0 = (1.0 + alpha * A) / a0; b1 = -2.0 * std::cos (w) / a0; b2 = (1.0 - alpha * A) / a0;
    a1 = b1; a2 = (1.0 - alpha / A) / a0;
}

void VoiceDSP::prepare (double sampleRate)
{
    sr = sampleRate;
    ctrlInterval = juce::jmax (8, (int) std::round (sr / kControlRate));
    reset();
}

void VoiceDSP::reset()
{
    phase = 0.0; tiltState = aspHp = aspPrev = dcX = dcY = 0.0;
    for (auto& f : formants) f.y1 = f.y2 = 0.0;
    nasalPole.y1 = nasalPole.y2 = 0.0;
    nasalZero.x1 = nasalZero.x2 = 0.0;
    fric.z1 = fric.z2 = singer.z1 = singer.z2 = 0.0;
    cur = VoiceTargets();
    target = cur;
    ctrlCounter = 0;
    updateFilters();
}

void VoiceDSP::setTargets (const VoiceTargets& t, bool snap)
{
    target = t;
    if (snap)
    {
        for (int i = 0; i < 5; ++i) { cur.F[i] = t.F[i]; cur.B[i] = t.B[i]; }
        cur.f0 = t.f0;
        cur.fricF = t.fricF; cur.fricBw = t.fricBw;
        cur.nasal = t.nasal;
    }
}

void VoiceDSP::updateFilters()
{
    for (int i = 0; i < 5; ++i)
        formants[i].set (cur.F[i], cur.B[i] * (1.0 + 0.4 * cur.nasal), sr);

    nasalPole.set (270.0, 100.0, sr);
    nasalZero.set (270.0 + cur.nasal * 230.0, 100.0, sr);
    fric.bandpass (cur.fricF, juce::jmax (0.5, (double) cur.fricF / juce::jmax (100.0f, cur.fricBw)), sr);
    singer.peak (2900.0 * timbre.formantScale, 1.4, 1.0 + 5.0 * cur.tension, sr);
}

void VoiceDSP::process (float* out, int n)
{
    const double fCoef  = 1.0 - std::exp (-1.0 / (0.014 * kControlRate));  // formants: 14 ms
    const double aCoef  = 1.0 - std::exp (-1.0 / (0.004 * kControlRate));  // amplitudes: 4 ms
    const double fqCoef = 1.0 - std::exp (-1.0 / (0.002 * kControlRate));  // pitch: 2 ms

    for (int i = 0; i < n; ++i)
    {
        if (ctrlCounter-- <= 0)
        {
            ctrlCounter = ctrlInterval - 1;
            for (int k = 0; k < 5; ++k)
            {
                cur.F[k] += (float) ((target.F[k] - cur.F[k]) * fCoef);
                cur.B[k] += (float) ((target.B[k] - cur.B[k]) * fCoef);
            }
            cur.nasal  += (float) ((target.nasal - cur.nasal) * fCoef);
            cur.fricF  += (float) ((target.fricF - cur.fricF) * aCoef);
            cur.fricBw += (float) ((target.fricBw - cur.fricBw) * aCoef);
            cur.av += (float) ((target.av - cur.av) * aCoef);
            cur.ah += (float) ((target.ah - cur.ah) * aCoef);
            cur.af += (float) ((target.af - cur.af) * (aCoef * 1.5));
            cur.tension += (float) ((target.tension - cur.tension) * fCoef);
            cur.f0 += (float) ((target.f0 - cur.f0) * fqCoef);
            updateFilters();
        }

        // ---- glottal source (KLGLOTT88 derivative, polyBLEP at closure) ------
        const double f0 = juce::jlimit (30.0, sr * 0.2, (double) cur.f0) * periodJitter;
        const double dt = f0 / sr;
        phase += dt;
        if (phase >= 1.0)
        {
            phase -= 1.0;
            periodJitter  = 1.0 + (rng.nextDouble() * 2.0 - 1.0) * timbre.jitter;
            periodShimmer = 1.0 + (rng.nextDouble() * 2.0 - 1.0) * timbre.shimmer;
        }
        const double oq = 0.75 - 0.33 * cur.tension;
        double g = 0.0;
        if (phase < oq)
        {
            const double x = phase / oq;
            g = (2.0 * x - 3.0 * x * x);            // 0 -> peak 1/3 -> -1 at closure
        }
        // polyBLEP for the +1 jump at phase == oq
        {
            double p = phase - oq; if (p < 0.0) p += 1.0;
            const double d = dt / oq;   // step width relative to period
            const double w = juce::jmin (0.5, juce::jmax (dt, d * 0.5));
            if (p < w)            { const double t = p / w;         g += 0.5 * (t + t - t * t - 1.0); }
            else if (p > 1.0 - w) { const double t = (p - 1.0) / w; g += 0.5 * (t * t + t + t + 1.0); }
        }
        // spectral tilt (soft voice = darker)
        const double tiltFc = 1200.0 + 5000.0 * (0.5 * cur.tension + 0.5 * timbre.brightness);
        const double tiltA = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * tiltFc / sr);
        tiltState += (g - tiltState) * tiltA;
        const double voiced = tiltState * periodShimmer;

        // aspiration (high-passed noise, pulsed with the glottal cycle)
        const double nz = noise();
        aspHp = 0.97 * (aspHp + nz - aspPrev); aspPrev = nz;
        const double asp = aspHp * (phase < oq ? 1.0 : 0.45);

        double x = cur.av * voiced * 3.0 + (cur.ah + timbre.breathBase * cur.av) * asp * 0.35;

        // nasal pole/zero pair, then cascade formants (high to low)
        x = nasalZero.tick (nasalPole.tick (x));
        for (int k = 4; k >= 0; --k) x = formants[k].tick (x);

        // frication branch (parallel)
        x += fric.tick (noise()) * cur.af * 0.9;

        x = singer.tick (x);

        // DC blocker
        const double y = x - dcX + 0.995 * dcY; dcX = x; dcY = y;
        out[i] = (float) y;
    }
}

//==============================================================================
juce::AudioBuffer<float> SynthRenderer::render (const Timeline& tl, const ControlCurves& curves,
                                                const VoiceTimbre& timbre, const ExpressionSettings& settings,
                                                std::atomic<bool>* cancel, std::function<void (float)> progress)
{
    VoiceDSP dsp;
    dsp.prepare (kEngineSampleRate);
    dsp.setTimbre (timbre);

    const int ctrl = dsp.controlInterval();
    const int frames = curves.numFrames();
    juce::AudioBuffer<float> out (1, frames * ctrl + ctrl);
    out.clear();
    auto* w = out.getWritePointer (0);

    const float formantFactor = timbre.formantScale * std::pow (2.0f, settings.formantShift / 12.0f);
    size_t seg = 0;
    bool wasSilent = true;
    const auto silence = phonemeAt (0);

    for (int f = 0; f < frames; ++f)
    {
        if (cancel != nullptr && cancel->load()) break;
        if (progress && (f & 4095) == 0) progress ((float) f / (float) frames);

        const double t = f / curves.rate;
        while (seg < tl.segments.size() && tl.segments[seg].t1 <= t) ++seg;

        VoiceTargets tg;
        bool silent = true;
        if (seg < tl.segments.size() && tl.segments[seg].t0 <= t)
        {
            const auto& s = tl.segments[seg];
            const PhonemeInfo* next = nullptr;
            if (seg + 1 < tl.segments.size() && tl.segments[seg + 1].t0 - s.t1 < 0.002)
                next = &phonemeAt (tl.segments[seg + 1].ph);
            const double dur = juce::jmax (1.0e-4, s.t1 - s.t0);
            tg = targetsForPhoneme (phonemeAt (s.ph), next, (t - s.t0) / dur, dur,
                                    curves.f0Hz[(size_t) f], curves.amp[(size_t) f],
                                    curves.breath[(size_t) f], curves.tension[(size_t) f], formantFactor);
            silent = false;
        }
        else
        {
            // silence: keep formants of the next phoneme so the onset is clean
            const PhonemeInfo& ref = seg < tl.segments.size() ? phonemeAt (tl.segments[seg].ph) : silence;
            tg = targetsForPhoneme (ref, nullptr, 0.0, 0.1, curves.f0Hz[(size_t) f], 0.0f, 0.0f, 0.5f, formantFactor);
            tg.av = tg.ah = tg.af = 0.0f;
        }
        dsp.setTargets (tg, wasSilent && ! silent);
        wasSilent = silent;
        dsp.process (w + f * ctrl, ctrl);
    }

    // normalise to -3 dBFS peak (dynamics are preserved, overall level is consistent)
    const float peak = out.getMagnitude (0, 0, out.getNumSamples());
    if (peak > 1.0e-6f) out.applyGain (0.707f / peak);
    if (progress) progress (1.0f);
    return out;
}

//==============================================================================
void LiveVoice::prepare (double sampleRate, int maxBlock)
{
    sr = sampleRate;
    dsp.prepare (sampleRate);
    scratch.assign ((size_t) juce::jmax (maxBlock, 64), 0.0f);
    state = State::Idle;
    held.clear();
    currentNote = -1;
}

void LiveVoice::setSyllables (std::vector<juce::StringArray> s)
{
    const juce::SpinLock::ScopedLockType sl (sylLock);
    pendingSyllables = std::move (s);
    syllablesDirty = true;
}

void LiveVoice::setTimbre (const VoiceTimbre& t)
{
    const juce::SpinLock::ScopedLockType sl (timbreLock);
    pendingTimbre = t;
    timbreDirty = true;
}

void LiveVoice::controller (int cc, float value)
{
    if (cc == 1)  mod = value;
    if (cc == 2)  breathCC = value;
    if (cc == 11) expr = value;
    if (cc == 120 || cc == 123) allNotesOff();
}

void LiveVoice::allNotesOff()
{
    held.clear();
    if (state != State::Idle) { state = State::Release; releaseTime = 0.0; }
    currentNote = -1;
}

void LiveVoice::startSyllable (bool legato)
{
    onset.clear(); coda.clear(); nucleus = phonemeIndex ("aa");
    if (! syllables.empty())
    {
        const auto& ph = syllables[sylIndex % syllables.size()];
        ++sylIndex;
        if (ph.isEmpty() && legato) { state = State::Nucleus; return; }   // hold "-": keep the vowel
        int v = -1;
        for (int i = 0; i < ph.size(); ++i) if (isVowel (phonemeIndex (ph[i]))) { v = i; break; }
        if (v >= 0)
        {
            for (int i = 0; i < v; ++i) onset.add (ph[i]);
            for (int i = v + 1; i < ph.size(); ++i) coda.add (ph[i]);
            nucleus = phonemeIndex (ph[v]);
        }
    }
    seqPos = 0; segTime = 0.0;
    if (onset.isEmpty()) { state = State::Nucleus; }
    else
    {
        state = State::Onset;
        segDur = phonemeAt (juce::jmax (0, phonemeIndex (onset[0]))).typicalMs * 0.001 * (legato ? 0.6 : 0.8);
    }
}

void LiveVoice::noteOn (int note, float vel)
{
    const bool legato = currentNote >= 0;
    held.push_back (note);
    currentNote = note;
    velocity = vel;
    targetPitch = note;
    if (! legato) { pitch = note; noteTime = 0.0; vibPhase = 0.0; }
    else noteTime = 0.0;
    startSyllable (legato);
}

void LiveVoice::noteOff (int note)
{
    held.erase (std::remove (held.begin(), held.end(), note), held.end());
    if (note != currentNote) return;
    if (! held.empty())
    {
        currentNote = held.back();
        targetPitch = currentNote;
        return;
    }
    currentNote = -1;
    seqPos = 0; segTime = 0.0;
    if (! coda.isEmpty())
    {
        state = State::Coda;
        segDur = phonemeAt (juce::jmax (0, phonemeIndex (coda[0]))).typicalMs * 0.001;
    }
    else { state = State::Release; releaseTime = 0.0; }
}

void LiveVoice::tick()
{
    const double dt = dsp.controlInterval() / sr;
    noteTime += dt;
    segTime += dt;

    // pitch glide + vibrato + bend
    const double portaTau = juce::jmax (0.005, settings.portamentoMs * 0.001 * 0.35);
    pitch += (targetPitch - pitch) * (1.0 - std::exp (-dt / portaTau));
    double cents = 0.0;
    const double delay = settings.vibratoDelayMs * 0.001;
    if (noteTime > delay)
    {
        vibPhase += juce::MathConstants<double>::twoPi * settings.vibratoRateHz * dt;
        const double fade = juce::jlimit (0.0, 1.0, (noteTime - delay) / 0.25);
        cents = settings.vibratoDepthCents * (1.0 + 1.5 * mod) * fade * std::sin (vibPhase);
    }
    const float f0 = midiToHz ((float) (pitch + bend * settings.pitchBendRange + cents * 0.01));
    const float amp = (0.35f + 0.65f * velocity) * (0.4f + 0.6f * settings.expression) * expr;
    const float breath = juce::jlimit (0.0f, 1.0f, settings.breathiness + 0.6f * breathCC);
    const float tension = juce::jlimit (0.0f, 1.0f, settings.tension + (velocity - 0.7f) * 0.3f);

    const PhonemeInfo* curPh = &phonemeAt (nucleus);
    const PhonemeInfo* next = nullptr;
    double u = 0.5, dur = 1.0;
    float level = amp;

    switch (state)
    {
        case State::Onset:
            if (segTime >= segDur)
            {
                ++seqPos; segTime = 0.0;
                if (seqPos >= onset.size()) { state = State::Nucleus; break; }
                segDur = phonemeAt (juce::jmax (0, phonemeIndex (onset[seqPos]))).typicalMs * 0.001 * 0.8;
            }
            if (state == State::Onset)
            {
                curPh = &phonemeAt (juce::jmax (0, phonemeIndex (onset[seqPos])));
                next = seqPos + 1 < onset.size() ? &phonemeAt (juce::jmax (0, phonemeIndex (onset[seqPos + 1]))) : &phonemeAt (nucleus);
                u = segTime / segDur; dur = segDur;
            }
            break;
        case State::Coda:
            if (segTime >= segDur)
            {
                ++seqPos; segTime = 0.0;
                if (seqPos >= coda.size()) { state = State::Release; releaseTime = 0.0; break; }
                segDur = phonemeAt (juce::jmax (0, phonemeIndex (coda[seqPos]))).typicalMs * 0.001;
            }
            if (state == State::Coda)
            {
                curPh = &phonemeAt (juce::jmax (0, phonemeIndex (coda[seqPos])));
                u = segTime / segDur; dur = segDur;
            }
            break;
        case State::Release:
            releaseTime += dt;
            level = amp * (float) juce::jmax (0.0, 1.0 - releaseTime / 0.08);
            if (releaseTime > 0.1) state = State::Idle;
            break;
        case State::Nucleus:
        case State::Idle:
            break;
    }
    if (state == State::Nucleus) { curPh = &phonemeAt (nucleus); u = juce::jmin (0.5, noteTime); dur = 10.0; }

    auto tg = targetsForPhoneme (*curPh, next, u, dur, f0, state == State::Idle ? 0.0f : level, breath, tension, formantFactor);
    dsp.setTargets (tg);
}

void LiveVoice::render (juce::AudioBuffer<float>& buffer, int start, int num, float gain)
{
    {
        const juce::SpinLock::ScopedTryLockType sl (sylLock);
        if (sl.isLocked() && syllablesDirty) { std::swap (syllables, pendingSyllables); syllablesDirty = false; sylIndex = 0; }   // swap: no allocation on the audio thread
    }
    {
        const juce::SpinLock::ScopedTryLockType sl (timbreLock);
        if (sl.isLocked() && timbreDirty) { dsp.setTimbre (pendingTimbre); formantFactor = pendingTimbre.formantScale; timbreDirty = false; }
    }
    if (state == State::Idle) return;

    const float ff = formantFactor * std::pow (2.0f, settings.formantShift / 12.0f);
    const float keep = formantFactor;
    formantFactor = ff;

    int done = 0;
    const int ctrl = dsp.controlInterval();
    while (done < num)
    {
        const int chunk = juce::jmin (ctrl, num - done, (int) scratch.size());
        tick();
        dsp.process (scratch.data(), chunk);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* w = buffer.getWritePointer (ch, start + done);
            for (int i = 0; i < chunk; ++i)
                w[i] += std::tanh (scratch[(size_t) i] * 0.35f) * gain;
        }
        done += chunk;
    }
    formantFactor = keep;
}

} // namespace vf
