#pragma once

#include <utility>
#include <vector>

namespace vf
{

/** Time-aligns a spoken phrase onto the sung timing of the same words.

    Both inputs are HuBERT content units ([frames x dim], row-major, 50 frames/s). Content units
    are largely speaker- and pitch-independent, so the built-in guide vocal (which already has
    the note timing) and the Voicebox speech (which has natural pronunciation) can be matched
    frame by frame with dynamic time warping on cosine distance.

    Returns guideFrames x dim units: for every guide frame, the mean of the speech frames the
    warping path assigns to it. Vowels are stretched by repeating speech frames, consonants keep
    their natural length wherever the guide gives them room. */
std::vector<float> warpUnitsOntoGuide (const float* guide, int guideFrames,
                                       const float* speech, int speechFrames, int dim,
                                       float* meanCost = nullptr);

/** Index range of the non-silent part of a mono signal (threshold relative to its peak),
    padded by padSamples. Returns {0, 0} for silence. */
std::pair<int, int> trimSilence (const float* x, int n, float thresholdDb = -38.0f, int padSamples = 800);

} // namespace vf
