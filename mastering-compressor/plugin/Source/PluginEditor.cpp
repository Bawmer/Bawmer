#include "PluginEditor.h"

namespace lumen
{
// ---------------------------------------------------------------------------------------------
Knob::Knob (juce::AudioProcessorValueTreeState& s, const char* paramId, const juce::String& text, bool bipolar, bool cool)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 18);
    slider.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
    slider.setVelocityBasedMode (false);
    slider.setMouseDragSensitivity (300);
    slider.getProperties().set ("bipolar", bipolar);
    slider.getProperties().set ("cool", cool);
    slider.setPopupDisplayEnabled (false, false, nullptr);
    if (auto* p = s.getParameter (paramId))
        slider.setTooltip (p->getName (64));
    caption.setText (text, juce::dontSendNotification);
    caption.setJustificationType (juce::Justification::centred);
    caption.setFont (uiFont (11.5f, true));
    caption.setColour (juce::Label::textColourId, colours::textDim);
    caption.setInterceptsMouseClicks (false, false);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (s, paramId, slider);
    // double-click resets to the parameter default
    if (auto* p = s.getParameter (paramId))
        slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
}

void Knob::addTo (juce::Component& parent)
{
    parent.addAndMakeVisible (slider);
    parent.addAndMakeVisible (caption);
}

void Knob::setBounds (juce::Rectangle<int> r)
{
    caption.setBounds (r.removeFromTop (16));
    slider.setBounds (r);
}

void Knob::setEnabled (bool e)
{
    slider.setEnabled (e);
    caption.setAlpha (e ? 1.0f : 0.4f);
}

Toggle::Toggle (juce::AudioProcessorValueTreeState& s, const char* paramId, const juce::String& text, bool cool)
{
    button.setButtonText (text);
    button.getProperties().set ("cool", cool);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (s, paramId, button);
}

Choice::Choice (juce::AudioProcessorValueTreeState& s, const char* paramId, const juce::String& text)
{
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (s.getParameter (paramId)))
        box.addItemList (p->choices, 1);
    box.setJustificationType (juce::Justification::centredLeft);
    caption.setText (text, juce::dontSendNotification);
    caption.setFont (uiFont (10.5f, true));
    caption.setColour (juce::Label::textColourId, colours::textDim);
    caption.setJustificationType (juce::Justification::centredLeft);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (s, paramId, box);
}

void Choice::addTo (juce::Component& parent)
{
    parent.addAndMakeVisible (box);
    parent.addAndMakeVisible (caption);
}

void Choice::setBounds (juce::Rectangle<int> r)
{
    caption.setBounds (r.removeFromTop (14));
    box.setBounds (r.removeFromTop (24));
}

// ---------------------------------------------------------------------------------------------
MainPanel::MainPanel (LumenAudioProcessor& p)
    : proc (p), state (p.apvts),
      bypass (state, ids::bypass, "BYPASS"), matchBypass (state, ids::matchBypass, "MATCH LEVEL", true),
      oversampling (state, ids::oversampling, "OVERSAMPLING"), osQuality (state, ids::osQuality, "OS FILTER"),
      threshold (state, ids::threshold, "THRESHOLD"), ratio (state, ids::ratio, "RATIO"),
      attack (state, ids::attack, "ATTACK"), release (state, ids::release, "RELEASE"), knee (state, ids::knee, "KNEE"),
      lookahead (state, ids::lookahead, "LOOKAHEAD"), makeup (state, ids::makeup, "MAKEUP", true),
      mix (state, ids::mix, "MIX"), link (state, ids::link, "STEREO LINK"),
      scHpf (state, ids::scHpf, "SC HPF", false, true), scLpf (state, ids::scLpf, "SC LPF", false, true),
      scHpfOn (state, ids::scHpfOn, "HPF", true), scLpfOn (state, ids::scLpfOn, "LPF", true),
      extSc (state, ids::extSc, "EXT SC", true), autoRelease (state, ids::autoRelease, "AUTO REL", true),
      autoGain (state, ids::autoGain, "AUTO GAIN", true),
      detection (state, ids::detection, "DETECTION"), channelMode (state, ids::channelMode, "CHANNELS"),
      topology (state, ids::topology, "TOPOLOGY"), blend (state, ids::blend, "HYBRID BLEND"),
      drive (state, ids::drive, "DRIVE"), character (state, ids::character, "CHARACTER"),
      colorMix (state, ids::colorMix, "COLOUR MIX"),
      transformer (state, ids::transformer, "TRANSFORMER"), amplifier (state, ids::amplifier, "AMPLIFIER")
{
    const char* modeNames[] = { "ULTRA", "DIGITAL", "ANALOG", "HYBRID" };
    const char* modeTips[] = { "Ultra Transparent: no colouration, minimum processing, extra-smooth gain",
                               "Digital Transparent: exact, predictable digital ballistics",
                               "Analog Mastering: program-dependent timing, soft knee, optional colour",
                               "Hybrid: blend digital precision with analog envelopes and colour" };
    for (int i = 0; i < 4; ++i)
    {
        auto& b = modeButtons[i];
        b.setButtonText (modeNames[i]);
        b.setTooltip (modeTips[i]);
        b.setClickingTogglesState (false);
        b.onClick = [this, i] {
            if (auto* param = state.getParameter (ids::mode))
            {
                param->beginChangeGesture();
                param->setValueNotifyingHost (param->convertTo0to1 ((float) i));
                param->endChangeGesture();
            }
        };
        addAndMakeVisible (b);
    }

    addAndMakeVisible (presetBox);
    presetBox.setTextWhenNothingSelected ("Presets");
    presetBox.onChange = [this] {
        const int idx = presetBox.getSelectedItemIndex();
        if (idx >= 0 && idx != proc.presets.getCurrentIndex())
            proc.presets.loadPreset (idx);
    };
    loadPresetList();

    prevPreset.onClick = [this] {
        const int n = presetBox.getNumItems();
        if (n > 0) presetBox.setSelectedItemIndex ((presetBox.getSelectedItemIndex() - 1 + n) % n);
    };
    nextPreset.onClick = [this] {
        const int n = presetBox.getNumItems();
        if (n > 0) presetBox.setSelectedItemIndex ((presetBox.getSelectedItemIndex() + 1) % n);
    };
    savePreset.onClick = [this] { showSaveDialog(); };
    deletePreset.onClick = [this] {
        const int idx = presetBox.getSelectedItemIndex();
        if (proc.presets.isFactory (idx))
            return;
        proc.presets.deletePreset (idx);
        loadPresetList();
    };
    slotA.onClick = [this] { proc.switchToSlot (0); };
    slotB.onClick = [this] { proc.switchToSlot (1); };
    copyAB.onClick = [this] { proc.copyActiveToOther(); };
    undo.onClick = [this] { proc.undoManager.undo(); };
    redo.onClick = [this] { proc.undoManager.redo(); };
    monitorMode.onClick = [this] {
        showMidSide = ! showMidSide;
        monitorMode.setButtonText (showMidSide ? "M/S" : "L/R");
        inMeter.setLabels (showMidSide ? "M" : "L", showMidSide ? "S" : "R");
        outMeter.setLabels (showMidSide ? "M" : "L", showMidSide ? "S" : "R");
    };
    monitorMode.setTooltip ("Meter display: Left/Right or Mid/Side");
    copyAB.setTooltip ("Copy the active slot to the other slot");
    savePreset.setTooltip ("Save the current settings as a user preset");
    deletePreset.setTooltip ("Delete the selected user preset");

    for (auto* b : { &prevPreset, &nextPreset, &savePreset, &deletePreset, &slotA, &slotB, &copyAB, &undo, &redo, &monitorMode })
        addAndMakeVisible (*b);

    for (auto* t : { &bypass, &matchBypass, &scHpfOn, &scLpfOn, &extSc, &autoRelease, &autoGain, &transformer, &amplifier })
        addAndMakeVisible (t->button);
    bypass.button.setTooltip ("Compare with the unprocessed signal (latency-aligned)");
    matchBypass.button.setTooltip ("Loudness-matched bypass: bypassed signal is gain-matched to the processed output");
    extSc.button.setTooltip ("Use the external sidechain input for detection");
    autoRelease.button.setTooltip ("Program-dependent dual release");
    autoGain.button.setTooltip ("Automatic makeup gain from the long-term average gain reduction");

    for (auto* c : { &oversampling, &osQuality, &detection, &channelMode, &topology })
        c->addTo (*this);

    infoLabel.setFont (uiFont (11.5f));
    infoLabel.setColour (juce::Label::textColourId, colours::textDim);
    infoLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (infoLabel);

    for (auto* c : std::initializer_list<juce::Component*> { &inMeter, &outMeter, &grMeter, &curve, &history })
        addAndMakeVisible (c);

    for (auto* k : { &threshold, &ratio, &attack, &release, &knee, &lookahead, &makeup, &mix, &link, &scHpf, &scLpf, &blend,
                     &drive, &character, &colorMix })
        k->addTo (*this);

    setSize (kWidth, kHeight);
    refresh();
}

void MainPanel::loadPresetList()
{
    proc.presets.refreshUserPresets();
    presetBox.clear (juce::dontSendNotification);
    const auto names = proc.presets.getPresetNames();
    const int numFactory = proc.presets.getNumFactoryPresets();
    for (int i = 0; i < names.size(); ++i)
    {
        if (i == numFactory)
            presetBox.addSeparator();
        presetBox.addItem ((i < numFactory ? "" : "* ") + names[i], i + 1);
    }
    presetBox.setSelectedItemIndex (proc.presets.getCurrentIndex(), juce::dontSendNotification);
}

void MainPanel::showSaveDialog()
{
    auto* w = new juce::AlertWindow ("Save Preset", "Name for the user preset:", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", proc.presets.isFactory (proc.presets.getCurrentIndex()) ? "My Preset" : proc.presets.getCurrentName());
    w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainPanel> safe (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int result) {
                            if (result == 1 && safe != nullptr)
                            {
                                safe->proc.presets.savePreset (w->getTextEditorContents ("name"));
                                safe->loadPresetList();
                            }
                        }),
                        true);
}

void MainPanel::updateModeButtons()
{
    const int mode = juce::roundToInt (state.getRawParameterValue (ids::mode)->load());
    for (int i = 0; i < 4; ++i)
        modeButtons[i].setToggleState (i == mode, juce::dontSendNotification);
    slotA.setToggleState (proc.getActiveSlot() == 0, juce::dontSendNotification);
    slotB.setToggleState (proc.getActiveSlot() == 1, juce::dontSendNotification);
    undo.setEnabled (proc.undoManager.canUndo());
    redo.setEnabled (proc.undoManager.canRedo());
}

void MainPanel::updateEnablement()
{
    const int mode = juce::roundToInt (state.getRawParameterValue (ids::mode)->load());
    const bool analogish = mode >= 2;
    const bool colourOn = analogish && (transformer.button.getToggleState() || amplifier.button.getToggleState());
    topology.box.setEnabled (analogish && ! extSc.button.getToggleState());
    blend.setEnabled (mode == 3);
    drive.setEnabled (colourOn);
    character.setEnabled (colourOn && amplifier.button.getToggleState());
    colorMix.setEnabled (colourOn);
    transformer.button.setEnabled (analogish);
    amplifier.button.setEnabled (analogish);
    oversampling.box.setEnabled (colourOn);
    osQuality.box.setEnabled (colourOn);
    scHpf.setEnabled (scHpfOn.button.getToggleState());
    scLpf.setEnabled (scLpfOn.button.getToggleState());
    link.setEnabled (juce::roundToInt (state.getRawParameterValue (ids::channelMode)->load()) != 1);
}

void MainPanel::refresh()
{
    updateModeButtons();
    updateEnablement();

    auto& m = proc.meters;
    const float inA = MeterBus::take (m.inPeak[0]), inB = MeterBus::take (m.inPeak[1]);
    const float outA = MeterBus::take (m.outPeak[0]), outB = MeterBus::take (m.outPeak[1]);
    const float inM = MeterBus::take (m.inMid), inS = MeterBus::take (m.inSide);
    const float outM = MeterBus::take (m.outMid), outS = MeterBus::take (m.outSide);
    const float grA = MeterBus::take (m.gr[0]), grB = MeterBus::take (m.gr[1]);
    const float det = MeterBus::take (m.detectorDb, -120.0f);

    inMeter.push (showMidSide ? inM : inA, showMidSide ? inS : inB);
    outMeter.push (showMidSide ? outM : outA, showMidSide ? outS : outB);
    const bool msProc = juce::roundToInt (state.getRawParameterValue (ids::channelMode)->load()) == 2;
    grMeter.setLabels (msProc ? "M" : "L", msProc ? "S" : "R");
    grMeter.push (grA, grB);

    const int mode = juce::roundToInt (state.getRawParameterValue (ids::mode)->load());
    const double kneeDb = state.getRawParameterValue (ids::knee)->load();
    curve.setCurve (state.getRawParameterValue (ids::threshold)->load(), state.getRawParameterValue (ids::ratio)->load(),
                    mode == 2 ? CompressorEngine::analogKnee (kneeDb) : kneeDb,
                    state.getRawParameterValue (ids::makeup)->load() + m.autoGainDb.load());
    curve.setOperatingPoint (det, juce::jmax (grA, grB));

    // gain-reduction history
    const int w = proc.history.writeIndex.load (std::memory_order_acquire);
    if (w - historyReadIndex > GrHistory::kSize)
        historyReadIndex = w - GrHistory::kSize;
    while (historyReadIndex < w)
    {
        const int i = historyReadIndex++ % GrHistory::kSize;
        history.push (proc.history.gr[i], proc.history.level[i]);
    }
    history.repaint();

    const double sr = proc.getCurrentSampleRate();
    const int lat = proc.getReportedLatency();
    juce::String info;
    info << "64-bit  |  " << juce::String (sr / 1000.0, 1) << " kHz  |  latency " << lat << " smp ("
         << juce::String (1000.0 * lat / juce::jmax (1.0, sr), 2) << " ms)";
    if (autoGain.button.getToggleState())
        info << "  |  auto gain " << juce::String (m.autoGainDb.load(), 1) << " dB";
    if (bypass.button.getToggleState() && matchBypass.button.getToggleState())
        info << "  |  match " << juce::String (m.matchDb.load(), 1) << " dB";
    infoLabel.setText (info, juce::dontSendNotification);
}

void MainPanel::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);

    // header: brushed metal
    auto header = getLocalBounds().removeFromTop (62);
    metal.draw (g, header, renderScale);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawHorizontalLine (header.getBottom() - 1, 0.0f, (float) getWidth());

    g.setFont (uiFont (24.0f, true));
    g.setColour (juce::Colours::black.withAlpha (0.5f)); // engraved: dark offset + light face
    g.drawText ("LUMEN", juce::Rectangle<int> (22, 11, 110, 26), juce::Justification::centredLeft);
    g.setColour (juce::Colour (0xffe8eaed));
    g.drawText ("LUMEN", juce::Rectangle<int> (22, 10, 110, 26), juce::Justification::centredLeft);
    g.setColour (colours::accent.darker (0.2f));
    g.drawText ("MC-64", juce::Rectangle<int> (110, 10, 90, 26), juce::Justification::centredLeft);
    g.setColour (juce::Colour (0xffb4b9c0));
    g.setFont (uiFont (10.0f, true));
    g.drawText ("64-BIT MASTERING COMPRESSOR", juce::Rectangle<int> (23, 36, 220, 14), juce::Justification::centredLeft);

    // section panels
    auto drawPanel = [&] (juce::Rectangle<int> r, const juce::String& title) {
        auto rf = r.toFloat();
        juce::ColourGradient grad (colours::panel.brighter (0.03f), 0.0f, rf.getY(), colours::panel.darker (0.08f), 0.0f,
                                   rf.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (rf, 7.0f);
        g.setColour (colours::panelEdge);
        g.drawRoundedRectangle (rf.reduced (0.5f), 7.0f, 1.0f);
        if (title.isNotEmpty())
        {
            g.setColour (colours::textDim.withAlpha (0.8f));
            g.setFont (uiFont (10.5f, true));
            g.drawText (title, r.reduced (12, 6).removeFromTop (12), juce::Justification::topLeft);
        }
    };
    drawPanel (mainSection, "");
    drawPanel (scSection, "SIDECHAIN  /  DETECTOR");
    drawPanel (analogSection, "ANALOG  /  HYBRID");
}

void MainPanel::resized()
{
    // header (62 px)
    {
        auto r = juce::Rectangle<int> (250, 15, 380, 32);
        for (auto& b : modeButtons)
            b.setBounds (r.removeFromLeft (92).reduced (2, 0));

        auto right = juce::Rectangle<int> (650, 15, kWidth - 650 - 16, 32);
        redo.setBounds (right.removeFromRight (50));
        right.removeFromRight (4);
        undo.setBounds (right.removeFromRight (50));
        right.removeFromRight (10);
        copyAB.setBounds (right.removeFromRight (42));
        right.removeFromRight (4);
        slotB.setBounds (right.removeFromRight (30));
        slotA.setBounds (right.removeFromRight (30));
        right.removeFromRight (10);
        deletePreset.setBounds (right.removeFromRight (40));
        right.removeFromRight (3);
        savePreset.setBounds (right.removeFromRight (44));
        right.removeFromRight (3);
        nextPreset.setBounds (right.removeFromRight (24));
        prevPreset.setBounds (right.removeFromRight (24));
        right.removeFromRight (3);
        presetBox.setBounds (right);
    }

    // strip (y 70..110)
    {
        bypass.button.setBounds (16, 80, 92, 26);
        matchBypass.button.setBounds (114, 80, 120, 26);
        monitorMode.setBounds (240, 80, 50, 26);
        oversampling.setBounds ({ 304, 68, 92, 40 });
        osQuality.setBounds ({ 404, 68, 132, 40 });
        infoLabel.setBounds (540, 80, kWidth - 540 - 16, 26);
    }

    // displays (y 118..410)
    {
        auto r = juce::Rectangle<int> (16, 118, kWidth - 32, 292);
        inMeter.setBounds (r.removeFromLeft (70));
        r.removeFromLeft (10);
        outMeter.setBounds (r.removeFromRight (70));
        r.removeFromRight (8);
        grMeter.setBounds (r.removeFromRight (62));
        r.removeFromRight (10);
        curve.setBounds (r.removeFromLeft (310).withTrimmedTop (16).withTrimmedBottom (32));
        r.removeFromLeft (10);
        history.setBounds (r.withTrimmedTop (16).withTrimmedBottom (32));
    }

    // main knob row (y 420..560)
    mainSection = { 16, 418, kWidth - 32, 140 };
    {
        auto r = mainSection.reduced (8, 10);
        Knob* knobs[] = { &threshold, &ratio, &attack, &release, &knee, &lookahead, &makeup, &mix, &link };
        const int w = r.getWidth() / 9;
        for (auto* k : knobs)
            k->setBounds (r.removeFromLeft (w).reduced (6, 0));
    }

    // lower sections (y 568..744)
    scSection = { 16, 568, 560, 176 };
    analogSection = { 588, 568, kWidth - 588 - 16, 176 };
    {
        auto r = scSection.reduced (12, 8).withTrimmedTop (16);
        auto knobsArea = r.removeFromLeft (220);
        auto hp = knobsArea.removeFromLeft (110);
        scHpfOn.button.setBounds (hp.removeFromBottom (24).reduced (14, 0));
        scHpf.setBounds (hp.reduced (4, 0));
        auto lp = knobsArea;
        scLpfOn.button.setBounds (lp.removeFromBottom (24).reduced (14, 0));
        scLpf.setBounds (lp.reduced (4, 0));

        r.removeFromLeft (10);
        auto col1 = r.removeFromLeft (150);
        detection.setBounds (col1.removeFromTop (44));
        col1.removeFromTop (6);
        channelMode.setBounds (col1.removeFromTop (44));
        col1.removeFromTop (8);
        extSc.button.setBounds (col1.removeFromTop (26));

        r.removeFromLeft (12);
        auto col2 = r;
        col2.removeFromTop (14);
        autoRelease.button.setBounds (col2.removeFromTop (26));
        col2.removeFromTop (10);
        autoGain.button.setBounds (col2.removeFromTop (26));
    }
    {
        auto r = analogSection.reduced (12, 8).withTrimmedTop (16);
        auto col = r.removeFromLeft (130);
        topology.setBounds (col.removeFromTop (44));
        col.removeFromTop (10);
        transformer.button.setBounds (col.removeFromTop (26));
        col.removeFromTop (8);
        amplifier.button.setBounds (col.removeFromTop (26));
        r.removeFromLeft (8);
        const int w = r.getWidth() / 4;
        for (auto* k : { &blend, &drive, &character, &colorMix })
            k->setBounds (r.removeFromLeft (w).reduced (4, 6));
    }
}

// ---------------------------------------------------------------------------------------------
LumenAudioProcessorEditor::LumenAudioProcessorEditor (LumenAudioProcessor& p)
    : AudioProcessorEditor (p), proc (p), panel (p)
{
    setLookAndFeel (&lnf);
    panel.setLookAndFeel (&lnf);
    addAndMakeVisible (panel);
    addMouseListener (this, true);
    setWantsKeyboardFocus (true);

    setResizable (true, true);
    setResizeLimits (MainPanel::kWidth * 6 / 10, MainPanel::kHeight * 6 / 10, MainPanel::kWidth * 2, MainPanel::kHeight * 2);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) MainPanel::kWidth / MainPanel::kHeight);
    setSize (MainPanel::kWidth, MainPanel::kHeight);
    startTimerHz (30);
}

LumenAudioProcessorEditor::~LumenAudioProcessorEditor()
{
    stopTimer();
    panel.setLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void LumenAudioProcessorEditor::paint (juce::Graphics& g) { g.fillAll (colours::background); }

void LumenAudioProcessorEditor::resized()
{
    const float scale = juce::jmin ((float) getWidth() / MainPanel::kWidth, (float) getHeight() / MainPanel::kHeight);
    panel.setTransform (juce::AffineTransform::scale (scale));
    panel.setRenderScale (scale * (float) juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->scale);
    panel.setBounds (0, 0, MainPanel::kWidth, MainPanel::kHeight);
}

void LumenAudioProcessorEditor::mouseDown (const juce::MouseEvent&)
{
    // each click-drag gesture becomes one undoable step
    proc.undoManager.beginNewTransaction();
}

bool LumenAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (mods.isCommandDown() && key.getKeyCode() == 'Z')
    {
        if (mods.isShiftDown()) proc.undoManager.redo();
        else proc.undoManager.undo();
        return true;
    }
    if (mods.isCommandDown() && key.getKeyCode() == 'Y')
    {
        proc.undoManager.redo();
        return true;
    }
    return false;
}

void LumenAudioProcessorEditor::timerCallback() { panel.refresh(); }
} // namespace lumen
