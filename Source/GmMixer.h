#pragma once

#include "Engine.h"

namespace perf
{

/** The General MIDI player's mixer: one row per channel -- the instrument
    (the file's own, or any GM instrument; a kit on channel 10), volume, pan
    and mute. Opened from the player's row in the Instruments panel, as a
    plugin's Edit GUI would be. */
void showGmMixer (Engine&, int inputIndex, int program, int slot);

/** The General MIDI program names for a menu, by family, as idBase + program. */
void addGmInstrumentsToMenu (juce::PopupMenu&, int idBase, int selectedProgram = -1);
/** The standard drum kits (program numbers on channel 10), as idBase + program. */
void addGmKitsToMenu (juce::PopupMenu&, int idBase, int selectedProgram = -1);
juce::String gmKitName (int program);

} // namespace perf
