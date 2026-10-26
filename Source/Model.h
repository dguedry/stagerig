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
    /** What makes the sound: a hosted plugin; a MIDI device -- a hardware
        synth, a module, a lighting controller -- played through a MIDI output
        port; the built-in General MIDI player; or another input's sounds
        (the notes go to that keyboard's current program). */
    enum class Kind { plugin, external, gm, input };
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
    /** Which incoming channels this slot listens to, one bit per channel
        (bit 0 = channel 1); all of them by default. With the input on Omni,
        one program can then give each channel its own sound -- a bass on 2
        and a pad on 1, from one keyboard or one MIDI file. Messages without
        a channel reach every slot. */
    static constexpr int kAllChannels = 0xffff;
    int listensTo = kAllChannels;
    bool hears (int channel) const    { return channel < 1 || channel > 16 || (listensTo & (1 << (channel - 1))) != 0; }
    bool hearsAll() const             { return (listensTo & kAllChannels) == kAllChannels; }
    void setHears (int channel, bool on)
    {
        if (channel < 1 || channel > 16) return;
        listensTo = on ? (listensTo | (1 << (channel - 1))) : (listensTo & ~(1 << (channel - 1)));
    }
    /** "All channels", "Channel 5", "Channels 1, 2, 5". */
    juce::String listensToText() const;

    // Another input's sounds ------------------------------------------------------
    int targetInput = -1;                          // Kind::input: index into Setup::inputs
    juce::String targetInputName;                  // for display, kept up to date on rename
    /** Which of that input's programs plays the notes: one of them by
        number, or -1 for whatever the input is playing at the time. A
        program named here is kept loaded and sounding alongside the
        keyboard's own, so a MIDI file's part can play "Upper: Strings"
        while Upper itself is on the piano. */
    int targetProgram = -1;
    juce::String targetProgramName;                // for display, kept up to date on rename

    // The General MIDI player ------------------------------------------------------
    /** Per channel, what the player's own mixer sets over what the file
        says: -1 = as the file has it. */
    struct GmChannel
    {
        int program = -1;             // the instrument (a kit, on channel 10)
        int volume = -1, pan = -1;    // CC 7, CC 10
        bool muted = false;
        bool isDefault() const { return program < 0 && volume < 0 && pan < 0 && ! muted; }
    };
    std::array<GmChannel, 16> gmChannels {};

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
    bool isGm() const                 { return kind == Kind::gm; }
    bool isInput() const              { return kind == Kind::input; }
    /** What to call it: the plugin's name, the port's, the player's, the keyboard's. */
    juce::String displayName() const
    {
        switch (kind)
        {
            case Kind::external: return midiOutName + " (MIDI)";
            case Kind::gm:       return "General MIDI player";
            case Kind::input:    return targetProgram >= 0 ? targetInputName + ": " + targetProgramName : targetInputName + "'s sounds";
            case Kind::plugin:   break;
        }
        return plugin.name;
    }

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
        from the MIDI device below. The backing track is an input too: the
        songs' MIDI files play into it, all channels, and each song selects
        its program there -- so a file's parts reach instruments exactly as
        a keyboard's notes do, by what each one listens to. There is at most
        one, made when the first MIDI file is added. */
    enum class Kind { midi, audio, track };
    Kind kind = Kind::midi;
    bool isAudio() const { return kind == Kind::audio; }
    bool isTrack() const { return kind == Kind::track; }
    static constexpr const char* trackName = "Backing track";
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

/** How MIDI files were routed before the backing track became an input:
    read from older setups, turned into a track program by
    Setup::migrateMidiFiles(), then dropped. */
struct LegacyMidiRoute
{
    enum class Kind { gm, keys, slot, mute };
    Kind kind = Kind::gm;
    int input = 0, program = -1, slot = -1;
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
    /** A MIDI file (.mid) plays into the backing-track input, every channel
        as it is in the file; the song's program there decides what each part
        plays. Nothing else to set: `output` and `gainDb` are an audio stem's. */

    // Older setups only; see LegacyMidiRoute.
    std::array<LegacyMidiRoute, 16> legacyRoutes {};
    bool hasLegacyRoutes = false;

    bool isMidi() const { return juce::File (file).hasFileExtension ("mid;midi;smf"); }
    juce::String displayName() const { return name.isNotEmpty() ? name : juce::File (file).getFileNameWithoutExtension(); }
    juce::var toVar() const;
    static StemDef fromVar (const juce::var&);
};

/** Something a section does the moment the track enters it: put a keyboard
    on a program, mute or unmute a part of the MIDI files, bypass or enable
    an effect, or send a MIDI message to a device -- a lighting desk's cue.
    The stage manager's cue sheet, kept with the song. */
struct ActionDef
{
    enum class Kind { program, part, effect, midi };
    Kind kind = Kind::program;
    int input = 0;                   // program, effect: which input
    int program = 0;                 // program: which of the input's programs
    int channel = 1;                 // part: the part's channel (1-16); midi: the channel
    bool on = true;                  // part: plays (not muted); effect: on (not bypassed)
    int chain = -1, effect = 0;      // effect: a ProgramDef chain address and an index in it
    juce::String portIdentifier, portName;   // midi: the device
    enum class MidiType { programChange, controller, noteOn };
    MidiType midiType = MidiType::programChange;
    int data1 = 0, data2 = 127;      // midi: the program, or the controller and its value, or the note and its velocity

    juce::var toVar() const;
    static ActionDef fromVar (const juce::var&);
};

/** A named point in a song's backing track, where a section starts: the
    intro, the verse, the chorus. A vamp section repeats until released --
    the band keeps the intro going until the singer is ready. */
struct MarkerDef
{
    juce::String name;
    double seconds = 0.0;
    bool vamp = false;
    std::vector<ActionDef> actions;

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

    /** Semitones the keyboards are shifted for this song -- down a step for
        the singer -- applied to their notes on the way in. 0 for most songs. */
    int transpose = 0;

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
    /** A click through the whole track, on `countInOutput`, at the song's
        tempo -- the metronome in the drummer's ears. Beat 1 of bar 1 falls
        `firstBeat` seconds into the files (0 unless the recording has a
        lead-in), and the click is accented every `beatsPerBar` beats. A
        song with no files and a click is a plain metronome. */
    bool click = false;
    int beatsPerBar = 4;
    double firstBeat = 0.0;
    /** Where Play from the top begins (seconds into the files), for a file
        with a lead-in you do not want to hear -- a quiet count-in, a breath.
        0 means at the first sound: silence at the head of a file is skipped
        anyway. Sections before it can still be gone to. */
    double startSeconds = 0.0;
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

    /** The backing-track input's index, or -1 while there is none. */
    int trackInput() const;
    /** Turns MIDI files routed the old way (through a keyboard, by channel,
        or by the short-lived per-channel table) into a program on the
        backing-track input, making that input if need be. Called on load. */
    void migrateMidiFiles();

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

    /** MIDI devices that are sent MIDI clock at the rig's tempo -- a drum
        machine, a hardware synth's arpeggiator, a pedal -- with Start and
        Stop when the backing track starts and stops. Empty for none. */
    struct MidiPortRef { juce::String identifier, name; };
    std::vector<MidiPortRef> clockOut;

    juce::var toVar() const;
    static Setup fromVar (const juce::var&);

    juce::Result saveToFile (const juce::File&) const;
    static juce::Result loadFromFile (const juce::File&, Setup& out);

    static Setup makeDefault();
};

} // namespace perf
