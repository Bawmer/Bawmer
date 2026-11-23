#pragma once

#include "PitchDetector.h"

namespace vf
{

struct DetectedNote
{
    double t0 = 0.0, t1 = 0.0;   // seconds in the stem
    int    pitch = 60;
    float  velocity = 0.8f;
    float  meanMidi = 60.0f;     // un-rounded centre pitch
};

struct VocalToMidiOptions
{
    float minNoteMs       = 90.0f;   // shorter fragments are merged
    float splitSemitones  = 0.75f;   // sustained pitch change that starts a new note
    float splitHoldMs     = 60.0f;   // how long the change must last
    float onsetDb         = 7.0f;    // energy jump that starts a new note (repeated syllables)
    float sensitivity     = 0.5f;    // 0 = only clear notes, 1 = catch everything
};

struct VocalAnalysis
{
    F0Track f0;
    std::vector<DetectedNote> notes;
};

/** Vocal-to-MIDI: pitch tracking + note segmentation. */
VocalAnalysis vocalToMidi (const juce::AudioBuffer<float>& mono, double sampleRate,
                           const VocalToMidiOptions& opts = {}, std::atomic<bool>* cancel = nullptr);

} // namespace vf
