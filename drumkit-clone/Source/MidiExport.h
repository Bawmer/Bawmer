#pragma once

#include "KitAnalyzer.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace dkc
{
/** Turns the hits found in the source into MIDI: one part per pad (Kick, Snare, Hat...). */
struct MidiOptions
{
    enum NoteMap { padNotes = 0, generalMidi };
    enum FileLayout { filePerDrum = 0, multiTrackFile };

    double bpm = 120.0;
    int quantize = 0;                 // 0 = off, otherwise grid steps per beat (2 = 1/8, 4 = 1/16, 8 = 1/32)
    int noteMap = padNotes;
    int layout = filePerDrum;
};

class MidiExport
{
public:
    static constexpr int ticksPerQuarter = 960;
    static constexpr int padRootNote = 36;   // Drumkit Clone pad 1 = C1

    /** The MIDI note a pad is written with. */
    static int noteFor (const Kit&, int pad, int noteMap);

    /** A sequence of the pad's hits, timestamps in ticks. */
    static juce::MidiMessageSequence padSequence (const Kit&, int pad, const MidiOptions&);

    /** One-track file (tempo, name and notes) for a single pad. */
    static juce::MidiFile padFile (const Kit&, int pad, const MidiOptions&);

    /** Type-1 file: a tempo track, then one named track per pad. */
    static juce::MidiFile multiTrackFile (const Kit&, const MidiOptions&);

    static bool write (const juce::MidiFile&, const juce::File&);

    /** Writes the MIDI to drag into a DAW: one file per drum, or one multi-track file.
        pad = -1 for every pad. Returns the files written (empty on failure). */
    static juce::StringArray writeForDrag (const Kit&, const MidiOptions&, const juce::File& folder, int pad = -1);
};
}
