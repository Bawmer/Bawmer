#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <lumen/CompressorEngine.h>

namespace lumen
{
/** Peak meter with RMS-style ballistics and peak hold, dB scale from -60 to +6 dBFS. */
class LevelMeter final : public juce::Component
{
public:
    explicit LevelMeter (juce::String title);
    void setLabels (const juce::String& a, const juce::String& b);
    void push (float peakA, float peakB); // linear
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override { holdA = holdB = -100.0f; clipA = clipB = false; repaint(); }

private:
    static float dbToY (float db, juce::Rectangle<float> r);
    juce::String title, labelA { "L" }, labelB { "R" };
    float levelA = -100.0f, levelB = -100.0f, holdA = -100.0f, holdB = -100.0f;
    int holdTimeA = 0, holdTimeB = 0;
    bool clipA = false, clipB = false;
};

/** Gain-reduction meter: bars grow downwards from 0 dB, 0..24 dB scale, high resolution readout. */
class GainReductionMeter final : public juce::Component
{
public:
    void push (float grA, float grB);
    void paint (juce::Graphics&) override;
    void setLabels (const juce::String& first, const juce::String& second) { labelA = first; labelB = second; }

private:
    float a = 0.0f, b = 0.0f, holdMax = 0.0f;
    int holdTime = 0;
    juce::String labelA { "L" }, labelB { "R" };
};

/** Static transfer curve + live operating point. */
class TransferCurveView final : public juce::Component
{
public:
    void setCurve (double thresholdDb, double ratio, double kneeDb, double makeupDb);
    void setOperatingPoint (float inputDb, float grDb);
    void paint (juce::Graphics&) override;

private:
    double threshold = -12.0, ratio = 2.0, knee = 6.0, makeup = 0.0;
    float pointIn = -120.0f, pointGr = 0.0f;
};

/** Scrolling gain-reduction history (about 8 s) with the input level drawn behind it. */
class GainHistoryView final : public juce::Component
{
public:
    static constexpr int kPoints = 800;
    void push (float grDb, float levelDb);
    void paint (juce::Graphics&) override;

private:
    float gr[kPoints] {};
    float level[kPoints] {};
    int head = 0;
};
} // namespace lumen
