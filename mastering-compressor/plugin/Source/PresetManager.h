#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace lumen
{
/** Factory presets (compiled in) + user presets (XML files in the user's app-data folder). */
class PresetManager
{
public:
    explicit PresetManager (juce::AudioProcessorValueTreeState& state);

    juce::StringArray getPresetNames() const;  // factory first, then user
    int getNumFactoryPresets() const noexcept;
    bool loadPreset (int index);
    bool savePreset (const juce::String& name);   // user preset (overwrites same name)
    bool deletePreset (int index);                // user presets only
    bool isFactory (int index) const noexcept { return index >= 0 && index < getNumFactoryPresets(); }
    int getCurrentIndex() const noexcept { return currentIndex; }
    juce::String getCurrentName() const;
    void refreshUserPresets();

    static juce::File getUserPresetDirectory();

private:
    struct FactoryPreset
    {
        juce::String name;
        std::vector<std::pair<juce::String, float>> values; // anything not listed = default
    };

    void applyValues (const std::vector<std::pair<juce::String, float>>& values);

    juce::AudioProcessorValueTreeState& state;
    std::vector<FactoryPreset> factory;
    juce::Array<juce::File> userFiles;
    int currentIndex = 0;
};
} // namespace lumen
