#include "EngineClient.h"
#include "VoicePreset.h"

namespace vf
{

bool EngineClient::isInstalled()
{
    return ! Settings::get().resolveEngineCommand().isEmpty();
}

juce::String EngineClient::describeInstallation()
{
    auto cmd = Settings::get().resolveEngineCommand();
    if (cmd.isEmpty())
        return "VoxForge Engine not found. Install it (see manual) or set its location in Presets > Settings.";
    return "Engine: " + cmd.joinIntoString (" ");
}

EngineClient::Outcome EngineClient::run (const juce::StringArray& args, std::atomic<bool>* cancel,
                                         std::function<void (float, const juce::String&)> progress,
                                         std::function<void (const juce::String&)> logFn)
{
    Outcome o;
    auto cmd = Settings::get().resolveEngineCommand();
    if (cmd.isEmpty()) { o.error = describeInstallation(); return o; }
    cmd.addArray (args);

    juce::ChildProcess proc;
    if (! proc.start (cmd, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        o.error = "Could not start: " + cmd.joinIntoString (" ");
        return o;
    }

    juce::String pending;
    char buf[4096];
    auto handleLine = [&] (const juce::String& lineIn)
    {
        auto line = lineIn.trim();
        if (line.isEmpty()) return;
        auto v = line.startsWithChar ('{') ? juce::JSON::parse (line) : juce::var();
        if (! v.isObject())
        {
            o.log.add (line);
            if (logFn) logFn (line);
            return;
        }
        const auto type = v.getProperty ("type", "").toString();
        const auto msg = v.getProperty ("message", "").toString();
        if (type == "progress")      { if (progress) progress ((float) v.getProperty ("value", 0.0), msg); }
        else if (type == "result")   { o.result = v; o.ok = true; }
        else if (type == "error")    { o.error = msg; }
        else                         { o.log.add (msg); if (logFn) logFn (msg); }
    };

    while (true)
    {
        if (cancel != nullptr && cancel->load())
        {
            proc.kill();
            o.ok = false;
            o.error = "Cancelled";
            return o;
        }
        const int n = proc.readProcessOutput (buf, sizeof (buf));
        if (n <= 0)
        {
            if (! proc.isRunning()) break;
            juce::Thread::sleep (20);
            continue;
        }
        pending += juce::String::fromUTF8 (buf, n);
        int nl;
        while ((nl = pending.indexOfChar ('\n')) >= 0)
        {
            handleLine (pending.substring (0, nl));
            pending = pending.substring (nl + 1);
        }
    }
    handleLine (pending);

    const auto exitCode = proc.getExitCode();
    if (! o.ok && o.error.isEmpty())
        o.error = "Engine exited with code " + juce::String ((int) exitCode)
                  + (o.log.isEmpty() ? juce::String() : ": " + o.log[o.log.size() - 1]);
    return o;
}

//==============================================================================
EngineJob::EngineJob (juce::StringArray args, juce::String title)
    : juce::Thread ("VoxForge engine job"), arguments (std::move (args)), jobTitle (std::move (title)) {}

EngineJob::~EngineJob()
{
    cancelFlag = true;
    stopThread (5000);
}

void EngineJob::run()
{
    auto outcome = EngineClient::run (arguments, &cancelFlag,
        [this] (float p, const juce::String& m)
        {
            prog = p;
            const juce::ScopedLock sl (lock);
            statusText = m;
        },
        [this] (const juce::String& l)
        {
            const juce::ScopedLock sl (lock);
            log.add (l);
            if (log.size() > 500) log.removeRange (0, 100);
        });

    {
        const juce::ScopedLock sl (lock);
        resultVar = outcome.result;
        errorText = outcome.error;
        statusText = outcome.ok ? "Finished" : ("Failed: " + outcome.error);
    }
    ok = outcome.ok;
    prog = 1.0f;
    finished = true;
}

} // namespace vf
