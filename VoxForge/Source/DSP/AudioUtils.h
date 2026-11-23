#pragma once

#include <JuceHeader.h>

namespace vf
{

/** Loads any supported audio file (wav/aiff/flac/ogg/mp3 where available) mixed to mono. */
bool loadAudioMono (const juce::File& file, juce::AudioBuffer<float>& out, double& sampleRate,
                    double maxSeconds = 60.0 * 30.0);

/** High-quality resampling (anti-alias low-pass when downsampling + windowed sinc). */
juce::AudioBuffer<float> resampleMono (const juce::AudioBuffer<float>& in, double inRate, double outRate);

bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate, int bits = 24);

juce::AudioFormatManager& formatManager();

} // namespace vf
