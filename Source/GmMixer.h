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
/** The General MIDI family a program belongs to ("Piano", "Organ"...). */
juce::String gmFamilyName (int program);
/** The sound a General MIDI player has on the channels it takes notes from:
    one name when they agree, as a keyboard's do. */
juce::String gmSlotSoundName (const SlotDef&);
/** The drum kits' program numbers, in menu order. */
std::vector<int> gmKitPrograms();

/** What plays channel `channel` of the backing-track program `program` on
    input `track`: the instrument's name, with the GM player's instrument for
    that channel spelled out (`fileProgram` being the file's own choice). */
juce::String partInstrumentName (const Engine&, int track, int program, int channel, int fileProgram);

} // namespace perf
