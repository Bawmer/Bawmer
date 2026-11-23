#include "ReplaceTab.h"
#include "../PluginProcessor.h"

namespace vf::ui
{

void StemView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff111218));
    g.fillRoundedRectangle (r, 6.0f);
    auto* stem = proc.stemPreview();
    if (stem == nullptr)
    {
        g.setColour (Colours::dim);
        g.drawText ("Drop a dry lead-vocal stem (WAV / FLAC / AIFF) here", getLocalBounds(), juce::Justification::centred);
        return;
    }
    const int w = juce::jmax (1, getWidth());
    if (! cacheValid || (int) peaks.size() != w)
    {
        peaks.assign ((size_t) w, 0.0f);
        const float* x = stem->getReadPointer (0);
        const int n = stem->getNumSamples();
        for (int i = 0; i < w; ++i)
        {
            const int a = (int) ((juce::int64) n * i / w), b = (int) ((juce::int64) n * (i + 1) / w);
            float m = 0.0f;
            for (int k = a; k < b; k += 4) m = juce::jmax (m, std::abs (x[k]));
            peaks[(size_t) i] = m;
        }
        cacheValid = true;
    }
    const float mid = r.getCentreY(), hh = r.getHeight() * 0.45f;
    g.setColour (Colours::accent2.withAlpha (0.7f));
    for (int i = 0; i < w; ++i)
        g.drawVerticalLine (i, mid - peaks[(size_t) i] * hh, mid + peaks[(size_t) i] * hh);

    // detected notes from the source
    auto& prj = proc.project();
    const double total = stem->getNumSamples() / kEngineSampleRate;
    g.setColour (Colours::accent.withAlpha (0.55f));
    for (auto& n : prj.notes)
    {
        if (! n.fromSource) continue;
        const float x0 = (float) (n.srcStartSec / total * w), x1 = (float) (n.srcEndSec / total * w);
        g.fillRect (x0, r.getBottom() - 6.0f, juce::jmax (1.0f, x1 - x0), 4.0f);
    }
}

ReplaceTab::ReplaceTab (VoxForgeProcessor& p) : proc (p), stemView (p)
{
    for (auto* s : { &s1, &s2, &s3, &s4 }) addAndMakeVisible (*s);
    addAndMakeVisible (stemView);
    for (auto* b : { &loadBtn, &analyseBtn, &cancelBtn, &openEditorBtn, &renderBtn, &exportBtn }) addAndMakeVisible (*b);
    for (auto* l : { &fileLabel, &offsetLabel, &sensLabel, &minLabel, &splitLabel, &resultLabel, &s3Text, &voiceLabel, &nuanceLabel, &monitorLabel, &hint })
        addAndMakeVisible (*l);
    styleDim (fileLabel, 12.5f);
    styleDim (resultLabel, 13.0f);
    styleDim (s3Text, 12.5f);
    styleDim (hint, 12.0f);
    resultLabel.setJustificationType (juce::Justification::topLeft);
    s3Text.setJustificationType (juce::Justification::topLeft);
    hint.setJustificationType (juce::Justification::topLeft);

    loadBtn.setColour (juce::TextButton::buttonColourId, Colours::accent);
    loadBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Load vocal stem", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) loadStem (fc.getResult()); });
    };

    offsetLabel.setText ("Stem starts at beat", juce::dontSendNotification);
    offsetSlider.setRange (0.0, 2048.0, 0.25);
    offsetSlider.setSliderStyle (juce::Slider::IncDecButtons);
    offsetSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 70, 22);
    offsetSlider.setValue (proc.project().stemOffsetBeats, juce::dontSendNotification);
    offsetSlider.setTooltip ("Where the stem starts on the DAW timeline, in beats (0 = bar 1). Place the plug-in's MIDI/instrument track so the stem lines up.");
    offsetSlider.onValueChange = [this]
    {
        proc.project().stemOffsetBeats = offsetSlider.getValue();
        // keep notes attached to the audio
        proc.projectChanged();
    };
    addAndMakeVisible (offsetSlider);

    auto setupSlider = [this] (juce::Slider& s, juce::Label& l, const juce::String& name, double lo, double hi, double v, const juce::String& suffix)
    {
        l.setText (name, juce::dontSendNotification);
        s.setRange (lo, hi, (hi - lo) / 100.0);
        s.setValue (v, juce::dontSendNotification);
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
        s.setTextValueSuffix (suffix);
        addAndMakeVisible (s);
    };
    setupSlider (sensSlider, sensLabel, "Sensitivity", 0.0, 1.0, 0.5, "");
    setupSlider (minNoteSlider, minLabel, "Min note length", 40.0, 300.0, 90.0, " ms");
    setupSlider (splitSlider, splitLabel, "Split on pitch change", 0.3, 2.0, 0.75, " st");
    lyricsToggle.setToggleState (true, juce::dontSendNotification);
    addAndMakeVisible (lyricsToggle);
    addAndMakeVisible (analysisBar);
    analyseBtn.setColour (juce::TextButton::buttonColourId, Colours::accent);
    analyseBtn.onClick = [this]
    {
        VocalToMidiOptions o;
        o.sensitivity = (float) sensSlider.getValue();
        o.minNoteMs = (float) minNoteSlider.getValue();
        o.splitSemitones = (float) splitSlider.getValue();
        proc.analyseStem (o, lyricsToggle.getToggleState());
    };
    cancelBtn.onClick = [this] { proc.stemAnalyzer().cancel(); };

    s3Text.setText ("Detected notes land in the MIDI-Lyrics Editor. Move / retune notes freely. Teal notes keep the original "
                    "articulation of the stem; type a new syllable on a note (double-click) and it turns orange: that word is "
                    "re-sung by the synthesiser in your voice. '-' extends the previous vowel.", juce::dontSendNotification);
    openEditorBtn.onClick = [this] { if (openEditor) openEditor(); };

    nuanceLabel.setText ("Keep original nuance", juce::dontSendNotification);
    nuanceSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    nuanceSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
    nuanceSlider.setTooltip ("How much of the original pitch movement (vibrato, scoops, bends) is kept on top of the edited melody.");
    addAndMakeVisible (nuanceSlider);
    nuanceAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, ParamIDs::nuance, nuanceSlider);
    monitorLabel.setText ("Monitor", juce::dontSendNotification);
    monitorBox.addItemList ({ "Rendered", "Original Stem", "Both" }, 1);
    addAndMakeVisible (monitorBox);
    monitorAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, ParamIDs::monitor, monitorBox);
    renderBtn.setColour (juce::TextButton::buttonColourId, Colours::accent);
    renderBtn.onClick = [this] { proc.renderNow(); };
    exportBtn.onClick = [this]
    {
        if (! proc.hasTake()) { proc.lastMessage = "Render first."; return; }
        chooser = std::make_unique<juce::FileChooser> ("Export replaced vocal",
            juce::File (proc.project().stemPath).getSiblingFile (juce::File (proc.project().stemPath).getFileNameWithoutExtension() + " (VoxForge).wav"), "*.wav");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            proc.lastMessage = proc.exportTake (f.withFileExtension ("wav")) ? "Exported " + f.getFileName() : "Export failed";
        });
    };
    addAndMakeVisible (renderBar);
    hint.setText ("Tip: mute the original vocal track in your DAW and put VoxForge on an instrument track; use Monitor = Both to A/B. "
                  "The exported WAV starts at beat 0 of the project, ready to drag back into the DAW.", juce::dontSendNotification);
}

void ReplaceTab::paint (juce::Graphics& g) { g.fillAll (Colours::bg); }

void ReplaceTab::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto top = r.removeFromTop (190);
    s1.setBounds (top);
    auto c1 = s1.content();
    auto row = c1.removeFromTop (34);
    loadBtn.setBounds (row.removeFromLeft (170).reduced (2));
    row.removeFromLeft (10);
    offsetSlider.setBounds (row.removeFromRight (150).reduced (0, 4));
    offsetLabel.setBounds (row.removeFromRight (140));
    fileLabel.setBounds (row);
    c1.removeFromTop (6);
    stemView.setBounds (c1);

    r.removeFromTop (10);
    auto mid = r.removeFromTop (220);
    auto left = mid.removeFromLeft (mid.getWidth() / 2 - 5);
    mid.removeFromLeft (10);
    s2.setBounds (left);
    auto c2 = s2.content();
    for (auto [s, l] : { std::pair<juce::Slider*, juce::Label*> { &sensSlider, &sensLabel }, { &minNoteSlider, &minLabel }, { &splitSlider, &splitLabel } })
    {
        auto rr = c2.removeFromTop (26);
        l->setBounds (rr.removeFromLeft (150));
        s->setBounds (rr);
    }
    lyricsToggle.setBounds (c2.removeFromTop (26));
    c2.removeFromTop (4);
    auto br = c2.removeFromTop (34);
    analyseBtn.setBounds (br.removeFromLeft (150).reduced (2));
    cancelBtn.setBounds (br.removeFromLeft (80).reduced (2));
    br.removeFromLeft (6);
    analysisBar.setBounds (br.reduced (0, 8));
    resultLabel.setBounds (c2);

    s3.setBounds (mid);
    auto c3 = s3.content();
    openEditorBtn.setBounds (c3.removeFromBottom (36).withWidth (220).reduced (2));
    s3Text.setBounds (c3);

    r.removeFromTop (10);
    s4.setBounds (r);
    auto c4 = s4.content();
    voiceLabel.setBounds (c4.removeFromTop (24));
    auto nr = c4.removeFromTop (28);
    nuanceLabel.setBounds (nr.removeFromLeft (150));
    nuanceSlider.setBounds (nr.removeFromLeft (300));
    nr.removeFromLeft (20);
    monitorLabel.setBounds (nr.removeFromLeft (70));
    monitorBox.setBounds (nr.removeFromLeft (160).reduced (0, 2));
    c4.removeFromTop (6);
    auto rb = c4.removeFromTop (36);
    renderBtn.setBounds (rb.removeFromLeft (170).reduced (2));
    exportBtn.setBounds (rb.removeFromLeft (190).reduced (2));
    rb.removeFromLeft (8);
    renderBar.setBounds (rb.reduced (0, 9));
    c4.removeFromTop (4);
    hint.setBounds (c4);
}

void ReplaceTab::loadStem (const juce::File& f)
{
    juce::String err;
    if (! proc.loadStem (f, err))
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Load stem", err);
    stemView.invalidate();
}

bool ReplaceTab::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files) if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3")) return true;
    return false;
}

void ReplaceTab::filesDropped (const juce::StringArray& files, int, int)
{
    for (auto& f : files) if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3")) { loadStem (juce::File (f)); break; }
}

void ReplaceTab::tick()
{
    auto& prj = proc.project();
    fileLabel.setText (prj.stemPath.isEmpty() ? juce::String ("No stem loaded") : juce::File (prj.stemPath).getFileName(),
                       juce::dontSendNotification);

    auto& an = proc.stemAnalyzer();
    if (an.isRunning())
    {
        analysisProgress = an.progress();
        analysisBar.setTextToDisplay (an.status());
    }
    if (an.hasResult())
    {
        proc.applyStemAnalysis();
        analysisProgress = 1.0;
        analysisBar.setTextToDisplay ("Done");
        stemView.invalidate();
    }

    if (proc.projectVersion() != lastVersion)
    {
        lastVersion = proc.projectVersion();
        int src = 0, changed = 0;
        for (auto& n : prj.notes) { src += n.fromSource ? 1 : 0; changed += n.lyricChangedFromSource() ? 1 : 0; }
        juce::String t;
        if (src > 0)
        {
            t << juce::String (src) << " notes detected";
            if (changed > 0) t << ", " << changed << " with new words";
            t << ".\n";
            if (prj.lyricsText.isNotEmpty()) t << "Lyrics: " << prj.lyricsText.substring (0, 220) << "\n";
        }
        if (proc.lastAnalysisNote.isNotEmpty()) t << proc.lastAnalysisNote;
        resultLabel.setText (t, juce::dontSendNotification);
        offsetSlider.setValue (prj.stemOffsetBeats, juce::dontSendNotification);
        stemView.repaint();
    }

    const auto& v = proc.currentVoice();
    juce::String vt = "Voice: " + v.name;
    vt << (v.isNeural() ? "  (neural - original articulation is preserved)"
                        : "  (Lite/DSP - the edited melody is re-sung by the built-in engine; train a neural voice for true replacement)");
    voiceLabel.setText (vt, juce::dontSendNotification);
    voiceLabel.setColour (juce::Label::textColourId, v.isNeural() ? Colours::accent2 : Colours::warn);

    auto& re = proc.renderer();
    renderProgress = re.isBusy() ? re.progress() : (proc.hasTake() ? 1.0 : 0.0);
    renderBar.setTextToDisplay (re.isBusy() ? re.status() : proc.takeSummary());
}

} // namespace vf::ui
