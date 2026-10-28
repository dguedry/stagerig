#pragma once

#include <juce_core/juce_core.h>
#include <cstdint>
#include <functional>
#include <vector>

namespace perf
{

/** Changes the length and the pitch of a stereo recording without changing
    the other: a backing track played at tonight's tempo, or taken down a
    step for the singer. Offline, on the loaded track, so there is no cost
    while it plays; a song re-renders in a few seconds when its tempo or
    transpose changes.

    How: the pitch is moved by resampling, which also moves the length, and
    the length is then put where it should be by WSOLA -- overlapping
    windows of the sound laid down at the new spacing, each one nudged to
    where it joins the last without a seam. Good for the changes a gig
    needs, a few percent of tempo and a few semitones; a track stretched far
    from its own speed starts to smear, as every such method does.

    `timeScale` is the new length over the old (1.2 plays slower, longer);
    `semitones` moves the pitch. `shouldStop` is polled between frames. */
void stretchAndShift (std::vector<int16_t>& left, std::vector<int16_t>& right,
                      double timeScale, double semitones, double sampleRate,
                      std::function<bool()> shouldStop = {});

} // namespace perf
