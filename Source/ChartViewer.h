#pragma once

#include "Engine.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace perf
{

/** Opens a song's chart on the laptop, in a window of its own: a PDF's pages
    one at a time, an image, or a text or ChordPro chart beside an editor
    that saves back to the file the tablet shows. Several charts on one song
    appear as tabs. Opening the same song's charts again brings the window
    forward instead of opening another. */
void showChartViewer (Engine&, int songIndex, int chartIndex);

/** The text of a ChordPro or plain chart laid out for reading: a title, a
    subtitle, comments, and each line of words with its chords set above it
    by character, as the tablet does. Monospace, so the chords line up. */
class ChartTextView : public juce::Component
{
public:
    void setText (const juce::String& text, bool chordPro);
    void setScale (float);                       // 1 = 15 px text
    float getScale() const { return scale; }
    int preferredHeight() const;
    int preferredWidth() const;
    void paint (juce::Graphics&) override;

private:
    struct Line { enum Kind { title, subtitle, comment, chords, words, plain } kind; juce::String text; };
    std::vector<Line> lines;
    float scale = 1.0f;
    float lineHeight() const;
};

} // namespace perf
