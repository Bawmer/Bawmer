#include "Voicebox.h"
#include "VoicePreset.h"

namespace vf::voicebox
{

namespace
{
    struct ParsedUrl { juce::String host; int port = 80; juce::String path; bool valid = false; };

    ParsedUrl parse (const juce::String& url)
    {
        ParsedUrl p;
        auto u = url.trim();
        if (! u.startsWithIgnoreCase ("http://")) return p;      // local service: plain HTTP only
        u = u.substring (7);
        const int slash = u.indexOfChar ('/');
        auto hostPort = slash >= 0 ? u.substring (0, slash) : u;
        p.path = slash >= 0 ? u.substring (slash) : "/";
        const int colon = hostPort.lastIndexOfChar (':');
        p.host = colon >= 0 ? hostPort.substring (0, colon) : hostPort;
        if (colon >= 0) p.port = hostPort.substring (colon + 1).getIntValue();
        if (p.host == "localhost") p.host = "127.0.0.1";
        p.valid = p.host.isNotEmpty() && p.port > 0 && p.port < 65536;
        return p;
    }

    bool decodeChunked (const char* data, size_t size, juce::MemoryBlock& out)
    {
        size_t pos = 0;
        while (pos < size)
        {
            size_t lineEnd = pos;
            while (lineEnd + 1 < size && ! (data[lineEnd] == '\r' && data[lineEnd + 1] == '\n')) ++lineEnd;
            if (lineEnd + 1 >= size) return false;
            const auto sizeLine = juce::String (data + pos, lineEnd - pos).upToFirstOccurrenceOf (";", false, false).trim();
            const auto chunk = (size_t) sizeLine.getHexValue64();
            pos = lineEnd + 2;
            if (chunk == 0) return true;
            if (pos + chunk > size) return false;
            out.append (data + pos, chunk);
            pos += chunk + 2;                                   // chunk data + CRLF
        }
        return false;
    }

    juce::File cacheDir() { return Paths::cacheDir().getChildFile ("voicebox"); }

    void trimCache()
    {
        auto files = cacheDir().findChildFiles (juce::File::findFiles, false, "*.wav");
        if (files.size() <= 400) return;
        std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
                   { return a.getLastModificationTime() < b.getLastModificationTime(); });
        for (int i = 0; i < files.size() - 400; ++i) files.getReference (i).deleteFile();
    }
}

HttpResponse request (const juce::String& method, const juce::String& url, const juce::String& jsonBody,
                      int timeoutMs, std::atomic<bool>* cancel)
{
    HttpResponse r;
    const auto u = parse (url);
    if (! u.valid) { r.error = "Voicebox URL must look like http://127.0.0.1:17493"; return r; }

    juce::StreamingSocket sock;
    if (! sock.connect (u.host, u.port, juce::jmin (timeoutMs, 4000)))
    {
        r.error = "cannot connect to Voicebox at " + u.host + ":" + juce::String (u.port) + " (is the Voicebox app running?)";
        return r;
    }

    const auto body = jsonBody.toStdString();
    juce::String head;
    head << method << " " << u.path << " HTTP/1.1\r\n"
         << "Host: " << u.host << ":" << u.port << "\r\n"
         << "User-Agent: VoxForge\r\n"
         << "Accept: */*\r\n"
         << "Connection: close\r\n";
    if (method == "POST")
        head << "Content-Type: application/json\r\nContent-Length: " << (int) body.size() << "\r\n";
    head << "\r\n";
    const auto headStd = head.toStdString();
    if (sock.write (headStd.data(), (int) headStd.size()) != (int) headStd.size()
        || (! body.empty() && sock.write (body.data(), (int) body.size()) != (int) body.size()))
    {
        r.error = "could not send the request to Voicebox";
        return r;
    }

    // read until the server closes the connection (Connection: close)
    juce::MemoryBlock raw;
    char buf[16384];
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    for (;;)
    {
        if (cancel != nullptr && cancel->load()) { r.error = "cancelled"; return r; }
        if (juce::Time::getMillisecondCounterHiRes() > deadline) { r.error = "Voicebox did not answer in time"; return r; }
        const int ready = sock.waitUntilReady (true, 250);
        if (ready < 0) break;
        if (ready == 0) continue;
        const int n = sock.read (buf, (int) sizeof (buf), false);
        if (n <= 0) break;
        raw.append (buf, (size_t) n);
    }

    const char* d = static_cast<const char*> (raw.getData());
    const size_t size = raw.getSize();
    size_t headerEnd = 0;
    for (size_t i = 0; i + 3 < size; ++i)
        if (d[i] == '\r' && d[i + 1] == '\n' && d[i + 2] == '\r' && d[i + 3] == '\n') { headerEnd = i; break; }
    if (headerEnd == 0) { r.error = raw.isEmpty() ? "Voicebox closed the connection without answering" : "malformed HTTP response"; return r; }

    const auto headers = juce::StringArray::fromLines (juce::String (d, headerEnd));
    r.status = headers[0].fromFirstOccurrenceOf (" ", false, false).getIntValue();
    bool chunked = false;
    for (auto& h : headers)
    {
        const auto key = h.upToFirstOccurrenceOf (":", false, false).trim().toLowerCase();
        const auto val = h.fromFirstOccurrenceOf (":", false, false).trim();
        if (key == "transfer-encoding" && val.containsIgnoreCase ("chunked")) chunked = true;
        if (key == "content-type") r.contentType = val;
    }
    const char* payload = d + headerEnd + 4;
    const size_t payloadSize = size - headerEnd - 4;
    if (chunked)
    {
        if (! decodeChunked (payload, payloadSize, r.body)) r.error = "incomplete response from Voicebox";
    }
    else
    {
        r.body.append (payload, payloadSize);
    }
    return r;
}

static juce::String describe (const HttpResponse& r)
{
    if (r.error.isNotEmpty()) return r.error;
    auto detail = r.body.toString().substring (0, 300);
    if (auto v = juce::JSON::parse (detail); v.hasProperty ("detail")) detail = v["detail"].toString();
    return "Voicebox HTTP " + juce::String (r.status) + (detail.isNotEmpty() ? ": " + detail : juce::String());
}

bool health (const juce::String& baseUrl, juce::String& error)
{
    auto r = request ("GET", baseUrl.trimCharactersAtEnd ("/") + "/health", {}, 8000);
    if (! r.ok()) { error = describe (r); return false; }
    return true;
}

bool listProfiles (const juce::String& baseUrl, std::vector<Profile>& out, juce::String& error)
{
    out.clear();
    auto r = request ("GET", baseUrl.trimCharactersAtEnd ("/") + "/profiles", {}, 10000);
    if (! r.ok()) { error = describe (r); return false; }
    const auto v = juce::JSON::parse (r.body.toString());
    if (! v.isArray()) { error = "unexpected answer from Voicebox /profiles"; return false; }
    for (auto& p : *v.getArray())
        out.push_back ({ p["id"].toString(), p["name"].toString(),
                         p.hasProperty ("language") ? p["language"].toString() : juce::String ("en") });
    return true;
}

bool speak (const juce::String& baseUrl, const juce::String& profileId, const juce::String& text,
            const juce::String& language, juce::AudioBuffer<float>& out, double& sampleRate,
            juce::String& error, std::atomic<bool>* cancel, bool* fromCache)
{
    if (fromCache != nullptr) *fromCache = false;
    const auto lang = language.isNotEmpty() ? language : juce::String ("en");
    const auto key = juce::String::toHexString ((profileId + "|" + lang + "|" + text).hashCode64());
    const auto cached = cacheDir().getChildFile (key + ".wav");

    juce::WavAudioFormat wav;
    auto decode = [&] (std::unique_ptr<juce::InputStream> in) -> bool
    {
        std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (in.release(), true));
        if (reader == nullptr || reader->lengthInSamples <= 0) return false;
        const int n = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) 600 * 48000);
        juce::AudioBuffer<float> tmp ((int) reader->numChannels, n);
        reader->read (&tmp, 0, n, 0, true, true);
        out.setSize (1, n);
        out.copyFrom (0, 0, tmp, 0, 0, n);
        for (int c = 1; c < tmp.getNumChannels(); ++c) out.addFrom (0, 0, tmp, c, 0, n);
        if (tmp.getNumChannels() > 1) out.applyGain (1.0f / (float) tmp.getNumChannels());
        sampleRate = reader->sampleRate;
        return true;
    };

    if (cached.existsAsFile() && decode (cached.createInputStream()))
    {
        cached.setLastModificationTime (juce::Time::getCurrentTime());
        if (fromCache != nullptr) *fromCache = true;
        return true;
    }

    auto body = new juce::DynamicObject();
    body->setProperty ("profile_id", profileId);
    body->setProperty ("text", text);
    body->setProperty ("language", lang);
    body->setProperty ("seed", 7);                  // deterministic: re-renders sound the same
    const auto json = juce::JSON::toString (juce::var (body), true);

    // TTS on a CPU can take a while for a long phrase
    auto r = request ("POST", baseUrl.trimCharactersAtEnd ("/") + "/generate/stream", json, 300000, cancel);
    if (! r.ok()) { error = describe (r); return false; }
    if (! decode (std::make_unique<juce::MemoryInputStream> (r.body, false)))
    {
        error = "Voicebox returned audio VoxForge cannot read (" + r.contentType + ")";
        return false;
    }
    cacheDir().createDirectory();
    cached.replaceWithData (r.body.getData(), r.body.getSize());
    trimCache();
    return true;
}

juce::String phraseText (const juce::StringArray& lyrics, const juce::Array<bool>& wordStarts)
{
    juce::String text;
    for (int i = 0; i < lyrics.size(); ++i)
    {
        auto l = lyrics[i].trim();
        if (l.isEmpty() || l == "-" || l == "+" || l == "~" || l == juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa2")))
            continue;
        if (l.startsWithChar ('[')) return {};                       // explicit phonemes: TTS cannot read them
        l = l.removeCharacters ("-");
        if (text.isNotEmpty() && wordStarts[i]) text << ' ';
        text << l;
    }
    return text.trim();
}

} // namespace vf::voicebox
