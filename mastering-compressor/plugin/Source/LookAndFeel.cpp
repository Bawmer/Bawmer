#include "LookAndFeel.h"

namespace lumen
{
juce::Font uiFont (float height, bool bold)
{
    auto opts = juce::FontOptions().withHeight (height);
    if (bold)
        opts = opts.withStyle ("Bold");
    return juce::Font (opts);
}

void BrushedMetal::draw (juce::Graphics& g, juce::Rectangle<int> area, float scale)
{
    if (cache.isNull() || area != cacheArea || std::abs (scale - cacheScale) > 0.01f)
    {
        cacheArea = area;
        cacheScale = scale;
        const int w = juce::jmax (1, juce::roundToInt (area.getWidth() * scale));
        const int h = juce::jmax (1, juce::roundToInt (area.getHeight() * scale));
        cache = juce::Image (juce::Image::RGB, w, h, false);
        juce::Graphics ig (cache);
        juce::ColourGradient base (juce::Colour (0xff3a3e44), 0.0f, 0.0f, juce::Colour (0xff24272b), 0.0f, (float) h, false);
        base.addColour (0.45, juce::Colour (0xff33373c));
        ig.setGradientFill (base);
        ig.fillAll();
        juce::Random rng (0x4c554d45); // deterministic texture
        for (int y = 0; y < h; ++y)
        {
            const float a = 0.025f + rng.nextFloat() * 0.05f;
            ig.setColour ((rng.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha (a));
            const float x0 = rng.nextFloat() * w * 0.3f;
            ig.drawHorizontalLine (y, x0, x0 + w * (0.5f + rng.nextFloat() * 0.7f));
        }
        // subtle sheen
        juce::ColourGradient sheen (juce::Colours::white.withAlpha (0.0f), 0.0f, 0.0f,
                                    juce::Colours::white.withAlpha (0.0f), (float) w, 0.0f, false);
        sheen.addColour (0.35, juce::Colours::white.withAlpha (0.06f));
        sheen.addColour (0.5, juce::Colours::white.withAlpha (0.0f));
        ig.setGradientFill (sheen);
        ig.fillAll();
    }
    g.drawImage (cache, area.toFloat());
}

LumenLookAndFeel::LumenLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, colours::background);
    setColour (juce::Slider::textBoxTextColourId, colours::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, colours::text);
    setColour (juce::ComboBox::backgroundColourId, colours::graphite);
    setColour (juce::ComboBox::textColourId, colours::text);
    setColour (juce::ComboBox::outlineColourId, colours::panelEdge);
    setColour (juce::ComboBox::arrowColourId, colours::accent);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff202328));
    setColour (juce::PopupMenu::textColourId, colours::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, colours::accent.withAlpha (0.25f));
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    setColour (juce::TextButton::buttonColourId, colours::graphite);
    setColour (juce::TextButton::buttonOnColourId, colours::accent);
    setColour (juce::TextButton::textColourOffId, colours::text);
    setColour (juce::TextButton::textColourOnId, juce::Colour (0xff17191c));
    setColour (juce::TextEditor::backgroundColourId, colours::graphite);
    setColour (juce::TextEditor::textColourId, colours::text);
    setColour (juce::TextEditor::highlightColourId, colours::accent.withAlpha (0.4f));
    setColour (juce::AlertWindow::backgroundColourId, colours::panel);
    setColour (juce::AlertWindow::textColourId, colours::text);
}

juce::Slider::SliderLayout LumenLookAndFeel::getSliderLayout (juce::Slider& s)
{
    juce::Slider::SliderLayout layout;
    auto b = s.getLocalBounds();
    if (s.getTextBoxPosition() == juce::Slider::TextBoxBelow)
    {
        layout.textBoxBounds = b.removeFromBottom (18);
        b.removeFromBottom (2);
    }
    layout.sliderBounds = b;
    return layout;
}

juce::Label* LumenLookAndFeel::createSliderTextBox (juce::Slider& s)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (s);
    l->setFont (uiFont (13.0f));
    l->setJustificationType (juce::Justification::centred);
    l->setColour (juce::Label::textColourId, colours::text);
    l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    l->setColour (juce::Label::backgroundWhenEditingColourId, colours::graphite);
    l->setColour (juce::Label::textWhenEditingColourId, colours::text);
    l->setColour (juce::Label::outlineWhenEditingColourId, colours::accent);
    return l;
}

void LumenLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start,
                                         float end, juce::Slider& s)
{
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (4.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto c = bounds.getCentre();
    const float angle = start + pos * (end - start);
    const bool enabled = s.isEnabled();
    const auto accent = (s.getProperties()["cool"] ? colours::accentCool : colours::accent)
                            .withMultipliedAlpha (enabled ? 1.0f : 0.35f);

    // track
    const float arcR = radius - 2.0f;
    juce::Path track;
    track.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, start, end, true);
    g.setColour (juce::Colour (0xff0d0e10));
    g.strokePath (track, juce::PathStrokeType (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // value arc (from centre for bipolar parameters)
    const bool bipolar = s.getProperties()["bipolar"];
    const float from = bipolar ? (start + end) * 0.5f : start;
    juce::Path value;
    value.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
    g.setColour (accent);
    g.strokePath (value, juce::PathStrokeType (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // knob body: machined metal
    const float kr = radius * 0.74f;
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillEllipse (c.x - kr, c.y - kr + 2.5f, kr * 2.0f, kr * 2.0f);
    juce::ColourGradient body (juce::Colour (0xff5b6068), c.x - kr, c.y - kr, juce::Colour (0xff23262a), c.x + kr,
                               c.y + kr, false);
    g.setGradientFill (body);
    g.fillEllipse (c.x - kr, c.y - kr, kr * 2.0f, kr * 2.0f);
    juce::ColourGradient face (juce::Colour (0xff474b52), c.x, c.y - kr * 0.8f, juce::Colour (0xff2b2e33), c.x,
                               c.y + kr * 0.8f, false);
    g.setGradientFill (face);
    const float fr = kr * 0.86f;
    g.fillEllipse (c.x - fr, c.y - fr, fr * 2.0f, fr * 2.0f);
    // concentric machining rings
    for (float r = fr * 0.3f; r < fr; r += 2.2f)
    {
        g.setColour (juce::Colours::white.withAlpha (0.018f));
        g.drawEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f, 0.6f);
    }
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawEllipse (c.x - kr, c.y - kr, kr * 2.0f, kr * 2.0f, 1.0f);

    // pointer
    juce::Path pointer;
    pointer.addRoundedRectangle (-1.5f, -fr * 0.92f, 3.0f, fr * 0.45f, 1.5f);
    g.setColour (enabled ? juce::Colours::white.withAlpha (0.92f) : colours::textDim);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (c.x, c.y));
}

void LumenLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = b.getToggleState();
    const auto accent = b.getProperties()["cool"] ? colours::accentCool : colours::accent;
    g.setColour (colours::graphite.brighter (highlighted ? 0.12f : 0.0f));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (on ? accent.withAlpha (0.8f) : colours::panelEdge);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);

    // LED
    const float d = 7.0f;
    auto led = juce::Rectangle<float> (r.getX() + 8.0f, r.getCentreY() - d * 0.5f, d, d);
    if (on)
    {
        g.setColour (accent.withAlpha (0.35f));
        g.fillEllipse (led.expanded (3.0f));
    }
    g.setColour (on ? accent : juce::Colour (0xff3a3e44));
    g.fillEllipse (led);

    g.setColour (b.isEnabled() ? (on ? colours::text : colours::textDim) : colours::textDim.withAlpha (0.4f));
    g.setFont (uiFont (12.0f, true));
    g.drawFittedText (b.getButtonText(), r.withTrimmedLeft (20.0f).toNearestInt(), juce::Justification::centredLeft, 1);
}

void LumenLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted,
                                             bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = b.getToggleState();
    juce::Colour fill = on ? colours::accent : colours::graphite;
    if (highlighted) fill = fill.brighter (0.1f);
    if (down) fill = fill.darker (0.15f);
    juce::ColourGradient grad (fill.brighter (0.08f), 0.0f, r.getY(), fill.darker (0.12f), 0.0f, r.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (on ? colours::accent.brighter (0.3f) : colours::panelEdge.brighter (0.15f));
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
}

void LumenLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.setColour (b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                     .withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.4f));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (4, 2), juce::Justification::centred, 1);
}

void LumenLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h).reduced (0.5f);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
    juce::Path arrow;
    const float ax = (float) w - 14.0f, ay = (float) h * 0.5f;
    arrow.addTriangle (ax - 4.0f, ay - 2.0f, ax + 4.0f, ay - 2.0f, ax, ay + 3.0f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.3f));
    g.fillPath (arrow);
}
} // namespace lumen
