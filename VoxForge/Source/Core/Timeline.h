#pragma once

#include "VocalTypes.h"
#include <functional>

namespace vf
{

/** One phoneme placed in time (seconds, relative to project beat 0). */
struct PhonemeSegment
{
    int    ph       = 0;      // index into phoneme table
    double t0       = 0.0;
    double t1       = 0.0;
    int    note     = -1;     // index of the owning note (first note of the syllable)
    bool   nucleus  = false;  // the sustained vowel of a syllable
};

struct Timeline
{
    std::vector<PhonemeSegment> segments;
    double endTime = 0.0;
};

/** Builds the phoneme timeline from notes + lyrics.
    Singer-style timing: onset consonants are placed *before* the note start so
    the vowel lands on the beat; coda consonants close the syllable at the end.
    @param include  optional per-note mask (only those notes are sung). */
Timeline buildTimeline (const VocalProject& project, const std::vector<bool>* include = nullptr);

/** Frame-rate control curves that drive the synth / neural voice. */
struct ControlCurves
{
    double rate = kControlRate;
    std::vector<float> f0Hz;     // target pitch incl. vibrato, bend, portamento, drift
    std::vector<float> amp;      // overall dynamics 0..1
    std::vector<float> breath;   // aspiration 0..1
    std::vector<float> tension;  // 0..1
    int numFrames() const noexcept { return (int) f0Hz.size(); }
};

/** Extra pitch in cents as a function of time (used by vocal replacement to
    re-apply the nuance of the original performance). */
using PitchNuanceFn = std::function<float (double timeSec)>;

ControlCurves buildCurves (const VocalProject& project,
                           const ExpressionSettings& settings,
                           double durationSec,
                           const std::vector<bool>* include = nullptr,
                           const PitchNuanceFn& nuance = nullptr);

inline float midiToHz (float midi) noexcept { return 440.0f * std::pow (2.0f, (midi - 69.0f) / 12.0f); }
inline float hzToMidi (float hz) noexcept   { return hz > 0.0f ? 69.0f + 12.0f * std::log2 (hz / 440.0f) : 0.0f; }

} // namespace vf
