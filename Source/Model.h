#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

namespace perf
{

//==============================================================================
/** An effect plugin in an insert chain (per slot, or per program). */
struct EffectDef
{
    juce::PluginDescription plugin;
    juce::MemoryBlock state;
    bool bypassed = false;

    juce::var toVar() const;
    static EffectDef fromVar (const juce::var&);
};

//==============================================================================
/** One instrument inside a program, with its own insert-effect chain. */
struct SlotDef
{
    juce::PluginDescription plugin;   // which plugin
    juce::MemoryBlock state;          // last saved plugin state
    bool enabled = true;
    float gainDb = 0.0f;
    int transpose = 0;                // semitones
    int lowKey = 0, highKey = 127;    // key zone
    int lowVelocity = 1, highVelocity = 127;   // note-ons outside are dropped (velocity layers)
    float velocityCurve = 0.0f;       // -1..1: <0 softer, 0 linear, >0 louder for the same touch
    float pan = 0.0f;                 // -1 (left) .. 1 (right); a balance on the stereo output, unity at centre
    int outChannel = 0;               // 0 = keep incoming channel, 1..16 = force
    std::vector<EffectDef> effects;   // processed in order after the instrument

    juce::var toVar() const;
    static SlotDef fromVar (const juce::var&);
};

//==============================================================================
/** Maps a MIDI controller message to a plugin parameter. */
struct MappingDef
{
    enum class Source : int { CC = 0, PitchBend = 1, ChannelPressure = 2 };

    Source source = Source::CC;
    int number = 1;                   // CC number (ignored for other sources)
    int slot = 0;                     // index into ProgramDef::slots, or -1 for the program chain
    int effect = -1;                  // -1 = the instrument itself, else index into the effect chain
    juce::String paramId;             // HostedAudioProcessorParameter ID (or index as string)
    juce::String paramName;           // for display only
    float minValue = 0.0f, maxValue = 1.0f;
    bool passThrough = false;         // also forward the raw message to the plugins

    juce::String sourceDescription() const;

    juce::var toVar() const;
    static MappingDef fromVar (const juce::var&);
};

//==============================================================================
struct ProgramDef
{
    juce::String name;

    /** Free-text category: "Organs", "Strings", "Brass". Purely a label for
        finding things -- a program's number is fixed by MIDI Program Change, so
        a group can never move or renumber it. Empty means ungrouped.

        Per input, like everything else about a program: 007 on Upper has nothing
        to do with 007 on Lower, and neither do their groups. */
    juce::String group;
    std::vector<SlotDef> slots;
    std::vector<EffectDef> effects;   // program-level chain, applied to the summed slots
    float inputGainDb = 0.0f;         // audio inputs: trim on the signal before the chain
    std::vector<MappingDef> mappings;

    bool isEmpty() const { return slots.empty() && effects.empty(); }

    /** Returns the chain a mapping/effect index refers to, or nullptr. */
    const std::vector<EffectDef>* chainFor (int slot) const
    {
        if (slot < 0) return &effects;
        return slot < (int) slots.size() ? &slots[(size_t) slot].effects : nullptr;
    }

    juce::var toVar() const;
    static ProgramDef fromVar (const juce::var&);
};

//==============================================================================
/** An output: a pair of the device's channels, with a master chain and a
    volume on the way out. Every input lands on one. The default setup has a
    single "Main" on channels 1-2, which is the old behaviour exactly; a second
    one puts, say, the lower manual on 3-4 so the desk gets it on its own
    channel, or a click on the in-ear feed. Only the chain's effects are used
    of the ProgramDef inside -- it is a ProgramDef so the same chain code,
    addressed through Engine::outputAddress(), serves both. */
struct OutputDef
{
    juce::String name { "Main" };
    int left = 0, right = 1;          // device output channels, 0-based
    float gainDb = 0.0f;              // the master volume
    int volumeCC = 0;                 // controller that moves it, on any input and channel; 0 = none
    ProgramDef chain;

    bool isDefault() const { return name == "Main" && left == 0 && right == 1 && gainDb == 0.0f && volumeCC == 0 && chain.effects.empty(); }

    juce::var toVar() const;
    static OutputDef fromVar (const juce::var&);
};

//==============================================================================
/** A MIDI source such as "Upper" or "Lower" keyboard, or an audio input. */
struct InputDef
{
    static constexpr int numPrograms = 128;

    juce::String name { "Input" };

    /** What arrives on this input. A keyboard sends notes to the instruments
        in a program's slots. An audio input -- a microphone, a guitar, a
        hardware synth -- is the sound itself, so its programs are effect
        chains and nothing else. Both take program changes and controllers
        from the MIDI device below. */
    enum class Kind { midi, audio };
    Kind kind = Kind::midi;
    bool isAudio() const { return kind == Kind::audio; }
    /** An audio input's device channels, counted from 0 among the channels
        open in Audio settings. audioRight = -1 is mono: the one channel feeds
        both sides. */
    int audioLeft = 0, audioRight = 1;
    /** Which of the setup's outputs this input lands on. */
    int output = 0;

    juce::String midiDeviceIdentifier;
    juce::String midiDeviceName;      // used to re-find a device if identifiers change
    int channel = 0;                  // 0 = omni
    bool respondToProgramChange = true;
    /** Which channel carries Program Change for this input. Keyboards differ: most
        send it on the channel they play on, but a workstation may use a global
        channel (a Roland Jupiter-50 sends registrations on 16 while playing on
        1/3/4), and some send it on a channel you cannot change. -1 = the same
        channel as the notes (the default, and what every keyboard that does the
        usual thing wants), 0 = any channel on this input's MIDI port, 1..16 = that
        channel only. */
    static constexpr int pcChannelSameAsNotes = -1;
    static constexpr int pcChannelAny = 0;
    int programChangeChannel = pcChannelSameAsNotes;
    int currentProgram = 0;
    std::vector<ProgramDef> programs { (size_t) numPrograms };

    juce::var toVar() const;
    static InputDef fromVar (const juce::var&);
};

//==============================================================================
/** One song in a set: what every keyboard plays, and at what tempo.

    A program is a sound; a song is the moment the band starts playing. On
    stage you do not want to pick Upper 004 and Lower 001 and then set 116 bpm
    -- you want to say "Oye Como Va" and have the rig be ready.

    Programs are stored per input and numbered 0..127 each, so a song holds one
    number per input rather than a single program. -1 means "leave that
    keyboard alone", which is what you want for an input you are not using in
    this song, or a pad you keep set up between numbers. */
/** A chart for a song: a lead sheet, a scan, a typed chord chart. The file
    lives in the setup's charts folder (see chartsFolderFor) and is referred to
    by name only, so a setup and its folder can be moved together. */
struct ChartRef
{
    juce::String file;     // file name within the charts folder
    juce::String title;    // what the tablet calls it; the file name if empty

    juce::String displayName() const { return title.isNotEmpty() ? title : file; }

    juce::var toVar() const;
    static ChartRef fromVar (const juce::var&);
};

/** The sidecar folder for a setup file: MySetup.stagerig.json keeps its
    charts in MySetup.stagerig.charts/, beside it. */
juce::File chartsFolderFor (const juce::File& setupFile);

/** One audio file of a song's backing track: a stem (drums, bass, pads), a
    full mix, or a click and cue track for the in-ears. Kept in the folder
    beside the setup, like charts, so the two travel together. */
struct StemDef
{
    juce::String file;            // name of the file in the setup's folder
    juce::String name;            // shown in the list; the file name when empty
    int output = 0;               // which of the setup's outputs it plays on
    float gainDb = 0.0f;
    /** A MIDI file (.mid) plays through an input instead: into that input's
        current program, like the keys do. `midiChannel` keeps only one
        channel of the file (0 = all), so one file can feed Upper and Lower. */
    int input = 0;
    int midiChannel = 0;

    bool isMidi() const { return juce::File (file).hasFileExtension ("mid;midi;smf"); }
    juce::String displayName() const { return name.isNotEmpty() ? name : juce::File (file).getFileNameWithoutExtension(); }
    juce::var toVar() const;
    static StemDef fromVar (const juce::var&);
};

/** A named point in a song's backing track, where a section starts: the
    intro, the verse, the chorus. A vamp section repeats until released --
    the band keeps the intro going until the singer is ready. */
struct MarkerDef
{
    juce::String name;
    double seconds = 0.0;
    bool vamp = false;

    juce::var toVar() const;
    static MarkerDef fromVar (const juce::var&);
};

struct SongDef
{
    juce::String name;

    /** One program per input, by index into Setup::inputs. Shorter than the
        input list if a song predates an input being added; missing entries are
        treated as -1. */
    std::vector<int> programs;

    /** Tempo for this song, or 0 to keep whatever the tempo already is. Zero
        rather than a default, so a song that does not care about tempo does not
        silently reset one you tapped. */
    double tempoBpm = 0.0;

    /** Free text: a key, a count-in, "capo 2", whatever helps on stage. Shown
        on the tablet under the song name. */
    juce::String notes;

    /** Sheet music for the stand. Empty for most songs; the tablet shows a
        Chart button only when there is something to show. */
    std::vector<ChartRef> charts;

    /** The backing track: any number of stems, played together from the start
        when the transport is started, each on its own output. Empty for a
        song played entirely by hand. */
    std::vector<StemDef> stems;
    /** Clicks before the track starts, at the song's tempo; 0 for none. They
        play on `countInOutput`, which is usually the in-ear pair. */
    int countInBeats = 0;
    int countInOutput = 0;
    /** Sections of the backing track, by where each starts. */
    std::vector<MarkerDef> markers;

    int programFor (int inputIndex) const
    {
        return inputIndex >= 0 && inputIndex < (int) programs.size()
                 ? programs[(size_t) inputIndex] : -1;
    }

    juce::var toVar() const;
    static SongDef fromVar (const juce::var&);
};

//==============================================================================
/** An ordered list of songs: one gig, one rehearsal, one support slot.

    Sets hold song indices rather than copies, so the same song can appear in
    several sets and fixing its tempo once fixes it everywhere. */
struct SetDef
{
    juce::String name;
    std::vector<int> songs;          // indices into Setup::songs

    juce::var toVar() const;
    static SetDef fromVar (const juce::var&);
};

//==============================================================================
struct Setup
{
    std::vector<InputDef> inputs;
    /** Where the sound goes: at least one, "Main" on channels 1-2 by default. */
    std::vector<OutputDef> outputs = std::vector<OutputDef> (1);

    /** Every song this setup knows, in no particular order: the sets decide
        the order they are played in. */
    std::vector<SongDef> songs;
    std::vector<SetDef> sets;

    /** Which set is being played, and how far into it, so picking up where you
        left off survives a restart. -1 = no set chosen. */
    int currentSet = -1;
    int currentSongInSet = -1;

    /** Controller that steps to the next song, and to the previous one, or 0
        for none. Global like the tap tempo controller, and for the same reason:
        a footswitch has to work whatever is loaded. */
    int nextSongCC = 0;
    int prevSongCC = 0;
    /** Controller that starts and stops the song's backing track; 0 for none.
        Global, like the song footswitches. */
    int transportCC = 0;
    /** Controller that releases a vamp, or else queues the next section. */
    int sectionCC = 0;

    bool preloadAllPrograms = false;  // keep every used program's plugins resident
    double releaseTailSeconds = 4.0;  // how long the outgoing program keeps sounding
    bool holdAcrossChange = true;     // a program change leaves held notes and the pedal to the player

    /** Tempo handed to every plugin, for tempo-synced delays and arpeggiators.
        One tempo for the whole rig rather than one per program: a band plays a
        song at a tempo, not an instrument. */
    double tempoBpm = 120.0;

    /** Controller number that taps the tempo, or 0 for none. Global for the same
        reason the tempo is: a footswitch should work whatever is loaded. */
    int tapTempoCC = 0;

    juce::var toVar() const;
    static Setup fromVar (const juce::var&);

    juce::Result saveToFile (const juce::File&) const;
    static juce::Result loadFromFile (const juce::File&, Setup& out);

    static Setup makeDefault();
};

} // namespace perf
