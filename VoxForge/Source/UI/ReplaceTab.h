#pragma once

#include "LookAndFeel.h"

class VoxForgeProcessor;

namespace vf::ui
{

/** Waveform of the loaded stem with detected notes. */
class StemView : public juce::Component
{
public:
    explicit StemView (VoxForgeProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    void invalidate() { cacheValid = false; repaint(); }
private:
    VoxForgeProcessor& proc;
    std::vector<float> peaks;
    bool cacheValid = false;
};

/** Vocal replacement workflow: load stem -> Vocal-to-MIDI + lyrics -> edit -> re-render in the cloned voice. */
class ReplaceTab : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    explicit ReplaceTab (VoxForgeProcessor& p);
    void resized() override;
    void paint (juce::Graphics&) override;
    void tick();
    std::function<void()> openEditor;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    void loadStem (const juce::File& f);

    VoxForgeProcessor& proc;
    Section s1 { "1. Load the vocal stem" }, s2 { "2. Vocal-to-MIDI + lyrics extraction" },
            s3 { "3. Edit notes & words" }, s4 { "4. Re-render in your cloned voice" };
    StemView stemView;
    juce::TextButton loadBtn { "Load vocal stem..." }, analyseBtn { "Analyse stem" }, cancelBtn { "Cancel" },
                     openEditorBtn { "Open MIDI-Lyrics Editor" }, renderBtn { "Render replacement" }, exportBtn { "Export replaced vocal..." };
    juce::Label fileLabel, offsetLabel, sensLabel, minLabel, splitLabel, resultLabel, s3Text, voiceLabel, nuanceLabel, monitorLabel, hint;
    juce::Slider offsetSlider, sensSlider, minNoteSlider, splitSlider, nuanceSlider;
    juce::ToggleButton lyricsToggle { "Extract lyrics (offline speech recognition)" };
    juce::ComboBox monitorBox;
    double analysisProgress = 0.0, renderProgress = 0.0;
    juce::ProgressBar analysisBar { analysisProgress }, renderBar { renderProgress };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> nuanceAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> monitorAttach;
    std::unique_ptr<juce::FileChooser> chooser;
    int lastVersion = -1;
};

} // namespace vf::ui
