#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <vector>

namespace vf
{

/** Client for a locally running Voicebox app (https://github.com/jamiepine/voicebox, MIT),
    a voice studio built on Qwen3-TTS and other speech engines with a REST API on
    http://127.0.0.1:17493 by default.

    VoxForge uses Voicebox in two ways:
      - as a natural pronunciation guide: Voicebox speaks the lyrics of each phrase, and the
        speech is time-aligned onto the notes before the neural voice re-sings it;
      - as a training source: the engine asks Voicebox for minutes of speech in a voice
        profile and trains a neural voice on it (see engine/voxforge_engine/voicebox.py).

    Plain HTTP over a socket (localhost service, no proxy, no TLS), chunked responses supported. */
namespace voicebox
{
    struct Profile
    {
        juce::String id, name, language;
    };

    struct HttpResponse
    {
        int status = 0;
        juce::String contentType;
        juce::MemoryBlock body;
        juce::String error;           // transport error (no response)
        bool ok() const noexcept { return error.isEmpty() && status >= 200 && status < 300; }
    };

    constexpr const char* kDefaultUrl = "http://127.0.0.1:17493";

    /** One request. url = http://host[:port]/path. Blocks; call from a background thread. */
    HttpResponse request (const juce::String& method, const juce::String& url, const juce::String& jsonBody,
                          int timeoutMs, std::atomic<bool>* cancel = nullptr);

    bool health (const juce::String& baseUrl, juce::String& error);
    bool listProfiles (const juce::String& baseUrl, std::vector<Profile>& out, juce::String& error);

    /** Speaks text with a voice profile (POST /generate/stream). Results are cached on disk
        by (profile, language, text), so re-renders do not ask Voicebox again. */
    bool speak (const juce::String& baseUrl, const juce::String& profileId, const juce::String& text,
                const juce::String& language, juce::AudioBuffer<float>& out, double& sampleRate,
                juce::String& error, std::atomic<bool>* cancel = nullptr, bool* fromCache = nullptr);

    /** Text Voicebox should speak for a run of notes: syllables joined into words, holds and
        "original sound" placeholders skipped. Empty if a note uses explicit [phonemes]. */
    juce::String phraseText (const juce::StringArray& lyrics, const juce::Array<bool>& wordStarts);
}

} // namespace vf
