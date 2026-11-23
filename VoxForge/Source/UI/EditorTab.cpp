#include "EditorTab.h"
#include "../PluginProcessor.h"
#include "../Core/Lyrics.h"

namespace vf::ui
{

EditorTab::EditorTab (VoxForgeProcessor& p) : proc (p), roll (p)
{
    viewport.setViewedComponent (&roll, false);
    viewport.setScrollBarsShown (true, true);
    addAndMakeVisible (viewport);
    roll.onChanged = [this] { onNotesEdited(); };
    roll.onSelection = [this] (int i) { showSelection (i); };

    for (auto* b : { &captureBtn, &importBtn, &quantizeBtn, &deleteBtn, &previewBtn, &stopBtn, &renderBtn, &exportBtn, &bpmBtn })
        addAndMakeVisible (*b);
    captureBtn.setClickingTogglesState (true);
    captureBtn.setTooltip ("Arm, then press Play in your DAW: the MIDI notes of this track are recorded into the editor.");
    captureBtn.onClick = [this] { proc.setCaptureArmed (captureBtn.getToggleState()); };
    importBtn.onClick = [this] { importMidi(); };
    quantizeBtn.onClick = [this] { roll.quantizeSelected(); };
    deleteBtn.onClick = [this] { roll.deleteSelected(); };
    previewBtn.onClick = [this] { proc.startPreview (selected >= 0 ? proc.project().notes[(size_t) selected].startBeat : 0.0); };
    stopBtn.onClick = [this] { proc.stopPreview(); };
    renderBtn.onClick = [this] { proc.renderNow(); };
    renderBtn.setColour (juce::TextButton::buttonColourId, Colours::accent);
    exportBtn.onClick = [this] { exportWav(); };
    bpmBtn.setTooltip ("Re-time the project to the current DAW tempo (keeps notes on the same beats).");
    bpmBtn.onClick = [this] { proc.project().bpm = proc.hostBpm(); proc.projectChanged(); };

    addAndMakeVisible (autoRenderToggle);
    autoRenderToggle.setToggleState (proc.autoRender, juce::dontSendNotification);
    autoRenderToggle.onClick = [this] { proc.autoRender = autoRenderToggle.getToggleState(); };

    gridBox.addItemList ({ "Grid 1/4", "Grid 1/8", "Grid 1/16", "Grid 1/32", "Grid off" }, 1);
    gridBox.setSelectedId (3, juce::dontSendNotification);
    gridBox.onChange = [this]
    {
        static const double g[] = { 1.0, 0.5, 0.25, 0.125, 0.0 };
        roll.setGrid (g[juce::jlimit (0, 4, gridBox.getSelectedId() - 1)]);
    };
    addAndMakeVisible (gridBox);

    zoomSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    zoomSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    zoomSlider.setRange (12.0, 300.0);
    zoomSlider.setSkewFactorFromMidPoint (70.0);
    zoomSlider.setValue (roll.getZoom(), juce::dontSendNotification);
    zoomSlider.onValueChange = [this] { roll.setZoom (zoomSlider.getValue()); };
    addAndMakeVisible (zoomSlider);
    zoomLabel.setText ("Zoom", juce::dontSendNotification);
    styleDim (zoomLabel, 12.0f);
    addAndMakeVisible (zoomLabel);
    styleDim (bpmLabel, 12.0f);
    addAndMakeVisible (bpmLabel);

    // ---- lyrics -------------------------------------------------------------------
    addAndMakeVisible (lyricsSection);
    lyricsEditor.setMultiLine (true, true);
    lyricsEditor.setReturnKeyStartsNewLine (true);
    lyricsEditor.setFont (juce::Font (juce::FontOptions (15.0f)));
    lyricsEditor.setText (proc.project().lyricsText, false);
    lyricsEditor.setTextToShowWhenEmpty ("Type or paste lyrics here, e.g.  twin-kle twin-kle lit-tle star - -", Colours::dim);
    addAndMakeVisible (lyricsEditor);
    addAndMakeVisible (applyLyricsBtn);
    addAndMakeVisible (applyFromSelBtn);
    applyLyricsBtn.setColour (juce::TextButton::buttonColourId, Colours::accent);
    applyLyricsBtn.onClick = [this]
    {
        proc.project().lyricsText = lyricsEditor.getText();
        applyLyricsToNotes (proc.project().notes, lyricsEditor.getText(), 0);
        onNotesEdited();
    };
    applyFromSelBtn.onClick = [this]
    {
        proc.project().lyricsText = lyricsEditor.getText();
        applyLyricsToNotes (proc.project().notes, lyricsEditor.getText(), juce::jmax (0, selected));
        onNotesEdited();
    };
    lyricsHelp.setText ("One syllable per note.  hap-py = split syllables   -  = hold vowel (melisma)   [hh ax l ow] = phonemes.  "
                        "Double-click a note to type its syllable, Return jumps to the next note.", juce::dontSendNotification);
    styleDim (lyricsHelp, 12.0f);
    addAndMakeVisible (lyricsHelp);

    styleDim (selectionLabel, 12.5f);
    addAndMakeVisible (selectionLabel);
    phonemeEditor.setTextToShowWhenEmpty ("phonemes (ARPAbet) for the selected note", Colours::dim);
    addAndMakeVisible (phonemeEditor);
    addAndMakeVisible (phonemeApplyBtn);
    phonemeApplyBtn.onClick = [this]
    {
        if (selected < 0 || selected >= (int) proc.project().notes.size()) return;
        proc.project().notes[(size_t) selected].phonemes = phonemeEditor.getText().trim();
        onNotesEdited();
    };

    // ---- expression knobs -------------------------------------------------------------
    addAndMakeVisible (expressionSection);
    const std::pair<const char*, const char*> ks[] = {
        { ParamIDs::vibDepth, "Vibrato" }, { ParamIDs::vibRate, "Vib. Rate" }, { ParamIDs::vibDelay, "Vib. Delay" },
        { ParamIDs::breath, "Breathiness" }, { ParamIDs::bendRange, "Bend Range" }, { ParamIDs::porta, "Portamento" },
        { ParamIDs::expression, "Expression" }, { ParamIDs::tension, "Tension" }, { ParamIDs::formant, "Formant" },
        { ParamIDs::humanize, "Humanize" }, { ParamIDs::gain, "Output" } };
    for (auto& k : ks)
    {
        knobs.push_back (std::make_unique<Knob> (proc.apvts, k.first, k.second));
        addAndMakeVisible (*knobs.back());
    }
    addAndMakeVisible (liveToggle);
    liveToggle.setTooltip ("Play the voice in real time from incoming MIDI (monitoring). The rendered take is used during DAW playback.");
    liveAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, ParamIDs::live, liveToggle);

    lyricsSection.toBack();
    expressionSection.toBack();

    juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<EditorTab> (this)]
    {
        if (sp == nullptr) return;
        int lo = 127;
        for (auto& n : sp->proc.project().notes) lo = juce::jmin (lo, n.pitch);
        sp->roll.centreOnPitch (lo < 127 ? lo + 6 : 64);
    });
}

void EditorTab::paint (juce::Graphics& g) { g.fillAll (Colours::bg); }

void EditorTab::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto bar = r.removeFromTop (34);
    auto place = [&bar] (juce::Component& c, int w) { c.setBounds (bar.removeFromLeft (w).reduced (2)); bar.removeFromLeft (4); };
    place (captureBtn, 118); place (importBtn, 100); place (gridBox, 100); place (quantizeBtn, 80); place (deleteBtn, 70);
    zoomLabel.setBounds (bar.removeFromLeft (40)); place (zoomSlider, 110);
    exportBtn.setBounds (bar.removeFromRight (100).reduced (2)); bar.removeFromRight (4);
    renderBtn.setBounds (bar.removeFromRight (90).reduced (2)); bar.removeFromRight (4);
    autoRenderToggle.setBounds (bar.removeFromRight (104).reduced (2)); bar.removeFromRight (4);
    stopBtn.setBounds (bar.removeFromRight (56).reduced (2)); bar.removeFromRight (2);
    previewBtn.setBounds (bar.removeFromRight (56).reduced (2));
    r.removeFromTop (6);

    auto right = r.removeFromRight (250);
    r.removeFromRight (8);
    expressionSection.setBounds (right);
    auto ec = expressionSection.getBounds().reduced (10).withTrimmedTop (26);
    auto bottom = ec.removeFromBottom (64);
    liveToggle.setBounds (bottom.removeFromTop (26));
    bpmBtn.setBounds (bottom.removeFromTop (30).reduced (0, 2).withWidth (130));
    bpmLabel.setBounds (bpmBtn.getRight() + 6, bpmBtn.getY(), ec.getRight() - bpmBtn.getRight() - 6, bpmBtn.getHeight());
    const int cols = 3;
    const int rows = ((int) knobs.size() + cols - 1) / cols;
    const int kh = juce::jmin (118, ec.getHeight() / juce::jmax (1, rows));
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        const int c = (int) i % cols, row = (int) i / cols;
        knobs[i]->setBounds (ec.getX() + c * ec.getWidth() / cols, ec.getY() + row * kh, ec.getWidth() / cols, kh);
    }

    auto lyr = r.removeFromBottom (170);
    r.removeFromBottom (8);
    viewport.setBounds (r);
    roll.updateSize();

    lyricsSection.setBounds (lyr);
    auto lc = lyricsSection.getBounds().reduced (12).withTrimmedTop (22);
    auto selRow = lc.removeFromBottom (28);
    selectionLabel.setBounds (selRow.removeFromLeft (selRow.getWidth() / 2));
    phonemeApplyBtn.setBounds (selRow.removeFromRight (50).reduced (2));
    phonemeEditor.setBounds (selRow.reduced (2));
    lyricsHelp.setBounds (lc.removeFromBottom (20));
    auto btns = lc.removeFromRight (170);
    applyLyricsBtn.setBounds (btns.removeFromTop (34).reduced (2));
    applyFromSelBtn.setBounds (btns.removeFromTop (34).reduced (2));
    lyricsEditor.setBounds (lc.reduced (0, 2).withTrimmedRight (8));
}

void EditorTab::onNotesEdited()
{
    proc.project().lyricsText = lyricsEditor.getText();
    proc.projectChanged();
    lastVersion = proc.projectVersion();
    roll.updateSize();
    roll.repaint();
    showSelection (roll.firstSelected());
}

void EditorTab::showSelection (int i)
{
    selected = i;
    auto& notes = proc.project().notes;
    if (i < 0 || i >= (int) notes.size())
    {
        selectionLabel.setText ("No note selected  (" + juce::String ((int) notes.size()) + " notes)", juce::dontSendNotification);
        phonemeEditor.setText ({}, false);
        return;
    }
    const auto& n = notes[(size_t) i];
    auto ph = phonemesForNote (n);
    juce::String info = juce::MidiMessage::getMidiNoteName (n.pitch, true, true, 4) + "  \"" + n.lyric + "\"";
    if (n.fromSource) info << (n.lyricChangedFromSource() ? "  (new word - synthesised)" : "  (original articulation)");
    selectionLabel.setText (info, juce::dontSendNotification);
    phonemeEditor.setText (ph.joinIntoString (" "), false);
}

void EditorTab::refresh()
{
    lyricsEditor.setText (proc.project().lyricsText, false);
    roll.updateSize();
    roll.repaint();
    showSelection (roll.firstSelected());
    lastVersion = proc.projectVersion();
}

void EditorTab::tick()
{
    if (proc.projectVersion() != lastVersion) refresh();
    if (captureBtn.getToggleState() != proc.isCaptureArmed())
        captureBtn.setToggleState (proc.isCaptureArmed(), juce::dontSendNotification);
    captureBtn.setButtonText (proc.isCapturing() ? "Recording..." : "Capture MIDI");

    const double host = proc.hostBpm(), prjBpm = proc.project().bpm;
    const bool mismatch = std::abs (host - prjBpm) > 0.01;
    bpmLabel.setText ("Project " + juce::String (prjBpm, 1) + " bpm" + (mismatch ? " / DAW " + juce::String (host, 1) : ""),
                      juce::dontSendNotification);
    bpmLabel.setColour (juce::Label::textColourId, mismatch ? Colours::warn : Colours::dim);

    if (proc.hostIsPlaying() || proc.isPreviewing()) roll.repaint();
}

void EditorTab::importMidi()
{
    chooser = std::make_unique<juce::FileChooser> ("Import MIDI melody", juce::File(), "*.mid;*.midi");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (! f.existsAsFile()) return;
        juce::String err;
        if (! proc.importMidiFile (f, err))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Import MIDI", err);
        refresh();
    });
}

void EditorTab::exportWav()
{
    if (! proc.hasTake()) { proc.lastMessage = "Render first, then export."; return; }
    chooser = std::make_unique<juce::FileChooser> ("Export rendered vocal", juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("VoxForge vocal.wav"), "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        proc.lastMessage = proc.exportTake (f.withFileExtension ("wav")) ? "Exported " + f.getFileName() : "Export failed";
    });
}

bool EditorTab::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files) if (f.endsWithIgnoreCase (".mid") || f.endsWithIgnoreCase (".midi") || f.endsWithIgnoreCase (".txt")) return true;
    return false;
}

void EditorTab::filesDropped (const juce::StringArray& files, int, int)
{
    for (auto& path : files)
    {
        juce::File f (path);
        if (path.endsWithIgnoreCase (".txt"))
        {
            lyricsEditor.setText (f.loadFileAsString(), false);
            proc.project().lyricsText = lyricsEditor.getText();
        }
        else
        {
            juce::String err;
            if (! proc.importMidiFile (f, err))
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Import MIDI", err);
        }
    }
    refresh();
}

} // namespace vf::ui
