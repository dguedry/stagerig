#pragma once

#include "Engine.h"
#include "LevelMeter.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace perf
{

/** The big "what is playing" display.

    Meant to be read at playing distance, from a laptop on a stand: for each
    input, the program number as sent from the keyboard and the program's name,
    as large as the space allows. When you pick programs by number on the
    keyboard you get no confirmation of what loaded; this is that confirmation.
    A program still loading is marked, so a silent keyboard is explained. */
class StagePanel : public juce::Component,
                   private juce::Timer
{
public:
    static constexpr int preferredHeight = 132;

    explicit StagePanel (Engine& e) : engine (e) { startTimerHz (20); }

    void paint (juce::Graphics& g) override
    {
        const auto& inputs = engine.getSetup().inputs;
        g.setColour (juce::Colour (0xff101116));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
        if (inputs.empty()) return;

        // The backing track's program follows the song: nothing to read off at a glance.
        std::vector<size_t> shown;
        for (size_t i = 0; i < inputs.size(); ++i) if (! inputs[i].isTrack()) shown.push_back (i);
        if (shown.empty()) return;
        auto r = getLocalBounds().reduced (10, 8);
        const int each = r.getWidth() / (int) shown.size();
        for (size_t i : shown)
        {
            auto cell = r.removeFromLeft (each).reduced (6, 0);
            const auto& def = inputs[i];
            const int program = def.currentProgram;
            const auto& prog = def.programs[(size_t) program];
            bool loading = false;
            for (int slot = 0; slot < (int) prog.slots.size() && ! loading; ++slot)
                loading = engine.isPluginLoading ((int) i, program, slot);
            for (int e = 0; e < (int) prog.effects.size() && ! loading; ++e)
                loading = engine.isPluginLoading ((int) i, program, -1, e);
            // An audio input's program with no effects is a pass-through, not nothing.
            const bool empty = def.isAudio() ? prog.name.isEmpty() && prog.effects.empty() : prog.isEmpty();

            // input name, small and dim: you know which manual is which, it is
            // the program that needs reading
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.setColour (juce::Colour (0xff9aa0ab));
            g.drawText (def.name.toUpperCase(), cell.removeFromTop (16), juce::Justification::centredLeft, true);

            auto numberArea = cell.removeFromLeft (juce::jmin (108, cell.getWidth() / 3));
            g.setFont (juce::FontOptions ((float) juce::jmin (54, numberArea.getHeight()), juce::Font::bold));
            g.setColour (empty ? juce::Colour (0xff4a4d57) : juce::Colour (0xff5aa9ff));
            g.drawText (juce::String (program).paddedLeft ('0', 3), numberArea, juce::Justification::centredLeft, false);

            // An audio input shows what is arriving, under its program name.
            if (def.isAudio())
                perf::LevelMeterPainter::paint (g, cell.removeFromBottom (12).withTrimmedTop (2), engine.getInputLevel ((int) i));

            juce::String name = prog.name.isNotEmpty() ? prog.name : empty ? (def.isAudio() ? "straight through" : "empty program") : "(unnamed)";
            if (loading) name += "  ...loading";
            g.setFont (juce::FontOptions ((float) juce::jmin (34, cell.getHeight() - 4), juce::Font::bold));
            g.setColour (empty ? juce::Colour (0xff6b7079) : (loading ? juce::Colour (0xffd8a657) : juce::Colours::white));
            g.drawFittedText (name, cell, juce::Justification::centredLeft, 2, 0.6f);
        }
    }

private:
    void timerCallback() override
    {
        // Repaint only when something visible changed: this runs all night.
        juce::String now;
        const auto& inputs = engine.getSetup().inputs;
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            const int p = inputs[i].currentProgram;
            bool loading = false;
            const auto& prog = inputs[i].programs[(size_t) p];
            for (int slot = 0; slot < (int) prog.slots.size() && ! loading; ++slot)
                loading = engine.isPluginLoading ((int) i, p, slot);
            now << inputs[i].name << "|" << p << "|" << prog.name << "|" << (loading ? "L" : "-");
            if (inputs[i].isAudio())
            {
                const auto l = engine.getInputLevel ((int) i);
                now << "|" << (int) (LevelMeterPainter::position (l.left) * 100) << "," << (int) (LevelMeterPainter::position (l.right) * 100) << (l.clipped ? "!" : "");
            }
            now << ";";
        }
        if (now != shown) { shown = now; repaint(); }
    }

    Engine& engine;
    juce::String shown;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StagePanel)
};

} // namespace perf
