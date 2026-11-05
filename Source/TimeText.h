#pragma once

#include <juce_core/juce_core.h>

namespace perf
{

/** A time in seconds as a clock reads it -- "3:07", or with decimals "3:07.4"
    -- the one way everywhere a time is shown. */
inline juce::String clockText (double seconds, int decimals = 0)
{
    seconds = juce::jmax (0.0, seconds);
    const int m = (int) (seconds / 60.0);
    const double s = seconds - m * 60.0;
    return juce::String (m) + ":" + (decimals > 0 ? juce::String (s, decimals).paddedLeft ('0', 3 + decimals)
                                                 : juce::String ((int) s).paddedLeft ('0', 2));
}

/** The time a player typed: "3:07.5", "187.5" or "187" are all 187.5 s. */
inline double parseClock (const juce::String& text)
{
    const auto t = text.trim();
    if (t.containsChar (':'))
        return juce::jmax (0.0, t.upToFirstOccurrenceOf (":", false, false).getDoubleValue() * 60.0
                              + t.fromFirstOccurrenceOf (":", false, false).getDoubleValue());
    return juce::jmax (0.0, t.getDoubleValue());
}

} // namespace perf
