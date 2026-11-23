#include "PianoRoll.h"
#include "../PluginProcessor.h"

namespace vf::ui
{

static bool isBlack (int p) { const int k = p % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }

PianoRoll::PianoRoll (VoxForgeProcessor& p) : proc (p)
{
    setWantsKeyboardFocus (true);
    updateSize();
}

void PianoRoll::updateSize()
{
    const double len = juce::jmax (32.0, proc.project().lengthInBeats() + 16.0,
                                   proc.project().secondsToBeats (proc.stemPreview() != nullptr
                                       ? proc.stemPreview()->getNumSamples() / kEngineSampleRate : 0.0) + proc.project().stemOffsetBeats + 8.0);
    setSize ((int) (kKeyWidth + len * beatWidth), (kHighNote - kLowNote + 1) * kRow);
}

void PianoRoll::setZoom (double ppb)
{
    beatWidth = juce::jlimit (12.0, 400.0, ppb);
    updateSize();
    repaint();
}

juce::Rectangle<float> PianoRoll::noteRect (const Note& n) const
{
    return { beatToX (n.startBeat), pitchToY (n.pitch) + 1.0f, (float) juce::jmax (4.0, n.lengthBeats * beatWidth), (float) kRow - 2.0f };
}

void PianoRoll::paint (juce::Graphics& g)
{
    auto& prj = proc.project();
    auto clip = g.getClipBounds();
    const int viewX = viewport() != nullptr ? viewport()->getViewPositionX() : 0;

    // rows
    for (int p = kLowNote; p <= kHighNote; ++p)
    {
        const float y = pitchToY (p);
        g.setColour (isBlack (p) ? juce::Colour (0xff17181f) : juce::Colour (0xff1c1e26));
        g.fillRect ((float) clip.getX(), y, (float) clip.getWidth(), (float) kRow);
        if (p % 12 == 0)
        {
            g.setColour (Colours::outline.withAlpha (0.7f));
            g.drawHorizontalLine ((int) (y + kRow), (float) clip.getX(), (float) clip.getRight());
        }
    }

    // grid
    const double b0 = juce::jmax (0.0, std::floor (xToBeat ((float) clip.getX())));
    const double b1 = xToBeat ((float) clip.getRight()) + 1.0;
    const double sub = beatWidth > 120 ? 0.25 : (beatWidth > 50 ? 0.5 : 1.0);
    for (double b = b0; b <= b1; b += sub)
    {
        const float x = beatToX (b);
        const bool bar = std::fmod (b, 4.0) < 1.0e-6;
        const bool beat = std::fmod (b, 1.0) < 1.0e-6;
        g.setColour (bar ? Colours::outline : (beat ? Colours::outline.withAlpha (0.45f) : Colours::outline.withAlpha (0.18f)));
        g.drawVerticalLine ((int) x, 0.0f, (float) getHeight());
        if (bar)
        {
            g.setColour (Colours::dim);
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText (juce::String ((int) (b / 4.0) + 1), (int) x + 3, juce::jmax (0, viewport() != nullptr ? viewport()->getViewPositionY() : 0), 30, 14, juce::Justification::left);
        }
    }

    // stem pitch overlay (vocal replacement)
    if (! prj.srcF0.empty())
    {
        const double off = prj.stemOffsetBeats;
        juce::Path path;
        bool pen = false;
        for (size_t i = 0; i < prj.srcF0.size(); ++i)
        {
            const float m = prj.srcF0[i];
            const double beat = off + prj.secondsToBeats (i / prj.srcF0FrameRate);
            const float x = beatToX (beat);
            if (x < clip.getX() - 10 || x > clip.getRight() + 10) { pen = false; continue; }
            if (m <= 0.0f) { pen = false; continue; }
            const float y = pitchToY (0) - (m * kRow) + kRow * 0.5f;
            if (! pen) path.startNewSubPath (x, y); else path.lineTo (x, y);
            pen = true;
        }
        g.setColour (Colours::accent2.withAlpha (0.55f));
        g.strokePath (path, juce::PathStrokeType (1.5f));
    }

    // notes
    for (size_t i = 0; i < prj.notes.size(); ++i)
    {
        const auto& n = prj.notes[i];
        auto r = noteRect (n);
        if (! r.toNearestInt().intersects (clip)) continue;
        const bool hold = n.lyric.trim() == "-";
        auto col = n.fromSource ? (n.lyricChangedFromSource() ? Colours::warn : Colours::accent2.darker (0.35f)) : Colours::accent;
        if (hold) col = col.withAlpha (0.65f);
        g.setColour (n.selected ? col.brighter (0.45f) : col);
        g.fillRoundedRectangle (r, 3.5f);
        g.setColour (n.selected ? juce::Colours::white : col.darker (0.5f));
        g.drawRoundedRectangle (r, 3.5f, n.selected ? 1.6f : 1.0f);
        // velocity bar
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillRect (r.withTrimmedTop (r.getHeight() - 2.0f).withWidth (r.getWidth() * n.velocity));
        // lyric
        if (r.getWidth() > 14)
        {
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            g.drawText (n.lyric, r.reduced (4, 0).toNearestInt(), juce::Justification::centredLeft, true);
        }
    }

    // rubber band
    if (drag == Drag::Rubber)
    {
        g.setColour (Colours::accent.withAlpha (0.18f));
        g.fillRect (rubber);
        g.setColour (Colours::accent);
        g.drawRect (rubber);
    }

    // playhead
    const double ph = proc.playheadBeat();
    if (proc.hostIsPlaying() || proc.isPreviewing())
    {
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawVerticalLine ((int) beatToX (ph), 0.0f, (float) getHeight());
    }

    // piano keys (pinned to the left of the visible area)
    for (int p = kLowNote; p <= kHighNote; ++p)
    {
        const float y = pitchToY (p);
        g.setColour (isBlack (p) ? juce::Colour (0xff22242c) : juce::Colour (0xffd8dae2));
        g.fillRect ((float) viewX, y, (float) kKeyWidth, (float) kRow - 1.0f);
        if (p % 12 == 0)
        {
            g.setColour (juce::Colour (0xff30333d));
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText (juce::MidiMessage::getMidiNoteName (p, true, true, 4), viewX, (int) y, kKeyWidth - 3, kRow, juce::Justification::centredRight);
        }
    }
    g.setColour (Colours::outline);
    g.drawVerticalLine (viewX + kKeyWidth, 0.0f, (float) getHeight());
}

int PianoRoll::noteAt (juce::Point<int> p, bool& right, bool& left) const
{
    auto& notes = proc.project().notes;
    for (int i = (int) notes.size(); --i >= 0;)
    {
        auto r = noteRect (notes[(size_t) i]);
        if (r.contains (p.toFloat()))
        {
            right = p.x > r.getRight() - 6;
            left = ! right && p.x < r.getX() + 5 && r.getWidth() > 16;
            return i;
        }
    }
    right = left = false;
    return -1;
}

void PianoRoll::mouseMove (const juce::MouseEvent& e)
{
    bool r, l;
    const int i = noteAt (e.getPosition(), r, l);
    setMouseCursor (i >= 0 && (r || l) ? juce::MouseCursor::LeftRightResizeCursor
                                       : (i >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor));
}

void PianoRoll::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (lyricEditor != nullptr) finishLyricEdit (true);
    auto& notes = proc.project().notes;
    if (e.x < (viewport() != nullptr ? viewport()->getViewPositionX() : 0) + kKeyWidth) return;

    bool right, left;
    const int i = noteAt (e.getPosition(), right, left);
    dragStart = e.getPosition();

    if (i >= 0)
    {
        if (e.mods.isShiftDown() || e.mods.isCommandDown()) notes[(size_t) i].selected = ! notes[(size_t) i].selected;
        else if (! notes[(size_t) i].selected) { selectAll (false); notes[(size_t) i].selected = true; }
        dragNote = i;
        drag = right ? Drag::ResizeRight : (left ? Drag::ResizeLeft : Drag::Move);
        dragOrig = notes;
        if (onSelection) onSelection (firstSelected());
    }
    else
    {
        if (! e.mods.isShiftDown()) selectAll (false);
        drag = Drag::Rubber;
        rubber = { e.x, e.y, 0, 0 };
        if (onSelection) onSelection (-1);
    }
    repaint();
}

void PianoRoll::mouseDrag (const juce::MouseEvent& e)
{
    auto& notes = proc.project().notes;
    if (drag == Drag::Rubber)
    {
        rubber = juce::Rectangle<int> (dragStart, e.getPosition());
        for (auto& n : notes) n.selected = noteRect (n).toNearestInt().intersects (rubber);
        repaint();
        return;
    }
    if (dragNote < 0 || dragOrig.size() != notes.size()) return;

    const double dBeat = (e.x - dragStart.x) / beatWidth;
    const int dPitch = -(int) std::round ((e.y - dragStart.y) / (double) kRow);
    const auto& anchor = dragOrig[(size_t) dragNote];

    for (size_t k = 0; k < notes.size(); ++k)
    {
        if (! notes[k].selected) continue;
        const auto& o = dragOrig[k];
        auto& n = notes[k];
        if (drag == Drag::Move)
        {
            const double newAnchor = e.mods.isAltDown() ? anchor.startBeat + dBeat : snap (anchor.startBeat + dBeat);
            n.startBeat = juce::jmax (0.0, o.startBeat + (newAnchor - anchor.startBeat));
            n.pitch = juce::jlimit (kLowNote, kHighNote, o.pitch + dPitch);
        }
        else if (drag == Drag::ResizeRight)
        {
            const double end = e.mods.isAltDown() ? o.endBeat() + dBeat : snap (o.endBeat() + dBeat);
            n.lengthBeats = juce::jmax (grid > 0 ? grid * 0.5 : 0.05, end - o.startBeat);
        }
        else if (drag == Drag::ResizeLeft)
        {
            const double st = juce::jmin (o.endBeat() - 0.05, e.mods.isAltDown() ? o.startBeat + dBeat : snap (o.startBeat + dBeat));
            n.startBeat = juce::jmax (0.0, st);
            n.lengthBeats = o.endBeat() - n.startBeat;
        }
    }
    repaint();
}

void PianoRoll::mouseUp (const juce::MouseEvent& e)
{
    const bool moved = e.getDistanceFromDragStart() > 2;
    if ((drag == Drag::Move || drag == Drag::ResizeRight || drag == Drag::ResizeLeft) && moved)
        changed();
    if (drag == Drag::Rubber && onSelection) onSelection (firstSelected());
    drag = Drag::None;
    dragNote = -1;
    repaint();
}

void PianoRoll::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto& notes = proc.project().notes;
    bool r, l;
    const int i = noteAt (e.getPosition(), r, l);
    if (i >= 0) { editLyric (i); return; }
    if (e.x < (viewport() != nullptr ? viewport()->getViewPositionX() : 0) + kKeyWidth) return;

    Note n;
    n.startBeat = juce::jmax (0.0, std::floor (xToBeat ((float) e.x) / juce::jmax (0.0625, grid)) * juce::jmax (0.0625, grid));
    n.lengthBeats = juce::jmax (0.25, grid > 0 ? grid * 2.0 : 0.5);
    n.pitch = yToPitch ((float) e.y);
    n.lyric = "la";
    selectAll (false);
    n.selected = true;
    notes.push_back (n);
    proc.project().sortNotes();
    changed();
    editLyric (firstSelected());
}

void PianoRoll::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown())
    {
        setZoom (beatWidth * (w.deltaY > 0 ? 1.15 : 0.87));
        return;
    }
    Component::mouseWheelMove (e, w);   // let the viewport scroll
}

bool PianoRoll::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) { deleteSelected(); return true; }
    if (k == juce::KeyPress::upKey)   { transposeSelected (k.getModifiers().isShiftDown() ? 12 : 1); return true; }
    if (k == juce::KeyPress::downKey) { transposeSelected (k.getModifiers().isShiftDown() ? -12 : -1); return true; }
    if (k.getModifiers().isCommandDown() && k.getKeyCode() == 'A') { selectAll (true); return true; }
    if (k.getModifiers().isCommandDown() && k.getKeyCode() == 'Q') { quantizeSelected(); return true; }
    if (k == juce::KeyPress::returnKey) { if (firstSelected() >= 0) editLyric (firstSelected()); return true; }
    return false;
}

void PianoRoll::selectAll (bool sel)
{
    for (auto& n : proc.project().notes) n.selected = sel;
    repaint();
    if (onSelection) onSelection (firstSelected());
}

int PianoRoll::firstSelected() const
{
    auto& notes = proc.project().notes;
    for (size_t i = 0; i < notes.size(); ++i) if (notes[i].selected) return (int) i;
    return -1;
}

void PianoRoll::deleteSelected()
{
    auto& notes = proc.project().notes;
    const auto before = notes.size();
    notes.erase (std::remove_if (notes.begin(), notes.end(), [] (const Note& n) { return n.selected; }), notes.end());
    if (notes.size() != before) changed();
}

void PianoRoll::quantizeSelected()
{
    if (grid <= 0) return;
    for (auto& n : proc.project().notes)
        if (n.selected)
        {
            const double end = snap (n.endBeat());
            n.startBeat = snap (n.startBeat);
            n.lengthBeats = juce::jmax (grid, end - n.startBeat);
        }
    changed();
}

void PianoRoll::transposeSelected (int semis)
{
    bool any = false;
    for (auto& n : proc.project().notes)
        if (n.selected) { n.pitch = juce::jlimit (kLowNote, kHighNote, n.pitch + semis); any = true; }
    if (any) changed();
}

void PianoRoll::changed()
{
    proc.project().sortNotes();
    updateSize();
    repaint();
    if (onChanged) onChanged();
}

void PianoRoll::scrollToNote (int i)
{
    auto& notes = proc.project().notes;
    if (auto* vp = viewport(); vp != nullptr && i >= 0 && i < (int) notes.size())
    {
        auto r = noteRect (notes[(size_t) i]).toNearestInt();
        const auto view = vp->getViewArea();
        if (! view.contains (r))
            vp->setViewPosition (juce::jmax (0, r.getX() - view.getWidth() / 3), juce::jmax (0, r.getCentreY() - view.getHeight() / 2));
    }
}

void PianoRoll::centreOnPitch (int midi)
{
    if (auto* vp = viewport())
        vp->setViewPosition (vp->getViewPositionX(), juce::jmax (0, (int) pitchToY (midi) - vp->getHeight() / 2));
}

//==============================================================================
void PianoRoll::editLyric (int i)
{
    auto& notes = proc.project().notes;
    if (i < 0 || i >= (int) notes.size()) return;
    scrollToNote (i);
    editingNote = i;
    lyricEditor = std::make_unique<juce::TextEditor>();
    lyricEditor->setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    lyricEditor->setText (notes[(size_t) i].lyric, false);
    lyricEditor->selectAll();
    lyricEditor->addListener (this);
    auto r = noteRect (notes[(size_t) i]).toNearestInt();
    lyricEditor->setBounds (r.getX(), r.getY() - 4, juce::jmax (90, r.getWidth()), kRow + 8);
    addAndMakeVisible (*lyricEditor);
    lyricEditor->grabKeyboardFocus();
}

void PianoRoll::finishLyricEdit (bool commit)
{
    if (lyricEditor == nullptr) return;
    auto& notes = proc.project().notes;
    if (commit && editingNote >= 0 && editingNote < (int) notes.size())
    {
        auto text = lyricEditor->getText().trim();
        if (text.isEmpty()) text = "-";
        auto& n = notes[(size_t) editingNote];
        if (text != n.lyric)
        {
            n.lyric = text;
            n.phonemes = {};            // re-derive phonemes from the new text
            if (text.startsWithChar ('[')) n.phonemes = text.removeCharacters ("[]");
            if (onChanged) onChanged();
        }
    }
    lyricEditor->removeListener (this);
    auto* ed = lyricEditor.release();
    juce::MessageManager::callAsync ([ed] { delete ed; });
    editingNote = -1;
    repaint();
}

void PianoRoll::textEditorReturnKeyPressed (juce::TextEditor& ed)
{
    if (&ed != lyricEditor.get()) return;
    const int next = editingNote + 1;
    finishLyricEdit (true);
    // Return jumps to the next note (fast lyric entry), Escape stops
    if (next < (int) proc.project().notes.size())
    {
        selectAll (false);
        proc.project().notes[(size_t) next].selected = true;
        editLyric (next);
    }
    else grabKeyboardFocus();
}

void PianoRoll::textEditorEscapeKeyPressed (juce::TextEditor& ed)
{
    if (&ed != lyricEditor.get()) return;
    finishLyricEdit (false);
    grabKeyboardFocus();
}

void PianoRoll::textEditorFocusLost (juce::TextEditor& ed)
{
    if (&ed == lyricEditor.get()) finishLyricEdit (true);
}

} // namespace vf::ui
