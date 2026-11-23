#pragma once

#include <JuceHeader.h>
#include <vector>

namespace vf
{

/** Internal render rate of the vocal engine. Everything is rendered at this
    rate and then resampled to the host rate (44.1 kHz ... 192 kHz). */
static constexpr double kEngineSampleRate = 48000.0;

/** Control-rate used for pitch / dynamics / formant curves (frames per second). */
static constexpr double kControlRate = 1500.0;   // 32 samples @ 48 kHz

//==============================================================================
/** One sung note. Timing lives in beats (quarter notes) so it follows the DAW grid. */
struct Note
{
    double startBeat   = 0.0;
    double lengthBeats = 1.0;
    int    pitch       = 60;          // MIDI note number
    float  velocity    = 0.8f;        // 0..1
    juce::String lyric = "la";        // syllable shown on the note, "-" = hold previous vowel (melisma)
    juce::String phonemes;            // optional explicit ARPAbet override, e.g. "hh ax l ow"
    float  pitchOffsetCents = 0.0f;   // fine tune per note
    bool   wordStart = true;          // syllable starts a new word ("hap" true, "py" false)

    // ---- vocal replacement -------------------------------------------------
    bool   fromSource   = false;      // note was detected in the loaded stem
    double srcStartSec  = -1.0;       // where it was sung in the stem
    double srcEndSec    = -1.0;
    juce::String srcLyric;            // lyric recognised in the stem

    // ---- UI only (not saved) -----------------------------------------------
    bool selected = false;

    double endBeat() const noexcept { return startBeat + lengthBeats; }
    bool lyricChangedFromSource() const { return fromSource && srcLyric.isNotEmpty()
                                                 && lyric.trim().toLowerCase() != srcLyric.trim().toLowerCase(); }
};

/** A breakpoint of a captured MIDI controller curve (pitch-bend, CC1, CC2, CC11). */
struct ControlPoint
{
    double beat  = 0.0;
    float  value = 0.0f;
};

/** Snapshot of all expression parameters taken when a render starts. */
struct ExpressionSettings
{
    float vibratoDepthCents = 35.0f;
    float vibratoRateHz     = 5.5f;
    float vibratoDelayMs    = 280.0f;
    float breathiness       = 0.15f;  // 0..1
    float pitchBendRange    = 2.0f;   // semitones
    float portamentoMs      = 70.0f;
    float expression        = 0.75f;  // 0..1 dynamics
    float tension           = 0.5f;   // 0 soft .. 1 pressed
    float formantShift      = 0.0f;   // semitones, -6..+6
    float humanize          = 0.35f;  // pitch drift / timing jitter amount 0..1
    float nuance            = 0.6f;   // replacement: how much original pitch nuance to keep 0..1
};

//==============================================================================
/** The whole editable vocal: notes, lyrics and captured controller curves. */
struct VocalProject
{
    std::vector<Note> notes;
    juce::String lyricsText;
    double bpm = 120.0;

    std::vector<ControlPoint> pitchBend;   // -1..1
    std::vector<ControlPoint> modWheel;    // CC1  0..1 -> extra vibrato
    std::vector<ControlPoint> breathCC;    // CC2  0..1 -> extra breath
    std::vector<ControlPoint> expressionCC;// CC11 0..1 -> dynamics

    // ---- vocal replacement ------------------------------------------------
    juce::String stemPath;
    double stemOffsetBeats = 0.0;          // where the stem starts on the DAW timeline
    std::vector<float> srcF0;              // detected f0 of the stem in MIDI (0 = unvoiced), not saved
    double srcF0FrameRate = 100.0;

    double beatsToSeconds (double beats) const noexcept { return beats * 60.0 / juce::jmax (1.0, bpm); }
    double secondsToBeats (double secs)  const noexcept { return secs * juce::jmax (1.0, bpm) / 60.0; }

    void sortNotes();
    double lengthInBeats() const;
    void clearControllers();

    juce::ValueTree toValueTree() const;
    void fromValueTree (const juce::ValueTree& v);

    /** Hash of everything that influences the rendered audio (used for caching). */
    juce::String contentHash (const ExpressionSettings&, const juce::String& voiceId) const;
};

/** Linear interpolation of a controller curve at a beat position (holds ends). */
float evalControl (const std::vector<ControlPoint>& curve, double beat, float defaultValue);

} // namespace vf
