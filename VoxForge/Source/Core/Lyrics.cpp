#include "Lyrics.h"
#include "Phonemes.h"

namespace vf
{

//==============================================================================
// Built-in singing dictionary (common lyric words, ARPAbet without stress).
static const char* kBuiltinDict =
    "a:ax;i:ay;i'm:ay m;im:ay m;i'll:ay l;i've:ay v;me:m iy;my:m ay;mine:m ay n;you:y uw;your:y ao r;you're:y ao r;"
    "we:w iy;us:ah s;our:aw er;they:dh ey;them:dh eh m;their:dh eh r;there:dh eh r;the:dh ax;this:dh ih s;that:dh ae t;"
    "these:dh iy z;those:dh ow z;then:dh eh n;than:dh ae n;though:dh ow;he:hh iy;she:sh iy;his:hh ih z;her:hh er;"
    "it:ih t;its:ih t s;is:ih z;are:aa r;was:w ah z;were:w er;be:b iy;been:b ih n;am:ae m;do:d uw;does:d ah z;done:d ah n;"
    "to:t uw;too:t uw;two:t uw;of:ah v;for:f ao r;from:f r ah m;with:w ih dh;without:w ih dh aw t;in:ih n;on:aa n;at:ae t;"
    "and:ae n d;or:ao r;but:b ah t;so:s ow;if:ih f;as:ae z;by:b ay;up:ah p;down:d aw n;out:aw t;all:ao l;no:n ow;not:n aa t;"
    "oh:ow;ooh:uw;ah:aa;yeah:y eh;hey:hh ey;la:l aa;na:n aa;da:d aa;love:l ah v;heart:hh aa r t;night:n ay t;light:l ay t;"
    "life:l ay f;time:t ay m;world:w er l d;baby:b ey b iy;girl:g er l;boy:b oy;one:w ah n;only:ow n l iy;know:n ow;"
    "never:n eh v er;ever:eh v er;forever:f er eh v er;feel:f iy l;feeling:f iy l ih ng;fire:f ay er;higher:hh ay er;"
    "give:g ih v;get:g eh t;go:g ow;gone:g ao n;come:k ah m;coming:k ah m ih ng;some:s ah m;someone:s ah m w ah n;"
    "say:s ey;said:s eh d;see:s iy;saw:s ao;sky:s k ay;eyes:ay z;eye:ay;away:ax w ey;again:ax g eh n;alone:ax l ow n;"
    "dream:d r iy m;dreams:d r iy m z;dance:d ae n s;tonight:t ax n ay t;stay:s t ey;way:w ey;day:d ey;away:ax w ey;"
    "home:hh ow m;hold:hh ow l d;soul:s ow l;rain:r ey n;fall:f ao l;falling:f ao l ih ng;call:k ao l;tell:t eh l;"
    "want:w aa n t;need:n iy d;can:k ae n;can't:k ae n t;could:k uh d;would:w uh d;should:sh uh d;will:w ih l;won't:w ow n t;"
    "don't:d ow n t;what:w ah t;when:w eh n;where:w eh r;why:w ay;who:hh uw;how:hh aw;now:n aw;here:hh ih r;"
    "free:f r iy;sweet:s w iy t;beautiful:b y uw t ih f ax l;everything:eh v r iy th ih ng;nothing:n ah th ih ng;"
    "something:s ah m th ih ng;believe:b ih l iy v;together:t ax g eh dh er;voice:v oy s;sing:s ih ng;song:s ao ng;"
    "music:m y uw z ih k;sound:s aw n d;breathe:b r iy dh;breath:b r eh th;water:w ao t er;ocean:ow sh ax n;"
    "fly:f l ay;run:r ah n;burn:b er n;turn:t er n;mind:m ay n d;find:f ay n d;kind:k ay n d;back:b ae k;"
    "touch:t ah ch;much:m ah ch;cry:k r ay;tears:t ih r z;smile:s m ay l;people:p iy p ax l;little:l ih t ax l;"
    "angel:ey n jh ax l;heaven:hh eh v ax n;shine:sh ay n;star:s t aa r;stars:s t aa r z;moon:m uw n;sun:s ah n;"
    "summer:s ah m er;winter:w ih n t er;over:ow v er;under:ah n d er;inside:ih n s ay d;outside:aw t s ay d;"
    "remember:r ih m eh m b er;tomorrow:t ax m aa r ow;yesterday:y eh s t er d ey;goodbye:g uh d b ay;"
    "hello:hh ax l ow;good:g uh d;bad:b ae d;right:r ay t;wrong:r ao ng;young:y ah ng;old:ow l d;new:n uw;true:t r uw;"
    "blue:b l uw;through:th r uw;thought:th ao t;think:th ih ng k;thing:th ih ng;things:th ih ng z;"
    "have:hh ae v;had:hh ae d;has:hh ae z;make:m ey k;made:m ey d;take:t ey k;break:b r ey k;"
    "every:eh v r iy;very:v eh r iy;more:m ao r;before:b ih f ao r;door:d ao r;floor:f l ao r;";

static std::string key (const juce::String& s) { return s.toLowerCase().toStdString(); }

static juce::StringArray splitPhonemes (const juce::String& s)
{
    juce::StringArray out;
    for (auto& t : juce::StringArray::fromTokens (s, " ,\t", ""))
    {
        auto p = t.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz");
        if (p.isNotEmpty() && phonemeIndex (p) >= 0)
        {
            auto idx = phonemeIndex (p);
            out.add (phonemeAt (idx).name);
        }
    }
    return out;
}

Lexicon& Lexicon::get()
{
    static Lexicon instance;
    return instance;
}

Lexicon::Lexicon()
{
    for (auto& entry : juce::StringArray::fromTokens (kBuiltinDict, ";", ""))
    {
        auto word = entry.upToFirstOccurrenceOf (":", false, false).trim();
        auto ph   = entry.fromFirstOccurrenceOf (":", false, false);
        if (word.isNotEmpty())
            builtin[key (word)] = splitPhonemes (ph);
    }
}

int Lexicon::loadDictionary (const juce::File& file)
{
    if (! file.existsAsFile()) return 0;
    juce::StringArray lines;
    file.readLines (lines);

    std::unordered_map<std::string, juce::StringArray> d;
    for (auto& line : lines)
    {
        if (line.startsWith (";;;") || line.isEmpty()) continue;
        auto word = line.upToFirstOccurrenceOf (" ", false, false).trim();
        auto ph   = line.fromFirstOccurrenceOf (" ", false, false).upToFirstOccurrenceOf ("#", false, false);
        if (word.containsChar ('('))     // alternate pronunciations "WORD(2)" -> keep first
            continue;
        auto phs = splitPhonemes (ph);
        if (word.isNotEmpty() && ! phs.isEmpty())
            d[key (word)] = phs;
    }

    const juce::ScopedLock sl (lock);
    user = std::move (d);
    userDictSize = (int) user.size();
    return userDictSize;
}

juce::StringArray Lexicon::lookup (const juce::String& wordIn) const
{
    auto word = wordIn.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz'");
    if (word.isEmpty()) return {};
    {
        const juce::ScopedLock sl (lock);
        if (auto it = user.find (key (word)); it != user.end()) return it->second;
    }
    if (auto it = builtin.find (key (word)); it != builtin.end()) return it->second;

    // strip possessive / contraction and retry
    auto noApos = word.removeCharacters ("'");
    {
        const juce::ScopedLock sl (lock);
        if (auto it = user.find (key (noApos)); it != user.end()) return it->second;
    }
    if (auto it = builtin.find (key (noApos)); it != builtin.end()) return it->second;
    return letterToSound (noApos);
}

//==============================================================================
// Letter-to-sound rules (compact English approximation).
static bool isVowelLetter (juce::juce_wchar c) { return juce::String ("aeiouy").containsChar (c); }

juce::StringArray Lexicon::letterToSound (const juce::String& wIn) const
{
    juce::StringArray out;
    const juce::String w = wIn.toLowerCase();
    const int n = w.length();
    auto at = [&] (int i) -> juce::juce_wchar { return (i >= 0 && i < n) ? w[i] : 0; };
    auto add = [&] (const char* ph) { for (auto& p : juce::StringArray::fromTokens (ph, " ", "")) out.add (p); };
    auto starts = [&] (int i, const char* s) { return w.substring (i).startsWith (s); };

    // count vowel groups (for silent-e decisions)
    int vowelGroups = 0;
    for (int i = 0; i < n; ++i)
        if (isVowelLetter (at (i)) && ! isVowelLetter (at (i - 1))) ++vowelGroups;

    int i = 0;
    while (i < n)
    {
        const auto c = at (i);
        const bool atEnd = (i == n - 1);

        // --- multi-letter patterns -------------------------------------------
        struct R { const char* pat; const char* ph; bool startOnly; bool endOnly; };
        static const R rules[] =
        {
            { "tion", "sh ax n", false, false }, { "sion", "zh ax n", false, false }, { "ture", "ch er", false, false },
            { "ight", "ay t", false, false },    { "igh", "ay", false, false },        { "ough", "ao", false, false },
            { "augh", "ao", false, false },      { "eigh", "ey", false, false },       { "tch", "ch", false, false },
            { "dge", "jh", false, false },       { "sch", "s k", false, false },       { "ear", "ih r", false, false },
            { "air", "eh r", false, false },     { "are", "eh r", false, true },       { "ore", "ao r", false, true },
            { "ire", "ay er", false, true },     { "ure", "y uh r", false, true },     { "kn", "n", true, false },
            { "wr", "r", true, false },          { "gn", "n", false, true },           { "mb", "m", false, true },
            { "ck", "k", false, false },         { "ph", "f", false, false },          { "sh", "sh", false, false },
            { "ch", "ch", false, false },        { "th", "th", false, false },         { "wh", "w", false, false },
            { "ng", "ng", false, false },        { "nk", "ng k", false, false },       { "qu", "k w", false, false },
            { "ee", "iy", false, false },        { "ea", "iy", false, false },         { "oo", "uw", false, false },
            { "ou", "aw", false, false },        { "ow", "ow", false, true },          { "ow", "aw", false, false },
            { "ai", "ey", false, false },        { "ay", "ey", false, false },         { "oi", "oy", false, false },
            { "oy", "oy", false, false },        { "au", "ao", false, false },         { "aw", "ao", false, false },
            { "ie", "ay", false, true },         { "ie", "iy", false, false },         { "ue", "uw", false, false },
            { "ew", "uw", false, false },        { "ei", "ey", false, false },         { "ey", "iy", false, true },
            { "oa", "ow", false, false },        { "ar", "aa r", false, false },       { "er", "er", false, false },
            { "ir", "er", false, false },        { "ur", "er", false, false },         { "or", "ao r", false, false },
        };

        bool matched = false;
        for (auto& r : rules)
        {
            const int len = (int) strlen (r.pat);
            if (! starts (i, r.pat)) continue;
            if (r.startOnly && i != 0) continue;
            if (r.endOnly && i + len != n) continue;
            // "er"/"ar"/"or" followed by a vowel -> let the r start the next syllable
            if ((juce::String (r.pat) == "er" || juce::String (r.pat) == "ar" || juce::String (r.pat) == "or")
                && isVowelLetter (at (i + 2)) && at (i + 2) != 'y')
                continue;
            add (r.ph);
            i += len;
            matched = true;
            break;
        }
        if (matched) continue;

        // --- final "le" after consonant: "little" -> l ax l ---------------------
        if (c == 'l' && at (i + 1) == 'e' && i + 2 == n && i > 0 && ! isVowelLetter (at (i - 1)))
        {
            add ("ax l"); i += 2; continue;
        }

        // --- final "ed" ------------------------------------------------------------
        if (c == 'e' && at (i + 1) == 'd' && i + 2 == n && vowelGroups > 1)
        {
            const auto p = at (i - 1);
            if (p == 't' || p == 'd') add ("ih d");
            else if (juce::String ("pkfsxc").containsChar (p)) add ("t");
            else add ("d");
            i += 2; continue;
        }

        if (isVowelLetter (c))
        {
            // silent final e
            if (c == 'e' && atEnd && vowelGroups > 1) { ++i; continue; }
            if (c == 'e' && atEnd && n <= 2)          { add ("iy"); ++i; continue; }   // "me", "he"

            // magic e: V C e (end) or V C e s/d (end)
            const bool magic = ! isVowelLetter (at (i + 1)) && at (i + 1) != 0 && at (i + 2) == 'e'
                               && (i + 3 == n || ((at (i + 3) == 's' || at (i + 3) == 'd') && i + 4 == n))
                               && at (i + 1) != 'r' && c != 'y';
            if (c == 'y')
            {
                if (i == 0)                         add ("y");
                else if (atEnd && vowelGroups <= 1) add ("ay");
                else if (atEnd)                     add ("iy");
                else                                add ("ih");
                ++i; continue;
            }
            switch (c)
            {
                case 'a': add (magic ? "ey" : "ae"); break;
                case 'e': add (magic ? "iy" : "eh"); break;
                case 'i': add (magic ? "ay" : "ih"); break;
                case 'o': add (magic ? "ow" : (atEnd ? "ow" : "aa")); break;
                case 'u': add (magic ? "uw" : "ah"); break;
                default: break;
            }
            ++i; continue;
        }

        // --- consonants --------------------------------------------------------------
        if (c == at (i + 1) && c != 'c')   // doubled consonant
        {
            ++i; continue;
        }
        const auto nx = at (i + 1);
        switch (c)
        {
            case 'b': add ("b"); break;
            case 'c': add ((nx == 'e' || nx == 'i' || nx == 'y') ? "s" : "k"); if (nx == 'c') ++i; break;
            case 'd': add ("d"); break;
            case 'f': add ("f"); break;
            case 'g': add ((nx == 'e' || nx == 'i' || nx == 'y') && i > 0 ? "jh" : "g"); break;
            case 'h': add ("hh"); break;
            case 'j': add ("jh"); break;
            case 'k': add ("k"); break;
            case 'l': add ("l"); break;
            case 'm': add ("m"); break;
            case 'n': add ("n"); break;
            case 'p': add ("p"); break;
            case 'q': add ("k"); break;
            case 'r': add ("r"); break;
            case 's': add ((atEnd && i > 0 && ! juce::String ("ptkf").containsChar (at (i - 1))) ? "z" : "s"); break;
            case 't': add ("t"); break;
            case 'v': add ("v"); break;
            case 'w': add ("w"); break;
            case 'x': add (i == 0 ? "z" : "k s"); break;
            case 'z': add ("z"); break;
            default: break;
        }
        ++i;
    }

    // guarantee at least one vowel
    bool hasVowel = false;
    for (auto& p : out) hasVowel |= isVowel (phonemeIndex (p));
    if (! hasVowel) out.add ("ax");
    return out;
}

//==============================================================================
static bool legalOnset (const juce::StringArray& c)
{
    if (c.size() <= 1) return c.size() == 1 ? c[0] != "ng" : true;
    static const juce::StringArray two { "p r","p l","b r","b l","t r","d r","k r","k l","g r","g l","f r","f l",
                                         "th r","sh r","s p","s t","s k","s m","s n","s l","s w","t w","d w","k w",
                                         "p y","b y","k y","m y","f y","v y","hh y","g w" };
    static const juce::StringArray three { "s p r","s p l","s t r","s k r","s k w","s k l","s p y","s k y" };
    auto j = c.joinIntoString (" ");
    return c.size() == 2 ? two.contains (j) : (c.size() == 3 ? three.contains (j) : false);
}

std::vector<juce::StringArray> syllabifyPhonemes (const juce::StringArray& ph)
{
    std::vector<int> nuclei;
    for (int i = 0; i < ph.size(); ++i)
        if (isVowel (phonemeIndex (ph[i]))) nuclei.push_back (i);

    std::vector<juce::StringArray> out;
    if (nuclei.empty()) { out.push_back (ph); return out; }

    int start = 0;
    for (size_t k = 0; k < nuclei.size(); ++k)
    {
        int end;   // exclusive end of this syllable
        if (k + 1 < nuclei.size())
        {
            const int a = nuclei[k], b = nuclei[k + 1];
            juce::StringArray cons;
            for (int i = a + 1; i < b; ++i) cons.add (ph[i]);
            // maximal legal onset for the next syllable
            int onsetLen = 0;
            for (int len = cons.size(); len >= 0; --len)
            {
                juce::StringArray tail;
                for (int i = cons.size() - len; i < cons.size(); ++i) tail.add (cons[i]);
                if (legalOnset (tail)) { onsetLen = len; break; }
            }
            end = b - onsetLen;
        }
        else end = ph.size();

        juce::StringArray syl;
        for (int i = start; i < end; ++i) syl.add (ph[i]);
        out.push_back (syl);
        start = end;
    }
    return out;
}

juce::StringArray splitWordForDisplay (const juce::String& word, int n)
{
    juce::StringArray out;
    if (n <= 1 || word.length() < 2) { out.add (word); for (int i = 1; i < n; ++i) out.add ("-"); return out; }

    // vowel groups
    struct G { int s, e; };
    std::vector<G> groups;
    const auto lw = word.toLowerCase();
    for (int i = 0; i < lw.length();)
    {
        if (isVowelLetter (lw[i]) && ! (lw[i] == 'y' && i == 0))
        {
            int j = i;
            while (j < lw.length() && isVowelLetter (lw[j])) ++j;
            groups.push_back ({ i, j });
            i = j;
        }
        else ++i;
    }
    // drop silent final e
    if ((int) groups.size() > n && ! groups.empty() && groups.back().s == lw.length() - 1 && lw.getLastCharacter() == 'e')
        groups.pop_back();

    if ((int) groups.size() == n)
    {
        int prev = 0;
        for (int k = 0; k + 1 < n; ++k)
        {
            const int c0 = groups[(size_t) k].e, c1 = groups[(size_t) k + 1].s;
            const int consonants = c1 - c0;
            const int cut = consonants <= 1 ? c0 : c0 + consonants / 2;
            out.add (word.substring (prev, cut));
            prev = cut;
        }
        out.add (word.substring (prev));
        return out;
    }

    // fallback: even split
    const int len = word.length();
    for (int k = 0; k < n; ++k)
        out.add (word.substring (k * len / n, (k + 1) * len / n));
    return out;
}

juce::StringArray phonemesForText (const juce::String& textIn)
{
    auto text = textIn.trim();
    if (text.isEmpty() || text == "-") return {};
    if (text.startsWithChar ('[') )
        return splitPhonemes (text.removeCharacters ("[]"));
    juce::StringArray all;
    for (auto& w : juce::StringArray::fromTokens (text, " ", ""))
        all.addArray (Lexicon::get().lookup (w));
    return all;
}

std::vector<Syllable> lyricsToSyllables (const juce::String& textIn)
{
    std::vector<Syllable> out;

    // Normalise: keep letters, apostrophes, hyphens, brackets
    juce::String text;
    bool inBracket = false;
    for (auto c : textIn)
    {
        if (c == '[') inBracket = true;
        if (c == ']') inBracket = false;
        if (inBracket && c == ' ') { text << '_'; continue; }     // protect spaces inside [...]
        if (juce::CharacterFunctions::isLetter (c) || c == '\'' || c == '-' || c == '[' || c == ']' || c == '_' )
            text << c;
        else if (c == 0x2013 || c == 0x2014)  text << '-';
        else text << ' ';
    }

    for (auto& token : juce::StringArray::fromTokens (text, " ", ""))
    {
        if (token.isEmpty()) continue;
        if (token.containsOnly ("-"))
        {
            for (int k = 0; k < token.length(); ++k) { Syllable s; s.text = "-"; s.isHold = true; out.push_back (s); }
            continue;
        }
        if (token.startsWithChar ('['))
        {
            Syllable s;
            s.text = token.replaceCharacter ('_', ' ');
            s.phonemes = splitPhonemes (s.text.removeCharacters ("[]"));
            out.push_back (s);
            continue;
        }

        auto parts = juce::StringArray::fromTokens (token, "-", "");
        parts.removeEmptyStrings();
        const auto whole = parts.joinIntoString ("");
        auto ph = Lexicon::get().lookup (whole);
        auto syls = syllabifyPhonemes (ph);

        if (parts.size() > 1)
        {
            if ((int) syls.size() == parts.size())
            {
                for (int k = 0; k < parts.size(); ++k)
                    out.push_back ({ parts[k], syls[(size_t) k], false });
            }
            else
            {
                for (auto& p : parts)
                    out.push_back ({ p, Lexicon::get().lookup (p), false });
            }
        }
        else
        {
            auto disp = splitWordForDisplay (whole, (int) syls.size());
            for (size_t k = 0; k < syls.size(); ++k)
                out.push_back ({ disp[(int) k], syls[k], false });
        }
        // trailing hyphen ("love-") means: hold after the word
        if (token.endsWithChar ('-')) { Syllable s; s.text = "-"; s.isHold = true; out.push_back (s); }
    }
    return out;
}

void applyLyricsToNotes (std::vector<Note>& notes, const juce::String& text, int firstNote)
{
    auto syls = lyricsToSyllables (text);
    size_t k = 0;
    for (int i = juce::jmax (0, firstNote); i < (int) notes.size(); ++i)
    {
        auto& n = notes[(size_t) i];
        if (k < syls.size())
        {
            n.lyric = syls[k].text;
            n.phonemes = syls[k].isHold ? juce::String() : syls[k].phonemes.joinIntoString (" ");
            ++k;
        }
        else
        {
            n.lyric = "la";
            n.phonemes = "l aa";
        }
    }
}

juce::StringArray phonemesForNote (const Note& note)
{
    const auto l = note.lyric.trim();
    if (l == "-" || l == "+" || l == "~") return {};
    if (note.phonemes.isNotEmpty()) return splitPhonemes (note.phonemes);
    auto ph = phonemesForText (l);
    if (ph.isEmpty()) ph.add ("aa");
    return ph;
}

} // namespace vf
