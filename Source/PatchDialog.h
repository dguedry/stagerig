#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace perf
{

/** Patch recall for something on a MIDI port: bank select and program
    change, sent when the program is selected. -1 = not sent. `done` gets
    (bankMsb, bankLsb, program), each -1 for "leave it". */
void showPatchRecallDialog (const juce::String& portName, int msb, int lsb, int patch, std::function<void (int, int, int)> done);

} // namespace perf
