#include "VoicePreset.h"

namespace vf
{

//==============================================================================
juce::File Paths::dataDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("VoxForge");
    d.createDirectory();
    return d;
}
juce::File Paths::voicesDir()      { auto d = dataDir().getChildFile ("Voices"); d.createDirectory(); return d; }
juce::File Paths::modelsDir()      { auto d = dataDir().getChildFile ("Models"); d.createDirectory(); return d; }
juce::File Paths::cacheDir()       { auto d = dataDir().getChildFile ("Cache");  d.createDirectory(); return d; }
juce::File Paths::cmuDict()        { return dataDir().getChildFile ("cmudict.dict"); }
juce::File Paths::contentEncoder() { return modelsDir().getChildFile ("content_encoder.onnx"); }

//==============================================================================
Settings& Settings::get()
{
    static Settings s;
    return s;
}

Settings::Settings()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "VoxForge";
    o.filenameSuffix = ".settings";
    o.folderName = "VoxForge";
    o.osxLibrarySubFolder = "Application Support";
    o.commonToAllUsers = false;
    o.storageFormat = juce::PropertiesFile::storeAsXML;
    file = std::make_unique<juce::PropertiesFile> (Paths::dataDir().getChildFile ("settings.xml"), o);
}

juce::String Settings::engineCommand() const { return file->getValue ("engineCommand"); }
juce::String Settings::language() const      { return file->getValue ("language", "auto"); }
bool Settings::useGpu() const                { return file->getBoolValue ("useGpu", true); }

juce::StringArray Settings::resolveEngineCommand() const
{
    juce::StringArray cmd;
    const auto custom = engineCommand().trim();
    if (custom.isNotEmpty())
    {
        cmd.addTokens (custom, " ", "\"");
        cmd.trim();
        cmd.removeEmptyStrings();
        for (auto& c : cmd) c = c.unquoted();
        return cmd;
    }

   #if JUCE_WINDOWS
    const juce::String exe = "voxforge-engine.exe";
   #else
    const juce::String exe = "voxforge-engine";
   #endif

    // 1. inside the plug-in bundle:  VoxForge.vst3/Contents/Resources/engine/
    const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    juce::Array<juce::File> candidates;
    candidates.add (self.getParentDirectory().getParentDirectory().getChildFile ("Resources/engine").getChildFile (exe));
    // 2. installer location
    candidates.add (juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory).getChildFile ("VoxForge/engine").getChildFile (exe));
    // 3. per-user data folder (PyInstaller build)
    candidates.add (Paths::dataDir().getChildFile ("engine").getChildFile (exe));
    // 4. per-user Python environment created by scripts/install_engine.ps1 (or .sh)
   #if JUCE_WINDOWS
    candidates.add (Paths::dataDir().getChildFile ("engine-venv/Scripts").getChildFile (exe));
   #else
    candidates.add (Paths::dataDir().getChildFile ("engine-venv/bin").getChildFile (exe));
   #endif

    for (auto& f : candidates)
        if (f.existsAsFile()) { cmd.add (f.getFullPathName()); return cmd; }

    return cmd;   // empty: engine not installed
}

//==============================================================================
static juce::var floatArray (const std::vector<float>& v)
{
    juce::Array<juce::var> a;
    for (auto x : v) a.add (x);
    return a;
}

juce::var VoiceModel::toJson() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("format", "voxforge-voice");
    o->setProperty ("version", 1);
    o->setProperty ("name", name);
    o->setProperty ("id", id);
    o->setProperty ("created", created);
    o->setProperty ("engine", engine);
    o->setProperty ("notes", notes);

    auto* t = new juce::DynamicObject();
    t->setProperty ("formantScale", timbre.formantScale);
    t->setProperty ("jitter", timbre.jitter);
    t->setProperty ("shimmer", timbre.shimmer);
    t->setProperty ("breathBase", timbre.breathBase);
    t->setProperty ("brightness", timbre.brightness);
    t->setProperty ("correctionDb", floatArray (timbre.correctionDb));
    o->setProperty ("timbre", t);

    auto* e = new juce::DynamicObject();
    e->setProperty ("vibratoDepthCents", expression.vibratoDepthCents);
    e->setProperty ("vibratoRateHz", expression.vibratoRateHz);
    e->setProperty ("vibratoDelayMs", expression.vibratoDelayMs);
    e->setProperty ("breathiness", expression.breathiness);
    e->setProperty ("portamentoMs", expression.portamentoMs);
    e->setProperty ("tension", expression.tension);
    e->setProperty ("formantShift", expression.formantShift);
    e->setProperty ("humanize", expression.humanize);
    o->setProperty ("expression", e);

    auto* s = new juce::DynamicObject();
    s->setProperty ("seconds", stats.seconds);
    s->setProperty ("medianMidi", stats.medianMidi);
    s->setProperty ("minMidi", stats.minMidi);
    s->setProperty ("maxMidi", stats.maxMidi);
    s->setProperty ("vibratoRate", stats.vibratoRate);
    s->setProperty ("vibratoDepth", stats.vibratoDepth);
    o->setProperty ("stats", s);

    if (neuralModel.isNotEmpty())
    {
        auto* n = new juce::DynamicObject();
        n->setProperty ("model", neuralModel);
        n->setProperty ("sampleRate", neuralSampleRate);
        n->setProperty ("hop", neuralHop);
        o->setProperty ("neural", n);
    }
    juce::Array<juce::var> sm;
    for (auto& x : samples) sm.add (x);
    o->setProperty ("samples", sm);
    return juce::var (o);
}

VoiceModel VoiceModel::fromJson (const juce::var& v, const juce::File& folder)
{
    VoiceModel m;
    m.folder = folder;
    m.name = v.getProperty ("name", folder.getFileName()).toString();
    m.id = v.getProperty ("id", folder.getFileName()).toString();
    m.created = v.getProperty ("created", "").toString();
    m.engine = v.getProperty ("engine", "lite").toString();
    m.notes = v.getProperty ("notes", "").toString();

    auto t = v.getProperty ("timbre", {});
    m.timbre.formantScale = t.getProperty ("formantScale", 1.0f);
    m.timbre.jitter = t.getProperty ("jitter", 0.004f);
    m.timbre.shimmer = t.getProperty ("shimmer", 0.03f);
    m.timbre.breathBase = t.getProperty ("breathBase", 0.0f);
    m.timbre.brightness = t.getProperty ("brightness", 0.5f);
    if (auto* arr = t.getProperty ("correctionDb", {}).getArray())
        for (auto& x : *arr) m.timbre.correctionDb.push_back ((float) x);

    auto e = v.getProperty ("expression", {});
    m.expression.vibratoDepthCents = e.getProperty ("vibratoDepthCents", 35.0f);
    m.expression.vibratoRateHz = e.getProperty ("vibratoRateHz", 5.5f);
    m.expression.vibratoDelayMs = e.getProperty ("vibratoDelayMs", 280.0f);
    m.expression.breathiness = e.getProperty ("breathiness", 0.15f);
    m.expression.portamentoMs = e.getProperty ("portamentoMs", 70.0f);
    m.expression.tension = e.getProperty ("tension", 0.5f);
    m.expression.formantShift = e.getProperty ("formantShift", 0.0f);
    m.expression.humanize = e.getProperty ("humanize", 0.35f);

    auto s = v.getProperty ("stats", {});
    m.stats.seconds = s.getProperty ("seconds", 0.0);
    m.stats.medianMidi = s.getProperty ("medianMidi", 60.0f);
    m.stats.minMidi = s.getProperty ("minMidi", 48.0f);
    m.stats.maxMidi = s.getProperty ("maxMidi", 72.0f);
    m.stats.vibratoRate = s.getProperty ("vibratoRate", 5.5f);
    m.stats.vibratoDepth = s.getProperty ("vibratoDepth", 30.0f);

    auto n = v.getProperty ("neural", {});
    m.neuralModel = n.getProperty ("model", "").toString();
    m.neuralSampleRate = n.getProperty ("sampleRate", 48000.0);
    m.neuralHop = n.getProperty ("hop", 960);

    if (auto* arr = v.getProperty ("samples", {}).getArray())
        for (auto& x : *arr) m.samples.add (x.toString());
    return m;
}

bool VoiceModel::save() const
{
    if (folder == juce::File()) return false;
    folder.createDirectory();
    return folder.getChildFile ("voice.json").replaceWithText (juce::JSON::toString (toJson(), false));
}

VoiceModel VoiceModel::builtinDefault()
{
    VoiceModel m;
    m.name = "VoxForge Default";
    m.id = "builtin-default";
    m.engine = "lite";
    m.notes = "Built-in neutral voice (no cloning).";
    return m;
}

//==============================================================================
PresetManager::PresetManager() { rescan(); }

juce::String PresetManager::sanitise (const juce::String& name)
{
    auto s = name.trim().retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-()");
    return s.isEmpty() ? juce::String ("Voice") : s.substring (0, 64);
}

void PresetManager::rescan()
{
    list.clear();
    list.push_back (VoiceModel::builtinDefault());
    for (auto& dir : Paths::voicesDir().findChildFiles (juce::File::findDirectories, false))
    {
        auto json = dir.getChildFile ("voice.json");
        if (! json.existsAsFile()) continue;
        auto v = juce::JSON::parse (json);
        if (! v.isObject()) continue;
        list.push_back (VoiceModel::fromJson (v, dir));
    }
    std::sort (list.begin() + 1, list.end(), [] (const VoiceModel& a, const VoiceModel& b)
               { return a.name.compareIgnoreCase (b.name) < 0; });
}

const VoiceModel* PresetManager::find (const juce::String& key) const
{
    for (auto& v : list) if (v.id == key) return &v;
    for (auto& v : list) if (v.name.equalsIgnoreCase (key)) return &v;
    return nullptr;
}

juce::File PresetManager::createVoiceFolder (const juce::String& name) const
{
    auto base = Paths::voicesDir().getChildFile (sanitise (name));
    auto f = base;
    for (int i = 2; f.exists(); ++i) f = Paths::voicesDir().getChildFile (sanitise (name) + " " + juce::String (i));
    f.createDirectory();
    return f;
}

bool PresetManager::saveVoice (VoiceModel& v)
{
    if (v.id.isEmpty()) v.id = juce::Uuid().toString();
    if (v.created.isEmpty()) v.created = juce::Time::getCurrentTime().toISO8601 (true);
    if (v.folder == juce::File()) v.folder = createVoiceFolder (v.name);
    const bool ok = v.save();
    rescan();
    return ok;
}

bool PresetManager::renameVoice (const juce::String& id, const juce::String& newName)
{
    for (auto& v : list)
    {
        if (v.id != id || v.id == "builtin-default") continue;
        auto copy = v;
        copy.name = newName.trim();
        auto target = Paths::voicesDir().getChildFile (sanitise (newName));
        if (! target.exists() && copy.folder.moveFileTo (target)) copy.folder = target;
        const bool ok = copy.save();
        rescan();
        return ok;
    }
    return false;
}

bool PresetManager::deleteVoice (const juce::String& id)
{
    for (auto& v : list)
    {
        if (v.id != id || v.id == "builtin-default") continue;
        const bool ok = v.folder.deleteRecursively();
        rescan();
        return ok;
    }
    return false;
}

bool PresetManager::duplicateVoice (const juce::String& id, const juce::String& newName)
{
    for (auto& v : list)
    {
        if (v.id != id) continue;
        auto copy = v;
        copy.name = newName;
        copy.id = juce::Uuid().toString();
        copy.created = juce::Time::getCurrentTime().toISO8601 (true);
        copy.folder = createVoiceFolder (newName);
        if (v.folder.isDirectory())
            v.folder.copyDirectoryTo (copy.folder);
        const bool ok = copy.save();
        rescan();
        return ok;
    }
    return false;
}

bool PresetManager::exportVoice (const juce::String& id, const juce::File& zipFile) const
{
    auto* v = find (id);
    if (v == nullptr || ! v->folder.isDirectory()) return false;
    juce::ZipFile::Builder b;
    for (auto& f : v->folder.findChildFiles (juce::File::findFiles, true))
        b.addFile (f, 6, f.getRelativePathFrom (v->folder).replaceCharacter ('\\', '/'));
    zipFile.deleteFile();
    juce::FileOutputStream os (zipFile);
    if (! os.openedOk()) return false;
    return b.writeToStream (os, nullptr);
}

juce::Result PresetManager::importVoice (const juce::File& zipFile)
{
    juce::ZipFile zip (zipFile);
    const int idx = zip.getIndexOfFileName ("voice.json");
    if (idx < 0) return juce::Result::fail ("Not a VoxForge voice file (voice.json missing).");

    std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (idx));
    auto v = juce::JSON::parse (in != nullptr ? in->readEntireStreamAsString() : juce::String());
    const auto name = v.getProperty ("name", zipFile.getFileNameWithoutExtension()).toString();

    auto folder = createVoiceFolder (name);
    auto r = zip.uncompressTo (folder, true);
    if (r.failed()) { folder.deleteRecursively(); return r; }

    // give the imported copy a fresh id so it never clashes with an existing preset
    auto m = VoiceModel::fromJson (v, folder);
    if (find (m.id) != nullptr) m.id = juce::Uuid().toString();
    m.save();
    rescan();
    return juce::Result::ok();
}

} // namespace vf
