#pragma once

#include "ParamInfo.h"
#include <algorithm>
#include <vector>

namespace perf
{

/** The parameters a front panel shows for a plugin when nobody chose: the
    ones a player reaches for, found by name in a sensible order -- what
    defines the instrument first (drawbars, the filter, the Leslie), then the
    envelope, then the effects -- and then the first continuous ones to make
    up the number. Switches, anything called bypass, the MIDI-controller
    stand-ins a VST3 publishes, and the knobs of a compressor, limiter, EQ or
    gate section are left out of the search by name: an effect's "attack" is
    not the envelope's, and turning it with the effect off does nothing you
    can hear. Shared by the Stage view's knobs and the tablet's. */
inline std::vector<int> pickPanelParams (const ParamInfoList& params, int max)
{
    struct Want { const char* word; int upTo; };
    static const Want wanted[] = { { "drawbar", 9 }, { "cutoff", 1 }, { "filter freq", 1 }, { "frequency", 1 }, { "resonance", 1 },
                                   { "leslie", 1 }, { "rotary", 1 }, { "percussion", 1 }, { "attack", 1 }, { "decay", 1 }, { "sustain", 1 },
                                   { "release", 1 }, { "drive", 1 }, { "reverb", 1 }, { "delay", 1 }, { "chorus", 1 }, { "tone", 1 },
                                   { "bright", 1 }, { "mix", 1 }, { "vibrato", 1 }, { "tremolo", 1 }, { "rate", 1 }, { "depth", 1 } };
    static const char* const sections[] = { "compressor", "compress", "limiter", "equaliz", " eq", "eq ", "gate" };
    std::vector<int> picked;
    auto usable = [] (const ParamInfo& p) { return p.automatable && ! p.boolean && p.numSteps != 2 && p.midiController < 0 && ! p.name.containsIgnoreCase ("bypass"); };
    auto inEffectSection = [] (const ParamInfo& p) { for (const char* s : sections) if (p.name.containsIgnoreCase (s)) return true; return false; };
    auto taken = [&picked] (int i) { return std::find (picked.begin(), picked.end(), i) != picked.end(); };
    for (const auto& w : wanted)
    {
        int found = 0;
        for (const auto& p : params)
        {
            if ((int) picked.size() >= max || found >= w.upTo) break;
            if (usable (p) && ! taken (p.index) && ! inEffectSection (p) && p.name.containsIgnoreCase (w.word)) { picked.push_back (p.index); ++found; }
        }
    }
    for (const auto& p : params)
    {
        if ((int) picked.size() >= max) break;
        if (usable (p) && ! taken (p.index)) picked.push_back (p.index);
    }
    return picked;
}
} // namespace perf
