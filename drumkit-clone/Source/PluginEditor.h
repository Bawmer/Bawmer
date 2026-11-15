#pragma once

#include "PluginProcessor.h"

class DrumkitCloneEditor : public juce::AudioProcessorEditor,
                           public juce::FileDragAndDropTarget,
                           public juce::DragAndDropContainer,
                           private juce::ChangeListener,
                           private juce::Timer
{
public:
    explicit DrumkitCloneEditor (DrumkitCloneProcessor&);
    ~DrumkitCloneEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

    void selectPad (int pad);
    void padPressed (int pad);
    void padDraggedOut (int pad);
    /** Drags MIDI out to the DAW: every drum (pad = -1) or one pad. */
    void dragMidiOut (int pad);

    static juce::Colour padColour (int pad);

private:
    class Overview;
    class PadButton;
    class MidiDragHandle;
    struct Look;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refresh();
    void chooseFile();
    void exportKit();

    DrumkitCloneProcessor& processor;
    std::unique_ptr<Look> look;
    dkc::Kit::Ptr kit;

    std::unique_ptr<Overview> overview;
    juce::OwnedArray<PadButton> pads;

    std::unique_ptr<MidiDragHandle> midiHandle;

    juce::TextButton loadButton { "Load audio..." }, exportButton { "Export kit..." }, reanalyseButton { "Re-analyze" };
    juce::Slider bpm;
    juce::ComboBox quantizeBox, noteMapBox, layoutBox;
    juce::Label bpmLabel, quantizeLabel, noteMapLabel, layoutLabel;
    juce::Slider sensitivity, separation, gain, tune, pan, master;
    juce::Label sensitivityLabel, separationLabel, gainLabel, tuneLabel, panLabel, masterLabel, selectedLabel, infoLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gainAttachment, tuneAttachment, panAttachment, masterAttachment;
    std::unique_ptr<juce::FileChooser> chooser;

    int selectedPad = 0;
    bool fileHover = false, draggingOut = false, wasAnalysing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumkitCloneEditor)
};
