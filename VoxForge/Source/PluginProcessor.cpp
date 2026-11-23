#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Core/Lyrics.h"
#include "DSP/AudioUtils.h"

using namespace vf;

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout VoxForgeProcessor::createLayout()
{
    using P = juce::AudioParameterFloat;
    using R = juce::NormalisableRange<float>;
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    auto attr = [] (const char* unit) { return juce::AudioParameterFloatAttributes().withLabel (unit); };

    p.push_back (std::make_unique<P> (juce::ParameterID { ParamIDs::vibDepth, 1 },   "Vibrato Depth",  R (0.0f, 150.0f, 0.1f), 35.0f,  attr ("cents")));
    p.push_back (std::make_unique<P> (juce::ParameterID { ParamIDs::vibRate, 1 },    "Vibrato Rate",   R (3.0f, 9.0f, 0.01f), 5.5f,    attr ("Hz")));
    p.push_back (std::make_unique<P> (juce::ParameterID { ParamIDs::vibDelay, 1 },   "Vibrato Delay",  R (0.0f, 1200.0f, 1.0f), 280.0f, attr ("ms")));
    p.push_back (std::make_unique<P> (juce::ParameterID { ParamIDs::breath, 1 },     "Breathiness",    R (0.0f, 1.0f, 0.001f), 0.15f,  attr ("")));
    p.push_back (std::make_unique<P> (juce::ParameterID { ParamIDs::bendRange, 1 },  "Pitch-Bend Range", R (0.0f, 24.0f, 1.0f), 2.0f,  attr ("st")));
    p.push_back (std::make_unique<P> (juce::ParameterID { ParamIDs::porta, 1 },      "Portamento",     R (0.0f, 400.0f, 1.0f), 70.0f,  attr ("ms")));
    p.push_back (std::make_unique<P> (juce::ParameterID { ParamIDs::expression, 1 }, "Expression",     R (0.0f, 1.0f, 0.001f), 0.75f,  attr ("")));
    p.push_back (std::make_unique<P> (juce::ParameterID { ParamIDs::tension, 1 },    "Tension",        R (0.0f, 1.0f, 0.001f), 0.5f,   attr ("")));
    p.push_back (std::make_unique<P> (juce::ParameterID { ParamIDs::formant, 1 },    "Formant / Gender", R (-6.0f, 6.0f, 0.01f), 0.0f, attr ("st")));
    p.push_back (std::make_unique<P> (juce::ParameterID { ParamIDs::humanize, 1 },   "Humanize",       R (0.0f, 1.0f, 0.001f), 0.35f,  attr ("")));
    p.push_back (std::make_unique<P> (juce::ParameterID { ParamIDs::nuance, 1 },     "Original Nuance", R (0.0f, 1.0f, 0.001f), 0.6f,  attr ("")));
    p.push_back (std::make_unique<P> (juce::ParameterID { ParamIDs::gain, 1 },       "Output Gain",    R (-24.0f, 12.0f, 0.1f), 0.0f,  attr ("dB")));
    p.push_back (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ParamIDs::live, 1 }, "Live MIDI Voice", true));
    p.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { ParamIDs::monitor, 1 }, "Monitor",
                                                               juce::StringArray { "Rendered", "Original Stem", "Both" }, 0));
    return { p.begin(), p.end() };
}

VoxForgeProcessor::VoxForgeProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Params", createLayout())
{
    if (! Lexicon::get().hasUserDictionary())       // shared by all instances: parse once per process
        Lexicon::get().loadDictionary (Paths::cmuDict());

    // a short demo phrase so the plug-in makes sound out of the box
    const int demo[] = { 60, 62, 64, 65, 67, 67 };
    for (int i = 0; i < 6; ++i)
    {
        Note n; n.startBeat = i; n.lengthBeats = i == 5 ? 2.0 : 0.95; n.pitch = demo[i];
        proj.notes.push_back (n);
    }
    proj.lyricsText = "hel-lo world sing with me";
    applyLyricsToNotes (proj.notes, proj.lyricsText);
    renderDirty = true;          // render the demo phrase once the host is running
    dirtySince = 0;

    startTimerHz (30);
}

VoxForgeProcessor::~VoxForgeProcessor()
{
    stopTimer();
    if (trainJob != nullptr) trainJob->cancel();
}

bool VoxForgeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

ExpressionSettings VoxForgeProcessor::expressionSettings() const
{
    auto v = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    ExpressionSettings s;
    s.vibratoDepthCents = v (ParamIDs::vibDepth);
    s.vibratoRateHz = v (ParamIDs::vibRate);
    s.vibratoDelayMs = v (ParamIDs::vibDelay);
    s.breathiness = v (ParamIDs::breath);
    s.pitchBendRange = v (ParamIDs::bendRange);
    s.portamentoMs = v (ParamIDs::porta);
    s.expression = v (ParamIDs::expression);
    s.tension = v (ParamIDs::tension);
    s.formantShift = v (ParamIDs::formant);
    s.humanize = v (ParamIDs::humanize);
    s.nuance = v (ParamIDs::nuance);
    return s;
}

//==============================================================================
void VoxForgeProcessor::prepareToPlay (double sr, int block)
{
    const bool rateChanged = std::abs (sr - sampleRate) > 0.5;
    sampleRate = sr;
    blockSize = block;
    liveVoice.prepare (sr, block);
    updateLiveVoice();

    if (rateChanged)
    {
        // re-fit the current take and stem to the new host rate (44.1 ... 192 kHz)
        std::shared_ptr<RenderedTake> t;
        { const juce::SpinLock::ScopedLockType sl (takeLock); t = take; }
        if (t != nullptr)
        {
            auto copy = std::make_shared<RenderedTake> (*t);
            copy->hostRate = sr;
            copy->hostAudio = resampleMono (copy->engineAudio, kEngineSampleRate, sr);
            const juce::SpinLock::ScopedLockType sl (takeLock);
            graveyard.push_back (take);
            take = copy;
        }
        if (stemEngine.getNumSamples() > 0)
        {
            auto s = resampleMono (stemEngine, kEngineSampleRate, sr);
            const juce::SpinLock::ScopedLockType sl (stemLock);
            stemHost = std::move (s);
        }
    }
}

void VoxForgeProcessor::pushCap (const CapEvent& e)
{
    int s1, n1, s2, n2;
    capFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0) capBuffer[(size_t) s1] = e;
    else if (n2 > 0) capBuffer[(size_t) s2] = e;
    capFifo.finishedWrite (n1 + n2);
}

void VoxForgeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    buffer.clear();

    // ---- host transport -------------------------------------------------------
    bool playing = false;
    double ppq = 0.0, bpm = lastBpm.load();
    bool hasPpq = false;
    juce::int64 timeSamples = 0;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            if (auto b = pos->getBpm()) bpm = *b;
            if (auto p = pos->getPpqPosition()) { ppq = *p; hasPpq = true; }
            if (auto ts = pos->getTimeInSamples()) timeSamples = *ts;
        }
    }
    lastBpm = bpm;
    hostPlaying = playing;
    if (! hasPpq) ppq = (double) timeSamples / sampleRate * bpm / 60.0;
    const double samplesPerBeat = sampleRate * 60.0 / juce::jmax (1.0, bpm);

    // ---- MIDI capture ----------------------------------------------------------
    const bool armed = captureArmed.load();
    if (armed && playing && ! wasPlaying) { pushCap ({ 8, ppq, 0, (float) bpm }); capturing = true; }
    if (wasPlaying && ! playing && capturing.load()) { pushCap ({ 9, ppq, 0, 0.0f }); capturing = false; }
    wasPlaying = playing;

    const bool liveOn = apvts.getRawParameterValue (ParamIDs::live)->load() > 0.5f;
    const auto settings = expressionSettings();
    liveVoice.setSettings (settings);

    for (const auto m : midi)
    {
        const auto msg = m.getMessage();
        const double beat = ppq + m.samplePosition / samplesPerBeat;
        if (capturing.load())
        {
            if (msg.isNoteOn())              pushCap ({ 0, beat, msg.getNoteNumber(), msg.getFloatVelocity() });
            else if (msg.isNoteOff())        pushCap ({ 1, beat, msg.getNoteNumber(), 0.0f });
            else if (msg.isPitchWheel())     pushCap ({ 2, beat, 0, (float) (msg.getPitchWheelValue() - 8192) / 8192.0f });
            else if (msg.isController())     pushCap ({ 3, beat, msg.getControllerNumber(), (float) msg.getControllerValue() / 127.0f });
        }
        if (msg.isNoteOn())             liveVoice.noteOn (msg.getNoteNumber(), msg.getFloatVelocity());
        else if (msg.isNoteOff())       liveVoice.noteOff (msg.getNoteNumber());
        else if (msg.isPitchWheel())    liveVoice.pitchWheel ((float) (msg.getPitchWheelValue() - 8192) / 8192.0f);
        else if (msg.isController())    liveVoice.controller (msg.getControllerNumber(), (float) msg.getControllerValue() / 127.0f);
        else if (msg.isAllNotesOff() || msg.isAllSoundOff()) liveVoice.allNotesOff();
    }

    const float gain = juce::Decibels::decibelsToGain (apvts.getRawParameterValue (ParamIDs::gain)->load());
    const int monitor = (int) apvts.getRawParameterValue (ParamIDs::monitor)->load();

    // ---- rendered take / original stem playback -----------------------------------
    double startSec = -1.0;
    if (playing)
    {
        startSec = ppq * 60.0 / juce::jmax (1.0, projectBpm.load());
        playBeat = ppq;
        previewing = false;
    }
    else if (previewing.load())
    {
        startSec = previewSample.load() / sampleRate;
        playBeat = startSec * projectBpm.load() / 60.0;
        previewSample = previewSample.load() + numSamples;
    }

    bool takePlayed = false;
    if (startSec >= 0.0 && ! capturing.load())
    {
        const auto start = (juce::int64) std::llround (startSec * sampleRate);
        auto copyFrom = [&] (const juce::AudioBuffer<float>& src, juce::int64 srcStart, float g)
        {
            const juce::int64 len = src.getNumSamples();
            if (srcStart >= len || srcStart + numSamples <= 0) return false;
            const int dstOff = (int) std::max<juce::int64> (0, -srcStart);
            const juce::int64 s0 = std::max<juce::int64> (0, srcStart);
            const int n = (int) std::min<juce::int64> (numSamples - dstOff, len - s0);
            if (n <= 0) return false;
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.addFrom (ch, dstOff, src, 0, (int) s0, n, g);
            return true;
        };

        if (monitor == 0 || monitor == 2)
        {
            const juce::SpinLock::ScopedTryLockType sl (takeLock);
            if (sl.isLocked() && take != nullptr && std::abs (take->hostRate - sampleRate) < 0.5)
                takePlayed = copyFrom (take->hostAudio, start, gain);
        }
        if (monitor == 1 || monitor == 2)
        {
            const juce::SpinLock::ScopedTryLockType sl (stemLock);
            if (sl.isLocked() && stemHost.getNumSamples() > 0)
                copyFrom (stemHost, start - (juce::int64) std::llround (stemOffsetSec.load() * sampleRate), monitor == 2 ? 0.7f : 1.0f);
        }
    }

    // ---- live voice -----------------------------------------------------------------
    bool hasRender;
    { const juce::SpinLock::ScopedTryLockType sl (takeLock); hasRender = ! sl.isLocked() || take != nullptr; }
    juce::ignoreUnused (takePlayed);
    if (liveOn && (capturing.load() || ! playing || ! hasRender))
        liveVoice.render (buffer, 0, numSamples, gain * 0.8f);

    midi.clear();
}

//==============================================================================
void VoxForgeProcessor::timerCallback()
{
    drainCapture();

    if (auto t = renderEngine.takeFinished())
        installTake (t);

    if (pendingCacheHash.isNotEmpty())
    {
        auto h = pendingCacheHash;
        pendingCacheHash.clear();
        if (auto cached = RenderEngine::loadCached (h, sampleRate)) installTake (cached);
        else if (! proj.notes.empty()) { renderDirty = true; dirtySince = 0; }
    }

    if (renderDirty && autoRender && ! captureArmed.load()
        && juce::Time::getMillisecondCounter() - dirtySince > 700)
    {
        renderDirty = false;
        renderNow();
    }

    // parameter changes also invalidate the render
    if (autoRender && ! renderEngine.isBusy() && installedHash.isNotEmpty() && ! renderDirty
        && currentHash() != installedHash && currentHash() != lastRequestedHash)
    {
        renderDirty = true;
        dirtySince = juce::Time::getMillisecondCounter();
    }

    if (! renderEngine.isBusy())
    {
        std::vector<std::shared_ptr<RenderedTake>> dead;
        {
            const juce::SpinLock::ScopedLockType sl (takeLock);
            dead.swap (graveyard);          // O(1) under the lock ...
        }
        dead.clear();                       // ... buffers are freed outside it
    }

    if (trainJob != nullptr && trainJob->isFinished())
    {
        if (trainJob->succeeded())
        {
            presetManager.rescan();
            if (auto* v = presetManager.find (trainingVoiceId))
            {
                auto copy = *v;
                const auto res = trainJob->result();
                copy.engine = "neural";
                copy.neuralModel = res.getProperty ("model", "voice.onnx").toString();
                copy.neuralSampleRate = res.getProperty ("sampleRate", 48000.0);
                copy.neuralHop = res.getProperty ("hop", 960);
                copy.save();
                presetManager.rescan();
                lastMessage = "Neural voice '" + copy.name + "' is ready.";
                if (voice.id == copy.id) selectVoice (copy.id, false);
            }
        }
        else lastMessage = "Training failed: " + trainJob->error();
        trainJob.reset();
    }
}

void VoxForgeProcessor::drainCapture()
{
    int s1, n1, s2, n2;
    const int ready = capFifo.getNumReady();
    if (ready == 0) return;
    capFifo.prepareToRead (ready, s1, n1, s2, n2);
    std::vector<CapEvent> evs;
    for (int i = 0; i < n1; ++i) evs.push_back (capBuffer[(size_t) (s1 + i)]);
    for (int i = 0; i < n2; ++i) evs.push_back (capBuffer[(size_t) (s2 + i)]);
    capFifo.finishedRead (n1 + n2);

    bool changed = false;
    for (auto& e : evs)
    {
        switch (e.type)
        {
            case 8:   // capture pass starts: replace the notes
                proj.notes.clear();
                proj.clearControllers();
                openNotes.clear();
                captureBpm = e.b;
                proj.bpm = e.b;
                projectBpm = e.b;
                changed = true;
                break;
            case 0:
            {
                Note n;
                n.startBeat = e.beat; n.lengthBeats = 0.0; n.pitch = e.a; n.velocity = e.b;
                proj.notes.push_back (n);
                openNotes[e.a] = proj.notes.size() - 1;
                changed = true;
                break;
            }
            case 1:
                if (auto it = openNotes.find (e.a); it != openNotes.end())
                {
                    auto& n = proj.notes[it->second];
                    n.lengthBeats = juce::jmax (0.05, e.beat - n.startBeat);
                    openNotes.erase (it);
                    changed = true;
                }
                break;
            case 2: proj.pitchBend.push_back ({ e.beat, e.b }); break;
            case 3:
                if (e.a == 1)  proj.modWheel.push_back ({ e.beat, e.b });
                if (e.a == 2)  proj.breathCC.push_back ({ e.beat, e.b });
                if (e.a == 11) proj.expressionCC.push_back ({ e.beat, e.b });
                break;
            case 9:   // transport stopped: close the pass
            {
                for (auto& [pitch, idx] : openNotes)
                    proj.notes[idx].lengthBeats = juce::jmax (0.05, e.beat - proj.notes[idx].startBeat);
                openNotes.clear();
                // monophonic clean-up: trim overlaps (singers sing one note at a time)
                proj.sortNotes();
                for (size_t i = 0; i + 1 < proj.notes.size(); ++i)
                    if (proj.notes[i].endBeat() > proj.notes[i + 1].startBeat)
                        proj.notes[i].lengthBeats = juce::jmax (0.05, proj.notes[i + 1].startBeat - proj.notes[i].startBeat);
                applyLyricsToNotes (proj.notes, proj.lyricsText);
                captureArmed = false;
                lastMessage = "Captured " + juce::String ((int) proj.notes.size()) + " notes from the DAW.";
                changed = true;
                break;
            }
            default: break;
        }
    }
    if (changed) projectChanged (! captureArmed.load());
}

void VoxForgeProcessor::setCaptureArmed (bool shouldArm)
{
    captureArmed = shouldArm;
    if (shouldArm) lastMessage = "Capture armed: press Play in your DAW to record the MIDI part.";
}

//==============================================================================
void VoxForgeProcessor::projectChanged (bool rerender)
{
    proj.sortNotes();
    projectBpm = proj.bpm;
    stemOffsetSec = proj.beatsToSeconds (proj.stemOffsetBeats);
    ++version;
    updateLiveVoice();
    if (rerender)
    {
        renderDirty = true;
        dirtySince = juce::Time::getMillisecondCounter();
    }
}

void VoxForgeProcessor::updateLiveVoice()
{
    std::vector<juce::StringArray> syl;
    for (auto& n : proj.notes) syl.push_back (phonemesForNote (n));
    liveVoice.setSyllables (std::move (syl));
    liveVoice.setTimbre (voice.timbre);
}

juce::String VoxForgeProcessor::currentHash() const
{
    const auto modelStamp = voice.isNeural() ? juce::String (voice.neuralModelFile().getLastModificationTime().toMilliseconds()) : juce::String();
    const auto replace = proj.stemPath.isNotEmpty() && std::any_of (proj.notes.begin(), proj.notes.end(), [] (const Note& n) { return n.fromSource; });
    return proj.contentHash (expressionSettings(), voice.id + modelStamp + juce::String (voice.timbre.formantScale)
                             + (replace ? "R" : "S"));
}

void VoxForgeProcessor::renderNow()
{
    RenderRequest r;
    r.project = proj;
    r.settings = expressionSettings();
    r.voice = voice;
    r.replaceMode = proj.stemPath.isNotEmpty()
                    && std::any_of (proj.notes.begin(), proj.notes.end(), [] (const Note& n) { return n.fromSource; });
    r.hostRate = sampleRate;
    r.hash = currentHash();
    lastRequestedHash = r.hash;
    renderEngine.request (std::move (r));
}

void VoxForgeProcessor::installTake (std::shared_ptr<RenderedTake> t)
{
    if (t == nullptr) return;
    if (std::abs (t->hostRate - sampleRate) > 0.5)
    {
        t->hostRate = sampleRate;
        t->hostAudio = resampleMono (t->engineAudio, kEngineSampleRate, sampleRate);
    }
    {
        const juce::SpinLock::ScopedLockType sl (takeLock);
        graveyard.push_back (take);
        take = t;
    }
    installedHash = t->hash;
    lastMessage = "Render ready: " + t->summary;
}

bool VoxForgeProcessor::hasTake() const
{
    const juce::SpinLock::ScopedLockType sl (takeLock);
    return take != nullptr;
}

juce::String VoxForgeProcessor::takeSummary() const
{
    const juce::SpinLock::ScopedLockType sl (takeLock);
    return take != nullptr ? take->summary : juce::String ("not rendered yet");
}

bool VoxForgeProcessor::renderIsStale() const
{
    return installedHash.isEmpty() || installedHash != currentHash();
}

const juce::AudioBuffer<float>* VoxForgeProcessor::takeEngineAudio() const
{
    const juce::SpinLock::ScopedLockType sl (takeLock);
    return take != nullptr ? &take->engineAudio : nullptr;
}

bool VoxForgeProcessor::exportTake (const juce::File& f)
{
    std::shared_ptr<RenderedTake> t;
    { const juce::SpinLock::ScopedLockType sl (takeLock); t = take; }
    if (t == nullptr) return false;
    const double sr = sampleRate > 0 ? sampleRate : kEngineSampleRate;
    auto audio = resampleMono (t->engineAudio, kEngineSampleRate, sr);
    return writeWav (f, audio, sr, 24);
}

void VoxForgeProcessor::startPreview (double fromBeat)
{
    previewSample = proj.beatsToSeconds (juce::jmax (0.0, fromBeat)) * sampleRate;
    previewing = true;
}

//==============================================================================
void VoxForgeProcessor::selectVoice (const juce::String& id, bool applyExpression)
{
    presetManager.rescan();
    if (auto* v = presetManager.find (id)) voice = *v;
    else voice = VoiceModel::builtinDefault();

    if (applyExpression && voice.id != "builtin-default")
    {
        auto set = [this] (const char* pid, float value)
        {
            if (auto* p = apvts.getParameter (pid)) p->setValueNotifyingHost (p->convertTo0to1 (value));
        };
        set (ParamIDs::vibDepth, voice.expression.vibratoDepthCents);
        set (ParamIDs::vibRate, voice.expression.vibratoRateHz);
        set (ParamIDs::vibDelay, voice.expression.vibratoDelayMs);
        set (ParamIDs::breath, voice.expression.breathiness);
        set (ParamIDs::porta, voice.expression.portamentoMs);
        set (ParamIDs::tension, voice.expression.tension);
        set (ParamIDs::formant, voice.expression.formantShift);
        set (ParamIDs::humanize, voice.expression.humanize);
    }
    updateLiveVoice();
    lastMessage = "Voice: " + voice.name + (voice.isNeural() ? " (neural)" : " (lite)");
    renderDirty = true;
    dirtySince = juce::Time::getMillisecondCounter();
}

void VoxForgeProcessor::voicesChanged()
{
    presetManager.rescan();
    if (presetManager.find (voice.id) == nullptr) selectVoice ("builtin-default", false);
}

void VoxForgeProcessor::startNeuralTraining (const VoiceModel& v, int epochs, const juce::String& quality)
{
    if (trainJob != nullptr) return;
    juce::StringArray args { "train",
                             "--name", v.name,
                             "--voice-dir", v.folder.getFullPathName(),
                             "--models-dir", Paths::modelsDir().getFullPathName(),
                             "--epochs", juce::String (epochs),
                             "--quality", quality };
    if (Settings::get().useGpu()) args.add ("--gpu");
    args.add ("--samples");
    for (auto& s : v.samples) args.add (v.folder.getChildFile (s).getFullPathName());

    trainingVoiceId = v.id;
    trainJob = std::make_unique<EngineJob> (args, "Training '" + v.name + "'");
    trainJob->start();
    lastMessage = "Neural training started for '" + v.name + "'.";
}

void VoxForgeProcessor::cancelTraining()
{
    if (trainJob != nullptr) trainJob->cancel();
}

//==============================================================================
bool VoxForgeProcessor::importMidiFile (const juce::File& f, juce::String& error)
{
    juce::FileInputStream in (f);
    juce::MidiFile mf;
    if (! in.openedOk() || ! mf.readFrom (in)) { error = "Not a readable MIDI file."; return false; }
    const int tpq = mf.getTimeFormat();
    if (tpq <= 0) { error = "SMPTE-timed MIDI files are not supported."; return false; }

    // take the track with the most notes (the melody)
    int best = -1, bestCount = 0;
    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        int c = 0;
        for (auto* e : *mf.getTrack (t)) if (e->message.isNoteOn()) ++c;
        if (c > bestCount) { bestCount = c; best = t; }
    }
    if (best < 0) { error = "The MIDI file contains no notes."; return false; }

    juce::MidiMessageSequence seq (*mf.getTrack (best));
    seq.updateMatchedPairs();
    proj.notes.clear();
    proj.clearControllers();
    for (int i = 0; i < seq.getNumEvents(); ++i)
    {
        auto* e = seq.getEventPointer (i);
        const double beat = e->message.getTimeStamp() / tpq;
        if (e->message.isNoteOn() && e->noteOffObject != nullptr)
        {
            Note n;
            n.startBeat = beat;
            n.lengthBeats = juce::jmax (0.05, e->noteOffObject->message.getTimeStamp() / tpq - beat);
            n.pitch = e->message.getNoteNumber();
            n.velocity = e->message.getFloatVelocity();
            proj.notes.push_back (n);
        }
        else if (e->message.isPitchWheel())
            proj.pitchBend.push_back ({ beat, (float) (e->message.getPitchWheelValue() - 8192) / 8192.0f });
        else if (e->message.isController())
        {
            const float v = e->message.getControllerValue() / 127.0f;
            if (e->message.getControllerNumber() == 1)  proj.modWheel.push_back ({ beat, v });
            if (e->message.getControllerNumber() == 2)  proj.breathCC.push_back ({ beat, v });
            if (e->message.getControllerNumber() == 11) proj.expressionCC.push_back ({ beat, v });
        }
    }
    proj.sortNotes();
    for (size_t i = 0; i + 1 < proj.notes.size(); ++i)
        if (proj.notes[i].endBeat() > proj.notes[i + 1].startBeat)
            proj.notes[i].lengthBeats = juce::jmax (0.05, proj.notes[i + 1].startBeat - proj.notes[i].startBeat);
    proj.bpm = lastBpm.load();
    applyLyricsToNotes (proj.notes, proj.lyricsText);
    projectChanged();
    return true;
}

//==============================================================================
bool VoxForgeProcessor::loadStem (const juce::File& f, juce::String& error)
{
    juce::AudioBuffer<float> mono; double sr = 0;
    if (! loadAudioMono (f, mono, sr)) { error = "Cannot read " + f.getFileName(); return false; }
    stemEngine = resampleMono (mono, sr, kEngineSampleRate);
    auto host = resampleMono (mono, sr, sampleRate);
    {
        const juce::SpinLock::ScopedLockType sl (stemLock);
        stemHost = std::move (host);
    }
    proj.stemPath = f.getFullPathName();
    projectChanged (false);
    lastMessage = "Loaded stem " + f.getFileName() + " (" + juce::String (mono.getNumSamples() / sr, 1) + " s)";
    return true;
}

void VoxForgeProcessor::analyseStem (const VocalToMidiOptions& opts, bool transcribe)
{
    if (proj.stemPath.isEmpty()) return;
    analyzer.start (juce::File (proj.stemPath), opts, transcribe, Settings::get().language());
}

bool VoxForgeProcessor::applyStemAnalysis()
{
    if (! analyzer.hasResult()) return false;
    auto r = analyzer.takeResult();
    if (! r.ok) { lastMessage = "Analysis failed: " + r.error; return false; }
    proj.bpm = lastBpm.load();
    notesFromAnalysis (r, proj);
    lastAnalysisNote = r.transcriptionNote;
    lastMessage = "Vocal-to-MIDI: " + juce::String ((int) proj.notes.size()) + " notes"
                  + (r.words.empty() ? juce::String() : ", " + juce::String ((int) r.words.size()) + " words recognised");
    projectChanged();
    return true;
}

//==============================================================================
void VoxForgeProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree root ("VoxForgeState");
    root.setProperty ("version", 1, nullptr);
    root.setProperty ("voiceId", voice.id, nullptr);
    root.setProperty ("takeHash", installedHash, nullptr);
    root.setProperty ("autoRender", autoRender, nullptr);
    root.setProperty ("tab", lastEditorTab, nullptr);
    root.appendChild (apvts.copyState(), nullptr);
    root.appendChild (proj.toValueTree(), nullptr);
    if (auto xml = root.createXml()) copyXmlToBinary (*xml, destData);
}

void VoxForgeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr) return;
    auto root = juce::ValueTree::fromXml (*xml);
    if (! root.hasType ("VoxForgeState")) return;

    if (auto params = root.getChildWithName (apvts.state.getType()); params.isValid())
        apvts.replaceState (params);
    proj.fromValueTree (root.getChildWithName ("VocalProject"));
    autoRender = root.getProperty ("autoRender", true);
    lastEditorTab = root.getProperty ("tab", 2);
    const auto voiceId = root.getProperty ("voiceId", "builtin-default").toString();
    const auto hash = root.getProperty ("takeHash", "").toString();

    std::weak_ptr<int> alive = aliveToken;
    juce::MessageManager::callAsync ([this, alive, voiceId, hash]
    {
        if (alive.expired()) return;
        selectVoice (voiceId, false);
        if (proj.stemPath.isNotEmpty() && juce::File (proj.stemPath).existsAsFile())
        {
            juce::String err;
            loadStem (juce::File (proj.stemPath), err);
        }
        projectChanged (false);
        renderDirty = false;
        pendingCacheHash = hash.isNotEmpty() ? hash : currentHash();
    });
}

juce::AudioProcessorEditor* VoxForgeProcessor::createEditor()
{
    return new VoxForgeEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VoxForgeProcessor();
}
