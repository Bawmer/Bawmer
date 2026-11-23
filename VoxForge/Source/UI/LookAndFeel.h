#pragma once

#include <JuceHeader.h>

namespace vf::ui
{

namespace Colours
{
    inline const juce::Colour bg       { 0xff14151b };
    inline const juce::Colour panel    { 0xff1d1f28 };
    inline const juce::Colour panel2   { 0xff262935 };
    inline const juce::Colour outline  { 0xff343848 };
    inline const juce::Colour accent   { 0xff7c5cff };
    inline const juce::Colour accent2  { 0xff22d3c5 };
    inline const juce::Colour text     { 0xffe8e9ef };
    inline const juce::Colour dim      { 0xff8b8fa3 };
    inline const juce::Colour warn     { 0xffffb454 };
    inline const juce::Colour error    { 0xffff6b6b };
    inline const juce::Colour ok       { 0xff5be38a };
}

class VFLookAndFeel : public juce::LookAndFeel_V4
{
public:
    VFLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float start, float end, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool highlighted, bool down) override;
    void drawTabButton (juce::TabBarButton&, juce::Graphics&, bool isMouseOver, bool isMouseDown) override;
    int getTabButtonBestWidth (juce::TabBarButton&, int tabDepth) override { juce::ignoreUnused (tabDepth); return 190; }
    void drawProgressBar (juce::Graphics&, juce::ProgressBar&, int w, int h, double progress, const juce::String& text) override;
    juce::Font getTextButtonFont (juce::TextButton&, int h) override;
};

/** Rounded panel with a title, used to group controls. */
class Section : public juce::Component
{
public:
    explicit Section (juce::String t = {}) : title (std::move (t)) {}
    void setTitle (const juce::String& t) { title = t; repaint(); }
    void paint (juce::Graphics& g) override;
    /** Content area in the PARENT's coordinates (controls are siblings of the section). */
    juce::Rectangle<int> content() const { return getBounds().reduced (12).withTrimmedTop (title.isEmpty() ? 0 : 22); }
private:
    juce::String title;
};

/** Knob with label underneath, attached to a parameter. */
class Knob : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState& s, const juce::String& paramId, const juce::String& label);
    void resized() override;
    juce::Slider slider;
private:
    juce::Label label;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

void styleHeading (juce::Label& l, float size = 15.0f);
void styleDim (juce::Label& l, float size = 13.0f);

} // namespace vf::ui
