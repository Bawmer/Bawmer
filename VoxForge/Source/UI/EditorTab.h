#pragma once

#include "PianoRoll.h"

class VoxForgeProcessor;

namespace vf::ui
{

/** MIDI-lyrics editor tab: piano roll, lyrics entry, expression controls, render. */
class EditorTab : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    explicit EditorTab (VoxForgeProcessor& p);
    void resized() override;
    void paint (juce::Graphics&) override;
    void refresh();                 // project changed externally (capture, analysis)
    void tick();                    // ~15 Hz from the editor

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    void onNotesEdited();
    void showSelection (int index);
    void importMidi();
    void exportWav();

    VoxForgeProcessor& proc;
    PianoRoll roll;
    RollViewport viewport;

    juce::TextButton captureBtn { "Capture MIDI" }, importBtn { "Import MIDI" }, quantizeBtn { "Quantize" },
                     deleteBtn { "Delete" }, previewBtn { "Play" }, stopBtn { "Stop" }, renderBtn { "Render" },
                     exportBtn { "Export WAV" }, applyLyricsBtn { "Apply lyrics to notes" },
                     applyFromSelBtn { "Apply from selected" }, phonemeApplyBtn { "Set" }, bpmBtn { "Use host tempo" };
    juce::ToggleButton autoRenderToggle { "Auto-render" }, liveToggle { "Live MIDI voice" };
    juce::ComboBox gridBox;
    juce::Slider zoomSlider;
    juce::TextEditor lyricsEditor, phonemeEditor;
    juce::Label lyricsHelp, selectionLabel, bpmLabel, zoomLabel;
    Section lyricsSection { "Lyrics" }, expressionSection { "Expression" };
    std::vector<std::unique_ptr<Knob>> knobs;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> liveAttach;
    std::unique_ptr<juce::FileChooser> chooser;
    int lastVersion = -1;
    int selected = -1;
};

} // namespace vf::ui
