#pragma once

#include "../Core/Timeline.h"
#include "../Core/Phonemes.h"

namespace vf
{

/** Timbre parameters of a voice used by the built-in (DSP) engine.
    The "Lite clone" analysis of the user's samples fills these in. */
struct VoiceTimbre
{
    float formantScale = 1.0f;      // vocal-tract length factor (female ~1.15, male ~0.95)
    float jitter       = 0.004f;    // period jitter (fraction)
    float shimmer      = 0.03f;     // amplitude shimmer (fraction)
    float breathBase   = 0.0f;      // extra aspiration measured from the samples
    float brightness   = 0.5f;      // source tilt 0 dark .. 1 bright
    std::vector<float> correctionDb; // spectral correction per analysis band (see Spectral.h)
};

/** Instantaneous synthesis targets (updated at control rate). */
struct VoiceTargets
{
    float f0 = 220.0f;
    float av = 0.0f;       // voicing amplitude
    float ah = 0.0f;       // aspiration amplitude
    float af = 0.0f;       // frication amplitude
    float fricF = 5000.0f, fricBw = 3000.0f;
    float F[5] { 500, 1500, 2500, 3300, 3800 };
    float B[5] { 80, 100, 140, 250, 300 };
    float nasal = 0.0f;
    float tension = 0.5f;
};

/** Computes synthesis targets for one phoneme.
    @param u        position inside the segment 0..1
    @param segDur   segment duration in seconds (for diphthong glide timing) */
VoiceTargets targetsForPhoneme (const PhonemeInfo& cur, const PhonemeInfo* next, double u, double segDur,
                                float f0, float amp, float breath, float tension, float formantFactor);

//==============================================================================
/** Klatt-style cascade formant synthesiser with a band-limited glottal source.
    Sample-rate independent (works at 44.1k ... 192k). */
class VoiceDSP
{
public:
    void prepare (double sampleRate);
    void reset();
    /** Copies only the scalar timbre fields (the DSP never needs correctionDb), so
        this is allocation-free and safe on the audio thread. */
    void setTimbre (const VoiceTimbre& t)
    {
        timbre.formantScale = t.formantScale; timbre.jitter = t.jitter; timbre.shimmer = t.shimmer;
        timbre.breathBase = t.breathBase; timbre.brightness = t.brightness;
    }
    void setTargets (const VoiceTargets& t, bool snap = false);
    /** Renders n samples (overwrites out). Targets are smoothed internally. */
    void process (float* out, int n);
    int controlInterval() const noexcept { return ctrlInterval; }

private:
    struct Reso
    {
        double a = 1, b = 0, c = 0, y1 = 0, y2 = 0;
        void set (double f, double bw, double sr);
        inline double tick (double x) { const double y = a * x + b * y1 + c * y2; y2 = y1; y1 = y; return y; }
    };
    struct AntiReso
    {
        double a = 1, b = 0, c = 0, x1 = 0, x2 = 0;
        void set (double f, double bw, double sr);
        inline double tick (double x) { const double y = a * x + b * x1 + c * x2; x2 = x1; x1 = x; return y; }
    };
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void bandpass (double f, double q, double sr);
        void peak (double f, double q, double gainDb, double sr);
        inline double tick (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    };

    void updateFilters();
    float noise() noexcept { seed = seed * 1664525u + 1013904223u; return (float) ((int32_t) seed) * (1.0f / 2147483648.0f); }

    double sr = kEngineSampleRate;
    int ctrlInterval = 32, ctrlCounter = 0;
    VoiceTimbre timbre;
    VoiceTargets target, cur;
    double phase = 0.0, periodJitter = 1.0, periodShimmer = 1.0;
    double tiltState = 0.0, aspHp = 0.0, aspPrev = 0.0, dcX = 0.0, dcY = 0.0;
    Reso formants[5], nasalPole;
    AntiReso nasalZero;
    Biquad fric, singer;
    uint32_t seed = 22222u;
    juce::Random rng { 777 };
};

//==============================================================================
/** Offline renderer: phoneme timeline + control curves -> mono audio at kEngineSampleRate. */
class SynthRenderer
{
public:
    static juce::AudioBuffer<float> render (const Timeline& tl, const ControlCurves& curves,
                                            const VoiceTimbre& timbre, const ExpressionSettings& settings,
                                            std::atomic<bool>* cancel = nullptr,
                                            std::function<void (float)> progress = nullptr);
};

//==============================================================================
/** Real-time monophonic singing voice for "Live" monitoring from DAW MIDI.
    Sings the project's syllables one per note-on. */
class LiveVoice
{
public:
    void prepare (double sampleRate, int maxBlock);
    void setSyllables (std::vector<juce::StringArray> syllables);   // message thread
    void setTimbre (const VoiceTimbre& t);                          // message thread
    void setSettings (const ExpressionSettings& s) { settings = s; }
    void resetSyllablePosition() { sylIndex = 0; }

    void noteOn (int note, float velocity);
    void noteOff (int note);
    void pitchWheel (float value) { bend = value; }               // -1..1
    void controller (int cc, float value);
    void allNotesOff();

    /** Adds the voice into the buffer (all channels). */
    void render (juce::AudioBuffer<float>& buffer, int start, int num, float gain);
    bool isActive() const noexcept { return state != State::Idle; }

private:
    enum class State { Idle, Onset, Nucleus, Coda, Release };
    void startSyllable (bool legato);
    void tick();

    VoiceDSP dsp;
    double sr = 48000.0;
    ExpressionSettings settings;
    juce::SpinLock sylLock;
    std::vector<juce::StringArray> syllables, pendingSyllables;
    bool syllablesDirty = false;
    juce::SpinLock timbreLock;
    VoiceTimbre pendingTimbre;
    bool timbreDirty = false;
    float formantFactor = 1.0f;

    size_t sylIndex = 0;
    juce::StringArray onset, coda;
    int nucleus = 1;
    int seqPos = 0;
    double segTime = 0.0, segDur = 0.0, noteTime = 0.0, releaseTime = 0.0;
    State state = State::Idle;

    std::vector<int> held;
    int currentNote = -1;
    float velocity = 0.8f, bend = 0.0f, mod = 0.0f, breathCC = 0.0f, expr = 1.0f;
    double pitch = 60.0, targetPitch = 60.0, vibPhase = 0.0;
    std::vector<float> scratch;
};

} // namespace vf
