#include "StemAnalyzer.h"
#include "EngineClient.h"
#include "VoicePreset.h"
#include "../DSP/AudioUtils.h"
#include "../Core/Lyrics.h"
#include "../Core/Timeline.h"

namespace vf
{

StemAnalyzer::StemAnalyzer() : juce::Thread ("VoxForge stem analysis") {}

StemAnalyzer::~StemAnalyzer()
{
    cancelFlag = true;
    stopThread (10000);
}

void StemAnalyzer::start (const juce::File& stem, const VocalToMidiOptions& opts, bool transcribe, const juce::String& language,
                          const juce::String& lyricsHint)
{
    cancelFlag = true;
    stopThread (10000);
    file = stem;
    options = opts;
    doTranscribe = transcribe;
    lang = language;
    // one line, no quotes: safe on the Windows command line
    hint = lyricsHint.replaceCharacters ("\r\n\t", "   ").removeCharacters ("\"\\").trim().substring (0, 800);
    cancelFlag = false;
    resultReady = false;
    prog = 0.0f;
    startThread (juce::Thread::Priority::normal);
}

StemAnalysisResult StemAnalyzer::takeResult()
{
    const juce::ScopedLock sl (lock);
    resultReady = false;
    return std::move (result);
}

void StemAnalyzer::run()
{
    StemAnalysisResult r;
    auto setStatus = [this] (const juce::String& s, float p) { prog = p; const juce::ScopedLock sl (lock); statusText = s; };

    setStatus ("Loading " + file.getFileName(), 0.02f);
    juce::AudioBuffer<float> mono; double sr = 0;
    if (! loadAudioMono (file, mono, sr))
    {
        r.error = "Cannot read " + file.getFullPathName();
    }
    else
    {
        r.stemSeconds = mono.getNumSamples() / sr;
        setStatus ("Vocal-to-MIDI: tracking pitch", 0.1f);
        r.analysis = vocalToMidi (mono, sr, options, &cancelFlag);
        if (cancelFlag) { r.error = "Cancelled"; }
        else
        {
            r.ok = true;
            setStatus ("Found " + juce::String ((int) r.analysis.notes.size()) + " notes", 0.4f);

            if (doTranscribe)
            {
                if (! EngineClient::isInstalled())
                {
                    r.transcriptionNote = "Lyrics extraction needs the VoxForge Engine (not installed). Type the lyrics in the editor.";
                }
                else
                {
                    setStatus ("Extracting lyrics (speech recognition)", 0.45f);
                    juce::StringArray args { "transcribe", "--audio", file.getFullPathName(),
                                             "--models-dir", Paths::modelsDir().getFullPathName(),
                                             "--language", lang.isEmpty() ? juce::String ("auto") : lang,
                                             "--model", Settings::get().lyricsModel() };
                    if (hint.isNotEmpty()) { args.add ("--prompt"); args.add (hint); }
                    if (Settings::get().useGpu()) args.add ("--gpu");
                    auto o = EngineClient::run (args, &cancelFlag,
                                                [&] (float p, const juce::String& m) { setStatus ("Lyrics: " + m, 0.45f + 0.5f * p); });
                    if (o.ok)
                    {
                        r.transcript = o.result.getProperty ("text", "").toString();
                        if (auto* words = o.result.getProperty ("words", {}).getArray())
                            for (auto& w : *words)
                                r.words.push_back ({ w.getProperty ("word", "").toString().trim(),
                                                     (double) w.getProperty ("start", 0.0), (double) w.getProperty ("end", 0.0) });
                    }
                    else r.transcriptionNote = "Lyrics extraction failed: " + o.error;
                }
            }
        }
    }

    setStatus (r.ok ? "Analysis complete" : r.error, 1.0f);
    {
        const juce::ScopedLock sl (lock);
        result = std::move (r);
    }
    resultReady = true;
}

//==============================================================================
void notesFromAnalysis (const StemAnalysisResult& r, VocalProject& p)
{
    const double offsetSec = p.beatsToSeconds (p.stemOffsetBeats);
    p.notes.clear();
    for (auto& d : r.analysis.notes)
    {
        Note n;
        n.startBeat = p.secondsToBeats (d.t0 + offsetSec);
        n.lengthBeats = juce::jmax (0.05, p.secondsToBeats (d.t1 - d.t0));
        n.pitch = d.pitch;
        n.velocity = d.velocity;
        n.fromSource = true;
        n.srcStartSec = d.t0;
        n.srcEndSec = d.t1;
        n.lyric = juce::String::charToString ((juce::juce_wchar) 0x2022);   // "•" = keep the original sound
        n.srcLyric = n.lyric;
        n.phonemes = "aa";
        p.notes.push_back (n);
    }

    // f0 track for the piano roll overlay
    p.srcF0.clear();
    p.srcF0FrameRate = r.analysis.f0.frameRate;
    for (auto h : r.analysis.f0.hz) p.srcF0.push_back (hzToMidi (h));

    if (r.words.empty()) return;

    // ---- assign recognised words (with syllables) to notes -------------------------
    std::vector<bool> used (p.notes.size(), false);
    std::vector<double> wordEnd (p.notes.size(), -1.0);
    juce::StringArray lyricWords;
    size_t searchFrom = 0;
    for (auto& w : r.words)
    {
        auto text = w.text.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'");
        if (text.isEmpty()) continue;
        lyricWords.add (text.toLowerCase());
        auto syls = lyricsToSyllables (text.toLowerCase());
        if (syls.empty()) continue;

        std::vector<size_t> cand;
        for (size_t i = searchFrom; i < r.analysis.notes.size(); ++i)
        {
            const auto& d = r.analysis.notes[i];
            const double mid = 0.5 * (d.t0 + d.t1);
            if (mid >= w.t0 - 0.06 && mid <= w.t1 + 0.06 && ! used[i]) cand.push_back (i);
            if (d.t0 > w.t1 + 0.1) break;
        }
        if (cand.empty())
        {
            // nearest unused note after the previous word
            for (size_t i = searchFrom; i < r.analysis.notes.size(); ++i)
                if (! used[i] && r.analysis.notes[i].t1 >= w.t0 - 0.15) { cand.push_back (i); break; }
            if (cand.empty()) continue;
        }

        const size_t m = cand.size(), k = syls.size();
        if (m >= k)
        {
            for (size_t j = 0; j < m; ++j)
            {
                auto& n = p.notes[cand[j]];
                const size_t sj = j * k / m;
                const bool first = (j == 0) || (sj != (j - 1) * k / m);
                n.srcLyric = n.lyric = first ? syls[sj].text : juce::String ("-");
                n.phonemes = first ? syls[sj].phonemes.joinIntoString (" ") : juce::String();
            }
        }
        else
        {
            for (size_t j = 0; j < m; ++j)
            {
                auto& n = p.notes[cand[j]];
                if (j + 1 < m) { n.srcLyric = n.lyric = syls[j].text; n.phonemes = syls[j].phonemes.joinIntoString (" "); }
                else
                {
                    juce::String t; juce::StringArray ph;
                    for (size_t q = j; q < k; ++q) { t << syls[q].text; ph.addArray (syls[q].phonemes); }
                    n.srcLyric = n.lyric = t;
                    n.phonemes = ph.joinIntoString (" ");
                }
            }
        }
        for (auto c : cand) { used[c] = true; wordEnd[c] = w.t1; }
        searchFrom = cand.back() + 1;
    }

    // notes right after a word without own text are melisma holds of that word
    for (size_t i = 1; i < p.notes.size(); ++i)
        if (! used[i] && used[i - 1] && r.analysis.notes[i].t0 - r.analysis.notes[i - 1].t1 < 0.05
            && r.analysis.notes[i].t0 < wordEnd[i - 1] + 0.15)
        {
            p.notes[i].lyric = p.notes[i].srcLyric = "-";
            p.notes[i].phonemes = {};
            used[i] = true;
            wordEnd[i] = wordEnd[i - 1];
        }

    p.lyricsText = lyricWords.joinIntoString (" ");
}

} // namespace vf
