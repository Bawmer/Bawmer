#pragma once

#include <JuceHeader.h>

namespace vf
{

enum class PhClass
{
    Vowel, Diphthong, Nasal, Liquid, Glide, Fricative, Affricate, Plosive, Aspirate, Silence
};

/** Articulatory targets for one ARPAbet phoneme (adult reference voice). */
struct PhonemeInfo
{
    const char* name;
    PhClass cls;
    bool  voiced;
    float f[5];          // formant targets (Hz)
    float bw[5];         // bandwidths (Hz)
    float fEnd[3];       // diphthong end target F1..F3 (0 = none)
    float fricCenter;    // frication noise centre (Hz)
    float fricBw;        // frication bandwidth (Hz)
    float fricGain;      // frication level 0..1
    float nasal;         // nasal coupling 0..1
    float typicalMs;     // natural duration when used as onset / coda
};

/** Phoneme table access. Index 0 is always silence ("sil"). */
int phonemeCount() noexcept;
const PhonemeInfo& phonemeAt (int index) noexcept;
int phonemeIndex (const juce::String& arpabet) noexcept;      // -1 when unknown (stress digits ignored)
inline bool isVowelClass (PhClass c) noexcept { return c == PhClass::Vowel || c == PhClass::Diphthong; }
bool isVowel (int index) noexcept;

} // namespace vf
