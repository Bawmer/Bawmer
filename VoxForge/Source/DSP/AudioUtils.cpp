#include "AudioUtils.h"

namespace vf
{

juce::AudioFormatManager& formatManager()
{
    static juce::AudioFormatManager fm;
    static bool init = false;
    static juce::CriticalSection lock;
    const juce::ScopedLock sl (lock);
    if (! init) { fm.registerBasicFormats(); init = true; }
    return fm;
}

bool loadAudioMono (const juce::File& file, juce::AudioBuffer<float>& out, double& sampleRate, double maxSeconds)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager().createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0) return false;

    sampleRate = reader->sampleRate;
    const auto len = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (maxSeconds * sampleRate));
    const int ch = (int) juce::jmax (1u, reader->numChannels);
    juce::AudioBuffer<float> tmp (ch, len);
    reader->read (&tmp, 0, len, 0, true, true);

    out.setSize (1, len);
    out.clear();
    for (int c = 0; c < ch; ++c)
        out.addFrom (0, 0, tmp, c, 0, len, 1.0f / (float) ch);
    return true;
}

juce::AudioBuffer<float> resampleMono (const juce::AudioBuffer<float>& in, double inRate, double outRate)
{
    if (std::abs (inRate - outRate) < 1.0e-6 || in.getNumSamples() == 0)
    {
        juce::AudioBuffer<float> copy (1, in.getNumSamples());
        if (in.getNumSamples() > 0) copy.copyFrom (0, 0, in, 0, 0, in.getNumSamples());
        return copy;
    }

    juce::AudioBuffer<float> src (1, in.getNumSamples());
    src.copyFrom (0, 0, in, 0, 0, in.getNumSamples());

    if (outRate < inRate)
    {
        // 8th-order low-pass at 45 % of the new rate
        auto coeffs = juce::IIRCoefficients::makeLowPass (inRate, outRate * 0.45, 0.54);
        auto coeffs2 = juce::IIRCoefficients::makeLowPass (inRate, outRate * 0.45, 1.31);
        for (int pass = 0; pass < 2; ++pass)
        {
            juce::IIRFilter f1, f2;
            f1.setCoefficients (coeffs);
            f2.setCoefficients (coeffs2);
            f1.processSamples (src.getWritePointer (0), src.getNumSamples());
            f2.processSamples (src.getWritePointer (0), src.getNumSamples());
        }
    }

    const double ratio = inRate / outRate;
    const int outLen = (int) std::floor (in.getNumSamples() / ratio);
    juce::AudioBuffer<float> out (1, juce::jmax (1, outLen));
    out.clear();

    // pad the input so the interpolator can read past the end
    juce::AudioBuffer<float> padded (1, src.getNumSamples() + 256);
    padded.clear();
    padded.copyFrom (0, 0, src, 0, 0, src.getNumSamples());

    juce::WindowedSincInterpolator interp;
    interp.process (ratio, padded.getReadPointer (0), out.getWritePointer (0), outLen);
    return out;
}

bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate, int bits)
{
    file.deleteFile();
    file.getParentDirectory().createDirectory();
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    if (stream == nullptr) return false;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sampleRate,
                                                                          (unsigned int) buffer.getNumChannels(),
                                                                          bits, {}, 0));
    if (writer == nullptr) return false;
    stream.release();   // writer owns it now
    return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

} // namespace vf
