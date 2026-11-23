#pragma once

#include "VoicePreset.h"
#include "NeuralBackend.h"
#include "../Core/Timeline.h"
#include "../DSP/PitchDetector.h"

namespace vf
{

/** A finished render: mono audio aligned to project time 0 (beat 0). */
struct RenderedTake
{
    juce::AudioBuffer<float> engineAudio;   // kEngineSampleRate
    juce::AudioBuffer<float> hostAudio;     // resampled to hostRate
    double hostRate = 0.0;
    juce::String hash;
    bool neural = false;
    bool replacement = false;
    juce::String summary;
};

struct RenderRequest
{
    VocalProject project;
    ExpressionSettings settings;
    VoiceModel voice;
    bool replaceMode = false;
    double hostRate = 48000.0;
    juce::String hash;
};

/** Background offline renderer ("Render" button). Highest quality, not real-time. */
class RenderEngine : private juce::Thread
{
public:
    RenderEngine();
    ~RenderEngine() override;

    void request (RenderRequest r);
    void cancel();

    bool isBusy() const noexcept { return busy.load(); }
    float progress() const noexcept { return prog.load(); }
    juce::String status() const { const juce::ScopedLock sl (lock); return statusText; }

    /** Returns and clears a newly finished take (message thread polls this). */
    std::shared_ptr<RenderedTake> takeFinished();

    NeuralBackend& neural() { return backend; }
    juce::String neuralStatus();

    /** Loads a cached take from disk if it exists. */
    static std::shared_ptr<RenderedTake> loadCached (const juce::String& hash, double hostRate);

private:
    void run() override;
    std::shared_ptr<RenderedTake> renderSynth (const RenderRequest& r);
    std::shared_ptr<RenderedTake> renderReplace (const RenderRequest& r);
    bool prepareNeural (const VoiceModel& v);
    void setStatus (const juce::String& s, float p);
    std::vector<float> framesF0FromCurves (const ControlCurves& c, const Timeline& tl, int frames) const;

    NeuralBackend backend;
    juce::CriticalSection lock;
    juce::WaitableEvent wake;
    std::unique_ptr<RenderRequest> pending;
    std::shared_ptr<RenderedTake> finished;
    std::atomic<bool> busy { false }, cancelFlag { false };
    std::atomic<float> prog { 0.0f };
    juce::String statusText { "Idle" };

    // stem cache (vocal replacement)
    juce::String cachedStemKey;
    juce::AudioBuffer<float> stem16k;
    std::vector<float> stemUnits;
    int stemFrames = 0, stemDim = 0;
    F0Track stemF0;
};

} // namespace vf
