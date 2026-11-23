#pragma once

#include "../Core/VocalTypes.h"
#include "../DSP/FormantSynth.h"

namespace vf
{

/** File-system locations. Windows: %APPDATA%\VoxForge\... */
namespace Paths
{
    juce::File dataDir();
    juce::File voicesDir();
    juce::File modelsDir();     // shared models (content encoder, base checkpoints, whisper)
    juce::File cacheDir();
    juce::File cmuDict();
    juce::File contentEncoder();
}

/** Persistent global settings (engine location, language, GPU ...). */
class Settings
{
public:
    static Settings& get();
    juce::PropertiesFile& props() { return *file; }

    juce::String engineCommand() const;      // user override; empty = auto
    juce::StringArray resolveEngineCommand() const;   // command prefix to launch the trainer/transcriber
    juce::String language() const;           // whisper language code or "auto"
    juce::String lyricsModel() const;        // whisper model size: tiny, base, small, medium, large-v3
    bool useGpu() const;

private:
    Settings();
    std::unique_ptr<juce::PropertiesFile> file;
};

struct VoiceStats
{
    double seconds = 0.0;      // voiced seconds analysed
    float medianMidi = 60.0f, minMidi = 48.0f, maxMidi = 72.0f;
    float vibratoRate = 5.5f, vibratoDepth = 30.0f;
};

/** A cloned voice = a named preset folder in Voices/. */
struct VoiceModel
{
    juce::String name;
    juce::String id;
    juce::String created;
    juce::String engine = "lite";      // "lite" (DSP clone) or "neural" (trained ONNX model)
    juce::String notes;
    VoiceTimbre timbre;
    ExpressionSettings expression;     // the preset's default expression
    VoiceStats stats;
    juce::String neuralModel;          // file name inside the folder, e.g. voice.onnx
    double neuralSampleRate = 48000.0;
    int neuralHop = 960;
    juce::StringArray samples;         // relative paths of training samples
    juce::File folder;

    bool isNeural() const { return engine == "neural" && neuralModelFile().existsAsFile(); }
    juce::File neuralModelFile() const { return neuralModel.isNotEmpty() ? folder.getChildFile (neuralModel) : juce::File(); }

    juce::var toJson() const;
    static VoiceModel fromJson (const juce::var& v, const juce::File& folder);
    bool save() const;
    static VoiceModel builtinDefault();
};

/** Scans, saves, renames, deletes, imports and exports voice presets. */
class PresetManager
{
public:
    PresetManager();

    void rescan();
    const std::vector<VoiceModel>& voices() const { return list; }
    const VoiceModel* find (const juce::String& nameOrId) const;

    /** Creates a unique folder for a new voice and returns it. */
    juce::File createVoiceFolder (const juce::String& name) const;
    bool saveVoice (VoiceModel& v);
    bool renameVoice (const juce::String& id, const juce::String& newName);
    bool deleteVoice (const juce::String& id);
    bool duplicateVoice (const juce::String& id, const juce::String& newName);
    bool exportVoice (const juce::String& id, const juce::File& zipFile) const;   // .vfvoice
    juce::Result importVoice (const juce::File& zipFile);

    static juce::String sanitise (const juce::String& name);

private:
    std::vector<VoiceModel> list;
};

} // namespace vf
