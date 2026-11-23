#pragma once

#include <JuceHeader.h>
#include <vector>

namespace vf
{

/** Frame-wise f0 track. */
struct F0Track
{
    double frameRate = 100.0;          // frames per second
    std::vector<float> hz;             // 0 = unvoiced
    std::vector<float> periodicity;    // 0..1 (1 = perfectly periodic)
    std::vector<float> rmsDb;          // frame energy

    int size() const noexcept { return (int) hz.size(); }
    float hzAt (double seconds) const;
};

struct PitchOptions
{
    float fminHz = 60.0f;
    float fmaxHz = 1100.0f;
    float threshold = 0.15f;      // YIN threshold
    float voicingGateDb = -50.0f; // absolute energy gate
    double hopSeconds = 0.01;
};

/** FFT-accelerated YIN pitch tracker (runs internally at 16 kHz). */
F0Track detectPitch (const juce::AudioBuffer<float>& mono, double sampleRate, const PitchOptions& opts = {},
                     std::atomic<bool>* cancel = nullptr);

} // namespace vf
