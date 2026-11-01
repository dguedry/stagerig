#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

namespace perf
{

/** One knob of a controller setup: which of the plugin's parameters it
    turns, and what the keyboard's screen calls it. The parameter is found by
    its id first and by its name after, so a setup made on one computer still
    works on another where the plugin numbers things differently. */
struct ControlKnob
{
    juce::String paramId;      // the plugin's parameter id
    juce::String paramName;    // the plugin's name for it, to find it when the id does not
    juce::String label;        // what the screen shows; the parameter's name when empty

    bool isEmpty() const       { return paramId.isEmpty() && paramName.isEmpty(); }
};

/** Eight knobs under a name: "Sound", "Effects", "Mic positions". */
struct ControlPage
{
    static constexpr int numKnobs = 8;
    juce::String name;
    std::vector<ControlKnob> knobs;    // up to eight; missing ones do nothing
};

/** A run of keys that means something: keyswitches, a drum kit's pads, the
    range a sampled instrument really has. Lit in its colour on a keyboard
    with lights, and named on its screen. Notes are as the plugin receives
    them, so a setup stays right whatever the instrument is transposed by. */
struct KeyZone
{
    int low = 0, high = 127;
    juce::uint32 colour = 0xff8000;    // 0xRRGGBB
    juce::String label;
};

/** What a keyboard's knobs, screen and lights do for one plugin, or for one
    instrument inside a plugin that holds many (a Kontakt instrument): pages
    of named knobs, and coloured runs of keys. A setup is a small file, made
    once and shared: whoever has the same plugin can import it.

    The file is JSON:
        { "format": "stagerig-controller-setup", "version": 1,
          "name": "Alicia's Keys", "plugin": "Kontakt 8", "manufacturer": "Native Instruments",
          "author": "...", "notes": "...",
          "pages": [ { "name": "Sound", "knobs": [ { "id": "...", "param": "Reverb", "label": "Room" } ] } ],
          "keys":  [ { "low": 21, "high": 32, "colour": "#ff8000", "label": "Articulations" } ] } */
struct ControlMap
{
    static constexpr const char* formatTag = "stagerig-controller-setup";
    static constexpr int formatVersion = 1;
    static constexpr const char* fileExtension = ".stagerig-controls";

    juce::String name;                 // "Alicia's Keys"; the plugin's own name for a setup of the whole plugin
    juce::String pluginName, pluginManufacturer;
    juce::String author, notes;
    std::vector<ControlPage> pages;
    std::vector<KeyZone> keys;

    /** Made for this plugin: the same name, and the same maker when both say. */
    bool appliesTo (const juce::PluginDescription&) const;
    /** Made for this plugin and called this. */
    bool isSameAs (const ControlMap& other) const;

    juce::var toVar() const;
    /** False, with a reason, for something that is not a controller setup. */
    static bool fromVar (const juce::var&, ControlMap& out, juce::String& error);

    juce::Result saveToFile (const juce::File&) const;
    static juce::Result loadFromFile (const juce::File&, ControlMap& out);

    static juce::String colourToText (juce::uint32);
    static juce::uint32 colourFromText (const juce::String&, juce::uint32 fallback);
};

/** Every controller setup on this computer: the files in one folder, beside
    the settings. The folder is the library -- a file dropped in is a setup,
    and Import does no more than copy one there. */
class ControlMapLibrary
{
public:
    explicit ControlMapLibrary (const juce::File& folder);

    const juce::File& getFolder() const                     { return folder; }
    /** Reads the folder again. Files that are not setups are passed over. */
    void reload();

    int size() const                                        { return (int) maps.size(); }
    const ControlMap& get (int i) const                     { return maps[(size_t) i].map; }
    const juce::File& fileOf (int i) const                  { return maps[(size_t) i].file; }

    /** The setups made for this plugin, by name. */
    std::vector<const ControlMap*> forPlugin (const juce::PluginDescription&) const;
    /** The one called `name` made for this plugin; with no name, the first for it. Null if none. */
    const ControlMap* find (const juce::PluginDescription&, const juce::String& name) const;

    /** Writes a setup into the folder, replacing the one of the same plugin and name. */
    juce::Result save (const ControlMap&);
    juce::Result remove (const ControlMap&);
    /** Copies a setup file someone shared into the folder. `imported`, when
        given, receives it. One of the same plugin and name is replaced. */
    juce::Result importFile (const juce::File&, ControlMap* imported = nullptr);

    /** The file a setup is kept in: "<plugin> - <name>.stagerig-controls". */
    juce::File fileFor (const ControlMap&) const;

private:
    struct Entry { juce::File file; ControlMap map; };
    juce::File folder;
    std::vector<Entry> maps;
};

/** "C3" -> 60, "F#-1" -> 6, "61" -> 61; -1 for neither. Middle C is C3, as everywhere in StageRig. */
int parseNoteName (const juce::String&);

} // namespace perf
