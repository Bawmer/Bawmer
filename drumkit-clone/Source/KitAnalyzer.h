#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>
#include <vector>

namespace dkc
{
constexpr int kMaxPads = 16;

enum PadType { Kick = 0, Snare, Clap, Hat, OpenHat, Tom, Perc, Cymbal, numPadTypes };

const char* padTypeName (int type);

/** One isolated one-shot, ready to play. */
struct PadSample
{
    juce::String name;
    int type = Perc;
    int hitCount = 0;                 // how many hits in the source were grouped into this pad
    double sampleRate = 44100.0;
    juce::AudioBuffer<float> audio;
    std::vector<float> thumb;         // peak envelope for drawing

    void rebuildThumb (int points = 96);
};

/** A detected hit, for the overview display. */
struct HitMark
{
    float position = 0.0f;            // 0..1 through the source
    int pad = -1;                     // -1 = not assigned to a pad
};

struct Kit : public juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<Kit>;

    juce::String sourceName;
    double sourceSeconds = 0.0;
    int totalHits = 0;
    std::vector<PadSample> pads;
    std::vector<float> overview;      // peak envelope of the whole source
    std::vector<HitMark> hits;
};

struct AnalysisSettings
{
    float sensitivity = 0.5f;         // 0..1, higher finds quieter hits
    float separation = 0.5f;          // 0..1, higher splits similar sounds into more pads
    int maxPads = kMaxPads;
};

/** progress callback: receives 0..1, return false to cancel. */
using ProgressFn = std::function<bool (float)>;

class KitAnalyzer
{
public:
    /** Finds hits, groups them by sound and isolates a one-shot per group.
        Returns nullptr if cancelled. */
    static Kit::Ptr analyse (const juce::AudioBuffer<float>& source, double sampleRate,
                             const AnalysisSettings& settings, const ProgressFn& progress);

    /** Loads (up to 10 minutes of) an audio file and analyses it. */
    static Kit::Ptr analyseFile (const juce::File& file, const AnalysisSettings& settings,
                                 const ProgressFn& progress, juce::String& error);
};
}
