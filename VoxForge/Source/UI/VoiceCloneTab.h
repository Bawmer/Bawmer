#pragma once

#include "LookAndFeel.h"
#include "../Engine/VoiceAnalyzer.h"

class VoxForgeProcessor;

namespace vf::ui
{

/** Voice cloning: add dry samples -> analyse -> Lite clone preset -> optional neural training. */
class VoiceCloneTab : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      private juce::ListBoxModel,
                      private juce::Thread
{
public:
    explicit VoiceCloneTab (VoxForgeProcessor& p);
    ~VoiceCloneTab() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void tick();

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void run() override;          // background analysis
    void addFiles (const juce::Array<juce::File>& files);
    void startClone();
    void startTraining();
    void appendLog (const juce::String& s);

    VoxForgeProcessor& proc;
    Section samplesSection { "1. Your dry singing samples" }, cloneSection { "2. Create the voice clone" },
            neuralSection { "3. Neural training (optional, best quality)" };
    juce::ListBox list;
    juce::TextButton addBtn { "Add files..." }, removeBtn { "Remove" }, clearBtn { "Clear" },
                     cloneBtn { "Create Voice Clone" }, trainBtn { "Train Neural Voice" }, cancelBtn { "Cancel" };
    juce::Label tips, nameLabel, trainTarget, engineLabel, totalLabel;
    juce::TextEditor nameEditor, log;
    juce::ComboBox qualityBox;
    juce::Slider epochSlider;
    juce::ToggleButton autoTrain { "Start neural training right after cloning" };
    double progressValue = 0.0, trainProgress = 0.0;
    juce::ProgressBar progressBar { progressValue }, trainBar { trainProgress };
    std::unique_ptr<juce::FileChooser> chooser;

    struct Item { juce::File file; SampleReport report; bool inspected = false; };
    std::vector<Item> items;
    juce::CriticalSection itemLock;

    // analysis thread state
    std::atomic<bool> cancelFlag { false }, analysisDone { false };
    VoiceAnalysisResult analysis;
    juce::String pendingName;
    juce::String progressText;
    juce::CriticalSection logLock;
    juce::StringArray pendingLog;
    int lastLogLines = 0;
};

} // namespace vf::ui
