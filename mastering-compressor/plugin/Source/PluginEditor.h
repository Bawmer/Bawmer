#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Displays.h"
#include "LookAndFeel.h"
#include "PluginProcessor.h"

namespace lumen
{
/** Rotary control with caption, value readout and parameter attachment. */
struct Knob
{
    Knob (juce::AudioProcessorValueTreeState& s, const char* paramId, const juce::String& caption, bool bipolar = false,
          bool cool = false);
    juce::Slider slider;
    juce::Label caption;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    void addTo (juce::Component& parent);
    void setBounds (juce::Rectangle<int> r);
    void setEnabled (bool e);
};

struct Toggle
{
    Toggle (juce::AudioProcessorValueTreeState& s, const char* paramId, const juce::String& text, bool cool = false);
    juce::ToggleButton button;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
};

struct Choice
{
    Choice (juce::AudioProcessorValueTreeState& s, const char* paramId, const juce::String& caption);
    juce::ComboBox box;
    juce::Label caption;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
    void addTo (juce::Component& parent);
    void setBounds (juce::Rectangle<int> r);
};

/** Everything lives in a fixed-size logical canvas that is scaled with an affine transform, so
    the interface is resolution independent (vector rendering at any size / DPI). */
class MainPanel final : public juce::Component
{
public:
    static constexpr int kWidth = 1180, kHeight = 760;

    explicit MainPanel (LumenAudioProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh(); // called by the editor timer (30 Hz)
    void setRenderScale (float s) { renderScale = s; }

private:
    void loadPresetList();
    void showSaveDialog();
    void updateModeButtons();
    void updateEnablement();

    LumenAudioProcessor& proc;
    juce::AudioProcessorValueTreeState& state;
    BrushedMetal metal;
    float renderScale = 1.0f;

    // header
    juce::TextButton modeButtons[4];
    juce::ComboBox presetBox;
    juce::TextButton prevPreset { "<" }, nextPreset { ">" }, savePreset { "SAVE" }, deletePreset { "DEL" };
    juce::TextButton slotA { "A" }, slotB { "B" }, copyAB { "A>B" }, undo { "UNDO" }, redo { "REDO" };
    juce::TextButton monitorMode { "L/R" };

    // strip
    Toggle bypass, matchBypass;
    Choice oversampling, osQuality, lookaheadMode;
    juce::Label infoLabel;

    // displays
    LevelMeter inMeter { "INPUT" }, outMeter { "OUTPUT" };
    GainReductionMeter grMeter;
    TransferCurveView curve;
    GainHistoryView history;
    int historyReadIndex = 0;

    // main controls
    Knob threshold, ratio, attack, release, knee, lookahead, makeup, mix, link;
    // sidechain / detector
    Knob scHpf, scLpf;
    Toggle scHpfOn, scLpfOn, extSc, autoRelease, autoGain;
    Choice detection, channelMode;
    // analog / hybrid
    Choice topology;
    Knob blend, drive, character, colorMix;
    Toggle transformer, amplifier;

    bool showMidSide = false;
    juce::Rectangle<int> mainSection, scSection, analogSection;
};

class LumenAudioProcessorEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit LumenAudioProcessorEditor (LumenAudioProcessor&);
    ~LumenAudioProcessorEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void timerCallback() override;

    LumenAudioProcessor& proc;
    LumenLookAndFeel lnf;
    MainPanel panel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LumenAudioProcessorEditor)
};
} // namespace lumen
