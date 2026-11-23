// Headless tests for the VoxForge engine (no plug-in host needed).
//   VoxForgeTests [outputDir]
// Renders test vocals to WAV, runs Vocal-to-MIDI on them, checks the voice-clone analysis.

#include <JuceHeader.h>
#include "../Source/Core/Lyrics.h"
#include "../Source/Core/Timeline.h"
#include "../Source/DSP/FormantSynth.h"
#include "../Source/DSP/AudioUtils.h"
#include "../Source/DSP/VocalToMidi.h"
#include "../Source/DSP/Spectral.h"
#include "../Source/Engine/VoiceAnalyzer.h"
#include "../Source/Engine/VoicePreset.h"
#include "../Source/Engine/StemAnalyzer.h"
#include "../Source/Engine/NeuralBackend.h"

using namespace vf;

static int failures = 0;
#define CHECK(cond, msg) do { if (! (cond)) { ++failures; std::cout << "  FAIL: " << msg << "\n"; } else std::cout << "  ok:   " << msg << "\n"; } while (0)

static VocalProject melody (const juce::String& lyrics, std::initializer_list<int> pitches, double len = 0.9)
{
    VocalProject p;
    p.bpm = 100.0;
    double b = 0.5;
    for (int pitch : pitches)
    {
        Note n; n.startBeat = b; n.lengthBeats = len; n.pitch = pitch; n.velocity = 0.8f;
        p.notes.push_back (n);
        b += 1.0;
    }
    p.lyricsText = lyrics;
    applyLyricsToNotes (p.notes, lyrics);
    return p;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::File out = argc > 1 ? juce::File (argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile ("test-output");
    out.createDirectory();

    std::cout << "[1] Lyrics / G2P\n";
    {
        auto syl = lyricsToSyllables ("hel-lo beautiful world, twinkle little star - -");
        juce::StringArray shown;
        for (auto& s : syl) shown.add (s.text + "{" + s.phonemes.joinIntoString (" ") + "}");
        std::cout << "  " << shown.joinIntoString (" ") << "\n";
        CHECK (syl.size() == 13, "syllable count (hel lo beau ti ful world twin kle lit tle star - -) = " + juce::String ((int) syl.size()));
        CHECK (syl[0].phonemes.joinIntoString (" ") == "hh ax", "hello split -> 'hh ax' | 'l ow'");
        CHECK (syl.back().isHold, "'-' is a hold");
        auto ph = Lexicon::get().lookup ("singing");
        CHECK (ph.contains ("ng"), "letter-to-sound: singing -> " + ph.joinIntoString (" "));
        auto parts = splitWordForDisplay ("happy", 2);
        CHECK (parts[0] == "hap" && parts[1] == "py", "display split happy -> " + parts.joinIntoString ("|"));
    }

    std::cout << "[2] Timeline + synthesis\n";
    auto prj = melody ("twin-kle twin-kle lit-tle star how i won-der what you are", { 60, 60, 67, 67, 69, 69, 67, 65, 65, 64, 64, 62, 62, 60 });
    ExpressionSettings ex;
    auto tl = buildTimeline (prj);
    CHECK (! tl.segments.empty(), "timeline has " + juce::String ((int) tl.segments.size()) + " phoneme segments");
    bool onsetBeforeBeat = false;
    for (auto& s : tl.segments)
        if (! s.nucleus && s.note == 0) onsetBeforeBeat |= s.t0 < prj.beatsToSeconds (prj.notes[0].startBeat);
    CHECK (onsetBeforeBeat, "onset consonant of 'twin' starts before the beat");

    auto curves = buildCurves (prj, ex, tl.endTime + 0.5);
    VoiceTimbre timbre;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto audio = SynthRenderer::render (tl, curves, timbre, ex);
    const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
    const double secs = audio.getNumSamples() / kEngineSampleRate;
    std::cout << "  rendered " << secs << " s in " << ms << " ms (" << secs * 1000.0 / ms << "x real-time)\n";
    bool finite = true;
    for (int i = 0; i < audio.getNumSamples(); ++i) finite &= std::isfinite (audio.getSample (0, i));
    CHECK (finite, "no NaN / inf in output");
    CHECK (audio.getRMSLevel (0, 0, audio.getNumSamples()) > 0.02f, "output has signal");
    writeWav (out.getChildFile ("twinkle_dsp.wav"), audio, kEngineSampleRate);

    std::cout << "[3] Sample-rate support 44.1 - 192 kHz\n";
    for (double sr : { 44100.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        auto r = resampleMono (audio, kEngineSampleRate, sr);
        const double expect = audio.getNumSamples() * sr / kEngineSampleRate;
        CHECK (std::abs (r.getNumSamples() - expect) < 2.0, "resample to " + juce::String (sr, 0) + " Hz");
    }
    {
        LiveVoice lv;
        lv.prepare (192000.0, 512);
        lv.setSyllables ({ juce::StringArray { "l", "aa" } });
        juce::AudioBuffer<float> buf (2, 512);
        buf.clear();
        lv.render (buf, 0, 512, 1.0f);   // applies the syllables
        lv.noteOn (64, 0.8f);
        float peak = 0.0f;
        for (int b = 0; b < 200; ++b) { buf.clear(); lv.render (buf, 0, 512, 1.0f); peak = juce::jmax (peak, buf.getMagnitude (0, 512)); }
        CHECK (peak > 0.01f && std::isfinite (peak), "live voice runs at 192 kHz");
    }

    std::cout << "[4] Vocal-to-MIDI on the rendered vocal\n";
    {
        auto an = vocalToMidi (audio, kEngineSampleRate);
        int match = 0;
        for (auto& n : prj.notes)
        {
            const double a = prj.beatsToSeconds (n.startBeat), b = prj.beatsToSeconds (n.endBeat());
            for (auto& d : an.notes)
            {
                const double ov = juce::jmin (b, d.t1) - juce::jmax (a, d.t0);
                if (ov > 0.5 * (b - a) && d.pitch == n.pitch) { ++match; break; }
            }
        }
        if (std::getenv ("VF_DEBUG"))
        {
            for (auto& n : prj.notes) std::cout << "    truth " << prj.beatsToSeconds (n.startBeat) << "-" << prj.beatsToSeconds (n.endBeat()) << " p" << n.pitch << " " << n.lyric << "\n";
            for (auto& d : an.notes) std::cout << "    det   " << d.t0 << "-" << d.t1 << " p" << d.pitch << " (" << d.meanMidi << ")\n";
        }
        std::cout << "  detected " << an.notes.size() << " notes, " << match << "/" << prj.notes.size() << " match pitch+time\n";
        CHECK (match >= (int) prj.notes.size() * 8 / 10, "Vocal-to-MIDI accuracy >= 80 %");
    }

    std::cout << "[5] Lite voice clone analysis (recovers a known vocal-tract scale)\n";
    {
        auto big = melody ("ah oh ee oo eh ah oh ee oo eh ah oh ee oo eh ah oh ee oo eh",
                           { 64, 66, 68, 69, 71, 73, 71, 69, 68, 66, 64, 66, 68, 69, 71, 73, 71, 69, 68, 66 }, 0.95);
        auto btl = buildTimeline (big);
        auto bc = buildCurves (big, ex, btl.endTime + 0.5);
        VoiceTimbre target;
        target.formantScale = 1.14f;
        auto sample = SynthRenderer::render (btl, bc, target, ex);
        auto f = out.getChildFile ("clone_sample.wav");
        writeWav (f, sample, kEngineSampleRate);
        auto res = analyseVoiceSamples ({ f });
        CHECK (res.ok, "analysis ok");
        std::cout << "  estimated formant scale " << res.timbre.formantScale << " (true 1.14), median pitch "
                  << res.stats.medianMidi << ", vibrato " << res.stats.vibratoRate << " Hz / " << res.stats.vibratoDepth << " c\n";
        CHECK (std::abs (res.timbre.formantScale - 1.14f) <= 0.06f, "formant scale within 0.06");
        CHECK (res.timbre.correctionDb.size() == (size_t) SpectralBands::kNumBands, "spectral fingerprint present");

        // render with the clone, compare spectra
        auto cl = SynthRenderer::render (btl, bc, res.timbre, ex);
        applySpectralCorrection (cl, kEngineSampleRate, res.timbre.correctionDb);
        writeWav (out.getChildFile ("clone_render.wav"), cl, kEngineSampleRate);
    }

    std::cout << "[6] Spectral correction is transparent at 0 dB\n";
    {
        auto copy = audio;
        applySpectralCorrection (copy, kEngineSampleRate, std::vector<float> (SpectralBands::kNumBands, 0.0f));
        double err = 0.0, ref = 0.0;
        for (int i = 4096; i < audio.getNumSamples() - 4096; ++i)
        {
            const double d = copy.getSample (0, i) - audio.getSample (0, i);
            err += d * d; ref += (double) audio.getSample (0, i) * audio.getSample (0, i);
        }
        CHECK (err / juce::jmax (1.0e-9, ref) < 1.0e-6, "STFT round trip error " + juce::String (err / ref));
    }

    std::cout << "[7] Project state round trip\n";
    {
        auto v = prj.toValueTree();
        VocalProject p2;
        p2.fromValueTree (v);
        CHECK (p2.notes.size() == prj.notes.size() && p2.notes[3].lyric == prj.notes[3].lyric, "notes + lyrics restored");
        CHECK (p2.contentHash (ex, "a") == prj.contentHash (ex, "a"), "content hash stable");
    }

    std::cout << "[8] Vocal replacement: recognised words -> notes\n";
    {
        StemAnalysisResult r;
        r.ok = true;
        r.analysis = vocalToMidi (audio, kEngineSampleRate);
        const char* words[] = { "twinkle", "twinkle", "little", "star" };
        for (int w = 0; w < 4; ++w)
        {
            const double a = prj.beatsToSeconds (prj.notes[(size_t) w * 2].startBeat) - 0.1;
            const double b = prj.beatsToSeconds (prj.notes[(size_t) juce::jmin (w * 2 + 1, 6)].endBeat());
            r.words.push_back ({ words[w], a, w == 3 ? prj.beatsToSeconds (prj.notes[6].endBeat()) : b });
        }
        VocalProject rp;
        rp.bpm = prj.bpm;
        rp.stemPath = "stem.wav";
        notesFromAnalysis (r, rp);
        juce::StringArray ly;
        for (auto& n : rp.notes) ly.add (n.lyric);
        std::cout << "  " << ly.joinIntoString (" ") << "\n";
        CHECK (rp.notes.size() >= 7 && rp.notes[0].lyric == "twin" && rp.notes[1].lyric == "kle" && rp.notes[4].lyric == "lit",
               "syllables land on the detected notes");
        CHECK (rp.notes[0].fromSource && ! rp.notes[0].lyricChangedFromSource(), "notes keep the original articulation");
        rp.notes[0].lyric = "spar";
        CHECK (rp.notes[0].lyricChangedFromSource(), "typing a new word marks the note for re-synthesis");
        CHECK (std::abs (rp.beatsToSeconds (rp.notes[0].startBeat) - prj.beatsToSeconds (prj.notes[0].startBeat)) < 0.12,
               "detected timing maps back onto the beat grid");
    }

    // [9] Neural voice through the C++ ONNX backend (only when models are provided, e.g. in CI)
    const auto encPath = juce::SystemStats::getEnvironmentVariable ("VF_CONTENT_ENCODER", {});
    const auto voicePath = juce::SystemStats::getEnvironmentVariable ("VF_VOICE_MODEL", {});
    if (encPath.isNotEmpty() && voicePath.isNotEmpty())
    {
        std::cout << "[9] Neural voice (ONNX Runtime in the plug-in engine)\n";
        CHECK (NeuralBackend::isCompiledIn(), "neural backend compiled in");
        NeuralBackend nb;
        juce::String err;
        CHECK (nb.loadContentEncoder (juce::File (encPath), err), "content encoder loads " + err);
        CHECK (nb.loadVoice (juce::File (voicePath), err), "voice.onnx loads " + err);
        auto g16 = resampleMono (audio, kEngineSampleRate, NeuralBackend::kContentRate);
        std::vector<float> units; int frames = 0, dim = 0;
        const bool okUnits = nb.extractUnits (g16.getReadPointer (0), g16.getNumSamples(), units, frames, dim);
        CHECK (okUnits && dim == 768 && frames > 100, "content units " + juce::String (frames) + " x " + juce::String (dim));
        std::vector<float> f0 ((size_t) frames, 0.0f);
        for (int i = 0; i < frames; ++i)
        {
            const int k = juce::jlimit (0, curves.numFrames() - 1, (int) ((i + 0.5) / NeuralBackend::kFrameRate * curves.rate));
            if (curves.amp[(size_t) k] > 0.01f) f0[(size_t) i] = curves.f0Hz[(size_t) k];
        }
        std::vector<float> neural;
        const auto t1 = juce::Time::getMillisecondCounterHiRes();
        const bool okSyn = nb.synthesise (units, frames, dim, f0, neural);
        const auto synMs = juce::Time::getMillisecondCounterHiRes() - t1;
        double e = 0.0; bool fin = true;
        for (auto v : neural) { e += (double) v * v; fin &= std::isfinite (v); }
        const double rms = neural.empty() ? 0.0 : std::sqrt (e / (double) neural.size());
        std::cout << "  synthesised " << neural.size() / 48000.0 << " s in " << synMs << " ms, rms " << rms << "\n";
        CHECK (okSyn && (int) neural.size() == frames * NeuralBackend::kHop, "neural output length matches frames x 960");
        CHECK (fin && rms > 1.0e-4, "neural output is finite and not silent");
        juce::AudioBuffer<float> nbuf (1, (int) neural.size());
        if (! neural.empty()) nbuf.copyFrom (0, 0, neural.data(), (int) neural.size());
        writeWav (out.getChildFile ("twinkle_neural.wav"), nbuf, NeuralBackend::kOutputRate);
    }

    // [10] Lyrics extraction through the plug-in's StemAnalyzer + engine process
    const auto lyricsStem = juce::SystemStats::getEnvironmentVariable ("VF_LYRICS_STEM", {});
    if (lyricsStem.isNotEmpty())
    {
        std::cout << "[10] Lyrics extraction from a recording (StemAnalyzer -> voxforge-engine -> notes)\n";
        const auto expected = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("VF_LYRICS_EXPECT", {}).toLowerCase(), " ", "");
        StemAnalyzer analyzer;
        analyzer.start (juce::File (lyricsStem), {}, true, "en");
        const auto deadline = juce::Time::getMillisecondCounter() + 15u * 60u * 1000u;
        while (! analyzer.hasResult() && juce::Time::getMillisecondCounter() < deadline)
            juce::Thread::sleep (200);
        CHECK (analyzer.hasResult(), "analysis finished");
        auto r = analyzer.takeResult();
        std::cout << "  transcript: \"" << r.transcript << "\"  (" << r.words.size() << " words) " << r.transcriptionNote << "\n";
        int found = 0;
        const auto got = r.transcript.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz ");
        for (auto& w : expected) found += got.contains (w) ? 1 : 0;
        CHECK (r.ok && ! r.words.empty(), "words with timestamps returned");
        CHECK (expected.isEmpty() || found * 10 >= expected.size() * 6,
               "recognised " + juce::String (found) + "/" + juce::String (expected.size()) + " expected words");
        bool monotonic = true;
        for (size_t i = 1; i < r.words.size(); ++i) monotonic &= r.words[i].t0 >= r.words[i - 1].t0 - 0.05;
        CHECK (monotonic, "word timestamps are in order");
        VocalProject lp;
        lp.bpm = 120.0;
        lp.stemPath = lyricsStem;
        notesFromAnalysis (r, lp);
        int lyricNotes = 0;
        for (auto& n : lp.notes) lyricNotes += (n.lyric != juce::String::charToString ((juce::juce_wchar) 0x2022)) ? 1 : 0;
        std::cout << "  " << lp.notes.size() << " notes, " << lyricNotes << " with recognised syllables; lyrics: " << lp.lyricsText << "\n";
        CHECK (lyricNotes > 0 && lp.lyricsText.isNotEmpty(), "recognised words were placed on notes");
    }

    std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " FAILURE(S)") << "\n";
    std::cout << "WAV files written to " << out.getFullPathName() << "\n";
    return failures == 0 ? 0 : 1;
}
