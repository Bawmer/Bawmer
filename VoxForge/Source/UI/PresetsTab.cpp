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

    for (auto* b : { &useBtn, &useExprBtn, &renameBtn, &dupBtn, &deleteBtn, &exportBtn, &importBtn, &importRvcBtn, &saveExprBtn,
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
    importRvcBtn.setTooltip ("Use a voice trained in Applio or RVC WebUI (RVC v2 .pth, 32/40/48 kHz, HiFi-GAN). "
                             "Needs the VoxForge Engine for the one-time conversion.");
    importRvcBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Import Applio / RVC voice model", juce::File(), "*.pth");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (! f.existsAsFile()) return;
            askName ("Name for the imported voice", f.getFileNameWithoutExtension(),
                     [this, f] (juce::String n) { importRvcModel (f, n); });
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
    const juce::StringArray models { "tiny", "base", "small", "medium", "large-v3" };
    lyricsModelBox.addItemList ({ "Lyrics model: tiny (fastest)", "Lyrics model: base", "Lyrics model: small (default)",
                                  "Lyrics model: medium", "Lyrics model: large-v3 (best, slow)" }, 1);
    lyricsModelBox.setSelectedId (juce::jmax (1, models.indexOf (Settings::get().lyricsModel()) + 1), juce::dontSendNotification);
    lyricsModelBox.setTooltip ("Whisper model used to extract lyrics from vocal stems. Larger = more accurate on singing, slower; downloaded once.");
    lyricsModelBox.onChange = [this, models]
    {
        Settings::get().props().setValue ("lyricsModel", models[lyricsModelBox.getSelectedId() - 1]);
        Settings::get().props().saveIfNeeded();
    };
    addAndMakeVisible (lyricsModelBox);
    gpuToggle.setTooltip ("RTX / NVIDIA GPU: neural training and lyrics extraction use CUDA (engine), "
                          "neural rendering in the plug-in uses DirectML (no CUDA install needed). "
                          "Other DirectX 12 GPUs also work for rendering.");
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
    // ---- Voicebox ---------------------------------------------------------------------
    voiceboxLabel.setText ("Voicebox", juce::dontSendNotification);
    voiceboxLabel.setTooltip ("Voicebox: a free local voice studio (Qwen3-TTS voice cloning). VoxForge can use it as a "
                              "natural pronunciation guide and to train voices from a few seconds of audio.");
    addAndMakeVisible (voiceboxLabel);
    voiceboxUrlEditor.setText (Settings::get().voiceboxUrl(), false);
    voiceboxUrlEditor.setTextToShowWhenEmpty (voicebox::kDefaultUrl, Colours::dim);
    voiceboxUrlEditor.onTextChange = [this]
    {
        auto u = voiceboxUrlEditor.getText().trim();
        Settings::get().props().setValue ("voiceboxUrl", u.isNotEmpty() ? u : juce::String (voicebox::kDefaultUrl));
        Settings::get().props().saveIfNeeded();
    };
    addAndMakeVisible (voiceboxUrlEditor);
    voiceboxConnectBtn.setTooltip ("Read the voice profiles from the running Voicebox app");
    voiceboxConnectBtn.onClick = [this] { connectVoicebox(); };
    addAndMakeVisible (voiceboxConnectBtn);
    voiceboxProfileBox.setTextWhenNothingSelected ("guide voice profile (click Connect)");
    voiceboxProfileBox.setTooltip ("Voicebox profile that speaks the lyrics. Its timbre does not matter much: "
                                   "only its pronunciation and timing are used, the neural voice provides the timbre.");
    voiceboxProfileBox.onChange = [this]
    {
        const int i = voiceboxProfileBox.getSelectedId() - 1;
        if (i < 0 || i >= (int) voiceboxProfiles.size()) return;
        auto& pr = Settings::get().props();
        pr.setValue ("voiceboxGuideProfile", voiceboxProfiles[(size_t) i].id);
        pr.setValue ("voiceboxGuideProfileName", voiceboxProfiles[(size_t) i].name);
        pr.setValue ("voiceboxGuideLanguage", voiceboxProfiles[(size_t) i].language);
        pr.saveIfNeeded();
    };
    addAndMakeVisible (voiceboxProfileBox);
    if (Settings::get().voiceboxGuideProfile().isNotEmpty())
    {
        voiceboxProfiles.push_back ({ Settings::get().voiceboxGuideProfile(),
                                      Settings::get().props().getValue ("voiceboxGuideProfileName", "saved profile"),
                                      Settings::get().voiceboxGuideLanguage() });
        fillVoiceboxProfiles();
    }
    voiceboxGuideToggle.setTooltip ("When rendering MIDI with a neural voice, Voicebox speaks the lyrics of each phrase and "
                                    "VoxForge aligns that speech to your notes instead of the built-in robotic guide. "
                                    "Clearer, more natural words. The first render of a phrase waits for Voicebox; "
                                    "after that it is cached.");
    voiceboxGuideToggle.setToggleState (Settings::get().useVoiceboxGuide(), juce::dontSendNotification);
    voiceboxGuideToggle.onClick = [this]
    {
        Settings::get().props().setValue ("voiceboxGuide", voiceboxGuideToggle.getToggleState());
        Settings::get().props().saveIfNeeded();
        if (voiceboxGuideToggle.getToggleState() && Settings::get().voiceboxGuideProfile().isEmpty())
            statusLabel.setText ("Click Connect and choose a Voicebox profile to use as the pronunciation guide.", juce::dontSendNotification);
    };
    addAndMakeVisible (voiceboxGuideToggle);

    styleDim (statusLabel, 12.5f);
    statusLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (statusLabel);

    refreshList();
}

void PresetsTab::paint (juce::Graphics& g) { g.fillAll (Colours::bg); }

void PresetsTab::fillVoiceboxProfiles()
{
    voiceboxProfileBox.clear (juce::dontSendNotification);
    const auto current = Settings::get().voiceboxGuideProfile();
    for (size_t i = 0; i < voiceboxProfiles.size(); ++i)
    {
        voiceboxProfileBox.addItem (voiceboxProfiles[i].name + "  (" + voiceboxProfiles[i].language + ")", (int) i + 1);
        if (voiceboxProfiles[i].id == current) voiceboxProfileBox.setSelectedId ((int) i + 1, juce::dontSendNotification);
    }
}

void PresetsTab::connectVoicebox()
{
    const auto url = Settings::get().voiceboxUrl();
    statusLabel.setText ("Connecting to Voicebox at " + url + " ...", juce::dontSendNotification);
    juce::Component::SafePointer<PresetsTab> sp (this);
    juce::Thread::launch ([sp, url]
    {
        std::vector<voicebox::Profile> profiles;
        juce::String err;
        const bool ok = voicebox::listProfiles (url, profiles, err);
        juce::MessageManager::callAsync ([sp, ok, profiles, err]
        {
            if (sp == nullptr) return;
            if (! ok) { sp->statusLabel.setText ("Voicebox: " + err, juce::dontSendNotification); return; }
            sp->voiceboxProfiles = profiles;
            sp->fillVoiceboxProfiles();
            if (sp->voiceboxProfileBox.getSelectedId() == 0 && ! profiles.empty())
                sp->voiceboxProfileBox.setSelectedId (1, juce::sendNotificationSync);
            sp->statusLabel.setText ("Voicebox connected: " + juce::String ((int) profiles.size()) + " voice profile(s)."
                                     + (profiles.empty() ? "  Create a profile in Voicebox first." : juce::String()),
                                     juce::dontSendNotification);
        });
    });
}

void PresetsTab::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto settings = r.removeFromBottom (240);
    r.removeFromBottom (10);
    auto left = r.removeFromLeft (r.getWidth() * 45 / 100);
    r.removeFromLeft (10);

    listSection.setBounds (left);
    auto lc = listSection.content();
    auto lb = lc.removeFromBottom (36);
    importBtn.setBounds (lb.removeFromLeft (150).reduced (2));
    importRvcBtn.setBounds (lb.removeFromLeft (220).reduced (2));
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
    row.removeFromLeft (10);
    lyricsModelBox.setBounds (row.removeFromLeft (280).reduced (0, 2));
    row.removeFromLeft (14);
    gpuToggle.setBounds (row.removeFromLeft (340));
    sc.removeFromTop (6);
    row = sc.removeFromTop (30);
    voiceboxLabel.setBounds (row.removeFromLeft (130));
    voiceboxUrlEditor.setBounds (row.removeFromLeft (210).reduced (0, 2));
    voiceboxConnectBtn.setBounds (row.removeFromLeft (90).reduced (2));
    row.removeFromLeft (8);
    voiceboxProfileBox.setBounds (row.removeFromLeft (260).reduced (0, 2));
    row.removeFromLeft (14);
    voiceboxGuideToggle.setBounds (row.removeFromLeft (320));
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

void PresetsTab::importRvcModel (const juce::File& pth, const juce::String& name)
{
    if (! EngineClient::isInstalled())
    {
        proc.lastMessage = EngineClient::describeInstallation();
        return;
    }
    statusLabel.setText ("Importing " + pth.getFileName() + " ...", juce::dontSendNotification);
    auto folder = proc.presets().createVoiceFolder (name);
    juce::Thread::launch ([sp = juce::Component::SafePointer<PresetsTab> (this), pth, name, folder]
    {
        auto o = EngineClient::run ({ "import-model", "--model", pth.getFullPathName(),
                                      "--out", folder.getChildFile ("voice.onnx").getFullPathName() }, nullptr, nullptr);
        juce::MessageManager::callAsync ([sp, o, name, folder, pth]
        {
            if (sp == nullptr) return;
            if (! o.ok)
            {
                folder.deleteRecursively();
                sp->statusLabel.setText ("Import failed: " + o.error, juce::dontSendNotification);
                sp->proc.lastMessage = "Import failed: " + o.error;
                return;
            }
            VoiceModel v;
            v.name = name;
            v.folder = folder;
            v.engine = "neural";
            v.neuralModel = "voice.onnx";
            v.neuralSampleRate = o.result.getProperty ("sampleRate", 48000.0);
            v.neuralHop = o.result.getProperty ("hop", 960);
            v.notes = "Imported from Applio / RVC: " + pth.getFileName()
                      + " (" + juce::String ((int) v.neuralSampleRate / 1000) + " kHz)";
            const auto warn = o.result.getProperty ("warning", "").toString();
            if (warn.isNotEmpty()) v.notes << "\n" << warn;
            sp->proc.presets().saveVoice (v);
            sp->proc.voicesChanged();
            sp->proc.selectVoice (v.id, false);
            sp->refreshList();
            sp->statusLabel.setText ("Imported '" + name + "' as a neural voice." + (warn.isNotEmpty() ? "  " + warn : juce::String()),
                                     juce::dontSendNotification);
        });
    });
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
