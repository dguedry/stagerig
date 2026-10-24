#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <vector>

namespace perf
{

//==============================================================================
/** A block in a chain: an effect plugin in an insert chain (per slot, or
    per program), or, in a slot's MIDI chain, a MIDI processor plugin or a
    MIDI send. */
struct EffectDef
{
    /** A plugin, or -- in a slot's MIDI chain only -- a send: the MIDI at
        that point also goes out of a port, to a hardware synth or another
        program, and carries on unchanged to the next block. */
    enum class Kind { plugin, midiSend };
    Kind kind = Kind::plugin;

    juce::PluginDescription plugin;
    juce::MemoryBlock state;
    bool bypassed = false;

    // A MIDI send ------------------------------------------------------------
    juce::String portIdentifier, portName;
    int channel = 0;                  // 0 = as it arrives, 1..16 = rechannelled for the port
    int bankMsb = -1, bankLsb = -1, patch = -1;   // patch recall, as an external instrument's

    bool isSend() const               { return kind == Kind::midiSend; }
    juce::String displayName() const  { return isSend() ? portName : plugin.name; }

    juce::var toVar() const;
    static EffectDef fromVar (const juce::var&);
};

//==============================================================================
/** One instrument inside a program, with its own insert-effect chain. */
struct SlotDef
{
    /** What makes the sound: a hosted plugin, or an external instrument -- a
        hardware synth or module played through a MIDI output port. */
    enum class Kind { plugin, external };
    Kind kind = Kind::plugin;

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
    /** Which incoming channel this slot listens to: 0 = all of what its input
        lets through, 1..16 = that channel only. With the input on Omni, one
        program can then give each channel its own sound -- a bass on 2 and a
        pad on 1, from one keyboard or one MIDI file. */
    int receiveChannel = 0;

    // An external instrument ----------------------------------------------------
    juce::String midiOutIdentifier, midiOutName;   // the port it plays through
    /** Patch recall: sent to the instrument when the program is selected,
        on the channel it plays on. -1 = not sent. */
    int bankMsb = -1, bankLsb = -1, patch = -1;
    /** Its audio, back into the mix: the interface inputs it is plugged into
        (open device input channels, from 0), or -1 for none -- then it goes
        to the desk on its own and only its notes come from here. A right of
        -1 with a left set is mono. */
    int returnLeft = -1, returnRight = -1;

    bool isExternal() const           { return kind == Kind::external; }
    bool isPlugin() const             { return kind == Kind::plugin; }
    /** What to call it: the plugin's name, or the port's. */
    juce::String displayName() const  { return isExternal() ? midiOutName + " (MIDI)" : plugin.name; }

    /** The slot as a chain, in signal order:
            [MIDI chain] -> sound -> [effects]
        The MIDI chain works on the notes after the slot's own key range,
        transpose and channels, before the sound hears them: MIDI processor
        plugins (an arpeggiator, a chord generator) and MIDI sends, in order.
        The effects work on the sound, and hear the MIDI the sound heard. */
    std::vector<EffectDef> midiEffects;
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

    /** Chains are addressed by a slot number: -1 is the program's own chain,
        a slot's index its effects, and midiChainOf (slot) that slot's MIDI
        chain. One number keeps every (slot, effect) address -- mappings, the
        engine's add/move/remove, the editors -- working for all three. */
    static constexpr int midiChainBase = 1 << 16;
    static int  midiChainOf (int slot)        { return midiChainBase + slot; }
    static bool isMidiChain (int address)     { return address >= midiChainBase; }
    /** The slot a chain address belongs to (-1 for the program's chain). */
    static int  slotOfChain (int address)     { return isMidiChain (address) ? address - midiChainBase : address; }

    /** Returns the chain a mapping/effect index refers to, or nullptr. */
    const std::vector<EffectDef>* chainFor (int slot) const
    {
        if (slot < 0) return &effects;
        const int s = slotOfChain (slot);
        if (s >= (int) slots.size()) return nullptr;
        return isMidiChain (slot) ? &slots[(size_t) s].midiEffects : &slots[(size_t) s].effects;
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

/** Where one channel of a backing-track MIDI file plays. */
struct MidiRoute
{
    enum class Kind
    {
        gm,       // the built-in General MIDI player, with the file's own instruments
        keys,     // through a keyboard's current program, as if played on it
        slot,     // straight into one plugin of a keyboard's program
        mute
    };
    Kind kind = Kind::gm;
    int input = 0;                // keys, slot: which keyboard
    int program = -1;             // slot: which of its programs; -1 = whatever it is on
    int slot = -1;                // slot: which plugin in it

    bool operator== (const MidiRoute& o) const { return kind == o.kind && input == o.input && program == o.program && slot == o.slot; }
    bool operator!= (const MidiRoute& o) const { return ! (*this == o); }
};

/** One audio file of a song's backing track: a stem (drums, bass, pads), a
    full mix, or a click and cue track for the in-ears. Kept in the folder
    beside the setup, like charts, so the two travel together. */
struct StemDef
{
    juce::String file;            // name of the file in the setup's folder
    juce::String name;            // shown in the list; the file name when empty
    int output = 0;               // which of the setup's outputs it plays on
    float gainDb = 0.0f;
    /** A MIDI file (.mid): each of its channels plays somewhere. By default
        the General MIDI player, with the instruments the file asks for, on
        `output` at `gainDb`; any channel can instead play through a
        keyboard's program, straight into one of its plugins (past its key
        range and LISTENS TO, on the channel that plugin listens on; a named
        program keeps playing even while the keys play another), or not at all. */
    std::array<MidiRoute, 16> channels {};
    const MidiRoute& route (int channel) const    { return channels[(size_t) juce::jlimit (1, 16, channel) - 1]; }
    MidiRoute& route (int channel)                { return channels[(size_t) juce::jlimit (1, 16, channel) - 1]; }
    bool usesGm() const                           { for (auto& r : channels) if (r.kind == MidiRoute::Kind::gm) return true; return false; }

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
