// Lumen MC-64 — plugin-level tests (processor state, undo, presets, A/B, threading).
// Run headless as part of CTest: lumen_plugin_tests
#include "../plugin/Source/PluginProcessor.h"

#include <cstdio>
#include <memory>
#include <thread>

namespace
{
int failures = 0;
#define EXPECT(cond, ...)                                                       \
    do {                                                                        \
        if (! (cond)) {                                                         \
            ++failures;                                                         \
            std::printf ("  FAIL %s:%d: %s — ", __FILE__, __LINE__, #cond);     \
            std::printf (__VA_ARGS__);                                          \
            std::printf ("\n");                                                 \
        }                                                                       \
    } while (0)

void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil (30); }

float value (lumen::LumenAudioProcessor& p, const char* id)
{
    auto* rp = p.apvts.getParameter (id);
    return rp->convertFrom0to1 (rp->getValue());
}

void hostAutomate (lumen::LumenAudioProcessor& p, const char* id, float v)
{
    auto* rp = p.apvts.getParameter (id); // what a VST3 host does: setValue, no gestures
    rp->setValueNotifyingHost (rp->convertTo0to1 (v));
}

void guiEdit (lumen::LumenAudioProcessor& p, const char* id, std::initializer_list<float> dragValues)
{
    auto* rp = p.apvts.getParameter (id); // what a slider attachment does during a drag
    rp->beginChangeGesture();
    for (float v : dragValues)
        rp->setValueNotifyingHost (rp->convertTo0to1 (v));
    rp->endChangeGesture();
}

void testUndoIgnoresAutomation()
{
    auto pOwner = std::make_unique<lumen::LumenAudioProcessor>(); // heap: the engine holds MBs of
    auto& p = *pOwner;        // preallocated buffers (Windows stacks are 1 MB)
    for (int i = 0; i < 50; ++i)
    {
        hostAutomate (p, lumen::ids::threshold, -30.0f + 0.5f * (float) i);
        pump(); // let JUCE's parameter-tree timer run: that is where automation used to be recorded
    }
    EXPECT (! p.undoManager.canUndo(), "host automation was recorded as an undo step");
}

void testGuiEditsAreUndoable()
{
    auto pOwner = std::make_unique<lumen::LumenAudioProcessor>(); // heap: the engine holds MBs of
    auto& p = *pOwner;        // preallocated buffers (Windows stacks are 1 MB)
    const float before = value (p, lumen::ids::threshold);
    guiEdit (p, lumen::ids::threshold, { -20.0f, -25.0f, -30.0f }); // one drag
    EXPECT (p.undoManager.canUndo(), "GUI edit not recorded");
    hostAutomate (p, lumen::ids::ratio, 6.0f);                     // automation in between
    pump();
    EXPECT (p.undoManager.undo(), "undo failed");
    EXPECT (std::abs (value (p, lumen::ids::threshold) - before) < 1e-3f, "undo restored %.2f, expected %.2f",
            value (p, lumen::ids::threshold), before);
    EXPECT (! p.undoManager.canUndo(), "a drag must be exactly one undo step");
    EXPECT (p.undoManager.redo(), "redo failed");
    EXPECT (std::abs (value (p, lumen::ids::threshold) + 30.0f) < 1e-3f, "redo gave %.2f", value (p, lumen::ids::threshold));
}

void testPresetLoadAndABAreSingleSteps()
{
    auto pOwner = std::make_unique<lumen::LumenAudioProcessor>(); // heap: the engine holds MBs of
    auto& p = *pOwner;        // preallocated buffers (Windows stacks are 1 MB)
    const float thr0 = value (p, lumen::ids::threshold), ratio0 = value (p, lumen::ids::ratio);
    p.presets.loadPreset (1); // touches many parameters
    EXPECT (std::abs (value (p, lumen::ids::threshold) - thr0) > 0.1f, "preset did not change threshold");
    EXPECT (p.undoManager.undo(), "undo of preset failed");
    EXPECT (std::abs (value (p, lumen::ids::threshold) - thr0) < 1e-3f && std::abs (value (p, lumen::ids::ratio) - ratio0) < 1e-3f,
            "preset undo did not restore the previous state");
    EXPECT (! p.undoManager.canUndo(), "preset load must be exactly one undo step");

    guiEdit (p, lumen::ids::makeup, { 3.0f });
    p.switchToSlot (1); // B starts as a copy of the defaults
    EXPECT (std::abs (value (p, lumen::ids::makeup)) < 1e-3f, "slot B should have default makeup");
    EXPECT (p.undoManager.undo(), "undo of A/B switch failed");
    EXPECT (std::abs (value (p, lumen::ids::makeup) - 3.0f) < 1e-3f, "A/B undo restored %.2f", value (p, lumen::ids::makeup));
}

void testStateRoundTrip()
{
    juce::MemoryBlock blob;
    {
        auto pOwner = std::make_unique<lumen::LumenAudioProcessor>(); // heap: the engine holds MBs of
        auto& p = *pOwner;        // preallocated buffers (Windows stacks are 1 MB)
        p.presets.loadPreset (3);
        guiEdit (p, lumen::ids::knee, { 9.0f });
        p.switchToSlot (1);
        guiEdit (p, lumen::ids::ratio, { 7.0f });
        p.getStateInformation (blob);
    }
    auto qOwner = std::make_unique<lumen::LumenAudioProcessor>(); // heap: the engine holds MBs of
    auto& q = *qOwner;        // preallocated buffers (Windows stacks are 1 MB)
    q.setStateInformation (blob.getData(), (int) blob.getSize());
    pump();
    EXPECT (q.presets.getCurrentIndex() == 3, "preset index restored as %d", q.presets.getCurrentIndex());
    EXPECT (q.getActiveSlot() == 1, "active slot restored as %d", q.getActiveSlot());
    EXPECT (std::abs (value (q, lumen::ids::ratio) - 7.0f) < 1e-3f, "slot B ratio %.2f", value (q, lumen::ids::ratio));
    q.switchToSlot (0);
    EXPECT (std::abs (value (q, lumen::ids::knee) - 9.0f) < 1e-3f, "slot A knee %.2f", value (q, lumen::ids::knee));
    EXPECT (q.undoManager.canUndo(), "switch after restore should be undoable");
}

void testStateSaveIsSafeAgainstABSwitching()
{
    // smoke test for the A/B lock: a "host" thread saves state while the UI flips A/B
    auto pOwner = std::make_unique<lumen::LumenAudioProcessor>(); // heap: the engine holds MBs of
    auto& p = *pOwner;        // preallocated buffers (Windows stacks are 1 MB)
    std::atomic<bool> stop { false };
    std::atomic<int> saves { 0 };
    std::thread host ([&] {
        juce::MemoryBlock mb;
        while (! stop.load())
        {
            p.getStateInformation (mb);
            ++saves;
        }
    });
    for (int i = 0; i < 2000; ++i)
        p.switchToSlot (i % 2);
    stop = true;
    host.join();
    juce::MemoryBlock mb;
    p.getStateInformation (mb);
    EXPECT (mb.getSize() > 100 && saves.load() > 0, "state save produced %d bytes after %d concurrent saves", (int) mb.getSize(), saves.load());
}

void testHistoryIndexWraps()
{
    lumen::GrHistory h;
    h.writeIndex = 0xFFFFFFF0u; // just before the 2^32 wrap
    for (int i = 0; i < 64; ++i)
        h.push ((float) i, 0.0f);
    EXPECT (h.writeIndex.load() == 0x30u, "write index after wrap: %u", h.writeIndex.load());
    // the newest value sits at (w - 1) & mask, continuous across the wrap
    EXPECT (h.gr[(h.writeIndex.load() - 1u) & (lumen::GrHistory::kSize - 1)] == 63.0f, "newest slot wrong after wrap");
    uint32_t reader = 0xFFFFFFF0u, count = 0;
    while (reader != h.writeIndex.load()) { ++reader; ++count; }
    EXPECT (count == 64u, "reader saw %u new values across the wrap", count);
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    std::printf ("Lumen MC-64 plugin tests\n");
    testUndoIgnoresAutomation();
    testGuiEditsAreUndoable();
    testPresetLoadAndABAreSingleSteps();
    testStateRoundTrip();
    testStateSaveIsSafeAgainstABSwitching();
    testHistoryIndexWraps();
    std::printf ("%s: %d failure(s)\n", failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
