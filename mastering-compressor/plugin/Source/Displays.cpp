#include "Displays.h"
#include "LookAndFeel.h"

namespace lumen
{
namespace
{
void drawScreen (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (juce::Colour (0xff0e1012));
    g.fillRoundedRectangle (r, 5.0f);
    juce::ColourGradient glass (juce::Colours::white.withAlpha (0.035f), r.getX(), r.getY(),
                                juce::Colours::transparentWhite, r.getX(), r.getY() + r.getHeight() * 0.4f, false);
    g.setGradientFill (glass);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (juce::Colours::black);
    g.drawRoundedRectangle (r, 5.0f, 1.5f);
    g.setColour (colours::panelEdge.withAlpha (0.6f));
    g.drawRoundedRectangle (r.expanded (1.0f), 6.0f, 0.8f);
}

float toDb (float lin) { return lin > 1.0e-6f ? 20.0f * std::log10 (lin) : -120.0f; }
} // namespace

// ---------------------------------------------------------------------------------------------
LevelMeter::LevelMeter (juce::String t) : title (std::move (t)) {}

void LevelMeter::setLabels (const juce::String& a, const juce::String& b)
{
    labelA = a;
    labelB = b;
}

void LevelMeter::push (float peakA, float peakB)
{
    auto ballistic = [] (float& level, float& hold, int& holdTime, bool& clip, float peakLin) {
        const float db = toDb (peakLin);
        level = db > level ? db : juce::jmax (db, level - 1.2f); // ~36 dB/s fall at 30 fps
        if (db >= hold) { hold = db; holdTime = 45; }
        else if (--holdTime <= 0) hold = juce::jmax (level, hold - 0.6f);
        if (db > 0.0f) clip = true;
    };
    ballistic (levelA, holdA, holdTimeA, clipA, peakA);
    ballistic (levelB, holdB, holdTimeB, clipB, peakB);
    repaint();
}

float LevelMeter::dbToY (float db, juce::Rectangle<float> r)
{
    const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
    return r.getBottom() - norm * r.getHeight();
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (colours::textDim);
    g.setFont (uiFont (11.0f, true));
    g.drawText (title, r.removeFromTop (16.0f), juce::Justification::centred);
    auto readout = r.removeFromBottom (18.0f);
    auto labels = r.removeFromBottom (14.0f);
    drawScreen (g, r);
    auto inner = r.reduced (6.0f, 8.0f);
    auto scale = inner.removeFromRight (20.0f);
    const float barW = (inner.getWidth() - 4.0f) * 0.5f;
    auto barA = inner.removeFromLeft (barW);
    inner.removeFromLeft (4.0f);
    auto barB = inner.removeFromLeft (barW);

    auto drawBar = [&] (juce::Rectangle<float> bar, float level, float hold, bool clip) {
        g.setColour (juce::Colour (0xff1a1d20));
        g.fillRect (bar);
        const float y = dbToY (level, bar);
        juce::ColourGradient grad (colours::meterGreen, 0.0f, bar.getBottom(), colours::meterRed, 0.0f, bar.getY(), false);
        grad.addColour ((60.0f - 12.0f) / 66.0f, colours::meterGreen);
        grad.addColour ((60.0f - 3.0f) / 66.0f, colours::meterYellow);
        g.setGradientFill (grad);
        g.fillRect (bar.withTop (y));
        g.setColour (colours::text);
        g.fillRect (bar.getX(), dbToY (hold, bar) - 1.0f, bar.getWidth(), 1.5f);
        if (clip)
        {
            g.setColour (colours::meterRed);
            g.fillRect (bar.withHeight (3.0f).translated (0.0f, -6.0f));
        }
    };
    drawBar (barA, levelA, holdA, clipA);
    drawBar (barB, levelB, holdB, clipB);

    g.setFont (uiFont (9.5f));
    for (float db : { 6.0f, 0.0f, -6.0f, -12.0f, -18.0f, -24.0f, -36.0f, -48.0f, -60.0f })
    {
        const float y = dbToY (db, scale);
        g.setColour (colours::textDim.withAlpha (0.6f));
        g.fillRect (scale.getX(), y, 3.0f, 1.0f);
        g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (scale.getX() + 3.0f, y - 6.0f, 18.0f, 12.0f),
                    juce::Justification::centredRight);
    }

    g.setFont (uiFont (10.0f, true));
    g.setColour (colours::textDim);
    g.drawText (labelA, labels.withWidth (barW).translated (6.0f, 0.0f), juce::Justification::centred);
    g.drawText (labelB, labels.withWidth (barW).translated (10.0f + barW, 0.0f), juce::Justification::centred);

    const float hold = juce::jmax (holdA, holdB);
    g.setColour (hold > 0.0f ? colours::meterRed : colours::text);
    g.setFont (uiFont (12.0f));
    g.drawText (hold <= -99.0f ? juce::String ("-inf") : juce::String (hold, 1), readout, juce::Justification::centred);
}

// ---------------------------------------------------------------------------------------------
void GainReductionMeter::push (float grA, float grB)
{
    auto fall = [] (float& v, float target) { v = target > v ? target : juce::jmax (target, v - 0.5f); };
    fall (a, grA);
    fall (b, grB);
    const float m = juce::jmax (grA, grB);
    if (m >= holdMax) { holdMax = m; holdTime = 60; }
    else if (--holdTime <= 0) holdMax = juce::jmax (m, holdMax - 0.25f);
    repaint();
}

void GainReductionMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (colours::textDim);
    g.setFont (uiFont (11.0f, true));
    g.drawText ("GR", r.removeFromTop (16.0f), juce::Justification::centred);
    auto readout = r.removeFromBottom (18.0f);
    auto labels = r.removeFromBottom (14.0f);
    drawScreen (g, r);
    auto inner = r.reduced (6.0f, 8.0f);
    auto scale = inner.removeFromRight (18.0f);
    const float barW = (inner.getWidth() - 4.0f) * 0.5f;
    auto barA = inner.removeFromLeft (barW);
    inner.removeFromLeft (4.0f);
    auto barB = inner.removeFromLeft (barW);

    // non-linear scale: finer at low GR (where mastering lives)
    auto grToY = [] (float gr, juce::Rectangle<float> bar) {
        const float norm = std::sqrt (juce::jlimit (0.0f, 24.0f, gr) / 24.0f);
        return bar.getY() + norm * bar.getHeight();
    };
    for (auto [bar, v] : { std::pair { barA, a }, std::pair { barB, b } })
    {
        g.setColour (juce::Colour (0xff1a1d20));
        g.fillRect (bar);
        g.setColour (colours::gr);
        g.fillRect (bar.withBottom (grToY (v, bar)));
    }
    g.setFont (uiFont (9.5f));
    for (float db : { 0.0f, 0.5f, 1.0f, 2.0f, 3.0f, 6.0f, 10.0f, 16.0f, 24.0f })
    {
        const float y = grToY (db, scale);
        g.setColour (colours::textDim.withAlpha (0.6f));
        g.fillRect (scale.getX(), y, 3.0f, 1.0f);
        g.drawText (juce::exactlyEqual (db, 0.5f) ? juce::String (".5") : juce::String (juce::roundToInt (db)),
                    juce::Rectangle<float> (scale.getX() + 2.0f, y - 6.0f, 16.0f, 12.0f), juce::Justification::centredRight);
    }
    g.setFont (uiFont (10.0f, true));
    g.setColour (colours::textDim);
    g.drawText (labelA, labels.withWidth (barW).translated (6.0f, 0.0f), juce::Justification::centred);
    g.drawText (labelB, labels.withWidth (barW).translated (10.0f + barW, 0.0f), juce::Justification::centred);
    g.setColour (colours::gr);
    g.setFont (uiFont (12.0f));
    g.drawText (holdMax < 0.005f ? juce::String ("0.00") : "-" + juce::String (holdMax, 2), readout,
                juce::Justification::centred);
}

// ---------------------------------------------------------------------------------------------
void TransferCurveView::setCurve (double t, double r, double k, double m)
{
    if (juce::exactlyEqual (t, threshold) && juce::exactlyEqual (r, ratio) && juce::exactlyEqual (k, knee)
        && juce::exactlyEqual (m, makeup))
        return;
    threshold = t; ratio = r; knee = k; makeup = m;
    repaint();
}

void TransferCurveView::setOperatingPoint (float inputDb, float grDb)
{
    pointIn = inputDb;
    pointGr = grDb;
    repaint();
}

void TransferCurveView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    drawScreen (g, r);
    auto plot = r.reduced (26.0f, 14.0f).withTrimmedBottom (10.0f);
    plot = plot.withSizeKeepingCentre (juce::jmin (plot.getWidth(), plot.getHeight()), juce::jmin (plot.getWidth(), plot.getHeight()));
    constexpr float lo = -60.0f, hi = 6.0f;
    auto mapX = [&] (double db) { return plot.getX() + (float) ((db - lo) / (hi - lo)) * plot.getWidth(); };
    auto mapY = [&] (double db) { return plot.getBottom() - (float) ((juce::jlimit ((double) lo, (double) hi, db) - lo) / (hi - lo)) * plot.getHeight(); };

    g.setFont (uiFont (9.5f));
    for (float db = -60.0f; db <= 0.0f; db += 12.0f)
    {
        g.setColour (juce::Colours::white.withAlpha (juce::exactlyEqual (db, 0.0f) ? 0.12f : 0.05f));
        g.drawVerticalLine (juce::roundToInt (mapX (db)), plot.getY(), plot.getBottom());
        g.drawHorizontalLine (juce::roundToInt (mapY (db)), plot.getX(), plot.getRight());
        g.setColour (colours::textDim.withAlpha (0.7f));
        g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (plot.getX() - 26.0f, mapY (db) - 6.0f, 22.0f, 12.0f),
                    juce::Justification::centredRight);
        g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (mapX (db) - 12.0f, plot.getBottom() + 2.0f, 24.0f, 12.0f),
                    juce::Justification::centred);
    }
    // unity line
    g.setColour (juce::Colours::white.withAlpha (0.12f));
    g.drawLine (mapX (lo), mapY (lo), mapX (hi), mapY (hi), 1.0f);

    // knee region
    if (knee > 0.0)
    {
        g.setColour (colours::accent.withAlpha (0.05f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (mapX (threshold - knee * 0.5), plot.getY(),
                                                                 mapX (threshold + knee * 0.5), plot.getBottom()));
    }

    juce::Path curve;
    for (int i = 0; i <= 300; ++i)
    {
        const double in = lo + (hi - lo) * i / 300.0;
        const double out = GainComputer::staticOutput (in, threshold, ratio, knee) + makeup;
        if (i == 0) curve.startNewSubPath (mapX (in), mapY (out));
        else curve.lineTo (mapX (in), mapY (out));
    }
    g.saveState();
    g.reduceClipRegion (plot.toNearestInt());
    g.setColour (colours::accent.withAlpha (0.25f));
    g.strokePath (curve, juce::PathStrokeType (5.0f));
    g.setColour (colours::accent);
    g.strokePath (curve, juce::PathStrokeType (1.8f));
    g.restoreState();

    // threshold marker
    g.setColour (colours::accentCool.withAlpha (0.5f));
    const float tx = mapX (threshold);
    for (float y = plot.getY(); y < plot.getBottom(); y += 6.0f)
        g.drawVerticalLine (juce::roundToInt (tx), y, y + 3.0f);

    // operating point
    if (pointIn > lo)
    {
        const float px = mapX (pointIn), py = mapY (pointIn - pointGr + makeup);
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.fillEllipse (px - 7.0f, py - 7.0f, 14.0f, 14.0f);
        g.setColour (juce::Colours::white);
        g.fillEllipse (px - 3.5f, py - 3.5f, 7.0f, 7.0f);
    }
    g.setColour (colours::textDim);
    g.setFont (uiFont (10.0f, true));
    g.drawText ("TRANSFER", r.reduced (8.0f, 4.0f), juce::Justification::topLeft);
}

// ---------------------------------------------------------------------------------------------
void GainHistoryView::push (float grDb, float levelDb)
{
    gr[head] = grDb;
    level[head] = levelDb;
    head = (head + 1) % kPoints;
}

void GainHistoryView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    drawScreen (g, r);
    auto plot = r.reduced (10.0f, 18.0f).withTrimmedRight (22.0f);

    // input level (0 dB at top, -60 at bottom), as a faint area
    juce::Path lvl;
    lvl.startNewSubPath (plot.getX(), plot.getBottom());
    for (int i = 0; i < kPoints; ++i)
    {
        const int idx = (head + i) % kPoints;
        const float x = plot.getX() + plot.getWidth() * i / (float) (kPoints - 1);
        const float norm = juce::jlimit (0.0f, 1.0f, (level[idx] + 60.0f) / 60.0f);
        lvl.lineTo (x, plot.getBottom() - norm * plot.getHeight());
    }
    lvl.lineTo (plot.getRight(), plot.getBottom());
    lvl.closeSubPath();
    g.setColour (colours::accentCool.withAlpha (0.12f));
    g.fillPath (lvl);

    // GR grows down from the top. Auto-scaled range: 6, 12 or 24 dB.
    float maxGr = 0.0f;
    for (float v : gr) maxGr = juce::jmax (maxGr, v);
    const float range = maxGr <= 5.5f ? 6.0f : (maxGr <= 11.0f ? 12.0f : 24.0f);
    g.setFont (uiFont (9.5f));
    for (int i = 0; i <= 3; ++i)
    {
        const float db = range * i / 3.0f;
        const float y = plot.getY() + plot.getHeight() * i / 3.0f;
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
        g.setColour (colours::textDim.withAlpha (0.7f));
        g.drawText ("-" + juce::String (db, 0), juce::Rectangle<float> (plot.getRight() + 2.0f, y - 6.0f, 22.0f, 12.0f),
                    juce::Justification::centredLeft);
    }

    juce::Path grPath, grFill;
    grFill.startNewSubPath (plot.getX(), plot.getY());
    for (int i = 0; i < kPoints; ++i)
    {
        const int idx = (head + i) % kPoints;
        const float x = plot.getX() + plot.getWidth() * i / (float) (kPoints - 1);
        const float y = plot.getY() + juce::jlimit (0.0f, 1.0f, gr[idx] / range) * plot.getHeight();
        if (i == 0) grPath.startNewSubPath (x, y);
        else grPath.lineTo (x, y);
        grFill.lineTo (x, y);
    }
    grFill.lineTo (plot.getRight(), plot.getY());
    grFill.closeSubPath();
    g.setColour (colours::gr.withAlpha (0.18f));
    g.fillPath (grFill);
    g.setColour (colours::gr);
    g.strokePath (grPath, juce::PathStrokeType (1.6f));

    g.setColour (colours::textDim);
    g.setFont (uiFont (10.0f, true));
    g.drawText ("GAIN REDUCTION HISTORY", r.reduced (8.0f, 4.0f), juce::Justification::topLeft);
}
} // namespace lumen
