#pragma once

#include "ParamInfo.h"
#include <algorithm>
#include <vector>

namespace perf
{

/** The parameters a front panel shows for a plugin when nobody chose: the
    ones a player reaches for, found by name in a sensible order, then the
    first continuous ones to make up the number. Switches, anything called
    bypass, and the MIDI-controller stand-ins a VST3 publishes are left out.
    Shared by the Stage view's knobs and the tablet's. */
inline std::vector<int> pickPanelParams (const ParamInfoList& params, int max)
{
    static const char* const wanted[] = { "cutoff", "filter freq", "frequency", "resonance", "attack", "decay", "sustain", "release",
                                          "reverb", "delay", "chorus", "drive", "tone", "bright", "mix", "vibrato", "tremolo", "rate", "depth" };
    std::vector<int> picked;
    auto usable = [] (const ParamInfo& p) { return p.automatable && ! p.boolean && p.numSteps != 2 && p.midiController < 0 && ! p.name.containsIgnoreCase ("bypass"); };
    auto taken = [&picked] (int i) { return std::find (picked.begin(), picked.end(), i) != picked.end(); };
    for (const char* w : wanted)
    {
        if ((int) picked.size() >= max) break;
        for (const auto& p : params)
            if (usable (p) && ! taken (p.index) && p.name.containsIgnoreCase (w)) { picked.push_back (p.index); break; }
    }
    for (const auto& p : params)
    {
        if ((int) picked.size() >= max) break;
        if (usable (p) && ! taken (p.index)) picked.push_back (p.index);
    }
    return picked;
}

} // namespace perf
