#include "VoiceCloneTab.h"
#include "../PluginProcessor.h"

namespace vf::ui
{

VoiceCloneTab::VoiceCloneTab (VoxForgeProcessor& p) : juce::Thread ("VoxForge voice analysis"), proc (p)
{
    for (auto* s : { &samplesSection, &cloneSection, &neuralSection }) addAndMakeVisible (*s);

    list.setModel (this);
    list.setRowHeight (40);
    list.setMultipleSelectionEnabled (true);
    addAndMakeVisible (list);
    for (auto* b : { &addBtn, &removeBtn, &clearBtn, &cloneBtn, &trainBtn, &cancelBtn }) addAndMakeVisible (*b);

    addBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Add dry vocal samples", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& fc) { addFiles (fc.getResults()); });
    };
    removeBtn.onClick = [this]
    {
        const juce::ScopedLock sl (itemLock);
        auto rows = list.getSelectedRows();
        for (int i = rows.size(); --i >= 0;)
            if (rows[i] < (int) items.size()) items.erase (items.begin() + rows[i]);
        list.updateContent(); list.deselectAllRows();
    };
    clearBtn.onClick = [this] { const juce::ScopedLock sl (itemLock); items.clear(); list.updateContent(); };

    tips.setText ("Use clean, DRY recordings of your own singing: no reverb, delay, doubling, autotune or backing music. "
                  "Close mic, quiet room, -12 to -6 dB peaks. Sing your full range with sustained vowels and real lyrics. "
                  "1-3 minutes make a Lite clone; 10-30 minutes are recommended for neural training. Drag & drop files here.",
                  juce::dontSendNotification);
    tips.setJustificationType (juce::Justification::topLeft);
    styleDim (tips, 12.5f);
    addAndMakeVisible (tips);
    styleDim (totalLabel, 12.5f);
    addAndMakeVisible (totalLabel);

    nameLabel.setText ("Voice name", juce::dontSendNotification);
    addAndMakeVisible (nameLabel);
    nameEditor.setText ("My Voice", false);
    nameEditor.setFont (juce::Font (juce::FontOptions (16.0f)));
    addAndMakeVisible (nameEditor);
    cloneBtn.setColour (juce::TextButton::buttonColourId, Colours::accent);
    cloneBtn.onClick = [this] { startClone(); };
    cloneBtn.setTooltip ("Analyses your samples (pitch range, vocal-tract length, spectral fingerprint, breathiness, vibrato) "
                         "and saves a named voice preset that works immediately.");
    addAndMakeVisible (progressBar);
    addAndMakeVisible (autoTrain);

    qualityBox.addItemList ({ "Fast (~20 min on GPU)", "Standard (~1 h on GPU)", "High (~3 h on GPU)" }, 1);
    qualityBox.setSelectedId (2, juce::dontSendNotification);
    qualityBox.onChange = [this]
    {
        static const int e[] = { 60, 200, 600 };
        epochSlider.setValue (e[juce::jlimit (0, 2, qualityBox.getSelectedId() - 1)]);
    };
    addAndMakeVisible (qualityBox);
    epochSlider.setRange (10, 2000, 10);
    epochSlider.setValue (200);
    epochSlider.setTextValueSuffix (" epochs");
    epochSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    epochSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 90, 22);
    addAndMakeVisible (epochSlider);
    trainBtn.onClick = [this] { startTraining(); };
    cancelBtn.onClick = [this] { proc.cancelTraining(); cancelFlag = true; };
    styleDim (trainTarget, 12.5f);
    styleDim (engineLabel, 12.0f);
    addAndMakeVisible (trainTarget);
    addAndMakeVisible (engineLabel);
    addAndMakeVisible (trainBar);

    log.setMultiLine (true);
    log.setReadOnly (true);
    log.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain)));
    addAndMakeVisible (log);
    appendLog ("Ready. Add samples, name your voice and press 'Create Voice Clone'.");
}

VoiceCloneTab::~VoiceCloneTab()
{
    cancelFlag = true;
    stopThread (15000);
}

void VoiceCloneTab::paint (juce::Graphics& g) { g.fillAll (Colours::bg); }

void VoiceCloneTab::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto left = r.removeFromLeft (r.getWidth() * 52 / 100);
    r.removeFromLeft (10);

    samplesSection.setBounds (left);
    auto sc = samplesSection.content();
    tips.setBounds (sc.removeFromTop (70));
    auto btns = sc.removeFromBottom (36);
    addBtn.setBounds (btns.removeFromLeft (120).reduced (2));
    removeBtn.setBounds (btns.removeFromLeft (90).reduced (2));
    clearBtn.setBounds (btns.removeFromLeft (80).reduced (2));
    totalLabel.setBounds (btns.reduced (6, 0));
    list.setBounds (sc.reduced (0, 4));

    auto top = r.removeFromTop (190);
    cloneSection.setBounds (top);
    auto cc = cloneSection.content();
    auto row = cc.removeFromTop (32);
    nameLabel.setBounds (row.removeFromLeft (90));
    nameEditor.setBounds (row);
    cc.removeFromTop (8);
    cloneBtn.setBounds (cc.removeFromTop (40).withWidth (220));
    cc.removeFromTop (8);
    progressBar.setBounds (cc.removeFromTop (20));
    cc.removeFromTop (6);
    autoTrain.setBounds (cc.removeFromTop (24));

    r.removeFromTop (10);
    auto mid = r.removeFromTop (200);
    neuralSection.setBounds (mid);
    auto nc = neuralSection.content();
    trainTarget.setBounds (nc.removeFromTop (20));
    auto q = nc.removeFromTop (32);
    qualityBox.setBounds (q.removeFromLeft (220).reduced (0, 2));
    q.removeFromLeft (8);
    epochSlider.setBounds (q);
    nc.removeFromTop (6);
    auto tb = nc.removeFromTop (36);
    trainBtn.setBounds (tb.removeFromLeft (180).reduced (0, 2));
    tb.removeFromLeft (8);
    cancelBtn.setBounds (tb.removeFromLeft (90).reduced (0, 2));
    nc.removeFromTop (6);
    trainBar.setBounds (nc.removeFromTop (20));
    engineLabel.setBounds (nc.removeFromTop (34));

    r.removeFromTop (10);
    log.setBounds (r);
}

//==============================================================================
int VoiceCloneTab::getNumRows() { const juce::ScopedLock sl (itemLock); return (int) items.size(); }

void VoiceCloneTab::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    const juce::ScopedLock sl (itemLock);
    if (row >= (int) items.size()) return;
    const auto& it = items[(size_t) row];
    if (selected) g.fillAll (Colours::accent.withAlpha (0.25f));
    g.setColour (Colours::text);
    g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    g.drawText (it.file.getFileName(), 10, 3, w - 20, 18, juce::Justification::left, true);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    if (! it.inspected)
    {
        g.setColour (Colours::dim);
        g.drawText ("checking...", 10, 21, w - 20, 16, juce::Justification::left);
        return;
    }
    const auto& r = it.report;
    juce::String info = juce::String (r.seconds, 1) + " s, singing " + juce::String (r.voicedSeconds, 1) + " s";
    if (r.maxMidi > 0) info << ", range " << juce::MidiMessage::getMidiNoteName ((int) r.minMidi, true, true, 4)
                            << "-" << juce::MidiMessage::getMidiNoteName ((int) r.maxMidi, true, true, 4);
    info << ", peak " << juce::String (r.peakDb, 1) << " dB";
    g.setColour (r.warnings.isEmpty() ? Colours::ok : (r.usable ? Colours::warn : Colours::error));
    g.drawText ((r.warnings.isEmpty() ? juce::String ("OK  ") : juce::String ("!  ")) + info
                + (r.warnings.isEmpty() ? juce::String() : "  -  " + r.warnings[0]),
                10, 21, w - 20, 16, juce::Justification::left, true);
    juce::ignoreUnused (h);
}

bool VoiceCloneTab::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3")) return true;
    return false;
}

void VoiceCloneTab::filesDropped (const juce::StringArray& files, int, int)
{
    juce::Array<juce::File> fs;
    for (auto& f : files) if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3")) fs.add (juce::File (f));
    addFiles (fs);
}

void VoiceCloneTab::addFiles (const juce::Array<juce::File>& files)
{
    {
        const juce::ScopedLock sl (itemLock);
        for (auto& f : files)
        {
            bool dup = false;
            for (auto& it : items) dup |= it.file == f;
            if (! dup && f.existsAsFile()) items.push_back ({ f, {}, false });
        }
    }
    list.updateContent();
    list.repaint();

    // quick inspection in the background
    juce::Thread::launch ([sp = juce::Component::SafePointer<VoiceCloneTab> (this), files]
    {
        for (auto& f : files)
        {
            auto rep = inspectSample (f);
            juce::MessageManager::callAsync ([sp, f, rep]
            {
                if (sp == nullptr) return;
                const juce::ScopedLock sl (sp->itemLock);
                for (auto& it : sp->items) if (it.file == f) { it.report = rep; it.inspected = true; }
                sp->list.repaint();
            });
        }
    });
}

void VoiceCloneTab::appendLog (const juce::String& s)
{
    log.moveCaretToEnd();
    log.insertTextAtCaret (s + "\n");
}

//==============================================================================
void VoiceCloneTab::startClone()
{
    if (isThreadRunning()) return;
    if (items.empty()) { appendLog ("Add at least one sample first."); return; }
    pendingName = nameEditor.getText().trim();
    if (pendingName.isEmpty()) pendingName = "My Voice";
    cancelFlag = false;
    analysisDone = false;
    progressValue = 0.0;
    appendLog ("--- Cloning '" + pendingName + "' from " + juce::String ((int) items.size()) + " file(s) ---");
    cloneBtn.setEnabled (false);
    startThread();
}

void VoiceCloneTab::run()
{
    juce::Array<juce::File> files;
    {
        const juce::ScopedLock sl (itemLock);
        for (auto& it : items) files.add (it.file);
    }
    analysis = analyseVoiceSamples (files, &cancelFlag, [this] (float p, const juce::String& m)
    {
        progressValue = p;
        const juce::ScopedLock sl (logLock);
        pendingLog.add (m);
    });
    analysisDone = true;
}

void VoiceCloneTab::startTraining()
{
    const auto& v = proc.currentVoice();
    if (v.id == "builtin-default" || v.samples.isEmpty())
    {
        appendLog ("Select a cloned voice (with samples) in the header first, or create a clone.");
        return;
    }
    if (! EngineClient::isInstalled())
    {
        appendLog (EngineClient::describeInstallation());
        return;
    }
    if (proc.trainJob != nullptr) { appendLog ("A training job is already running."); return; }
    static const char* q[] = { "fast", "standard", "high" };
    proc.startNeuralTraining (v, (int) epochSlider.getValue(), q[juce::jlimit (0, 2, qualityBox.getSelectedId() - 1)]);
    lastLogLines = 0;
    appendLog ("Neural training started for '" + v.name + "'. You can close the window - training continues.");
}

void VoiceCloneTab::tick()
{
    {
        const juce::ScopedLock sl (logLock);
        for (auto& l : pendingLog) appendLog ("  " + l);
        pendingLog.clear();
    }

    double total = 0.0;
    {
        const juce::ScopedLock sl (itemLock);
        for (auto& it : items) total += it.report.voicedSeconds;
    }
    totalLabel.setText ("Total singing: " + juce::String (total / 60.0, 1) + " min", juce::dontSendNotification);

    if (analysisDone.exchange (false))
    {
        cloneBtn.setEnabled (true);
        if (! analysis.ok)
            appendLog ("Clone failed: " + analysis.error);
        else
        {
            VoiceModel v;
            v.name = pendingName;
            v.engine = "lite";
            v.timbre = analysis.timbre;
            v.stats = analysis.stats;
            v.expression = analysis.suggested;
            v.folder = proc.presets().createVoiceFolder (pendingName);

            // keep a copy of the training material with the preset (needed for neural training)
            auto sampleDir = v.folder.getChildFile ("samples");
            sampleDir.createDirectory();
            for (auto& rep : analysis.reports)
            {
                if (! rep.usable) continue;
                auto dst = sampleDir.getChildFile (rep.file.getFileName());
                if (rep.file.copyFileTo (dst)) v.samples.add ("samples/" + dst.getFileName());
            }
            proc.presets().saveVoice (v);
            proc.voicesChanged();
            proc.selectVoice (v.id, true);

            appendLog ("Voice '" + v.name + "' saved as preset.");
            appendLog ("  singing analysed: " + juce::String (v.stats.seconds, 1) + " s");
            appendLog ("  range: " + juce::MidiMessage::getMidiNoteName ((int) v.stats.minMidi, true, true, 4) + " - "
                       + juce::MidiMessage::getMidiNoteName ((int) v.stats.maxMidi, true, true, 4));
            appendLog ("  vocal-tract scale: " + juce::String (v.timbre.formantScale, 2)
                       + ", breathiness: " + juce::String (v.expression.breathiness, 2)
                       + ", vibrato: " + juce::String (v.stats.vibratoRate, 1) + " Hz / " + juce::String (v.stats.vibratoDepth, 0) + " cents");
            for (auto& w : analysis.warnings) appendLog ("  note: " + w);
            for (auto& r : analysis.reports)
                for (auto& w : r.warnings) appendLog ("  " + r.file.getFileName() + ": " + w);

            if (autoTrain.getToggleState()) startTraining();
        }
    }

    const auto& v = proc.currentVoice();
    trainTarget.setText ("Target voice: " + v.name + (v.isNeural() ? "  (already neural - retraining overwrites the model)" : ""),
                         juce::dontSendNotification);
    engineLabel.setText (EngineClient::describeInstallation() + "\nNeural playback: " + proc.renderer().neuralStatus(),
                         juce::dontSendNotification);

    if (proc.trainJob != nullptr)
    {
        trainProgress = proc.trainJob->progress();
        trainBar.setTextToDisplay (proc.trainJob->status());
        auto lines = proc.trainJob->logLines();
        for (int i = lastLogLines; i < lines.size(); ++i) appendLog ("  " + lines[i]);
        lastLogLines = lines.size();
        trainBtn.setEnabled (false);
    }
    else
    {
        trainBtn.setEnabled (true);
        if (trainProgress > 0.0) { trainProgress = 0.0; trainBar.setTextToDisplay ({}); appendLog (proc.lastMessage); }
    }
}

} // namespace vf::ui
