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

/** What a song can carry as a backing-track stem: audio (WAV, AIFF, FLAC, Ogg, MP3) or a MIDI file. */
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

/** A note by pitch class (0 = C .. 11 = B), spelt with flats or sharps. */
juce::String noteNameFor (int pitchClass, bool flats);

/** The pitch class a note name starts with ("Bb", "F#m7" -> 10, 6), or -1 if it is not one. */
int pitchClassOf (const juce::String& name);

/** The key a ChordPro chart says it is in ({key: G}, {key: Am}): its root, or -1 when it does not say. */
int chordProKeyRoot (const juce::String& text);

/** A chord name moved by `semitones` -- its root and any slash bass, the rest of the name kept -- spelt with flats or sharps. "N.C." and the like come back as they are. */
juce::String transposeChordName (const juce::String& chord, int semitones, bool flats);

/** The chart with every [chord] moved from the key it names to `toRoot`, and its {key:} line following.
    Unchanged when it names no key, or is in that key already. The words are never touched. */
juce::String transposeChordPro (const juce::String& text, int toRoot, bool flats);

} // namespace perf
