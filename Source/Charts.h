#pragma once

#include <juce_core/juce_core.h>

namespace perf
{

/**
    Sheet music for the stand: what counts as a chart, how to serve it, and the
    page images a PDF is turned into so every tablet shows it the same way.

    A PDF is pre-rendered to PNG pages when it is attached, because browsers
    differ about PDFs inside a page -- iPad Safari shows the first page in a
    frame and will not scroll it -- while an image is an image everywhere, and
    pages the tablet can turn one at a time. pdftoppm does the rendering; where
    it is not installed the PDF is handed to the browser as it is.
*/

/** "pdf", "image", "text", "chordpro", or empty for something unsupported. */
juce::String chartKindFor (const juce::File&);

bool isChartFile (const juce::File&);

/** For a file chooser. */
juce::String chartFilePatterns();

/** Audio a song can carry as a backing-track stem: WAV, AIFF, FLAC, Ogg, MP3. */
bool isTrackFile (const juce::File&);
juce::String trackFilePatterns();

/** The MIME type to serve the file with. */
juce::String chartContentTypeFor (const juce::File&);

/** Where a chart's rendered pages live: "<chart>.pages/" beside it. */
juce::File chartPagesFolder (const juce::File& chart);

/** The rendered pages of a PDF, in order. Empty if not rendered. */
juce::Array<juce::File> chartPages (const juce::File& chart);

/** Renders a PDF to page images, replacing any previous rendering. Blocks;
    see prerenderPdfPagesAsync. False, with a reason, if it could not. */
bool prerenderPdfPages (const juce::File& pdf, juce::String& error);

/** The same on a background thread, because a thick score takes seconds. */
void prerenderPdfPagesAsync (const juce::File& pdf);

} // namespace perf
