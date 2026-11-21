// Renders the editor to a PNG while program material runs through the processor.
// Developer tool used to verify metering / displays without a DAW:  lumen_ui_snapshot out.png [mode]
#include "../plugin/Source/PluginEditor.h"
#include "../plugin/Source/PluginProcessor.h"

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "snapshot.png"));
    const int mode = argc > 2 ? juce::String (argv[2]).getIntValue() : 3;

    lumen::LumenAudioProcessor proc;
    auto set = [&] (const char* id, float v) {
        auto* p = proc.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    set (lumen::ids::mode, (float) mode);
    set (lumen::ids::threshold, -22.0f);
    set (lumen::ids::ratio, 3.0f);
    set (lumen::ids::attack, 8.0f);
    set (lumen::ids::release, 180.0f);
    set (lumen::ids::autoRelease, 1.0f);
    set (lumen::ids::lookahead, 3.0f);

    const double fs = 48000.0;
    const int block = 480;
    proc.prepareToPlay (fs, block);
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setSize (1180, 760);
    auto* panel = dynamic_cast<lumen::MainPanel*> (editor->getChildComponent (0));

    juce::AudioBuffer<double> buf (2, block);
    juce::MidiBuffer midi;
    juce::Random rng (1);
    double phase = 0.0;
    long n = 0;
    for (int b = 0; b < 100 * 8; ++b) // 8 seconds
    {
        for (int i = 0; i < block; ++i, ++n)
        {
            // kick every 0.5 s + sustained chord + noise, with a louder section in the middle
            const double t = (double) (n % 24000) / fs;
            const double kick = std::exp (-t * 18.0) * std::sin (2.0 * juce::MathConstants<double>::pi * (50.0 + 90.0 * std::exp (-t * 30.0)) * t);
            phase += 2.0 * juce::MathConstants<double>::pi * 220.0 / fs;
            const double section = (n > fs * 3 && n < fs * 6) ? 1.6 : 0.8;
            const double pad = 0.12 * (std::sin (phase) + 0.6 * std::sin (phase * 1.26) + 0.5 * std::sin (phase * 1.5));
            const double hiss = 0.02 * (rng.nextDouble() * 2.0 - 1.0);
            buf.setSample (0, i, section * (0.55 * kick + pad + hiss));
            buf.setSample (1, i, section * (0.55 * kick + 0.8 * pad - hiss));
        }
        proc.processBlock (buf, midi);
        if (b % 3 == 0 && panel != nullptr)
            panel->refresh();
    }

    auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    juce::PNGImageFormat png;
    out.deleteFile();
    juce::FileOutputStream os (out);
    return png.writeImageToStream (image, os) ? 0 : 1;
}
