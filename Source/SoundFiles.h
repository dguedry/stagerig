#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Model.h"

namespace perf
{
class PluginHost;

/** A ready sound: one program in a file of its own -- its instruments,
    effects, splits, levels and knob mappings -- to open into any program.
    StageRig Sounds ships them beside its instruments; a player saves their
    own from any program (into "My sounds" beside the settings).

    The file is JSON: { "stagerigSound": 1, "name", "kind", "about",
    "program": the program as a setup holds it }. Two things make one travel:

    - A sampled instrument inside the sound's own folder is written relative
      to the file, so a pack works wherever it was unpacked. Any other is
      written where it is, and found again by its name if it moved.
    - An effect may carry "orElse": other effects to use, in order, when its
      plugin is not on this computer -- Dragonfly's reverb, or Calf's where a
      Linux has no Dragonfly. One with no stand-in is left out, and said so. */
struct SoundFiles
{
    static constexpr const char* extension = ".stagerig-sound";

    struct Info
    {
        juce::File file;
        juce::String name, kind, about, library;
        /** The plugins it plays that this computer does not have, by name. */
        juce::StringArray needs;
    };

    /** Every ready sound on this computer, sorted by kind then name: in the
        folders instruments are looked for, and in My sounds. */
    static juce::Array<Info> installed (const juce::File& settingsFolder, PluginHost&);

    /** Where a player's own sounds are saved. */
    static juce::File mySoundsFolder (const juce::File& settingsFolder);

    /** Writes `program` as a sound; its kind is one of SfzSynth::categories(). */
    static bool write (const juce::File&, const ProgramDef& program, const juce::String& kind,
                       const juce::String& about, juce::String& error);

    /** Reads a sound into `program`, ready to put in place: stand-ins chosen,
        sampled instruments found. `left` lists what will not play here (an
        instrument whose plugin is missing, an effect left out), for the player. */
    static bool read (const juce::File&, const juce::File& settingsFolder, PluginHost&,
                      ProgramDef& program, juce::StringArray& left, juce::String& error);

    /** Whether the plugin a description names is on this computer. */
    static bool pluginExists (const juce::PluginDescription&, const juce::File& settingsFolder, PluginHost&);
};

} // namespace perf
