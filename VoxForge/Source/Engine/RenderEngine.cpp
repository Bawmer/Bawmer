#include "RenderEngine.h"
#include "../DSP/FormantSynth.h"
#include "../DSP/AudioUtils.h"
#include "../DSP/Spectral.h"
#include "../Core/Phonemes.h"

namespace vf
{

RenderEngine::RenderEngine() : juce::Thread ("VoxForge renderer")
{
    startThread (juce::Thread::Priority::normal);
}

RenderEngine::~RenderEngine()
{
    cancelFlag = true;
    signalThreadShouldExit();
    wake.signal();
    stopThread (8000);
}

void RenderEngine::request (RenderRequest r)
{
    {
        // set both under the lock: the render thread clears cancelFlag only when it
        // takes a request, so a new request can never cancel itself
        const juce::ScopedLock sl (lock);
        pending = std::make_unique<RenderRequest> (std::move (r));
        cancelFlag = true;     // abort a running render, the new request wins
    }
    wake.signal();
}

void RenderEngine::cancel()
{
    const juce::ScopedLock sl (lock);
    pending.reset();
    cancelFlag = true;
}

std::shared_ptr<RenderedTake> RenderEngine::takeFinished()
{
    const juce::ScopedLock sl (lock);
    auto f = finished;
    finished.reset();
    return f;
}

void RenderEngine::setStatus (const juce::String& s, float p)
{
    prog = p;
    const juce::ScopedLock sl (lock);
    statusText = s;
}

juce::String RenderEngine::neuralStatus()
{
    if (! NeuralBackend::isCompiledIn()) return backend.status();
    if (! backend.hasContentEncoder())
    {
        juce::String err;
        if (Paths::contentEncoder().existsAsFile()) backend.loadContentEncoder (Paths::contentEncoder(), err);
        else return "neural ready once Models/content_encoder.onnx is installed";
    }
    return backend.status();
}

bool RenderEngine::prepareNeural (const VoiceModel& v)
{
    if (! NeuralBackend::isCompiledIn() || ! v.isNeural()) return false;
    juce::String err;
    if (! backend.hasContentEncoder() && ! backend.loadContentEncoder (Paths::contentEncoder(), err))
    {
        setStatus ("Neural engine unavailable: " + err, prog);
        return false;
    }
    if (! backend.loadVoice (v.neuralModelFile(), err, v.neuralSampleRate, v.neuralHop))
    {
        setStatus ("Voice model failed to load: " + err, prog);
        return false;
    }
    return true;
}

/** Neural voices may run at 32/40/48 kHz; takes are kept at the engine rate. */
static juce::AudioBuffer<float> toEngineRate (const std::vector<float>& audio, double rate)
{
    juce::AudioBuffer<float> b (1, juce::jmax (1, (int) audio.size()));
    b.clear();
    if (! audio.empty()) b.copyFrom (0, 0, audio.data(), (int) audio.size());
    return std::abs (rate - kEngineSampleRate) < 1.0 ? b : resampleMono (b, rate, kEngineSampleRate);
}

static juce::File cacheFileFor (const juce::String& hash) { return Paths::cacheDir().getChildFile (hash + ".wav"); }

std::shared_ptr<RenderedTake> RenderEngine::loadCached (const juce::String& hash, double hostRate)
{
    auto f = cacheFileFor (hash);
    if (! f.existsAsFile()) return nullptr;
    juce::AudioBuffer<float> audio;
    double sr = 0;
    if (! loadAudioMono (f, audio, sr) || std::abs (sr - kEngineSampleRate) > 1.0) return nullptr;
    auto t = std::make_shared<RenderedTake>();
    t->engineAudio = std::move (audio);
    t->hostRate = hostRate;
    t->hostAudio = resampleMono (t->engineAudio, kEngineSampleRate, hostRate);
    t->hash = hash;
    t->summary = "Loaded from render cache";
    return t;
}

void RenderEngine::run()
{
    while (! threadShouldExit())
    {
        wake.wait (500);
        std::unique_ptr<RenderRequest> req;
        {
            const juce::ScopedLock sl (lock);
            req = std::move (pending);
            if (req != nullptr) cancelFlag = false;
        }
        if (req == nullptr) continue;

        busy = true;
        setStatus ("Rendering...", 0.0f);
        std::shared_ptr<RenderedTake> take;
        try
        {
            take = req->replaceMode ? renderReplace (*req) : renderSynth (*req);
        }
        catch (const std::exception& e)
        {
            setStatus (juce::String ("Render failed: ") + e.what(), 0.0f);
        }

        if (take != nullptr && ! cancelFlag)
        {
            setStatus ("Resampling to host rate", 0.97f);
            take->hash = req->hash;
            take->hostRate = req->hostRate;
            take->hostAudio = resampleMono (take->engineAudio, kEngineSampleRate, req->hostRate);
            writeWav (cacheFileFor (req->hash), take->engineAudio, kEngineSampleRate, 24);

            // keep the cache folder bounded (12 most recent takes)
            auto files = Paths::cacheDir().findChildFiles (juce::File::findFiles, false, "*.wav");
            if (files.size() > 12)
            {
                std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
                           { return a.getLastModificationTime() < b.getLastModificationTime(); });
                for (int i = 0; i < files.size() - 12; ++i) files.getReference (i).deleteFile();
            }

            {
                const juce::ScopedLock sl (lock);
                finished = take;
            }
            setStatus ("Rendered: " + take->summary, 1.0f);
        }
        else if (cancelFlag)
        {
            bool hasPending;
            { const juce::ScopedLock sl (lock); hasPending = pending != nullptr; }
            if (! hasPending) setStatus ("Render cancelled", 0.0f);
            else wake.signal();
        }
        busy = false;
    }
}

//==============================================================================
std::vector<float> RenderEngine::framesF0FromCurves (const ControlCurves& c, const Timeline& tl, int frames) const
{
    std::vector<float> f0 ((size_t) frames, 0.0f);
    size_t seg = 0;
    for (int i = 0; i < frames; ++i)
    {
        const double t = (i + 0.5) / NeuralBackend::kFrameRate;
        while (seg < tl.segments.size() && tl.segments[seg].t1 <= t) ++seg;
        bool voiced = false;
        if (seg < tl.segments.size() && tl.segments[seg].t0 <= t)
        {
            const auto& info = phonemeAt (tl.segments[seg].ph);
            voiced = info.voiced && info.cls != PhClass::Plosive;
        }
        const int k = juce::jlimit (0, juce::jmax (0, c.numFrames() - 1), (int) (t * c.rate));
        if (voiced && c.numFrames() > 0 && c.amp[(size_t) k] > 0.01f) f0[(size_t) i] = c.f0Hz[(size_t) k];
    }
    return f0;
}

std::shared_ptr<RenderedTake> RenderEngine::renderSynth (const RenderRequest& r)
{
    auto take = std::make_shared<RenderedTake>();
    if (r.project.notes.empty())
    {
        take->engineAudio.setSize (1, (int) kEngineSampleRate);
        take->engineAudio.clear();
        take->summary = "empty project";
        return take;
    }

    setStatus ("Aligning lyrics to notes", 0.02f);
    auto tl = buildTimeline (r.project);
    const double dur = tl.endTime + 0.6;
    auto curves = buildCurves (r.project, r.settings, dur);

    const bool neural = prepareNeural (r.voice);
    setStatus (neural ? "Synthesising guide vocal" : "Synthesising vocal", 0.05f);
    auto guide = SynthRenderer::render (tl, curves, r.voice.timbre, r.settings, &cancelFlag,
                                        [this, neural] (float p) { prog = 0.05f + p * (neural ? 0.25f : 0.8f); });
    if (cancelFlag) return nullptr;

    if (neural)
    {
        setStatus ("Neural voice: extracting content", 0.32f);
        auto g16 = resampleMono (guide, kEngineSampleRate, NeuralBackend::kContentRate);
        std::vector<float> units; int frames = 0, dim = 0;
        if (backend.extractUnits (g16.getReadPointer (0), g16.getNumSamples(), units, frames, dim, &cancelFlag))
        {
            auto f0 = framesF0FromCurves (curves, tl, frames);
            setStatus ("Neural voice: rendering " + r.voice.name, 0.45f);
            std::vector<float> audio;
            if (backend.synthesise (units, frames, dim, f0, audio, &cancelFlag,
                                    [this] (float p) { prog = 0.45f + 0.5f * p; }))
            {
                take->engineAudio = toEngineRate (audio, backend.outputRate());
                const float pk = take->engineAudio.getMagnitude (0, 0, take->engineAudio.getNumSamples());
                if (pk > 0.98f) take->engineAudio.applyGain (0.95f / pk);
                take->neural = true;
                take->summary = "neural voice '" + r.voice.name + "', " + juce::String (r.project.notes.size()) + " notes";
                return take;
            }
        }
        if (cancelFlag) return nullptr;
        setStatus ("Neural render failed - using DSP engine", 0.9f);
    }

    if (! r.voice.timbre.correctionDb.empty())
        applySpectralCorrection (guide, kEngineSampleRate, r.voice.timbre.correctionDb);
    take->engineAudio = std::move (guide);
    take->summary = "DSP voice '" + r.voice.name + "', " + juce::String (r.project.notes.size()) + " notes";
    return take;
}

//==============================================================================
std::shared_ptr<RenderedTake> RenderEngine::renderReplace (const RenderRequest& r)
{
    const auto& prj = r.project;
    const juce::File stemFile (prj.stemPath);
    const bool neural = prepareNeural (r.voice);

    if (! neural || ! stemFile.existsAsFile())
    {
        // DSP fallback: re-sing the edited melody + lyrics with the built-in engine
        auto take = renderSynth (r);
        if (take != nullptr)
        {
            take->replacement = true;
            take->summary = "replacement (DSP engine, no neural voice): " + take->summary;
        }
        return take;
    }

    // ---- load + analyse the stem (cached) ------------------------------------
    const auto key = stemFile.getFullPathName() + "|" + juce::String (stemFile.getLastModificationTime().toMilliseconds());
    if (key != cachedStemKey)
    {
        setStatus ("Loading stem", 0.02f);
        juce::AudioBuffer<float> mono; double sr = 0;
        if (! loadAudioMono (stemFile, mono, sr)) { setStatus ("Cannot read stem", 0.0f); return nullptr; }
        stem16k = resampleMono (mono, sr, NeuralBackend::kContentRate);
        setStatus ("Extracting phonetic content from stem", 0.08f);
        if (! backend.extractUnits (stem16k.getReadPointer (0), stem16k.getNumSamples(), stemUnits, stemFrames, stemDim, &cancelFlag))
            return nullptr;
        setStatus ("Tracking stem pitch", 0.25f);
        stemF0 = detectPitch (stem16k, NeuralBackend::kContentRate, {}, &cancelFlag);
        cachedStemKey = key;
    }
    if (cancelFlag) return nullptr;

    const double offsetSec = prj.beatsToSeconds (prj.stemOffsetBeats);

    // ---- warp map: output (project) time -> stem time ---------------------------
    struct Anchor { double o, s; };
    std::vector<Anchor> anchors;
    std::vector<bool> synthMask (prj.notes.size(), false);
    bool anySynth = false;
    for (size_t i = 0; i < prj.notes.size(); ++i)
    {
        const auto& n = prj.notes[i];
        const bool useSource = n.fromSource && n.srcEndSec > n.srcStartSec && ! n.lyricChangedFromSource();
        if (useSource)
        {
            anchors.push_back ({ prj.beatsToSeconds (n.startBeat), n.srcStartSec });
            anchors.push_back ({ prj.beatsToSeconds (n.endBeat()), n.srcEndSec });
        }
        else { synthMask[i] = true; anySynth = true; }
    }
    std::sort (anchors.begin(), anchors.end(), [] (const Anchor& a, const Anchor& b) { return a.o < b.o; });
    std::vector<Anchor> mono;
    for (auto& a : anchors)
        if (mono.empty() || (a.o > mono.back().o + 1.0e-4 && a.s > mono.back().s + 1.0e-4)) mono.push_back (a);

    auto warp = [&] (double t) -> double
    {
        if (mono.empty()) return t - offsetSec;
        if (t <= mono.front().o) return mono.front().s - (mono.front().o - t);
        if (t >= mono.back().o)  return mono.back().s + (t - mono.back().o);
        auto it = std::lower_bound (mono.begin(), mono.end(), t, [] (const Anchor& a, double v) { return a.o < v; });
        const auto& b = *it; const auto& a = *(it - 1);
        return a.s + (t - a.o) * (b.s - a.s) / juce::jmax (1.0e-6, b.o - a.o);
    };

    // ---- curves ------------------------------------------------------------------
    double dur = prj.beatsToSeconds (prj.lengthInBeats()) + 0.8;
    dur = juce::jmax (dur, offsetSec + stem16k.getNumSamples() / NeuralBackend::kContentRate);
    const int frames = (int) std::ceil (dur * NeuralBackend::kFrameRate);

    // per-note source pitch centre for the nuance re-application
    std::vector<float> srcCentre (prj.notes.size(), 0.0f);
    for (size_t i = 0; i < prj.notes.size(); ++i)
    {
        const auto& n = prj.notes[i];
        if (! n.fromSource) continue;
        std::vector<float> v;
        for (double t = n.srcStartSec; t < n.srcEndSec; t += 0.01)
            if (auto hz = stemF0.hzAt (t); hz > 0.0f) v.push_back (hzToMidi (hz));
        if (! v.empty()) { std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end()); srcCentre[i] = v[v.size() / 2]; }
    }
    auto noteAt = [&] (double t) -> int
    {
        for (size_t i = 0; i < prj.notes.size(); ++i)
        {
            const double a = prj.beatsToSeconds (prj.notes[i].startBeat) - 0.05;
            const double b = prj.beatsToSeconds (prj.notes[i].endBeat()) + 0.02;
            if (t >= a && t < b) return (int) i;
        }
        return -1;
    };

    ExpressionSettings srcSettings = r.settings;
    srcSettings.vibratoDepthCents *= (1.0f - r.settings.nuance);   // original vibrato comes from the nuance
    srcSettings.humanize *= (1.0f - r.settings.nuance);
    auto nuanceFn = [&] (double t) -> float
    {
        const int ni = noteAt (t);
        if (ni < 0 || synthMask[(size_t) ni] || srcCentre[(size_t) ni] <= 0.0f) return 0.0f;
        const float hz = stemF0.hzAt (warp (t));
        if (hz <= 0.0f) return 0.0f;
        const float dev = juce::jlimit (-1.5f, 1.5f, hzToMidi (hz) - srcCentre[(size_t) ni]);
        return dev * 100.0f * r.settings.nuance;
    };
    setStatus ("Building pitch curves", 0.3f);
    auto srcCurves = buildCurves (prj, srcSettings, dur, nullptr, nuanceFn);
    if (cancelFlag) return nullptr;

    // ---- synthetic guide for new / re-worded notes ----------------------------------
    std::vector<float> synthUnits; int synthFrames = 0, synthDim = 0;
    Timeline synthTl;
    ControlCurves synthCurves;
    if (anySynth)
    {
        setStatus ("Synthesising new words", 0.35f);
        synthTl = buildTimeline (prj, &synthMask);
        synthCurves = buildCurves (prj, r.settings, dur, &synthMask);
        auto guide = SynthRenderer::render (synthTl, synthCurves, r.voice.timbre, r.settings, &cancelFlag);
        if (cancelFlag) return nullptr;
        auto g16 = resampleMono (guide, kEngineSampleRate, NeuralBackend::kContentRate);
        backend.extractUnits (g16.getReadPointer (0), g16.getNumSamples(), synthUnits, synthFrames, synthDim, &cancelFlag);
    }

    // ---- assemble units + f0 per output frame ---------------------------------------------
    setStatus ("Re-timing performance", 0.45f);
    const int dim = stemDim;
    std::vector<float> units ((size_t) frames * dim, 0.0f), f0 ((size_t) frames, 0.0f), srcGain ((size_t) frames, 0.0f);
    std::vector<float> synthWeight ((size_t) frames, 0.0f);

    for (int i = 0; i < frames; ++i)
    {
        const double t = (i + 0.5) / NeuralBackend::kFrameRate;
        const int ni = noteAt (t);
        synthWeight[(size_t) i] = (ni >= 0 && synthMask[(size_t) ni]) ? 1.0f : 0.0f;
    }
    // soften region borders (3 frames)
    {
        auto w = synthWeight;
        for (int i = 0; i < frames; ++i)
        {
            float s = 0.0f; int c = 0;
            for (int k = juce::jmax (0, i - 3); k <= juce::jmin (frames - 1, i + 3); ++k) { s += w[(size_t) k]; ++c; }
            synthWeight[(size_t) i] = s / (float) c;
        }
    }

    for (int i = 0; i < frames; ++i)
    {
        const double t = (i + 0.5) / NeuralBackend::kFrameRate;
        const float sw = synthWeight[(size_t) i];
        float* dst = units.data() + (size_t) i * dim;

        // source content (time-warped, linear interpolation between frames)
        const double st = warp (t);
        const double sf = st * NeuralBackend::kFrameRate - 0.5;
        const bool inStem = sf >= 0.0 && sf < stemFrames - 1;
        if (inStem && sw < 1.0f)
        {
            const int a = (int) std::floor (sf);
            const float fr = (float) (sf - a);
            const float* ua = stemUnits.data() + (size_t) a * dim;
            const float* ub = ua + dim;
            for (int d = 0; d < dim; ++d) dst[d] = (1.0f - sw) * (ua[d] + (ub[d] - ua[d]) * fr);
        }
        if (sw > 0.0f && synthFrames > 0 && synthDim == dim)
        {
            const int k = juce::jlimit (0, synthFrames - 1, i);
            const float* us = synthUnits.data() + (size_t) k * dim;
            for (int d = 0; d < dim; ++d) dst[d] += sw * us[d];
        }

        // f0
        const int ck = juce::jlimit (0, srcCurves.numFrames() - 1, (int) (t * srcCurves.rate));
        float srcHz = inStem ? stemF0.hzAt (st) : 0.0f;
        float out = 0.0f;
        if (sw < 0.5f)
        {
            if (srcHz > 0.0f)
            {
                // inside a note: edited melody; outside notes: original pitch
                out = noteAt (t) >= 0 ? srcCurves.f0Hz[(size_t) ck] : srcHz;
            }
        }
        else if (synthCurves.numFrames() > 0)
        {
            const int k = juce::jlimit (0, synthCurves.numFrames() - 1, (int) (t * synthCurves.rate));
            out = synthCurves.amp[(size_t) k] > 0.01f ? synthCurves.f0Hz[(size_t) k] : 0.0f;
        }
        f0[(size_t) i] = out;

        // source loudness for volume follow
        if (inStem)
        {
            const int s0 = (int) (st * NeuralBackend::kContentRate);
            double e = 0.0; int c = 0;
            for (int s = juce::jmax (0, s0 - 160); s < juce::jmin (stem16k.getNumSamples(), s0 + 160); ++s)
            { const float v = stem16k.getSample (0, s); e += v * v; ++c; }
            srcGain[(size_t) i] = c > 0 ? (float) std::sqrt (e / c) : 0.0f;
        }
    }

    // unvoice synthetic consonants exactly as the synth timeline says
    if (anySynth)
    {
        auto synthF0 = framesF0FromCurves (synthCurves, synthTl, frames);
        for (int i = 0; i < frames; ++i)
            if (synthWeight[(size_t) i] >= 0.5f) f0[(size_t) i] = synthF0[(size_t) i];
    }
    if (cancelFlag) return nullptr;

    setStatus ("Neural voice: rendering " + r.voice.name, 0.5f);
    std::vector<float> audio;
    if (! backend.synthesise (units, frames, dim, f0, audio, &cancelFlag, [this] (float p) { prog = 0.5f + 0.45f * p; }))
        return nullptr;

    // ---- volume follow (source regions keep the original dynamics) ----------------------
    {
        const int hop = backend.outputHop();
        float prevG = 1.0f;
        for (int i = 0; i < frames; ++i)
        {
            double e = 0.0;
            for (int s = i * hop; s < (i + 1) * hop && s < (int) audio.size(); ++s) e += audio[(size_t) s] * audio[(size_t) s];
            const float outRms = (float) std::sqrt (e / hop);
            float g = 1.0f;
            if (synthWeight[(size_t) i] < 0.5f)
            {
                g = outRms > 1.0e-5f ? std::sqrt (juce::jlimit (0.0f, 16.0f, srcGain[(size_t) i] / outRms)) : 1.0f;
                g = juce::jlimit (0.3f, 2.5f, g);
                if (srcGain[(size_t) i] < 1.0e-4f) g = 0.3f;
            }
            for (int s = i * hop; s < (i + 1) * hop && s < (int) audio.size(); ++s)
            {
                const float k = (float) (s - i * hop) / (float) hop;
                audio[(size_t) s] *= prevG + (g - prevG) * k;
            }
            prevG = g;
        }
    }

    auto take = std::make_shared<RenderedTake>();
    take->engineAudio = toEngineRate (audio, backend.outputRate());
    const float pk = take->engineAudio.getMagnitude (0, 0, take->engineAudio.getNumSamples());
    if (pk > 0.98f) take->engineAudio.applyGain (0.95f / pk);
    take->neural = true;
    take->replacement = true;
    take->summary = "vocal replaced with '" + r.voice.name + "'" + (anySynth ? " (incl. new words)" : "");
    return take;
}

} // namespace vf
