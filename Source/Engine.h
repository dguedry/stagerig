#pragma once

#include "Model.h"
#include "PluginHost.h"
#include "RemotePlugin.h"
#include "SongPlayer.h"
#include "MidiOuts.h"
#include "Recorder.h"
#include "MidiFx.h"
#include "GmSynth.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <set>
#include <pthread.h>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace perf
{

/**
    The audio/MIDI engine.

    Owns the Setup document plus the live plugin instances for every loaded program.
    All public methods must be called from the message thread. Audio and MIDI
    callbacks run on their own threads and touch only the runtime structures,
    guarded by a single lock that is held for the duration of each audio block.

    Signal flow per program:
        for each slot:   MIDI -> [MIDI chain] -> sound -> [effect chain] -> * gain  --+
                                                                                       +-> sum -> [program effect chain] -> out
    A slot's sound is a plugin or an external instrument (a MIDI port, its
    audio back on interface inputs). Its MIDI chain holds
    MIDI processor plugins and MIDI sends.

    Plugins are addressed by (slot, effect): slot >= 0 selects a slot's effect
    chain, ProgramDef::midiChainOf (slot) its MIDI chain, slot == -1 the
    program-level chain; effect == -1 means the slot's sound itself.
*/
class Engine : private juce::AudioIODeviceCallback,
               private juce::MidiInputCallback,
               private juce::AsyncUpdater,
               private juce::Timer,
               private juce::ChangeListener
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        /** Inputs added/removed or a whole setup loaded. */
        virtual void setupChanged() {}
        /** An output was added, removed or changed, or a master chain's plugin loaded or died. */
        virtual void outputsChanged() {}
        virtual void outputVolumeLearned (int output, int cc) { juce::ignoreUnused (output, cc); }
        /** The active program of an input changed (via MIDI or UI). */
        virtual void programChanged (int inputIndex, int program) { juce::ignoreUnused (inputIndex, program); }
        /** Slots, effects or mappings of a program were edited, or its load state changed. */
        virtual void programContentChanged (int inputIndex, int program) { juce::ignoreUnused (inputIndex, program); }
        /** A MIDI controller arrived while learn mode was armed. */
        virtual void learnReceived (int inputIndex, MappingDef::Source source, int number) { juce::ignoreUnused (inputIndex, source, number); }
        /** A controller was pressed while waiting to learn the tap controller. */
        virtual void tapTempoLearned (int cc) { juce::ignoreUnused (cc); }
        /** The user touched a parameter in a plugin GUI (effect == -1 for an instrument). */
        virtual void parameterTouched (int inputIndex, int program, int slot, int effect, int paramIndex) { juce::ignoreUnused (inputIndex, program, slot, effect, paramIndex); }
        virtual void statusMessage (const juce::String&) {}
        /** The backing track was armed, started, stopped or came to its end. */
        virtual void transportChanged() {}
    };

    /** @param startAudioDevice  pass false for headless tests; blocks are then rendered
                                 manually with renderBlockForTesting(). */
    Engine (PluginHost&, juce::PropertiesFile& settings, bool startAudioDevice = true);
    ~Engine() override;

    juce::AudioDeviceManager& getDeviceManager()    { return deviceManager; }
    PluginHost& getPluginHost()                     { return host; }

    /** Closes and reopens the current audio device (picks up a new PIPEWIRE_LATENCY). */
    void restartAudioDevice();

    //==============================================================================
    // Setup document
    const Setup& getSetup() const                   { return setup; }
    void loadSetup (Setup newSetup);
    /** Pulls the live plugin states into the model and returns a snapshot for saving. */
    /** `budgetMs` >= 0 bounds the whole capture: hosts that have stopped
        answering share one deadline instead of each costing a full wait. The
        model keeps whatever it last had for a plugin that does not answer. */
    /** `onlyChanged` asks only the plugins whose state may have changed since
        it was last read (RemotePlugin::stateMayHaveChanged): the autosave's
        way, because asking nineteen plugins at once stalls the ones that
        lock their audio to serialise -- a few seconds of static every
        minute. A save or a quit asks everything. */
    Setup captureSetup (int budgetMs = -1, bool onlyChanged = false);
    void setPreloadAllPrograms (bool);
    void setReleaseTailSeconds (double);
    /** Whether a program change leaves the outgoing program's held notes and
        sustain pedal to the player (it keeps sounding, taking note-offs and
        pedals but no new notes, and its tail starts when the last is let go),
        or releases everything at the change and lets the tail cover it. */
    void setHoldAcrossChange (bool);

    // Tempo ----------------------------------------------------------------------
    /** One tempo for the whole rig, handed to every plugin so tempo-synced
        delays and arpeggiators have something to sync to. */
    double getTempoBpm() const                      { return setup.tempoBpm; }
    void setTempoBpm (double bpm);

    /** Registers one tap and returns the tempo it implies, or 0 while there are
        not yet enough taps to be sure. Sets the tempo itself once it is. */
    double tapTempo();

    /** Forgets the taps so far, so the next tap starts a fresh count. */
    void resetTapTempo();

    /** Announces a parameter change as a plugin's own callback would, so the
        path from "a plugin moved something" to "the tablet notices" can be
        tested without a plugin. */
    void notifyParameterTouchedForTesting (int inputIndex, int program, int slot, int effect, int paramIndex)
    { noteParameterTouched (inputIndex, program, slot, effect, paramIndex); }

    /** A controller that taps the tempo, on any input and in any program: tempo
        belongs to the rig, not to one sound. 0 = nothing assigned. Channel 0
        means any channel, so a footswitch works wherever it is plugged in. */
    int getTapTempoCC() const                       { return setup.tapTempoCC; }

    /** MIDI clock out: 24 ticks a beat at the rig's tempo to these devices,
        with Start when the backing track starts and Stop when it stops, so
        a drum machine or an arpeggiator on a hardware synth keeps time with
        the rig. Message thread. */
    const std::vector<Setup::MidiPortRef>& getClockOut() const { return setup.clockOut; }
    bool sendsClockTo (const juce::String& identifier) const;
    void setClockTo (const juce::MidiDeviceInfo&, bool on);
    /** Ticks produced since the engine started, for tests. */
    int  getClockTicksForTesting() const            { return clockTicks.load(); }

    /** Recording the gig: every output to a WAV as it leaves for the desk,
        and what the keyboards sent to a MIDI file, in a dated folder beside
        the setup. Message thread. */
    bool startRecording (juce::String& error);
    void stopRecording();
    bool isRecording() const                        { return recorder.isRecording(); }
    double getRecordingSeconds() const              { return recorder.elapsedSeconds(); }
    juce::File getRecordingFolder() const           { return recorder.getFolder(); }
    /** Where takes go: "<setup>.recordings/" beside the setup file. */
    juce::File getRecordingsFolder() const;
    void setTapTempoCC (int cc);

    /** Waits for the next controller and makes that the tap controller, so you
        can press the pedal rather than look up what it sends. The listener's
        tapTempoLearned() fires when one arrives. */
    void armTapTempoLearn (bool);
    bool isTapTempoLearnArmed() const               { return tapLearnArmed.load(); }

    //==============================================================================
    // Inputs
    int  addInput (const juce::String& name, InputDef::Kind kind = InputDef::Kind::midi);
    void removeInput (int inputIndex);
    /** Which of the open device input channels an audio input listens to; right = -1 for mono. */
    void setInputAudioChannels (int inputIndex, int left, int right);
    /** An audio input program's trim, applied to the signal before its chain. */
    void setProgramInputGainDb (int inputIndex, int program, float dB);
    /** The device's open input channels, in the order an audio input counts
        them. Empty when no input device is open. */
    juce::StringArray getAudioInputChannelNames() const;
    //==============================================================================
    // Outputs: device channel pairs, each with a master chain and a volume.
    /** A master chain is addressed like a program's chain -- addEffect,
        getPlugin, openPluginEditor and the rest take (input, program) -- with
        this in place of the input index and 0 as the program. Negative, so it
        can never be mistaken for an input. */
    static int  outputAddress (int output)       { return -1 - output; }
    static bool isOutputAddress (int input)      { return input < 0; }
    static int  outputFromAddress (int input)    { return -1 - input; }

    int  addOutput (const juce::String& name, int left, int right);
    /** The last output cannot be removed; inputs on a removed one land on the first. */
    void removeOutput (int output);
    void setOutputName (int output, const juce::String&);
    void setOutputChannels (int output, int left, int right);
    void setOutputGainDb (int output, float dB);
    /** A controller, on any input and channel, that moves this output's volume; 0 = none. */
    void setOutputVolumeCC (int output, int cc);
    /** The next controller to arrive becomes that output's volume control. -1 cancels. */
    void armOutputVolumeLearn (int output);
    bool isOutputVolumeLearnArmed() const        { return outputLearn.load() >= 0; }
    void setInputOutput (int inputIndex, int output);
    /** The device's open output channels, in callback order. */
    juce::StringArray getAudioOutputChannelNames() const;
    /** Opens at least this many device output channels, best effort. */
    bool ensureAudioOutputsOpen (int channelsNeeded);

    /** What is wrong with the device setup, if anything, in plain words: a
        buffer too small for a hardware interface, input and output on
        different devices. Empty when all is well. */
    juce::StringArray deviceWarnings() const;

    /** An audio input's level before its trim: the peak on each side, falling
        at 20 dB a second, and whether it clipped within the last two seconds.
        Zero for a keyboard input. Read from any thread. */
    struct InputLevel { float left = 0.0f, right = 0.0f; bool clipped = false; };
    InputLevel getInputLevel (int inputIndex) const;
    /** Opens the device's inputs if none are, best effort: the output device's
        own inputs, with its default channels. False when that cannot be done,
        in which case Audio settings is where to choose one. */
    bool ensureAudioInputsOpen();
    void setInputName (int inputIndex, const juce::String&);
    void setInputMidiDevice (int inputIndex, const juce::MidiDeviceInfo&);
    void setInputProgramChangeChannel (int inputIndex, int channel);
    void setInputChannel (int inputIndex, int channel);
    void setInputRespondToProgramChange (int inputIndex, bool);
    /** Re-scans MIDI devices and re-opens the ones the setup refers to (hot-plug). */
    void refreshMidiDevices();

    //==============================================================================
    // Programs
    void selectProgram (int inputIndex, int program);

    //==============================================================================
    // Songs and sets
    //
    // A program is a sound; a song is the moment the band starts playing. One
    // song sets every keyboard at once and brings its own tempo, because that
    // is what "the next number" means on stage.

    const std::vector<SongDef>& getSongs() const { return setup.songs; }
    const std::vector<SetDef>&  getSets()  const { return setup.sets; }

    int addSong (const juce::String& name);            // returns its index
    void removeSong (int songIndex);

    /** Moves a song within the master list. Every set holds indices into that
        list, so they are rewritten to follow: reordering songs must never
        change what a set plays. Returns where it ended up. */
    int moveSong (int songIndex, int delta);
    void setSongName (int songIndex, const juce::String&);
    void setSongNotes (int songIndex, const juce::String&);
    void setSongTempo (int songIndex, double bpm);     // 0 = leave the tempo alone
    void setSongTranspose (int songIndex, int semitones);
    /** The keyboards' notes shifted by this many semitones on the way in: the
        song's setting, or a change made on the night. The backing track and
        the audio inputs are not shifted. A note held across a change is let
        go at the key it started on. */
    void setTranspose (int semitones);
    int  getTranspose() const { return liveTranspose.load(); }
    /** The song in play -- the set's current one -- or -1. */
    int  currentSongIndex() const;
    void setSongProgram (int songIndex, int inputIndex, int program);   // -1 = leave that input alone

    /** Fills a song from what is loaded right now: every input's current
        program, and the current tempo. Building a set is then "get the sound
        right, press Capture". */
    void captureSong (int songIndex);
    void addSongChart (int songIndex, const ChartRef&);
    void removeSongChart (int songIndex, int chartIndex);

    /** The file the setup was loaded from or last saved to, or none. The
        charts folder sits beside it, so both the desktop and the tablet
        server derive the same place from here. */
    /** Also makes the current song's track ready again: its files live beside this file. */
    void setSetupFile (const juce::File& f);
    juce::File getSetupFile() const          { return setupFile; }
    juce::File getChartsFolder() const       { return chartsFolderFor (setupFile); }

    /** Selects every input's program for this song and applies its tempo.
        Inputs the song leaves at -1 are not touched. */
    void selectSong (int songIndex);

    int addSet (const juce::String& name);
    void removeSet (int setIndex);

    /** Moves a set within the list. Returns its new index. */
    int moveSet (int setIndex, int delta);
    void setSetName (int setIndex, const juce::String&);
    void setSetSongs (int setIndex, const std::vector<int>& songIndices);

    /** Which set is being played and how far into it. -1 = none. */
    int getCurrentSet() const        { return setup.currentSet; }
    int getCurrentSongInSet() const  { return setup.currentSongInSet; }
    void selectSet (int setIndex);
    void selectSongInSet (int position);
    void nextSong();
    void previousSong();

    /** Footswitches that step through the set. 0 = none. */
    int getNextSongCC() const { return setup.nextSongCC; }
    int getPrevSongCC() const { return setup.prevSongCC; }
    void setNextSongCC (int cc);
    void setPrevSongCC (int cc);
    void armSongLearn (bool forNext);
    bool isSongLearnArmed() const { return songLearnArmed.load(); }

    //==============================================================================
    // Backing tracks: a song's stems, played from the folder beside the setup.
    void addSongStem (int songIndex, const StemDef&);
    void removeSongStem (int songIndex, int stemIndex);
    void setSongStem (int songIndex, int stemIndex, const StemDef&);
    void setSongCountIn (int songIndex, int beats, int output);
    void setSongMarkers (int songIndex, const std::vector<MarkerDef>&);
    /** The click through the song, and its accent; takes effect at once if the song is the one armed. */
    void setSongClick (int songIndex, bool on, int beatsPerBar);
    /** Where beat 1 of bar 1 falls in the song's files (seconds): the click and the count-in line up with it. */
    void setSongFirstBeat (int songIndex, double seconds);
    /** Where Play from the top begins in the song's files (seconds); 0 = the first sound. */
    void setSongStart (int songIndex, double seconds);
    /** The tempo the song's audio files were made at (0 = unknown). With the
        song's tempo set to something else, the files play stretched to it. */
    void setSongTrackBpm (int songIndex, double bpm);
    /** Whether the song's transpose also moves its files' pitch. */
    void setSongTransposeTrack (int songIndex, bool on);
    /** How the armed track is playing against its files: 1 = as they are. */
    double getTrackTimeScale() const                { return player != nullptr ? player->getTimeScale() : 1.0; }
    int    getTrackPitchSemitones() const           { return player != nullptr ? player->getPitchSemitones() : 0; }
    /** Where the armed track will start from while stopped (seconds). */
    double getTrackCue() const                      { return player != nullptr ? player->getCueSeconds() : 0.0; }

    /** The song whose track is ready to play: the one selected last, or -1. */
    int  getTrackSong() const                       { return trackSong; }
    bool hasTrack() const                           { return player != nullptr && (player->hasStems() || player->hasClick()); }
    /** False while the stems are still being read into memory. */
    bool isTrackReady() const                       { return player != nullptr && player->isReady(); }
    float getTrackLoadProgress() const              { return player != nullptr ? player->getLoadProgress() : 0.0f; }
    bool isTrackRunning() const                     { return player != nullptr && player->isRunning(); }
    bool isTrackCountingIn() const                  { return player != nullptr && player->getState() == SongPlayer::State::countIn; }
    double getTrackPosition() const                 { return player != nullptr ? player->getPositionSeconds() : 0.0; }
    double getTrackLength() const                   { return player != nullptr ? player->getLengthSeconds() : 0.0; }
    /** Starts the armed song's track from the top, after its count-in. */
    void startTrack();
    void stopTrack();
    void toggleTrack();

    /** Sections of the armed track. A section asked for while playing comes
        when the current one ends; while stopped, it is where Play starts. */
    int  getTrackSectionCount() const               { return player != nullptr ? player->getNumSections() : 0; }
    juce::String getTrackSectionName (int i) const  { return player != nullptr ? player->getSectionName (i) : juce::String(); }
    double getTrackSectionStart (int i) const       { return player != nullptr ? player->getSectionStart (i) : 0.0; }
    bool isTrackSectionVamp (int i) const           { return player != nullptr && player->isSectionVamp (i); }
    int  getTrackSectionMarker (int i) const        { return player != nullptr ? player->getSectionMarker (i) : -1; }
    /** Does what one action says, now: the section's cue sheet, one line. Also for trying one out from the window. */
    void performAction (int songIndex, const ActionDef&);
    /** A line of words for an action, for the window and the tablet. */
    juce::String describeAction (const ActionDef&) const;
    /** Message thread: the MIDI output ports StageRig can send to, for choosing one. */
    juce::Array<juce::MidiDeviceInfo> midiOutputDevices() const;
    void sendMidiToDevice (const juce::String& identifier, const juce::String& name, const juce::MidiMessage&);
    int  getTrackSection() const                    { return player != nullptr ? player->getCurrentSection() : -1; }
    int  getQueuedTrackSection() const              { return player != nullptr ? player->getQueuedSection() : -1; }
    bool isTrackVamping() const                     { return player != nullptr && player->isVamping(); }
    void queueTrackSection (int index);
    void releaseVamp();
    /** One pedal: releases a vamp, or else queues the next section. */
    void advanceTrack();

    int  getSectionCC() const                       { return setup.sectionCC; }
    void setSectionCC (int cc);
    void armSectionLearn()                          { sectionLearnArmed.store (true); }

    /** Looping a passage on a keyboard: record what you play, and it goes
        round in whole bars at the rig's tempo while you play over it. One
        pedal does it all -- press to record, press to play, press to stop,
        press to play again -- and a second clears. The pedal works on the
        keyboard it is pressed on; the button on the panel on the selected one. */
    enum class LoopState { empty, recording, playing, stopped };
    LoopState getLoopState (int inputIndex) const;
    int  getLoopBars (int inputIndex) const;
    void loopPedal (int inputIndex);
    void loopClear (int inputIndex);
    void loopStop (int inputIndex);
    int  getLooperCC() const                        { return setup.looperCC; }
    int  getLooperClearCC() const                   { return setup.looperClearCC; }
    void setLooperCC (int cc);
    void setLooperClearCC (int cc);
    void armLooperLearn (bool clearPedal)           { (clearPedal ? looperClearLearnArmed : looperLearnArmed).store (true); }
    /** The backing-track input: where the songs' MIDI files play. -1 while none. */
    int  getTrackInput() const                      { return setup.trackInput(); }
    /** Makes the backing-track input if there is none, and returns its index. */
    int  ensureTrackInput();
    /** The song's program on the backing-track input, made (named after the
        song, with the General MIDI player) if the song has none yet. */
    int  ensureTrackProgram (int songIndex);
    /** The one instrument of `program` that plays channel `channel`: it
        listens to it (alone, if it was hearing every channel) and every
        other one stops. -1 = none plays it. */
    void assignPart (int inputIndex, int program, int channel, int slot);
    /** Which instrument of `program` plays `channel`, or -1 (the first found). */
    int  partInstrument (int inputIndex, int program, int channel) const;
    /** A part's mute: the GM player's channel mute when it plays the part, else the instrument's enable. */
    void setPartMuted (int inputIndex, int program, int channel, bool muted);
    bool isPartMuted (int inputIndex, int program, int channel) const;

    /** While stopped: the armed track starts from here (seconds) on the next Play. */
    void cueTrack (double seconds)                  { if (player != nullptr) player->cue (seconds); }
    /** Reads a song's files again (after the MIDI editor saved one), if it is the armed song. */
    void reloadTrack (int songIndex)                { if (songIndex == trackSong) armTrack (songIndex); }
    /** Loads (or reloads) the track for this song, stopping whatever was playing. */
    void armTrack (int songIndex);

    /** The General MIDI soundfont MIDI stems play through; empty = the system's
        (FluidR3_GM). Kept in the settings, as it belongs to the machine. */
    juce::File getGmSoundfont() const;
    void setGmSoundfont (const juce::File&);

    int  getTransportCC() const                     { return setup.transportCC; }
    void setTransportCC (int cc);
    void armTransportLearn()                        { transportLearnArmed.store (true); }
    bool isTransportLearnArmed() const              { return transportLearnArmed.load(); }
    void setProgramName (int inputIndex, int program, const juce::String&);

    /** The program's category ("Organs", "Strings"), or empty for ungrouped. */
    void setProgramGroup (int inputIndex, int program, const juce::String&);

    /** Every group name in use on this input, in the order the programs appear,
        so a picker can offer what is already there rather than inviting
        "Organ", "organs" and "Organ " as three separate groups. */
    juce::StringArray getProgramGroups (int inputIndex) const;
    bool isProgramLoaded (int inputIndex, int program) const;
    void copyProgram (int inputIndex, int from, int to);
    void clearProgram (int inputIndex, int program);

    //==============================================================================
    // Slots (instruments)
    bool addSlot (int inputIndex, int program, const juce::PluginDescription&, juce::String& error);
    void removeSlot (int inputIndex, int program, int slot);
    void setSlotEnabled   (int inputIndex, int program, int slot, bool);
    void setSlotGainDb    (int inputIndex, int program, int slot, float);
    void setSlotTranspose (int inputIndex, int program, int slot, int);
    void setSlotKeyRange  (int inputIndex, int program, int slot, int low, int high);
    void setSlotVelocityRange (int inputIndex, int program, int slot, int low, int high);
    void setSlotVelocityCurve (int inputIndex, int program, int slot, float curve);   // -1..1
    void setSlotPan       (int inputIndex, int program, int slot, float pan);         // -1..1
    /** Velocity after a slot's curve: 0 leaves it, >0 lifts soft playing, <0 tames it. 1..127 in and out. */
    static int curveVelocity (int velocity, float curve);
    /** Left/right gains for a pan position: a balance law, unity in the centre. */
    static void panGains (float pan, float& left, float& right);
    void setSlotOutChannel(int inputIndex, int program, int slot, int);
    /** Which incoming channels the slot listens to: a mask, bit 0 = channel 1 (SlotDef::listensTo). */
    void setSlotListensTo (int inputIndex, int program, int slot, int channelMask);
    void setSlotHears (int inputIndex, int program, int slot, int channel, bool hears);

    // Other kinds of instrument -------------------------------------------------
    /** The built-in General MIDI player: a multitimbral instrument, the
        file's (or its own mixer's) instrument on each channel. */
    bool addGmSlot (int inputIndex, int program);
    /** The GM player's mixer for one channel: instrument, volume, pan, mute over the file's. */
    void setGmChannel (int inputIndex, int program, int slot, int channel, const SlotDef::GmChannel&);
    /** Another input's sounds: the slot's notes go to that input's current program. */
    /** An instrument that is another input's sounds: its program number
        `targetProgram`, or whatever that input is playing (-1). */
    bool addInputSlot (int inputIndex, int program, int targetInput, int targetProgram = -1);

    // External instruments: a slot played through a MIDI output port ------------
    bool addExternalSlot (int inputIndex, int program, const juce::MidiDeviceInfo&);
    void setSlotMidiOut (int inputIndex, int program, int slot, const juce::MidiDeviceInfo&);
    /** Bank select and program change sent when the program is selected (and now, if it is); -1 = not sent. */
    void setSlotPatch (int inputIndex, int program, int slot, int bankMsb, int bankLsb, int patch);
    /** The interface inputs the instrument's audio comes back on; -1 = none. */
    void setSlotReturn (int inputIndex, int program, int slot, int left, int right);
    /** Is the slot's MIDI port there? False for a plugin slot. */
    bool isSlotPortOpen (int inputIndex, int program, int slot) const;


    // A slot's MIDI chain: what its notes go through before the sound --------
    //
    // Addressed as any chain, with ProgramDef::midiChainOf (slot) for the slot:
    // addEffect (in, prog, midiChainOf (slot), plugin) puts a MIDI processor
    // there, and remove/move/bypass, getPlugin and the editors work as for an
    // effect. A send has functions of its own:
    bool addMidiSend (int inputIndex, int program, int slot, const juce::MidiDeviceInfo&);
    void setMidiSendPort (int inputIndex, int program, int slot, int effect, const juce::MidiDeviceInfo&);
    /** 0 = as the MIDI arrives at the send, 1..16 = rechannelled for its port. */
    void setMidiSendChannel (int inputIndex, int program, int slot, int effect, int channel);
    /** A built-in MIDI effect (arpeggiator, chord, scale) at the end of a slot's MIDI chain. */
    bool addBuiltInMidiEffect (int inputIndex, int program, int slot, EffectDef::Kind kind);
    /** Its settings, from `def`'s arp/chord/scale fields; takes effect at once. */
    void setBuiltInMidiEffect (int inputIndex, int program, int slot, int effect, const EffectDef& def);
    /** Sent from the port when the program is selected, as an external instrument's patch; -1 = not sent. */
    void setMidiSendPatch (int inputIndex, int program, int slot, int effect, int bankMsb, int bankLsb, int patch);


    //==============================================================================
    // Effects. slot == -1 addresses the program-level chain.
    bool addEffect (int inputIndex, int program, int slot, const juce::PluginDescription&, juce::String& error);
    void removeEffect (int inputIndex, int program, int slot, int effect);
    /** Moves an effect within its chain. */
    void moveEffect (int inputIndex, int program, int slot, int from, int to);
    void setEffectBypassed (int inputIndex, int program, int slot, int effect, bool);

    /** Any plugin in a program: effect == -1 is the instrument of `slot`; slot == -1 the
        program chain. Returns nullptr if the program isn't loaded or the plugin failed to
        start; a plugin whose process died is returned but reports isAlive() == false. */
    RemotePlugin* getPlugin (int inputIndex, int program, int slot, int effect = -1) const;
    bool isPluginAlive (int inputIndex, int program, int slot, int effect = -1) const;
    /** True while the plugin's process is being started on the loader thread. */
    bool isPluginLoading (int inputIndex, int program, int slot, int effect = -1) const;
    /** True while any plugin is still loading. */
    bool hasPendingLoads() const                    { return pendingLoads.load() > 0; }
    /** Plugins being loaded right now (queued ones not counted). */
    int getLoadsInFlight() const                    { return loadsInFlight.load(); }
    int getParallelLoads() const                    { return loaderThreadCount; }
    juce::String getPluginLoadError (int inputIndex, int program, int slot, int effect = -1) const;
    /** Restarts a plugin that failed or crashed, from its saved definition and state. */
    void reloadPlugin (int inputIndex, int program, int slot, int effect = -1);

    //==============================================================================
    // Mappings
    void addMapping    (int inputIndex, int program, const MappingDef&);
    void updateMapping (int inputIndex, int program, int mappingIndex, const MappingDef&);
    void removeMapping (int inputIndex, int program, int mappingIndex);

    /** Arms MIDI learn: the next CC / pitch bend / aftertouch on any input fires
        Listener::learnReceived and disarms. */
    void setLearnArmed (bool);
    bool isLearnArmed() const                       { return learnArmed.load(); }

    struct TouchedParam { int inputIndex = -1, program = -1, slot = -1, effect = -1, paramIndex = -1; bool valid() const { return paramIndex >= 0; } };
    TouchedParam getLastTouchedParam() const        { return lastTouched; }

    //==============================================================================
    /** All notes off + all sound off on every loaded plugin. */
    void panic();

    /** Feeds a MIDI message into an input as if it had arrived from its device
        (channel filter, program change and learn handling all apply). Thread-safe. */
    void injectMidi (int inputIndex, const juce::MidiMessage&);

    /** Renders one block exactly as the audio callback would. Only for tests /
        offline use when constructed with startAudioDevice = false. */
    void renderBlockForTesting (float* const* out, int numOut, int numSamples);
    /** The same, with device input: what an audio input would hear. */
    void renderBlockForTesting (const float* const* in, int numIn, float* const* out, int numOut, int numSamples);

    double getSampleRate() const                    { return sampleRate; }
    double getCpuUsage() const                      { return deviceManager.getCpuUsage(); }
    /** Blocks in which at least one plugin process missed its deadline. */
    int getLateBlockCount() const                   { return lateBlocks.load(); }
    /** Samples per block actually delivered by the device (PipeWire's JACK
        client reports its default 1024 via jack_get_buffer_size even while
        the graph runs a smaller quantum; this is the real number). 0 until
        the first callback. */
    int getLastBlockSize() const                    { return lastBlockSamples.load(); }

    void addListener (Listener* l)                  { listeners.add (l); }
    void removeListener (Listener* l)               { listeners.remove (l); }

private:
    //==============================================================================
    struct PluginNode;
    struct SlotRuntime;
    struct EffectRuntime;
    struct MappingRuntime;
    struct ProgramRuntime;
    struct InputRuntime;
    struct OutputRuntime;

    struct Event
    {
        enum Type { programChange, learn, touched, pluginDied, pluginLoaded, tapTempo, tapLearn,
                    songStep, songLearn, editorClosed, outputVolume, outputLearn,
                    transport, transportLearn, trackFinished, sectionPedal, sectionLearn, sectionEntered, mappedSwitch, looperLearn };
        Type type; int input = 0, a = 0, b = 0, c = 0, d = 0;
    };

    /** Plugins load on a background thread (starting Kontakt takes ~10 s; a Wine stall
        can take far longer) so program changes and preloading never block the UI. */
    struct LoadJob
    {
        uint64_t nodeId;
        juce::PluginDescription desc;
        juce::MemoryBlock state;
        double sampleRate;
        int blockSize;
        bool bridged = false;    // Wine/yabridge plugin: limited concurrency (see takeJob)
        bool gm = false;         // the General MIDI player instead: reads the soundfont
        juce::File soundfont;
    };
    struct LoadResult
    {
        uint64_t nodeId;
        std::unique_ptr<RemotePlugin> plugin;
        std::unique_ptr<GmSynth> gm;
        juce::String error;
        double sampleRate;
        int blockSize;
    };

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

    // MidiInputCallback
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    static bool acceptsProgramChangeOn (const InputRuntime&, int channel);
    void routeMidi (InputRuntime&, int inputIndex, const juce::MidiMessage&);   // caller holds lock

    // AsyncUpdater / Timer / ChangeListener
    void handleAsyncUpdate() override;
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void postEvent (const Event&);

    // runtime management (message thread)
    void rebuildRuntimes();
    /** `captureFirst` pulls state into the model before destroying plugins;
        false when the model is about to go too. `budgetMs` >= 0 bounds the
        whole teardown. */
    void teardownRuntimes (bool captureFirst = true, int budgetMs = -1);
    ProgramRuntime* ensureProgramLoaded (int inputIndex, int program);
    /** The programs named by "another input's sounds" slots in the programs
        that are current, as (input, program): kept loaded and rendered
        beside the inputs' own. */
    std::set<std::pair<int, int>> lentPrograms() const;
    void ensureLentLoaded();
    std::unique_ptr<ProgramRuntime> buildProgram (int inputIndex, int program);
    std::unique_ptr<SlotRuntime> buildSlot (int inputIndex, int program, int slotIndex, const SlotDef&);
    std::unique_ptr<EffectRuntime> buildEffect (int inputIndex, int program, int slotIndex, int effectIndex, const EffectDef&);
    void queueLoad (PluginNode&, const juce::PluginDescription&, const juce::MemoryBlock& state);
    void queueGmLoad (SlotRuntime&);
    void loaderThreadFunc();
    void attachLoaded (LoadResult&);
    void unloadProgram (InputRuntime&, int program);
    void destroyProgramPlugins (ProgramRuntime&, int patienceMs = 3000);
    void destroyNode (PluginNode*, int patienceMs = 3000);
    bool captureProgramState (int inputIndex, ProgramRuntime&, int timeoutMs = -1, bool onlyChanged = false);   // true if anything changed
    void resolveMappings (ProgramRuntime&, const ProgramDef&);
    void openMidiDevices();
    void housekeeping();
    std::vector<std::unique_ptr<EffectRuntime>>* runtimeChainFor (ProgramRuntime&, int slot) const;
    std::vector<EffectDef>* defChainFor (ProgramDef&, int slot) const;
    void renumberChain (std::vector<std::unique_ptr<EffectRuntime>>&);

    // audio thread
    void processInput (InputRuntime&, const float* const* in, int numIn, float* const* out, int numOut, int numSamples, bool panic);
    /** `inL`/`inR` carry an audio input's signal into the program; null for a keyboard. */
    /** `fromTrack`: the input is the backing track, whose files' program
        changes are for the GM player only. */
    /** `received`: MIDI another input's "its sounds" instrument sent here
        this block, which is played but never passed on again (no echoes
        between two keyboards that each layer the other). May be null. */
    void processProgram (ProgramRuntime&, const juce::MidiBuffer& in, const juce::MidiBuffer* received, const float* inL, const float* inR,
                         float* const* out, int numOut, int numSamples, bool panic, bool fromTrack);
    /** The GM player's view of a slot's MIDI: its mixer's choices over the file's. */
    void prepareGmMidi (SlotRuntime&, int numSamples);
    static void applyInputDef (InputRuntime&, const InputDef&);
    /** The program definition an address names: a program's, or an output's master chain. */
    ProgramDef* defFor (int inputIndex, int program);
    const ProgramDef* defFor (int inputIndex, int program) const { return const_cast<Engine*> (this)->defFor (inputIndex, program); }
    void buildOutputs();
    void runChain (std::vector<std::unique_ptr<EffectRuntime>>&, const juce::MidiBuffer& midi, juce::AudioBuffer<float>& stereo, int numSamples);

    bool validInput (int i) const     { return i >= 0 && i < (int) setup.inputs.size(); }
    bool validProgram (int p) const   { return p >= 0 && p < InputDef::numPrograms; }
    ProgramRuntime* getLoaded (int inputIndex, int program) const;
    PluginNode* getNode (int inputIndex, int program, int slot, int effect) const;

    void noteParameterTouched (int inputIndex, int program, int slot, int effect, int paramIndex);
    void notePluginDied (int inputIndex, int program, int slot, int effect);
    void noteEditorClosed (int inputIndex, int program);
    void notifyContent (int inputIndex, int program);

    //==============================================================================
    PluginHost& host;
    juce::PropertiesFile& settings;
    juce::AudioDeviceManager deviceManager;

    Setup setup;
    juce::File setupFile;             // see setSetupFile
    std::vector<std::unique_ptr<InputRuntime>> runtimes;
    std::vector<std::unique_ptr<OutputRuntime>> outputs;      // same lock as runtimes
    std::atomic<int> outputLearn { -1 };                        // which output is waiting for its volume controller
    juce::StringArray lastDeviceWarnings;
    pthread_t callbackThread {};
    bool callbackThreadKnown = false;              // audio thread only
    std::atomic<bool> audioThreadRealtime { true };

    /** Tap tempo. Only the recent taps count: a pause means the player has
        stopped tapping and started again, and averaging across that gap would
        give a tempo nobody asked for. */
    std::vector<double> tapTimes;      // milliseconds, message thread only
    std::atomic<int> tapCC { 0 };      // read on the audio thread

    juce::CriticalSection lock;          // guards runtimes for audio + midi threads
    juce::CriticalSection eventLock;
    std::vector<Event> pendingEvents;

    double sampleRate = 44100.0;
    int blockSize = 512;

    std::atomic<bool> learnArmed { false };
    std::atomic<bool> tapLearnArmed { false };
    std::atomic<bool> panicRequested { false };
    std::atomic<int> liveTranspose { 0 };
    std::atomic<int> lateBlocks { 0 };
    std::atomic<int> lastBlockSamples { 0 };
    timespec blockDeadline {};
    TouchedParam lastTouched;

    juce::ListenerList<Listener> listeners;

    // background loader: a pool of threads, each plugin is its own process anyway.
    // Wine-bridged plugins start one at a time until the first has come up (the
    // prefix's wineserver and services boot on that first start; concurrent Wine
    // start-ups stall), then at most kBridgedParallel at once.
    int bridgedParallel = 2;                      // settings "parallelBridgedLoads"
    std::atomic<int> nextCC { 0 }, prevCC { 0 };   // read on the MIDI thread
    std::atomic<bool> songLearnArmed { false };
    bool songLearnForNext = true;

    // Backing tracks
    juce::AudioFormatManager trackFormats;

    // External instruments
    MidiOuts midiOuts;
    Recorder recorder;
    double chunkStartMs = 0.0;                    // audio thread: when this chunk will be heard, on Time's hi-res clock
    double outputLatencyMs = 0.0;
    const float* const* chunkIn = nullptr;        // audio thread: this chunk's device inputs
    int chunkNumIn = 0;
    void sendPatches (ProgramRuntime&, const ProgramDef&);
    /** Arms the song the set is on (not started), so Play works after a load. */
    void armCurrentSong();
    EffectDef* sendDefFor (int inputIndex, int program, int slot, int effect);
    EffectRuntime* sendRuntimeFor (int inputIndex, int program, int slot, int effect) const;
    /** Audio thread: every slot's MIDI chain, on the slot's MIDI for this block. */
    void runMidiChains (ProgramRuntime&, int numSamples, bool notesOffNow, bool panicNow);
    std::unique_ptr<SongPlayer> player;           // swapped under `lock`

    bool trackProblemsReported = true;            // message thread
    std::atomic<int> sectionCCAtomic { 0 };

    // MIDI clock out: the ports (resolved on the message thread), read by the audio thread
    std::atomic<int> clockPorts[MidiOuts::kMaxPorts] {};
    std::atomic<int> numClockPorts { 0 };
    std::atomic<double> tempoAtomic { 120.0 };
    std::atomic<int> clockTicks { 0 };
    double clockSamplesToNext = 0.0;              // audio thread
    void resolveClockPorts();                     // message thread
    void sendClock (int numSamples, double chunkStartMs, bool trackStarted, bool trackStopped);   // audio thread
    std::atomic<bool> sectionLearnArmed { false };
    std::atomic<int> looperCCAtomic { 0 }, looperClearCCAtomic { 0 };
    std::atomic<bool> looperLearnArmed { false }, looperClearLearnArmed { false };
    void loopProcess (InputRuntime& in, int numSamples, bool panicNow);   // audio thread
    int trackSong = -1;                           // message thread
    int lastTrackSection = -1;                    // audio thread: to notice the track entering a section
    std::atomic<int> transportCCAtomic { 0 };     // read on the MIDI thread
    std::atomic<bool> transportLearnArmed { false };
    void resetPluginPositions();                  // audio thread, under lock

    int loaderThreadCount = 4;                    // settings "parallelLoads"
    std::vector<std::thread> loaderThreads;
    std::mutex loaderMutex;
    std::condition_variable loaderCv;
    std::deque<LoadJob> loadQueue;
    std::vector<LoadResult> loadResults;
    std::vector<RemotePlugin*> loadsInProgress;   // guarded by loaderMutex
    int bridgedInFlight = 0;                      // guarded by loaderMutex
    bool bridgedWarm = false;                     // Wine is up: a wineserver was already running, or one bridged plugin has loaded
    static bool wineserverRunning();
    std::atomic<bool> loaderQuit { false };
    std::atomic<int> pendingLoads { 0 };
    std::atomic<int> loadsInFlight { 0 };
    bool takeJob (LoadJob&);                      // called with loaderMutex held
    std::map<uint64_t, PluginNode*> nodeRegistry;   // message thread only

    friend struct PluginNode;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};

} // namespace perf
