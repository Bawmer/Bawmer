#pragma once

#include "VoicePreset.h"
#include <functional>

namespace vf
{

struct SampleReport
{
    juce::File file;
    double seconds = 0.0, voicedSeconds = 0.0;
    float peakDb = -120.0f, noiseFloorDb = -120.0f, clippingPercent = 0.0f;
    float minMidi = 0.0f, maxMidi = 0.0f;
    juce::StringArray warnings;
    bool usable = true;
};

struct VoiceAnalysisResult
{
    bool ok = false;
    juce::String error;
    VoiceTimbre timbre;
    VoiceStats stats;
    ExpressionSettings suggested;
    std::vector<SampleReport> reports;
    juce::StringArray warnings;
};

/** "Lite clone": analyses dry singing samples and derives the timbre of the
    built-in engine (formant scale, spectral envelope match, breathiness,
    vibrato habits, range). Runs in a few seconds, no GPU needed. */
VoiceAnalysisResult analyseVoiceSamples (const juce::Array<juce::File>& files,
                                         std::atomic<bool>* cancel = nullptr,
                                         std::function<void (float, const juce::String&)> progress = nullptr);

/** Quick quality check of a single file (used when samples are added). */
SampleReport inspectSample (const juce::File& file);

} // namespace vf
