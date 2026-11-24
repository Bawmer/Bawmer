#pragma once

#include "LookAndFeel.h"
#include "../Engine/Voicebox.h"

class VoxForgeProcessor;

namespace vf::ui
{

/** Preset manager: list / use / rename / duplicate / delete / import / export voices + global settings. */
class PresetsTab : public juce::Component, private juce::ListBoxModel
{
public:
    explicit PresetsTab (VoxForgeProcessor& p);
    void resized() override;
    void paint (juce::Graphics&) override;
    void tick();
    void refreshList();

private:
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void selectedRowsChanged (int) override { updateDetails(); }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void updateDetails();
    juce::String selectedId() const;
    void askName (const juce::String& title, const juce::String& initial, std::function<void (juce::String)> done);
    void importRvcModel (const juce::File& pth, const juce::String& name);
    void connectVoicebox();
    void fillVoiceboxProfiles();

    VoxForgeProcessor& proc;
    Section listSection { "Voice presets" }, detailSection { "Details" }, settingsSection { "Settings" };
    juce::ListBox list;
    juce::Label details;
    juce::TextButton useBtn { "Use voice" }, useExprBtn { "Use voice + its expression" }, renameBtn { "Rename" },
                     dupBtn { "Duplicate" }, deleteBtn { "Delete" }, exportBtn { "Export .vfvoice" },
                     importBtn { "Import .vfvoice" }, importRvcBtn { "Import Applio / RVC model..." }, saveExprBtn { "Save current expression into preset" },
                     openFolderBtn { "Open data folder" }, browseEngineBtn { "Browse..." }, testEngineBtn { "Test engine" },
                     dictBtn { "Load CMU dictionary..." };
    juce::Label engineLabel, langLabel, statusLabel;
    juce::TextEditor engineEditor;
    juce::ComboBox langBox, lyricsModelBox;
    juce::ToggleButton gpuToggle { "Use NVIDIA GPU (training, lyrics, rendering)" };
    juce::Label voiceboxLabel;
    juce::TextEditor voiceboxUrlEditor;
    juce::TextButton voiceboxConnectBtn { "Connect" };
    juce::ComboBox voiceboxProfileBox;
    juce::ToggleButton voiceboxGuideToggle { "Natural pronunciation (neural voices)" };
    std::vector<vf::voicebox::Profile> voiceboxProfiles;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::AlertWindow> nameWindow;
    int lastCount = -1;
};

} // namespace vf::ui
