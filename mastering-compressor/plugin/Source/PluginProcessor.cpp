#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace lumen
{
LumenAudioProcessor::LumenAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, &undoManager, "LumenMC64", createParameterLayout()),
      presets (apvts),
      reader (apvts)
{
    abSlots[0] = apvts.copyState();
    abSlots[1] = apvts.copyState();
    startTimerHz (10);
}

LumenAudioProcessor::~LumenAudioProcessor() { stopTimer(); }

bool LumenAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in != out)
        return false;
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void LumenAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    const int block = juce::jmax (1, samplesPerBlock);
    work.setSize (2, block, false, true, false);
    scWork.setSize (2, block, false, true, false);
    engine.setParams (reader.read());
    engine.prepare (sampleRate, block);
    historyInterval = juce::jmax (1, juce::roundToInt (sampleRate * 0.01));
    historySamples = 0;

    const int lat = engine.getLatencySamples();
    desiredLatency = lat;
    reportedLatency = lat;
    setLatencySamples (lat);
}

void LumenAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    processInternal (buffer);
}

void LumenAudioProcessor::processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer&)
{
    processInternal (buffer);
}

template <typename SampleType>
void LumenAudioProcessor::processInternal (juce::AudioBuffer<SampleType>& buffer)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    if (numSamples == 0)
        return;

    auto mainIn = getBusBuffer (buffer, true, 0);
    const int numCh = juce::jmin (2, mainIn.getNumChannels());
    for (int ch = numCh; ch < buffer.getNumChannels(); ++ch)
        if (ch >= getTotalNumInputChannels())
            buffer.clear (ch, 0, numSamples);
    if (numCh == 0)
        return;

    engine.setParams (reader.read());

    // sidechain (optional second input bus)
    int numSc = 0;
    if (getBusCount (true) > 1 && getBus (true, 1)->isEnabled())
    {
        auto sc = getBusBuffer (buffer, true, 1);
        numSc = juce::jmin (2, sc.getNumChannels());
    }

    // Host buffers can exceed the size given to prepareToPlay; process in chunks.
    const int maxChunk = work.getNumSamples();
    for (int offset = 0; offset < numSamples; offset += maxChunk)
    {
        const int n = juce::jmin (maxChunk, numSamples - offset);
        double* io[2] {};
        const double* scPtr[2] {};

        if constexpr (std::is_same_v<SampleType, double>)
        {
            for (int ch = 0; ch < numCh; ++ch)
                io[ch] = mainIn.getWritePointer (ch, offset);
        }
        else
        {
            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* src = mainIn.getReadPointer (ch, offset);
                auto* dst = work.getWritePointer (ch);
                for (int i = 0; i < n; ++i)
                    dst[i] = static_cast<double> (src[i]);
                io[ch] = dst;
            }
        }

        if (numSc > 0)
        {
            auto sc = getBusBuffer (buffer, true, 1);
            for (int ch = 0; ch < numSc; ++ch)
            {
                if constexpr (std::is_same_v<SampleType, double>)
                {
                    scPtr[ch] = sc.getReadPointer (ch, offset);
                }
                else
                {
                    auto* src = sc.getReadPointer (ch, offset);
                    auto* dst = scWork.getWritePointer (ch);
                    for (int i = 0; i < n; ++i)
                        dst[i] = static_cast<double> (src[i]);
                    scPtr[ch] = dst;
                }
            }
        }

        engine.process (io, numCh, numSc > 0 ? scPtr : nullptr, numSc, n);

        if constexpr (! std::is_same_v<SampleType, double>)
        {
            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* dst = mainIn.getWritePointer (ch, offset);
                const double* src = io[ch];
                for (int i = 0; i < n; ++i)
                    dst[i] = static_cast<float> (src[i]);
            }
        }
        publishMeters (n);
    }

    desiredLatency.store (engine.getLatencySamples(), std::memory_order_relaxed);
}

void LumenAudioProcessor::publishMeters (int numSamples) noexcept
{
    const auto& m = engine.getMeters();
    for (int ch = 0; ch < 2; ++ch)
    {
        MeterBus::accumulateMax (meters.inPeak[ch], static_cast<float> (m.inputPeak[ch]));
        MeterBus::accumulateMax (meters.outPeak[ch], static_cast<float> (m.outputPeak[ch]));
        MeterBus::accumulateMax (meters.gr[ch], static_cast<float> (m.gainReductionDb[ch]));
    }
    MeterBus::accumulateMax (meters.inMid, static_cast<float> (m.inMidPeak));
    MeterBus::accumulateMax (meters.inSide, static_cast<float> (m.inSidePeak));
    MeterBus::accumulateMax (meters.outMid, static_cast<float> (m.midPeak));
    MeterBus::accumulateMax (meters.outSide, static_cast<float> (m.sidePeak));
    MeterBus::accumulateMax (meters.detectorDb, static_cast<float> (juce::jmax (-120.0, m.detectorLevelDb)));
    meters.autoGainDb.store (static_cast<float> (m.autoGainDb), std::memory_order_relaxed);
    meters.matchDb.store (static_cast<float> (m.loudnessMatchDb), std::memory_order_relaxed);

    historyMaxGr = juce::jmax (historyMaxGr, static_cast<float> (juce::jmax (m.gainReductionDb[0], m.gainReductionDb[1])));
    historyMaxLevel = juce::jmax (historyMaxLevel, static_cast<float> (juce::Decibels::gainToDecibels (
                                                       juce::jmax (m.inputPeak[0], m.inputPeak[1]), -120.0)));
    historySamples += numSamples;
    if (historySamples >= historyInterval)
    {
        history.push (historyMaxGr, historyMaxLevel);
        historySamples = 0;
        historyMaxGr = 0.0f;
        historyMaxLevel = -120.0f;
    }
}

void LumenAudioProcessor::timerCallback()
{
    // Latency changes (lookahead / oversampling / colour stage) are reported from the message
    // thread, never from the audio callback.
    const int want = desiredLatency.load();
    if (want != reportedLatency.load())
    {
        reportedLatency = want;
        setLatencySamples (want);
    }
}

juce::AudioProcessorParameter* LumenAudioProcessor::getBypassParameter() const
{
    return apvts.getParameter (ids::bypass);
}

void LumenAudioProcessor::switchToSlot (int slot)
{
    slot = juce::jlimit (0, 1, slot);
    if (slot == activeSlot)
        return;
    abSlots[activeSlot] = apvts.copyState();
    activeSlot = slot;
    // keep the comparison switches as they are; load everything else
    auto target = abSlots[activeSlot].createCopy();
    for (auto child : target)
    {
        const auto id = child["id"].toString();
        if (id.isEmpty() || id == ids::bypass || id == ids::matchBypass)
            continue;
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (child["value"])));
    }
}

void LumenAudioProcessor::copyActiveToOther()
{
    abSlots[activeSlot] = apvts.copyState();
    abSlots[1 - activeSlot] = abSlots[activeSlot].createCopy();
}

void LumenAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    abSlots[activeSlot] = state.createCopy();
    juce::ValueTree ab ("AB");
    ab.setProperty ("active", activeSlot, nullptr);
    ab.setProperty ("preset", presets.getCurrentIndex(), nullptr);
    for (int i = 0; i < 2; ++i)
    {
        juce::ValueTree slot ("Slot");
        slot.appendChild (abSlots[i].createCopy(), nullptr);
        ab.appendChild (slot, nullptr);
    }
    juce::ValueTree root ("LumenMC64State");
    root.setProperty ("version", 1, nullptr);
    root.appendChild (state, nullptr);
    root.appendChild (ab, nullptr);
    if (auto xml = root.createXml())
        copyXmlToBinary (*xml, destData);
}

void LumenAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return;
    auto root = juce::ValueTree::fromXml (*xml);
    auto params = root.getChildWithName (apvts.state.getType());
    if (! params.isValid() && root.hasType (apvts.state.getType()))
        params = root; // plain APVTS state
    if (params.isValid())
        apvts.replaceState (params);

    auto ab = root.getChildWithName ("AB");
    if (ab.isValid() && ab.getNumChildren() == 2)
    {
        activeSlot = juce::jlimit (0, 1, static_cast<int> (ab["active"]));
        for (int i = 0; i < 2; ++i)
            if (ab.getChild (i).getNumChildren() > 0)
                abSlots[i] = ab.getChild (i).getChild (0).createCopy();
    }
    else
    {
        abSlots[0] = apvts.copyState();
        abSlots[1] = apvts.copyState();
        activeSlot = 0;
    }
    undoManager.clearUndoHistory();
}

juce::AudioProcessorEditor* LumenAudioProcessor::createEditor() { return new LumenAudioProcessorEditor (*this); }
} // namespace lumen

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new lumen::LumenAudioProcessor(); }
