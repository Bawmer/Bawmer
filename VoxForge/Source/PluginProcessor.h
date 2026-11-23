#pragma once

#include <JuceHeader.h>
#include "Core/VocalTypes.h"
#include "DSP/FormantSynth.h"
#include "Engine/VoicePreset.h"
#include "Engine/RenderEngine.h"
#include "Engine/StemAnalyzer.h"
#include "Engine/EngineClient.h"
#include "Engine/VoiceAnalyzer.h"

namespace ParamIDs
{
    inline constexpr const char* vibDepth   = "vibDepth";
    inline constexpr const char* vibRate    = "vibRate";
    inline constexpr const char* vibDelay   = "vibDelay";
    inline constexpr const char* breath     = "breath";
    inline constexpr const char* bendRange  = "bendRange";
    inline constexpr const char* porta      = "porta";
    inline constexpr const char* expression = "expression";
    inline constexpr const char* tension    = "tension";
    inline constexpr const char* formant    = "formant";
    inline constexpr const char* humanize   = "humanize";
    inline constexpr const char* nuance     = "nuance";
    inline constexpr const char* gain       = "gain";
    inline constexpr const char* live       = "live";
    inline constexpr const char* monitor    = "monitor";
}

class VoxForgeProcessor : public juce::AudioProcessor,
                          private juce::Timer
{
public:
    VoxForgeProcessor();
    ~VoxForgeProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.5; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Message-thread API used by the editor
    juce::AudioProcessorValueTreeState apvts;
    vf::VocalProject& project() { return proj; }
    void projectChanged (bool rerender = true);       // call after editing notes / lyrics
    int projectVersion() const noexcept { return version.load(); }

    vf::PresetManager& presets() { return presetManager; }
    const vf::VoiceModel& currentVoice() const { return voice; }
    void selectVoice (const juce::String& id, bool applyExpression);
    void voicesChanged();                             // after create / rename / delete

    vf::ExpressionSettings expressionSettings() const;

    // rendering
    void renderNow();
    bool autoRender = true;
    vf::RenderEngine& renderer() { return renderEngine; }
    bool hasTake() const;
    juce::String takeSummary() const;
    bool renderIsStale() const;
    bool exportTake (const juce::File& f);
    const juce::AudioBuffer<float>* takeEngineAudio() const;   // message thread only

    // capture of DAW MIDI
    void setCaptureArmed (bool shouldArm);
    bool isCaptureArmed() const noexcept { return captureArmed.load(); }
    bool isCapturing() const noexcept { return capturing.load(); }
    bool importMidiFile (const juce::File& f, juce::String& error);

    // host transport info (for the editor)
    double hostBpm() const noexcept { return lastBpm.load(); }
    double playheadBeat() const noexcept { return playBeat.load(); }
    bool hostIsPlaying() const noexcept { return hostPlaying.load(); }

    // preview transport (when the host is stopped)
    void startPreview (double fromBeat);
    void stopPreview() { previewing = false; }
    bool isPreviewing() const noexcept { return previewing.load(); }

    // vocal replacement
    bool loadStem (const juce::File& f, juce::String& error);
    void analyseStem (const vf::VocalToMidiOptions& opts, bool transcribe);
    vf::StemAnalyzer& stemAnalyzer() { return analyzer; }
    bool applyStemAnalysis();                          // moves analysis result into the project
    juce::String lastAnalysisNote;
    const juce::AudioBuffer<float>* stemPreview() const { return stemEngine.getNumSamples() > 0 ? &stemEngine : nullptr; }

    // voice cloning
    std::unique_ptr<vf::EngineJob> trainJob;
    juce::String trainingVoiceId;
    void startNeuralTraining (const vf::VoiceModel& v, int epochs, const juce::String& quality);
    void cancelTraining();

    juce::String lastMessage;                          // status line text
    int lastEditorTab = 2;

private:
    void timerCallback() override;
    void drainCapture();
    void installTake (std::shared_ptr<vf::RenderedTake> t);
    void updateLiveVoice();
    juce::String currentHash() const;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    vf::VocalProject proj;
    vf::PresetManager presetManager;
    vf::VoiceModel voice = vf::VoiceModel::builtinDefault();
    vf::RenderEngine renderEngine;
    vf::StemAnalyzer analyzer;
    vf::LiveVoice liveVoice;

    std::atomic<int> version { 0 };
    juce::String lastRequestedHash, installedHash;
    juce::uint32 dirtySince = 0;
    bool renderDirty = false;
    juce::String pendingCacheHash;

    // audio-thread shared state
    juce::SpinLock takeLock;
    std::shared_ptr<vf::RenderedTake> take;
    std::vector<std::shared_ptr<vf::RenderedTake>> graveyard;
    juce::AudioBuffer<float> stemEngine;     // stem at engine rate (for display / reload)
    juce::SpinLock stemLock;
    juce::AudioBuffer<float> stemHost;       // stem at host rate (A/B monitor)
    std::atomic<double> projectBpm { 120.0 }, stemOffsetSec { 0.0 };

    double sampleRate = 48000.0;
    int blockSize = 512;
    std::atomic<double> lastBpm { 120.0 }, playBeat { 0.0 };
    std::atomic<bool> hostPlaying { false }, previewing { false };
    std::atomic<double> previewSample { 0.0 };

    // MIDI capture FIFO (audio -> message thread)
    struct CapEvent { int type; double beat; int a; float b; };   // type: 0 on, 1 off, 2 bend, 3 cc, 8 start, 9 stop
    juce::AbstractFifo capFifo { 8192 };
    std::vector<CapEvent> capBuffer = std::vector<CapEvent> (8192);
    std::atomic<bool> captureArmed { false }, capturing { false };
    bool wasPlaying = false;
    std::map<int, size_t> openNotes;
    double captureBpm = 120.0;

    void pushCap (const CapEvent& e);
    std::shared_ptr<int> aliveToken = std::make_shared<int> (0);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoxForgeProcessor)
};
