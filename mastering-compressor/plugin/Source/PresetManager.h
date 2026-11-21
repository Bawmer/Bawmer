#pragma once

#include <atomic>
#include <functional>
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
    int getCurrentIndex() const noexcept { return currentIndex.load(); }
    void setCurrentIndex (int index) noexcept; // e.g. restoring a session (does not load values)
    std::function<void()> onBeginLoad, onEndLoad; // let the processor make a load one undo step
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
    std::atomic<int> currentIndex { 0 };
};
} // namespace lumen
