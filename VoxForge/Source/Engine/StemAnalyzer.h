#pragma once

#include "../Core/VocalTypes.h"
#include "../DSP/VocalToMidi.h"

namespace vf
{

struct TranscribedWord
{
    juce::String text;
    double t0 = 0.0, t1 = 0.0;
};

struct StemAnalysisResult
{
    bool ok = false;
    juce::String error;
    VocalAnalysis analysis;
    std::vector<TranscribedWord> words;
    juce::String transcript;
    juce::String transcriptionNote;     // why lyrics are missing, if they are
    double stemSeconds = 0.0;
};

/** Background "Vocal-to-MIDI + lyrics extraction" of a loaded vocal stem. */
class StemAnalyzer : private juce::Thread
{
public:
    StemAnalyzer();
    ~StemAnalyzer() override;

    /** @param lyricsHint  known lyrics (optional) - improves recognition of sung words */
    void start (const juce::File& stem, const VocalToMidiOptions& opts, bool transcribe, const juce::String& language,
                const juce::String& lyricsHint = {});
    void cancel() { cancelFlag = true; }

    bool isRunning() const noexcept { return isThreadRunning(); }
    bool hasResult() const noexcept { return resultReady.load(); }
    float progress() const noexcept { return prog.load(); }
    juce::String status() const { const juce::ScopedLock sl (lock); return statusText; }
    StemAnalysisResult takeResult();

private:
    void run() override;
    juce::File file;
    VocalToMidiOptions options;
    bool doTranscribe = true;
    juce::String lang, hint;
    std::atomic<bool> cancelFlag { false }, resultReady { false };
    std::atomic<float> prog { 0.0f };
    juce::CriticalSection lock;
    juce::String statusText;
    StemAnalysisResult result;
};

/** Converts detected notes + words into editable project notes (beats). Marks
    every note as coming from the source so unchanged words keep the original
    articulation when re-rendered. */
void notesFromAnalysis (const StemAnalysisResult& r, VocalProject& project);

} // namespace vf
