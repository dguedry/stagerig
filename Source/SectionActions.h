#pragma once

#include "Engine.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace perf
{

/** The cue sheet of one section of a song's backing track: what happens the
    moment the track enters it. A window with the section's actions, a Try
    button on each, and + Add..., which offers the four kinds in the setup's
    own words -- keyboards by name, programs by number and name, parts by
    channel, effects by what they are, MIDI devices by port. `changed` is
    called after every edit so the window behind can refresh. */
void showSectionActions (Engine&, int songIndex, int markerIndex, std::function<void()> changed);

} // namespace perf
