// Self-test: synthesises drum material, runs the analyser and the processor, and
// optionally renders the editor to a PNG (pass an output path as the first argument).
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <iostream>

using namespace dkc;

static int failures = 0;
#define CHECK(cond, msg) do { if (! (cond)) { ++failures; std::cout << "  FAIL: " << msg << "\n"; } } while (false)

struct Synth
{
    double sr = 44100.0;
    juce::Random rng { 42 };
    juce::AudioBuffer<float> buffer;

    explicit Synth (double seconds) { buffer.setSize (2, (int) (seconds * sr)); buffer.clear(); }

    void add (int at, int len, const std::function<float (int)>& fn, float gain)
    {
        for (int i = 0; i < len && at + i < buffer.getNumSamples(); ++i)
        {
            const float v = fn (i) * gain;
            buffer.addSample (0, at + i, v);
            buffer.addSample (1, at + i, v);
        }
    }

    void kick (double t, float g = 0.9f)
    {
        double phase = 0.0;
        add ((int) (t * sr), (int) (0.35 * sr), [&] (int i)
        {
            const double s = i / sr;
            phase += juce::MathConstants<double>::twoPi * (48.0 + 110.0 * std::exp (-s * 35.0)) / sr;
            return (float) (std::sin (phase) * std::exp (-s * 11.0));
        }, g);
    }

    void snare (double t, float g = 0.7f)
    {
        float lp = 0.0f;
        add ((int) (t * sr), (int) (0.22 * sr), [&] (int i)
        {
            const double s = i / sr;
            const float noise = rng.nextFloat() * 2.0f - 1.0f;
            lp += 0.35f * (noise - lp);
            return (float) (0.7 * (noise - 0.5f * lp) * std::exp (-s * 24.0)
                            + 0.6 * std::sin (juce::MathConstants<double>::twoPi * 195.0 * s) * std::exp (-s * 30.0));
        }, g);
    }

    void hat (double t, float g = 0.4f)
    {
        float prev = 0.0f;
        add ((int) (t * sr), (int) (0.06 * sr), [&] (int i)
        {
            const float noise = rng.nextFloat() * 2.0f - 1.0f;
            const float hp = noise - prev;      // crude high-pass
            prev = noise;
            return (float) (hp * std::exp (-(i / sr) * 90.0));
        }, g);
    }

    void bass (double t, double len, double freq, float g)
    {
        add ((int) (t * sr), (int) (len * sr), [&] (int i)
        {
            const double s = i / sr;
            const double env = juce::jmin (1.0, s / 0.08) * juce::jmin (1.0, (len - s) / 0.08);
            return (float) (env * (std::sin (juce::MathConstants<double>::twoPi * freq * s)
                                   + 0.4 * std::sin (juce::MathConstants<double>::twoPi * freq * 3.0 * s)));
        }, g);
    }
};

static void describe (const Kit& kit)
{
    std::cout << "  " << kit.totalHits << " hits -> " << kit.pads.size() << " pads:";
    for (auto& p : kit.pads)
        std::cout << "  [" << p.name << " x" << p.hitCount << ", " << juce::String (p.audio.getNumSamples() / p.sampleRate, 3) << "s]";
    std::cout << "\n";
}

static int countType (const Kit& kit, int type)
{
    int n = 0;
    for (auto& p : kit.pads) n += p.type == type ? 1 : 0;
    return n;
}

static int hitsOfType (const Kit& kit, int type)
{
    int n = 0;
    for (auto& p : kit.pads) n += p.type == type ? p.hitCount : 0;
    return n;
}

static Synth makeLoop (int bars, bool withBass)
{
    const double beat = 0.5;    // 120 bpm
    Synth s (bars * 4 * beat + 0.5);
    juce::Random level (7);
    for (int bar = 0; bar < bars; ++bar)
    {
        const double t0 = bar * 4 * beat;
        // kick on 1 and the "and" of 2... kept clear of snares/hats so the expected counts are exact
        s.kick (t0 + 0.0 * beat, 0.8f + 0.1f * level.nextFloat());
        s.kick (t0 + 2.5 * beat, 0.8f + 0.1f * level.nextFloat());
        s.snare (t0 + 1.0 * beat, 0.6f + 0.1f * level.nextFloat());
        s.snare (t0 + 3.0 * beat, 0.6f + 0.1f * level.nextFloat());
        for (double h : { 0.5, 1.5, 2.0, 3.5 })
            s.hat (t0 + h * beat, 0.3f + 0.1f * level.nextFloat());
        if (withBass)
        {
            static const double notes[] = { 55.0, 65.4, 73.4, 82.4, 49.0, 61.7 };
            // legato notes that change between the drum hits, a different one under each hit
            for (int n = 0; n < 4; ++n)
                s.bass (t0 + n * beat + 0.3 * beat, beat * 1.16, notes[(bar * 4 + n * 5 + bar * n) % 6] * 2.0, 0.1f);
        }
    }
    return s;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    AnalysisSettings settings;
    const auto noProgress = [] (float) { return true; };

    std::cout << "1. clean loop (4 bars: 8 kicks, 8 snares, 16 hats)\n";
    {
        auto s = makeLoop (4, false);
        auto kit = KitAnalyzer::analyse (s.buffer, s.sr, settings, noProgress);
        describe (*kit);
        CHECK (kit->totalHits == 32, "expected 32 hits, got " << kit->totalHits);
        CHECK (kit->pads.size() == 3, "expected 3 pads");
        CHECK (hitsOfType (*kit, Kick) == 8, "expected 8 kick hits, got " << hitsOfType (*kit, Kick));
        CHECK (hitsOfType (*kit, Snare) == 8, "expected 8 snare hits, got " << hitsOfType (*kit, Snare));
        CHECK (hitsOfType (*kit, Hat) == 16, "expected 16 hat hits, got " << hitsOfType (*kit, Hat));
        for (auto& p : kit->pads)
            CHECK (std::abs (p.audio.getMagnitude (0, p.audio.getNumSamples()) - 0.89f) < 0.01f, p.name << " not normalised");
    }

    std::cout << "2. loop with a bass line underneath (8 bars)\n";
    {
        auto s = makeLoop (8, true);
        auto kit = KitAnalyzer::analyse (s.buffer, s.sr, settings, noProgress);
        describe (*kit);
        CHECK (countType (*kit, Kick) >= 1 && countType (*kit, Snare) >= 1 && countType (*kit, Hat) >= 1, "kick/snare/hat should all be found");
        CHECK (hitsOfType (*kit, Kick) >= 14, "most kicks should be grouped, got " << hitsOfType (*kit, Kick));
        CHECK (hitsOfType (*kit, Hat) >= 28, "most hats should be grouped, got " << hitsOfType (*kit, Hat));
        CHECK (kit->pads.size() <= 8, "too many pads: " << kit->pads.size());

        // isolation: the hat one-shot should have lost the bass that played under it
        for (auto& p : kit->pads)
            if (p.type == Hat)
            {
                juce::AudioBuffer<float> copy (p.audio);
                juce::IIRFilter lp;
                lp.setCoefficients (juce::IIRCoefficients::makeLowPass (p.sampleRate, 400.0));
                lp.processSamples (copy.getWritePointer (0), copy.getNumSamples());
                const float lowDb = juce::Decibels::gainToDecibels (copy.getRMSLevel (0, 0, copy.getNumSamples()) / p.audio.getRMSLevel (0, 0, p.audio.getNumSamples()));
                std::cout << "  " << p.name << ": energy below 400 Hz " << juce::String (lowDb, 1) << " dB re. total\n";
                CHECK (lowDb < -18.0f, "bass leaked into " << p.name);
            }
    }

    std::cout << "3. single one-shot\n";
    {
        Synth s (1.0);
        s.snare (0.05);
        auto kit = KitAnalyzer::analyse (s.buffer, s.sr, settings, noProgress);
        describe (*kit);
        CHECK (kit->pads.size() == 1 && kit->totalHits == 1, "expected exactly one pad from one hit");
    }

    std::cout << "4. silence / tiny input\n";
    {
        juce::AudioBuffer<float> silent (2, 44100);
        silent.clear();
        auto kit = KitAnalyzer::analyse (silent, 44100.0, settings, noProgress);
        CHECK (kit != nullptr && kit->pads.empty(), "silence should give an empty kit");
        juce::AudioBuffer<float> tiny (1, 10);
        tiny.clear();
        CHECK (KitAnalyzer::analyse (tiny, 44100.0, settings, noProgress) != nullptr, "tiny input should not fail");
    }

    std::cout << "5. long input (3 minutes, 96 kHz) + cancel\n";
    {
        Synth s (180.0);
        s.sr = 96000.0;
        s.buffer.setSize (2, (int) (180.0 * 96000.0));
        s.buffer.clear();
        for (double t = 0.0; t < 179.0; t += 0.25)
        {
            const int step = (int) std::lround (t * 4.0) % 8;
            if (step == 0 || step == 5) s.kick (t); else if (step == 2 || step == 6) s.snare (t); else s.hat (t);
        }
        const auto startTime = juce::Time::getMillisecondCounterHiRes();
        auto kit = KitAnalyzer::analyse (s.buffer, s.sr, settings, noProgress);
        std::cout << "  analysed in " << juce::String ((juce::Time::getMillisecondCounterHiRes() - startTime) / 1000.0, 2) << " s\n";
        describe (*kit);
        CHECK (std::abs (kit->totalHits - 716) <= 4, "expected ~716 hits, got " << kit->totalHits);
        CHECK (kit->pads.size() >= 3 && kit->pads.size() <= 5, "expected 3-5 pads");
        CHECK (KitAnalyzer::analyse (s.buffer, s.sr, settings, [] (float p) { return p < 0.3f; }) == nullptr, "cancel should return null");
    }

    std::cout << "6. processor: file load, MIDI playback, state round-trip\n";
    {
        auto s = makeLoop (4, true);
        auto wavFile = juce::File::createTempFile (".wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (wavFile), s.sr, 2, 16, {}, 0));
            w->writeFromAudioSampleBuffer (s.buffer, 0, s.buffer.getNumSamples());
        }

        DrumkitCloneProcessor proc;
        proc.setPlayConfigDetails (0, 2, 48000.0, 256);
        proc.prepareToPlay (48000.0, 256);
        proc.loadFile (wavFile);
        for (int i = 0; i < 600 && (proc.isAnalysing() || proc.getKit() == nullptr); ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        auto kit = proc.getKit();
        CHECK (kit != nullptr && kit->pads.size() >= 3, "processor should have a kit after loading");
        if (kit != nullptr)
        {
            describe (*kit);
            auto renderNote = [&] (DrumkitCloneProcessor& p, int note)
            {
                juce::AudioBuffer<float> out (2, 256);
                juce::MidiBuffer midi;
                midi.addEvent (juce::MidiMessage::noteOn (10, note, (juce::uint8) 110), 17);
                p.processBlock (out, midi);
                for (int i = 0; i < 17; ++i)
                    CHECK (out.getSample (0, i) == 0.0f, "audio before the note-on offset");
                float peak = out.getMagnitude (0, 256);
                for (int b = 0; b < 300; ++b)
                {
                    midi.clear();
                    p.processBlock (out, midi);
                    peak = juce::jmax (peak, out.getMagnitude (0, 256));
                }
                return peak;
            };

            for (int i = 0; i < (int) kit->pads.size(); ++i)
                CHECK (renderNote (proc, 36 + i) > 0.1f, "pad " << i + 1 << " produced no sound");
            CHECK (renderNote (proc, 36 + (int) kit->pads.size()) == 0.0f, "empty pad should be silent");
            CHECK (renderNote (proc, 20) == 0.0f, "out-of-range note should be silent");

            proc.apvts.getParameter ("tune2")->setValueNotifyingHost (0.75f);
            juce::MemoryBlock state;
            proc.getStateInformation (state);
            std::cout << "  state size: " << state.getSize() / 1024 << " KiB\n";

            DrumkitCloneProcessor other;
            other.setPlayConfigDetails (0, 2, 44100.0, 256);
            other.prepareToPlay (44100.0, 256);
            other.setStateInformation (state.getData(), (int) state.getSize());
            auto restored = other.getKit();
            CHECK (restored != nullptr && restored->pads.size() == kit->pads.size(), "kit should survive a state round-trip");
            CHECK (restored != nullptr && restored->hits.size() == kit->hits.size(), "hit markers should survive");
            if (restored != nullptr && restored->pads.size() == kit->pads.size())
                for (size_t i = 0; i < kit->pads.size(); ++i)
                {
                    const auto& a = kit->pads[i].audio; const auto& b = restored->pads[i].audio;
                    CHECK (a.getNumSamples() == b.getNumSamples() && restored->pads[i].name == kit->pads[i].name, "pad " << i << " differs");
                    float maxDiff = 0.0f;
                    for (int n = 0; n < juce::jmin (a.getNumSamples(), b.getNumSamples()); ++n)
                        maxDiff = juce::jmax (maxDiff, std::abs (a.getSample (0, n) - b.getSample (0, n)));
                    CHECK (maxDiff < 1.0e-5f, "pad " << i << " audio changed by " << maxDiff);
                }
            CHECK (std::abs (other.apvts.getParameter ("tune2")->getValue() - 0.75f) < 1.0e-4f, "parameter should be restored");
            CHECK (renderNote (other, 36) > 0.1f, "restored kit should play");

            auto folder = juce::File::createTempFile ("kit");
            juce::String error;
            CHECK (other.exportKit (folder, error), "export failed: " << error);
            CHECK (folder.getNumberOfChildFiles (juce::File::findFiles, "*.wav") == (int) kit->pads.size(), "export should write one WAV per pad");
            folder.deleteRecursively();

            if (argc > 1)
            {
                std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
                static_cast<DrumkitCloneEditor*> (editor.get())->padPressed (1);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
                auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
                juce::File png (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
                png.deleteFile();
                juce::FileOutputStream os (png);
                juce::PNGImageFormat().writeImageToStream (image, os);
                std::cout << "  editor snapshot: " << png.getFullPathName() << "\n";
            }
        }
        wavFile.deleteFile();
    }

    std::cout << (failures == 0 ? "ALL PASSED" : "FAILED") << " (" << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
