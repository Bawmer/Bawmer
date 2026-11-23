// Loads the built VoxForge.vst3 through JUCE's VST3 hosting (like a DAW would),
// checks bus layouts, sample rates 44.1-192 kHz, MIDI -> audio, state save/restore,
// and opens the editor. Optionally writes a screenshot of each tab.
//   VoxForgeHostTest <path/to/VoxForge.vst3> [screenshot-dir]

#include <JuceHeader.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (! (cond)) { ++failures; std::cout << "  FAIL: " << msg << "\n"; } else std::cout << "  ok:   " << msg << "\n"; } while (0)

struct Playhead : juce::AudioPlayHead
{
    double sr = 48000, bpm = 120; juce::int64 pos = 0; bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (playing);
        p.setBpm (bpm);
        p.setTimeInSamples (pos);
        p.setPpqPosition ((double) pos / sr * bpm / 60.0);
        return p;
    }
};

static void pump (int ms)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (juce::Time::getMillisecondCounter() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 2) { std::cout << "usage: VoxForgeHostTest <VoxForge.vst3> [shots-dir]\n"; return 2; }

    juce::VST3PluginFormat fmt;
    juce::OwnedArray<juce::PluginDescription> descs;
    fmt.findAllTypesForFile (descs, argv[1]);
    CHECK (descs.size() == 1, "VST3 module scanned");
    if (descs.isEmpty()) return 1;
    auto& d = *descs[0];
    std::cout << "  " << d.name << " by " << d.manufacturerName << ", category " << d.category
              << ", instrument=" << (int) d.isInstrument << "\n";
    CHECK (d.isInstrument, "registered as instrument");

    juce::String err;
    auto plugin = fmt.createInstanceFromDescription (d, 48000.0, 512, err);
    CHECK (plugin != nullptr, "instance created " + err);
    if (plugin == nullptr) return 1;
    CHECK (plugin->acceptsMidi(), "accepts MIDI");
    CHECK (plugin->getTotalNumOutputChannels() == 2, "stereo output");
    std::cout << "  parameters: " << plugin->getParameters().size() << "\n";

    Playhead ph;
    plugin->setPlayHead (&ph);

    for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        ph.sr = sr; ph.pos = 0; ph.playing = false;
        plugin->setPlayConfigDetails (0, 2, sr, 512);
        plugin->prepareToPlay (sr, 512);
        juce::AudioBuffer<float> buf (2, 512);
        float peak = 0.0f;
        bool finite = true;
        // live MIDI voice while the transport is stopped
        for (int b = 0; b < (int) (sr / 512 * 1.0); ++b)
        {
            juce::MidiBuffer mb;
            if (b == 2) mb.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 0);
            if (b == (int) (sr / 512 * 0.8)) mb.addEvent (juce::MidiMessage::noteOff (1, 64), 0);
            buf.clear();
            plugin->processBlock (buf, mb);
            peak = juce::jmax (peak, buf.getMagnitude (0, 0, 512));
            for (int i = 0; i < 512; ++i) finite &= std::isfinite (buf.getSample (0, i));
        }
        CHECK (peak > 0.005f && finite, "live voice @ " + juce::String (sr, 0) + " Hz, peak " + juce::String (peak, 3));
        plugin->releaseResources();
    }

    // offline render of the demo project, then playback through the host transport
    ph.sr = 48000; ph.pos = 0;
    plugin->setPlayConfigDetails (0, 2, 48000, 512);
    plugin->prepareToPlay (48000, 512);
    pump (4000);   // auto-render runs in the background
    ph.playing = true;
    juce::AudioBuffer<float> buf (2, 512);
    float peak = 0.0f;
    for (int b = 0; b < 48000 * 4 / 512; ++b)
    {
        juce::MidiBuffer mb;
        buf.clear();
        plugin->processBlock (buf, mb);
        ph.pos += 512;
        peak = juce::jmax (peak, buf.getMagnitude (0, 0, 512));
    }
    CHECK (peak > 0.05f, "rendered take plays back with the host transport, peak " + juce::String (peak, 3));

    juce::MemoryBlock state;
    plugin->getStateInformation (state);
    CHECK (state.getSize() > 200, "state saved (" + juce::String ((int) state.getSize()) + " bytes)");
    plugin->setStateInformation (state.getData(), (int) state.getSize());
    pump (500);
    juce::MemoryBlock state2;
    plugin->getStateInformation (state2);
    CHECK (state2.getSize() > 200, "state restored");

    if (plugin->hasEditor())
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (plugin->createEditorIfNeeded());
        CHECK (ed != nullptr, "editor opens");
        if (ed != nullptr && argc > 2)
        {
            juce::File dir (argv[2]);
            dir.createDirectory();
            ed->setVisible (true);   // snapshot off-screen (works without a window manager)
            pump (800);
            auto* tabs = ed->findChildWithID ("tabs");
            juce::ignoreUnused (tabs);
            for (int t = 0; t < 4; ++t)
            {
                // switch tabs by simulating the TabbedComponent API through the component tree
                for (auto* c : ed->getChildren())
                    if (auto* tc = dynamic_cast<juce::TabbedComponent*> (c)) tc->setCurrentTabIndex (t);
                pump (600);
                auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
                juce::PNGImageFormat png;
                juce::FileOutputStream os (dir.getChildFile ("tab" + juce::String (t) + ".png"));
                os.setPosition (0); os.truncate();
                png.writeImageToStream (img, os);
            }
            std::cout << "  screenshots in " << dir.getFullPathName() << "\n";
        }
        ed.reset();
    }
    plugin->releaseResources();
    plugin.reset();

    std::cout << "\n" << (failures == 0 ? "HOST TEST PASSED" : juce::String (failures) + " FAILURE(S)") << "\n";
    return failures == 0 ? 0 : 1;
}
