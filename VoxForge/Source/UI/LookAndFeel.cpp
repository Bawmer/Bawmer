#include "LookAndFeel.h"

namespace vf::ui
{

VFLookAndFeel::VFLookAndFeel()
{
    auto scheme = juce::LookAndFeel_V4::ColourScheme (
        Colours::bg, Colours::panel, Colours::panel2, Colours::outline, Colours::text,
        Colours::accent, juce::Colours::white, Colours::panel2, Colours::text);
    setColourScheme (scheme);

    setColour (juce::ResizableWindow::backgroundColourId, Colours::bg);
    setColour (juce::TextButton::buttonColourId, Colours::panel2);
    setColour (juce::TextButton::buttonOnColourId, Colours::accent);
    setColour (juce::TextButton::textColourOffId, Colours::text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    setColour (juce::ComboBox::backgroundColourId, Colours::panel2);
    setColour (juce::ComboBox::outlineColourId, Colours::outline);
    setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff111218));
    setColour (juce::TextEditor::outlineColourId, Colours::outline);
    setColour (juce::TextEditor::focusedOutlineColourId, Colours::accent);
    setColour (juce::TextEditor::textColourId, Colours::text);
    setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff111218));
    setColour (juce::ListBox::outlineColourId, Colours::outline);
    setColour (juce::Slider::rotarySliderFillColourId, Colours::accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, Colours::outline);
    setColour (juce::Slider::thumbColourId, Colours::accent2);
    setColour (juce::Slider::trackColourId, Colours::accent);
    setColour (juce::Slider::backgroundColourId, Colours::outline);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxTextColourId, Colours::dim);
    setColour (juce::Label::textColourId, Colours::text);
    setColour (juce::ToggleButton::tickColourId, Colours::accent2);
    setColour (juce::ToggleButton::textColourId, Colours::text);
    setColour (juce::TabbedComponent::backgroundColourId, Colours::bg);
    setColour (juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ProgressBar::foregroundColourId, Colours::accent);
    setColour (juce::ProgressBar::backgroundColourId, Colours::panel2);
    setColour (juce::PopupMenu::backgroundColourId, Colours::panel);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Colours::accent);
    setColour (juce::ScrollBar::thumbColourId, Colours::outline);
}

void VFLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                      float start, float end, juce::Slider& s)
{
    auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (4.0f);
    const float r = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
    const auto c = bounds.getCentre();
    const float angle = start + pos * (end - start);
    const float lw = 4.0f;

    juce::Path track;
    track.addCentredArc (c.x, c.y, r - lw, r - lw, 0.0f, start, end, true);
    g.setColour (Colours::outline);
    g.strokePath (track, juce::PathStrokeType (lw, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
    const float from = bipolar ? (start + end) * 0.5f : start;
    juce::Path val;
    val.addCentredArc (c.x, c.y, r - lw, r - lw, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
    g.setGradientFill (juce::ColourGradient (Colours::accent, c.x - r, c.y, Colours::accent2, c.x + r, c.y, false));
    g.strokePath (val, juce::PathStrokeType (lw, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (Colours::panel2);
    g.fillEllipse (bounds.reduced (lw * 2.5f));
    juce::Path p;
    p.addRoundedRectangle (-1.5f, -(r - lw * 3.0f), 3.0f, r * 0.45f, 1.5f);
    g.setColour (Colours::text);
    g.fillPath (p, juce::AffineTransform::rotation (angle).translated (c.x, c.y));
}

void VFLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& base,
                                          bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    auto col = b.getToggleState() ? Colours::accent : base;
    if (! b.isEnabled()) col = col.withAlpha (0.4f);
    if (down) col = col.darker (0.2f);
    else if (highlighted) col = col.brighter (0.12f);
    g.setColour (col);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (Colours::outline.withAlpha (0.8f));
    g.drawRoundedRectangle (r, 6.0f, 1.0f);
}

juce::Font VFLookAndFeel::getTextButtonFont (juce::TextButton&, int h)
{
    return juce::Font (juce::FontOptions (juce::jmin (15.0f, (float) h * 0.5f)));
}

void VFLookAndFeel::drawTabButton (juce::TabBarButton& b, juce::Graphics& g, bool over, bool)
{
    auto r = b.getLocalBounds().toFloat().reduced (3.0f, 4.0f);
    const bool front = b.isFrontTab();
    g.setColour (front ? Colours::panel2 : (over ? Colours::panel : Colours::bg));
    g.fillRoundedRectangle (r, 8.0f);
    if (front)
    {
        g.setColour (Colours::accent);
        g.fillRoundedRectangle (r.removeFromBottom (3.0f).reduced (14.0f, 0.0f), 1.5f);
    }
    g.setColour (front ? Colours::text : Colours::dim);
    g.setFont (juce::Font (juce::FontOptions (15.0f, front ? juce::Font::bold : juce::Font::plain)));
    g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred);
}

void VFLookAndFeel::drawProgressBar (juce::Graphics& g, juce::ProgressBar&, int w, int h, double progress, const juce::String& text)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
    g.setColour (Colours::panel2);
    g.fillRoundedRectangle (r, h * 0.5f);
    if (progress >= 0.0 && progress <= 1.0)
    {
        g.setGradientFill (juce::ColourGradient (Colours::accent, 0, 0, Colours::accent2, (float) w, 0, false));
        g.fillRoundedRectangle (r.withWidth ((float) (w * progress)), h * 0.5f);
    }
    g.setColour (Colours::text);
    g.setFont (juce::Font (juce::FontOptions ((float) h * 0.62f)));
    g.drawText (text, r, juce::Justification::centred);
}

//==============================================================================
void Section::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (Colours::panel);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (Colours::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (r, 10.0f, 1.0f);
    if (title.isNotEmpty())
    {
        g.setColour (Colours::text);
        g.setFont (juce::Font (juce::FontOptions (14.5f, juce::Font::bold)));
        g.drawText (title, getLocalBounds().reduced (12, 8).removeFromTop (18), juce::Justification::centredLeft);
    }
}

Knob::Knob (juce::AudioProcessorValueTreeState& s, const juce::String& paramId, const juce::String& text)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
    addAndMakeVisible (slider);
    label.setText (text, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (juce::Font (juce::FontOptions (12.5f)));
    label.setColour (juce::Label::textColourId, Colours::dim);
    addAndMakeVisible (label);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (s, paramId, slider);
}

void Knob::resized()
{
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (16));
    slider.setBounds (r);
}

void styleHeading (juce::Label& l, float size)
{
    l.setFont (juce::Font (juce::FontOptions (size, juce::Font::bold)));
    l.setColour (juce::Label::textColourId, Colours::text);
}

void styleDim (juce::Label& l, float size)
{
    l.setFont (juce::Font (juce::FontOptions (size)));
    l.setColour (juce::Label::textColourId, Colours::dim);
}

} // namespace vf::ui
