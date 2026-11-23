// Renders each editor tab to PNG without a window (for docs / visual checks).
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

struct Shooter : juce::Timer
{
    juce::AudioProcessorEditor& ed;
    juce::File dir;
    int step = 0;
    Shooter (juce::AudioProcessorEditor& e, juce::File d) : ed (e), dir (d) { startTimer (1500); }
    void timerCallback() override
    {
        static const char* names[] = { "voice-cloning", "presets", "midi-lyrics-editor", "vocal-replacement" };
        if (step > 0)
        {
            auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            auto f = dir.getChildFile (juce::String (names[step - 1]) + ".png");
            f.deleteFile();
            juce::FileOutputStream os (f);
            juce::PNGImageFormat().writeImageToStream (img, os);
        }
        if (step == 4) { stopTimer(); juce::MessageManager::getInstance()->stopDispatchLoop(); return; }
        for (auto* c : ed.getChildren())
            if (auto* tc = dynamic_cast<juce::TabbedComponent*> (c)) tc->setCurrentTabIndex (step);
        ++step;
    }
};

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::File dir (argc > 1 ? argv[1] : "ui-shots");
    dir.createDirectory();
    auto proc = std::make_unique<VoxForgeProcessor>();
    proc->setPlayConfigDetails (0, 2, 48000.0, 512);
    proc->prepareToPlay (48000.0, 512);
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditor());
        ed->setSize (1240, 800);
        Shooter s (*ed, dir);
        juce::MessageManager::getInstance()->runDispatchLoop();
    }
    proc->releaseResources();
    std::cout << "wrote " << dir.getFullPathName() << "\n";
    return 0;
}
