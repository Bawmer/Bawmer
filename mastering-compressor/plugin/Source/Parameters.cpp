#include "Parameters.h"

namespace lumen
{
namespace
{
using APF = juce::AudioParameterFloat;
using APB = juce::AudioParameterBool;
using APC = juce::AudioParameterChoice;

juce::NormalisableRange<float> logRange (float lo, float hi, float centre)
{
    juce::NormalisableRange<float> r (lo, hi);
    r.setSkewForCentre (centre);
    return r;
}

juce::String fmtDb (float v, int) { return juce::String (v, std::abs (v) < 10.0f ? 2 : 1) + " dB"; }
juce::String fmtMs (float v, int)
{
    if (v >= 1000.0f) return juce::String (v / 1000.0f, 2) + " s";
    if (v < 1.0f)     return juce::String (v, 2) + " ms";
    return juce::String (v, v < 100.0f ? 1 : 0) + " ms";
}
juce::String fmtRatio (float v, int) { return juce::String (v, v < 10.0f ? 2 : 1) + ":1"; }
juce::String fmtPct (float v, int) { return juce::String (juce::roundToInt (v)) + " %"; }
juce::String fmtHz (float v, int)
{
    return v >= 1000.0f ? juce::String (v / 1000.0f, 1) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
}

std::unique_ptr<APF> floatParam (const char* id, const char* name, juce::NormalisableRange<float> range, float def,
                                 juce::String (*fmt) (float, int), const char* label)
{
    return std::make_unique<APF> (juce::ParameterID { id, 1 }, name, range, def,
                                  juce::AudioParameterFloatAttributes().withLabel (label).withStringFromValueFunction (fmt));
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<APC> (juce::ParameterID { ids::mode, 1 }, "Mode",
                                       juce::StringArray { "Ultra Transparent", "Digital", "Analog", "Hybrid" }, 0));

    layout.add (floatParam (ids::threshold, "Threshold", { -60.0f, 0.0f, 0.01f }, -12.0f, fmtDb, "dB"));
    layout.add (floatParam (ids::ratio, "Ratio", logRange (1.0f, 20.0f, 3.0f), 2.0f, fmtRatio, ":1"));
    layout.add (floatParam (ids::attack, "Attack", logRange (0.01f, 250.0f, 10.0f), 10.0f, fmtMs, "ms"));
    layout.add (floatParam (ids::release, "Release", logRange (10.0f, 5000.0f, 300.0f), 200.0f, fmtMs, "ms"));
    layout.add (floatParam (ids::knee, "Knee", { 0.0f, 24.0f, 0.01f }, 6.0f, fmtDb, "dB"));
    layout.add (floatParam (ids::makeup, "Makeup", { -24.0f, 24.0f, 0.01f }, 0.0f, fmtDb, "dB"));
    layout.add (floatParam (ids::mix, "Mix", { 0.0f, 100.0f, 0.1f }, 100.0f, fmtPct, "%"));
    layout.add (floatParam (ids::link, "Stereo Link", { 0.0f, 100.0f, 0.1f }, 100.0f, fmtPct, "%"));

    layout.add (std::make_unique<APB> (juce::ParameterID { ids::scHpfOn, 1 }, "SC HPF On", true));
    layout.add (floatParam (ids::scHpf, "SC HPF", logRange (20.0f, 500.0f, 100.0f), 60.0f, fmtHz, "Hz"));
    layout.add (std::make_unique<APB> (juce::ParameterID { ids::scLpfOn, 1 }, "SC LPF On", false));
    layout.add (floatParam (ids::scLpf, "SC LPF", logRange (1000.0f, 20000.0f, 5000.0f), 16000.0f, fmtHz, "Hz"));

    layout.add (std::make_unique<APC> (juce::ParameterID { ids::releaseMode, 1 }, "Release Mode",
                                       juce::StringArray { "Manual", "Auto Smooth", "Auto Program", "Auto Fast" }, 0));
    layout.add (floatParam (ids::hold, "Hold", { 0.0f, 50.0f, 0.01f }, 0.0f, fmtMs, "ms"));
    layout.add (std::make_unique<APB> (juce::ParameterID { ids::autoGain, 1 }, "Auto Gain", false));
    layout.add (floatParam (ids::lookahead, "Lookahead", { 0.0f, 20.0f, 0.01f }, 0.0f, fmtMs, "ms"));
    layout.add (std::make_unique<APC> (juce::ParameterID { ids::lookaheadMode, 1 }, "Lookahead Mode",
                                       juce::StringArray { "Exact (no overshoot)", "Classic (early attack)" }, 0));
    layout.add (std::make_unique<APC> (juce::ParameterID { ids::detection, 1 }, "Detection",
                                       juce::StringArray { "Peak", "RMS", "Hybrid", "True Peak" }, 1));
    layout.add (std::make_unique<APB> (juce::ParameterID { ids::extSc, 1 }, "External Sidechain", false));
    layout.add (std::make_unique<APC> (juce::ParameterID { ids::channelMode, 1 }, "Channel Mode",
                                       juce::StringArray { "Stereo", "Dual Mono", "Mid/Side" }, 0));
    layout.add (std::make_unique<APC> (juce::ParameterID { ids::topology, 1 }, "Topology",
                                       juce::StringArray { "Feed-Forward", "Feedback" }, 0));
    layout.add (std::make_unique<APC> (juce::ParameterID { ids::analogModel, 1 }, "Analog Model",
                                       juce::StringArray { "VCA", "Opto", "Vari-Mu", "FET" }, 0));
    layout.add (floatParam (ids::blend, "Hybrid Blend", { 0.0f, 100.0f, 0.1f }, 50.0f, fmtPct, "%"));

    layout.add (floatParam (ids::drive, "Drive", { 0.0f, 24.0f, 0.01f }, 6.0f, fmtDb, "dB"));
    layout.add (floatParam (ids::character, "Character", { 0.0f, 100.0f, 0.1f }, 35.0f, fmtPct, "%"));
    layout.add (floatParam (ids::colorMix, "Colour Mix", { 0.0f, 100.0f, 0.1f }, 100.0f, fmtPct, "%"));
    layout.add (std::make_unique<APB> (juce::ParameterID { ids::transformer, 1 }, "Transformer", true));
    layout.add (std::make_unique<APB> (juce::ParameterID { ids::amplifier, 1 }, "Amplifier", true));
    layout.add (std::make_unique<APC> (juce::ParameterID { ids::oversampling, 1 }, "Oversampling",
                                       juce::StringArray { "Off", "2x", "4x", "8x", "16x" }, 2));
    layout.add (std::make_unique<APC> (juce::ParameterID { ids::osQuality, 1 }, "Oversampling Filter",
                                       juce::StringArray { "Linear Phase HQ", "Low Latency" }, 0));

    layout.add (std::make_unique<APB> (juce::ParameterID { ids::bypass, 1 }, "Bypass", false));
    layout.add (std::make_unique<APB> (juce::ParameterID { ids::matchBypass, 1 }, "Loudness-Matched Bypass", true));
    return layout;
}

ParameterReader::ParameterReader (juce::AudioProcessorValueTreeState& s)
{
    mode = get (s, ids::mode);
    threshold = get (s, ids::threshold);
    ratio = get (s, ids::ratio);
    attack = get (s, ids::attack);
    release = get (s, ids::release);
    knee = get (s, ids::knee);
    makeup = get (s, ids::makeup);
    mix = get (s, ids::mix);
    link = get (s, ids::link);
    scHpfOn = get (s, ids::scHpfOn);
    scHpf = get (s, ids::scHpf);
    scLpfOn = get (s, ids::scLpfOn);
    scLpf = get (s, ids::scLpf);
    releaseMode = get (s, ids::releaseMode);
    hold = get (s, ids::hold);
    autoGain = get (s, ids::autoGain);
    lookahead = get (s, ids::lookahead);
    lookaheadMode = get (s, ids::lookaheadMode);
    detection = get (s, ids::detection);
    extSc = get (s, ids::extSc);
    channelMode = get (s, ids::channelMode);
    topology = get (s, ids::topology);
    analogModel = get (s, ids::analogModel);
    blend = get (s, ids::blend);
    drive = get (s, ids::drive);
    character = get (s, ids::character);
    colorMix = get (s, ids::colorMix);
    transformer = get (s, ids::transformer);
    amplifier = get (s, ids::amplifier);
    oversampling = get (s, ids::oversampling);
    osQuality = get (s, ids::osQuality);
    bypass = get (s, ids::bypass);
    matchBypass = get (s, ids::matchBypass);
}

std::atomic<float>* ParameterReader::get (juce::AudioProcessorValueTreeState& s, const char* id)
{
    auto* p = s.getRawParameterValue (id);
    jassert (p != nullptr);
    return p;
}

CompressorParams ParameterReader::read() const noexcept
{
    auto f = [] (std::atomic<float>* a) { return static_cast<double> (a->load (std::memory_order_relaxed)); };
    auto b = [] (std::atomic<float>* a) { return a->load (std::memory_order_relaxed) >= 0.5f; };
    auto i = [] (std::atomic<float>* a) { return juce::roundToInt (a->load (std::memory_order_relaxed)); };

    CompressorParams p;
    p.mode = static_cast<EngineMode> (juce::jlimit (0, 3, i (mode)));
    p.thresholdDb = f (threshold);
    p.ratio = f (ratio);
    p.attackMs = f (attack);
    p.releaseMs = f (release);
    p.kneeDb = f (knee);
    p.makeupDb = f (makeup);
    p.mix = f (mix) * 0.01;
    p.stereoLink = f (link) * 0.01;
    p.scHpfOn = b (scHpfOn);
    p.scHpfHz = f (scHpf);
    p.scLpfOn = b (scLpfOn);
    p.scLpfHz = f (scLpf);
    {
        const int rm = juce::jlimit (0, 3, i (releaseMode));
        p.autoRelease = rm > 0;
        p.autoReleaseShape = static_cast<AutoReleaseShape> (juce::jmax (0, rm - 1));
    }
    p.holdMs = f (hold);
    p.autoGain = b (autoGain);
    p.lookaheadMs = f (lookahead);
    p.lookaheadMode = static_cast<LookaheadMode> (juce::jlimit (0, 1, i (lookaheadMode)));
    p.detection = static_cast<DetectionMode> (juce::jlimit (0, 3, i (detection)));
    p.externalSidechain = b (extSc);
    p.channelMode = static_cast<ChannelMode> (juce::jlimit (0, 2, i (channelMode)));
    p.topology = static_cast<Topology> (juce::jlimit (0, 1, i (topology)));
    p.analogModel = static_cast<AnalogModel> (juce::jlimit (0, 3, i (analogModel)));
    p.hybridBlend = f (blend) * 0.01;
    p.driveDb = f (drive);
    p.character = f (character) * 0.01;
    p.colorMix = f (colorMix) * 0.01;
    p.transformer = b (transformer);
    p.amplifier = b (amplifier);
    p.oversamplingLog2 = juce::jlimit (0, 4, i (oversampling));
    p.oversamplingQuality = static_cast<OversamplingQuality> (juce::jlimit (0, 1, i (osQuality)));
    p.bypass = b (bypass);
    p.loudnessMatchedBypass = b (matchBypass);
    return p;
}
} // namespace lumen
