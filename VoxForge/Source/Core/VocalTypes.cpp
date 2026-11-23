#include "VocalTypes.h"
#include <algorithm>

namespace vf
{

void VocalProject::sortNotes()
{
    std::stable_sort (notes.begin(), notes.end(),
                      [] (const Note& a, const Note& b) { return a.startBeat < b.startBeat; });
}

double VocalProject::lengthInBeats() const
{
    double len = 0.0;
    for (auto& n : notes)
        len = juce::jmax (len, n.endBeat());
    return len;
}

void VocalProject::clearControllers()
{
    pitchBend.clear();
    modWheel.clear();
    breathCC.clear();
    expressionCC.clear();
}

static juce::ValueTree curveToTree (const juce::Identifier& id, const std::vector<ControlPoint>& c)
{
    juce::ValueTree t (id);
    juce::MemoryOutputStream mo;
    for (auto& p : c) { mo.writeDouble (p.beat); mo.writeFloat (p.value); }
    t.setProperty ("data", mo.getMemoryBlock().toBase64Encoding(), nullptr);
    return t;
}

static void curveFromTree (const juce::ValueTree& t, std::vector<ControlPoint>& c)
{
    c.clear();
    if (! t.isValid()) return;
    juce::MemoryBlock mb;
    mb.fromBase64Encoding (t.getProperty ("data").toString());
    juce::MemoryInputStream mi (mb, false);
    while (mi.getNumBytesRemaining() >= 12)
    {
        ControlPoint p;
        p.beat = mi.readDouble();
        p.value = mi.readFloat();
        c.push_back (p);
    }
}

juce::ValueTree VocalProject::toValueTree() const
{
    juce::ValueTree v ("VocalProject");
    v.setProperty ("bpm", bpm, nullptr);
    v.setProperty ("lyrics", lyricsText, nullptr);
    v.setProperty ("stemPath", stemPath, nullptr);
    v.setProperty ("stemOffsetBeats", stemOffsetBeats, nullptr);

    juce::ValueTree ns ("Notes");
    for (auto& n : notes)
    {
        juce::ValueTree t ("Note");
        t.setProperty ("s", n.startBeat, nullptr);
        t.setProperty ("l", n.lengthBeats, nullptr);
        t.setProperty ("p", n.pitch, nullptr);
        t.setProperty ("v", n.velocity, nullptr);
        t.setProperty ("ly", n.lyric, nullptr);
        if (! n.wordStart) t.setProperty ("ws", false, nullptr);
        if (n.phonemes.isNotEmpty())      t.setProperty ("ph", n.phonemes, nullptr);
        if (n.pitchOffsetCents != 0.0f)   t.setProperty ("pc", n.pitchOffsetCents, nullptr);
        if (n.fromSource)
        {
            t.setProperty ("src", true, nullptr);
            t.setProperty ("ss", n.srcStartSec, nullptr);
            t.setProperty ("se", n.srcEndSec, nullptr);
            t.setProperty ("sl", n.srcLyric, nullptr);
        }
        ns.appendChild (t, nullptr);
    }
    v.appendChild (ns, nullptr);
    v.appendChild (curveToTree ("PitchBend", pitchBend), nullptr);
    v.appendChild (curveToTree ("ModWheel", modWheel), nullptr);
    v.appendChild (curveToTree ("BreathCC", breathCC), nullptr);
    v.appendChild (curveToTree ("ExpressionCC", expressionCC), nullptr);
    return v;
}

void VocalProject::fromValueTree (const juce::ValueTree& v)
{
    if (! v.hasType ("VocalProject")) return;
    bpm = v.getProperty ("bpm", 120.0);
    lyricsText = v.getProperty ("lyrics").toString();
    stemPath = v.getProperty ("stemPath").toString();
    stemOffsetBeats = v.getProperty ("stemOffsetBeats", 0.0);

    notes.clear();
    for (auto t : v.getChildWithName ("Notes"))
    {
        Note n;
        n.startBeat   = t.getProperty ("s", 0.0);
        n.lengthBeats = t.getProperty ("l", 1.0);
        n.pitch       = t.getProperty ("p", 60);
        n.velocity    = t.getProperty ("v", 0.8f);
        n.lyric       = t.getProperty ("ly", "la").toString();
        n.phonemes    = t.getProperty ("ph").toString();
        n.pitchOffsetCents = t.getProperty ("pc", 0.0f);
        n.fromSource  = t.getProperty ("src", false);
        n.srcStartSec = t.getProperty ("ss", -1.0);
        n.srcEndSec   = t.getProperty ("se", -1.0);
        n.srcLyric    = t.getProperty ("sl").toString();
        n.wordStart   = (bool) t.getProperty ("ws", true);
        notes.push_back (n);
    }
    curveFromTree (v.getChildWithName ("PitchBend"), pitchBend);
    curveFromTree (v.getChildWithName ("ModWheel"), modWheel);
    curveFromTree (v.getChildWithName ("BreathCC"), breathCC);
    curveFromTree (v.getChildWithName ("ExpressionCC"), expressionCC);
    sortNotes();
}

juce::String VocalProject::contentHash (const ExpressionSettings& e, const juce::String& voiceId) const
{
    juce::MemoryOutputStream mo;
    mo << voiceId << "|" << bpm << "|" << stemPath << "|" << stemOffsetBeats;
    for (auto& n : notes)
        mo << n.startBeat << n.lengthBeats << n.pitch << n.velocity << n.lyric << n.phonemes
           << n.pitchOffsetCents << (int) n.wordStart << (int) n.fromSource << n.srcStartSec << n.srcEndSec;
    for (auto* c : { &pitchBend, &modWheel, &breathCC, &expressionCC })
        for (auto& p : *c) mo << p.beat << p.value;
    mo.write (&e, sizeof (e));
    return juce::String::toHexString ((juce::int64) mo.getMemoryBlock().toString().hashCode64());
}

float evalControl (const std::vector<ControlPoint>& curve, double beat, float defaultValue)
{
    if (curve.empty()) return defaultValue;
    if (beat <= curve.front().beat) return curve.front().value;
    if (beat >= curve.back().beat)  return curve.back().value;
    auto it = std::lower_bound (curve.begin(), curve.end(), beat,
                                [] (const ControlPoint& p, double b) { return p.beat < b; });
    auto& b = *it;
    auto& a = *(it - 1);
    const double span = b.beat - a.beat;
    if (span <= 1.0e-9) return b.value;
    // MIDI controllers are step-wise: hold the previous value unless points are dense
    const double t = (beat - a.beat) / span;
    return (float) (a.value + (b.value - a.value) * (span < 0.05 ? t : (t >= 1.0 ? 1.0 : 0.0)));
}

} // namespace vf
