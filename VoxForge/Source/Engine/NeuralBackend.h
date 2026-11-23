#pragma once

#include <JuceHeader.h>
#include <vector>
#include <memory>

namespace vf
{

/** Neural voice (ONNX Runtime, CPU or DirectML on Windows).

    Pipeline (singing-voice-conversion style, trained by the VoxForge engine):
      content_encoder.onnx : audio16k [1, N]                  -> units [1, T, C]   (50 frames/s)
      voice.onnx           : units [1, T, C], f0 [1, T] (Hz)   -> audio [1, T * hop] (48 kHz, hop 960)

    The content encoder is shared by all voices; voice.onnx is the user's
    trained clone. Built without VOXFORGE_WITH_ONNX the backend reports
    unavailable and VoxForge falls back to the DSP engine. */
class NeuralBackend
{
public:
    NeuralBackend();
    ~NeuralBackend();

    static bool isCompiledIn() noexcept;
    /** Human readable state ("ready", "content encoder missing", ...). */
    juce::String status() const;

    bool loadContentEncoder (const juce::File& onnx, juce::String& error);
    bool loadVoice (const juce::File& onnx, juce::String& error);
    bool hasContentEncoder() const noexcept;
    bool hasVoice() const noexcept;
    juce::File currentVoiceFile() const { return voiceFile; }

    static constexpr double kContentRate = 16000.0;
    static constexpr double kFrameRate = 50.0;       // content frames per second
    static constexpr double kOutputRate = 48000.0;
    static constexpr int kHop = 960;                  // output samples per frame

    /** Extracts content units from 16 kHz audio. Returns frames x dim (row-major). */
    bool extractUnits (const float* audio16k, int numSamples, std::vector<float>& units, int& frames, int& dim,
                       std::atomic<bool>* cancel = nullptr);

    /** Synthesises 48 kHz audio from units + f0 (Hz per frame, 0 = unvoiced). Chunked with crossfades. */
    bool synthesise (const std::vector<float>& units, int frames, int dim, const std::vector<float>& f0,
                     std::vector<float>& audioOut, std::atomic<bool>* cancel = nullptr,
                     std::function<void (float)> progress = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    juce::File voiceFile;
    juce::String lastError;
    juce::CriticalSection lock;
};

} // namespace vf
