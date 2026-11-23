#include "PluginEditor.h"

using vf::ui::styleDim;
namespace C = vf::ui::Colours;

VoxForgeEditor::VoxForgeEditor (VoxForgeProcessor& p)
    : AudioProcessorEditor (&p), proc (p), cloneTab (p), presetsTab (p), editorTab (p), replaceTab (p)
{
    setLookAndFeel (&lnf);

    tabs.setTabBarDepth (40);
    tabs.addTab ("Voice Cloning", C::bg, &cloneTab, false);
    tabs.addTab ("Presets", C::bg, &presetsTab, false);
    tabs.addTab ("MIDI-Lyrics Editor", C::bg, &editorTab, false);
    tabs.addTab ("Vocal Replacement", C::bg, &replaceTab, false);
    tabs.setCurrentTabIndex (juce::jlimit (0, 3, proc.lastEditorTab), false);
    addAndMakeVisible (tabs);
    replaceTab.openEditor = [this] { tabs.setCurrentTabIndex (2); };

    voiceLabel.setText ("Voice", juce::dontSendNotification);
    styleDim (voiceLabel, 13.0f);
    addAndMakeVisible (voiceLabel);
    voiceBox.onChange = [this]
    {
        const int idx = voiceBox.getSelectedItemIndex();
        if (idx >= 0 && idx < voiceIds.size() && voiceIds[idx] != proc.currentVoice().id)
            proc.selectVoice (voiceIds[idx], true);
    };
    addAndMakeVisible (voiceBox);
    engineBadge.setJustificationType (juce::Justification::centred);
    engineBadge.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    addAndMakeVisible (engineBadge);
    addAndMakeVisible (renderBar);
    styleDim (statusLine, 12.5f);
    addAndMakeVisible (statusLine);

    refreshVoiceBox();
    setResizable (true, true);
    setResizeLimits (1000, 680, 2400, 1600);
    setSize (1240, 800);
    startTimerHz (15);
}

VoxForgeEditor::~VoxForgeEditor()
{
    proc.lastEditorTab = tabs.getCurrentTabIndex();
    stopTimer();
    setLookAndFeel (nullptr);
}

void VoxForgeEditor::paint (juce::Graphics& g)
{
    g.fillAll (C::bg);
    auto header = getLocalBounds().removeFromTop (56);
    g.setGradientFill (juce::ColourGradient (C::panel, 0, 0, C::bg, 0, (float) header.getBottom(), false));
    g.fillRect (header);

    // logo
    auto logo = header.reduced (16, 10).removeFromLeft (420);
    juce::Path wave;
    const float cx = (float) logo.getX() + 16.0f, cy = (float) logo.getCentreY();
    for (int i = 0; i < 5; ++i)
    {
        const float h = (float) (6 + (i % 3) * 7 + (i == 2 ? 6 : 0));
        wave.addRoundedRectangle (cx + i * 6.0f - 12.0f, cy - h * 0.5f, 3.5f, h, 1.75f);
    }
    g.setGradientFill (juce::ColourGradient (C::accent, cx - 12, cy, C::accent2, cx + 18, cy, false));
    g.fillPath (wave);
    g.setColour (C::text);
    g.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
    g.drawText ("VoxForge", logo.withTrimmedLeft (44), juce::Justification::centredLeft);
    g.setColour (C::dim);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.drawText ("AI vocal synth & voice cloning", logo.withTrimmedLeft (150).withWidth (260).withTrimmedTop (6), juce::Justification::centredLeft);

    g.setColour (C::outline);
    g.drawHorizontalLine (getHeight() - 26, 0.0f, (float) getWidth());
}

void VoxForgeEditor::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (56).reduced (16, 12);
    header.removeFromLeft (420);
    renderBar.setBounds (header.removeFromRight (300).reduced (0, 4));
    header.removeFromRight (12);
    engineBadge.setBounds (header.removeFromRight (120).reduced (0, 3));
    header.removeFromRight (12);
    voiceBox.setBounds (header.removeFromRight (240));
    voiceLabel.setBounds (header.removeFromRight (50));

    statusLine.setBounds (r.removeFromBottom (26).reduced (12, 2));
    tabs.setBounds (r);
}

void VoxForgeEditor::refreshVoiceBox()
{
    voiceBox.clear (juce::dontSendNotification);
    voiceIds.clear();
    int i = 1;
    for (auto& v : proc.presets().voices())
    {
        voiceBox.addItem (v.name + (v.isNeural() ? "  [neural]" : ""), i++);
        voiceIds.add (v.id);
    }
    voiceBox.setSelectedItemIndex (juce::jmax (0, voiceIds.indexOf (proc.currentVoice().id)), juce::dontSendNotification);
    voiceCount = (int) proc.presets().voices().size();
    shownVoice = proc.currentVoice().id + (proc.currentVoice().isNeural() ? "n" : "l");
}

void VoxForgeEditor::timerCallback()
{
    if ((int) proc.presets().voices().size() != voiceCount
        || shownVoice != proc.currentVoice().id + (proc.currentVoice().isNeural() ? "n" : "l"))
        refreshVoiceBox();

    const bool neural = proc.currentVoice().isNeural() && vf::NeuralBackend::isCompiledIn();
    engineBadge.setText (neural ? "NEURAL" : "DSP ENGINE", juce::dontSendNotification);
    engineBadge.setColour (juce::Label::backgroundColourId, neural ? C::accent : C::panel2);
    engineBadge.setColour (juce::Label::textColourId, C::text);

    auto& re = proc.renderer();
    if (re.isBusy())
    {
        renderProgress = re.progress();
        renderBar.setTextToDisplay (re.status());
    }
    else
    {
        renderProgress = proc.hasTake() ? 1.0 : 0.0;
        renderBar.setTextToDisplay (proc.hasTake() ? (proc.renderIsStale() ? "Render out of date" : "Rendered") : "Not rendered");
    }

    statusLine.setText (proc.lastMessage, juce::dontSendNotification);

    switch (tabs.getCurrentTabIndex())
    {
        case 0: cloneTab.tick(); break;
        case 1: presetsTab.tick(); break;
        case 2: editorTab.tick(); break;
        case 3: replaceTab.tick(); break;
        default: break;
    }
    // background work that must be picked up even when the tab is hidden
    if (tabs.getCurrentTabIndex() != 3 && proc.stemAnalyzer().hasResult()) replaceTab.tick();
    if (tabs.getCurrentTabIndex() != 0 && proc.trainJob != nullptr) cloneTab.tick();
    proc.lastEditorTab = tabs.getCurrentTabIndex();
}
