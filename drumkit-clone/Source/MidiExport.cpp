#include "MidiExport.h"

namespace dkc
{
namespace
{
constexpr int kDrumChannel = 10;

juce::MidiMessageSequence tempoTrack (double bpm, const juce::String& name)
{
    juce::MidiMessageSequence seq;
    if (name.isNotEmpty())
        seq.addEvent (juce::MidiMessage::textMetaEvent (3, name), 0.0);
    seq.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::lround (60.0e6 / juce::jlimit (20.0, 400.0, bpm))), 0.0);
    seq.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0.0);
    return seq;
}
}

int MidiExport::noteFor (const Kit& kit, int pad, int noteMap)
{
    if (noteMap != MidiOptions::generalMidi || ! juce::isPositiveAndBelow (pad, (int) kit.pads.size()))
        return padRootNote + pad;

    // GM drum map; repeated types get the next variant (Kick 2 -> 35, Tom 2 -> 47 ...)
    int nth = 0;
    for (int i = 0; i < pad; ++i)
        nth += kit.pads[(size_t) i].type == kit.pads[(size_t) pad].type ? 1 : 0;

    auto pick = [nth] (std::initializer_list<int> notes) { return *(notes.begin() + juce::jmin ((int) notes.size() - 1, nth)); };
    switch (kit.pads[(size_t) pad].type)
    {
        case Kick:    return pick ({ 36, 35 });
        case Snare:   return pick ({ 38, 40, 37 });
        case Clap:    return pick ({ 39 });
        case Hat:     return pick ({ 42, 44 });
        case OpenHat: return pick ({ 46 });
        case Tom:     return pick ({ 45, 47, 48, 50, 43, 41 });
        case Cymbal:  return pick ({ 49, 57, 51, 55, 52 });
        case Perc:
        default:      return pick ({ 56, 54, 75, 76, 77, 60, 61, 62, 63, 64 });
    }
}

juce::MidiMessageSequence MidiExport::padSequence (const Kit& kit, int pad, const MidiOptions& options)
{
    const double ticksPerSecond = juce::jlimit (20.0, 400.0, options.bpm) / 60.0 * ticksPerQuarter;
    const int grid = options.quantize > 0 ? ticksPerQuarter / options.quantize : 0;
    const int note = noteFor (kit, pad, options.noteMap);

    // collect (tick, velocity), merging hits that quantise onto the same step
    std::vector<std::pair<int, float>> notes;
    for (auto& h : kit.hits)
    {
        if (h.pad != pad)
            continue;
        int tick = (int) std::lround (h.seconds * ticksPerSecond);
        if (grid > 0)
            tick = (int) std::lround ((double) tick / grid) * grid;
        if (! notes.empty() && notes.back().first == tick)
            notes.back().second = juce::jmax (notes.back().second, h.velocity);
        else
            notes.push_back ({ tick, h.velocity });
    }

    juce::MidiMessageSequence seq;
    const int defaultLength = ticksPerQuarter / 4;      // a 1/16 note
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const int tick = notes[i].first;
        int length = defaultLength;
        if (i + 1 < notes.size())
            length = juce::jmax (1, juce::jmin (length, notes[i + 1].first - tick - 1));
        const auto velocity = (juce::uint8) juce::jlimit (1, 127, (int) std::lround (notes[i].second * 127.0f));
        seq.addEvent (juce::MidiMessage::noteOn (kDrumChannel, note, velocity), tick);
        seq.addEvent (juce::MidiMessage::noteOff (kDrumChannel, note), tick + length);
    }
    seq.updateMatchedPairs();
    return seq;
}

juce::MidiFile MidiExport::padFile (const Kit& kit, int pad, const MidiOptions& options)
{
    juce::MidiFile file;
    file.setTicksPerQuarterNote (ticksPerQuarter);
    auto track = tempoTrack (options.bpm, kit.pads[(size_t) pad].name);
    track.addSequence (padSequence (kit, pad, options), 0.0);
    track.updateMatchedPairs();
    file.addTrack (track);
    return file;
}

juce::MidiFile MidiExport::multiTrackFile (const Kit& kit, const MidiOptions& options)
{
    juce::MidiFile file;
    file.setTicksPerQuarterNote (ticksPerQuarter);
    file.addTrack (tempoTrack (options.bpm, kit.sourceName));
    for (int pad = 0; pad < (int) kit.pads.size(); ++pad)
    {
        juce::MidiMessageSequence track;
        track.addEvent (juce::MidiMessage::textMetaEvent (3, kit.pads[(size_t) pad].name), 0.0);
        track.addSequence (padSequence (kit, pad, options), 0.0);
        track.updateMatchedPairs();
        file.addTrack (track);
    }
    return file;
}

bool MidiExport::write (const juce::MidiFile& midi, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream out (file);
    return out.openedOk() && midi.writeTo (out, 1);
}

juce::StringArray MidiExport::writeForDrag (const Kit& kit, const MidiOptions& options, const juce::File& folder, int pad)
{
    juce::StringArray written;
    if (kit.pads.empty() || ! folder.createDirectory())
        return written;

    const auto base = juce::File::createLegalFileName (kit.sourceName.isNotEmpty() ? kit.sourceName : juce::String ("Drumkit Clone"));

    if (pad < 0 && options.layout == MidiOptions::multiTrackFile)
    {
        const auto f = folder.getChildFile (base + " MIDI.mid");
        if (write (multiTrackFile (kit, options), f))
            written.add (f.getFullPathName());
        return written;
    }

    for (int i = 0; i < (int) kit.pads.size(); ++i)
    {
        if (pad >= 0 && i != pad)
            continue;
        // the file name becomes the track name in most DAWs
        const auto f = folder.getChildFile (juce::File::createLegalFileName (kit.pads[(size_t) i].name) + ".mid");
        if (! write (padFile (kit, i, options), f))
            return {};
        written.add (f.getFullPathName());
    }
    return written;
}
}
