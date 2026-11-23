#pragma once

#include <JuceHeader.h>
#include <functional>

namespace vf
{

/** Talks to the local VoxForge engine (voxforge-engine.exe, Python/PyTorch)
    which performs neural voice training and lyrics transcription offline.

    Protocol: the engine prints one JSON object per line on stdout:
      {"type":"progress","value":0.42,"message":"epoch 12/200"}
      {"type":"log","message":"..."}
      {"type":"result", ...payload...}
      {"type":"error","message":"..."}                                      */
class EngineClient
{
public:
    struct Outcome
    {
        bool ok = false;
        juce::var result;
        juce::String error;
        juce::StringArray log;
    };

    static bool isInstalled();
    static juce::String describeInstallation();

    /** Runs the engine synchronously (call from a background thread). */
    static Outcome run (const juce::StringArray& args, std::atomic<bool>* cancel,
                        std::function<void (float, const juce::String&)> progress,
                        std::function<void (const juce::String&)> log = nullptr);
};

/** Asynchronous engine job with pollable state (used for voice training so it
    keeps running when the editor window is closed). */
class EngineJob : private juce::Thread
{
public:
    explicit EngineJob (juce::StringArray args, juce::String title);
    ~EngineJob() override;

    void start() { startThread (juce::Thread::Priority::low); }
    void cancel() { cancelFlag = true; }

    bool isFinished() const noexcept { return finished.load(); }
    bool succeeded() const noexcept { return ok.load(); }
    float progress() const noexcept { return prog.load(); }
    juce::String title() const { return jobTitle; }
    juce::String status() const   { const juce::ScopedLock sl (lock); return statusText; }
    juce::String error() const    { const juce::ScopedLock sl (lock); return errorText; }
    juce::var result() const      { const juce::ScopedLock sl (lock); return resultVar; }
    juce::StringArray logLines() const { const juce::ScopedLock sl (lock); return log; }

private:
    void run() override;

    juce::StringArray arguments;
    juce::String jobTitle;
    std::atomic<bool> cancelFlag { false }, finished { false }, ok { false };
    std::atomic<float> prog { 0.0f };
    juce::CriticalSection lock;
    juce::String statusText, errorText;
    juce::var resultVar;
    juce::StringArray log;
};

} // namespace vf
