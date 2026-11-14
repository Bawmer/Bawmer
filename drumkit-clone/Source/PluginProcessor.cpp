#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace dkc;

//==============================================================================
class DrumkitCloneProcessor::AnalysisThread : public juce::Thread
{
public:
    AnalysisThread (DrumkitCloneProcessor& p, juce::File f, AnalysisSettings s)
        : juce::Thread ("Drumkit Clone analysis"), owner (p), file (std::move (f)), settings (s) {}

    void run() override
    {
        juce::String error;
        auto result = KitAnalyzer::analyseFile (file, settings, [this] (float p)
        {
            owner.progress.store (p);
            return ! threadShouldExit();
        }, error);

        if (threadShouldExit())
            return;

        {
            const juce::ScopedLock sl (owner.kitLock);
            if (result != nullptr && ! result->pads.empty())
            {
                owner.pendingKit = result;
                owner.hasPending = true;
                owner.sourcePath = file.getFullPathName();
                owner.status = {};
            }
            else
            {
                owner.status = error.isNotEmpty() ? error : juce::String ("Analysis failed");
            }
        }
        owner.analysing.store (false);
        owner.triggerAsyncUpdate();
    }

private:
    DrumkitCloneProcessor& owner;
    juce::File file;
    AnalysisSettings settings;
};

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout DrumkitCloneProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "master", 1 }, "Master",
                                                       NormalisableRange<float> (-36.0f, 6.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    for (int i = 0; i < kMaxPads; ++i)
    {
        const String n = "Pad " + String (i + 1) + " ";
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { gainId (i), 1 }, n + "Gain",
                                                           NormalisableRange<float> (-36.0f, 12.0f, 0.1f), 0.0f,
                                                           AudioParameterFloatAttributes().withLabel ("dB")));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { tuneId (i), 1 }, n + "Tune",
                                                           NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f,
                                                           AudioParameterFloatAttributes().withLabel ("st")));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { panId (i), 1 }, n + "Pan",
                                                           NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f));
    }
    return layout;
}

DrumkitCloneProcessor::DrumkitCloneProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "DrumkitClone", createLayout())
{
    masterParam = apvts.getRawParameterValue ("master");
    for (int i = 0; i < kMaxPads; ++i)
    {
        gainParams[(size_t) i] = apvts.getRawParameterValue (gainId (i));
        tuneParams[(size_t) i] = apvts.getRawParameterValue (tuneId (i));
        panParams[(size_t) i] = apvts.getRawParameterValue (panId (i));
        padTriggerCount[(size_t) i].store (0);
    }
}

DrumkitCloneProcessor::~DrumkitCloneProcessor()
{
    cancelAnalysis();
    cancelPendingUpdate();
}

bool DrumkitCloneProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void DrumkitCloneProcessor::prepareToPlay (double sampleRate, int)
{
    const juce::SpinLock::ScopedLockType sl (audioLock);
    hostSampleRate = sampleRate;
    for (auto& v : voices) v.active = false;
}

//==============================================================================
void DrumkitCloneProcessor::startVoice (int pad, float velocity)
{
    if (activeKit == nullptr || ! juce::isPositiveAndBelow (pad, (int) activeKit->pads.size()))
        return;

    const auto& sample = activeKit->pads[(size_t) pad];
    if (sample.audio.getNumSamples() < 2)
        return;

    // a pad retriggering chokes its previous hit over 4 ms instead of stacking up
    const float chokeStep = 1.0f / (float) (0.004 * hostSampleRate);
    Voice* target = nullptr;
    for (auto& v : voices)
    {
        if (v.active && v.pad == pad && v.fadeStep == 0.0f)
            v.fadeStep = chokeStep;
        if (! v.active && target == nullptr)
            target = &v;
    }
    if (target == nullptr)
    {
        // steal the voice that is furthest through its sample
        double furthest = -1.0;
        for (auto& v : voices)
        {
            const double done = v.position / juce::jmax (1, activeKit->pads[(size_t) v.pad].audio.getNumSamples());
            if (done > furthest) { furthest = done; target = &v; }
        }
    }

    const float gain = juce::Decibels::decibelsToGain (gainParams[(size_t) pad]->load() + masterParam->load())
                       * velocity * velocity;
    const float pan = juce::jlimit (-1.0f, 1.0f, panParams[(size_t) pad]->load());

    target->active = true;
    target->pad = pad;
    target->position = 0.0;
    target->rate = sample.sampleRate / hostSampleRate * std::pow (2.0, (double) tuneParams[(size_t) pad]->load() / 12.0);
    target->gainL = gain * juce::jmin (1.0f, 1.0f - pan);
    target->gainR = gain * juce::jmin (1.0f, 1.0f + pan);
    target->fade = 1.0f;
    target->fadeStep = 0.0f;

    padTriggerCount[(size_t) pad].fetch_add (1, std::memory_order_relaxed);
}

void DrumkitCloneProcessor::render (juce::AudioBuffer<float>& buffer, int start, int num)
{
    if (num <= 0 || activeKit == nullptr)
        return;

    float* outL = buffer.getWritePointer (0, start);
    float* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1, start) : nullptr;

    for (auto& v : voices)
    {
        if (! v.active)
            continue;

        const auto& audio = activeKit->pads[(size_t) v.pad].audio;
        const int len = audio.getNumSamples();
        const float* inL = audio.getReadPointer (0);
        const float* inR = audio.getReadPointer (audio.getNumChannels() > 1 ? 1 : 0);

        for (int i = 0; i < num; ++i)
        {
            const int idx = (int) v.position;
            if (idx >= len - 1) { v.active = false; break; }

            const float frac = (float) (v.position - idx);
            const float l = inL[idx] + frac * (inL[idx + 1] - inL[idx]);
            const float r = inR[idx] + frac * (inR[idx + 1] - inR[idx]);

            if (outR != nullptr)
            {
                outL[i] += l * v.gainL * v.fade;
                outR[i] += r * v.gainR * v.fade;
            }
            else
            {
                outL[i] += 0.5f * (l * v.gainL + r * v.gainR) * v.fade;
            }

            v.position += v.rate;
            if (v.fadeStep > 0.0f)
            {
                v.fade -= v.fadeStep;
                if (v.fade <= 0.0f) { v.active = false; break; }
            }
        }
    }
}

void DrumkitCloneProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    const juce::SpinLock::ScopedTryLockType lock (audioLock);
    if (! lock.isLocked())
        return;     // the kit is being swapped; one silent block

    const int numSamples = buffer.getNumSamples();

    if (auto triggers = uiTriggers.exchange (0))
        for (int pad = 0; pad < kMaxPads; ++pad)
            if (triggers & (1u << pad))
                startVoice (pad, 0.9f);

    int position = 0;
    for (const auto metadata : midi)
    {
        const auto msg = metadata.getMessage();
        if (! msg.isNoteOn())
            continue;       // one-shots: note-offs are ignored

        const int at = juce::jlimit (position, numSamples, metadata.samplePosition);
        render (buffer, position, at - position);
        position = at;
        startVoice (msg.getNoteNumber() - firstNote, msg.getFloatVelocity());
    }
    render (buffer, position, numSamples - position);
}

//==============================================================================
Kit::Ptr DrumkitCloneProcessor::getKit() const
{
    const juce::ScopedLock sl (kitLock);
    return kit;
}

void DrumkitCloneProcessor::setKit (Kit::Ptr newKit)
{
    Kit::Ptr old;
    {
        const juce::ScopedLock sl (kitLock);
        old = kit;
        kit = newKit;
        {
            const juce::SpinLock::ScopedLockType al (audioLock);
            activeKit = newKit.get();
            for (auto& v : voices) v.active = false;
        }
    }
    // 'old' is released here, off the audio thread and after the audio thread let go of it
}

juce::String DrumkitCloneProcessor::getStatusMessage() const
{
    const juce::ScopedLock sl (kitLock);
    return status;
}

void DrumkitCloneProcessor::handleAsyncUpdate()
{
    Kit::Ptr incoming;
    bool got = false;
    {
        const juce::ScopedLock sl (kitLock);
        if (hasPending)
        {
            incoming = pendingKit;
            pendingKit = nullptr;
            hasPending = false;
            got = true;
        }
    }
    if (got)
        setKit (incoming);
    sendChangeMessage();
}

void DrumkitCloneProcessor::cancelAnalysis()
{
    if (analysisThread != nullptr)
    {
        analysisThread->stopThread (10000);
        analysisThread.reset();
    }
    analysing.store (false);
}

void DrumkitCloneProcessor::startAnalysis (const juce::File& file)
{
    cancelAnalysis();
    {
        const juce::ScopedLock sl (kitLock);
        status = {};
    }
    progress.store (0.0f);
    analysing.store (true);

    AnalysisSettings settings;
    settings.sensitivity = sensitivity.load();
    settings.separation = separation.load();
    analysisThread = std::make_unique<AnalysisThread> (*this, file, settings);
    analysisThread->startThread();
    sendChangeMessage();
}

void DrumkitCloneProcessor::loadFile (const juce::File& file)
{
    if (file.existsAsFile())
        startAnalysis (file);
}

bool DrumkitCloneProcessor::canReanalyse() const
{
    const juce::ScopedLock sl (kitLock);
    return sourcePath.isNotEmpty() && juce::File (sourcePath).existsAsFile();
}

void DrumkitCloneProcessor::reanalyse()
{
    juce::String path;
    {
        const juce::ScopedLock sl (kitLock);
        path = sourcePath;
    }
    if (path.isNotEmpty())
        loadFile (juce::File (path));
}

//==============================================================================
bool DrumkitCloneProcessor::writeWav (const PadSample& pad, const juce::File& file)
{
    file.deleteFile();
    auto stream = file.createOutputStream();
    if (stream == nullptr)
        return false;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), pad.sampleRate,
                                                                          (unsigned int) pad.audio.getNumChannels(), 24, {}, 0));
    if (writer == nullptr)
        return false;
    stream.release();   // the writer owns it now
    return writer->writeFromAudioSampleBuffer (pad.audio, 0, pad.audio.getNumSamples());
}

bool DrumkitCloneProcessor::exportKit (const juce::File& folder, juce::String& error) const
{
    const auto current = getKit();
    if (current == nullptr || current->pads.empty())
    {
        error = "There is no kit to export";
        return false;
    }
    if (! folder.createDirectory())
    {
        error = "Couldn't create " + folder.getFullPathName();
        return false;
    }

    int index = 1;
    for (auto& pad : current->pads)
    {
        const auto name = juce::String (index++).paddedLeft ('0', 2) + " " + pad.name + ".wav";
        if (! writeWav (pad, folder.getChildFile (juce::File::createLegalFileName (name))))
        {
            error = "Couldn't write " + name;
            return false;
        }
    }
    return true;
}

//==============================================================================
// The kit itself is saved with the project (one-shots as 24-bit FLAC), so a session
// reopens with its pads even if the original file has moved.
void DrumkitCloneProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto root = apvts.copyState();
    root.removeChild (root.getChildWithName ("Kit"), nullptr);
    root.setProperty ("sensitivity", sensitivity.load(), nullptr);
    root.setProperty ("separation", separation.load(), nullptr);

    Kit::Ptr current;
    {
        const juce::ScopedLock sl (kitLock);
        current = kit;
        root.setProperty ("sourcePath", sourcePath, nullptr);
    }

    if (current != nullptr && ! current->pads.empty())
    {
        juce::ValueTree k ("Kit");
        k.setProperty ("sourceName", current->sourceName, nullptr);
        k.setProperty ("sourceSeconds", current->sourceSeconds, nullptr);
        k.setProperty ("totalHits", current->totalHits, nullptr);
        k.setProperty ("overview", juce::MemoryBlock (current->overview.data(), current->overview.size() * sizeof (float)), nullptr);

        juce::MemoryBlock hitData;
        {
            juce::MemoryOutputStream hs (hitData, false);
            for (auto& h : current->hits) { hs.writeFloat (h.position); hs.writeShort ((short) h.pad); }
        }
        k.setProperty ("hits", hitData, nullptr);

        juce::FlacAudioFormat flac;
        for (auto& pad : current->pads)
        {
            juce::MemoryBlock encoded;
            {
                auto* ms = new juce::MemoryOutputStream (encoded, false);
                std::unique_ptr<juce::AudioFormatWriter> writer (flac.createWriterFor (ms, pad.sampleRate,
                                                                                       (unsigned int) pad.audio.getNumChannels(), 24, {}, 5));
                if (writer == nullptr) { delete ms; continue; }
                writer->writeFromAudioSampleBuffer (pad.audio, 0, pad.audio.getNumSamples());
            }
            juce::ValueTree p ("Pad");
            p.setProperty ("name", pad.name, nullptr);
            p.setProperty ("type", pad.type, nullptr);
            p.setProperty ("hits", pad.hitCount, nullptr);
            p.setProperty ("audio", encoded, nullptr);
            k.appendChild (p, nullptr);
        }
        root.appendChild (k, nullptr);
    }

    juce::MemoryOutputStream out (dest, false);
    root.writeToStream (out);
}

void DrumkitCloneProcessor::setStateInformation (const void* data, int size)
{
    auto root = juce::ValueTree::readFromData (data, (size_t) size);
    if (! root.isValid() || root.getType() != apvts.state.getType())
        return;

    auto k = root.getChildWithName ("Kit");
    root.removeChild (k, nullptr);

    sensitivity.store ((float) root.getProperty ("sensitivity", 0.5f));
    separation.store ((float) root.getProperty ("separation", 0.5f));

    Kit::Ptr restored;
    if (k.isValid())
    {
        restored = new Kit();
        restored->sourceName = k["sourceName"].toString();
        restored->sourceSeconds = k["sourceSeconds"];
        restored->totalHits = k["totalHits"];

        if (auto* ov = k["overview"].getBinaryData())
        {
            restored->overview.resize (ov->getSize() / sizeof (float));
            std::memcpy (restored->overview.data(), ov->getData(), restored->overview.size() * sizeof (float));
        }
        if (auto* hd = k["hits"].getBinaryData())
        {
            juce::MemoryInputStream hs (*hd, false);
            while (hs.getNumBytesRemaining() >= 6)
            {
                HitMark m;
                m.position = hs.readFloat();
                m.pad = hs.readShort();
                restored->hits.push_back (m);
            }
        }

        juce::FlacAudioFormat flac;
        for (auto p : k)
        {
            auto* encoded = p["audio"].getBinaryData();
            if (encoded == nullptr || (int) restored->pads.size() >= kMaxPads)
                continue;
            std::unique_ptr<juce::AudioFormatReader> reader (flac.createReaderFor (new juce::MemoryInputStream (*encoded, false), true));
            if (reader == nullptr)
                continue;

            PadSample pad;
            pad.name = p["name"].toString();
            pad.type = p["type"];
            pad.hitCount = p["hits"];
            pad.sampleRate = reader->sampleRate;
            pad.audio.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&pad.audio, 0, (int) reader->lengthInSamples, 0, true, true);
            pad.rebuildThumb();
            restored->pads.push_back (std::move (pad));
        }
    }

    cancelAnalysis();
    {
        const juce::ScopedLock sl (kitLock);
        sourcePath = root.getProperty ("sourcePath").toString();
        status = {};
        hasPending = false;
        pendingKit = nullptr;
    }
    root.removeProperty ("sourcePath", nullptr);
    apvts.replaceState (root);
    setKit (restored);
    sendChangeMessage();
}

//==============================================================================
juce::AudioProcessorEditor* DrumkitCloneProcessor::createEditor()
{
    return new DrumkitCloneEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DrumkitCloneProcessor();
}
