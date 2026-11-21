#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <lumen/CompressorEngine.h>

namespace lumen::ids
{
inline constexpr const char* mode = "mode";
inline constexpr const char* threshold = "threshold";
inline constexpr const char* ratio = "ratio";
inline constexpr const char* attack = "attack";
inline constexpr const char* release = "release";
inline constexpr const char* knee = "knee";
inline constexpr const char* makeup = "makeup";
inline constexpr const char* mix = "mix";
inline constexpr const char* link = "link";
inline constexpr const char* scHpfOn = "scHpfOn";
inline constexpr const char* scHpf = "scHpf";
inline constexpr const char* scLpfOn = "scLpfOn";
inline constexpr const char* scLpf = "scLpf";
inline constexpr const char* releaseMode = "releaseMode";
inline constexpr const char* hold = "hold";
inline constexpr const char* autoGain = "autoGain";
inline constexpr const char* lookahead = "lookahead";
inline constexpr const char* lookaheadMode = "lookaheadMode";
inline constexpr const char* detection = "detection";
inline constexpr const char* extSc = "extSc";
inline constexpr const char* channelMode = "channelMode";
inline constexpr const char* topology = "topology";
inline constexpr const char* analogModel = "analogModel";
inline constexpr const char* blend = "blend";
inline constexpr const char* drive = "drive";
inline constexpr const char* character = "character";
inline constexpr const char* colorMix = "colorMix";
inline constexpr const char* transformer = "transformer";
inline constexpr const char* amplifier = "amplifier";
inline constexpr const char* oversampling = "oversampling";
inline constexpr const char* osQuality = "osQuality";
inline constexpr const char* bypass = "bypass";
inline constexpr const char* matchBypass = "matchBypass";
} // namespace lumen::ids

namespace lumen
{
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

/** Cached raw parameter pointers, read once per block on the audio thread. */
class ParameterReader
{
public:
    explicit ParameterReader (juce::AudioProcessorValueTreeState& state);
    CompressorParams read() const noexcept;

private:
    std::atomic<float>* get (juce::AudioProcessorValueTreeState& s, const char* id);

    std::atomic<float> *mode, *threshold, *ratio, *attack, *release, *knee, *makeup, *mix, *link, *scHpfOn, *scHpf,
        *scLpfOn, *scLpf, *releaseMode, *hold, *autoGain, *lookahead, *lookaheadMode, *detection, *extSc, *channelMode, *topology, *analogModel, *blend,
        *drive, *character, *colorMix, *transformer, *amplifier, *oversampling, *osQuality, *bypass, *matchBypass;
};
} // namespace lumen
