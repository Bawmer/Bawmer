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
    static constexpr int kSize = 4096;
    float gr[kSize] {};
    float level[kSize] {};
    std::atomic<int> writeIndex { 0 };

    void push (float g, float l) noexcept
    {
        const int w = writeIndex.load (std::memory_order_relaxed);
        gr[w % kSize] = g;
        level[w % kSize] = l;
        writeIndex.store (w + 1, std::memory_order_release);
    }
};

class LumenAudioProcessor final : public juce::AudioProcessor, private juce::Timer
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
    int getActiveSlot() const noexcept { return activeSlot; }
    void switchToSlot (int slot);        // stores the current settings in the active slot first
    void copyActiveToOther();

    juce::UndoManager undoManager;
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

    ParameterReader reader;
    CompressorEngine engine;
    juce::AudioBuffer<double> work, scWork; // used for single-precision hosts and sidechain conversion
    double currentSampleRate = 48000.0;
    int historySamples = 0, historyInterval = 480;
    float historyMaxGr = 0.0f, historyMaxLevel = -120.0f;
    std::atomic<int> desiredLatency { 0 }, reportedLatency { 0 };

    juce::ValueTree abSlots[2];
    int activeSlot = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LumenAudioProcessor)
};
} // namespace lumen
