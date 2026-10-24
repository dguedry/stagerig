#pragma once

#include "Engine.h"

namespace perf
{

/** Menus and names for the General MIDI player and for a MIDI file's parts. */

/** The General MIDI program names for a menu, by family, as idBase + program. */
void addGmInstrumentsToMenu (juce::PopupMenu&, int idBase, int selectedProgram = -1);
/** The standard drum kits (program numbers on channel 10), as idBase + program. */
void addGmKitsToMenu (juce::PopupMenu&, int idBase, int selectedProgram = -1);
juce::String gmKitName (int program);

/** What plays channel `channel` of the backing-track program `program` on
    input `track`: the instrument's name, with the GM player's instrument for
    that channel spelled out (`fileProgram` being the file's own choice). */
juce::String partInstrumentName (const Engine&, int track, int program, int channel, int fileProgram);

} // namespace perf
