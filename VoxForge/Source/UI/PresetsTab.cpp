#include "PresetsTab.h"
#include "../PluginProcessor.h"
#include "../Core/Lyrics.h"

namespace vf::ui
{

PresetsTab::PresetsTab (VoxForgeProcessor& p) : proc (p)
{
    for (auto* s : { &listSection, &detailSection, &settingsSection }) addAndMakeVisible (*s);
    list.setModel (this);
    list.setRowHeight (44);
    addAndMakeVisible (list);
    details.setJustificationType (juce::Justification::topLeft);
    details.setFont (juce::Font (juce::FontOptions (14.0f)));
    addAndMakeVisible (details);

    for (auto* b : { &useBtn, &useExprBtn, &renameBtn, &dupBtn, &deleteBtn, &exportBtn, &importBtn, &saveExprBtn,
                     &openFolderBtn, &browseEngineBtn, &testEngineBtn, &dictBtn })
        addAndMakeVisible (*b);
    useBtn.setColour (juce::TextButton::buttonColourId, Colours::accent);

    useBtn.onClick = [this] { if (auto id = selectedId(); id.isNotEmpty()) proc.selectVoice (id, false); };
    useExprBtn.onClick = [this] { if (auto id = selectedId(); id.isNotEmpty()) proc.selectVoice (id, true); };
    renameBtn.onClick = [this]
    {
        auto id = selectedId();
        auto* v = proc.presets().find (id);
        if (v == nullptr || id == "builtin-default") return;
        askName ("Rename voice", v->name, [this, id] (juce::String n)
        {
            proc.presets().renameVoice (id, n);
            proc.voicesChanged();
            if (proc.currentVoice().id == id) proc.selectVoice (id, false);
            refreshList();
        });
    };
    dupBtn.onClick = [this]
    {
        auto id = selectedId();
        auto* v = proc.presets().find (id);
        if (v == nullptr) return;
        askName ("Duplicate voice", v->name + " copy", [this, id] (juce::String n)
        {
            proc.presets().duplicateVoice (id, n);
            refreshList();
        });
    };
    deleteBtn.onClick = [this]
    {
        auto id = selectedId();
        auto* v = proc.presets().find (id);
        if (v == nullptr || id == "builtin-default") return;
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Delete voice",
            "Delete '" + v->name + "' and its samples/model permanently?", "Delete", "Cancel", this,
            juce::ModalCallbackFunction::create ([this, id] (int r)
            {
                if (r == 0) return;
                proc.presets().deleteVoice (id);
                proc.voicesChanged();
                refreshList();
            }));
    };
    exportBtn.onClick = [this]
    {
        auto id = selectedId();
        auto* v = proc.presets().find (id);
        if (v == nullptr || id == "builtin-default") return;
        chooser = std::make_unique<juce::FileChooser> ("Export voice", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                           .getChildFile (v->name + ".vfvoice"), "*.vfvoice");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, id] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            proc.lastMessage = proc.presets().exportVoice (id, f.withFileExtension ("vfvoice")) ? "Exported " + f.getFileName() : "Export failed";
        });
    };
    importBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Import voice", juce::File(), "*.vfvoice;*.zip");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (! f.existsAsFile()) return;
            auto r = proc.presets().importVoice (f);
            proc.lastMessage = r.wasOk() ? "Imported " + f.getFileNameWithoutExtension() : "Import failed: " + r.getErrorMessage();
            refreshList();
        });
    };
    saveExprBtn.onClick = [this]
    {
        auto id = selectedId();
        auto* v = proc.presets().find (id);
        if (v == nullptr || id == "builtin-default") return;
        auto copy = *v;
        copy.expression = proc.expressionSettings();
        copy.save();
        proc.voicesChanged();
        proc.lastMessage = "Expression saved into '" + copy.name + "'.";
        updateDetails();
    };

    // ---- settings ------------------------------------------------------------------
    engineLabel.setText ("Engine command", juce::dontSendNotification);
    addAndMakeVisible (engineLabel);
    engineEditor.setTextToShowWhenEmpty ("auto-detect  (or e.g.  \"C:\\Python311\\python.exe\" -m voxforge_engine)", Colours::dim);
    engineEditor.setText (Settings::get().engineCommand(), false);
    engineEditor.onTextChange = [this]
    {
        Settings::get().props().setValue ("engineCommand", engineEditor.getText().trim());
        Settings::get().props().saveIfNeeded();
    };
    addAndMakeVisible (engineEditor);
    browseEngineBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Locate voxforge-engine", juce::File(), "*");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f.existsAsFile()) engineEditor.setText ("\"" + f.getFullPathName() + "\"", true);
        });
    };
    testEngineBtn.onClick = [this]
    {
        statusLabel.setText ("Testing engine...", juce::dontSendNotification);
        juce::Component::SafePointer<PresetsTab> sp (this);   // (MSVC: no 'this' in a nested init-capture)
        juce::Thread::launch ([sp]
        {
            auto o = EngineClient::run ({ "check", "--models-dir", Paths::modelsDir().getFullPathName() }, nullptr, nullptr);
            juce::String text = o.ok ? "Engine OK: " + o.result.getProperty ("summary", "").toString() : "Engine error: " + o.error;
            juce::MessageManager::callAsync ([sp, text] { if (sp != nullptr) sp->statusLabel.setText (text, juce::dontSendNotification); });
        });
    };
    langLabel.setText ("Lyrics language", juce::dontSendNotification);
    addAndMakeVisible (langLabel);
    const juce::StringArray langs { "auto", "en", "es", "fr", "de", "it", "pt", "nl", "ja", "ko", "zh", "ru", "pl", "tr" };
    langBox.addItemList (langs, 1);
    langBox.setSelectedId (juce::jmax (1, langs.indexOf (Settings::get().language()) + 1), juce::dontSendNotification);
    langBox.onChange = [this, langs]
    {
        Settings::get().props().setValue ("language", langs[langBox.getSelectedId() - 1]);
        Settings::get().props().saveIfNeeded();
    };
    addAndMakeVisible (langBox);
    gpuToggle.setToggleState (Settings::get().useGpu(), juce::dontSendNotification);
    gpuToggle.onClick = [this]
    {
        Settings::get().props().setValue ("useGpu", gpuToggle.getToggleState());
        Settings::get().props().saveIfNeeded();
    };
    addAndMakeVisible (gpuToggle);
    openFolderBtn.onClick = [] { Paths::dataDir().startAsProcess(); };
    dictBtn.setTooltip ("Optional: a CMU pronouncing dictionary (cmudict.dict) greatly improves English pronunciation.");
    dictBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("CMU pronouncing dictionary", juce::File(), "*.dict;*.txt;*");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (! f.existsAsFile()) return;
            f.copyFileTo (Paths::cmuDict());
            const int n = Lexicon::get().loadDictionary (Paths::cmuDict());
            proc.lastMessage = "Pronunciation dictionary: " + juce::String (n) + " words";
        });
    };
    styleDim (statusLabel, 12.5f);
    statusLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (statusLabel);

    refreshList();
}

void PresetsTab::paint (juce::Graphics& g) { g.fillAll (Colours::bg); }

void PresetsTab::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto settings = r.removeFromBottom (200);
    r.removeFromBottom (10);
    auto left = r.removeFromLeft (r.getWidth() * 45 / 100);
    r.removeFromLeft (10);

    listSection.setBounds (left);
    auto lc = listSection.content();
    auto lb = lc.removeFromBottom (36);
    importBtn.setBounds (lb.removeFromLeft (150).reduced (2));
    list.setBounds (lc.reduced (0, 4));

    detailSection.setBounds (r);
    auto dc = detailSection.content();
    auto b1 = dc.removeFromBottom (36), b2 = dc.removeFromBottom (36), b3 = dc.removeFromBottom (36);
    useBtn.setBounds (b3.removeFromLeft (120).reduced (2));
    useExprBtn.setBounds (b3.removeFromLeft (210).reduced (2));
    renameBtn.setBounds (b2.removeFromLeft (100).reduced (2));
    dupBtn.setBounds (b2.removeFromLeft (100).reduced (2));
    deleteBtn.setBounds (b2.removeFromLeft (90).reduced (2));
    exportBtn.setBounds (b2.removeFromLeft (140).reduced (2));
    saveExprBtn.setBounds (b1.removeFromLeft (290).reduced (2));
    details.setBounds (dc);

    settingsSection.setBounds (settings);
    auto sc = settingsSection.content();
    auto row = sc.removeFromTop (30);
    engineLabel.setBounds (row.removeFromLeft (130));
    testEngineBtn.setBounds (row.removeFromRight (110).reduced (2));
    browseEngineBtn.setBounds (row.removeFromRight (90).reduced (2));
    engineEditor.setBounds (row.reduced (0, 2));
    sc.removeFromTop (6);
    row = sc.removeFromTop (30);
    langLabel.setBounds (row.removeFromLeft (130));
    langBox.setBounds (row.removeFromLeft (120).reduced (0, 2));
    row.removeFromLeft (20);
    gpuToggle.setBounds (row.removeFromLeft (340));
    sc.removeFromTop (6);
    row = sc.removeFromTop (32);
    openFolderBtn.setBounds (row.removeFromLeft (150).reduced (2));
    dictBtn.setBounds (row.removeFromLeft (190).reduced (2));
    sc.removeFromTop (4);
    statusLabel.setBounds (sc);
}

void PresetsTab::refreshList()
{
    proc.presets().rescan();
    list.updateContent();
    list.repaint();
    lastCount = (int) proc.presets().voices().size();
    updateDetails();
}

int PresetsTab::getNumRows() { return (int) proc.presets().voices().size(); }

void PresetsTab::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    const auto& vs = proc.presets().voices();
    if (row >= (int) vs.size()) return;
    const auto& v = vs[(size_t) row];
    if (selected) g.fillAll (Colours::accent.withAlpha (0.25f));
    const bool active = v.id == proc.currentVoice().id;
    g.setColour (active ? Colours::accent2 : Colours::text);
    g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
    g.drawText ((active ? juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x8f ")) : juce::String()) + v.name, 12, 4, w - 110, 20, juce::Justification::left, true);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.setColour (Colours::dim);
    juce::String sub = v.id == "builtin-default" ? juce::String ("built-in")
                     : juce::String (v.stats.seconds / 60.0, 1) + " min analysed, "
                       + juce::MidiMessage::getMidiNoteName ((int) v.stats.minMidi, true, true, 4) + "-"
                       + juce::MidiMessage::getMidiNoteName ((int) v.stats.maxMidi, true, true, 4);
    g.drawText (sub, 12, 24, w - 110, 16, juce::Justification::left, true);

    auto badge = juce::Rectangle<float> ((float) w - 92, 12.0f, 78.0f, 20.0f);
    g.setColour (v.isNeural() ? Colours::accent : Colours::panel2);
    g.fillRoundedRectangle (badge, 10.0f);
    g.setColour (Colours::text);
    g.drawText (v.isNeural() ? "NEURAL" : "LITE", badge, juce::Justification::centred);
    juce::ignoreUnused (h);
}

void PresetsTab::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    const auto& vs = proc.presets().voices();
    if (row < (int) vs.size()) proc.selectVoice (vs[(size_t) row].id, false);
    list.repaint();
}

juce::String PresetsTab::selectedId() const
{
    const auto& vs = proc.presets().voices();
    const int row = list.getSelectedRow();
    return row >= 0 && row < (int) vs.size() ? vs[(size_t) row].id : juce::String();
}

void PresetsTab::updateDetails()
{
    auto* v = proc.presets().find (selectedId());
    if (v == nullptr) { details.setText ("Select a voice on the left.\nDouble-click to make it active.", juce::dontSendNotification); return; }
    juce::String t;
    t << v->name << "\n\n";
    t << "Engine:  " << (v->isNeural() ? "Neural (trained model: " + v->neuralModel + ")" : juce::String ("Lite (DSP clone)")) << "\n";
    if (v->created.isNotEmpty()) t << "Created:  " << v->created.substring (0, 10) << "\n";
    if (v->id != "builtin-default")
    {
        t << "Singing analysed:  " << juce::String (v->stats.seconds / 60.0, 1) << " min  (" << v->samples.size() << " files)\n";
        t << "Range:  " << juce::MidiMessage::getMidiNoteName ((int) v->stats.minMidi, true, true, 4) << " - "
          << juce::MidiMessage::getMidiNoteName ((int) v->stats.maxMidi, true, true, 4)
          << "   (median " << juce::MidiMessage::getMidiNoteName ((int) std::round (v->stats.medianMidi), true, true, 4) << ")\n";
        t << "Vocal-tract scale:  " << juce::String (v->timbre.formantScale, 2) << "\n";
        t << "Natural vibrato:  " << juce::String (v->stats.vibratoRate, 1) << " Hz, " << juce::String (v->stats.vibratoDepth, 0) << " cents\n";
        t << "Preset expression:  vibrato " << juce::String (v->expression.vibratoDepthCents, 0) << " c, breath "
          << juce::String (v->expression.breathiness, 2) << ", tension " << juce::String (v->expression.tension, 2) << "\n";
        t << "\nFolder:  " << v->folder.getFullPathName() << "\n";
    }
    else t << "\n" << v->notes;
    details.setText (t, juce::dontSendNotification);
}

void PresetsTab::askName (const juce::String& title, const juce::String& initial, std::function<void (juce::String)> done)
{
    nameWindow = std::make_unique<juce::AlertWindow> (title, "Name:", juce::MessageBoxIconType::NoIcon);
    nameWindow->addTextEditor ("name", initial);
    nameWindow->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    nameWindow->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    nameWindow->enterModalState (true, juce::ModalCallbackFunction::create ([this, done] (int r)
    {
        if (r == 1 && nameWindow != nullptr)
        {
            auto n = nameWindow->getTextEditorContents ("name").trim();
            if (n.isNotEmpty()) done (n);
        }
        nameWindow.reset();
    }), false);
}

void PresetsTab::tick()
{
    if ((int) proc.presets().voices().size() != lastCount) refreshList();
    if (statusLabel.getText().isEmpty() || statusLabel.getText().startsWith ("Pronunciation"))
        statusLabel.setText ("Pronunciation: " + juce::String (Lexicon::get().hasUserDictionary() ? "CMU dictionary loaded" : "built-in rules (load cmudict for best English)")
                             + "\nNeural playback: " + proc.renderer().neuralStatus()
                             + "\n" + EngineClient::describeInstallation(), juce::dontSendNotification);
}

} // namespace vf::ui
