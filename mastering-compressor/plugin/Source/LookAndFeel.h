#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace lumen
{
namespace colours
{
inline const juce::Colour background { 0xff15171a };
inline const juce::Colour panel { 0xff1e2125 };
inline const juce::Colour panelEdge { 0xff2c3036 };
inline const juce::Colour graphite { 0xff26292e };
inline const juce::Colour text { 0xffd9dde3 };
inline const juce::Colour textDim { 0xff8a919b };
inline const juce::Colour accent { 0xffe0a64a };      // warm amber
inline const juce::Colour accentCool { 0xff6fb7d6 };  // steel blue
inline const juce::Colour gr { 0xffe0794a };
inline const juce::Colour meterGreen { 0xff6cc48a };
inline const juce::Colour meterYellow { 0xffe3c45a };
inline const juce::Colour meterRed { 0xffe0564a };
} // namespace colours

juce::Font uiFont (float height, bool bold = false);

/** Procedurally generated brushed-metal texture (cached per size). */
class BrushedMetal
{
public:
    void draw (juce::Graphics& g, juce::Rectangle<int> area, float scale);

private:
    juce::Image cache;
    juce::Rectangle<int> cacheArea;
    float cacheScale = 0.0f;
};

class LumenLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LumenLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end,
                           juce::Slider&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override { return uiFont (13.0f); }
    juce::Font getPopupMenuFont() override { return uiFont (14.0f); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return uiFont (12.5f, true); }
    juce::Label* createSliderTextBox (juce::Slider&) override;
    juce::Slider::SliderLayout getSliderLayout (juce::Slider&) override;
};
} // namespace lumen
