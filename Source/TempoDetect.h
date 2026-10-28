#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>

namespace perf
{

/** What detectTempo() found in a recording. */
struct TempoEstimate
{
    bool ok = false;
    double bpm = 0.0;            // the steady tempo
    double firstBeat = 0.0;      // seconds into the file where beat 1 of a bar first falls
    double confidence = 0.0;     // 0..1: how clearly one tempo stood out from the rest
    juce::String problem;        // why not, when ! ok
};

/** Finds the tempo of a recording with a steady beat, and where its first
    downbeat falls, so a click, a count-in and a bar grid can line up with
    it. Reads the whole file, a few seconds' work for a song, so call it off
    the message thread. Assumes one tempo throughout: a track that speeds
    up, slows down or is played rubato gets whatever tempo it mostly keeps,
    with a low confidence.

    How: an onset strength curve (spectral flux, ~94 frames a second) is
    autocorrelated over 50-220 bpm with a preference for tempi near 120;
    the beats are then placed on that curve and fitted by least squares,
    which pins the tempo down to a fraction of a bpm on a long file. The
    downbeat is the beat position, of `beatsPerBar`, where the low end
    (the kick) is strongest.

    `shouldStop` is polled while reading; `progress` is told 0..1. */
TempoEstimate detectTempo (juce::AudioFormatReader& reader, int beatsPerBar,
                           std::function<bool()> shouldStop = {}, std::function<void (float)> progress = {});

} // namespace perf
