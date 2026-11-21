#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <lumen/CompressorEngine.h>

#include "Parameters.h"
#include "PresetManager.h"

namespace lumen
{
/** Lock-free meter values: the audio thread accumulates maxima, the UI takes them. */
struct MeterBus
{
    std::atomic<float> inPeak[2] {}, outPeak[2] {}, inMid {}, inSide {}, outMid {}, outSide {}, gr[2] {};
    std::atomic<float> detectorDb { -120.0f }, autoGainDb { 0.0f }, matchDb { 0.0f };

    static void accumulateMax (std::atomic<float>& a, float v) noexcept
    {
        float cur = a.load (std::memory_order_relaxed);
        while (v > cur && ! a.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
    }
    static float take (std::atomic<float>& a, float resetValue = 0.0f) noexcept { return a.exchange (resetValue); }
};

/** Single-producer / single-consumer ring of gain-reduction samples (one per ~10 ms). */
struct GrHistory
{
    static constexpr uint32_t kSize = 4096; // power of two: index & (kSize - 1)
    float gr[kSize] {};
    float level[kSize] {};
    // unsigned: wraps modulo 2^32 by definition, and since kSize divides 2^32 the masked slot
    // stays continuous across the wrap; readers compare with unsigned differences
    std::atomic<uint32_t> writeIndex { 0 };

    void push (float g, float l) noexcept
    {
        const uint32_t w = writeIndex.load (std::memory_order_relaxed);
        gr[w & (kSize - 1)] = g;
        level[w & (kSize - 1)] = l;
        writeIndex.store (w + 1, std::memory_order_release);
    }
};

class LumenAudioProcessor final : public juce::AudioProcessor,
                                  private juce::Timer,
                                  private juce::AudioProcessorParameter::Listener
{
public:
    LumenAudioProcessor();
    ~LumenAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    bool supportsDoublePrecisionProcessing() const override { return true; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override;

    // --- A/B comparison -----------------------------------------------------------------
    int getActiveSlot() const;
    void switchToSlot (int slot);        // stores the current settings in the active slot first
    void copyActiveToOther();

    // --- undo / redo: GUI edits only ---------------------------------------------------------
    // The parameter tree has no undo manager, so host automation is never recorded. Undo steps
    // are taken from parameter *gestures*, which only the plugin's own GUI and preset/A-B code
    // produce (hosts automate via setValue, without gestures). Several changes inside one
    // batch (preset load, A/B switch) become a single step.
    juce::UndoManager undoManager;
    using ParamSnapshot = std::vector<std::pair<juce::String, float>>;
    ParamSnapshot captureParameters() const;          // real-world values, excluding bypass/match
    void applyParameters (const ParamSnapshot& snap); // as one notified gesture per changed param
    void beginUndoBatch();                            // message thread
    void endUndoBatch();
    juce::AudioProcessorValueTreeState apvts;
    PresetManager presets;
    MeterBus meters;
    GrHistory history;

    double getCurrentSampleRate() const noexcept { return currentSampleRate; }
    int getReportedLatency() const noexcept { return reportedLatency.load(); }

private:
    template <typename SampleType>
    void processInternal (juce::AudioBuffer<SampleType>& buffer);
    void timerCallback() override;
    void publishMeters (int numSamples) noexcept;
    void parameterValueChanged (int, float) override {}
    void parameterGestureChanged (int, bool gestureIsStarting) override;
    void commitUndoStepIfChanged();

    ParameterReader reader;
    CompressorEngine engine;
    juce::AudioBuffer<double> work, scWork; // used for single-precision hosts and sidechain conversion
    double currentSampleRate = 48000.0;
    int historySamples = 0, historyInterval = 480;
    float historyMaxGr = 0.0f, historyMaxLevel = -120.0f;
    std::atomic<int> desiredLatency { 0 }, reportedLatency { 0 };

    juce::CriticalSection abLock; // getStateInformation() may run on a host thread
    juce::ValueTree abSlots[2];
    int activeSlot = 0;

    ParamSnapshot undoBefore;
    int openGestures = 0, batchDepth = 0;
    bool applyingSnapshot = false;

    JUCE_DECLARE_WEAK_REFERENCEABLE (LumenAudioProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LumenAudioProcessor)
};
} // namespace lumen
