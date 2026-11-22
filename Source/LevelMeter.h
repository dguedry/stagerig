#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"

namespace perf
{

/** A stereo peak meter for an audio input, drawn the same wherever it appears:
    the inputs list, the effects header, the stage display. Two bars on a dB
    scale from -60 to 0, green to -12, amber to -3, red above, and a mark that
    stays lit for a moment after a clip -- the one thing the player needs to
    notice from across the stage. */
struct LevelMeterPainter
{
    static float position (float level)
    {
        if (level <= 0.001f) return 0.0f;
        const float db = juce::Decibels::gainToDecibels (level, -60.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    }

    static void paint (juce::Graphics& g, juce::Rectangle<int> area, const Engine::InputLevel& level)
    {
        auto r = area.toFloat();
        const auto clipW = juce::jmin (6.0f, r.getWidth() * 0.08f);
        auto clip = r.removeFromRight (clipW);
        r.removeFromRight (2.0f);

        auto bar = [&] (juce::Rectangle<float> b, float v)
        {
            g.setColour (juce::Colour (0xff000712));
            g.fillRoundedRectangle (b, 2.0f);
            const float x = position (v);
            if (x <= 0.0f) return;
            // Segments by colour, so the bar reads at a glance: where it ends and what colour it ended in.
            const float green = 0.8f, amber = 0.95f;     // -12 dB and -3 dB on the scale
            auto seg = [&] (float from, float to, juce::Colour c)
            {
                if (x <= from) return;
                auto s = b.withX (b.getX() + b.getWidth() * from).withWidth (b.getWidth() * (juce::jmin (x, to) - from));
                g.setColour (c);
                g.fillRect (s);
            };
            seg (0.0f, green, juce::Colour (0xff4fb86a));
            seg (green, amber, juce::Colour (0xffcea04a));
            seg (amber, 1.0f, juce::Colour (0xffe04a4a));
        };
        const float gap = 2.0f;
        const float h = (r.getHeight() - gap) / 2.0f;
        bar (r.removeFromTop (h), level.left);
        r.removeFromTop (gap);
        bar (r, level.right);

        g.setColour (level.clipped ? juce::Colour (0xffff3b3b) : juce::Colour (0xff1a3555));
        g.fillRoundedRectangle (clip, 2.0f);
    }
};

/** The meter as a component, repainting itself while the level moves. */
class LevelMeter : public juce::Component,
                   public juce::SettableTooltipClient,
                   private juce::Timer
{
public:
    explicit LevelMeter (Engine& e) : engine (e) { startTimerHz (30); }
    void setInput (int inputIndex) { input = inputIndex; repaint(); }

    void paint (juce::Graphics& g) override
    {
        LevelMeterPainter::paint (g, getLocalBounds(), engine.getInputLevel (input));
    }

private:
    void timerCallback() override
    {
        const auto l = engine.getInputLevel (input);
        const int key = (int) (LevelMeterPainter::position (l.left) * 200) * 1000
                      + (int) (LevelMeterPainter::position (l.right) * 200) * 2 + (l.clipped ? 1 : 0);
        if (key != lastKey) { lastKey = key; repaint(); }
    }

    Engine& engine;
    int input = -1;
    int lastKey = -1;
};

} // namespace perf
