#pragma once

#include "VocalTypes.h"

namespace vf
{

/** One sung syllable: display text and its phonemes (ARPAbet, lower case). */
struct Syllable
{
    juce::String text;
    juce::StringArray phonemes;
    bool isHold = false;          // "-" : continue previous vowel on a new note (melisma)
    bool wordStart = false;       // first syllable of a word
};

/** English grapheme-to-phoneme lexicon.
    Lookup order: user dictionary (cmudict.dict in the data folder) -> built-in
    singing dictionary -> letter-to-sound rules. */
class Lexicon
{
public:
    static Lexicon& get();

    /** Loads a CMU-format dictionary ("WORD  W ER1 D" per line). Returns entries loaded. */
    int loadDictionary (const juce::File& file);

    juce::StringArray lookup (const juce::String& word) const;
    bool hasUserDictionary() const noexcept { return userDictSize > 0; }

private:
    Lexicon();
    juce::StringArray letterToSound (const juce::String& word) const;

    std::unordered_map<std::string, juce::StringArray> builtin, user;
    int userDictSize = 0;
    mutable juce::CriticalSection lock;
};

/** Splits a phoneme sequence into syllables (one vowel nucleus each, maximal onset). */
std::vector<juce::StringArray> syllabifyPhonemes (const juce::StringArray& phonemes);

/** Splits an orthographic word into n display chunks ("happy",2 -> "hap","py"). */
juce::StringArray splitWordForDisplay (const juce::String& word, int n);

/** Tokenises free lyrics text into syllables. Supports:
      hap-py      explicit syllable split
      -           hold previous vowel over the next note
      [hh ax l]   explicit phonemes for one syllable                         */
std::vector<Syllable> lyricsToSyllables (const juce::String& text);

/** Assigns lyrics to notes in order, starting at note index firstNote. */
void applyLyricsToNotes (std::vector<Note>& notes, const juce::String& text, int firstNote = 0);

/** Phonemes to sing on a note (empty for a hold). */
juce::StringArray phonemesForNote (const Note& note);

/** Phonemes for a single typed syllable (G2P or explicit [..]). */
juce::StringArray phonemesForText (const juce::String& text);

} // namespace vf
