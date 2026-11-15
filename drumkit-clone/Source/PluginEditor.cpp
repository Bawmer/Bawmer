#include "PluginEditor.h"

using namespace dkc;

namespace
{
const juce::Colour kBackground (0xff14161a), kPanel (0xff1d2026), kPanelLight (0xff2a2e37),
                   kText (0xffe8eaee), kDim (0xff8a909c), kAccent (0xffffb347);

juce::Font uiFont (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

const char* const kAudioWildcard = "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a;*.aac;*.wma";
}

//==============================================================================
struct DrumkitCloneEditor::Look : public juce::LookAndFeel_V4
{
    Look()
    {
        setColour (juce::ResizableWindow::backgroundColourId, kBackground);
        setColour (juce::TextButton::buttonColourId, kPanelLight);
        setColour (juce::TextButton::textColourOffId, kText);
        setColour (juce::Slider::thumbColourId, kAccent);
        setColour (juce::Slider::trackColourId, kAccent.withAlpha (0.7f));
        setColour (juce::Slider::backgroundColourId, kPanelLight);
        setColour (juce::Slider::rotarySliderFillColourId, kAccent);
        setColour (juce::Slider::rotarySliderOutlineColourId, kPanelLight);
        setColour (juce::Slider::textBoxTextColourId, kText);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Label::textColourId, kDim);
        setColour (juce::ComboBox::backgroundColourId, kPanelLight);
        setColour (juce::ComboBox::textColourId, kText);
        setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::ComboBox::arrowColourId, kDim);
        setColour (juce::PopupMenu::backgroundColourId, kPanel);
        setColour (juce::PopupMenu::textColourId, kText);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, kAccent.withAlpha (0.35f));
        setColour (juce::PopupMenu::highlightedTextColourId, kText);
        setColour (juce::TooltipWindow::backgroundColourId, kPanel);
        setColour (juce::TooltipWindow::textColourId, kText);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override              { return uiFont (12.5f); }
    juce::Font getPopupMenuFont() override                             { return uiFont (13.0f); }

    juce::Font getTextButtonFont (juce::TextButton&, int) override   { return uiFont (13.0f, true); }
    juce::Font getLabelFont (juce::Label& l) override                { return l.getFont(); }
};

//==============================================================================
class DrumkitCloneEditor::Overview : public juce::Component
{
public:
    explicit Overview (DrumkitCloneEditor& e) : editor (e) {}

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour (kPanel);
        g.fillRoundedRectangle (area, 6.0f);

        const auto& p = editor.processor;
        const auto kit = editor.kit;
        auto inner = area.reduced (10.0f, 8.0f);

        if (p.isAnalysing())
        {
            g.setColour (kText);
            g.setFont (uiFont (15.0f, true));
            g.drawText ("Finding hits, grouping and isolating...", inner.removeFromTop (inner.getHeight() * 0.55f),
                        juce::Justification::centredBottom);
            auto bar = inner.removeFromTop (16.0f).withSizeKeepingCentre (juce::jmin (360.0f, inner.getWidth()), 6.0f);
            g.setColour (kPanelLight);
            g.fillRoundedRectangle (bar, 3.0f);
            g.setColour (kAccent);
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, p.getProgress())), 3.0f);
        }
        else if (kit == nullptr || kit->pads.empty())
        {
            g.setColour (editor.fileHover ? kAccent : kDim);
            const float dashes[] = { 6.0f, 5.0f };
            juce::Path outline, dashed;
            outline.addRoundedRectangle (area.reduced (8.0f), 6.0f);
            juce::PathStrokeType (1.5f).createDashedStroke (dashed, outline, dashes, 2);
            g.fillPath (dashed);

            g.setFont (uiFont (17.0f, true));
            g.setColour (editor.fileHover ? kAccent : kText);
            g.drawText ("Drop a loop, a one-shot or a whole song here", inner.removeFromTop (inner.getHeight() * 0.56f),
                        juce::Justification::centredBottom);
            g.setFont (uiFont (13.0f));
            g.setColour (kDim);
            const auto msg = p.getStatusMessage();
            g.drawText (msg.isNotEmpty() ? msg : juce::String ("WAV, AIFF, FLAC, OGG, MP3 - every hit becomes a pad"),
                        inner, juce::Justification::centredTop);
        }
        else
        {
            const auto& ov = kit->overview;
            const float midY = inner.getCentreY(), half = inner.getHeight() * 0.5f - 6.0f;
            float peak = 1.0e-4f;
            for (auto v : ov) peak = juce::jmax (peak, v);

            juce::Path wave;
            for (size_t i = 0; i < ov.size(); ++i)
            {
                const float x = inner.getX() + inner.getWidth() * (float) i / (float) ov.size();
                const float h = juce::jmax (0.5f, half * ov[i] / peak);
                wave.addRectangle (x, midY - h, juce::jmax (1.0f, inner.getWidth() / (float) ov.size()), h * 2.0f);
            }
            g.setColour (kDim.withAlpha (0.55f));
            g.fillPath (wave);

            for (auto& h : kit->hits)
            {
                const float x = inner.getX() + inner.getWidth() * h.position;
                const bool selected = h.pad == editor.selectedPad;
                g.setColour (h.pad >= 0 ? padColour (h.pad).withAlpha (selected ? 1.0f : 0.6f) : kDim.withAlpha (0.25f));
                g.fillRect (x, inner.getY(), selected ? 2.0f : 1.0f, inner.getHeight());
                if (h.pad >= 0)
                    g.fillEllipse (x - 2.5f, inner.getY() - 2.5f, 6.0f, 6.0f);
            }

            if (editor.fileHover)
            {
                g.setColour (kAccent);
                g.drawRoundedRectangle (area.reduced (1.0f), 6.0f, 2.0f);
            }
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mouseWasDraggedSinceMouseDown() && (editor.kit == nullptr || editor.kit->pads.empty()))
            editor.chooseFile();
    }

private:
    DrumkitCloneEditor& editor;
};

//==============================================================================
class DrumkitCloneEditor::PadButton : public juce::Component
{
public:
    PadButton (DrumkitCloneEditor& e, int i) : editor (e), index (i) {}

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat().reduced (3.0f);
        const auto kit = editor.kit;
        const bool filled = kit != nullptr && index < (int) kit->pads.size();
        const bool selected = filled && editor.selectedPad == index;
        const auto colour = padColour (index);

        g.setColour (filled ? kPanelLight.interpolatedWith (colour, 0.18f + 0.6f * flash) : kPanel);
        g.fillRoundedRectangle (area, 7.0f);
        g.setColour (selected ? colour : (filled ? colour.withAlpha (0.45f) : kPanelLight));
        g.drawRoundedRectangle (area, 7.0f, selected ? 2.2f : 1.0f);

        auto inner = area.reduced (8.0f, 6.0f);
        auto top = inner.removeFromTop (16.0f), bottom = inner.removeFromBottom (14.0f);

        g.setFont (uiFont (11.0f));
        g.setColour (filled ? kText.withAlpha (0.75f) : kDim);
        g.drawText (juce::MidiMessage::getMidiNoteName (DrumkitCloneProcessor::firstNote + index, true, true, 3),
                    bottom, juce::Justification::centredRight);

        if (! filled)
            return;

        const auto& pad = kit->pads[(size_t) index];
        g.setFont (uiFont (13.5f, true));
        g.setColour (kText);
        g.drawText (pad.name, top.withTrimmedRight (40.0f), juce::Justification::centredLeft);

        // drag handle for this drum's MIDI part
        const auto tag = midiTagBounds().toFloat();
        g.setColour (hoverTag ? kText : kBackground.withAlpha (0.35f));
        g.fillRoundedRectangle (tag, 4.0f);
        g.setColour (hoverTag ? kBackground : kText.withAlpha (0.9f));
        g.setFont (uiFont (10.0f, true));
        g.drawText ("MIDI", tag, juce::Justification::centred);
        g.setFont (uiFont (11.0f));
        g.setColour (kText.withAlpha (0.75f));
        g.drawText (juce::String (pad.hitCount) + (pad.hitCount == 1 ? " hit" : " hits"), bottom, juce::Justification::centredLeft);

        if (! pad.thumb.empty())
        {
            inner.reduce (0.0f, 3.0f);
            juce::Path wave;
            const float midY = inner.getCentreY(), half = inner.getHeight() * 0.5f, step = inner.getWidth() / (float) pad.thumb.size();
            for (size_t i = 0; i < pad.thumb.size(); ++i)
            {
                const float h = juce::jmax (0.6f, half * pad.thumb[i]);
                wave.addRoundedRectangle (inner.getX() + step * (float) i, midY - h, juce::jmax (1.0f, step - 1.0f), h * 2.0f, 0.5f);
            }
            g.setColour (colour.brighter (0.2f * flash));
            g.fillPath (wave);
        }
    }

    juce::Rectangle<int> midiTagBounds() const
    {
        return getLocalBounds().reduced (3).reduced (8, 6).removeFromTop (16).removeFromRight (36);
    }

    bool hasPad() const    { return editor.kit != nullptr && index < (int) editor.kit->pads.size(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        draggingTag = hasPad() && midiTagBounds().expanded (2).contains (e.getPosition());
        if (draggingTag)
            editor.selectPad (index);       // the tag selects but doesn't play
        else
            editor.padPressed (index);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingTag)
        {
            if (e.getDistanceFromDragStart() > 6)
                editor.dragMidiOut (index);
        }
        else if (e.getDistanceFromDragStart() > 12 && ! getLocalBounds().contains (e.getPosition()))
        {
            editor.padDraggedOut (index);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool over = hasPad() && midiTagBounds().expanded (2).contains (e.getPosition());
        if (over != hoverTag) { hoverTag = over; repaint(); }
        setMouseCursor (over ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverTag) { hoverTag = false; repaint(); }
    }

    void tick (int triggerCount)
    {
        const float before = flash;
        if (triggerCount != lastCount) { lastCount = triggerCount; flash = 1.0f; }
        else flash = flash > 0.02f ? flash * 0.8f : 0.0f;
        if (! juce::approximatelyEqual (before, flash))
            repaint();
    }

private:
    DrumkitCloneEditor& editor;
    const int index;
    int lastCount = 0;
    float flash = 0.0f;
    bool hoverTag = false, draggingTag = false;
};

//==============================================================================
/** Drag this onto the DAW's track area: one MIDI track per drum. */
class DrumkitCloneEditor::MidiDragHandle : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit MidiDragHandle (DrumkitCloneEditor& e) : editor (e)
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        setTooltip ("Drag onto your DAW's tracks: one MIDI track per drum (Kick, Snare, Hat...)");
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = isEnabled();
        g.setColour (on ? (hover ? kAccent : kAccent.withAlpha (0.85f)) : kPanelLight);
        g.fillRoundedRectangle (area, 5.0f);

        auto content = area.reduced (10.0f, 0.0f);
        auto icon = content.removeFromLeft (16.0f).withSizeKeepingCentre (14.0f, 14.0f);
        g.setColour (on ? kBackground : kDim);
        for (int row = 0; row < 3; ++row)       // a tiny piano-roll
            g.fillRoundedRectangle (icon.getX() + (float) (row * 3), icon.getY() + (float) row * 5.0f, 8.0f, 3.0f, 1.0f);
        content.removeFromLeft (8.0f);
        g.setFont (uiFont (13.0f, true));
        g.drawText ("DRAG MIDI TO DAW", content, juce::Justification::centredLeft);
    }

    void mouseEnter (const juce::MouseEvent&) override  { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { hover = false; repaint(); }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (isEnabled() && e.getDistanceFromDragStart() > 6)
            editor.dragMidiOut (-1);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (isEnabled() && ! e.mouseWasDraggedSinceMouseDown())
            editor.infoLabel.setText ("Hold and drag this onto your DAW's track area", juce::dontSendNotification);
    }

private:
    DrumkitCloneEditor& editor;
    bool hover = false;
};

//==============================================================================
juce::Colour DrumkitCloneEditor::padColour (int pad)
{
    static const juce::uint32 palette[] = { 0xffff6b5e, 0xffffb347, 0xffffe066, 0xff8fe07a, 0xff4fd6c4, 0xff5ab8ff, 0xff8c8cff, 0xffd08cff,
                                            0xffff8fc7, 0xffff8a5c, 0xffc9e265, 0xff5fe0a0, 0xff62c7e8, 0xff7da2ff, 0xffb399ff, 0xfff09ae0 };
    return juce::Colour (palette[(size_t) juce::jlimit (0, 15, pad)]);
}

DrumkitCloneEditor::DrumkitCloneEditor (DrumkitCloneProcessor& p)
    : AudioProcessorEditor (p), processor (p), look (std::make_unique<Look>())
{
    setLookAndFeel (look.get());

    overview = std::make_unique<Overview> (*this);
    addAndMakeVisible (*overview);

    for (int i = 0; i < kMaxPads; ++i)
        addAndMakeVisible (pads.add (new PadButton (*this, i)));

    midiHandle = std::make_unique<MidiDragHandle> (*this);
    addAndMakeVisible (*midiHandle);

    for (auto* b : { &loadButton, &exportButton, &reanalyseButton })
        addAndMakeVisible (*b);
    exportButton.setTooltip ("Save every pad as a WAV, plus the MIDI pattern (one file per drum and a multi-track file)");
    loadButton.onClick = [this] { chooseFile(); };
    exportButton.onClick = [this] { exportKit(); };
    reanalyseButton.onClick = [this] { processor.reanalyse(); };
    reanalyseButton.setTooltip ("Analyze the same file again with the current Sensitivity and Separation");

    auto setupLabel = [this] (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (uiFont (11.0f, true));
        l.setJustificationType (juce::Justification::centredLeft);
        l.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (l);
    };
    auto setupSlider = [this] (juce::Slider& s)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 18);
        addAndMakeVisible (s);
    };

    setupLabel (sensitivityLabel, "SENSITIVITY");
    setupLabel (separationLabel, "SEPARATION");
    setupLabel (gainLabel, "GAIN");
    setupLabel (tuneLabel, "TUNE");
    setupLabel (panLabel, "PAN");
    setupLabel (masterLabel, "MASTER");
    setupLabel (bpmLabel, "BPM");
    setupLabel (quantizeLabel, "QUANTIZE");
    setupLabel (noteMapLabel, "NOTES");
    setupLabel (layoutLabel, "FILES");

    bpm.setSliderStyle (juce::Slider::IncDecButtons);
    bpm.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 52, 22);
    bpm.setRange (40.0, 300.0, 0.01);
    bpm.setNumDecimalPlacesToDisplay (1);
    bpm.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
    bpm.onValueChange = [this] { processor.midiBpmOverride.store (bpm.getValue()); };
    addAndMakeVisible (bpm);

    quantizeBox.addItemList ({ "Off", "1/8", "1/16", "1/32" }, 1);
    quantizeBox.onChange = [this]
    {
        static const int steps[] = { 0, 2, 4, 8 };
        processor.midiQuantize.store (steps[juce::jlimit (0, 3, quantizeBox.getSelectedItemIndex())]);
    };
    quantizeBox.setTooltip ("Snap the MIDI notes to a grid (Off keeps the original groove)");
    noteMapBox.addItemList ({ "Pads (C1 up)", "GM drums" }, 1);
    noteMapBox.onChange = [this] { processor.midiNoteMap.store (noteMapBox.getSelectedItemIndex()); };
    noteMapBox.setTooltip ("Pads: plays Drumkit Clone. GM drums: Kick 36, Snare 38, Hat 42... for any drum plugin");
    layoutBox.addItemList ({ "One per drum", "Multi-track" }, 1);
    layoutBox.onChange = [this] { processor.midiLayout.store (layoutBox.getSelectedItemIndex()); };
    layoutBox.setTooltip ("One per drum: a .mid per drum, each lands on its own track. Multi-track: one Type 1 .mid file");
    for (auto* c : { &quantizeBox, &noteMapBox, &layoutBox })
        addAndMakeVisible (*c);
    setupLabel (selectedLabel, {});
    setupLabel (infoLabel, {});
    selectedLabel.setFont (uiFont (14.0f, true));
    infoLabel.setFont (uiFont (12.5f));
    infoLabel.setJustificationType (juce::Justification::centredRight);

    for (auto* s : { &sensitivity, &separation, &gain, &tune, &pan, &master })
        setupSlider (*s);

    for (auto* s : { &sensitivity, &separation })
    {
        s->setRange (0.0, 100.0, 1.0);
        s->setTextValueSuffix (" %");
        s->setDoubleClickReturnValue (true, 50.0);
    }
    sensitivity.setValue (processor.sensitivity.load() * 100.0, juce::dontSendNotification);
    separation.setValue (processor.separation.load() * 100.0, juce::dontSendNotification);
    sensitivity.onValueChange = [this] { processor.sensitivity.store ((float) sensitivity.getValue() / 100.0f); };
    separation.onValueChange = [this] { processor.separation.store ((float) separation.getValue() / 100.0f); };
    sensitivity.setTooltip ("Higher finds quieter hits");
    separation.setTooltip ("Higher splits similar sounds onto more pads");

    masterAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, "master", master);

    processor.addChangeListener (this);
    setResizable (true, true);
    setResizeLimits (700, 560, 1400, 1100);
    setSize (840, 680);

    refresh();
    selectPad (0);
    startTimerHz (30);
}

DrumkitCloneEditor::~DrumkitCloneEditor()
{
    processor.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

//==============================================================================
void DrumkitCloneEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);
    g.setColour (kText);
    g.setFont (uiFont (19.0f, true));
    g.drawText ("DRUMKIT", 16, 12, 90, 28, juce::Justification::centredLeft);
    g.setColour (kAccent);
    g.drawText ("CLONE", 104, 12, 80, 28, juce::Justification::centredLeft);
}

void DrumkitCloneEditor::resized()
{
    auto area = getLocalBounds().reduced (14, 10);

    auto header = area.removeFromTop (32);
    header.removeFromLeft (180);
    exportButton.setBounds (header.removeFromRight (118).reduced (0, 2));
    header.removeFromRight (8);
    loadButton.setBounds (header.removeFromRight (118).reduced (0, 2));
    header.removeFromRight (12);
    infoLabel.setBounds (header);

    area.removeFromTop (8);
    overview->setBounds (area.removeFromTop (juce::jmax (90, area.getHeight() / 5)));
    area.removeFromTop (8);

    auto row = [] (juce::Rectangle<int> r, juce::Label& label, juce::Slider& slider)
    {
        label.setBounds (r.removeFromLeft (84));
        slider.setBounds (r);
    };

    auto analysisRow = area.removeFromTop (26);
    reanalyseButton.setBounds (analysisRow.removeFromRight (110).reduced (0, 1));
    analysisRow.removeFromRight (14);
    row (analysisRow.removeFromLeft (analysisRow.getWidth() / 2).withTrimmedRight (10), sensitivityLabel, sensitivity);
    row (analysisRow, separationLabel, separation);
    area.removeFromTop (8);

    auto midiRow = area.removeFromTop (28);
    midiHandle->setBounds (midiRow.removeFromLeft (176));
    midiRow.removeFromLeft (16);
    const int unit = midiRow.getWidth() / 4;
    auto option = [unit, &midiRow] (juce::Label& label, juce::Component& c, int labelWidth)
    {
        auto r = midiRow.removeFromLeft (unit);
        label.setBounds (r.removeFromLeft (labelWidth));
        c.setBounds (r.withTrimmedRight (10).reduced (0, 2));
    };
    option (bpmLabel, bpm, 34);
    option (quantizeLabel, quantizeBox, 68);
    option (noteMapLabel, noteMapBox, 48);
    option (layoutLabel, layoutBox, 42);
    area.removeFromTop (8);

    auto padRow = area.removeFromBottom (58);
    selectedLabel.setBounds (padRow.removeFromTop (24));
    padRow.removeFromTop (4);
    const int w = padRow.getWidth() / 4;
    auto cell = [] (juce::Rectangle<int> r, juce::Label& label, juce::Slider& slider)
    {
        label.setBounds (r.removeFromLeft (52));
        slider.setBounds (r.withTrimmedRight (10));
    };
    cell (padRow.removeFromLeft (w), gainLabel, gain);
    cell (padRow.removeFromLeft (w), tuneLabel, tune);
    cell (padRow.removeFromLeft (w), panLabel, pan);
    cell (padRow, masterLabel, master);
    area.removeFromBottom (6);

    // 4x4 grid, pad 1 bottom-left like a drum machine
    const int cw = area.getWidth() / 4, ch = area.getHeight() / 4;
    for (int i = 0; i < kMaxPads; ++i)
        pads[i]->setBounds (area.getX() + (i % 4) * cw, area.getY() + (3 - i / 4) * ch, cw, ch);
}

//==============================================================================
void DrumkitCloneEditor::refresh()
{
    kit = processor.getKit();
    const bool hasKit = kit != nullptr && ! kit->pads.empty();

    if (hasKit)
        infoLabel.setText (kit->sourceName + "  -  " + juce::String (kit->totalHits) + " hits  -  "
                           + juce::String ((int) kit->pads.size()) + " pads", juce::dontSendNotification);
    else
        infoLabel.setText ({}, juce::dontSendNotification);

    exportButton.setEnabled (hasKit);
    midiHandle->setEnabled (hasKit);
    for (auto* c : std::initializer_list<juce::Component*> { &bpm, &quantizeBox, &noteMapBox, &layoutBox })
        c->setEnabled (hasKit);
    bpm.setValue (processor.getMidiBpm(), juce::dontSendNotification);
    bpm.setTooltip (hasKit ? "Tempo the MIDI is written at. Detected: " + juce::String (kit->bpm, 1) + " BPM" : juce::String());
    {
        const int q = processor.midiQuantize.load();
        quantizeBox.setSelectedItemIndex (q == 2 ? 1 : q == 4 ? 2 : q == 8 ? 3 : 0, juce::dontSendNotification);
        noteMapBox.setSelectedItemIndex (processor.midiNoteMap.load(), juce::dontSendNotification);
        layoutBox.setSelectedItemIndex (processor.midiLayout.load(), juce::dontSendNotification);
    }
    reanalyseButton.setEnabled (processor.canReanalyse() && ! processor.isAnalysing());
    sensitivity.setValue (processor.sensitivity.load() * 100.0, juce::dontSendNotification);
    separation.setValue (processor.separation.load() * 100.0, juce::dontSendNotification);

    selectPad (hasKit ? juce::jlimit (0, (int) kit->pads.size() - 1, selectedPad) : 0);
    repaint();
}

void DrumkitCloneEditor::selectPad (int pad)
{
    selectedPad = juce::jlimit (0, kMaxPads - 1, pad);
    const bool filled = kit != nullptr && selectedPad < (int) kit->pads.size();

    gainAttachment.reset();
    tuneAttachment.reset();
    panAttachment.reset();
    using Attachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    gainAttachment = std::make_unique<Attachment> (processor.apvts, DrumkitCloneProcessor::gainId (selectedPad), gain);
    tuneAttachment = std::make_unique<Attachment> (processor.apvts, DrumkitCloneProcessor::tuneId (selectedPad), tune);
    panAttachment = std::make_unique<Attachment> (processor.apvts, DrumkitCloneProcessor::panId (selectedPad), pan);

    for (auto* s : { &gain, &tune, &pan })
        s->setEnabled (filled);

    const auto note = juce::MidiMessage::getMidiNoteName (DrumkitCloneProcessor::firstNote + selectedPad, true, true, 3);
    selectedLabel.setText (filled ? "PAD " + juce::String (selectedPad + 1) + "  -  " + kit->pads[(size_t) selectedPad].name.toUpperCase()
                                        + "  -  " + note + "   (drag the pad for its WAV, its MIDI tag for its notes)"
                                  : juce::String ("PAD " + juce::String (selectedPad + 1) + "  -  empty"),
                           juce::dontSendNotification);
    selectedLabel.setColour (juce::Label::textColourId, filled ? padColour (selectedPad) : kDim);
    repaint();
}

void DrumkitCloneEditor::padPressed (int pad)
{
    selectPad (pad);
    processor.triggerPad (pad);
}

void DrumkitCloneEditor::padDraggedOut (int pad)
{
    if (draggingOut || kit == nullptr || pad >= (int) kit->pads.size())
        return;

    const auto& sample = kit->pads[(size_t) pad];
    auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Drumkit Clone");
    folder.createDirectory();
    const auto file = folder.getChildFile (juce::File::createLegalFileName (kit->sourceName + " - " + sample.name + ".wav"));
    if (! DrumkitCloneProcessor::writeWav (sample, file))
        return;

    draggingOut = true;
    juce::Component::SafePointer<DrumkitCloneEditor> safe (this);
    if (! performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this,
                                          [safe] { if (safe != nullptr) safe->draggingOut = false; }))
        draggingOut = false;
}

void DrumkitCloneEditor::dragMidiOut (int pad)
{
    if (draggingOut || kit == nullptr || kit->pads.empty())
        return;

    const auto files = processor.writeMidiForDrag (pad);
    if (files.isEmpty())
    {
        infoLabel.setText ("Couldn't write the MIDI files", juce::dontSendNotification);
        return;
    }

    draggingOut = true;
    juce::Component::SafePointer<DrumkitCloneEditor> safe (this);
    if (! performExternalDragDropOfFiles (files, false, this, [safe] { if (safe != nullptr) safe->draggingOut = false; }))
        draggingOut = false;
}

void DrumkitCloneEditor::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a loop, one-shot or song", juce::File(), kAudioWildcard);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult().existsAsFile())
                                  processor.loadFile (fc.getResult());
                          });
}

void DrumkitCloneEditor::exportKit()
{
    if (kit == nullptr || kit->pads.empty())
        return;

    chooser = std::make_unique<juce::FileChooser> ("Choose where to save the kit folder",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto dir = fc.getResult();
                              if (dir == juce::File() || kit == nullptr)
                                  return;
                              const auto folder = dir.getChildFile (juce::File::createLegalFileName (kit->sourceName + " Kit"));
                              juce::String error;
                              const bool ok = processor.exportKit (folder, error);
                              infoLabel.setText (ok ? "Saved to " + folder.getFullPathName() : error, juce::dontSendNotification);
                          });
}

//==============================================================================
bool DrumkitCloneEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    if (draggingOut)
        return false;
    for (auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;m4a;aac;wma;bwf"))
            return true;
    return false;
}

void DrumkitCloneEditor::fileDragEnter (const juce::StringArray&, int, int)    { fileHover = true; repaint(); }
void DrumkitCloneEditor::fileDragExit (const juce::StringArray&)               { fileHover = false; repaint(); }

void DrumkitCloneEditor::filesDropped (const juce::StringArray& files, int, int)
{
    fileHover = false;
    for (auto& f : files)
        if (juce::File (f).existsAsFile())
        {
            processor.loadFile (juce::File (f));
            break;
        }
    repaint();
}

void DrumkitCloneEditor::changeListenerCallback (juce::ChangeBroadcaster*)     { refresh(); }

void DrumkitCloneEditor::timerCallback()
{
    for (int i = 0; i < pads.size(); ++i)
        pads[i]->tick (processor.getPadTriggerCount (i));

    const bool analysing = processor.isAnalysing();
    if (analysing)
        overview->repaint();
    if (analysing != wasAnalysing)
    {
        wasAnalysing = analysing;
        refresh();
    }
}
