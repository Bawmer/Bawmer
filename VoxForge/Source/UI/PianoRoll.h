#pragma once

#include "LookAndFeel.h"
#include "../Core/VocalTypes.h"

class VoxForgeProcessor;

namespace vf::ui
{

/** MIDI + lyrics editor: notes with syllables, source pitch overlay, playhead. */
class PianoRoll : public juce::Component, private juce::TextEditor::Listener
{
public:
    explicit PianoRoll (VoxForgeProcessor& p);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

    void setZoom (double pixelsPerBeat);
    double getZoom() const noexcept { return beatWidth; }
    void setGrid (double beats) { grid = beats; }
    void updateSize();
    void selectAll (bool sel);
    void deleteSelected();
    void quantizeSelected();
    void transposeSelected (int semis);
    int firstSelected() const;
    void editLyric (int noteIndex);
    void scrollToNote (int noteIndex);
    void centreOnPitch (int midi);

    std::function<void()> onChanged;          // notes edited
    std::function<void (int)> onSelection;    // first selected note (-1 = none)

    static constexpr int kLowNote = 33, kHighNote = 96, kKeyWidth = 46, kRow = 14;

private:
    enum class Drag { None, Move, ResizeRight, ResizeLeft, Rubber, Create };

    int noteAt (juce::Point<int> p, bool& onRightEdge, bool& onLeftEdge) const;
    juce::Rectangle<float> noteRect (const Note& n) const;
    double xToBeat (float x) const { return (x - kKeyWidth) / beatWidth; }
    float beatToX (double b) const { return (float) (kKeyWidth + b * beatWidth); }
    int yToPitch (float y) const { return kHighNote - (int) std::floor (y / kRow); }
    float pitchToY (int p) const { return (float) ((kHighNote - p) * kRow); }
    double snap (double beat) const { return grid > 0 ? std::round (beat / grid) * grid : beat; }
    void changed();
    void finishLyricEdit (bool commit);
    void textEditorReturnKeyPressed (juce::TextEditor&) override;
    void textEditorEscapeKeyPressed (juce::TextEditor&) override;
    void textEditorFocusLost (juce::TextEditor&) override;
    juce::Viewport* viewport() const { return findParentComponentOfClass<juce::Viewport>(); }

    VoxForgeProcessor& proc;
    double beatWidth = 80.0, grid = 0.25;
    Drag drag = Drag::None;
    juce::Point<int> dragStart;
    std::vector<Note> dragOrig;
    int dragNote = -1;
    juce::Rectangle<int> rubber;
    std::unique_ptr<juce::TextEditor> lyricEditor;
    int editingNote = -1;
};

/** Viewport that repaints the piano keys when scrolled. */
class RollViewport : public juce::Viewport
{
public:
    void visibleAreaChanged (const juce::Rectangle<int>&) override { if (auto* c = getViewedComponent()) c->repaint(); }
};

} // namespace vf::ui
