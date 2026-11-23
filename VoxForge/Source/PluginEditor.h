#pragma once

#include "PluginProcessor.h"
#include "UI/LookAndFeel.h"
#include "UI/EditorTab.h"
#include "UI/VoiceCloneTab.h"
#include "UI/PresetsTab.h"
#include "UI/ReplaceTab.h"

class VoxForgeEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit VoxForgeEditor (VoxForgeProcessor&);
    ~VoxForgeEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void refreshVoiceBox();

    VoxForgeProcessor& proc;
    vf::ui::VFLookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };

    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    vf::ui::VoiceCloneTab cloneTab;
    vf::ui::PresetsTab presetsTab;
    vf::ui::EditorTab editorTab;
    vf::ui::ReplaceTab replaceTab;

    juce::ComboBox voiceBox;
    juce::Label voiceLabel, engineBadge, statusLine;
    double renderProgress = 0.0;
    juce::ProgressBar renderBar { renderProgress };
    juce::StringArray voiceIds;
    int voiceCount = -1;
    juce::String shownVoice;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoxForgeEditor)
};
