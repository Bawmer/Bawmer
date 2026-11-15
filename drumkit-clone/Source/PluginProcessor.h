#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "KitAnalyzer.h"
#include "MidiExport.h"
#include <array>
#include <atomic>

class DrumkitCloneProcessor : public juce::AudioProcessor,
                              public juce::ChangeBroadcaster,
                              private juce::AsyncUpdater
{
public:
    static constexpr int firstNote = 36;    // pad 1 = C1, the usual drum-map root
    static constexpr int numVoices = 24;

    DrumkitCloneProcessor();
    ~DrumkitCloneProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                     { return true; }

    const juce::String getName() const override         { return "Drumkit Clone"; }
    bool acceptsMidi() const override                   { return true; }
    bool producesMidi() const override                  { return false; }
    bool isMidiEffect() const override                  { return false; }
    double getTailLengthSeconds() const override        { return 2.0; }

    int getNumPrograms() override                       { return 1; }
    int getCurrentProgram() override                    { return 0; }
    void setCurrentProgram (int) override               {}
    const juce::String getProgramName (int) override    { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==============================================================================
    /** Starts analysing a file in the background. Message thread only. */
    void loadFile (const juce::File&);
    bool canReanalyse() const;
    void reanalyse();
    void cancelAnalysis();

    bool isAnalysing() const                            { return analysing.load(); }
    float getProgress() const                           { return progress.load(); }
    juce::String getStatusMessage() const;

    dkc::Kit::Ptr getKit() const;
    void setKit (dkc::Kit::Ptr);

    /** Plays a pad from the UI. */
    void triggerPad (int pad)                           { if (juce::isPositiveAndBelow (pad, dkc::kMaxPads)) uiTriggers.fetch_or (1u << pad); }
    /** Incremented each time a pad is triggered; the editor polls it for pad lights. */
    int getPadTriggerCount (int pad) const              { return padTriggerCount[(size_t) pad].load (std::memory_order_relaxed); }

    bool exportKit (const juce::File& folder, juce::String& error) const;

    //==============================================================================
    // MIDI drag-out: the hits of each drum as its own MIDI part
    std::atomic<double> midiBpmOverride { 0.0 };    // 0 = use the detected tempo
    std::atomic<int> midiQuantize { 0 }, midiNoteMap { dkc::MidiOptions::padNotes }, midiLayout { dkc::MidiOptions::filePerDrum };

    double getMidiBpm() const;
    dkc::MidiOptions getMidiOptions() const;
    /** Writes MIDI files to a temp folder for dragging; pad = -1 for all pads. */
    juce::StringArray writeMidiForDrag (int pad = -1) const;
    static bool writeWav (const dkc::PadSample&, const juce::File&);

    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float> sensitivity { 0.5f }, separation { 0.5f };

    static juce::String gainId (int pad)                { return "gain" + juce::String (pad); }
    static juce::String tuneId (int pad)                { return "tune" + juce::String (pad); }
    static juce::String panId (int pad)                 { return "pan" + juce::String (pad); }

private:
    struct Voice
    {
        bool active = false;
        int pad = 0;
        double position = 0.0, rate = 1.0;
        float gainL = 0.0f, gainR = 0.0f;
        float fade = 1.0f, fadeStep = 0.0f;     // fadeStep > 0 while being choked
    };

    class AnalysisThread;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void handleAsyncUpdate() override;
    void startVoice (int pad, float velocity);
    void render (juce::AudioBuffer<float>&, int start, int num);
    void startAnalysis (const juce::File&);

    // audio thread state (guarded by audioLock when the kit changes)
    juce::SpinLock audioLock;
    const dkc::Kit* activeKit = nullptr;
    std::array<Voice, numVoices> voices;
    double hostSampleRate = 44100.0;
    std::atomic<juce::uint32> uiTriggers { 0 };
    std::array<std::atomic<int>, dkc::kMaxPads> padTriggerCount;
    std::array<std::atomic<float>*, dkc::kMaxPads> gainParams {}, tuneParams {}, panParams {};
    std::atomic<float>* masterParam = nullptr;

    // kit ownership / analysis
    juce::CriticalSection kitLock;
    dkc::Kit::Ptr kit, pendingKit;
    bool hasPending = false;
    juce::String status, sourcePath;
    std::unique_ptr<AnalysisThread> analysisThread;
    std::atomic<bool> analysing { false };
    std::atomic<float> progress { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumkitCloneProcessor)
};
