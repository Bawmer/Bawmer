#include "PresetManager.h"
#include "Parameters.h"

namespace lumen
{
PresetManager::PresetManager (juce::AudioProcessorValueTreeState& s) : state (s)
{
    using namespace ids;
    // mode: 0 Ultra, 1 Digital, 2 Analog, 3 Hybrid | detection: 0 Peak, 1 RMS, 2 Hybrid
    // channelMode: 0 Stereo, 1 Dual Mono, 2 M/S | topology: 0 FF, 1 FB
    // releaseMode: 0 Manual, 1 Auto Smooth, 2 Auto Program, 3 Auto Fast
    factory = {
        { "Init - Ultra Transparent", {} },
        { "Mastering Glue 2:1", { { mode, 1 }, { threshold, -18 }, { ratio, 2 }, { attack, 30 }, { release, 300 },
                                  { knee, 6 }, { releaseMode, 2 }, { detection, 1 } } },
        { "Gentle Bus 1.5:1", { { mode, 0 }, { threshold, -20 }, { ratio, 1.5f }, { attack, 30 }, { release, 500 },
                                { knee, 12 }, { releaseMode, 1 }, { autoGain, 1 } } },
        { "Classical - Barely There", { { mode, 0 }, { threshold, -24 }, { ratio, 1.3f }, { attack, 50 },
                                        { release, 1200 }, { knee, 18 }, { releaseMode, 1 }, { scHpf, 40 } } },
        { "Cinematic Wide Dynamics", { { mode, 0 }, { threshold, -16 }, { ratio, 1.5f }, { attack, 80 },
                                       { release, 1500 }, { knee, 24 }, { releaseMode, 1 }, { lookahead, 2 } } },
        { "Transient Control (Lookahead)", { { mode, 1 }, { threshold, -10 }, { ratio, 4 }, { attack, 1 },
                                             { release, 80 }, { knee, 3 }, { lookahead, 5 }, { detection, 3 } } },
        { "Hip-Hop Low-End Control", { { mode, 1 }, { threshold, -14 }, { ratio, 3 }, { attack, 10 }, { release, 120 },
                                       { knee, 6 }, { scHpf, 30 }, { detection, 2 }, { hold, 20 } } },
        { "EDM Dense Master", { { mode, 1 }, { threshold, -16 }, { ratio, 3 }, { attack, 3 }, { release, 60 },
                                { knee, 4 }, { releaseMode, 3 }, { lookahead, 1.5f }, { detection, 2 }, { scHpf, 90 } } },
        { "Mid/Side Center Focus", { { mode, 1 }, { channelMode, 2 }, { link, 0 }, { threshold, -18 }, { ratio, 2 },
                                     { attack, 20 }, { release, 250 }, { knee, 6 } } },
        { "Analog Warmth", { { mode, 2 }, { threshold, -16 }, { ratio, 2 }, { attack, 20 }, { release, 250 },
                             { drive, 6 }, { character, 40 } } },
        { "Analog Feedback Vintage", { { mode, 2 }, { topology, 1 }, { threshold, -20 }, { ratio, 4 }, { attack, 15 },
                                       { release, 400 }, { drive, 9 }, { character, 55 } } },
        { "Rock Bus Weight", { { mode, 2 }, { threshold, -18 }, { ratio, 3 }, { attack, 25 }, { release, 180 },
                               { drive, 8 }, { character, 30 }, { amplifier, 0 } } },
        { "Hybrid Punch", { { mode, 3 }, { threshold, -18 }, { ratio, 3 }, { attack, 25 }, { release, 150 },
                            { blend, 60 }, { drive, 4 }, { colorMix, 60 } } },
        { "Jazz Natural Hybrid", { { mode, 3 }, { threshold, -22 }, { ratio, 1.8f }, { attack, 40 }, { release, 700 },
                                   { knee, 12 }, { blend, 40 }, { drive, 3 }, { colorMix, 40 }, { releaseMode, 1 } } },
        { "Pop Polish", { { mode, 3 }, { threshold, -17 }, { ratio, 2.5f }, { attack, 15 }, { release, 200 },
                          { knee, 8 }, { blend, 50 }, { drive, 5 }, { colorMix, 50 }, { releaseMode, 2 } } },
    };
    refreshUserPresets();
}

juce::File PresetManager::getUserPresetDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Lumen Audio").getChildFile ("MC-64").getChildFile ("Presets");
}

void PresetManager::refreshUserPresets()
{
    userFiles = getUserPresetDirectory().findChildFiles (juce::File::findFiles, false, "*.lmcpreset");
    userFiles.sort();
}

int PresetManager::getNumFactoryPresets() const noexcept { return static_cast<int> (factory.size()); }

juce::StringArray PresetManager::getPresetNames() const
{
    juce::StringArray names;
    for (auto& f : factory)
        names.add (f.name);
    for (auto& f : userFiles)
        names.add (f.getFileNameWithoutExtension());
    return names;
}

juce::String PresetManager::getCurrentName() const
{
    auto names = getPresetNames();
    return juce::isPositiveAndBelow (currentIndex, names.size()) ? names[currentIndex] : juce::String();
}

void PresetManager::applyValues (const std::vector<std::pair<juce::String, float>>& values)
{
    for (auto* p : state.processor.getParameters())
    {
        auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (rp == nullptr || rp->getParameterID() == ids::bypass || rp->getParameterID() == ids::matchBypass)
            continue;
        float norm = rp->getDefaultValue();
        for (auto& [id, v] : values)
            if (id == rp->getParameterID())
                norm = rp->convertTo0to1 (v);
        rp->beginChangeGesture();
        rp->setValueNotifyingHost (norm);
        rp->endChangeGesture();
    }
}

bool PresetManager::loadPreset (int index)
{
    if (isFactory (index))
    {
        applyValues (factory[static_cast<size_t> (index)].values);
        currentIndex = index;
        return true;
    }
    const int u = index - getNumFactoryPresets();
    if (! juce::isPositiveAndBelow (u, userFiles.size()))
        return false;
    auto xml = juce::XmlDocument::parse (userFiles[u]);
    if (xml == nullptr || ! xml->hasTagName (state.state.getType()))
        return false;
    std::vector<std::pair<juce::String, float>> values;
    auto tree = juce::ValueTree::fromXml (*xml);
    for (auto child : tree)
        if (child.hasProperty ("id"))
            values.emplace_back (child["id"].toString(), static_cast<float> (child["value"]));
    applyValues (values);
    currentIndex = index;
    return true;
}

bool PresetManager::savePreset (const juce::String& name)
{
    const auto clean = juce::File::createLegalFileName (name.trim());
    if (clean.isEmpty())
        return false;
    auto dir = getUserPresetDirectory();
    if (! dir.createDirectory())
        return false;
    auto snapshot = state.copyState();
    // never store comparison switches in presets
    for (int i = snapshot.getNumChildren(); --i >= 0;)
    {
        auto id = snapshot.getChild (i)["id"].toString();
        if (id == ids::bypass || id == ids::matchBypass || ! snapshot.getChild (i).hasProperty ("id"))
            snapshot.removeChild (i, nullptr);
    }
    auto xml = snapshot.createXml();
    auto file = dir.getChildFile (clean + ".lmcpreset");
    if (xml == nullptr || ! xml->writeTo (file))
        return false;
    refreshUserPresets();
    currentIndex = getNumFactoryPresets() + userFiles.indexOf (file);
    return true;
}

bool PresetManager::deletePreset (int index)
{
    const int u = index - getNumFactoryPresets();
    if (! juce::isPositiveAndBelow (u, userFiles.size()))
        return false;
    const bool ok = userFiles[u].deleteFile();
    refreshUserPresets();
    currentIndex = 0;
    return ok;
}
} // namespace lumen
