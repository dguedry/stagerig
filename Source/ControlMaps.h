#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "ParamInfo.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
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

/** A plugin maker's own knob pages, from its NKS presets (Native
    Instruments' standard for Komplete Kontrol: an .nksf file holds a preset's
    sound, its name and tags, and pages of eight knobs as the maker laid them
    out). Plugins said to be "NKS ready" come with them. Only the pages are
    used here, and the names to find which plugin they are for. */
struct NksPreset
{
    juce::String name, vendor;
    juce::String product;                  // what the maker calls the plugin: "Super 8 R2", "Spitfire Audio - BBC Symphony Orchestra"
    juce::String pluginName, pluginVendor; // the plugin's own names, which only some files carry
    /** The knobs name VST3 parameter ids; otherwise they are VST2 parameter
        numbers, which a plugin's VST3 version need not keep. */
    bool vst3Ids = false;
    /** Each knob's paramId is the file's id for it, its label the maker's
        name, and its paramName the maker's section for it ("Filter Env");
        a knob the maker left unassigned is empty. */
    std::vector<ControlPage> pages;

    static juce::Result read (const juce::File&, NksPreset& out);
    static juce::Result parse (const void* data, size_t size, NksPreset& out);
    /** How surely these are this plugin's: 0 not at all, 1 by a likely name
        ("Super 8 R2" for Super 8), 2 by the maker's full name for it, 3 by its own name. */
    int matches (const juce::PluginDescription&) const;
    /** Same knobs on the same pages, to tell presets with other pages apart. */
    juce::String pagesKey() const;
};

/** One of a plugin maker's sounds: an NKS preset file, by its name and where
    it sits in the maker's library (Native Instruments' Komplete Kontrol
    browses the same files). Read without the sound itself, which can be big;
    nksStateFor reads that when the sound is chosen. */
struct NksSound
{
    juce::File file;
    juce::String name, bank;               // bank: the maker's sub-banks, "Factory / Leads"
    juce::StringArray tags;                // the maker's types and characters: "Synth Lead", "Analog"
    juce::String vendor, product, pluginName, pluginVendor;
    /** JUCE's id for the VST3 the file names (PluginDescription::uniqueId), when it names one. */
    int vst3UniqueId = 0;
    bool hasVst3Id = false;

    static juce::Result read (const juce::File&, NksSound& out);
    /** How surely it is this plugin's sound: 0 not, 4 by the plugin's own id, else as NksPreset::matches. */
    int matches (const juce::PluginDescription&) const;
};

/** The state to give a plugin for an NKS sound, in the form JUCE's VST3 host
    takes (AudioProcessor::setStateInformation). `current` is the plugin's
    own state now, whose form says how its maker reads a sound: Native
    Instruments' plugins take the preset file itself; a VST3 preset (the
    maker's .vstpreset, inside) gives its two parts. Fails for a plugin not
    loaded as VST3, as NKS sounds are made for VST plugins. */
juce::Result nksStateFor (const juce::File&, const juce::MemoryBlock& current, juce::MemoryBlock& out);

/** The standard VST3 preset files (.vstpreset) made for this plugin: in the
    places VST3 keeps them, on Linux and inside the Wine prefixes Windows
    plugins run in ("VST3 Presets/<maker>/<plugin>"), each checked to name
    this very plugin. Reads the disk: not for the audio or message thread
    in a hurry. */
std::vector<juce::File> vst3PresetFilesFor (const juce::PluginDescription&);
/** A .vstpreset as the state JUCE's VST3 host takes. */
juce::Result vst3PresetState (const juce::File&, juce::MemoryBlock& out);

/** NKS pages as a setup for the plugin as it is loaded here. A knob is
    found by its id when the ids are the plugin's kind (VST3 and VST3);
    otherwise by its VST2 number when the parameter there has a name like the
    maker's, else by that name alone. A knob found nowhere is left out, so a
    knob never turns the wrong thing. */
ControlMap controlMapFromNks (const NksPreset&, const juce::PluginDescription&, const ParamInfoList&);

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
    /** Reloads when a file in the folder came, went or changed since the last look (a layout dropped in, or
        edited by hand, shows without a restart); true when it did. Cheap: one directory listing. */
    bool reloadIfChanged();

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

    /** Looks through these folders for NKS presets, in the background (it
        can take a minute among a big plugin collection). Until it is done,
        and for a plugin with none, findNks finds nothing. */
    void startNksSearch (const juce::Array<juce::File>& folders);
    /** Where plugins keep NKS presets on this computer: on Linux, inside the
        Wine prefixes the Windows plugins run in. */
    static juce::Array<juce::File> defaultNksFolders();
    /** The NKS pages for this plugin: those most of its presets have. Null
        when there are none or the search has not finished. */
    const NksPreset* findNks (const juce::PluginDescription&) const;
    /** The NKS pages of one sound's file (a slot's soundFile): the knobs of
        that very sound. Null when the file is gone or has none. Message thread. */
    const NksPreset* nksOfFile (const juce::String& path) const;
    /** Every NKS sound found for this plugin, by bank and name. Empty when
        there are none or the search has not finished. Any thread. */
    std::vector<NksSound> soundsFor (const juce::PluginDescription&) const;
    /** Goes up by one when the search finishes, so what was worked out
        without its pages can be worked out again. */
    int getNksGeneration() const                            { return nksGeneration.load(); }
    /** For tests: waits for the search, up to `ms`. */
    bool waitForNks (int ms) const;

    ~ControlMapLibrary();

private:
    struct Entry { juce::File file; ControlMap map; };
    juce::File folder;
    juce::String folderSignature;      // names and modification times, as last loaded
    std::vector<Entry> maps;

    mutable std::map<juce::String, NksPreset> nksFiles;    // read as asked for; one with no pages is kept empty
    std::vector<NksPreset> nks;            // written once, by the search, before nksGeneration goes up
    std::vector<NksSound> sounds;          // likewise
    std::atomic<int> nksGeneration { 0 };
    std::atomic<bool> nksStop { false };
    std::thread nksThread;
};

/** "C3" -> 60, "F#-1" -> 6, "61" -> 61; -1 for neither. Middle C is C3, as everywhere in StageRig. */
int parseNoteName (const juce::String&);

} // namespace perf
