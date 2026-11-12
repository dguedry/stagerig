#include "Engine.h"
#if JUCE_LINUX
 #include <alsa/asoundlib.h>
#endif
#include "LowLatency.h"
#include "Liveness.h"
#include "DeviceReservation.h"
#include "ControlMaps.h"
#if JUCE_LINUX
 #include <dlfcn.h>
#endif
#include <thread>

#include <pthread.h>
#include <cstring>
#include <climits>
#include <cmath>
#include <algorithm>

using namespace juce;

namespace perf
{
#if JUCE_LINUX
namespace { juce::String alsaNameNow (juce::AudioIODeviceType*, const juce::String& saved, bool input);
            bool alsaCardsChanged (juce::AudioIODeviceType*, const juce::String& openOutput); }
#endif

//==============================================================================
// Runtime structures
//==============================================================================

/** A hosted plugin: an instrument or an effect, living in its own process. */
struct Engine::PluginNode : public RemotePlugin::Listener
{
    PluginNode (Engine& e, int in, int prog, int slot, int effect)
        : engine (e), inputIndex (in), program (prog), slotIndex (slot), effectIndex (effect), id (nextId++) {}

    ~PluginNode() override { if (plugin != nullptr) plugin->setListener (nullptr); }

    bool alive() const { return plugin != nullptr && plugin->isAlive(); }

    static inline std::atomic<uint64_t> nextId { 1 };

    // RemotePlugin::Listener (reader thread)
    void remoteParameterTouched (RemotePlugin&, int index, float) override
    {
        engine.noteParameterTouched (inputIndex.load(), program, slotIndex.load(), effectIndex.load(), index);
    }
    void remoteParameterChanged (RemotePlugin&, int index, float) override
    {
        engine.noteParameterTouched (inputIndex.load(), program, slotIndex.load(), effectIndex.load(), index);
    }
    void remoteDied (RemotePlugin&) override
    {
        engine.notePluginDied (inputIndex.load(), program, slotIndex.load(), effectIndex.load());
    }
    void remoteEditorClosed (RemotePlugin&) override
    {
        engine.noteEditorClosed (inputIndex.load(), program);
    }

    Engine& engine;
    std::atomic<int> inputIndex;
    int program;
    std::atomic<int> slotIndex, effectIndex;
    const uint64_t id;
    std::atomic<bool> loading { false };
    std::unique_ptr<RemotePlugin> plugin;
    String loadError;
    MidiBuffer midi;
};

struct Engine::EffectRuntime : public PluginNode
{
    using PluginNode::PluginNode;
    std::atomic<bool> bypassed { false };

    /** A MIDI send in a slot's MIDI chain: no plugin, a port. Fixed when built. */
    bool send = false;
    std::atomic<int> outPort { -1 }, sendChannel { 0 };
    /** A built-in MIDI effect: no plugin, a little code. Fixed when built; configured live. */
    std::unique_ptr<MidiFx> builtIn;
    MidiBuffer produced;           // a MIDI processor's output, swapped into the slot's MIDI
};

struct Engine::SlotRuntime : public PluginNode
{
    SlotRuntime (Engine& e, int in, int prog, int slot) : PluginNode (e, in, prog, slot, -1) {}

    /** An external instrument: played through a MIDI port, heard (if at all)
        through interface inputs. Fixed when the slot is built. */
    bool external = false;
    std::atomic<int> outPort { -1 }, returnLeft { -1 }, returnRight { -1 };
    /** The General MIDI player: the synth lives here instead of in a process. */
    bool isGm = false;
    std::unique_ptr<GmSynth> gm;                 // set by the loader, swapped under the lock
    std::atomic<int> gmProgram[16], gmVolume[16], gmPan[16];
    std::atomic<bool> gmMuted[16];
    std::atomic<bool> gmDirty { true };          // its mixer changed: tell the synth
    MidiBuffer gmMidi;                           // the slot's MIDI as the player should see it
    /** Another input's sounds: the notes go there. */
    bool isInputSlot = false;
    std::atomic<int> targetInput { -1 }, targetProgram { -1 };
    /** Whether it takes notes: a running plugin, a running GM player, an
        external instrument, another input, or anything with a MIDI chain
        (whose sends and processors want them). */
    bool playable() const { return external || isInputSlot || alive() || (isGm && gm != nullptr) || ! midiEffects.empty(); }

    void applyDef (const SlotDef& d)
    {
        enabled.store (d.enabled);
        gain.store (Decibels::decibelsToGain (d.gainDb, -60.0f));
        transpose.store (d.transpose);
        lowKey.store (d.lowKey);
        highKey.store (d.highKey);
        lowVelocity.store (d.lowVelocity);
        highVelocity.store (d.highVelocity);
        velocityCurve.store (d.velocityCurve);
        pan.store (d.pan);
        outChannel.store (d.outChannel);
        listensTo.store (d.listensTo);
        returnLeft.store (d.returnLeft);
        returnRight.store (d.returnRight);
        targetInput.store (d.targetInput);
        targetProgram.store (d.targetProgram);
        for (int c = 0; c < 16; ++c)
        {
            gmProgram[c].store (d.gmChannels[(size_t) c].program);
            gmVolume[c].store (d.gmChannels[(size_t) c].volume);
            gmPan[c].store (d.gmChannels[(size_t) c].pan);
            gmMuted[c].store (d.gmChannels[(size_t) c].muted);
        }
        gmDirty.store (true);
    }
    bool hears (int channel) const
    {
        return channel < 1 || channel > 16 || (listensTo.load (std::memory_order_relaxed) & (1 << (channel - 1))) != 0;
    }

    std::atomic<bool> enabled { true };
    std::atomic<float> gain { 1.0f }, velocityCurve { 0.0f }, pan { 0.0f };
    std::atomic<int> transpose { 0 }, lowKey { 0 }, highKey { 127 }, lowVelocity { 1 }, highVelocity { 127 }, outChannel { 0 }, listensTo { SlotDef::kAllChannels };
    std::vector<std::unique_ptr<EffectRuntime>> midiEffects;   // before the sound: processors and sends
    std::vector<std::unique_ptr<EffectRuntime>> effects;
    AudioBuffer<float> stereo;     // the slot's signal as it travels down the chain
};

struct Engine::MappingRuntime
{
    MappingDef def;
    RemotePlugin* plugin = nullptr;
    int paramIndex = -1;

    /* A built-in target instead of a plugin parameter: the instrument or
       effect itself -- on or off from a pedal, a level from a knob -- so a
       layer or an effect can be worked without a plugin in the loop. */
    enum BuiltIn { none, onOff, toggle, level };
    BuiltIn builtIn = none;
    SlotRuntime* slotNode = nullptr;
    EffectRuntime* effectNode = nullptr;
    bool wasHigh = false;          // audio thread: for the toggle's edge
};

/* A keyboard speaking MIDI 2.0 sends each controller, channel pressure and
   pitch bend at full resolution, which MIDI 1.0 rounds. The last full value
   of each, per channel, written on the MIDI thread before the message is
   queued and read with it on the audio thread. Bit 32 marks one as there. */
struct ExactValues
{
    std::array<std::atomic<uint64_t>, 16 * 128> cc {};
    std::array<std::atomic<uint64_t>, 16> pressure {}, bend {};

    std::atomic<uint64_t>* slotFor (const MidiMessage& m)
    {
        const int ch = m.getChannel() - 1;
        if (ch < 0 || ch > 15) return nullptr;
        if (m.isController())
        {
            // A controller number is 7 bits; a malformed packet's is not a slot.
            const int n = m.getControllerNumber();
            return n >= 0 && n < 128 ? &cc[(size_t) (ch * 128 + n)] : nullptr;
        }
        if (m.isChannelPressure())  return &pressure[(size_t) ch];
        if (m.isPitchWheel())       return &bend[(size_t) ch];
        return nullptr;
    }
    void store (const MidiMessage& m, uint32_t value)
    {
        if (auto* a = slotFor (m)) a->store ((uint64_t) 1 << 32 | value, std::memory_order_relaxed);
    }
    /** This message's value, 0..1, at full resolution when the last one stored
        is the value it was made from; otherwise -1. Several on one controller
        in one block each check against the newest, so all but the last fall
        back to their own 7 bits, and the last -- the one that stays -- is exact. */
    float read (const MidiMessage& m) const
    {
        auto* a = const_cast<ExactValues*> (this)->slotFor (m);
        if (a == nullptr) return -1.0f;
        const auto e = a->load (std::memory_order_relaxed);
        if ((e >> 32) == 0) return -1.0f;
        const auto v = (uint32_t) e;
        const bool same = m.isController()      ? (int) (v >> 25) == m.getControllerValue()
                        : m.isChannelPressure() ? (int) (v >> 25) == m.getChannelPressureValue()
                                                : (int) (v >> 18) == m.getPitchWheelValue();
        return same ? (float) ((double) v / 4294967295.0) : -1.0f;
    }
    void clear()
    {
        for (auto& a : cc) a.store (0);
        for (auto& a : pressure) a.store (0);
        for (auto& a : bend) a.store (0);
    }
};

struct Engine::ProgramRuntime
{
    int program = 0;
    const ExactValues* exact = nullptr;    // its input's, for its mappings; none for an output's chain
    std::vector<std::unique_ptr<SlotRuntime>> slots;
    std::vector<std::unique_ptr<EffectRuntime>> effects;   // program-level chain
    std::vector<MappingRuntime> mappings;
    AudioBuffer<float> mix;        // summed slots, before the program chain
    MidiBuffer programMidi;        // MIDI seen by the program chain
    int releaseSamplesLeft = 0;
    bool needsNotesOff = false;
    std::atomic<float> inputGain { 1.0f };    // audio inputs: the trim before the chain

    /* Lent to another input: a slot there names this program, so it plays
       beside the input's current one. `lent` is marked each block from the
       programs that are current; the notes arrive in `sentMidi` and are
       played from `received` -- never passed on again. */
    bool lent = false;
    MidiBuffer sentMidi, received;

    /* What the player is holding on this program, kept on the audio thread
       as the notes go through: one bit per key per channel, and the sustain
       pedal per channel. A program change reads it to decide whether the
       outgoing program can be left to finish on its own. */
    uint64_t heldKeys[16][2] = {};
    bool sustainDown[16] = {};
    /* Left behind by a change with something held: it still gets note-offs,
       pedals and wheels -- never a new note -- and its tail waits until the
       last of them is let go. */
    bool holding = false;

    bool anythingHeld() const
    {
        for (int c = 0; c < 16; ++c)
            if (heldKeys[c][0] != 0 || heldKeys[c][1] != 0 || sustainDown[c]) return true;
        return false;
    }
    void clearHeld()
    {
        std::memset (heldKeys, 0, sizeof (heldKeys));
        std::memset (sustainDown, 0, sizeof (sustainDown));
    }
    void track (const MidiMessage& m)
    {
        const int ch = m.getChannel() - 1;
        if (ch < 0 || ch > 15) return;
        if (m.isNoteOn())
        {
            const int n = m.getNoteNumber();
            heldKeys[ch][n >> 6] |= (uint64_t) 1 << (n & 63);
        }
        else if (m.isNoteOff())
        {
            const int n = m.getNoteNumber();
            heldKeys[ch][n >> 6] &= ~((uint64_t) 1 << (n & 63));
        }
        else if (m.isController())
        {
            const int cc = m.getControllerNumber();
            if (cc == 64)                      sustainDown[ch] = m.getControllerValue() >= 64;
            else if (cc == 123 || cc == 120)   heldKeys[ch][0] = heldKeys[ch][1] = 0;
        }
    }
};

struct Engine::InputRuntime
{
    String deviceIdentifier;                  // guarded by Engine::lock
    /* Its device went away or was reconnected: whatever it was holding will
       never be let go by it, so the audio thread lets go of it all. */
    std::atomic<bool> deviceLost { false };
    std::atomic<int> channel { 0 };
    std::atomic<bool> respondToProgramChange { true };
    std::atomic<int> programChangeChannel { InputDef::pcChannelSameAsNotes };
    std::atomic<bool> audio { false };                       // an audio input: the signal is the instrument
    std::atomic<bool> track { false };                       // the backing track: its MIDI files play here
    std::atomic<int> audioLeft { 0 }, audioRight { 1 };      // device input channels; right -1 = mono
    std::atomic<float> levelL { 0.0f }, levelR { 0.0f };     // the meter: pre-trim peaks, falling 20 dB/s
    std::atomic<uint32_t> clipMs { 0 };                      // when the input last touched full scale
    std::atomic<int> output { 0 };                           // which output it lands on
    // The keyboard's taught knobs and buttons (SurfaceControls), read on the MIDI thread.
    std::array<std::atomic<int>, SurfaceControls::numKnobs> knobCC {};    // taught
    std::array<std::atomic<int>, SurfaceControls::numKnobs> heardCC {};   // taught, or the spare the keyboard was told: SurfaceControls::heardCC()
    std::atomic<bool> knobsRelative { false };
    std::atomic<bool> autoKnobs { false };      // untaught knobs answer to the spares, on a keyboard told what to send
    std::array<std::atomic<int>, SurfaceControls::numActions> buttonCC {};
    ExactValues exact;             // a MIDI 2.0 keyboard's full values
    MidiMessageCollector collector;
    MidiBuffer block;
    MidiBuffer trackMidi;          // the backing track's MIDI for this block, merged into `block`
    MidiBuffer sentMidi;           // what other inputs' "its sounds" slots sent here, for the next block
    MidiBuffer received;           // the same, as this block plays it (never forwarded again)
    MidiBuffer holdBlock;          // this block's MIDI less the note-ons, for programs left sounding
    MidiBuffer shifted;            // this block's MIDI with the transpose applied
    int8_t noteShift[16][128] = {};   // what each sounding key was shifted by at its note-on
    int shiftedHeld = 0;              // how many of those are shifted at all

    /* The looper: what was played, in samples from the loop's start, going
       round in whole bars. Commands come from the pedal or the panel as an
       atomic and are acted on at the top of a block, so a change is clean. */
    struct Looper
    {
        struct Ev { int64 at; uint8 data[3]; uint8 size; };
        std::vector<Ev> events;
        std::atomic<int> state { 0 };     // Engine::LoopState
        std::atomic<int> command { 0 };   // 1 = the pedal, 2 = clear, 3 = stop
        std::atomic<int> bars { 0 };
        int64 length = 0, pos = 0, recorded = 0;
        uint8 held[16][128] = {};         // notes the loop is sounding, to let go on a stop
    } loop;
    std::map<int, std::unique_ptr<ProgramRuntime>> loaded;
    ProgramRuntime* active = nullptr;
    std::vector<ProgramRuntime*> releasing;
    bool isReleasing (const ProgramRuntime* p) const
    {
        return std::find (releasing.begin(), releasing.end(), p) != releasing.end();
    }
    void stopReleasing (const ProgramRuntime* p)
    {
        releasing.erase (std::remove (releasing.begin(), releasing.end(), p), releasing.end());
    }
};

/** An output: what its inputs add up to, through the master chain, at the
    master volume, onto two device channels. */
struct Engine::OutputRuntime
{
    ProgramRuntime chain;                        // only its effects are used
    std::atomic<float> gain { 1.0f };
    std::atomic<int> left { 0 }, right { 1 }, volumeCC { 0 };
    AudioBuffer<float> mix;
};

//==============================================================================
Engine::Engine (PluginHost& h, PropertiesFile& s, bool startAudioDevice) : host (h), settings (s)
{
    std::fill (std::begin (guardGain), std::end (guardGain), 1.0f);

    /* Link is made now and switched on only if it was on last time. Its
       callbacks arrive on its own thread, so they only post events. */
    link = std::make_unique<ableton::Link> (setup.tempoBpm);
    link->setNumPeersCallback ([this] (std::size_t n) { linkPeers.store ((int) n); });
    link->setTempoCallback ([this] (double bpm) { linkTempoFromPeer.store (bpm); postEvent ({ Event::linkTempo, 0, 0, 0, 0, 0 }); });
    link->setStartStopCallback ([this] (bool playing) { linkPlayingFromPeer.store (playing); postEvent ({ Event::linkStartStop, 0, playing ? 1 : 0, 0, 0, 0 }); });
    link->enableStartStopSync (settings.getBoolValue ("linkStartStop", false));
    link->enable (settings.getBoolValue ("linkEnabled", false));

    if (startAudioDevice)
    {
        std::unique_ptr<XmlElement> savedAudio (settings.getXmlValue ("audioDeviceState"));
        if (savedAudio != nullptr) savedAudio->deleteAllChildElementsWithTagName ("MIDIINPUT");
        if (const char* st = std::getenv ("STAGERIG_TEST_STALL"); st != nullptr && std::atoi (st) > 0)
            stallTestFrom = Time::getMillisecondCounter() + (uint32) std::atoi (st) * 1000;
        pendingEvents.reserve (1024);
        handlingEvents.reserve (1024);
        /* A sound card chosen on ALSA is opened or nothing is: falling back to
           "the default" there leads into PipeWire's emulation (see
           CardInputsAlsa). If the card will not open at the saved rate (the
           SL73 plays only at 48 kHz), at the rate it offers; failing that, the
           audio watch tries the chosen card again. */
        const bool savedAlsa = savedAudio != nullptr && savedAudio->getStringAttribute ("deviceType") == "ALSA";
       #if JUCE_LINUX
        /* Chosen before ALSA's list was each card once by a plain name: the old
           name ("SL; USB Stream Output", a card's front or surround device...)
           becomes the card's direct device, and is saved so. */
        if (savedAlsa)
            for (auto* t : deviceManager.getAvailableDeviceTypes())
                if (t->getTypeName() == "ALSA")
                {
                    t->scanForDevices();
                    bool changed = false;
                    for (auto [attr, input] : { std::pair { "audioOutputDeviceName", false }, std::pair { "audioInputDeviceName", true } })
                    {
                        const auto old = savedAudio->getStringAttribute (attr);
                        if (old.isEmpty()) continue;
                        if (const auto now = alsaNameNow (t, old, input); now.isNotEmpty() && now != old)
                        {
                            std::fprintf (stderr, "[audio] '%s' is now '%s'\n", old.toRawUTF8(), now.toRawUTF8());
                            savedAudio->setAttribute (attr, now);
                            changed = true;
                        }
                    }
                    if (changed) settings.setValue ("audioDeviceState", savedAudio.get());
                }
       #endif
        auto err = deviceManager.initialise (0, 2, savedAudio.get(), ! savedAlsa);
        if (savedAlsa && err.isNotEmpty())
        {
            std::fprintf (stderr, "[audio] the chosen sound card did not open (%s); trying it at its own rate\n", err.toRawUTF8());
            XmlElement anyRate (*savedAudio);
            anyRate.removeAttribute ("audioDeviceRate");
            err = deviceManager.initialise (0, 2, &anyRate, false);
            if (err.isNotEmpty())
                std::fprintf (stderr, "[audio] still not open (%s); the audio watch will try again\n", err.toRawUTF8());
        }

        /* First run, or a saved state on PipeWire's ALSA emulation ("Default
           ALSA Output (currently PipeWire Media Server)"): prefer the JACK type
           (PipeWire's graph, no resampling, small quantum) at 48 kHz. The
           emulation is never kept while JACK has devices: its input has been
           seen to crash inside libasound on the first read (snd_pcm_mmap_readi),
           and a rig that crashed into it would crash on every start after. A
           real ALSA device chosen by hand (a card by name) is left alone. */
        const bool pipewireAlsa = savedAudio != nullptr
                                  && savedAudio->getStringAttribute ("deviceType") == "ALSA"
                                  && (savedAudio->getStringAttribute ("audioOutputDeviceName").contains ("PipeWire")
                                      || savedAudio->getStringAttribute ("audioInputDeviceName").contains ("PipeWire"));
        if (savedAudio == nullptr || pipewireAlsa)
            for (auto* type : deviceManager.getAvailableDeviceTypes())
                if (type->getTypeName() == "JACK")
                {
                    type->scanForDevices();
                    if (type->getDeviceNames (false).isEmpty()) break;
                    deviceManager.setCurrentAudioDeviceType ("JACK", true);
                    auto setup = deviceManager.getAudioDeviceSetup();
                    setup.sampleRate = 48000.0;
                    deviceManager.setAudioDeviceSetup (setup, true);
                    settings.setValue ("jackMigrated", true);
                    break;
                }

        deviceManager.addChangeListener (this);
        deviceManager.addAudioCallback (this);
    }
    deviceManager.addMidiInputDeviceCallback ({}, this);
    midi2 = std::make_unique<Midi2Inputs> ([this] (const String& id, const MidiMessage& m, int64 exact) { receiveMidi (id, m, exact); });
    midiDeviceList = MidiDeviceListConnection::make ([this] { midiDevicesChanged(); });
    /* What is there at the start is known from the start: the first change
       heard of would otherwise take every port for a new arrival, and close
       and open them all under whoever was playing. */
    for (const auto& d : MidiInput::getAvailableDevices())  knownMidiDevices.add ("i " + d.name + "|" + d.identifier);
    for (const auto& d : MidiOutput::getAvailableDevices()) knownMidiDevices.add ("o " + d.name + "|" + d.identifier);

    // Backing tracks: WAV, AIFF, FLAC, Ogg and MP3, read ahead on their own thread.
    trackFormats.registerBasicFormats();

    /* Default from the machine rather than a fixed 4. Loading is mostly waiting
       -- Wine starting, a plugin reading its own files -- so the useful number
       is "how many can be in flight", not "how many cores are free". Measured
       on a 16-core/32-thread machine with 7 bridged plugins: at 4, the fifth
       waited 11 s for a slot; at 8 and at 12 they all started at once and the
       last was ready in the time the slowest single plugin took. Half the
       hardware threads, so a 4-core laptop still gets 4 and does not thrash. */
    const int defaultLoads = jlimit (4, 12, SystemStats::getNumCpus() / 2);
    loaderThreadCount = jlimit (1, 16, settings.getIntValue ("parallelLoads", defaultLoads));
    bridgedParallel   = jlimit (1, 16, settings.getIntValue ("parallelBridgedLoads", loaderThreadCount));
    // Measured: two Kontakts starting together on a cold prefix both come up in the time
    // one takes (11 s for the pair instead of 19 s), so bridged loads run in parallel by
    // default; "bridgedColdStartSolo" starts the first one alone if a machine needs that.
    bridgedWarm = wineserverRunning() || ! settings.getBoolValue ("bridgedColdStartSolo", false);
    for (int i = 0; i < loaderThreadCount; ++i)
        loaderThreads.emplace_back ([this] { loaderThreadFunc(); });

    loadSetup (Setup::makeDefault());
    startTimer (750);
}

Engine::~Engine()
{
    stopTimer();
    cancelPendingUpdate();
    midiDeviceList.reset();
    midi2.reset();
    deviceManager.removeMidiInputDeviceCallback ({}, this);
    deviceManager.removeAudioCallback (this);
    deviceManager.removeChangeListener (this);
    /* No capture: whatever was worth saving was saved by the window on its
       way out, and the model dies with this object. Bounded, because a quit
       that waits on every host that has stopped answering is a quit that
       takes minutes. */
    teardownRuntimes (false, 6000);

    {
        std::unique_ptr<SongPlayer> gone;
        { const ScopedLock sl (lock); gone = std::move (player); }
    }

    // Stop the loader: drop queued jobs, cut short a load in progress, join.
    {
        std::lock_guard<std::mutex> l (loaderMutex);
        loaderQuit.store (true);
        loadQueue.clear();
        for (auto* p : loadsInProgress) p->shutdown();
    }
    loaderCv.notify_all();
    for (auto& t : loaderThreads) if (t.joinable()) t.join();
    for (auto& r : loadResults)
        if (r.plugin != nullptr) r.plugin->shutdown();
}

void Engine::restartAudioDevice()
{
    auto setup = deviceManager.getAudioDeviceSetup();
    deviceManager.closeAudioDevice();
    deviceManager.setAudioDeviceSetup (setup, true);
    if (deviceManager.getCurrentAudioDevice() == nullptr)
        deviceManager.restartLastAudioDevice();
}

#if JUCE_LINUX
namespace
{
    /** A JACK device that never says its buffer is zero samples.

        PipeWire's JACK reports a buffer of 0 for a moment while StageRig's ports
        are linked again (an input switched off, a buffer size changed). JUCE
        splits every block into pieces of the size read at start, and pieces of
        none never finish a block: the audio thread spun for ever, holding the
        lock the screen then waited on, and the rig froze with the sound gone.
        The last real size stands in, or the size asked for. */
    class SteadyBufferDevice final : public AudioIODevice, private AudioIODeviceCallback
    {
    public:
        /** `plain`, when given, turns the device's own error on opening into words for a player. */
        explicit SteadyBufferDevice (std::unique_ptr<AudioIODevice> realDevice, std::function<String (const String&)> plain = {})
            : AudioIODevice (realDevice->getName(), realDevice->getTypeName()), real (std::move (realDevice)), plainError (std::move (plain)) {}

        ~SteadyBufferDevice() override { real->close(); }

        AudioIODevice& inner() noexcept { return *real; }

        StringArray getOutputChannelNames() override                 { return real->getOutputChannelNames(); }
        StringArray getInputChannelNames() override                  { return real->getInputChannelNames(); }
        std::optional<BigInteger> getDefaultOutputChannels() const override { return real->getDefaultOutputChannels(); }
        std::optional<BigInteger> getDefaultInputChannels() const override  { return real->getDefaultInputChannels(); }
        Array<double> getAvailableSampleRates() override             { return real->getAvailableSampleRates(); }
        Array<int> getAvailableBufferSizes() override
        {
            auto sizes = real->getAvailableBufferSizes();
            sizes.removeAllInstancesOf (0);
            if (sizes.isEmpty()) sizes.add (getCurrentBufferSizeSamples());
            return sizes;
        }
        int getDefaultBufferSize() override                          { return steady (real->getDefaultBufferSize()); }

        String open (const BigInteger& ins, const BigInteger& outs, double rate, int bufferSize) override
        {
            if (bufferSize > 0) requested = bufferSize;
            const auto error = real->open (ins, outs, rate, bufferSize);
            if (error.isEmpty() || ! plainError) return error;
            std::fprintf (stderr, "[audio] %s: %s\n", getName().toRawUTF8(), error.toRawUTF8());
            return plainError (error);
        }
        void close() override                                        { real->close(); }
        bool isOpen() override                                       { return real->isOpen(); }

        void start (AudioIODeviceCallback* cb) override
        {
            if (cb == nullptr) { stop(); return; }
            if (client.load() != nullptr && client.load() != cb) real->stop();   // so the new one hears it start
            client.store (cb);
            real->start (this);
        }
        void stop() override
        {
            real->stop();     // returns once no block is running
            client.store (nullptr);
        }
        /* JUCE's ALSA device says it is not playing for good once any call in
           its loop has failed -- an underrun among them, which the next line
           there recovers from, the sound carrying on. Taken at its word, one
           underrun had the audio watch close and open the interface: a
           dropout for nothing, and the opening over and over that locks up a
           USB keyboard's audio. For ALSA (the device given `plain`), started
           and open is playing; a loop that has really ended stops calling,
           which the watch sees for itself. */
        bool isPlaying() override
        {
            if (client.load() == nullptr) return false;
            return plainError ? real->isOpen() : real->isPlaying();
        }
        String getLastError() override                               { return real->getLastError(); }

        int getCurrentBufferSizeSamples() override                   { return steady (real->getCurrentBufferSizeSamples()); }
        double getCurrentSampleRate() override                       { return real->getCurrentSampleRate(); }
        int getCurrentBitDepth() override                            { return real->getCurrentBitDepth(); }
        BigInteger getActiveOutputChannels() const override          { return real->getActiveOutputChannels(); }
        BigInteger getActiveInputChannels() const override           { return real->getActiveInputChannels(); }
        int getOutputLatencyInSamples() override                     { return real->getOutputLatencyInSamples(); }
        int getInputLatencyInSamples() override                      { return real->getInputLatencyInSamples(); }
        AudioWorkgroup getWorkgroup() const override                 { return real->getWorkgroup(); }
        bool hasControlPanel() const override                        { return real->hasControlPanel(); }
        bool showControlPanel() override                             { return real->showControlPanel(); }
        bool setAudioPreprocessingEnabled (bool b) override          { return real->setAudioPreprocessingEnabled (b); }
        int getXRunCount() const noexcept override                   { return real->getXRunCount(); }

    private:
        int steady (int reported)
        {
            if (reported > 0) { lastGood = reported; return reported; }
            if (lastGood > 0) return lastGood;
            return requested > 0 ? requested : 1024;
        }

        // The real device calls these; they go on to the manager, naming this device.
        void audioDeviceAboutToStart (AudioIODevice*) override
        {
            if (auto* cb = client.load()) cb->audioDeviceAboutToStart (this);
        }
        void audioDeviceStopped() override
        {
            if (auto* cb = client.load()) cb->audioDeviceStopped();
        }
        void audioDeviceError (const String& message) override
        {
            if (auto* cb = client.load()) cb->audioDeviceError (message);
        }
        void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                               int numSamples, const AudioIODeviceCallbackContext& context) override
        {
            if (auto* cb = client.load(); cb != nullptr && numSamples > 0)
                cb->audioDeviceIOCallbackWithContext (in, numIn, out, numOut, numSamples, context);
            else
                for (int c = 0; c < numOut; ++c)
                    if (out[c] != nullptr) FloatVectorOperations::clear (out[c], jmax (0, numSamples));
        }

        std::unique_ptr<AudioIODevice> real;
        std::function<String (const String&)> plainError;
        std::atomic<AudioIODeviceCallback*> client { nullptr };
        std::atomic<int> lastGood { 0 };
        int requested = 0;
    };

    /** ALSA as StageRig offers it: each sound card's own devices, once each,
        by a plain name ("SL — USB Audio"), and always the direct one behind it
        (hw:CARD=...,DEV=...).

        ALSA itself lists a card a dozen ways -- front, surround 2.1 to 7.1,
        S/PDIF, dmix, dsnoop, "plug" conversions, the USB stream, and its
        default -- and, besides, "default", "pipewire" and "pulse", which lead
        back into PipeWire: its input has crashed StageRig in libasound, and
        opening it with PipeWire stuck hung StageRig at start. Nobody can be
        expected to pick the right one; on a stage the right one is the card
        itself, with nothing in between, so that is the only one shown. */
    class CardInputsAlsa : public AudioIODeviceType
    {
    public:
        explicit CardInputsAlsa (std::unique_ptr<AudioIODeviceType> real)
            : AudioIODeviceType (real->getTypeName()), alsa (std::move (real)) {}

        void scanForDevices() override
        {
            cardsSeen = cardList();
            alsa->scanForDevices();
            inputs.clear();
            outputs.clear();
            const auto direct = directDevices();
            for (bool input : { false, true })
            {
                auto& list = input ? inputs : outputs;
                const auto juceNames = alsa->getDeviceNames (input);
                for (const auto& d : direct)
                    if (juceNames.contains (d.juceName) && suitable (d, input))
                        list.push_back (d);
                // A USB interface first: what ALSA opens when nothing is chosen yet.
                std::stable_sort (list.begin(), list.end(), [] (const Direct& x, const Direct& y) { return isUsb (x.card) && ! isUsb (y.card); });
            }
        }

        StringArray getDeviceNames (bool wantInputNames) const override
        {
            StringArray names;
            for (const auto& d : wantInputNames ? inputs : outputs) names.add (d.name);
            return names;
        }

        int getDefaultDeviceIndex (bool) const override { return 0; }

        int getIndexOfDevice (AudioIODevice* device, bool asInput) const override
        {
            if (auto* wrapped = dynamic_cast<SteadyBufferDevice*> (device)) device = &wrapped->inner();
            const int i = alsa->getIndexOfDevice (device, asInput);
            if (i < 0) return i;
            const auto juceName = alsa->getDeviceNames (asInput)[i];
            const auto& list = asInput ? inputs : outputs;
            for (size_t k = 0; k < list.size(); ++k)
                if (list[k].juceName == juceName) return (int) k;
            return -1;
        }

        bool hasSeparateInputsAndOutputs() const override { return true; }

        AudioIODevice* createDevice (const String& outputName, const String& inputName) override
        {
            const auto* out = find (outputs, outputName);
            const auto* in  = find (inputs, inputName);
            if ((outputName.isNotEmpty() && out == nullptr) || (inputName.isNotEmpty() && in == nullptr))
                return nullptr;

            /* The cards are asked for from PipeWire first, which closes them. JUCE
               opens an ALSA device waiting until it is free, on the message
               thread: a card PipeWire still had open froze the Audio window (and
               the rig) for good. One still busy after that is not opened at all;
               the audio watch tries again. */
            Array<int> cards;
            if (out != nullptr) cards.addIfNotAlreadyThere (out->card);
            if (in != nullptr)  cards.addIfNotAlreadyThere (in->card);
            if (const auto busy = DeviceReservation::holdOnly (cards); ! busy.isEmpty())
            {
                StringArray which;
                for (int card : busy) which.add (String (card));
                std::fprintf (stderr, "[audio] sound card %s still in use by another program; not opened\n", which.joinIntoString (", ").toRawUTF8());
                return nullptr;
            }
            std::unique_ptr<AudioIODevice> d (alsa->createDevice (out != nullptr ? out->juceName : String(), in != nullptr ? in->juceName : String()));
            if (d == nullptr) return nullptr;
            const int card = out != nullptr ? out->card : in->card;
            const String name = out != nullptr ? out->name : in->name;
            return new SteadyBufferDevice (std::move (d), [card, name] (const String& e) { return plainError (e, card, name); });
        }

        /** A card plugged in or taken out since the last look: the list is made
            again (ALSA tells nobody), so the Audio window shows what is there and
            the audio watch finds an interface switched back on. Only a card
            coming is announced: JUCE, told its open device has gone, would open
            another interface in its place. Cheap enough for every watch tick
            (one small file); the full scan, which asks every card, only then. */
        bool cardsChanged (const String& openOutput)
        {
            if (cardList() == cardsSeen) return false;
            const auto before = outputs.size() + inputs.size();
            StringArray had;
            for (const auto& d : outputs) had.add (d.name);
            for (const auto& d : inputs)  had.add (d.name);
            scanForDevices();
            bool added = outputs.size() + inputs.size() > before;
            for (const auto& d : outputs) added = added || ! had.contains (d.name);
            for (const auto& d : inputs)  added = added || ! had.contains (d.name);
            /* Told while its open device is not in the list (gone), JUCE would open
               another interface in its place: then only the audio watch acts. */
            if (added && (openOutput.isEmpty() || find (outputs, openOutput) != nullptr)) callDeviceChangeListeners();
            return true;
        }

        /** The name shown now for a device saved under any older name: one of
            ALSA's other names for the same card ("SL; USB Stream Output", the
            card's front or surround device...) becomes that card's direct
            device. Empty when the card is not there. */
        String currentNameFor (const String& saved, bool input) const
        {
            const auto& list = input ? inputs : outputs;
            if (find (list, saved) != nullptr) return saved;
            const int card = cardOf (saved);
            for (const auto& d : list)
                if (d.juceName == saved || (card >= 0 && d.card == card)) return d.name;
            return {};
        }

    private:
        struct Direct { String name, juceName; int card = -1, device = -1; };

        /** A device fit for a rig: not a monitor's HDMI sound, nor a digital
            S/PDIF socket, which a keyboard player never plugs into and which
            ALSA lists beside the analog outputs under the same card name. */
        static bool isUsb (int card) { return File ("/proc/asound/card" + String (card) + "/usbid").existsAsFile(); }

        static bool suitable (const Direct& d, bool input)
        {
            const File info ("/proc/asound/card" + String (d.card) + "/pcm" + String (d.device) + (input ? "c" : "p") + "/info");
            if (! info.existsAsFile()) return false;      // the card has no such direction
            const auto id = info.loadFileAsString().fromFirstOccurrenceOf ("id:", false, false)
                                .upToFirstOccurrenceOf ("\n", false, false).trim().toLowerCase();
            // STAGERIG_TEST_ALSA_ALL: for trying the ALSA paths on a monitor's sound, away from the rig's interface.
            if (std::getenv ("STAGERIG_TEST_ALSA_ALL") == nullptr)
                for (auto* digital : { "hdmi", "digital", "iec958", "spdif", "s/pdif", "displayport" })
                    if (id.contains (digital)) return false;
            return true;
        }

        static const Direct* find (const std::vector<Direct>& list, const String& name)
        {
            if (name.isEmpty()) return nullptr;
            for (const auto& d : list) if (d.name == name) return &d;
            return nullptr;
        }

        template <typename Fn>
        static void forEachHint (Fn&& fn)
        {
            void** hints = nullptr;
            if (snd_device_name_hint (-1, "pcm", &hints) != 0) return;
            for (auto** h = hints; *h != nullptr; ++h)
            {
                auto text = [h] (const char* key)
                {
                    char* v = snd_device_name_get_hint (*h, key);
                    const String r (CharPointer_UTF8 (v != nullptr ? v : ""));
                    ::free (v);
                    return r;
                };
                fn (text ("NAME"), text ("DESC"));
            }
            snd_device_name_free_hint (hints);
        }

        static int cardIndex (const String& alsaId)
        {
            const auto cardId = alsaId.fromFirstOccurrenceOf ("CARD=", false, false).upToFirstOccurrenceOf (",", false, false);
            return cardId.isEmpty() ? -1 : snd_card_get_index (cardId.toRawUTF8());
        }

        /** Every card's direct devices: "SL — USB Audio" for hw:CARD=SL,DEV=0,
            from the first line of ALSA's description ("SL, USB Audio"). */
        static std::vector<Direct> directDevices()
        {
            std::vector<Direct> out;
            forEachHint ([&out] (const String& id, const String& desc)
            {
                if (! id.startsWith ("hw:CARD=")) return;
                Direct d;
                d.juceName = desc.replace ("\n", "; ");
                const auto first = desc.upToFirstOccurrenceOf ("\n", false, false);
                d.name = first.replaceFirstOccurrenceOf (", ", String (CharPointer_UTF8 (" \xe2\x80\x94 ")));
                d.card = cardIndex (id);
                d.device = id.fromLastOccurrenceOf ("DEV=", false, false).getIntValue();
                for (const auto& o : out) if (o.name == d.name) d.name << " (" << id.fromLastOccurrenceOf ("DEV=", false, false) << ")";
                out.push_back (d);
            });
            return out;
        }

        /** The card behind any name ALSA (as JUCE shows it) gives one of its devices. */
        static int cardOf (const String& juceName)
        {
            String bare = juceName;
            if (bare.endsWithChar (')'))
                if (const int open = bare.lastIndexOf (" ("); open > 0
                    && bare.substring (open + 2, bare.length() - 1).containsOnly ("0123456789"))
                    bare = bare.substring (0, open);
            int card = -1;
            forEachHint ([&] (const String& id, const String& desc)
            {
                if (card < 0 && id.contains ("CARD=") && desc.replace ("\n", "; ") == bare) card = cardIndex (id);
            });
            return card;
        }

        static String cardList() { return File ("/proc/asound/cards").loadFileAsString(); }

        /** A card's name as its maker gives it ("Saffire 6 USB"), from the kernel's list. */
        static String cardName (int card)
        {
            for (const auto& line : StringArray::fromLines (cardList()))
                if (line.trim().startsWith (String (card) + " [") && line.contains (" - "))
                    return line.fromFirstOccurrenceOf (" - ", false, false).trim();
            return "card " + String (card);
        }

        /** ALSA's words for why a card did not open, in a player's. */
        static String plainError (const String& error, int card, const String& name)
        {
            const auto e = error.toLowerCase();
            if (e.contains ("no space left on device"))
            {
                /* The kernel's "not enough bandwidth": a USB socket group has room
                   for so much audio at once, and another USB audio device on the
                   same group is using it. Which one, by its bus. */
                const auto bus = File ("/proc/asound/card" + String (card) + "/usbbus").loadFileAsString().upToFirstOccurrenceOf ("/", false, false).trim();
                StringArray others;
                if (bus.isNotEmpty())
                    for (const auto& dir : File ("/proc/asound").findChildFiles (File::findDirectories, false, "card*"))
                        if (dir.getFileName() != "card" + String (card)
                            && dir.getChildFile ("usbbus").loadFileAsString().upToFirstOccurrenceOf ("/", false, false).trim() == bus)
                            others.add (cardName (dir.getFileName().substring (4).getIntValue()));
                const auto who = others.isEmpty() ? String ("another USB device") : others.joinIntoString (" and ");
                return name + " has no room on its USB connection for its sound: " + who + " shares the same group of USB "
                       "sockets and is using it. Plug " + name + " into a socket of another group (on many computers, the "
                       "front sockets, or another row at the back), or switch " + who + " off. Then choose Reopen audio.";
            }
            if (e.contains ("busy"))
                return name + " is in use by another program. Close it, or choose Reopen audio.";
            if (e.contains ("no such device") || e.contains ("no such file"))
                return name + " is not there: switched off or unplugged. Switch it on: StageRig reconnects by itself.";
            return name + " did not open (" + error + ").";
        }

        std::unique_ptr<AudioIODeviceType> alsa;
        std::vector<Direct> inputs, outputs;
        String cardsSeen;
    };

    bool alsaCardsChanged (AudioIODeviceType* t, const String& openOutput)
    {
        if (auto* cards = dynamic_cast<CardInputsAlsa*> (t)) return cards->cardsChanged (openOutput);
        return false;
    }

    String alsaNameNow (AudioIODeviceType* t, const String& saved, bool input)
    {
        if (auto* cards = dynamic_cast<CardInputsAlsa*> (t)) return cards->currentNameFor (saved, input);
        return {};
    }


    /* Set when the sound system has stopped answering (see soundSystemAnswers). */
    std::atomic<bool> soundSystemStuck { false };

    /** Whether PipeWire's JACK (or a JACK server) answers within timeoutMs.

        A USB audio device that locks up (the SL73 has: "usb_set_interface
        failed (-110)") can leave PipeWire's session manager stuck in the kernel,
        and from then on every program asking PipeWire anything waits for good.
        StageRig asked on its message thread -- to start, and every few seconds
        to look for devices -- and never opened, or froze. It asks on a thread of
        its own now and stops waiting; one question left unanswered is not
        asked again until it is answered. */
    bool soundSystemAnswers (int timeoutMs)
    {
        static std::atomic<bool> asking { false };
        if (asking.load()) { soundSystemStuck = true; return false; }

        using OpenFn = void* (*) (const char*, int, int*, ...);
        using CloseFn = int (*) (void*);
        static OpenFn openClient = nullptr;
        static CloseFn closeClient = nullptr;
        if (openClient == nullptr)
        {
            void* lib = ::dlopen ("libjack.so.0", RTLD_NOW | RTLD_NOLOAD);
            if (lib == nullptr) lib = ::dlopen ("libjack.so.0", RTLD_NOW);
            if (lib == nullptr) return true;     // no JACK at all: nothing to wait on
            openClient = reinterpret_cast<OpenFn> (::dlsym (lib, "jack_client_open"));
            closeClient = reinterpret_cast<CloseFn> (::dlsym (lib, "jack_client_close"));
            if (openClient == nullptr || closeClient == nullptr) { openClient = nullptr; return true; }
        }

        auto answer = std::make_shared<std::atomic<int>> (0);     // 0 waiting, 1 answered
        asking = true;
        std::thread ([answer]
        {
            int status = 0;
            if (void* c = openClient ("StageRigProbe", 0x01 /* JackNoStartServer */, &status)) closeClient (c);
            answer->store (1);
            asking = false;
        }).detach();

        const auto until = Time::getMillisecondCounter() + (uint32) timeoutMs;
        while (answer->load() == 0 && Time::getMillisecondCounter() < until) Thread::sleep (10);
        const bool answered = answer->load() != 0;
        soundSystemStuck = ! answered;
        if (! answered) std::fprintf (stderr, "[audio] the sound system (PipeWire) is not answering\n");
        return answered;
    }

    /** JACK, with each device it makes wrapped in SteadyBufferDevice. */
    class SteadyBufferJack final : public AudioIODeviceType, private AudioIODeviceType::Listener
    {
    public:
        explicit SteadyBufferJack (std::unique_ptr<AudioIODeviceType> realType)
            : AudioIODeviceType (realType->getTypeName()), jack (std::move (realType))
        {
            jack->addListener (this);
        }
        ~SteadyBufferJack() override { jack->removeListener (this); }

        void scanForDevices() override
        {
            if (soundSystemAnswers (2000)) jack->scanForDevices();     // else the last lists stand
        }
        StringArray getDeviceNames (bool wantInputNames) const override { return jack->getDeviceNames (wantInputNames); }
        int getDefaultDeviceIndex (bool forInput) const override        { return jack->getDefaultDeviceIndex (forInput); }
        bool hasSeparateInputsAndOutputs() const override               { return jack->hasSeparateInputsAndOutputs(); }

        int getIndexOfDevice (AudioIODevice* device, bool asInput) const override
        {
            if (auto* d = dynamic_cast<SteadyBufferDevice*> (device)) device = &d->inner();
            return jack->getIndexOfDevice (device, asInput);
        }

        AudioIODevice* createDevice (const String& outputName, const String& inputName) override
        {
            if (! soundSystemAnswers (2000)) return nullptr;
            std::unique_ptr<AudioIODevice> d (jack->createDevice (outputName, inputName));
            return d != nullptr ? new SteadyBufferDevice (std::move (d)) : nullptr;
        }

    private:
        // JACK tells its listeners when ports are linked or unlinked; the manager listens here.
        void audioDeviceListChanged() override { callDeviceChangeListeners(); }

        std::unique_ptr<AudioIODeviceType> jack;
    };
}
#endif

void Engine::DeviceManager::createAudioDeviceTypes (OwnedArray<AudioIODeviceType>& types)
{
    AudioDeviceManager::createAudioDeviceTypes (types);
   #if JUCE_LINUX
    for (int i = 0; i < types.size(); ++i)
    {
        if (types[i]->getTypeName() == "ALSA")
        {
            std::unique_ptr<AudioIODeviceType> real (types.removeAndReturn (i));
            types.insert (i, new CardInputsAlsa (std::move (real)));
        }
        else if (types[i]->getTypeName() == "JACK")
        {
            std::unique_ptr<AudioIODeviceType> real (types.removeAndReturn (i));
            types.insert (i, new SteadyBufferJack (std::move (real)));
        }
    }
   #endif
}

/* The audio, watched so that it plays whatever happens to its devices.

   - A device switched off and on, or the sound system restarted: through
     PipeWire (JACK) a device that goes away takes StageRig's connections with it
     and nothing makes them again when it returns. StageRig keeps its choice and
     reopens the audio once the chosen devices have been back for a moment (all
     their ports there), within about three seconds of their return.
   - Audio that stops with the device still looking open (a stuck capture
     device holding up PipeWire's graph, an ALSA device whose thread gave up):
     the callbacks are counted, and three seconds without one is a stall.
   - A device back with fewer channels than chosen is not taken as back.
   - Reopening is tried again after 3, 6, 12 and then every 30 seconds while it
     fails, so a device that cannot open does not cut the sound every tick.
   - An input on another device than the output that keeps stalling the audio is
     left out, so the output plays on; Audio... or Reopen audio tries it again.
   Nothing is done while Audio... is open: the choice is being made there. */
/** Said in the status bar, and in the log with the time, for after the show. */
void Engine::sayAudio (const String& text)
{
    std::fprintf (stderr, "[audio-watch %s] %s\n", Time::getCurrentTime().toString (false, true, true, true).toRawUTF8(), text.toRawUTF8());
    listeners.call ([text] (Listener& l) { l.statusMessage (text); });
}

void Engine::watchAudioDevice()
{
    const auto now = Time::getMillisecondCounter();
    if (now - lastAudioWatch < 1500) return;
    lastAudioWatch = now;
   #if JUCE_LINUX
    /* An interface plugged in or out: ALSA's list made again, the Audio window's
       with it -- whichever type is in use, so switching to ALSA finds the
       interfaces there now, not those there when StageRig started. */
    for (auto* t : deviceManager.getAvailableDeviceTypes())
        if (t->getTypeName() == "ALSA")
            alsaCardsChanged (t, deviceManager.getCurrentAudioDeviceType() == "ALSA" ? deviceManager.getAudioDeviceSetup().outputDeviceName : String());
   #endif
    noteDeviceXruns();
    // Someone is choosing in Audio...: the choice is theirs to make.
    if (audioWindowBusy()) { audioRetries = 0; audioRetryAt = 0; return; }

    std::unique_ptr<XmlElement> saved (settings.getXmlValue ("audioDeviceState"));
    if (saved == nullptr) return;
    const auto type    = saved->getStringAttribute ("deviceType");
    const auto outName = saved->getStringAttribute ("audioOutputDeviceName");
    const auto inName  = saved->getStringAttribute ("audioInputDeviceName");
    const bool hasInput = inName.isNotEmpty() && saved->getStringAttribute ("audioDeviceInChans", "11").containsChar ('1');
    auto bitsIn = [&saved] (const char* attr, int otherwise)
    {
        if (! saved->hasAttribute (attr)) return otherwise;
        BigInteger b; b.parseString (saved->getStringAttribute (attr), 2);
        return b.countNumberOfSetBits();
    };
    // In the chosen type's list, as last looked at.
    auto listed = [this, &type] (const String& name, bool input)
    {
        for (auto* t : deviceManager.getAvailableDeviceTypes())
            if (t->getTypeName() == type) return t->getDeviceNames (input).contains (name);
        return true;
    };
    // Looked at again. ALSA's is kept current above; JACK's can be asked without disturbing the open device.
    auto lookAgain = [this, &type]
    {
        if (type == "JACK")
            for (auto* t : deviceManager.getAvailableDeviceTypes())
                if (t->getTypeName() == "JACK") t->scanForDevices();
    };

    /* An input left out because it had gone (below): seen again for two looks,
       it is wanted again, and the device is opened with it. */
    if (awayInput.isNotEmpty())
    {
        if (awayInput != inName || ! hasInput) awayInput = {};          // another choice since
        else
        {
            if (++awayLooks % 4 == 0) lookAgain();
            awayBackTicks = listed (awayInput, true) ? awayBackTicks + 1 : 0;
            if (awayBackTicks >= 2)
            {
                sayAudio ("Audio: the input " + awayInput + " is back. StageRig is opening it again.");
                awayInput = {};
                awayBackTicks = 0;
            }
        }
    }
    bool wantsInput = hasInput && inName != benchedInput && inName != awayInput;

    auto* d = deviceManager.getCurrentAudioDevice();
    const auto open = deviceManager.getAudioDeviceSetup();
    const auto calls = audioCallbacks.load (std::memory_order_relaxed);
    const bool moving = calls != lastAudioCallbacks;
    lastAudioCallbacks = calls;

    String missing;
    bool stalled = false;
    if (d == nullptr || ! d->isOpen() || ! d->isPlaying())
        missing = outName.isNotEmpty() ? outName : String ("the sound system");
    else if (type == "JACK" || type == "ALSA")
    {
        const int wantOut = jmin (bitsIn ("audioDeviceOutChans", 2), d->getOutputChannelNames().size());
        const int wantIn  = jmin (bitsIn ("audioDeviceInChans", 2), d->getInputChannelNames().size());
        if (d->getTypeName() != type)
            missing = outName;
        else if (outName.isNotEmpty() && (open.outputDeviceName != outName || d->getActiveOutputChannels().countNumberOfSetBits() < jmax (1, wantOut)))
            missing = outName;
        else if (wantsInput && (open.inputDeviceName != inName || d->getActiveInputChannels().countNumberOfSetBits() < jmax (1, wantIn)))
            missing = inName;
    }
    if (missing.isEmpty() && d != nullptr && ! moving && now - audioStartedAt.load() > 3000)
    {
        missing = open.outputDeviceName.isNotEmpty() ? open.outputDeviceName : d->getName();
        stalled = true;
    }

    // Just reopened and not yet heard from: neither back nor gone yet.
    if (missing.isEmpty() && audioGone.isNotEmpty() && ! moving) return;

    if (missing.isEmpty())
    {
        if (audioGone.isNotEmpty())
        {
            sayAudio (audioGone + " is back: sound restored.");
            // A restarted sound system has forgotten the buffer it was held at.
            if (type == "JACK" && settings.getBoolValue ("pipewireTakeover", true))
                LowLatency::forceClock (settings.getIntValue ("pipewireQuantum", 128), 48000);
        }
        audioGone = {};
        audioWhyNot = {};
        audioRetries = 0;
        audioRetryAt = 0;
        audioPresentTicks = 0;
        inputGoneTicks = 0;
        return;
    }

    // What is there now.
    lookAgain();
    const bool outPresent = outName.isEmpty() || listed (outName, false);

    /* The output is there and the input is not: the sound must not wait for a
       microphone's interface. Left out after two looks, said, and opened
       again when it is back (above). */
    bool inputJustLeft = false;
    if (outPresent && wantsInput && inName != outName && ! listed (inName, true))
    {
        if (++inputGoneTicks < 2) return;
        inputGoneTicks = 0;
        awayInput = inName;
        awayBackTicks = awayLooks = 0;
        wantsInput = false;
        inputJustLeft = true;
        audioGone = missing;
        sayAudio ("Audio: the input " + inName + " has gone, so StageRig plays on without it, and opens it again when it is back.");
    }
    else inputGoneTicks = 0;
    const bool present = outPresent && (! wantsInput || listed (inName, true));

    if (audioGone != missing)
    {
        audioGone = missing;
        // Still there and silent is stopped; not there is gone, whichever way it was noticed.
        sayAudio (stalled && present ? "Audio: " + missing + " stopped playing. StageRig is reopening it."
                                     : "Audio: " + missing + " has gone. Switch it back on: StageRig reconnects by itself.");
    }
   #if JUCE_LINUX
    if (soundSystemStuck.load())
    {
        if (! audioSaidStuck)
            sayAudio ("Audio: the computer's sound system has stopped answering. This happens when a USB audio "
                      "device locks up: switch " + (outName.isNotEmpty() ? outName : String ("your audio interface"))
                      + " off and on, or unplug it for a few seconds. StageRig carries on by itself.");
        audioSaidStuck = true;
        return;
    }
    audioSaidStuck = false;
   #endif
    audioPresentTicks = present ? audioPresentTicks + 1 : 0;
    if (! present || (! stalled && ! inputJustLeft && audioPresentTicks < 2)) return;      // back a moment, with all its ports
    if (now < audioRetryAt) return;
    // Twice at each wait, then longer: 3, 3, 6, 6, 12, 12... to thirty seconds.
    audioRetryAt = now + (uint32) jmin (30000, 3000 << jmin (audioRetries / 2, 4));
    ++audioRetries;

    // An input on another device that keeps stalling the audio is left out, so the output plays on.
    String leaveOut;
    if (stalled && audioRetries >= 3 && wantsInput && inName != outName)
    {
        benchedInput = leaveOut = inName;
        sayAudio ("Audio: the input " + inName + " keeps stopping the sound, so StageRig plays on without it. "
                  "Reopen audio in Audio... tries it again.");
    }

   #if JUCE_LINUX
    /* Open, answering, and no sound asked for after three reopenings: the
       computer's sound system has stopped running its audio (PipeWire's clock
       wedged, as after a USB device's lock-up). Opening it again does
       nothing; starting it again does, and takes a few seconds -- the JACK
       device drops out and is gone back to as for any restart. Once in ten
       minutes at most, and not when it is the output's own input holding
       things up (left out just above). */
    if (type == "JACK" && stalled && audioRetries == 4 && settings.getBoolValue ("restartSoundSystem", true)
        && (soundSystemRestartedAt == 0 || now - soundSystemRestartedAt > 600000))
    {
        soundSystemRestartedAt = now;
        ChildProcess restart;
        if (restart.start (StringArray { "systemctl", "--user", "restart", "pipewire.service", "pipewire-pulse.service", "wireplumber.service" }, 0))
        {
            sayAudio ("Audio: the computer's sound system has stopped running. StageRig is starting it again: "
                      "the sound is back in a few seconds.");
            audioRetryAt = now + 4000;
            return;
        }
        sayAudio ("Audio: the computer's sound system has stopped running, and StageRig could not start it again. "
                  "Log out and in again, or restart the computer.");
    }
    /* An interface that is there and will not play, three times running:
       asking a USB keyboard's audio over and over is what locks it up, and
       switching it off and on is what clears it. */
    if (type == "ALSA" && audioRetries == 3 && audioWhyNot.isEmpty())
        sayAudio ("Audio: " + missing + " is connected but will not play. Switch it off and on, or unplug it for a few seconds: "
                  "StageRig keeps trying.");
   #endif

    if (type != "JACK" && type != "ALSA")
    {
        restartAudioDevice();
        return;
    }
    // As chosen, channels included: the open device's own have gone with the ports.
    auto setup = open;
    setup.outputDeviceName = outName;
    setup.inputDeviceName = (wantsInput && leaveOut.isEmpty()) ? inName : String();
    if (saved->hasAttribute ("audioDeviceInChans") && setup.inputDeviceName.isNotEmpty())
    {
        setup.inputChannels.parseString (saved->getStringAttribute ("audioDeviceInChans"), 2);
        setup.useDefaultInputChannels = false;
    }
    else if (setup.inputDeviceName.isEmpty())
    {
        setup.inputChannels.clear();
        setup.useDefaultInputChannels = false;
    }
    if (saved->hasAttribute ("audioDeviceOutChans"))
    {
        setup.outputChannels.parseString (saved->getStringAttribute ("audioDeviceOutChans"), 2);
        setup.useDefaultOutputChannels = false;
    }
    else
        setup.useDefaultOutputChannels = true;
    if (saved->hasAttribute ("audioDeviceRate")) setup.sampleRate = saved->getDoubleAttribute ("audioDeviceRate");
    if (saved->hasAttribute ("audioDeviceBufferSize")) setup.bufferSize = saved->getIntAttribute ("audioDeviceBufferSize");
    if (deviceManager.getCurrentAudioDeviceType() != type)
        deviceManager.setCurrentAudioDeviceType (type, true);
    std::fprintf (stderr, "[audio-watch] reopening: out '%s', in '%s' (attempt %d)\n",
                  setup.outputDeviceName.toRawUTF8(), setup.inputDeviceName.toRawUTF8(), audioRetries);
    deviceManager.closeAudioDevice();
    // Why it did not open, said once in a player's words (the ALSA type gives them).
    if (const auto why = deviceManager.setAudioDeviceSetup (setup, true); why.isNotEmpty() && why != audioWhyNot)
    {
        audioWhyNot = why;
        if (type == "ALSA") sayAudio ("Audio: " + why);
    }
    lastAudioWatch = Time::getMillisecondCounter();
}

/** The device's underruns since the last look (the buffer ran dry: too small
    for a busy moment), counted for the status bar and written in the log, at
    most a line every ten seconds, for after the show. */
void Engine::noteDeviceXruns()
{
    auto* d = deviceManager.getCurrentAudioDevice();
    if (d == nullptr) { lastXruns = deviceXruns.load(); lastDeviceError = {}; return; }
    /* An error the device met and played on from (JUCE's ALSA keeps the last
       one for good; an underrun's is "Broken pipe"): written down once. */
    if (const auto err = d->getLastError(); err != lastDeviceError)
    {
        lastDeviceError = err;
        if (err.isNotEmpty() && d->isPlaying())
            std::fprintf (stderr, "[audio-watch %s] %s reported \"%s\" and plays on\n",
                          Time::getCurrentTime().toString (false, true, true, true).toRawUTF8(), d->getName().toRawUTF8(), err.toRawUTF8());
    }
    const int n = deviceXruns.load();
    const auto now = Time::getMillisecondCounter();
    if (n > lastXruns && now - xrunsSaidAt > 10000)
    {
        std::fprintf (stderr, "[audio-watch %s] %d underrun%s on %s (%d samples at %.0f Hz; the device's own count %d): %d since the start\n",
                      Time::getCurrentTime().toString (false, true, true, true).toRawUTF8(), n - lastXruns, n - lastXruns == 1 ? "" : "s",
                      d->getName().toRawUTF8(), d->getCurrentBufferSizeSamples(), d->getCurrentSampleRate(), d->getXRunCount(), n);
        lastXruns = n;
        xrunsSaidAt = now;
    }
}

void Engine::changeListenerCallback (ChangeBroadcaster*)
{
    // Off ALSA: the sound cards StageRig asked PipeWire for are its again.
    if (deviceManager.getCurrentAudioDeviceType() != "ALSA") DeviceReservation::releaseAll();

    /* The choice is kept as made. Saved while someone is choosing in Audio...
       (a click there within the minute), or when StageRig opens channels for an input or output itself with
       the chosen devices all there. Never while one has gone: the open device
       has lost those channels, or JUCE has fallen back to other devices, and
       saving either would forget the choice the watchdog goes back to. */
    bool complete = deviceManager.getCurrentAudioDevice() != nullptr;
    if (complete && ! audioWindowBusy())
    {
        std::unique_ptr<XmlElement> saved (settings.getXmlValue ("audioDeviceState"));
        const auto s = deviceManager.getAudioDeviceSetup();
        if (saved != nullptr)
        {
            const auto type = saved->getStringAttribute ("deviceType");
            if (type != deviceManager.getCurrentAudioDeviceType()
                || saved->getStringAttribute ("audioOutputDeviceName") != s.outputDeviceName
                || (saved->getStringAttribute ("audioInputDeviceName").isNotEmpty()
                    && saved->getStringAttribute ("audioInputDeviceName") != s.inputDeviceName))
                complete = false;       // other devices than the chosen ones: a fallback
            // Fewer channels open than chosen: a device back only in part.
            if (auto* d = deviceManager.getCurrentAudioDevice(); complete && d != nullptr)
            {
                auto chosenBits = [&saved] (const char* attr, int otherwise)
                {
                    if (! saved->hasAttribute (attr)) return otherwise;
                    BigInteger b; b.parseString (saved->getStringAttribute (attr), 2);
                    return b.countNumberOfSetBits();
                };
                if (d->getActiveOutputChannels().countNumberOfSetBits() < jmin (chosenBits ("audioDeviceOutChans", 1), d->getOutputChannelNames().size())
                    || (s.inputDeviceName.isNotEmpty()
                        && d->getActiveInputChannels().countNumberOfSetBits() < jmin (chosenBits ("audioDeviceInChans", 0), d->getInputChannelNames().size())))
                    complete = false;
            }
        }
        if (complete && deviceManager.getCurrentAudioDeviceType() == "JACK")
            for (auto* t : deviceManager.getAvailableDeviceTypes())
                if (t->getTypeName() == "JACK")
                {
                    t->scanForDevices();
                    if ((s.outputDeviceName.isNotEmpty() && ! t->getDeviceNames (false).contains (s.outputDeviceName))
                        || (s.inputDeviceName.isNotEmpty() && ! t->getDeviceNames (true).contains (s.inputDeviceName)))
                        complete = false;
                }
    }
    if (complete)
        if (auto xml = deviceManager.createStateXml())
        {
            // Which MIDI inputs are open is the engine's to say (openMidiDevices), never JUCE's from here.
            xml->deleteAllChildElementsWithTagName ("MIDIINPUT");
            settings.setValue ("audioDeviceState", xml.get());
        }
    settings.saveIfNeeded();

    // Said once, where it will be seen: the Audio window repeats it for as long as it is true.
    const auto warnings = deviceWarnings();
    if (warnings != lastDeviceWarnings)
    {
        lastDeviceWarnings = warnings;
        if (! warnings.isEmpty())
            listeners.call ([w = warnings.joinIntoString ("  ")] (Listener& l) { l.statusMessage ("Audio: " + w); });
    }
}

/** Whether two of PipeWire's device names are the same card: "Saffire 6 Analog
    Stereo" and "Saffire 6 Analog Surround 4.0" are, the name less its last
    "Analog ..." or "Digital ..." part being the card. */
static bool sameCard (const String& a, const String& b)
{
    auto card = [] (const String& n)
    {
        for (auto* kind : { " Analog ", " Digital ", " Pro " })
            if (const int i = n.lastIndexOf (kind); i > 0) return n.substring (0, i);
        return n;
    };
    return card (a) == card (b);
}

StringArray Engine::deviceWarnings() const
{
    StringArray out;
    auto* d = deviceManager.getCurrentAudioDevice();
    if (d == nullptr) return out;
    const auto s = deviceManager.getAudioDeviceSetup();

    /* A hardware interface driven directly, with nothing between it and this
       process: the two settings that are easy to pick and sound like a broken
       cable. 48 samples is one USB packet -- a full-speed interface cannot
       keep that up -- and an input on another device is a second clock, so
       the two drift and the stream resyncs with a crackle. */
    if (! audioThreadRealtime.load())
        out.add ("the audio thread could not be made realtime: " + LowLatency::realtimeSchedulingHint());
    if (d->getTypeName() == "ALSA" && d->getCurrentBufferSizeSamples() < 128)
        out.add (String (d->getCurrentBufferSizeSamples()) + "-sample buffer on a direct ALSA device: expect dropouts and crackle. 256 is safe on a USB interface, 128 on a good PCI one.");

    /* Two devices are two clocks. Directly on ALSA they drift apart; through
       PipeWire (JACK) one drives and the other is resampled to follow it, which a
       USB device, a keyboard's audio above all, does with crackles at small
       buffers. Both names are a whole device ("Saffire 6 Analog Stereo" and
       "... Analog Surround 4.0" are the same card's two sides, so compare the
       card part too). */
    if (s.inputDeviceName.isNotEmpty() && s.inputChannels.countNumberOfSetBits() > 0
        && s.inputDeviceName != s.outputDeviceName
        && ! (d->getTypeName() == "JACK" && sameCard (s.inputDeviceName, s.outputDeviceName)))
        out.add ("input (" + s.inputDeviceName + ") and output (" + s.outputDeviceName + ") are different devices, so two clocks "
                 "have to be kept together and the audio can crackle. Use one interface for both, or choose no input "
                 "if no audio input is used.");
    return out;
}

void Engine::notifyContent (int inputIndex, int program)
{
    if (isOutputAddress (inputIndex))
        listeners.call ([] (Listener& l) { l.outputsChanged(); });
    else
        listeners.call ([=] (Listener& l) { l.programContentChanged (inputIndex, program); });
}

//==============================================================================
// Setup
//==============================================================================
void Engine::loadSetup (Setup newSetup)
{
    {
        std::unique_ptr<SongPlayer> gone;
        { const ScopedLock sl (lock); gone = std::move (player); }
        trackSong = -1;
    }
    teardownRuntimes (false);     // their state would be thrown away with the setup on the next line
    setup = std::move (newSetup);
    songInPlay = -1;                        // the set's position says which song, until one is selected
    currentScene = -1;
    // A setup with an audio input needs the device's inputs; a keyboard rig does not pay for them.
    for (auto& in : setup.inputs)
        if (in.isAudio()) { ensureAudioInputsOpen(); break; }
    int outChannels = 0;
    for (auto& o : setup.outputs) outChannels = jmax (outChannels, o.left + 1, o.right + 1);
    if (outChannels > 2) ensureAudioOutputsOpen (outChannels);
    // The MIDI and audio threads read the pedals' controllers from their own copies.
    for (int p = 0; p < numPedals; ++p)
    {
        pedalField ((Pedal) p) = jlimit (0, 127, pedalField ((Pedal) p));
        pedalCC[(size_t) p].store (pedalField ((Pedal) p), std::memory_order_relaxed);
    }
    pedalLearning.store (-1);
    tapTimes.clear();
    rebuildRuntimes();
    /* The song the set was on gets its track back once setSetupFile() says
       where the files are -- the caller sets that right after this. */
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::setSetupFile (const File& f)
{
    const bool moved = f != setupFile;
    setupFile = f;
    if (moved || (trackSong < 0 && player == nullptr)) armCurrentSong();
}

void Engine::armCurrentSong()
{
    /* Ready to play, as it was when the setup was saved: the track armed,
       not started. The programs are already where the file left them. */
    if (setup.currentSet < 0 || setup.currentSet >= (int) setup.sets.size()) return;
    const auto& st = setup.sets[(size_t) setup.currentSet];
    if (setup.currentSongInSet < 0 || setup.currentSongInSet >= (int) st.songs.size()) return;
    const int song = st.songs[(size_t) setup.currentSongInSet];
    if (song >= 0 && song < (int) setup.songs.size() && ! setup.songs[(size_t) song].stems.empty())
        armTrack (song);
}

Setup Engine::captureSetup (int budgetMs, bool onlyChanged)
{
    const auto deadline = Time::getMillisecondCounter() + (uint32) jmax (0, budgetMs);
    for (int i = 0; i < (int) runtimes.size(); ++i)
        for (auto& [prog, rt] : runtimes[(size_t) i]->loaded)
        {
            int timeout = -1;
            if (budgetMs >= 0)
            {
                const auto now = Time::getMillisecondCounter();
                if (now >= deadline) continue;            // out of time: keep what we have
                timeout = (int) jmin ((uint32) 30000, deadline - now);
            }
            captureProgramState (i, *rt, timeout, onlyChanged);
        }
    for (int o = 0; o < (int) outputs.size(); ++o)
    {
        int timeout = -1;
        if (budgetMs >= 0)
        {
            const auto now = Time::getMillisecondCounter();
            if (now >= deadline) break;
            timeout = (int) jmin ((uint32) 30000, deadline - now);
        }
        captureProgramState (outputAddress (o), outputs[(size_t) o]->chain, timeout, onlyChanged);
    }
    return setup;
}

void Engine::setPreloadAllPrograms (bool b)  { setup.preloadAllPrograms = b; }
void Engine::setReleaseTailSeconds (double s) { setup.releaseTailSeconds = jlimit (0.0, 30.0, s); }
void Engine::setHoldAcrossChange (bool b)     { setup.holdAcrossChange = b; }

//==============================================================================
void Engine::setTempoBpm (double bpm)
{
    const auto clamped = jlimit (20.0, 300.0, bpm);
    if (std::abs (clamped - setup.tempoBpm) < 0.001) return;
    setup.tempoBpm = clamped;
    tempoAtomic.store (clamped);
    if (link != nullptr && link->isEnabled())
    {
        // Told to the others, unless it came from them (then Link has it already).
        auto st = link->captureAppSessionState();
        if (std::abs (st.tempo() - clamped) > 0.01)
        {
            st.setTempo (clamped, link->clock().micros());
            link->commitAppSessionState (st);
        }
    }

    // Every plugin, not just the ones sounding: a preloaded program should not
    // come in at the old tempo when it is selected.
    for (auto& in : runtimes)
    {
        if (in == nullptr) continue;
        for (auto& [prog, rt] : in->loaded)
        {
            for (auto& s : rt->slots)
            {
                if (s->plugin != nullptr) s->plugin->setTempo (clamped, 4, 4);
                for (auto* ch : { &s->midiEffects, &s->effects }) for (auto& e : *ch)
                    if (e->plugin != nullptr) e->plugin->setTempo (clamped, 4, 4);
            }
            for (auto& e : rt->effects)
                if (e->plugin != nullptr) e->plugin->setTempo (clamped, 4, 4);
        }
    }
    for (auto& o : outputs)
        for (auto& e : o->chain.effects)
            if (e->plugin != nullptr) e->plugin->setTempo (clamped, 4, 4);

    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

double Engine::tapTempo()
{
    const auto now = Time::getMillisecondCounterHiRes();

    /* A gap this long means the last tap was part of a different attempt, not
       this one. 2 seconds is 30 bpm: slower than anything anyone taps, so no
       real tempo is lost by starting over. */
    constexpr double kGapMs = 2000.0;
    if (! tapTimes.empty() && now - tapTimes.back() > kGapMs)
        tapTimes.clear();

    tapTimes.push_back (now);

    // Keep a short window: the player speeding up should be followed, not
    // averaged away against taps from ten seconds ago.
    constexpr size_t kMaxTaps = 8;
    if (tapTimes.size() > kMaxTaps)
        tapTimes.erase (tapTimes.begin(), tapTimes.begin() + (int) (tapTimes.size() - kMaxTaps));

    // One tap sets nothing: two is the first interval worth believing.
    if (tapTimes.size() < 2) return 0.0;

    const double span = tapTimes.back() - tapTimes.front();
    const double perBeat = span / (double) (tapTimes.size() - 1);
    if (perBeat <= 0.0) return 0.0;

    const double bpm = 60000.0 / perBeat;
    if (bpm < 20.0 || bpm > 300.0) return 0.0;      // a slip, not a tempo

    setTempoBpm (bpm);
    return setup.tempoBpm;
}

void Engine::resetTapTempo() { tapTimes.clear(); }

String Engine::openPluginWindow (int inputIndex, int program, int slot, int effect, String* titleOut)
{
    auto* plugin = getPlugin (inputIndex, program, slot, effect);
    if (plugin == nullptr || ! plugin->isAlive()) return "That plugin is not running. Use Reload to restart it.";

    String title = plugin->getName();
    if (isOutputAddress (inputIndex))
    {
        const int o = outputFromAddress (inputIndex);
        if (o >= 0 && o < (int) setup.outputs.size())
            title = "Output " + setup.outputs[(size_t) o].name + " / effect " + String (effect + 1) + " / " + plugin->getName();
    }
    else if (validInput (inputIndex) && program >= 0 && program < InputDef::numPrograms)
    {
        const auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
        title = setup.inputs[(size_t) inputIndex].name + " / " + String (program).paddedLeft ('0', 3)
              + (def.name.isNotEmpty() ? "  " + def.name : String()) + " / " + plugin->getName();
    }
    if (titleOut != nullptr) *titleOut = title;
    if (plugin->isEditorOpen()) return {};
    return plugin->showEditor (title) ? String() : plugin->getName() + ": could not open its window (" + plugin->getLastError() + ")";
}

//==============================================================================
// The rig's pedals: one table, one learn, one press.
//==============================================================================
const char* Engine::pedalName (Pedal p)
{
    switch (p)
    {
        case Pedal::tapTempo:    return "Tap tempo";
        case Pedal::nextSong:    return "Next song";
        case Pedal::prevSong:    return "Previous song";
        case Pedal::transport:   return "Start / stop the track";
        case Pedal::section:     return "Next section, next scene, or release a vamp";
        case Pedal::looper:      return "Record, play, stop";
        case Pedal::looperClear: return "Clear the loop";
        case Pedal::count:       break;
    }
    return "";
}

int& Engine::pedalField (Pedal p)
{
    switch (p)
    {
        case Pedal::tapTempo:    return setup.tapTempoCC;
        case Pedal::nextSong:    return setup.nextSongCC;
        case Pedal::prevSong:    return setup.prevSongCC;
        case Pedal::transport:   return setup.transportCC;
        case Pedal::section:     return setup.sectionCC;
        case Pedal::looper:      return setup.looperCC;
        case Pedal::looperClear: return setup.looperClearCC;
        case Pedal::count:       break;
    }
    return setup.tapTempoCC;
}

int Engine::getPedalCC (Pedal p) const
{
    return p < Pedal::count ? pedalCC[(size_t) p].load (std::memory_order_relaxed) : 0;
}

void Engine::setPedalCC (Pedal p, int cc)
{
    if (p >= Pedal::count) return;
    cc = jlimit (0, 127, cc);
    // One controller, one job: the pedal or output volume that had it lets go.
    if (cc > 0)
    {
        for (int o = 0; o < numPedals; ++o)
            if (o != (int) p && pedalCC[(size_t) o].load() == cc) { pedalField ((Pedal) o) = 0; pedalCC[(size_t) o].store (0); }
        for (int o = 0; o < (int) setup.outputs.size(); ++o)
            if (setup.outputs[(size_t) o].volumeCC == cc) { setup.outputs[(size_t) o].volumeCC = 0; outputs[(size_t) o]->volumeCC.store (0); }
    }
    pedalField (p) = cc;
    pedalCC[(size_t) p].store (cc, std::memory_order_relaxed);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::armPedalLearn (Pedal p)
{
    if (p >= Pedal::count) return;
    outputLearn.store (-1);
    pedalLearning.store ((int) p);
}

void Engine::cancelPedalLearns()
{
    pedalLearning.store (-1);
    outputLearn.store (-1);
    surfaceLearnInput.store (-1);
}

void Engine::pedalPressed (int i, InputRuntime& in, Pedal p)
{
    switch (p)
    {
        case Pedal::tapTempo:    postEvent ({ Event::tapTempo, i, 0, 0, 0, 0 }); break;
        case Pedal::nextSong:    postEvent ({ Event::songStep, i, +1, 0, 0, 0 }); break;
        case Pedal::prevSong:    postEvent ({ Event::songStep, i, -1, 0, 0, 0 }); break;
        case Pedal::transport:   postEvent ({ Event::transport, i, 0, 0, 0, 0 }); break;
        case Pedal::section:     postEvent ({ Event::sectionPedal, i, 0, 0, 0, 0 }); break;
        // The looper's pedals act on the keyboard they are pressed on, straight away.
        case Pedal::looper:      in.loop.command.store (1); break;
        case Pedal::looperClear: in.loop.command.store (2); break;
        case Pedal::count:       break;
    }
}

void Engine::rebuildRuntimes()
{
    buildOutputs();
    tempoAtomic.store (setup.tempoBpm);
    resolveClockPorts();
    for (int i = 0; i < (int) setup.inputs.size(); ++i)
        runtimes.push_back (makeInputRuntime (setup.inputs[(size_t) i]));

    openMidiDevices();

    for (int i = 0; i < (int) setup.inputs.size(); ++i)
    {
        auto* prog = ensureProgramLoaded (i, setup.inputs[(size_t) i].currentProgram);
        const ScopedLock sl (lock);
        runtimes[(size_t) i]->active = prog;
    }
    ensureLentLoaded();
}

void Engine::teardownRuntimes (bool captureFirst, int budgetMs)
{
    // Take everything away from the audio thread first.
    std::vector<std::unique_ptr<InputRuntime>> old;
    std::vector<std::unique_ptr<OutputRuntime>> oldOutputs;
    {
        const ScopedLock sl (lock);
        old = std::move (runtimes);
        runtimes.clear();
        oldOutputs = std::move (outputs);
        outputs.clear();
    }

    const auto deadline = Time::getMillisecondCounter() + (uint32) jmax (0, budgetMs);
    auto remaining = [&] { return budgetMs < 0 ? -1 : (int) jmax (0, (int) (deadline - Time::getMillisecondCounter())); };

    for (int i = 0; i < (int) old.size(); ++i)
    {
        auto& in = *old[(size_t) i];
        in.active = nullptr;
        in.releasing.clear();
        if (captureFirst)
            for (auto& [prog, rt] : in.loaded)
                if (i < (int) setup.inputs.size())
                    captureProgramState (i, *rt, remaining());
    }
    if (captureFirst)
        for (int o = 0; o < (int) oldOutputs.size() && o < (int) setup.outputs.size(); ++o)
            captureProgramState (outputAddress (o), oldOutputs[(size_t) o]->chain, remaining());

    /* Two phases. Every host is told to quit first, so they all wind down at
       the same time; then each is reaped with what patience is left. One
       after another, each with its own full wait, seventeen hosts of which
       three had stopped answering took minutes to leave. */
    for (auto& in : old)
        for (auto& [prog, rt] : in->loaded)
        {
            for (auto& s : rt->slots)
            {
                for (auto* ch : { &s->midiEffects, &s->effects }) for (auto& e : *ch) if (e->plugin != nullptr) e->plugin->requestQuit();
                if (s->plugin != nullptr) s->plugin->requestQuit();
            }
            for (auto& e : rt->effects) if (e->plugin != nullptr) e->plugin->requestQuit();
        }
    for (auto& o : oldOutputs)
        for (auto& e : o->chain.effects) if (e->plugin != nullptr) e->plugin->requestQuit();
    const int patience = budgetMs < 0 ? 3000 : jmin (3000, remaining());
    for (auto& in : old)
        for (auto& [prog, rt] : in->loaded)
            destroyProgramPlugins (*rt, patience);
    for (auto& o : oldOutputs)
        destroyProgramPlugins (o->chain, patience);
    // Every reaper gets its patience and then a SIGKILL; a little more covers the kill itself.
    RemotePlugin::waitForReapers (patience + 2000);
}

//==============================================================================
// Inputs
//==============================================================================
std::unique_ptr<Engine::InputRuntime> Engine::makeInputRuntime (const InputDef& def)
{
    // The one way an input's runtime is made, at load and when one is added
    // while running: everything the audio thread needs is sized here, never there.
    auto rt = std::make_unique<InputRuntime>();
    rt->loop.events.reserve (16384);          // a few minutes of playing
    rt->collector.reset (sampleRate);
    // Room enough that the audio thread never has to ask for more: a backing track's
    // parts all starting at a seam, with their notes-off, come to well over a thousand bytes each.
    rt->trackMidi.ensureSize (32768);
    rt->holdBlock.ensureSize (8192);
    rt->shifted.ensureSize (4096);
    rt->block.ensureSize (8192);              // the keys and a loop playing round them
    rt->sentMidi.ensureSize (2048);
    rt->received.ensureSize (2048);
    applyInputDef (*rt, def);
    return rt;
}

void Engine::applyInputDef (InputRuntime& rt, const InputDef& def)
{
    rt.channel.store (def.channel);
    rt.respondToProgramChange.store (def.respondToProgramChange);
    rt.programChangeChannel.store (def.programChangeChannel);
    rt.audio.store (def.isAudio());
    rt.track.store (def.isTrack());
    rt.audioLeft.store (def.audioLeft);
    rt.audioRight.store (def.audioRight);
    rt.output.store (def.output);
    const auto heard = def.controls.heardCC (rt.autoKnobs.load());
    for (int k = 0; k < SurfaceControls::numKnobs; ++k)
    {
        rt.knobCC[(size_t) k].store (def.controls.knobCC[(size_t) k]);
        rt.heardCC[(size_t) k].store (heard[(size_t) k]);
    }
    rt.knobsRelative.store (def.controls.knobsRelative);
    for (int a = 0; a < SurfaceControls::numActions; ++a) rt.buttonCC[(size_t) a].store (SurfaceControls::ccOf (def.controls.buttons[(size_t) a]));
}

void Engine::setInputControls (int inputIndex, const SurfaceControls& c)
{
    if (! validInput (inputIndex)) return;
    setup.inputs[(size_t) inputIndex].controls = c;
    applyInputDef (*runtimes[(size_t) inputIndex], setup.inputs[(size_t) inputIndex]);
}

ProgramDef* Engine::defFor (int inputIndex, int program)
{
    if (isOutputAddress (inputIndex))
    {
        const int o = outputFromAddress (inputIndex);
        return o >= 0 && o < (int) setup.outputs.size() ? &setup.outputs[(size_t) o].chain : nullptr;
    }
    return validInput (inputIndex) && validProgram (program) ? &setup.inputs[(size_t) inputIndex].programs[(size_t) program] : nullptr;
}

//==============================================================================
// Outputs
//==============================================================================
void Engine::buildOutputs()
{
    std::vector<std::unique_ptr<OutputRuntime>> built;
    for (int o = 0; o < (int) setup.outputs.size(); ++o)
    {
        const auto& def = setup.outputs[(size_t) o];
        auto rt = std::make_unique<OutputRuntime>();
        rt->chain.program = 0;
        rt->chain.mix.setSize (2, blockSize);
        rt->chain.programMidi.ensureSize (64);
        rt->mix.setSize (2, blockSize);
        rt->gain.store (Decibels::decibelsToGain (def.gainDb, -60.0f));
        rt->left.store (def.left);
        rt->right.store (def.right);
        rt->volumeCC.store (def.volumeCC);
        for (int e = 0; e < (int) def.chain.effects.size(); ++e)
            rt->chain.effects.push_back (buildEffect (outputAddress (o), 0, -1, e, def.chain.effects[(size_t) e]));
        built.push_back (std::move (rt));
    }
    const ScopedLock sl (lock);
    outputs = std::move (built);
}

int Engine::addOutput (const String& name, int left, int right)
{
    OutputDef def;
    def.name = name;
    def.left = jlimit (0, 63, left);
    def.right = jlimit (-1, 63, right);
    setup.outputs.push_back (def);
    const int idx = (int) setup.outputs.size() - 1;

    auto rt = std::make_unique<OutputRuntime>();
    rt->chain.program = 0;
    rt->chain.mix.setSize (2, blockSize);
    rt->chain.programMidi.ensureSize (64);
    rt->mix.setSize (2, blockSize);
    rt->left.store (def.left);
    rt->right.store (def.right);
    {
        const ScopedLock sl (lock);
        outputs.push_back (std::move (rt));
    }
    ensureAudioOutputsOpen (jmax (def.left, def.right) + 1);
    listeners.call ([] (Listener& l) { l.outputsChanged(); });
    return idx;
}

void Engine::removeOutput (int output)
{
    if (output < 0 || output >= (int) setup.outputs.size() || setup.outputs.size() <= 1) return;

    std::unique_ptr<OutputRuntime> gone;
    {
        const ScopedLock sl (lock);
        gone = std::move (outputs[(size_t) output]);
        outputs.erase (outputs.begin() + output);
        // Inputs on it land on the first output; those on later outputs follow their output's new index.
        for (int i = 0; i < (int) runtimes.size(); ++i)
        {
            auto& def = setup.inputs[(size_t) i];
            if (def.output == output) def.output = 0;
            else if (def.output > output) --def.output;
            runtimes[(size_t) i]->output.store (def.output);
        }
        for (int o = output; o < (int) outputs.size(); ++o)
            for (auto& e : outputs[(size_t) o]->chain.effects)
                e->inputIndex.store (outputAddress (o));
    }
    destroyProgramPlugins (gone->chain);
    setup.outputs.erase (setup.outputs.begin() + output);
    listeners.call ([] (Listener& l) { l.outputsChanged(); l.setupChanged(); });
}

void Engine::setOutputName (int output, const String& name)
{
    if (output < 0 || output >= (int) setup.outputs.size()) return;
    setup.outputs[(size_t) output].name = name;
}

void Engine::setOutputChannels (int output, int left, int right)
{
    if (output < 0 || output >= (int) setup.outputs.size()) return;
    auto& def = setup.outputs[(size_t) output];
    def.left = jlimit (0, 63, left);
    def.right = jlimit (-1, 63, right);
    outputs[(size_t) output]->left.store (def.left);
    outputs[(size_t) output]->right.store (def.right);
    ensureAudioOutputsOpen (jmax (def.left, def.right) + 1);
    listeners.call ([] (Listener& l) { l.outputsChanged(); });
}

void Engine::setOutputGainDb (int output, float dB)
{
    if (output < 0 || output >= (int) setup.outputs.size()) return;
    dB = jlimit (-60.0f, 12.0f, dB);
    setup.outputs[(size_t) output].gainDb = dB;
    outputs[(size_t) output]->gain.store (Decibels::decibelsToGain (dB, -60.0f));
}

void Engine::setOutputVolumeCC (int output, int cc)
{
    if (output < 0 || output >= (int) setup.outputs.size()) return;
    cc = jlimit (0, 127, cc);
    // One controller, one job: a pedal or another output that had it lets go.
    if (cc > 0)
    {
        for (int p = 0; p < numPedals; ++p)
            if (pedalCC[(size_t) p].load() == cc) { pedalField ((Pedal) p) = 0; pedalCC[(size_t) p].store (0); }
        for (int o = 0; o < (int) setup.outputs.size(); ++o)
            if (o != output && setup.outputs[(size_t) o].volumeCC == cc) { setup.outputs[(size_t) o].volumeCC = 0; outputs[(size_t) o]->volumeCC.store (0); }
    }
    setup.outputs[(size_t) output].volumeCC = cc;
    outputs[(size_t) output]->volumeCC.store (cc);
    listeners.call ([] (Listener& l) { l.outputsChanged(); l.setupChanged(); });
}

void Engine::armOutputVolumeLearn (int output)
{
    pedalLearning.store (-1);                                   // one learn at a time
    outputLearn.store (output >= 0 && output < (int) setup.outputs.size() ? output : -1);
}

void Engine::setInputOutput (int inputIndex, int output)
{
    if (! validInput (inputIndex) || output < 0 || output >= (int) setup.outputs.size()) return;
    setup.inputs[(size_t) inputIndex].output = output;
    runtimes[(size_t) inputIndex]->output.store (output);
    listeners.call ([] (Listener& l) { l.outputsChanged(); });
}

StringArray Engine::getAudioOutputChannelNames() const
{
    StringArray out;
    if (auto* d = deviceManager.getCurrentAudioDevice())
    {
        const auto names = d->getOutputChannelNames();
        const auto active = d->getActiveOutputChannels();
        for (int i = 0; i < names.size(); ++i)
            if (active[i]) out.add (names[i]);
    }
    return out;
}

bool Engine::ensureAudioOutputsOpen (int channelsNeeded)
{
    auto* d = deviceManager.getCurrentAudioDevice();
    if (d == nullptr) return false;
    if (d->getActiveOutputChannels().countNumberOfSetBits() >= channelsNeeded) return true;
    if (channelsNeeded > d->getOutputChannelNames().size()) return false;     // the device has no such channel

    auto s = deviceManager.getAudioDeviceSetup();
    s.useDefaultOutputChannels = false;
    s.outputChannels.clear();
    s.outputChannels.setRange (0, channelsNeeded, true);
    const auto err = deviceManager.setAudioDeviceSetup (s, true);
    const auto open = getAudioOutputChannelNames();
    std::fprintf (stderr, "[audio] opening %d output channels on '%s': %s, %d open\n", channelsNeeded,
                  s.outputDeviceName.toRawUTF8(), err.isEmpty() ? "ok" : err.toRawUTF8(), open.size());
    return err.isEmpty() && open.size() >= channelsNeeded;
}

int Engine::addInput (const String& name, InputDef::Kind kind)
{
    InputDef def;
    def.name = name;
    def.kind = kind;
    if (kind == InputDef::Kind::audio)
    {
        /* Whatever the device has open. Stereo when it has two, mono when
           one; the inputs panel is where to pick otherwise. */
        const int open = getAudioInputChannelNames().size();
        def.audioLeft = 0;
        def.audioRight = open >= 2 ? 1 : -1;
        ensureAudioInputsOpen();
    }
    setup.inputs.push_back (def);

    auto rt = makeInputRuntime (def);
    {
        const ScopedLock sl (lock);
        runtimes.push_back (std::move (rt));
    }
    const int idx = (int) setup.inputs.size() - 1;
    auto* prog = ensureProgramLoaded (idx, 0);
    {
        const ScopedLock sl (lock);
        runtimes[(size_t) idx]->active = prog;
    }
    listeners.call ([] (Listener& l) { l.setupChanged(); });
    return idx;
}

void Engine::removeInput (int inputIndex)
{
    if (! validInput (inputIndex)) return;

    std::unique_ptr<InputRuntime> in;
    {
        const ScopedLock sl (lock);
        in = std::move (runtimes[(size_t) inputIndex]);
        runtimes.erase (runtimes.begin() + inputIndex);
        // Nodes report their input index for "touched" events; fix the survivors.
        for (int i = inputIndex; i < (int) runtimes.size(); ++i)
            for (auto& [p, rt] : runtimes[(size_t) i]->loaded)
            {
                for (auto& s : rt->slots)
                {
                    s->inputIndex.store (i);
                    for (auto* ch : { &s->midiEffects, &s->effects }) for (auto& e : *ch) e->inputIndex.store (i);
                }
                for (auto& e : rt->effects) e->inputIndex.store (i);
            }
    }

    for (auto& [p, rt] : in->loaded)
        destroyProgramPlugins (*rt);

    setup.inputs.erase (setup.inputs.begin() + inputIndex);
    // Songs hold one program per input, by index: the entry for this input goes, the rest shift.
    for (auto& song : setup.songs)
    {
        if ((size_t) inputIndex < song.programs.size())
            song.programs.erase (song.programs.begin() + inputIndex);
        /* Section and scene actions name inputs by index too: those for this
           input go, and those for later inputs follow it down -- else a scene
           would put the wrong keyboard on a program. */
        for (auto& m : song.markers)
        {
            auto& acts = m.actions;
            acts.erase (std::remove_if (acts.begin(), acts.end(), [inputIndex] (const ActionDef& a)
                { return (a.kind == ActionDef::Kind::program || a.kind == ActionDef::Kind::effect) && a.input == inputIndex; }), acts.end());
            for (auto& a : acts)
                if ((a.kind == ActionDef::Kind::program || a.kind == ActionDef::Kind::effect) && a.input > inputIndex) --a.input;
        }
    }
    // Slots that played this input's sounds, or a later input's, follow the renumbering.
    for (auto& other : setup.inputs)
        for (auto& prog : other.programs)
            for (auto& sd : prog.slots)
                if (sd.isInput())
                {
                    if (sd.targetInput == inputIndex)     { sd.targetInput = -1; sd.targetInputName = "(removed input)"; }
                    else if (sd.targetInput > inputIndex) --sd.targetInput;
                }
    {
        const ScopedLock sl (lock);
        for (auto& r : runtimes)
            for (auto& [p, rt] : r->loaded)
                for (auto& sd : rt->slots)
                    if (sd->isInputSlot)
                    {
                        const int t = sd->targetInput.load();
                        if (t == inputIndex) sd->targetInput.store (-1);
                        else if (t > inputIndex) sd->targetInput.store (t - 1);
                    }
    }
    openMidiDevices();
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::setInputName (int inputIndex, const String& name)
{
    if (! validInput (inputIndex)) return;
    setup.inputs[(size_t) inputIndex].name = name;
    for (auto& in : setup.inputs)
        for (auto& prog : in.programs)
            for (auto& sd : prog.slots)
                if (sd.isInput() && sd.targetInput == inputIndex) sd.targetInputName = name;
}

void Engine::setInputMidiDevice (int inputIndex, const MidiDeviceInfo& info)
{
    if (! validInput (inputIndex)) return;
    auto& def = setup.inputs[(size_t) inputIndex];
    def.midiDeviceIdentifier = info.identifier;
    def.midiDeviceName = info.name;
    openMidiDevices();
}

void Engine::setInputChannel (int inputIndex, int channel)
{
    if (! validInput (inputIndex)) return;
    setup.inputs[(size_t) inputIndex].channel = jlimit (0, 16, channel);
    runtimes[(size_t) inputIndex]->channel.store (channel);
}

void Engine::setInputAudioChannels (int inputIndex, int left, int right)
{
    if (! validInput (inputIndex)) return;
    auto& def = setup.inputs[(size_t) inputIndex];
    def.audioLeft = jlimit (0, 63, left);
    def.audioRight = jlimit (-1, 63, right);
    runtimes[(size_t) inputIndex]->audioLeft.store (def.audioLeft);
    runtimes[(size_t) inputIndex]->audioRight.store (def.audioRight);
}

void Engine::setProgramInputGainDb (int inputIndex, int program, float dB)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    dB = jlimit (-60.0f, 24.0f, dB);
    setup.inputs[(size_t) inputIndex].programs[(size_t) program].inputGainDb = dB;
    if (auto* rt = getLoaded (inputIndex, program))
        rt->inputGain.store (Decibels::decibelsToGain (dB, -60.0f));
}

Engine::InputLevel Engine::getInputLevel (int inputIndex) const
{
    InputLevel l;
    if (! validInput (inputIndex) || inputIndex >= (int) runtimes.size()) return l;
    const auto& rt = *runtimes[(size_t) inputIndex];
    if (! rt.audio.load (std::memory_order_relaxed)) return l;
    l.left = rt.levelL.load (std::memory_order_relaxed);
    l.right = rt.levelR.load (std::memory_order_relaxed);
    const auto clipAt = rt.clipMs.load (std::memory_order_relaxed);
    l.clipped = clipAt != 0 && Time::getMillisecondCounter() - clipAt < 2000;
    return l;
}

StringArray Engine::getAudioInputChannelNames() const
{
    StringArray out;
    if (auto* d = deviceManager.getCurrentAudioDevice())
    {
        // In callback order: the open channels only, as the audio thread sees them.
        const auto names = d->getInputChannelNames();
        const auto active = d->getActiveInputChannels();
        for (int i = 0; i < names.size(); ++i)
            if (active[i]) out.add (names[i]);
    }
    return out;
}

bool Engine::ensureAudioInputsOpen()
{
    auto* d = deviceManager.getCurrentAudioDevice();
    if (d == nullptr) return false;
    if (d->getActiveInputChannels().countNumberOfSetBits() > 0) return true;

    auto s = deviceManager.getAudioDeviceSetup();
    if (s.inputDeviceName.isEmpty()) s.inputDeviceName = s.outputDeviceName;
    /* The first two channels, named: "default input channels" means as many
       as initialise() asked for, which is none -- the device opened fine
       with no inputs at all the first time round. */
    s.useDefaultInputChannels = false;
    s.inputChannels.clear();
    s.inputChannels.setRange (0, 2, true);
    const auto err = deviceManager.setAudioDeviceSetup (s, true);
    const auto open = getAudioInputChannelNames();
    std::fprintf (stderr, "[audio] opening inputs on '%s' (type %s, outputs on '%s'): %s, %d input channel(s) open%s%s\n",
                  s.inputDeviceName.toRawUTF8(), deviceManager.getCurrentAudioDeviceType().toRawUTF8(),
                  s.outputDeviceName.toRawUTF8(), err.isEmpty() ? "ok" : err.toRawUTF8(), open.size(),
                  open.isEmpty() ? "" : ": ", open.joinIntoString (", ").toRawUTF8());
    return err.isEmpty() && open.size() > 0;
}

void Engine::setInputProgramChangeChannel (int inputIndex, int channel)
{
    if (! validInput (inputIndex)) return;
    channel = jlimit (-1, 16, channel);
    setup.inputs[(size_t) inputIndex].programChangeChannel = channel;
    runtimes[(size_t) inputIndex]->programChangeChannel.store (channel);
}

void Engine::setInputRespondToProgramChange (int inputIndex, bool b)
{
    if (! validInput (inputIndex)) return;
    setup.inputs[(size_t) inputIndex].respondToProgramChange = b;
    runtimes[(size_t) inputIndex]->respondToProgramChange.store (b);
}

File Engine::getRecordingsFolder() const
{
    if (setupFile != File())
        return setupFile.getSiblingFile (setupFile.getFileNameWithoutExtension().upToLastOccurrenceOf (".stagerig", false, false) + ".stagerig.recordings");
    return File::getSpecialLocation (File::userMusicDirectory).getChildFile ("StageRig recordings");
}

bool Engine::startRecording (String& error)
{
    if (recorder.isRecording()) return true;
    StringArray outNames, inNames;
    for (const auto& o : setup.outputs) outNames.add (o.name);
    for (const auto& in : setup.inputs) inNames.add (in.name);
    if (outNames.isEmpty()) outNames.add ("Main");
    const File take = getRecordingsFolder().getNonexistentChildFile (Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S"), "", false);
    const bool ok = recorder.start (take, outNames, inNames, sampleRate, jmax (blockSize, 64), error);
    listeners.call ([] (Listener& l) { l.transportChanged(); });
    return ok;
}

void Engine::stopRecording()
{
    if (! recorder.isRecording()) return;
    { const ScopedLock sl (lock); recorder.disarm(); }       // the audio thread stops writing before the writers go
    recorder.stop();
    listeners.call ([] (Listener& l) { l.transportChanged(); });
}

bool Engine::sendsClockTo (const String& identifier) const
{
    for (const auto& c : setup.clockOut) if (c.identifier == identifier) return true;
    return false;
}

void Engine::setClockTo (const MidiDeviceInfo& device, bool on)
{
    auto& list = setup.clockOut;
    list.erase (std::remove_if (list.begin(), list.end(), [&] (const Setup::MidiPortRef& c) { return c.identifier == device.identifier; }), list.end());
    if (on) list.push_back ({ device.identifier, device.name });
    resolveClockPorts();
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::resolveClockPorts()
{
    int n = 0;
    for (const auto& c : setup.clockOut)
    {
        if (n >= MidiOuts::kMaxPorts) break;
        if (const int port = midiOuts.portFor (c.identifier, c.name); port >= 0) clockPorts[n++].store (port);
    }
    numClockPorts.store (n);
}

void Engine::sendClock (int numSamples, double chunkStartMs, bool trackStarted, bool trackStopped)
{
    const int n = numClockPorts.load (std::memory_order_relaxed);
    if (n <= 0) { clockSamplesToNext = 0.0; return; }
    /* Twenty-four ticks a beat, free-running at the rig's tempo so a synced
       device follows a tap at once; Start when the track starts, so its bar
       one is the device's bar one, and Stop when the track stops. */
    if (trackStarted)
    {
        clockSamplesToNext = 0.0;
        for (int i = 0; i < n; ++i) midiOuts.push (clockPorts[i].load (std::memory_order_relaxed), MidiMessage::midiStart(), chunkStartMs);
    }
    const double samplesPerTick = sampleRate * 60.0 / (jmax (20.0, tempoAtomic.load (std::memory_order_relaxed)) * 24.0);
    while (clockSamplesToNext < (double) numSamples)
    {
        const double at = chunkStartMs + clockSamplesToNext * 1000.0 / sampleRate;
        for (int i = 0; i < n; ++i) midiOuts.push (clockPorts[i].load (std::memory_order_relaxed), MidiMessage::midiClock(), at);
        clockTicks.fetch_add (1, std::memory_order_relaxed);
        clockSamplesToNext += samplesPerTick;
    }
    clockSamplesToNext -= (double) numSamples;
    if (trackStopped)
        for (int i = 0; i < n; ++i) midiOuts.push (clockPorts[i].load (std::memory_order_relaxed), MidiMessage::midiStop(), chunkStartMs + numSamples * 1000.0 / sampleRate);
}

void Engine::midiDevicesChanged() { midiDevicesChanged ({}); }

/* A USB keyboard or interface that drops out and comes back quickly can come back
   under the very port numbers it had, so the device list looks unchanged and its
   old, dead connections stay open: the keys go silent with nothing said. The
   kernel numbers every arrival afresh, though, so a sound card whose USB device
   number changed has been away. Its ports are opened again, and what its inputs
   held is let go. A card's MIDI ports are ALSA client 16 + 4 x card. */
void Engine::watchUsbReconnects()
{
   #if JUCE_LINUX
    const auto now = Time::getMillisecondCounter();
    if (now - lastUsbWatch < 1000) return;
    lastUsbWatch = now;

    StringArray cards;
    for (const auto& e : RangedDirectoryIterator (File ("/sys/class/sound"), false, "card*", File::findFilesAndDirectories))
    {
        const auto name = e.getFile().getFileName();
        if (! name.substring (4).containsOnly ("0123456789") || name.length() <= 4) continue;
        char resolved[PATH_MAX];
        if (::realpath (e.getFile().getChildFile ("device").getFullPathName().toRawUTF8(), resolved) == nullptr) continue;
        const auto usb = File (String (CharPointer_UTF8 (resolved))).getParentDirectory();
        const auto devnum = usb.getChildFile ("devnum");
        if (! devnum.existsAsFile()) continue;          // not on USB
        cards.add (name.substring (4) + "|" + usb.getFullPathName() + "|" + devnum.loadFileAsString().trim());
    }
    if (usbCards.isEmpty() && ! usbCardsKnown) { usbCards = cards; usbCardsKnown = true; return; }
    if (cards == usbCards) return;

    StringArray again;     // card numbers present before and now, but a new arrival
    for (const auto& c : cards)
    {
        const auto card = c.upToFirstOccurrenceOf ("|", false, false);
        for (const auto& old : usbCards)
            if (old.upToFirstOccurrenceOf ("|", false, false) == card && old != c) again.add (card);
    }
    usbCards = cards;
    if (again.isEmpty()) return;

    StringArray reconnected, names;
    auto ports = MidiInput::getAvailableDevices();
    ports.addArray (MidiOutput::getAvailableDevices());
    for (const auto& d : ports)
    {
        const int client = d.identifier.upToFirstOccurrenceOf ("-", false, false).getIntValue();
        if (client >= 16 && client < 128 && again.contains (String ((client - 16) / 4)))
        {
            reconnected.addIfNotAlreadyThere (d.identifier);
            names.addIfNotAlreadyThere (d.name.trim());
        }
    }
    if (reconnected.isEmpty()) return;
    midiDevicesChanged (reconnected);
    listeners.call ([n = names.joinIntoString (", ")] (Listener& l) { l.statusMessage (n + " reconnected."); });
   #endif
}

void Engine::midiDevicesChanged (const StringArray& reconnected)
{
    StringArray now;
    const auto ins = MidiInput::getAvailableDevices();
    for (const auto& d : ins) now.add ("i " + d.name + "|" + d.identifier);
    for (const auto& d : MidiOutput::getAvailableDevices()) now.add ("o " + d.name + "|" + d.identifier);
    if (now == knownMidiDevices && reconnected.isEmpty()) return;

    /* A port that has just appeared, or come back: everything holding the port
       of that number lets go of it BEFORE anything opens it. JUCE keeps one
       connection a port number for as long as anything holds it, dead or not,
       and hands that same one to whoever opens the number next -- so a
       keyboard back under its old number, opened while its control surface
       (or the engine's own old input) still held the dead connection, came
       back deaf: keys silent, "connected" on the status line, and nothing
       tried again. The surfaces first, then the engine's inputs; the hub
       finds its keyboards again on its next look. */
    bool appeared = ! reconnected.isEmpty();
    StringArray outsAgain;
    for (const auto& d : ins)
        if (! knownMidiDevices.contains ("i " + d.name + "|" + d.identifier)) appeared = true;
    for (const auto& d : MidiOutput::getAvailableDevices())
        if (! knownMidiDevices.contains ("o " + d.name + "|" + d.identifier))
        {
            appeared = true;
            outsAgain.addIfNotAlreadyThere (d.identifier);      // its old connection, if it had one, is dead too
        }
    if (appeared)
        listeners.call ([] (Listener& l) { l.midiDevicesReconnected(); });
    for (const auto& d : ins)
        if (! knownMidiDevices.contains ("i " + d.name + "|" + d.identifier))
            deviceManager.setMidiInputDeviceEnabled (d.identifier, false);
    knownMidiDevices = now;

    std::vector<bool> had;
    for (const auto& rt : runtimes) had.push_back (rt->deviceIdentifier.isNotEmpty());
    openMidiDevices (reconnected);
    // The same cards' output ports, back under their old numbers too.
    for (const auto& d : MidiOutput::getAvailableDevices())
        for (const auto& id : reconnected)
            if (d.identifier.upToFirstOccurrenceOf ("-", false, false) == id.upToFirstOccurrenceOf ("-", false, false))
                outsAgain.addIfNotAlreadyThere (d.identifier);
    midiOuts.refresh (outsAgain);
    resolveClockPorts();
    listeners.call ([] (Listener& l) { l.setupChanged(); });

    // A device gives no sign of its own: say what changed, once for each.
    StringArray back, gone;
    for (int i = 0; i < (int) setup.inputs.size() && i < (int) had.size(); ++i)
    {
        const auto& def = setup.inputs[(size_t) i];
        if (def.midiDeviceName.isEmpty()) continue;
        const bool has = runtimes[(size_t) i]->deviceIdentifier.isNotEmpty();
        if (has && ! had[(size_t) i])      back.addIfNotAlreadyThere (def.midiDeviceName);
        else if (! has && had[(size_t) i]) gone.addIfNotAlreadyThere (def.midiDeviceName);
    }
    String msg;
    if (! back.isEmpty()) msg << back.joinIntoString (", ") << (back.size() == 1 ? " is" : " are") << " connected. ";
    if (! gone.isEmpty()) msg << gone.joinIntoString (", ") << (gone.size() == 1 ? " has" : " have") << " gone: plug it back in and it is picked up.";
    if (msg.isNotEmpty()) listeners.call ([&msg] (Listener& l) { l.statusMessage (msg.trim()); });
}

void Engine::refreshMidiDevices()
{
    openMidiDevices();
    midiOuts.refresh();
    resolveClockPorts();
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::openMidiDevices (const StringArray& reconnected)
{
    auto available = MidiInput::getAvailableDevices();

    /* The name says which keyboard; the identifier only separates two of the same
       name. An identifier is a port number that changes between plug-ins and boots,
       and the old one can belong to another device by now, so alone it counts only
       for an input that has no name.

       First every input whose port is where it was, by name and number. Then the
       others, by name, each to a port of that name no other input has: two
       keyboards of one name stay two, and inputs that shared a port (a split)
       go on sharing its new one. A port is shared otherwise only when it is the
       one device of that name, which is then the same keyboard. */
    const int n = (int) setup.inputs.size();
    std::vector<String> before ((size_t) n), chosen ((size_t) n);
    {
        const ScopedLock sl (lock);
        for (int i = 0; i < n && i < (int) runtimes.size(); ++i) before[(size_t) i] = runtimes[(size_t) i]->deviceIdentifier;
    }
    std::map<String, StringArray> claims;      // port -> the saved port numbers of the inputs on it
    StringArray confirmed;                     // ports an input is on where it was
    for (int i = 0; i < n; ++i)
    {
        const auto& def = setup.inputs[(size_t) i];
        if (def.midiDeviceIdentifier.isEmpty()) continue;
        for (auto& d : available)
            if (d.identifier == def.midiDeviceIdentifier && (d.name == def.midiDeviceName || def.midiDeviceName.isEmpty()))
            {
                chosen[(size_t) i] = d.identifier;
                claims[d.identifier].addIfNotAlreadyThere (def.midiDeviceIdentifier);
                confirmed.addIfNotAlreadyThere (d.identifier);
            }
    }
    for (int i = 0; i < n; ++i)
    {
        auto& def = setup.inputs[(size_t) i];
        if (chosen[(size_t) i].isNotEmpty() || def.midiDeviceName.isEmpty()) continue;
        const String group = def.midiDeviceIdentifier.isNotEmpty() ? def.midiDeviceIdentifier : "name:" + def.midiDeviceName;
        Array<MidiDeviceInfo> same;
        for (auto& d : available) if (d.name == def.midiDeviceName) same.add (d);
        String pick;
        for (auto& d : same)
            if (claims[d.identifier].isEmpty() || claims[d.identifier].contains (group)) { pick = d.identifier; break; }
        if (pick.isNotEmpty())
        {
            claims[pick].addIfNotAlreadyThere (group);
            def.midiDeviceIdentifier = pick;
            chosen[(size_t) i] = pick;
        }
        /* The one device of that name, taken by another input that moved too: the same
           keyboard, split across inputs whose port numbers had drifted apart. Shared,
           but not written down, so the input goes to a second keyboard of that name
           when one appears. Never one an input is on where it was: that is the other
           keyboard of a pair, and its twin is just away. */
        else if (same.size() == 1 && ! confirmed.contains (same.getReference (0).identifier))
            chosen[(size_t) i] = same.getReference (0).identifier;
    }

    StringArray wanted;
    for (int i = 0; i < n; ++i)
    {
        if (chosen[(size_t) i].isNotEmpty()) wanted.addIfNotAlreadyThere (chosen[(size_t) i]);
        const ScopedLock sl (lock);
        runtimes[(size_t) i]->deviceIdentifier = chosen[(size_t) i];
        // Gone, moved, or reconnected under the same number: what it held is let go.
        if (before[(size_t) i].isNotEmpty() && (before[(size_t) i] != chosen[(size_t) i] || reconnected.contains (before[(size_t) i])))
            runtimes[(size_t) i]->deviceLost.store (true);
    }

    // A port reconnected under its old number has a dead connection on the books: closed, to open afresh.
    for (const auto& id : reconnected)
        deviceManager.setMidiInputDeviceEnabled (id, false);
    if (midi2 != nullptr && ! reconnected.isEmpty()) midi2->forget (reconnected);

    /* A keyboard speaking MIDI 2.0 is heard through Midi2Inputs, unless an
       input on it asks for MIDI 1.0; the MIDI 1.0 path leaves it alone, so
       nothing arrives twice. */
    StringArray asMidi2;
    for (const auto& id : wanted)
    {
        bool midi1 = false;
        for (const auto& def : setup.inputs)
            if (def.midiDeviceIdentifier == id && def.midi1Only) midi1 = true;
        if (! midi1) asMidi2.add (id);
    }
    /* Its MIDI 1.0 port is closed before the MIDI 2.0 connection opens, so
       nothing arrives twice in between; a keyboard the MIDI 2.0 path then
       declines gets it back below. */
    if (midi2 != nullptr)
        for (auto& d : available)
            if (asMidi2.contains (d.identifier) && Midi2Inputs::isMidi2 (d.identifier))
                deviceManager.setMidiInputDeviceEnabled (d.identifier, false);
    const auto heard = midi2 != nullptr ? midi2->open (asMidi2) : StringArray();
    if (heard != heardAsMidi2)
    {
        // A keyboard moved between the MIDI 1.0 and 2.0 paths: what was in flight on the old one is lost.
        const ScopedLock sl (lock);
        for (auto& rt : runtimes)
        {
            rt->exact.clear();
            if (rt->deviceIdentifier.isNotEmpty() && heard.contains (rt->deviceIdentifier) != heardAsMidi2.contains (rt->deviceIdentifier))
                rt->deviceLost.store (true);
        }
    }
    heardAsMidi2 = heard;

    /* Opened as the device it is now. A port number that was open for another
       device (one gone, this one arrived under its number before the list's
       change was heard of), or for one no longer there, holds a dead
       connection that enabling would take for a live one: closed first. */
    for (auto it = midiOpenedAs.begin(); it != midiOpenedAs.end();)
    {
        bool same = false;
        for (auto& d : available) if (d.identifier == it->first && d.name == it->second) same = true;
        if (! same) { deviceManager.setMidiInputDeviceEnabled (it->first, false); it = midiOpenedAs.erase (it); }
        else ++it;
    }
    for (auto& d : available)
    {
        const bool on = wanted.contains (d.identifier) && ! heard.contains (d.identifier);
        deviceManager.setMidiInputDeviceEnabled (d.identifier, on);
        if (on) midiOpenedAs[d.identifier] = d.name; else midiOpenedAs.erase (d.identifier);
    }
}

bool Engine::isInputHeardAsMidi2 (int inputIndex) const
{
    return validInput (inputIndex) && heardAsMidi2.contains (setup.inputs[(size_t) inputIndex].midiDeviceIdentifier);
}

void Engine::setInputAutoKnobs (int inputIndex, bool b)
{
    if (! validInput (inputIndex)) return;
    auto& rt = *runtimes[(size_t) inputIndex];
    rt.autoKnobs.store (b);
    const auto heard = setup.inputs[(size_t) inputIndex].controls.heardCC (b);
    for (int k = 0; k < SurfaceControls::numKnobs; ++k) rt.heardCC[(size_t) k].store (heard[(size_t) k]);
}

bool Engine::isInputAutoKnobs (int inputIndex) const
{
    return validInput (inputIndex) && runtimes[(size_t) inputIndex]->autoKnobs.load();
}

int Engine::keyboardKnobFor (int inputIndex, int cc) const
{
    if (! validInput (inputIndex) || cc <= 0) return -1;
    const auto heard = setup.inputs[(size_t) inputIndex].controls.heardCC (isInputAutoKnobs (inputIndex));
    for (int k = 0; k < SurfaceControls::numKnobs; ++k)
        if (heard[(size_t) k] == cc) return k;
    return -1;
}

void Engine::setInputMidi1Only (int inputIndex, bool b)
{
    if (! validInput (inputIndex)) return;
    setup.inputs[(size_t) inputIndex].midi1Only = b;
    openMidiDevices();
}

//==============================================================================
// Programs
//==============================================================================
Engine::ProgramRuntime* Engine::getLoaded (int inputIndex, int program) const
{
    if (isOutputAddress (inputIndex))
    {
        const int o = outputFromAddress (inputIndex);
        return o >= 0 && o < (int) outputs.size() ? &outputs[(size_t) o]->chain : nullptr;
    }
    if (! validInput (inputIndex)) return nullptr;
    auto& m = runtimes[(size_t) inputIndex]->loaded;
    auto it = m.find (program);
    return it == m.end() ? nullptr : it->second.get();
}

bool Engine::isHoldingNotes (int inputIndex) const
{
    const ScopedLock sl (lock);
    if (inputIndex < 0 || inputIndex >= (int) runtimes.size()) return false;
    const auto* active = runtimes[(size_t) inputIndex]->active;
    return active != nullptr && active->anythingHeld();
}

bool Engine::isProgramLoaded (int inputIndex, int program) const
{
    return getLoaded (inputIndex, program) != nullptr;
}

//==============================================================================
// Songs and sets
//==============================================================================
int Engine::addSong (const String& name)
{
    SongDef s;
    s.name = name.isNotEmpty() ? name : "Song " + String ((int) setup.songs.size() + 1);
    s.programs.assign (setup.inputs.size(), -1);
    setup.songs.push_back (std::move (s));
    listeners.call ([] (Listener& l) { l.setupChanged(); });
    return (int) setup.songs.size() - 1;
}

void Engine::removeSong (int songIndex)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    // Its program on the backing track was its own: it goes too, unless another song shares it.
    if (const int track = setup.trackInput(); track >= 0)
        if (const int p = setup.songs[(size_t) songIndex].programFor (track); p >= 0)
        {
            bool shared = false;
            for (int i = 0; i < (int) setup.songs.size(); ++i)
                if (i != songIndex && setup.songs[(size_t) i].programFor (track) == p) shared = true;
            if (! shared) clearProgram (track, p);
        }
    setup.songs.erase (setup.songs.begin() + songIndex);

    /* Sets hold indices, so everything after the hole shifts down and any set
       that used this song drops it. Doing this here rather than leaving
       dangling indices about is the difference between a deleted song and a
       set that silently plays the wrong number. */
    for (auto& st : setup.sets)
    {
        auto& v = st.songs;
        v.erase (std::remove (v.begin(), v.end(), songIndex), v.end());
        for (auto& i : v) if (i > songIndex) --i;
    }
    if (setup.currentSet >= 0 && setup.currentSongInSet >= (int) setup.sets[(size_t) setup.currentSet].songs.size())
        setup.currentSongInSet = -1;
    if (songInPlay == songIndex)     { songInPlay = -1; currentScene = -1; }
    else if (songInPlay > songIndex) --songInPlay;
    // The armed track's song follows the renumbering too.
    if (trackSong == songIndex)      trackSong = -1;
    else if (trackSong > songIndex)  --trackSong;

    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

int Engine::moveSong (int songIndex, int delta)
{
    const int n = (int) setup.songs.size();
    const int to = songIndex + delta;
    if (songIndex < 0 || songIndex >= n || to < 0 || to >= n) return songIndex;

    std::swap (setup.songs[(size_t) songIndex], setup.songs[(size_t) to]);

    /* Sets point at songs by index, so swapping two songs would otherwise
       silently change what every set plays. Rewrite the references instead:
       the running order stays exactly as it was. */
    for (auto& st : setup.sets)
        for (auto& i : st.songs)
        {
            if (i == songIndex)   i = to;
            else if (i == to)     i = songIndex;
        }
    if (songInPlay == songIndex)  songInPlay = to;
    else if (songInPlay == to)    songInPlay = songIndex;
    if (trackSong == songIndex)   trackSong = to;
    else if (trackSong == to)     trackSong = songIndex;

    listeners.call ([] (Listener& l) { l.setupChanged(); });
    return to;
}

void Engine::setSongName (int songIndex, const String& name)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    auto& song = setup.songs[(size_t) songIndex];
    // The song's program on the backing track was named after it: it follows.
    if (const int track = setup.trackInput(); track >= 0)
        if (const int p = song.programFor (track); p >= 0 && setup.inputs[(size_t) track].programs[(size_t) p].name == song.name)
            setProgramName (track, p, name);
    song.name = name;
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::addSongChart (int songIndex, const ChartRef& chart)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size() || chart.file.isEmpty()) return;
    setup.songs[(size_t) songIndex].charts.push_back (chart);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::removeSongChart (int songIndex, int chartIndex)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    auto& charts = setup.songs[(size_t) songIndex].charts;
    if (chartIndex < 0 || chartIndex >= (int) charts.size()) return;
    charts.erase (charts.begin() + chartIndex);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::setSongNotes (int songIndex, const String& notes)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    setup.songs[(size_t) songIndex].notes = notes;
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::setSongTempo (int songIndex, double bpm)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    // 0 means "leave the tempo alone"; anything else is clamped like the global one.
    const double before = stretchOf (setup.songs[(size_t) songIndex]);
    setup.songs[(size_t) songIndex].tempoBpm = bpm <= 0.0 ? 0.0 : jlimit (20.0, 300.0, bpm);
    // Files that know their own tempo follow the song's: re-read them when the stretch actually changes.
    if (songIndex == trackSong && std::abs (stretchOf (setup.songs[(size_t) songIndex]) - before) > 1e-9) armTrack (songIndex);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

double Engine::stretchOf (const SongDef& s)
{
    return s.trackBpm > 0.0 && s.tempoBpm > 0.0 && std::abs (s.trackBpm - s.tempoBpm) > 0.005 ? s.trackBpm / s.tempoBpm : 1.0;
}

void Engine::setSongTempoAndFiles (int songIndex, double tempoBpm, double trackBpm, double firstBeat)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    auto& s = setup.songs[(size_t) songIndex];
    const double before = stretchOf (s);
    s.tempoBpm = tempoBpm <= 0.0 ? 0.0 : jlimit (20.0, 300.0, tempoBpm);
    s.trackBpm = trackBpm <= 0.0 ? 0.0 : jlimit (20.0, 300.0, trackBpm);
    s.firstBeat = jmax (0.0, firstBeat);
    if (songIndex == trackSong)
    {
        if (std::abs (stretchOf (s) - before) > 1e-9) armTrack (songIndex);      // one re-read for the whole edit
        else if (player != nullptr) player->setClick (s.click, s.beatsPerBar, s.firstBeat);
    }
    if (songIndex == currentSongIndex() && s.tempoBpm > 0.0) setTempoBpm (s.tempoBpm);
    listeners.call ([] (Listener& l) { l.setupChanged(); l.transportChanged(); });
}

void Engine::setSongProgram (int songIndex, int inputIndex, int program)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    if (! validInput (inputIndex)) return;

    auto& s = setup.songs[(size_t) songIndex];
    if (s.programs.size() < setup.inputs.size()) s.programs.resize (setup.inputs.size(), -1);
    s.programs[(size_t) inputIndex] = (program >= 0 && program < InputDef::numPrograms) ? program : -1;
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::captureSong (int songIndex)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;

    auto& s = setup.songs[(size_t) songIndex];
    s.programs.assign (setup.inputs.size(), -1);
    for (size_t i = 0; i < setup.inputs.size(); ++i)
        s.programs[i] = setup.inputs[i].currentProgram;
    s.tempoBpm = setup.tempoBpm;
    s.transpose = liveTranspose.load();
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::setTranspose (int semitones)
{
    const int st = jlimit (-24, 24, semitones);
    if (liveTranspose.exchange (st) == st) return;
    applySongKey();                 // the notes the effects hear moved, so the key they play in moves with them
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::setSongKey (int songIndex, int root, int mode)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    auto& s = setup.songs[(size_t) songIndex];
    s.keyRoot = jlimit (-1, 11, root);
    s.keyMode = jlimit (0, MidiFx::numScaleModes() - 1, mode);
    if (songIndex == currentSongIndex()) applySongKey();
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

bool Engine::keyInPlay (int& root, int& mode) const
{
    const int song = currentSongIndex();
    if (song < 0 || ! setup.songs[(size_t) song].hasKey()) return false;
    const auto& s = setup.songs[(size_t) song];
    root = ((s.keyRoot + liveTranspose.load()) % 12 + 12) % 12;
    mode = s.keyMode;
    return true;
}

EffectDef Engine::effectiveEffectDef (const EffectDef& d) const
{
    if (! d.followSongKey || (d.kind != EffectDef::Kind::scale && d.kind != EffectDef::Kind::run && d.kind != EffectDef::Kind::bigBand)) return d;
    int root, mode;
    if (! keyInPlay (root, mode)) return d;
    EffectDef e = d;
    e.scaleRoot = root;
    e.scaleMode = mode;
    return e;
}

void Engine::applySongKey()
{
    /* Every one loaded, in every program, not just the ones in play: a
       preloaded program should not come in on the last song's key. */
    for (size_t i = 0; i < runtimes.size() && i < setup.inputs.size(); ++i)
        for (auto& [p, rt] : runtimes[i]->loaded)
        {
            if (p < 0 || p >= (int) setup.inputs[i].programs.size()) continue;
            const auto& pd = setup.inputs[i].programs[(size_t) p];
            for (size_t s = 0; s < rt->slots.size() && s < pd.slots.size(); ++s)
            {
                const auto& defs = pd.slots[s].midiEffects;
                auto& fxs = rt->slots[s]->midiEffects;
                for (size_t e = 0; e < fxs.size() && e < defs.size(); ++e)
                    if (fxs[e]->builtIn != nullptr && defs[e].isBuiltIn())
                        fxs[e]->builtIn->configure (effectiveEffectDef (defs[e]));
            }
        }
}

void Engine::setSongTranspose (int songIndex, int semitones)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    setup.songs[(size_t) songIndex].transpose = jlimit (-24, 24, semitones);
    if (songIndex == currentSongIndex()) setTranspose (semitones);
    if (songIndex == trackSong && setup.songs[(size_t) songIndex].transposeTrack) armTrack (songIndex);   // the files move too
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

int Engine::currentSongIndex() const
{
    // A song tapped outside the set list is the song in play as much as one stepped to.
    if (songInPlay >= 0 && songInPlay < (int) setup.songs.size()) return songInPlay;
    if (setup.currentSet < 0 || setup.currentSet >= (int) setup.sets.size()) return -1;
    const auto& songs = setup.sets[(size_t) setup.currentSet].songs;
    if (setup.currentSongInSet < 0 || setup.currentSongInSet >= (int) songs.size()) return -1;
    const int s = songs[(size_t) setup.currentSongInSet];
    return s >= 0 && s < (int) setup.songs.size() ? s : -1;
}

void Engine::selectSong (int songIndex, int scene)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    songInPlay = songIndex;
    currentScene = -1;

    const auto& s = setup.songs[(size_t) songIndex];
    /* A song with scenes opens on one: the inputs that scene puts on a
       program are left for it, so no keyboard loads the song's program only
       to drop it a moment later. */
    const bool scenes = songHasScenes (songIndex);
    if (scenes) scene = jlimit (0, (int) s.markers.size() - 1, scene);
    std::vector<bool> sceneSets (setup.inputs.size(), false);
    if (scenes)
        for (const auto& a : s.markers[(size_t) scene].actions)
            if (a.kind == ActionDef::Kind::program && a.input >= 0 && a.input < (int) sceneSets.size()) sceneSets[(size_t) a.input] = true;
    for (int i = 0; i < (int) setup.inputs.size(); ++i)
        if (const int p = s.programFor (i); p >= 0 && ! sceneSets[(size_t) i])
            selectProgram (i, p);

    // Tempo after the programs: a plugin that has just loaded is told the tempo
    // by setTempoBpm, so doing it in this order means no program starts at the
    // old one.
    if (s.tempoBpm > 0.0)
    {
        setTempoBpm (s.tempoBpm);
        resetTapTempo();
    }
    setTranspose (s.transpose);
    applySongKey();                 // the song's key, for the effects that follow it
    beatsPerBarAtomic.store (jmax (1, s.beatsPerBar));
    // Its backing track is made ready, not started: starting is always on purpose.
    armTrack (songIndex);
    // A song played by hand opens on a scene: the first, unless asked for another.
    if (scenes) selectScene (scene);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

//==============================================================================
// Scenes
//==============================================================================
bool Engine::songHasScenes (int songIndex) const
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return false;
    const auto& s = setup.songs[(size_t) songIndex];
    return s.isPlayedByHand() && ! s.markers.empty();
}

int Engine::getSceneCount() const
{
    const int song = currentSongIndex();
    return songHasScenes (song) ? (int) setup.songs[(size_t) song].markers.size() : 0;
}

void Engine::selectScene (int index)
{
    /* The song in play: the one last selected, or straight after a load the
       one the set list is on, which becomes the one in play. */
    const int songIndex = currentSongIndex();
    if (! songHasScenes (songIndex)) return;
    songInPlay = songIndex;
    const auto& song = setup.songs[(size_t) songIndex];
    if (index < 0 || index >= (int) song.markers.size()) return;
    currentScene = index;
    const auto actions = song.markers[(size_t) index].actions;      // a copy: an action may edit the setup
    for (const auto& a : actions) performAction (songIndex, a);
    listeners.call ([] (Listener& l) { l.transportChanged(); });
}

int Engine::addScene (int songIndex, const String& name)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return -1;
    auto& s = setup.songs[(size_t) songIndex];
    MarkerDef m;
    m.name = name.isNotEmpty() ? name : "Scene " + String ((int) s.markers.size() + 1);
    s.markers.push_back (m);
    const int index = (int) s.markers.size() - 1;
    captureScene (songIndex, index);
    if (songIndex == songInPlay && currentScene < 0) currentScene = index;
    listeners.call ([] (Listener& l) { l.transportChanged(); l.setupChanged(); });
    return index;
}

void Engine::captureScene (int songIndex, int sceneIndex)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    auto& s = setup.songs[(size_t) songIndex];
    if (sceneIndex < 0 || sceneIndex >= (int) s.markers.size()) return;
    auto& actions = s.markers[(size_t) sceneIndex].actions;
    // The program actions go, replaced by one per input for what is loaded now; parts, effects and cues stay.
    actions.erase (std::remove_if (actions.begin(), actions.end(), [] (const ActionDef& a) { return a.kind == ActionDef::Kind::program; }), actions.end());
    std::vector<ActionDef> programs;
    for (int i = 0; i < (int) setup.inputs.size(); ++i)
    {
        if (setup.inputs[(size_t) i].isTrack()) continue;         // the track's program follows the song
        ActionDef a;
        a.kind = ActionDef::Kind::program; a.input = i; a.program = setup.inputs[(size_t) i].currentProgram;
        if (a.program >= 0) programs.push_back (a);
    }
    actions.insert (actions.begin(), programs.begin(), programs.end());
    listeners.call ([] (Listener& l) { l.transportChanged(); l.setupChanged(); });
}

void Engine::moveScene (int songIndex, int from, int to)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    auto& m = setup.songs[(size_t) songIndex].markers;
    if (from < 0 || from >= (int) m.size() || to < 0 || to >= (int) m.size() || from == to) return;
    auto item = m[(size_t) from];
    m.erase (m.begin() + from);
    m.insert (m.begin() + to, item);
    if (songIndex == songInPlay && currentScene >= 0)
    {
        if (currentScene == from) currentScene = to;
        else if (from < currentScene && to >= currentScene) --currentScene;
        else if (from > currentScene && to <= currentScene) ++currentScene;
    }
    listeners.call ([] (Listener& l) { l.transportChanged(); l.setupChanged(); });
}

//==============================================================================
// Backing tracks
//==============================================================================
void Engine::addSongStem (int songIndex, const StemDef& d)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    setup.songs[(size_t) songIndex].stems.push_back (d);
    if (d.isMidi()) ensureTrackProgram (songIndex);
    if (songIndex == trackSong) armTrack (songIndex);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

int Engine::ensureTrackProgram (int songIndex)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return -1;
    /* A MIDI file plays into the backing-track input, on the program this
       song selects there. The first file gives the song that program --
       named after it, with the General MIDI player, so the file plays at
       once -- and its parts are given other instruments from the song's
       Backing track window. */
    const int track = ensureTrackInput();
    auto& song = setup.songs[(size_t) songIndex];
    if (const int have = song.programFor (track); have >= 0) return have;
    auto& programs = setup.inputs[(size_t) track].programs;
    int p = -1;
    for (int i = 0; i < (int) programs.size() && p < 0; ++i)
        if (programs[(size_t) i].isEmpty() && programs[(size_t) i].name.isEmpty()) p = i;
    if (p < 0) p = InputDef::numPrograms - 1;
    if (song.programs.size() <= (size_t) track) song.programs.resize ((size_t) track + 1, -1);
    song.programs[(size_t) track] = p;
    setProgramName (track, p, song.name);
    addGmSlot (track, p);
    if (songIndex == trackSong) selectProgram (track, p);
    return p;
}

int Engine::ensureTrackInput()
{
    if (const int t = setup.trackInput(); t >= 0) return t;
    const int idx = addInput (InputDef::trackName, InputDef::Kind::track);
    setInputChannel (idx, 0);
    setInputRespondToProgramChange (idx, false);
    return idx;
}

void Engine::assignPart (int inputIndex, int program, int channel, int slot)
{
    if (! validInput (inputIndex) || ! validProgram (program) || channel < 1 || channel > 16) return;
    const auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    for (int s = 0; s < (int) def.slots.size(); ++s)
    {
        const auto& sd = def.slots[(size_t) s];
        /* The chosen one takes the channel: an instrument that was hearing
           everything now hears this part alone, so the parts stay one each;
           one already on a few channels gets one more. The rest let it go. */
        if (s == slot && sd.hearsAll()) setSlotListensTo (inputIndex, program, s, 1 << (channel - 1));
        else                            setSlotHears (inputIndex, program, s, channel, s == slot);
    }
    notifyContent (inputIndex, program);
}

void Engine::setPartMuted (int inputIndex, int program, int channel, bool muted)
{
    const int sl = partInstrument (inputIndex, program, channel);
    if (sl < 0) return;
    const auto& sd = setup.inputs[(size_t) inputIndex].programs[(size_t) program].slots[(size_t) sl];
    if (sd.isGm())
    {
        auto gc = sd.gmChannels[(size_t) channel - 1];
        gc.muted = muted;
        setGmChannel (inputIndex, program, sl, channel, gc);
    }
    else
        setSlotEnabled (inputIndex, program, sl, ! muted);
}

bool Engine::isPartMuted (int inputIndex, int program, int channel) const
{
    const int sl = partInstrument (inputIndex, program, channel);
    if (sl < 0) return true;
    const auto& sd = setup.inputs[(size_t) inputIndex].programs[(size_t) program].slots[(size_t) sl];
    return sd.isGm() ? sd.gmChannels[(size_t) channel - 1].muted : ! sd.enabled;
}

int Engine::partInstrument (int inputIndex, int program, int channel) const
{
    if (! validInput (inputIndex) || ! validProgram (program)) return -1;
    const auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    for (int s = 0; s < (int) def.slots.size(); ++s)
        if (def.slots[(size_t) s].hears (channel)) return s;
    return -1;
}

void Engine::removeSongStem (int songIndex, int stemIndex)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    auto& stems = setup.songs[(size_t) songIndex].stems;
    if (stemIndex < 0 || stemIndex >= (int) stems.size()) return;
    stems.erase (stems.begin() + stemIndex);
    if (songIndex == trackSong) armTrack (songIndex);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::setSongStem (int songIndex, int stemIndex, const StemDef& d)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    auto& stems = setup.songs[(size_t) songIndex].stems;
    if (stemIndex < 0 || stemIndex >= (int) stems.size()) return;
    const bool fileChanged = stems[(size_t) stemIndex].file != d.file;
    stems[(size_t) stemIndex] = d;
    if (songIndex != trackSong) return;
    /* A new output or level takes effect at once, even mid-song; only a
       different file needs the track loaded again. */
    if (! fileChanged && player != nullptr)
        player->setStemMix (stemIndex, d.output, d.gainDb);
    else
        armTrack (songIndex);
}

void Engine::setSongCountIn (int songIndex, int beats, int output)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    auto& s = setup.songs[(size_t) songIndex];
    s.countInBeats = jlimit (0, 16, beats);
    s.countInOutput = jmax (0, output);
}

void Engine::setSongClick (int songIndex, bool on, int beatsPerBar)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    auto& s = setup.songs[(size_t) songIndex];
    s.click = on;
    s.beatsPerBar = jlimit (1, 16, beatsPerBar);
    if (songIndex == trackSong)
    {
        if (player != nullptr) player->setClick (s.click, s.beatsPerBar, s.firstBeat);
        else if (on) armTrack (songIndex);                 // no files: the click alone is the track
    }
    if (songIndex == currentSongIndex()) beatsPerBarAtomic.store (s.beatsPerBar);
    listeners.call ([] (Listener& l) { l.transportChanged(); l.setupChanged(); });
}

void Engine::setSongFirstBeat (int songIndex, double seconds)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    auto& s = setup.songs[(size_t) songIndex];
    s.firstBeat = jmax (0.0, seconds);
    if (songIndex == trackSong && player != nullptr) player->setClick (s.click, s.beatsPerBar, s.firstBeat);
    listeners.call ([] (Listener& l) { l.transportChanged(); l.setupChanged(); });
}

void Engine::setSongStart (int songIndex, double seconds)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    auto& s = setup.songs[(size_t) songIndex];
    s.startSeconds = jmax (0.0, seconds);
    if (songIndex == trackSong && player != nullptr) player->setStart (s.startSeconds);
    listeners.call ([] (Listener& l) { l.transportChanged(); l.setupChanged(); });
}

void Engine::performAction (int songIndex, const ActionDef& a)
{
    switch (a.kind)
    {
        case ActionDef::Kind::program:
            if (validInput (a.input)) selectProgram (a.input, a.program);
            break;
        case ActionDef::Kind::part:
        {
            const int track = getTrackInput();
            if (track < 0 || songIndex < 0 || songIndex >= (int) setup.songs.size()) break;
            if (const int prog = setup.songs[(size_t) songIndex].programFor (track); prog >= 0)
                setPartMuted (track, prog, a.channel, ! a.on);
            break;
        }
        case ActionDef::Kind::effect:
            if (validInput (a.input))
                setEffectBypassed (a.input, a.program >= 0 ? a.program : setup.inputs[(size_t) a.input].currentProgram, a.chain, a.effect, ! a.on);
            break;
        case ActionDef::Kind::midi:
        {
            const int ch = jlimit (1, 16, a.channel);
            MidiMessage m = a.midiType == ActionDef::MidiType::programChange ? MidiMessage::programChange (ch, a.data1)
                          : a.midiType == ActionDef::MidiType::controller    ? MidiMessage::controllerEvent (ch, a.data1, a.data2)
                                                                             : MidiMessage::noteOn (ch, a.data1, (uint8) a.data2);
            sendMidiToDevice (a.portIdentifier, a.portName, m);
            if (a.midiType == ActionDef::MidiType::noteOn)
            {
                // A cue note is a press: let it go a moment later so a lighting desk sees a key, not a stuck one.
                if (const int port = midiOuts.portFor (a.portIdentifier, a.portName); port >= 0)
                {
                    // The queue has one writer, the audio thread, which writes under the lock: so does this.
                    const ScopedLock sl (lock);
                    midiOuts.push (port, MidiMessage::noteOff (ch, a.data1), Time::getMillisecondCounterHiRes() + 100.0);
                }
            }
            break;
        }
    }
}

String Engine::describeAction (const ActionDef& a) const
{
    switch (a.kind)
    {
        case ActionDef::Kind::program:
        {
            if (! validInput (a.input)) return "a keyboard that is gone";
            const auto& in = setup.inputs[(size_t) a.input];
            const auto& pn = in.programs[(size_t) jlimit (0, InputDef::numPrograms - 1, a.program)].name;
            return in.name + " to " + String (a.program).paddedLeft ('0', 3) + (pn.isNotEmpty() ? " " + pn : String());
        }
        case ActionDef::Kind::part:
            return String (a.on ? "Unmute" : "Mute") + " the part on channel " + String (a.channel);
        case ActionDef::Kind::effect:
        {
            if (! validInput (a.input)) return "an effect on a keyboard that is gone";
            const auto& in = setup.inputs[(size_t) a.input];
            const auto& prog = in.programs[(size_t) jlimit (0, InputDef::numPrograms - 1, a.program >= 0 ? a.program : in.currentProgram)];
            String what = "effect " + String (a.effect + 1);
            if (auto* chain = prog.chainFor (a.chain); chain != nullptr && a.effect < (int) chain->size())
                what = (*chain)[(size_t) a.effect].displayName();
            const String where = a.chain < 0 ? "the whole program"
                               : ProgramDef::slotOfChain (a.chain) < (int) prog.slots.size() ? prog.slots[(size_t) ProgramDef::slotOfChain (a.chain)].displayName()
                                                                                              : String ("an instrument");
            return String (a.on ? "Turn on " : "Bypass ") + what + " (" + where + ") on " + in.name;
        }
        case ActionDef::Kind::midi:
        {
            const String what = a.midiType == ActionDef::MidiType::programChange ? "program " + String (a.data1 + 1)
                              : a.midiType == ActionDef::MidiType::controller    ? "controller " + String (a.data1) + " = " + String (a.data2)
                                                                                 : "note " + MidiMessage::getMidiNoteName (a.data1, true, true, 4) + " (" + String (a.data2) + ")";
            return "Send " + what + " on channel " + String (a.channel) + " to " + a.portName;
        }
    }
    return {};
}

Array<MidiDeviceInfo> Engine::midiOutputDevices()
{
    Array<MidiDeviceInfo> out;
    for (const auto& d : MidiOutput::getAvailableDevices())
        if (! d.name.containsIgnoreCase ("Midi Through")) out.add (d);
    return out;
}

void Engine::sendMidiToDevice (const String& identifier, const String& name, const MidiMessage& m)
{
    if (const int port = midiOuts.portFor (identifier, name); port >= 0) midiOuts.sendNow (port, m);
}

void Engine::setSongTrackBpm (int songIndex, double bpm)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    const double before = stretchOf (setup.songs[(size_t) songIndex]);
    setup.songs[(size_t) songIndex].trackBpm = bpm <= 0.0 ? 0.0 : jlimit (20.0, 300.0, bpm);
    if (songIndex == trackSong && std::abs (stretchOf (setup.songs[(size_t) songIndex]) - before) > 1e-9) armTrack (songIndex);   // the stretch is baked in: read the files again
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::setSongTransposeTrack (int songIndex, bool on)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    setup.songs[(size_t) songIndex].transposeTrack = on;
    if (songIndex == trackSong) armTrack (songIndex);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::setSongMarkers (int songIndex, const std::vector<MarkerDef>& markers)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    if (songIndex == songInPlay && currentScene >= 0)
    {
        /* The scene in play stays the scene in play when others are added or
           removed around it: found again by name and place. Gone, nothing is. */
        const auto& old = setup.songs[(size_t) songIndex].markers;
        int found = -1;
        if (currentScene < (int) old.size())
            for (int i = 0; i < (int) markers.size() && found < 0; ++i)
                if (markers[(size_t) i].name == old[(size_t) currentScene].name && std::abs (markers[(size_t) i].seconds - old[(size_t) currentScene].seconds) < 1e-6)
                    found = i;
        currentScene = found;
    }
    setup.songs[(size_t) songIndex].markers = markers;
    if (songIndex == trackSong && player != nullptr)
    {
        const ScopedLock sl (lock);
        player->setMarkers (markers);
    }
    listeners.call ([] (Listener& l) { l.transportChanged(); });
}

void Engine::queueTrackSection (int index)
{
    if (! hasTrack()) { selectScene (index); return; }        // a song played by hand: its scenes change at once
    if (player == nullptr) return;
    player->queueSection (index);
    listeners.call ([] (Listener& l) { l.transportChanged(); });
}

void Engine::releaseVamp()
{
    if (player == nullptr) return;
    player->releaseVamp();
    listeners.call ([] (Listener& l) { l.transportChanged(); });
}

void Engine::advanceTrack()
{
    if (! hasTrack())
    {
        if (currentScene + 1 < getSceneCount()) selectScene (currentScene + 1);
        return;
    }
    if (player == nullptr) return;
    player->advance();
    listeners.call ([] (Listener& l) { l.transportChanged(); });
}

void Engine::setLinkEnabled (bool on)
{
    if (link == nullptr) return;
    if (on)
    {
        /* Join at the rig's tempo, told to Link before it is switched on:
           switching on makes Link reset its session on its own thread, keeping
           the session's tempo as it stands then. Told after, the reset could
           land second and put the old tempo back, and its tempo callback would
           carry the rig off to it. Both are queued to the same thread in order,
           so first the tempo, then the switch. Peers are not known yet at this
           point; when the sessions merge, Link settles the tempo and the tempo
           callback brings the rig along if it is not ours. */
        auto st = link->captureAppSessionState();
        st.setTempo (setup.tempoBpm, link->clock().micros());
        link->commitAppSessionState (st);
    }
    link->enable (on);
    settings.setValue ("linkEnabled", on);
    settings.saveIfNeeded();
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::setLinkStartStopSync (bool on)
{
    if (link == nullptr) return;
    link->enableStartStopSync (on);
    settings.setValue ("linkStartStop", on);
    settings.saveIfNeeded();
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

int64 Engine::linkStartDelay (int countInBeats, int beatsPerBar) const
{
    if (link == nullptr || ! link->isEnabled() || linkPeers.load() == 0) return 0;
    const double q = jmax (1, beatsPerBar);
    const auto st = link->captureAppSessionState();
    const auto at = link->clock().micros() + std::chrono::microseconds ((int64) (outputLatencyMs * 1000.0));
    double beats = q - st.phaseAtTime (at, q);                        // to the next bar line
    beats -= countInBeats;                                            // the count-in ends on it
    while (beats < 0.0) beats += q;
    return (int64) (beats * 60.0 / jmax (20.0, st.tempo()) * sampleRate);
}

Engine::LoopState Engine::getLoopState (int i) const
{
    return i >= 0 && i < (int) runtimes.size() ? (LoopState) runtimes[(size_t) i]->loop.state.load() : LoopState::empty;
}

int Engine::getLoopBars (int i) const
{
    return i >= 0 && i < (int) runtimes.size() ? runtimes[(size_t) i]->loop.bars.load() : 0;
}

void Engine::loopPedal (int i)
{
    if (i < 0 || i >= (int) runtimes.size()) return;
    runtimes[(size_t) i]->loop.command.store (1);
}

void Engine::loopClear (int i)
{
    if (i < 0 || i >= (int) runtimes.size()) return;
    runtimes[(size_t) i]->loop.command.store (2);
}

void Engine::loopStop (int i)
{
    if (i < 0 || i >= (int) runtimes.size()) return;
    runtimes[(size_t) i]->loop.command.store (3);
}


void Engine::loopProcess (InputRuntime& in, int numSamples, bool panicNow)
{
    auto& lp = in.loop;
    if (lp.events.capacity() == 0) return;                 // never sized: an input made before the looper (not expected)
    auto releaseHeld = [&]
    {
        for (int c = 0; c < 16; ++c)
            for (int k = 0; k < 128; ++k)
                if (lp.held[c][k]) { in.block.addEvent (MidiMessage::noteOff (c + 1, k), 0); lp.held[c][k] = 0; }
    };
    if (panicNow) std::memset (lp.held, 0, sizeof (lp.held));

    const double bpm = jmax (20.0, tempoAtomic.load (std::memory_order_relaxed));
    const int64 barSamples = jmax ((int64) 1, (int64) (sampleRate * 60.0 / bpm * jmax (1, beatsPerBarAtomic.load (std::memory_order_relaxed))));

    const int cmd = lp.command.exchange (0);
    auto st = (LoopState) lp.state.load();
    if (cmd == 2)
    {
        releaseHeld();
        lp.events.clear();
        lp.length = lp.pos = lp.recorded = 0;
        lp.bars.store (0);
        st = LoopState::empty;
    }
    else if (cmd == 3)
    {
        if (st == LoopState::playing) { releaseHeld(); st = LoopState::stopped; }
        else if (st == LoopState::recording) { lp.events.clear(); lp.recorded = 0; st = LoopState::empty; }
    }
    else if (cmd == 1)
    {
        switch (st)
        {
            case LoopState::empty:
                lp.events.clear();
                lp.recorded = 0;
                st = LoopState::recording;
                break;
            case LoopState::recording:
            {
                // Whole bars, the nearest; at least one. What was played past the end is dropped.
                const int64 bars = jmax ((int64) 1, (int64) std::llround ((double) lp.recorded / (double) barSamples));
                lp.length = bars * barSamples;
                /* Past the end: a note-off whose note-on is inside the loop
                   moves to the last sample, so the note is let go each pass;
                   anything else there is dropped. */
                for (auto& e : lp.events)
                    if (e.at >= lp.length && e.size == 3 && ((e.data[0] & 0xf0) == 0x80 || ((e.data[0] & 0xf0) == 0x90 && e.data[2] == 0)))
                        e.at = lp.length - 1;
                lp.events.erase (std::remove_if (lp.events.begin(), lp.events.end(), [&] (const InputRuntime::Looper::Ev& e) { return e.at >= lp.length; }), lp.events.end());
                std::stable_sort (lp.events.begin(), lp.events.end(), [] (const InputRuntime::Looper::Ev& a, const InputRuntime::Looper::Ev& b) { return a.at < b.at; });
                lp.bars.store ((int) bars);
                lp.pos = 0;
                st = LoopState::playing;
                break;
            }
            case LoopState::playing:
                releaseHeld();
                st = LoopState::stopped;
                break;
            case LoopState::stopped:
                lp.pos = 0;
                st = LoopState::playing;
                break;
        }
    }
    lp.state.store ((int) st);

    if (st == LoopState::recording)
    {
        for (const auto meta : in.block)
        {
            const auto m = meta.getMessage();
            if (m.getRawDataSize() > 3 || m.isProgramChange() || m.getChannel() == 0) continue;
            if (lp.events.size() >= lp.events.capacity()) break;      // full: the loop keeps what it has, never allocates here
            InputRuntime::Looper::Ev e;
            e.at = lp.recorded + meta.samplePosition;
            e.size = (uint8) m.getRawDataSize();
            std::memcpy (e.data, m.getRawData(), e.size);
            lp.events.push_back (e);
        }
        lp.recorded += numSamples;
    }
    else if (st == LoopState::playing && lp.length > 0)
    {
        auto play = [&] (int64 from, int64 to, int offset)
        {
            for (const auto& e : lp.events)
                if (e.at >= from && e.at < to)
                {
                    const int at = (int) (e.at - from) + offset;
                    in.block.addEvent (e.data, (int) e.size, at);
                    const MidiMessage m (e.data, (int) e.size);
                    if (m.getChannel() >= 1)
                    {
                        if (m.isNoteOn()) lp.held[m.getChannel() - 1][m.getNoteNumber()] = 1;
                        else if (m.isNoteOff()) lp.held[m.getChannel() - 1][m.getNoteNumber()] = 0;
                    }
                }
        };
        const int64 end = lp.pos + numSamples;
        if (end <= lp.length)
            play (lp.pos, end, 0);
        else
        {
            play (lp.pos, lp.length, 0);
            play (0, end - lp.length, (int) (lp.length - lp.pos));
        }
        lp.pos = end % lp.length;
    }
}


void Engine::armTrack (int songIndex)
{
    // Whatever is playing stops first, with its short fade, so a change of song never clicks.
    if (player != nullptr && player->isRunning())
    {
        player->stop();
        for (int i = 0; i < 20 && player->isRunning(); ++i) Thread::sleep (5);
    }

    // Read into memory on its own thread; Play before it is done waits for it.
    std::unique_ptr<SongPlayer> fresh;
    if (songIndex >= 0 && songIndex < (int) setup.songs.size()
        && (! setup.songs[(size_t) songIndex].stems.empty() || setup.songs[(size_t) songIndex].click))
    {
        fresh = std::make_unique<SongPlayer> (trackFormats, getChartsFolder(),
                                              setup.songs[(size_t) songIndex], sampleRate, blockSize);
        trackProblemsReported = false;
    }

    std::unique_ptr<SongPlayer> gone;
    {
        const ScopedLock sl (lock);
        gone = std::move (player);
        player = std::move (fresh);
    }
    trackSong = songIndex;
    listeners.call ([] (Listener& l) { l.transportChanged(); });
}

File Engine::getGmSoundfont() const
{
    const auto path = settings.getValue ("gmSoundfont");
    return path.isNotEmpty() && File::isAbsolutePath (path) ? File (path) : File();
}

void Engine::setGmSoundfont (const File& f)
{
    settings.setValue ("gmSoundfont", f.getFullPathName());
    settings.saveIfNeeded();
    // Every GM player reads the new soundfont.
    for (auto& in : runtimes)
        for (auto& [p, rt] : in->loaded)
            for (auto& sd : rt->slots)
                if (sd->isGm) queueGmLoad (*sd);
}

void Engine::startTrack()
{
    if (player == nullptr || ! hasTrack()) return;
    if (player->isRunning()) return;
    if (! player->isReady())
        listeners.call ([] (Listener& l) { l.statusMessage ("Backing track still loading -- it starts as soon as it is ready."); });

    int beats = 0, output = 0;
    double bpm = setup.tempoBpm > 0.0 ? setup.tempoBpm : 120.0;
    if (trackSong >= 0 && trackSong < (int) setup.songs.size())
    {
        const auto& s = setup.songs[(size_t) trackSong];
        beats = s.countInBeats;
        output = s.countInOutput;
        if (s.tempoBpm > 0.0) bpm = s.tempoBpm;
    }
    const int bpb = trackSong >= 0 && trackSong < (int) setup.songs.size() ? setup.songs[(size_t) trackSong].beatsPerBar : 4;
    const bool onLinkBar = link != nullptr && link->isEnabled() && linkPeers.load() > 0;
    // Not loaded yet with Link on: the bar line is only worth computing when the files can actually start, so the start is held and placed by housekeeping.
    player->start (beats, 60.0 / bpm, output, onLinkBar ? (player->isReady() ? linkStartDelay (beats, bpb) : -1) : 0);
    linkStartBeats = beats; linkStartBpb = bpb;
    if (link != nullptr && link->isEnabled() && link->isStartStopSyncEnabled())
    {
        auto st = link->captureAppSessionState();
        if (! st.isPlaying()) { st.setIsPlaying (true, link->clock().micros()); link->commitAppSessionState (st); }
    }
    listeners.call ([] (Listener& l) { l.transportChanged(); });
}

void Engine::stopTrack()
{
    if (player == nullptr || ! player->isRunning()) return;
    player->stop();
    if (link != nullptr && link->isEnabled() && link->isStartStopSyncEnabled())
    {
        auto st = link->captureAppSessionState();
        if (st.isPlaying()) { st.setIsPlaying (false, link->clock().micros()); link->commitAppSessionState (st); }
    }
    listeners.call ([] (Listener& l) { l.transportChanged(); });
}

void Engine::toggleTrack()
{
    if (isTrackRunning()) stopTrack(); else startTrack();
}


void Engine::resetPluginPositions()
{
    /* Bar one, beat one, for everything that could sync to the track: a
       tempo-synced delay or arpeggiator then lands on the track's beat
       rather than wherever its own count had wandered to. */
    for (auto& in : runtimes)
        for (auto& [prog, rt] : in->loaded)
        {
            for (auto& s : rt->slots)
            {
                if (s->plugin != nullptr) s->plugin->resetMusicalPosition();
                if (s->gm != nullptr) { s->gm->reset(); s->gmDirty.store (true); }   // the GM player starts the song clean
                for (auto* ch : { &s->midiEffects, &s->effects }) for (auto& e : *ch)
                {
                    if (e->plugin != nullptr) e->plugin->resetMusicalPosition();
                    if (e->builtIn != nullptr) e->builtIn->restartTiming();      // not reset(): held notes must still get their note-offs
                }
            }
            for (auto& e : rt->effects) if (e->plugin != nullptr) e->plugin->resetMusicalPosition();
        }
    for (auto& o : outputs)
        for (auto& e : o->chain.effects) if (e->plugin != nullptr) e->plugin->resetMusicalPosition();
}

int Engine::addSet (const String& name)
{
    SetDef s;
    s.name = name.isNotEmpty() ? name : "Set list " + String ((int) setup.sets.size() + 1);
    setup.sets.push_back (std::move (s));
    listeners.call ([] (Listener& l) { l.setupChanged(); });
    return (int) setup.sets.size() - 1;
}

void Engine::removeSet (int setIndex)
{
    if (setIndex < 0 || setIndex >= (int) setup.sets.size()) return;
    setup.sets.erase (setup.sets.begin() + setIndex);
    if (setup.currentSet == setIndex)      { setup.currentSet = -1; setup.currentSongInSet = -1; }
    else if (setup.currentSet > setIndex)  --setup.currentSet;
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

int Engine::moveSet (int setIndex, int delta)
{
    const int n = (int) setup.sets.size();
    const int to = setIndex + delta;
    if (setIndex < 0 || setIndex >= n || to < 0 || to >= n) return setIndex;

    std::swap (setup.sets[(size_t) setIndex], setup.sets[(size_t) to]);

    // Keep playing the set you were playing, wherever it has moved to.
    if (setup.currentSet == setIndex)   setup.currentSet = to;
    else if (setup.currentSet == to)    setup.currentSet = setIndex;

    listeners.call ([] (Listener& l) { l.setupChanged(); });
    return to;
}

void Engine::setSetName (int setIndex, const String& name)
{
    if (setIndex < 0 || setIndex >= (int) setup.sets.size()) return;
    setup.sets[(size_t) setIndex].name = name;
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::setSetSongs (int setIndex, const std::vector<int>& songIndices)
{
    if (setIndex < 0 || setIndex >= (int) setup.sets.size()) return;

    std::vector<int> clean;
    for (int i : songIndices)
        if (i >= 0 && i < (int) setup.songs.size())
            clean.push_back (i);          // a song may appear twice: encores happen

    setup.sets[(size_t) setIndex].songs = std::move (clean);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::addSongToSet (int setIndex, int songIndex)
{
    if (setIndex < 0 || setIndex >= (int) setup.sets.size() || songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    setup.sets[(size_t) setIndex].songs.push_back (songIndex);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::removeFromSet (int setIndex, int position)
{
    if (setIndex < 0 || setIndex >= (int) setup.sets.size()) return;
    auto& songs = setup.sets[(size_t) setIndex].songs;
    if (position < 0 || position >= (int) songs.size()) return;
    songs.erase (songs.begin() + position);
    if (setup.currentSet == setIndex)
    {
        if (setup.currentSongInSet == position)     setup.currentSongInSet = -1;
        else if (setup.currentSongInSet > position) --setup.currentSongInSet;
    }
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::moveInSet (int setIndex, int position, int delta)
{
    if (setIndex < 0 || setIndex >= (int) setup.sets.size()) return;
    auto& songs = setup.sets[(size_t) setIndex].songs;
    const int to = position + delta;
    if (position < 0 || position >= (int) songs.size() || to < 0 || to >= (int) songs.size() || to == position) return;
    std::swap (songs[(size_t) position], songs[(size_t) to]);
    if (setup.currentSet == setIndex)
    {
        if (setup.currentSongInSet == position)  setup.currentSongInSet = to;
        else if (setup.currentSongInSet == to)   setup.currentSongInSet = position;
    }
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

int Engine::duplicateSet (int setIndex, const String& name)
{
    if (setIndex < 0 || setIndex >= (int) setup.sets.size()) return -1;
    SetDef copy = setup.sets[(size_t) setIndex];
    copy.name = name.isNotEmpty() ? name : copy.name + " copy";
    setup.sets.push_back (std::move (copy));
    listeners.call ([] (Listener& l) { l.setupChanged(); });
    return (int) setup.sets.size() - 1;
}

void Engine::selectSet (int setIndex)
{
    if (setIndex < -1 || setIndex >= (int) setup.sets.size()) return;
    setup.currentSet = setIndex;
    setup.currentSongInSet = -1;          // chosen, not started
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::selectSongInSet (int position)
{
    if (setup.currentSet < 0 || setup.currentSet >= (int) setup.sets.size()) return;

    const auto& st = setup.sets[(size_t) setup.currentSet];
    if (position < 0 || position >= (int) st.songs.size()) return;

    setup.currentSongInSet = position;
    selectSong (st.songs[(size_t) position]);
}

void Engine::nextSong()
{
    if (setup.currentSet < 0 || setup.currentSet >= (int) setup.sets.size()) return;
    const auto& st = setup.sets[(size_t) setup.currentSet];
    if (st.songs.empty()) return;

    /* Stops at the end rather than wrapping. Wrapping to the first number after
       the last one is the wrong thing to do to a band that has just finished. */
    const int next = setup.currentSongInSet + 1;
    if (next < (int) st.songs.size()) selectSongInSet (next);
}

void Engine::previousSong()
{
    if (setup.currentSet < 0) return;
    if (setup.currentSongInSet > 0) selectSongInSet (setup.currentSongInSet - 1);
}



//==============================================================================
void Engine::selectProgram (int inputIndex, int program)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;

    auto& def = setup.inputs[(size_t) inputIndex];
    auto& in = *runtimes[(size_t) inputIndex];
    def.currentProgram = program;

    auto* next = ensureProgramLoaded (inputIndex, program);

    // `active` only changes on this thread, so reading it outside the lock is safe.
    ProgramRuntime* const outgoing = in.active != next ? in.active : nullptr;

    {
        const ScopedLock sl (lock);
        if (in.active != next)
        {
            if (in.active != nullptr)
            {
                /* Leave it sounding. With a key or the pedal down it carries
                   on receiving the note-offs and pedals until the player lets
                   go, and only then starts its tail -- a pad held under the
                   change, or the pedal down over a bar line, is not cut. The
                   alternative releases everything now and lets the tail cover
                   the ringing out. */
                in.active->holding = setup.holdAcrossChange && in.active->anythingHeld();
                in.active->needsNotesOff = ! in.active->holding;
                in.active->releaseSamplesLeft = (int) (setup.releaseTailSeconds * sampleRate);
                if (! in.isReleasing (in.active))
                    in.releasing.push_back (in.active);
            }
            in.stopReleasing (next);
            next->holding = false;
            next->releaseSamplesLeft = 0;
            in.active = next;
        }
    }
    // External instruments switch to their patch with the program.
    sendPatches (*next, setup.inputs[(size_t) inputIndex].programs[(size_t) program]);
    ensureLentLoaded();                 // the programs its slots name on other inputs

    /* The outgoing program's state into the model, now that the new sound has
       taken over. Anything set inside a plugin's own window -- SWAM being
       told which controller is expression -- lives only in that process
       until it is captured, and switching away is the natural moment: the
       change already marks the setup dirty and brings the autosave forward.
       After the switch, never before it: a plugin slow to answer (a dialog
       up in its window, a big library to write out) held the change itself,
       up to thirty seconds, and was then put down while it still played.
       Only the plugins that may have changed, for a second at most each; one
       that does not answer in that keeps what the model has and is asked
       again by the autosave. It is still loaded: unloading comes later, on
       this thread. */
    if (outgoing != nullptr)
        captureProgramState (inputIndex, *outgoing, 1000, true);

    listeners.call ([=] (Listener& l) { l.programChanged (inputIndex, program); });
}

void Engine::setProgramName (int inputIndex, int program, const String& name)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    setup.inputs[(size_t) inputIndex].programs[(size_t) program].name = name;
    // The slots elsewhere that play this program by name follow it.
    for (auto& in : setup.inputs)
        for (auto& p : in.programs)
            for (auto& sd : p.slots)
                if (sd.isInput() && sd.targetInput == inputIndex && sd.targetProgram == program) sd.targetProgramName = name;
}

void Engine::setProgramGroup (int inputIndex, int program, const String& group)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    const auto trimmed = group.trim();
    if (def.group == trimmed) return;
    def.group = trimmed;
    listeners.call ([=] (Listener& l) { l.programContentChanged (inputIndex, program); });
}

StringArray Engine::getProgramGroups (int inputIndex) const
{
    StringArray out;
    if (! validInput (inputIndex)) return out;
    for (const auto& p : setup.inputs[(size_t) inputIndex].programs)
        if (p.group.isNotEmpty())
            out.addIfNotAlreadyThere (p.group);
    return out;
}

void Engine::copyProgram (int inputIndex, int from, int to)
{
    if (! validInput (inputIndex) || ! validProgram (from) || ! validProgram (to) || from == to) return;
    if (auto* rt = getLoaded (inputIndex, from))
        captureProgramState (inputIndex, *rt);

    clearProgram (inputIndex, to);
    auto& in = setup.inputs[(size_t) inputIndex];
    in.programs[(size_t) to] = in.programs[(size_t) from];

    // If the destination is loaded (e.g. it's the active program), rebuild it live.
    if (getLoaded (inputIndex, to) != nullptr)
    {
        unloadProgram (*runtimes[(size_t) inputIndex], to);
        auto* prog = ensureProgramLoaded (inputIndex, to);
        const ScopedLock sl (lock);
        if (in.currentProgram == to)
            runtimes[(size_t) inputIndex]->active = prog;
    }
    notifyContent (inputIndex, to);
}

void Engine::clearProgram (int inputIndex, int program)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& in = *runtimes[(size_t) inputIndex];
    const bool wasActive = (in.active != nullptr && in.active->program == program);

    unloadProgram (in, program);
    setup.inputs[(size_t) inputIndex].programs[(size_t) program] = ProgramDef();

    if (wasActive)
    {
        auto* prog = ensureProgramLoaded (inputIndex, program);
        const ScopedLock sl (lock);
        in.active = prog;
    }
    notifyContent (inputIndex, program);
}

Engine::ProgramRuntime* Engine::ensureProgramLoaded (int inputIndex, int program)
{
    if (auto* rt = getLoaded (inputIndex, program))
        return rt;

    auto built = buildProgram (inputIndex, program);
    auto* raw = built.get();
    {
        const ScopedLock sl (lock);
        runtimes[(size_t) inputIndex]->loaded[program] = std::move (built);
    }
    notifyContent (inputIndex, program);
    return raw;
}

std::unique_ptr<Engine::ProgramRuntime> Engine::buildProgram (int inputIndex, int program)
{
    auto rt = std::make_unique<ProgramRuntime>();
    rt->program = program;
    rt->exact = &runtimes[(size_t) inputIndex]->exact;
    rt->mix.setSize (2, blockSize);
    rt->programMidi.ensureSize (2048);
    rt->sentMidi.ensureSize (2048);
    rt->received.ensureSize (2048);

    const auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    rt->inputGain.store (Decibels::decibelsToGain (def.inputGainDb, -60.0f));
    for (int s = 0; s < (int) def.slots.size(); ++s)
        rt->slots.push_back (buildSlot (inputIndex, program, s, def.slots[(size_t) s]));
    for (int e = 0; e < (int) def.effects.size(); ++e)
        rt->effects.push_back (buildEffect (inputIndex, program, -1, e, def.effects[(size_t) e]));

    resolveMappings (*rt, def);
    return rt;
}

std::unique_ptr<Engine::SlotRuntime> Engine::buildSlot (int inputIndex, int program, int slotIndex, const SlotDef& def)
{
    auto slot = std::make_unique<SlotRuntime> (*this, inputIndex, program, slotIndex);
    slot->applyDef (def);
    slot->stereo.setSize (2, blockSize);
    slot->midi.ensureSize (16384);
    nodeRegistry[slot->id] = slot.get();
    if (def.isExternal())
    {
        // No process to start: the instrument is the hardware, reached through its port.
        slot->external = true;
        slot->outPort.store (midiOuts.portFor (def.midiOutIdentifier, def.midiOutName));
    }
    else if (def.isGm())
    {
        slot->isGm = true;
        slot->gmMidi.ensureSize (32768);
        queueGmLoad (*slot);
    }
    else if (def.isInput())
        slot->isInputSlot = true;      // nothing to start: the notes go to another input
    else
        queueLoad (*slot, def.plugin, def.state);
    for (int e = 0; e < (int) def.midiEffects.size(); ++e)
        slot->midiEffects.push_back (buildEffect (inputIndex, program, ProgramDef::midiChainOf (slotIndex), e, def.midiEffects[(size_t) e]));
    for (int e = 0; e < (int) def.effects.size(); ++e)
        slot->effects.push_back (buildEffect (inputIndex, program, slotIndex, e, def.effects[(size_t) e]));
    return slot;
}

std::unique_ptr<Engine::EffectRuntime> Engine::buildEffect (int inputIndex, int program, int slotIndex, int effectIndex, const EffectDef& def)
{
    auto fx = std::make_unique<EffectRuntime> (*this, inputIndex, program, slotIndex, effectIndex);
    fx->bypassed.store (def.bypassed);
    fx->midi.ensureSize (2048);
    fx->produced.ensureSize (2048);
    nodeRegistry[fx->id] = fx.get();
    if (def.isSend())
    {
        fx->send = true;
        fx->sendChannel.store (def.channel);
        fx->outPort.store (midiOuts.portFor (def.portIdentifier, def.portName));
        return fx;
    }
    if (def.isBuiltIn())
    {
        fx->builtIn = MidiFx::create (effectiveEffectDef (def));
        return fx;
    }
    queueLoad (*fx, def.plugin, def.state);
    return fx;
}

void Engine::queueLoad (PluginNode& node, const PluginDescription& desc, const MemoryBlock& state)
{
    node.loading.store (true);
    node.loadError.clear();
    ++pendingLoads;
    const bool bridged = PluginHost::isWineBridged (desc.fileOrIdentifier);   // filesystem look-up: outside the lock
    {
        std::lock_guard<std::mutex> l (loaderMutex);
        loadQueue.push_back ({ node.id, desc, state, sampleRate, blockSize, bridged });
    }
    loaderCv.notify_one();
}

void Engine::queueGmLoad (SlotRuntime& slot)
{
    slot.loading.store (true);
    slot.loadError.clear();
    ++pendingLoads;
    LoadJob job;
    job.nodeId = slot.id;
    job.sampleRate = sampleRate;
    job.blockSize = blockSize;
    job.gm = true;
    job.soundfont = GmSynth::findSoundfont (getGmSoundfont());
    {
        std::lock_guard<std::mutex> l (loaderMutex);
        loadQueue.push_back (std::move (job));
    }
    loaderCv.notify_one();
}

/** Any wineserver on this machine means a Wine prefix is booted; the first bridged
    plugin then starts as fast as the rest and need not go alone. */
bool Engine::wineserverRunning()
{
    for (const auto& d : File ("/proc").findChildFiles (File::findDirectories, false))
        if (d.getFileName().containsOnly ("0123456789")
            && d.getChildFile ("comm").loadFileAsString().trim() == "wineserver")
            return true;
    return false;
}

bool Engine::takeJob (LoadJob& out)
{
    for (auto it = loadQueue.begin(); it != loadQueue.end(); ++it)
    {
        if (it->bridged)
        {
            const int limit = bridgedWarm ? bridgedParallel : 1;
            if (bridgedInFlight >= limit) continue;      // leave it for later, try a native one
            ++bridgedInFlight;
        }
        out = std::move (*it);
        loadQueue.erase (it);
        return true;
    }
    return false;
}

void Engine::loaderThreadFunc()
{
    for (;;)
    {
        LoadJob job;
        {
            std::unique_lock<std::mutex> l (loaderMutex);
            loaderCv.wait (l, [this, &job] { return loaderQuit.load() || takeJob (job); });
            if (loaderQuit.load()) return;
        }

        if (job.gm)
        {
            // The General MIDI player: a soundfont read, not a process.
            LoadResult result { job.nodeId, nullptr, nullptr, {}, job.sampleRate, job.blockSize };
            ++loadsInFlight;
            if (job.soundfont == File()) result.error = GmSynth::unavailableReason (getGmSoundfont());
            else result.gm = GmSynth::create (job.sampleRate, job.soundfont, result.error);
            {
                std::lock_guard<std::mutex> l (loaderMutex);
                loadResults.push_back (std::move (result));
            }
            --loadsInFlight;
            postEvent ({ Event::pluginLoaded, 0, 0, 0, 0, 0 });
            continue;
        }

        LoadResult result { job.nodeId, std::make_unique<RemotePlugin>(), nullptr, {}, job.sampleRate, job.blockSize };
        {
            std::lock_guard<std::mutex> l (loaderMutex);
            loadsInProgress.push_back (result.plugin.get());
        }
        ++loadsInFlight;

        if (! result.plugin->load (job.desc, job.sampleRate, job.blockSize, result.error))
        {
            if (const auto missing = PluginHost::brokenBridgeTarget (job.desc.fileOrIdentifier); missing.isNotEmpty())
                result.error << "  (its yabridge link points to a missing file: " << missing << " -- reinstall the plugin, then run yabridgectl sync and rescan)";
            {
                std::lock_guard<std::mutex> l (loaderMutex);
                loadsInProgress.erase (std::remove (loadsInProgress.begin(), loadsInProgress.end(), result.plugin.get()), loadsInProgress.end());
            }
            result.plugin->shutdown();
            result.plugin.reset();
        }
        else if (job.state.getSize() > 0 && ! result.plugin->setState (job.state))
        {
            result.error = job.desc.name + ": could not restore its saved state (" + result.plugin->getLastError() + ")";
        }

        const bool ok = result.plugin != nullptr;
        {
            std::lock_guard<std::mutex> l (loaderMutex);
            loadsInProgress.erase (std::remove (loadsInProgress.begin(), loadsInProgress.end(),
                                                ok ? result.plugin.get() : nullptr), loadsInProgress.end());
            if (job.bridged) { --bridgedInFlight; if (ok) bridgedWarm = true; }
            loadResults.push_back (std::move (result));
        }
        --loadsInFlight;
        loaderCv.notify_all();      // a bridged slot may have opened up
        postEvent ({ Event::pluginLoaded, 0, 0, 0, 0, 0 });
    }
}

void Engine::attachLoaded (LoadResult& r)
{
    --pendingLoads;
    auto it = nodeRegistry.find (r.nodeId);
    if (it == nodeRegistry.end())
    {
        // The node went away while its plugin was loading (program unloaded, slot removed).
        if (r.plugin != nullptr) r.plugin->shutdown();
        return;
    }
    auto* node = it->second;
    node->loading.store (false);

    if (auto* slot = dynamic_cast<SlotRuntime*> (node); slot != nullptr && slot->isGm)
    {
        if (r.gm == nullptr)
        {
            node->loadError = r.error;
            listeners.call ([&] (Listener& l) { l.statusMessage ("General MIDI player: " + r.error); });
        }
        else
        {
            std::unique_ptr<GmSynth> old;
            {
                const ScopedLock sl (lock);
                old = std::move (slot->gm);
                slot->gm = std::move (r.gm);
                slot->gmDirty.store (true);
            }
        }
        notifyContent (node->inputIndex.load(), node->program);
        return;
    }

    if (r.plugin == nullptr)
    {
        node->loadError = r.error;
        /* A restart after a crash that did not load -- Wine still going down
           with the hosts it took along -- is tried again in a few seconds,
           within the same limit as the restarts themselves; it was tried
           once and then left off for good. */
        const auto now = Time::getMillisecondCounter();
        auto& times = restartTimes[node->id];
        times.erase (std::remove_if (times.begin(), times.end(), [now] (uint32 t) { return now - t > 60000; }), times.end());
        if (! times.empty() && (int) times.size() < maxAutoRestartsPerMinute)
        {
            times.push_back (now);
            loadRetries.emplace_back (node->id, now + 4000);
            listeners.call ([&] (Listener& l) { l.statusMessage ("The plugin did not start again (" + r.error + "). Trying once more..."); });
        }
        else
            listeners.call ([&] (Listener& l) { l.statusMessage ("Failed to load plugin: " + r.error); });
    }
    else
    {
        if (r.error.isNotEmpty())
            listeners.call ([&] (Listener& l) { l.statusMessage (r.error); });
        if (r.sampleRate != sampleRate || r.blockSize != blockSize)
            r.plugin->prepare (sampleRate, blockSize);      // device changed while loading
        // A plugin that has just loaded knows nothing about the rig's tempo.
        r.plugin->setTempo (setup.tempoBpm, 4, 4);
        r.plugin->setListener (node);
        {
            const ScopedLock sl (lock);
            node->plugin = std::move (r.plugin);
        }
        /* Dead on arrival: its process went between loading and now (while its
           state was being set, say), before there was anyone to tell. Told
           now, so it is started again like any other that stops. */
        if (! node->plugin->isAlive())
            notePluginDied (node->inputIndex.load(), node->program, node->slotIndex.load(), node->effectIndex.load());
    }

    const int inputIndex = node->inputIndex.load();
    const int program = node->program;
    if (validInput (inputIndex) && validProgram (program))
        if (auto* rt = getLoaded (inputIndex, program))
        {
            const ScopedLock sl (lock);
            resolveMappings (*rt, setup.inputs[(size_t) inputIndex].programs[(size_t) program]);
        }
    notifyContent (inputIndex, program);
}

void Engine::unloadProgram (InputRuntime& in, int program)
{
    std::unique_ptr<ProgramRuntime> rt;
    {
        const ScopedLock sl (lock);
        auto it = in.loaded.find (program);
        if (it == in.loaded.end()) return;
        rt = std::move (it->second);
        in.loaded.erase (it);
        if (in.active == rt.get()) in.active = nullptr;
        in.stopReleasing (rt.get());
    }

    // Find our input index to capture state into the right definition.
    // What changed, within three seconds a plugin: it is going anyway, and the screen must not wait on it.
    for (int i = 0; i < (int) runtimes.size(); ++i)
        if (runtimes[(size_t) i].get() == &in)
            captureProgramState (i, *rt, 3000, true);

    destroyProgramPlugins (*rt);
}

void Engine::destroyProgramPlugins (ProgramRuntime& rt, int patienceMs)
{
    for (auto& s : rt.slots)
    {
        for (auto* ch : { &s->midiEffects, &s->effects }) for (auto& e : *ch) destroyNode (e.get(), patienceMs);
        destroyNode (s.get(), patienceMs);
    }
    for (auto& e : rt.effects) destroyNode (e.get(), patienceMs);
    rt.slots.clear();
    rt.effects.clear();
}

void Engine::destroyNode (PluginNode* node, int patienceMs)
{
    if (node == nullptr) return;
    nodeRegistry.erase (node->id);       // a load still in flight will be discarded on arrival
    node->loading.store (false);
    if (node->plugin == nullptr) return;
    node->plugin->setListener (nullptr);
    node->plugin->shutdown (patienceMs);
    node->plugin.reset();
}

bool Engine::captureProgramState (int inputIndex, ProgramRuntime& rt, int timeoutMs, bool onlyChanged)
{
    auto* defPtr = defFor (inputIndex, rt.program);
    if (defPtr == nullptr) return false;
    auto& def = *defPtr;

    bool changed = false;
    auto capture = [&changed, timeoutMs, onlyChanged] (PluginNode& node, MemoryBlock& dest)
    {
        if (node.alive() && (! onlyChanged || node.plugin->stateMayHaveChanged()))
        {
            MemoryBlock mb;
            if (node.plugin->getState (mb, timeoutMs) && mb.getSize() > 0 && mb != dest)
            {
                dest = std::move (mb);
                changed = true;
            }
        }
    };

    for (size_t s = 0; s < rt.slots.size() && s < def.slots.size(); ++s)
    {
        capture (*rt.slots[s], def.slots[s].state);
        auto& fxRt = rt.slots[s]->effects;
        auto& fxDef = def.slots[s].effects;
        for (size_t e = 0; e < fxRt.size() && e < fxDef.size(); ++e)
            capture (*fxRt[e], fxDef[e].state);
        auto& mRt = rt.slots[s]->midiEffects;
        auto& mDef = def.slots[s].midiEffects;
        for (size_t e = 0; e < mRt.size() && e < mDef.size(); ++e)
            capture (*mRt[e], mDef[e].state);     // a send has no plugin: alive() is false and it is skipped
    }
    for (size_t e = 0; e < rt.effects.size() && e < def.effects.size(); ++e)
        capture (*rt.effects[e], def.effects[e].state);
    return changed;
}

//==============================================================================
// Slots
//==============================================================================
bool Engine::addSlot (int inputIndex, int program, const PluginDescription& desc, String& error)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return false;

    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    SlotDef sd;
    sd.plugin = desc;
    def.slots.push_back (sd);
    const int slotIndex = (int) def.slots.size() - 1;

    bool ok = true;
    if (auto* rt = getLoaded (inputIndex, program))
    {
        auto slot = buildSlot (inputIndex, program, slotIndex, sd);
        error.clear();     // loading continues in the background; failures arrive as status messages
        const ScopedLock sl (lock);
        rt->slots.push_back (std::move (slot));
    }
    notifyContent (inputIndex, program);
    return ok;
}

void Engine::removeSlot (int inputIndex, int program, int slotIndex)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slotIndex < 0 || slotIndex >= (int) def.slots.size()) return;

    // Fix up mappings that point at this or later slots.
    for (auto it = def.mappings.begin(); it != def.mappings.end();)
    {
        const int owner = ProgramDef::slotOfChain (it->slot);    // its MIDI chain goes with it
        if (owner == slotIndex)          it = def.mappings.erase (it);
        else { if (owner > slotIndex) --it->slot; ++it; }
    }
    def.slots.erase (def.slots.begin() + slotIndex);

    if (auto* rt = getLoaded (inputIndex, program))
    {
        std::unique_ptr<SlotRuntime> removed;
        {
            const ScopedLock sl (lock);
            if (slotIndex < (int) rt->slots.size())
            {
                removed = std::move (rt->slots[(size_t) slotIndex]);
                rt->slots.erase (rt->slots.begin() + slotIndex);
                for (int s = slotIndex; s < (int) rt->slots.size(); ++s)
                {
                    rt->slots[(size_t) s]->slotIndex.store (s);
                    for (auto& e : rt->slots[(size_t) s]->effects) e->slotIndex.store (s);
                    for (auto& e : rt->slots[(size_t) s]->midiEffects) e->slotIndex.store (ProgramDef::midiChainOf (s));
                }
            }
            resolveMappings (*rt, def);
        }
        if (removed != nullptr)
        {
            for (auto* ch : { &removed->midiEffects, &removed->effects }) for (auto& e : *ch) destroyNode (e.get());
            destroyNode (removed.get());
        }
    }
    notifyContent (inputIndex, program);
}

#define PERF_SLOT_SETTER(name, field, expr)                                                    \
    void Engine::name (int inputIndex, int program, int slotIndex, decltype (SlotDef::field) v) \
    {                                                                                          \
        if (! validInput (inputIndex) || ! validProgram (program)) return;                     \
        auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];              \
        if (slotIndex < 0 || slotIndex >= (int) def.slots.size()) return;                      \
        def.slots[(size_t) slotIndex].field = v;                                               \
        if (auto* rt = getLoaded (inputIndex, program))                                        \
            if (slotIndex < (int) rt->slots.size())                                            \
                rt->slots[(size_t) slotIndex]->expr;                                           \
    }

void Engine::setSlotPanelParams (int inputIndex, int program, int slotIndex, const std::vector<String>& ids)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slotIndex < 0 || slotIndex >= (int) def.slots.size()) return;
    def.slots[(size_t) slotIndex].panelParams = ids;
    notifyContent (inputIndex, program);
}

void Engine::setSlotControlMap (int inputIndex, int program, int slotIndex, const String& name)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slotIndex < 0 || slotIndex >= (int) def.slots.size()) return;
    def.slots[(size_t) slotIndex].controlMap = name;
    notifyContent (inputIndex, program);
}

PERF_SLOT_SETTER (setSlotEnabled,    enabled,    enabled.store (v))
PERF_SLOT_SETTER (setSlotGainDb,     gainDb,     gain.store (Decibels::decibelsToGain (v, -60.0f)))
PERF_SLOT_SETTER (setSlotTranspose,  transpose,  transpose.store (v))
PERF_SLOT_SETTER (setSlotOutChannel, outChannel, outChannel.store (v))
PERF_SLOT_SETTER (setSlotListensTo, listensTo, listensTo.store (v & SlotDef::kAllChannels))

void Engine::setSlotHears (int inputIndex, int program, int slotIndex, int channel, bool hears)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slotIndex < 0 || slotIndex >= (int) def.slots.size()) return;
    auto sd = def.slots[(size_t) slotIndex];
    sd.setHears (channel, hears);
    setSlotListensTo (inputIndex, program, slotIndex, sd.listensTo);
}

bool Engine::addGmSlot (int inputIndex, int program)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return false;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    SlotDef sd;
    sd.kind = SlotDef::Kind::gm;
    def.slots.push_back (sd);
    const int slotIndex = (int) def.slots.size() - 1;
    if (auto* rt = getLoaded (inputIndex, program))
    {
        auto slot = buildSlot (inputIndex, program, slotIndex, sd);
        const ScopedLock sl (lock);
        rt->slots.push_back (std::move (slot));
    }
    notifyContent (inputIndex, program);
    return true;
}

void Engine::setGmChannel (int inputIndex, int program, int slotIndex, int channel, const SlotDef::GmChannel& c)
{
    if (! validInput (inputIndex) || ! validProgram (program) || channel < 1 || channel > 16) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slotIndex < 0 || slotIndex >= (int) def.slots.size() || ! def.slots[(size_t) slotIndex].isGm()) return;
    auto& gc = def.slots[(size_t) slotIndex].gmChannels[(size_t) channel - 1];
    gc.program = jlimit (-1, 127, c.program);
    gc.volume = jlimit (-1, 127, c.volume);
    gc.pan = jlimit (-1, 127, c.pan);
    gc.muted = c.muted;
    if (auto* rt = getLoaded (inputIndex, program))
        if (slotIndex < (int) rt->slots.size())
        {
            auto& s = *rt->slots[(size_t) slotIndex];
            s.gmProgram[channel - 1].store (gc.program);
            s.gmVolume[channel - 1].store (gc.volume);
            s.gmPan[channel - 1].store (gc.pan);
            s.gmMuted[channel - 1].store (gc.muted);
            s.gmDirty.store (true);
        }
}

bool Engine::addInputSlot (int inputIndex, int program, int targetInput, int targetProgram)
{
    if (! validInput (inputIndex) || ! validProgram (program) || ! validInput (targetInput) || targetInput == inputIndex) return false;
    if (targetProgram >= 0 && ! validProgram (targetProgram)) return false;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    SlotDef sd;
    sd.kind = SlotDef::Kind::input;
    sd.targetInput = targetInput;
    sd.targetInputName = setup.inputs[(size_t) targetInput].name;
    sd.targetProgram = targetProgram;
    if (targetProgram >= 0) sd.targetProgramName = setup.inputs[(size_t) targetInput].programs[(size_t) targetProgram].name;
    def.slots.push_back (sd);
    const int slotIndex = (int) def.slots.size() - 1;
    if (auto* rt = getLoaded (inputIndex, program))
    {
        auto slot = buildSlot (inputIndex, program, slotIndex, sd);
        const ScopedLock sl (lock);
        rt->slots.push_back (std::move (slot));
    }
    ensureLentLoaded();
    notifyContent (inputIndex, program);
    return true;
}

std::set<std::pair<int, int>> Engine::lentPrograms() const
{
    std::set<std::pair<int, int>> out;
    for (const auto& in : setup.inputs)
    {
        if (! validProgram (in.currentProgram)) continue;
        for (const auto& sd : in.programs[(size_t) in.currentProgram].slots)
            if (sd.isInput() && sd.targetProgram >= 0 && validInput (sd.targetInput) && validProgram (sd.targetProgram))
                out.insert ({ sd.targetInput, sd.targetProgram });
    }
    return out;
}

void Engine::ensureLentLoaded()
{
    for (const auto& [i, p] : lentPrograms())
        if (getLoaded (i, p) == nullptr)
            ensureProgramLoaded (i, p);
}
PERF_SLOT_SETTER (setSlotVelocityCurve, velocityCurve, velocityCurve.store (jlimit (-1.0f, 1.0f, v)))
PERF_SLOT_SETTER (setSlotPan,        pan,        pan.store (jlimit (-1.0f, 1.0f, v)))
#undef PERF_SLOT_SETTER

void Engine::setSlotVelocityRange (int inputIndex, int program, int slotIndex, int low, int high)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slotIndex < 0 || slotIndex >= (int) def.slots.size()) return;
    low = jlimit (1, 127, low); high = jlimit (low, 127, high);
    def.slots[(size_t) slotIndex].lowVelocity = low;
    def.slots[(size_t) slotIndex].highVelocity = high;
    if (auto* rt = getLoaded (inputIndex, program))
        if (slotIndex < (int) rt->slots.size())
        {
            rt->slots[(size_t) slotIndex]->lowVelocity.store (low);
            rt->slots[(size_t) slotIndex]->highVelocity.store (high);
        }
}

int Engine::curveVelocity (int velocity, float curve)
{
    // A gamma curve through (0,0) and (127,127): curve +1 -> exponent 1/4 (soft playing
    // comes out loud), -1 -> exponent 4 (it takes a hard hit to get loud).
    const float x = jlimit (0.0f, 1.0f, (float) velocity / 127.0f);
    const float gamma = std::pow (4.0f, -jlimit (-1.0f, 1.0f, curve));
    return jlimit (1, 127, (int) std::lround (127.0f * std::pow (x, gamma)));
}

void Engine::panGains (float pan, float& left, float& right)
{
    pan = jlimit (-1.0f, 1.0f, pan);
    left  = jmin (1.0f, 1.0f - pan);
    right = jmin (1.0f, 1.0f + pan);
}

void Engine::setSlotKeyRange (int inputIndex, int program, int slotIndex, int low, int high)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slotIndex < 0 || slotIndex >= (int) def.slots.size()) return;
    low = jlimit (0, 127, low); high = jlimit (low, 127, high);
    def.slots[(size_t) slotIndex].lowKey = low;
    def.slots[(size_t) slotIndex].highKey = high;
    if (auto* rt = getLoaded (inputIndex, program))
        if (slotIndex < (int) rt->slots.size())
        {
            rt->slots[(size_t) slotIndex]->lowKey.store (low);
            rt->slots[(size_t) slotIndex]->highKey.store (high);
        }
}

//==============================================================================
// Effects
//==============================================================================
std::vector<std::unique_ptr<Engine::EffectRuntime>>* Engine::runtimeChainFor (ProgramRuntime& rt, int slot) const
{
    if (slot < 0) return &rt.effects;
    const int s = ProgramDef::slotOfChain (slot);
    if (s >= (int) rt.slots.size()) return nullptr;
    return ProgramDef::isMidiChain (slot) ? &rt.slots[(size_t) s]->midiEffects : &rt.slots[(size_t) s]->effects;
}

std::vector<EffectDef>* Engine::defChainFor (ProgramDef& def, int slot) const
{
    if (slot < 0) return &def.effects;
    const int s = ProgramDef::slotOfChain (slot);
    if (s >= (int) def.slots.size()) return nullptr;
    return ProgramDef::isMidiChain (slot) ? &def.slots[(size_t) s].midiEffects : &def.slots[(size_t) s].effects;
}

void Engine::renumberChain (std::vector<std::unique_ptr<EffectRuntime>>& chain)
{
    for (int i = 0; i < (int) chain.size(); ++i)
        chain[(size_t) i]->effectIndex.store (i);
}

bool Engine::addEffect (int inputIndex, int program, int slot, const PluginDescription& desc, String& error)
{
    auto* defPtr = defFor (inputIndex, program);
    if (defPtr == nullptr) return false;
    auto& def = *defPtr;
    auto* chainDef = defChainFor (def, slot);
    if (chainDef == nullptr) return false;

    EffectDef ed;
    ed.plugin = desc;
    chainDef->push_back (ed);
    const int effectIndex = (int) chainDef->size() - 1;

    bool ok = true;
    if (auto* rt = getLoaded (inputIndex, program))
        if (auto* chain = runtimeChainFor (*rt, slot))
        {
            auto fx = buildEffect (inputIndex, program, slot, effectIndex, ed);
            error.clear();
            const ScopedLock sl (lock);
            chain->push_back (std::move (fx));
        }
    notifyContent (inputIndex, program);
    return ok;
}

void Engine::removeEffect (int inputIndex, int program, int slot, int effect)
{
    auto* defPtr = defFor (inputIndex, program);
    if (defPtr == nullptr) return;
    auto& def = *defPtr;
    auto* chainDef = defChainFor (def, slot);
    if (chainDef == nullptr || effect < 0 || effect >= (int) chainDef->size()) return;

    for (auto it = def.mappings.begin(); it != def.mappings.end();)
    {
        if (it->slot == slot && it->effect == effect)       it = def.mappings.erase (it);
        else { if (it->slot == slot && it->effect > effect) --it->effect; ++it; }
    }
    chainDef->erase (chainDef->begin() + effect);

    if (auto* rt = getLoaded (inputIndex, program))
    {
        std::unique_ptr<EffectRuntime> removed;
        {
            const ScopedLock sl (lock);
            if (auto* chain = runtimeChainFor (*rt, slot))
                if (effect < (int) chain->size())
                {
                    removed = std::move ((*chain)[(size_t) effect]);
                    chain->erase (chain->begin() + effect);
                    renumberChain (*chain);
                }
            resolveMappings (*rt, def);
        }
        destroyNode (removed.get());
    }
    notifyContent (inputIndex, program);
}

void Engine::moveEffect (int inputIndex, int program, int slot, int from, int to)
{
    auto* defPtr = defFor (inputIndex, program);
    if (defPtr == nullptr) return;
    auto& def = *defPtr;
    auto* chainDef = defChainFor (def, slot);
    if (chainDef == nullptr) return;
    const int n = (int) chainDef->size();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;

    auto moveItem = [] (auto& vec, int a, int b)
    {
        auto item = std::move (vec[(size_t) a]);
        vec.erase (vec.begin() + a);
        vec.insert (vec.begin() + b, std::move (item));
    };
    moveItem (*chainDef, from, to);

    for (auto& m : def.mappings)
        if (m.slot == slot && m.effect >= 0)
        {
            if (m.effect == from)                       m.effect = to;
            else if (from < to && m.effect > from && m.effect <= to) --m.effect;
            else if (to < from && m.effect >= to && m.effect < from) ++m.effect;
        }

    if (auto* rt = getLoaded (inputIndex, program))
    {
        const ScopedLock sl (lock);
        if (auto* chain = runtimeChainFor (*rt, slot))
            if (from < (int) chain->size() && to < (int) chain->size())
            {
                moveItem (*chain, from, to);
                renumberChain (*chain);
            }
        resolveMappings (*rt, def);
    }
    notifyContent (inputIndex, program);
}

void Engine::setEffectBypassed (int inputIndex, int program, int slot, int effect, bool b)
{
    auto* defPtr = defFor (inputIndex, program);
    if (defPtr == nullptr) return;
    auto& def = *defPtr;
    auto* chainDef = defChainFor (def, slot);
    if (chainDef == nullptr || effect < 0 || effect >= (int) chainDef->size()) return;
    (*chainDef)[(size_t) effect].bypassed = b;

    if (auto* rt = getLoaded (inputIndex, program))
        if (auto* chain = runtimeChainFor (*rt, slot))
            if (effect < (int) chain->size())
                (*chain)[(size_t) effect]->bypassed.store (b);
}

Engine::PluginNode* Engine::getNode (int inputIndex, int program, int slot, int effect) const
{
    auto* rt = getLoaded (inputIndex, program);
    if (rt == nullptr) return nullptr;
    if (effect < 0)
    {
        if (slot < 0 || slot >= (int) rt->slots.size()) return nullptr;
        return rt->slots[(size_t) slot].get();
    }
    auto* chain = runtimeChainFor (*rt, slot);
    if (chain == nullptr || effect >= (int) chain->size()) return nullptr;
    return (*chain)[(size_t) effect].get();
}

RemotePlugin* Engine::getPlugin (int inputIndex, int program, int slot, int effect) const
{
    auto* node = getNode (inputIndex, program, slot, effect);
    return node != nullptr ? node->plugin.get() : nullptr;
}

String Engine::setPluginSound (int inputIndex, int program, int slot, int effect, const File& nksFile, const String& soundName)
{
    auto* p = getPlugin (inputIndex, program, slot, effect);
    if (p == nullptr || ! p->isAlive()) return "The plugin is not running.";
    MemoryBlock now, state;
    if (! p->getState (now, 10000)) return "The plugin did not answer.";
    if (const auto r = nksStateFor (nksFile, now, state); r.failed()) return r.getErrorMessage();
    if (const auto wrong = setPluginState (inputIndex, program, slot, effect, state, soundName); wrong.isNotEmpty()) return wrong;
    // The file is remembered: its knob pages are this sound's (a Kontakt instrument's own, not another's).
    if (auto* def = defFor (inputIndex, program); def != nullptr && effect < 0 && slot >= 0 && slot < (int) def->slots.size())
        def->slots[(size_t) slot].soundFile = nksFile.getFullPathName();
    return {};
}

String Engine::setPluginState (int inputIndex, int program, int slot, int effect, const MemoryBlock& state, const String& soundName)
{
    auto* p = getPlugin (inputIndex, program, slot, effect);
    if (p == nullptr || ! p->isAlive()) return "The plugin is not running.";
    if (! p->setState (state)) return "The plugin did not take the sound.";
    keepPluginState (inputIndex, program, slot, effect, soundName, state);
    return {};
}

String Engine::setPluginProgram (int inputIndex, int program, int slot, int effect, int index, const String& soundName)
{
    auto* p = getPlugin (inputIndex, program, slot, effect);
    if (p == nullptr || ! p->isAlive()) return "The plugin is not running.";
    if (! p->setProgram (index)) return "The plugin did not change its sound.";
    keepPluginState (inputIndex, program, slot, effect, soundName, {});
    return {};
}

void Engine::keepPluginState (int inputIndex, int program, int slot, int effect, const String& soundName, const MemoryBlock& otherwise)
{
    auto* p = getPlugin (inputIndex, program, slot, effect);
    // Kept as the plugin now has it (its own form of the sound), so a reload is this sound.
    MemoryBlock kept;
    if (p == nullptr || ! p->getState (kept, 10000) || kept.getSize() == 0) kept = otherwise;
    if (kept.isEmpty()) return;
    if (auto* def = defFor (inputIndex, program))
    {
        if (effect < 0)
        {
            if (slot >= 0 && slot < (int) def->slots.size())
            {
                def->slots[(size_t) slot].state = std::move (kept);
                def->slots[(size_t) slot].soundName = soundName;
                def->slots[(size_t) slot].soundFile.clear();        // setPluginSound names it again, for a maker's file
            }
        }
        else if (auto* chain = const_cast<std::vector<EffectDef>*> (def->chainFor (slot)); chain != nullptr && effect < (int) chain->size())
        {
            (*chain)[(size_t) effect].state = std::move (kept);
            (*chain)[(size_t) effect].soundName = soundName;
        }
    }
}

bool Engine::isPluginAlive (int inputIndex, int program, int slot, int effect) const
{
    auto* node = getNode (inputIndex, program, slot, effect);
    if (node != nullptr && effect < 0)
    {
        auto* s = static_cast<SlotRuntime*> (node);
        if (s->external)    return midiOuts.isOpen (s->outPort.load());
        if (s->isGm)        return s->gm != nullptr;
        if (s->isInputSlot) return validInput (s->targetInput.load());
    }
    if (node != nullptr && effect >= 0)
        if (auto* fx = static_cast<EffectRuntime*> (node); fx->send)
            return midiOuts.isOpen (fx->outPort.load());
    return node != nullptr && node->alive();
}

//==============================================================================
// External instruments
//==============================================================================
bool Engine::addExternalSlot (int inputIndex, int program, const MidiDeviceInfo& port)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return false;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    SlotDef sd;
    sd.kind = SlotDef::Kind::external;
    sd.midiOutIdentifier = port.identifier;
    sd.midiOutName = port.name;
    sd.outChannel = 1;                    // a hardware synth listens on one channel; 1 unless told
    def.slots.push_back (sd);
    const int slotIndex = (int) def.slots.size() - 1;
    if (auto* rt = getLoaded (inputIndex, program))
    {
        auto slot = buildSlot (inputIndex, program, slotIndex, sd);
        const ScopedLock sl (lock);
        rt->slots.push_back (std::move (slot));
    }
    notifyContent (inputIndex, program);
    return true;
}

void Engine::setSlotMidiOut (int inputIndex, int program, int slotIndex, const MidiDeviceInfo& port)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slotIndex < 0 || slotIndex >= (int) def.slots.size() || ! def.slots[(size_t) slotIndex].isExternal()) return;
    auto& sd = def.slots[(size_t) slotIndex];
    sd.midiOutIdentifier = port.identifier;
    sd.midiOutName = port.name;
    if (auto* rt = getLoaded (inputIndex, program))
        if (slotIndex < (int) rt->slots.size())
            rt->slots[(size_t) slotIndex]->outPort.store (midiOuts.portFor (port.identifier, port.name));
    notifyContent (inputIndex, program);
}

void Engine::setSlotPatch (int inputIndex, int program, int slotIndex, int msb, int lsb, int patch)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slotIndex < 0 || slotIndex >= (int) def.slots.size()) return;
    auto& sd = def.slots[(size_t) slotIndex];
    sd.bankMsb = jlimit (-1, 127, msb);
    sd.bankLsb = jlimit (-1, 127, lsb);
    sd.patch   = jlimit (-1, 127, patch);
    // Playing now: the instrument changes at once, the way it will when the program is selected.
    if (auto* rt = getLoaded (inputIndex, program); rt != nullptr && runtimes[(size_t) inputIndex]->active == rt)
        sendPatches (*rt, def);
}

void Engine::setSlotReturn (int inputIndex, int program, int slotIndex, int left, int right)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slotIndex < 0 || slotIndex >= (int) def.slots.size()) return;
    def.slots[(size_t) slotIndex].returnLeft = jlimit (-1, 63, left);
    def.slots[(size_t) slotIndex].returnRight = jlimit (-1, 63, right);
    if (left >= 0) ensureAudioInputsOpen();
    if (auto* rt = getLoaded (inputIndex, program))
        if (slotIndex < (int) rt->slots.size())
        {
            rt->slots[(size_t) slotIndex]->returnLeft.store (def.slots[(size_t) slotIndex].returnLeft);
            rt->slots[(size_t) slotIndex]->returnRight.store (def.slots[(size_t) slotIndex].returnRight);
        }
}

bool Engine::addMidiSend (int inputIndex, int program, int slotIndex, const MidiDeviceInfo& port)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return false;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slotIndex < 0 || slotIndex >= (int) def.slots.size()) return false;
    EffectDef ed;
    ed.kind = EffectDef::Kind::midiSend;
    ed.portIdentifier = port.identifier;
    ed.portName = port.name;
    auto& chainDef = def.slots[(size_t) slotIndex].midiEffects;
    chainDef.push_back (ed);
    const int chainAddress = ProgramDef::midiChainOf (slotIndex);
    if (auto* rt = getLoaded (inputIndex, program))
        if (auto* chain = runtimeChainFor (*rt, chainAddress))
        {
            auto fx = buildEffect (inputIndex, program, chainAddress, (int) chainDef.size() - 1, ed);
            const ScopedLock sl (lock);
            chain->push_back (std::move (fx));
        }
    notifyContent (inputIndex, program);
    return true;
}

bool Engine::addBuiltInMidiEffect (int inputIndex, int program, int slotIndex, EffectDef::Kind kind)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return false;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slotIndex < 0 || slotIndex >= (int) def.slots.size()) return false;
    EffectDef ed;
    ed.kind = kind;
    if (! ed.isBuiltIn()) return false;
    auto& chainDef = def.slots[(size_t) slotIndex].midiEffects;
    chainDef.push_back (ed);
    const int chainAddress = ProgramDef::midiChainOf (slotIndex);
    if (auto* rt = getLoaded (inputIndex, program))
        if (auto* chain = runtimeChainFor (*rt, chainAddress))
        {
            auto fx = buildEffect (inputIndex, program, chainAddress, (int) chainDef.size() - 1, ed);
            const ScopedLock sl (lock);
            chain->push_back (std::move (fx));
        }
    notifyContent (inputIndex, program);
    return true;
}

void Engine::setBuiltInMidiEffect (int inputIndex, int program, int slotIndex, int effect, const EffectDef& settings)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slotIndex < 0 || slotIndex >= (int) def.slots.size()) return;
    auto& chain = def.slots[(size_t) slotIndex].midiEffects;
    if (effect < 0 || effect >= (int) chain.size() || ! chain[(size_t) effect].isBuiltIn()) return;
    auto& ed = chain[(size_t) effect];
    ed.arpRate = settings.arpRate; ed.arpMode = settings.arpMode; ed.arpOctaves = settings.arpOctaves; ed.arpGate = settings.arpGate; ed.arpLatch = settings.arpLatch;
    ed.chordType = settings.chordType; ed.scaleRoot = settings.scaleRoot; ed.scaleMode = settings.scaleMode;
    ed.runNotes = settings.runNotes; ed.runBeats = settings.runBeats; ed.runDegree = settings.runDegree; ed.runDown = settings.runDown; ed.runToKey = settings.runToKey; ed.runHoldLast = settings.runHoldLast;
    ed.followSongKey = settings.followSongKey; ed.bandChords = settings.bandChords;
    ed.humanTimingMs = settings.humanTimingMs; ed.humanVelocity = settings.humanVelocity;
    ed.phraseNotes = settings.phraseNotes; ed.phraseName = settings.phraseName; ed.phraseBeats = settings.phraseBeats;
    ed.phraseRoot = settings.phraseRoot; ed.phraseStop = settings.phraseStop; ed.phraseLoop = settings.phraseLoop;
    if (auto* fx = static_cast<EffectRuntime*> (getNode (inputIndex, program, ProgramDef::midiChainOf (slotIndex), effect)))
        if (fx->builtIn != nullptr) fx->builtIn->configure (effectiveEffectDef (ed));
    notifyContent (inputIndex, program);
}

EffectDef* Engine::sendDefFor (int inputIndex, int program, int slotIndex, int effect)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return nullptr;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slotIndex < 0 || slotIndex >= (int) def.slots.size()) return nullptr;
    auto& chain = def.slots[(size_t) slotIndex].midiEffects;
    if (effect < 0 || effect >= (int) chain.size() || ! chain[(size_t) effect].isSend()) return nullptr;
    return &chain[(size_t) effect];
}

Engine::EffectRuntime* Engine::sendRuntimeFor (int inputIndex, int program, int slotIndex, int effect) const
{
    auto* node = getNode (inputIndex, program, ProgramDef::midiChainOf (slotIndex), effect);
    auto* fx = static_cast<EffectRuntime*> (node);
    return fx != nullptr && fx->send ? fx : nullptr;
}

void Engine::setMidiSendPort (int inputIndex, int program, int slotIndex, int effect, const MidiDeviceInfo& port)
{
    auto* ed = sendDefFor (inputIndex, program, slotIndex, effect);
    if (ed == nullptr) return;
    ed->portIdentifier = port.identifier;
    ed->portName = port.name;
    if (auto* fx = sendRuntimeFor (inputIndex, program, slotIndex, effect))
        fx->outPort.store (midiOuts.portFor (port.identifier, port.name));
    notifyContent (inputIndex, program);
}

void Engine::setMidiSendChannel (int inputIndex, int program, int slotIndex, int effect, int channel)
{
    auto* ed = sendDefFor (inputIndex, program, slotIndex, effect);
    if (ed == nullptr) return;
    ed->channel = jlimit (0, 16, channel);
    if (auto* fx = sendRuntimeFor (inputIndex, program, slotIndex, effect))
        fx->sendChannel.store (ed->channel);
    notifyContent (inputIndex, program);
}

void Engine::setMidiSendPatch (int inputIndex, int program, int slotIndex, int effect, int msb, int lsb, int patch)
{
    auto* ed = sendDefFor (inputIndex, program, slotIndex, effect);
    if (ed == nullptr) return;
    ed->bankMsb = jlimit (-1, 127, msb);
    ed->bankLsb = jlimit (-1, 127, lsb);
    ed->patch   = jlimit (-1, 127, patch);
    if (auto* rt = getLoaded (inputIndex, program); rt != nullptr && runtimes[(size_t) inputIndex]->active == rt)
        sendPatches (*rt, setup.inputs[(size_t) inputIndex].programs[(size_t) program]);
    notifyContent (inputIndex, program);
}

void Engine::sendPatches (ProgramRuntime& rt, const ProgramDef& def)
{
    auto recall = [this] (int port, int ch, int msb, int lsb, int patch)
    {
        if (msb >= 0)   midiOuts.sendNow (port, MidiMessage::controllerEvent (ch, 0, msb));
        if (lsb >= 0)   midiOuts.sendNow (port, MidiMessage::controllerEvent (ch, 32, lsb));
        if (patch >= 0) midiOuts.sendNow (port, MidiMessage::programChange (ch, patch));
    };
    for (size_t s = 0; s < rt.slots.size() && s < def.slots.size(); ++s)
    {
        const auto& sd = def.slots[s];
        // A send's patch, on the channel it sends on: its own, else the slot's.
        for (size_t e = 0; e < rt.slots[s]->midiEffects.size() && e < sd.midiEffects.size(); ++e)
            if (const auto& md = sd.midiEffects[e]; md.isSend() && rt.slots[s]->midiEffects[e]->send && ! md.bypassed)
                recall (rt.slots[s]->midiEffects[e]->outPort.load(), md.channel > 0 ? md.channel : (sd.outChannel > 0 ? sd.outChannel : 1),
                        md.bankMsb, md.bankLsb, md.patch);
        if (! sd.isExternal() || ! rt.slots[s]->external) continue;
        const int port = rt.slots[s]->outPort.load();
        const int ch = sd.outChannel > 0 ? sd.outChannel : 1;
        if (sd.bankMsb >= 0) midiOuts.sendNow (port, MidiMessage::controllerEvent (ch, 0, sd.bankMsb));
        if (sd.bankLsb >= 0) midiOuts.sendNow (port, MidiMessage::controllerEvent (ch, 32, sd.bankLsb));
        if (sd.patch >= 0)   midiOuts.sendNow (port, MidiMessage::programChange (ch, sd.patch));
    }
}

bool Engine::isPluginLoading (int inputIndex, int program, int slot, int effect) const
{
    auto* node = getNode (inputIndex, program, slot, effect);
    return node != nullptr && node->loading.load();
}

String Engine::getPluginLoadError (int inputIndex, int program, int slot, int effect) const
{
    auto* node = getNode (inputIndex, program, slot, effect);
    if (node == nullptr || node->loading.load()) return {};
    if (effect < 0)
    {
        auto* s = static_cast<SlotRuntime*> (node);
        if (s->external)
            return midiOuts.isOpen (s->outPort.load()) ? String() : "MIDI port not connected: " + midiOuts.getName (s->outPort.load());
        if (s->isInputSlot)
            return validInput (s->targetInput.load()) ? String() : String ("That input is gone: remove this and add another");
    }
    else if (auto* fx = static_cast<EffectRuntime*> (node); fx->send)
        return midiOuts.isOpen (fx->outPort.load()) ? String() : "MIDI port not connected: " + midiOuts.getName (fx->outPort.load());
    if (node->plugin != nullptr && ! node->plugin->isAlive())
        return "Plugin stopped: " + node->plugin->getLastError();
    return node->loadError;
}

void Engine::reloadPlugin (int inputIndex, int program, int slot, int effect)
{
    auto* node = getNode (inputIndex, program, slot, effect);
    if (node != nullptr && effect < 0 && static_cast<SlotRuntime*> (node)->isGm)
    {
        if (! node->loading.load()) queueGmLoad (*static_cast<SlotRuntime*> (node));
        return;
    }
    if (node != nullptr && ((effect < 0 && (static_cast<SlotRuntime*> (node)->external || static_cast<SlotRuntime*> (node)->isInputSlot))
                            || (effect >= 0 && static_cast<EffectRuntime*> (node)->send)))
    {
        // An external instrument: "reload" is looking for its port again.
        midiOuts.refresh();
        notifyContent (inputIndex, program);
        return;
    }
    auto* defPtr = defFor (inputIndex, program);
    if (node == nullptr || defPtr == nullptr) return;
    auto& def = *defPtr;

    const PluginDescription* desc = nullptr;
    const MemoryBlock* state = nullptr;
    if (effect < 0)
    {
        if (slot < 0 || slot >= (int) def.slots.size()) return;
        desc = &def.slots[(size_t) slot].plugin;
        state = &def.slots[(size_t) slot].state;
    }
    else if (auto* chain = defChainFor (def, slot))
    {
        if (effect >= (int) chain->size()) return;
        desc = &(*chain)[(size_t) effect].plugin;
        state = &(*chain)[(size_t) effect].state;
    }
    if (desc == nullptr) return;

    if (node->loading.load()) return;

    // Detach the old process from the audio thread, then start a fresh one in the background.
    std::unique_ptr<RemotePlugin> old;
    {
        const ScopedLock sl (lock);
        old = std::move (node->plugin);
        if (auto* rt = getLoaded (inputIndex, program))
            resolveMappings (*rt, def);
    }
    if (old != nullptr) { old->setListener (nullptr); old->shutdown(); }

    nodeRegistry[node->id] = node;
    queueLoad (*node, *desc, *state);
    notifyContent (inputIndex, program);
}

//==============================================================================
// Mappings
//==============================================================================
void Engine::resolveMappings (ProgramRuntime& rt, const ProgramDef& def)
{
    std::vector<MappingRuntime> resolved;
    for (auto& m : def.mappings)
    {
        MappingRuntime mr;
        mr.def = m;

        PluginNode* node = nullptr;
        if (m.effect < 0)
        {
            if (m.slot >= 0 && m.slot < (int) rt.slots.size())
                node = rt.slots[(size_t) m.slot].get();
        }
        else if (auto* chain = runtimeChainFor (rt, m.slot))
        {
            if (m.effect < (int) chain->size())
                node = (*chain)[(size_t) m.effect].get();
        }

        if (m.paramId.startsWith ("#"))
        {
            mr.builtIn = m.paramId == "#on" ? MappingRuntime::onOff : m.paramId == "#toggle" ? MappingRuntime::toggle
                       : m.paramId == "#level" ? MappingRuntime::level : MappingRuntime::none;
            if (m.effect < 0 && m.slot >= 0 && m.slot < (int) rt.slots.size()) mr.slotNode = rt.slots[(size_t) m.slot].get();
            else if (auto* chain = runtimeChainFor (rt, m.slot); chain != nullptr && m.effect >= 0 && m.effect < (int) chain->size())
                mr.effectNode = (*chain)[(size_t) m.effect].get();
            if (mr.builtIn == MappingRuntime::level && mr.slotNode == nullptr) mr.builtIn = MappingRuntime::none;   // a level is an instrument's
        }
        else if (node != nullptr && node->plugin != nullptr)
        {
            mr.plugin = node->plugin.get();
            mr.paramIndex = node->plugin->findParameterIndex (m.paramId);
        }
        resolved.push_back (mr);
    }
    rt.mappings = std::move (resolved);
}

void Engine::addMapping (int inputIndex, int program, const MappingDef& m)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    def.mappings.push_back (m);
    if (auto* rt = getLoaded (inputIndex, program)) { const ScopedLock sl (lock); resolveMappings (*rt, def); }
    notifyContent (inputIndex, program);
}

void Engine::updateMapping (int inputIndex, int program, int mappingIndex, const MappingDef& m)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (mappingIndex < 0 || mappingIndex >= (int) def.mappings.size()) return;
    def.mappings[(size_t) mappingIndex] = m;
    if (auto* rt = getLoaded (inputIndex, program)) { const ScopedLock sl (lock); resolveMappings (*rt, def); }
    notifyContent (inputIndex, program);
}

void Engine::removeMapping (int inputIndex, int program, int mappingIndex)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (mappingIndex < 0 || mappingIndex >= (int) def.mappings.size()) return;
    def.mappings.erase (def.mappings.begin() + mappingIndex);
    if (auto* rt = getLoaded (inputIndex, program)) { const ScopedLock sl (lock); resolveMappings (*rt, def); }
    notifyContent (inputIndex, program);
}

void Engine::setLearnArmed (bool b) { learnArmed.store (b); }

void Engine::noteParameterTouched (int inputIndex, int program, int slot, int effect, int paramIndex)
{
    // Can arrive on any thread; dedupe and hand off to the message thread.
    postEvent ({ Event::touched, inputIndex, program, slot, effect, paramIndex });
}

void Engine::noteEditorClosed (int inputIndex, int program)
{
    // Reader thread; the capture is an IPC round trip that belongs on the message thread.
    postEvent ({ Event::editorClosed, inputIndex, program, 0, 0, 0 });
}

void Engine::notePluginDied (int inputIndex, int program, int slot, int effect)
{
    postEvent ({ Event::pluginDied, inputIndex, program, slot, effect, 0 });
}

void Engine::panic() { panicRequested.store (true); }

//==============================================================================
// Events (MIDI/audio thread -> message thread)
//==============================================================================
void Engine::postEvent (const Event& e)
{
    {
        const ScopedLock sl (eventLock);
        if (e.type == Event::touched)
        {
            // Coalesce bursts of the same parameter moving.
            for (auto& p : pendingEvents)
                if (p.type == Event::touched && p.input == e.input && p.a == e.a && p.b == e.b && p.c == e.c && p.d == e.d)
                    return;
        }
        pendingEvents.push_back (e);
    }
    triggerAsyncUpdate();
}

void Engine::handleAsyncUpdate()
{
    /* The two lists trade places and keep their room, so posting from the audio
       and MIDI threads does not allocate (both are given room for 1024 at start). */
    auto& events = handlingEvents;
    events.clear();
    {
        const ScopedLock sl (eventLock);
        events.swap (pendingEvents);
    }

    for (auto& e : events)
    {
        switch (e.type)
        {
            case Event::programChange:
                selectProgram (e.input, e.a);
                break;

            case Event::pedalLearn:
            {
                // a: the pedal, b: the controller pressed. Say so: a pedal gives no feedback of its own.
                const auto pedal = (Pedal) e.a;
                String took;
                for (int p = 0; p < numPedals; ++p)
                    if (p != e.a && pedalCC[(size_t) p].load() == e.b) took = String (" (it was ") + pedalName ((Pedal) p) + ")";
                for (const auto& o : setup.outputs)
                    if (o.volumeCC == e.b) took = " (it was " + o.name + "'s volume)";
                setPedalCC (pedal, e.b);
                const String msg = String (pedalName (pedal)) + ": CC " + String (e.b) + took;
                listeners.call ([&] (Listener& l) { l.statusMessage (msg); });
                if (pedal == Pedal::tapTempo) listeners.call ([cc = e.b] (Listener& l) { l.tapTempoLearned (cc); });
                break;
            }

            case Event::outputLearn:
                setOutputVolumeCC (e.input, e.a);
                listeners.call ([o = e.input, cc = e.a] (Listener& l) { l.outputVolumeLearned (o, cc); });
                break;

            case Event::outputVolume:
                // The audio thread already moved the gain; this keeps the model and the fader with it.
                if (e.input >= 0 && e.input < (int) setup.outputs.size())
                {
                    // b: the exact place from a MIDI 2.0 keyboard, in 24 bits plus one, so the model lands where the gain did.
                    const float f = e.b > 0 ? (float) (e.b - 1) / 16777215.0f : (float) e.a / 127.0f;
                    setup.outputs[(size_t) e.input].gainDb = Decibels::gainToDecibels (f * f, -60.0f);
                    listeners.call ([] (Listener& l) { l.outputsChanged(); });
                }
                break;

            case Event::tapTempo:
                if (const double bpm = tapTempo(); bpm > 0.0)
                    listeners.call ([bpm] (Listener& l)
                        { l.statusMessage ("Tempo " + String (bpm, 1) + " bpm"); });
                break;

            case Event::transport:
                if (! hasTrack())
                    listeners.call ([] (Listener& l) { l.statusMessage ("This song has no backing track."); });
                else
                    toggleTrack();
                break;

            case Event::linkTempo:
                // A peer changed the tempo: the rig follows, and tells the plugins.
                if (const double bpm = linkTempoFromPeer.load(); bpm > 0.0) setTempoBpm (bpm);    // which tells the listeners
                break;

            case Event::linkStartStop:
                if (link != nullptr && link->isEnabled() && link->isStartStopSyncEnabled() && hasTrack())
                {
                    if (e.a != 0 && ! isTrackRunning()) startTrack();
                    else if (e.a == 0 && isTrackRunning()) stopTrack();
                }
                break;

            case Event::sectionPedal:
                if (hasTrack())
                {
                    advanceTrack();
                    const int q = getQueuedTrackSection();
                    const auto msg = isTrackVamping() ? String ("Vamp: released at the end of this pass")
                                   : q >= 0 ? "Next: " + getTrackSectionName (q) : String ("No section after this one");
                    listeners.call ([msg] (Listener& l) { l.statusMessage (msg); });
                }
                else if (getSceneCount() > 0)
                {
                    const bool last = currentScene + 1 >= getSceneCount();
                    advanceTrack();
                    const auto msg = last ? String ("Last scene") : setup.songs[(size_t) currentSongIndex()].markers[(size_t) currentScene].name + "  (" + String (currentScene + 1) + "/" + String (getSceneCount()) + ")";
                    listeners.call ([msg] (Listener& l) { l.statusMessage (msg); });
                }
                break;

            case Event::trackFinished:
                listeners.call ([] (Listener& l) { l.transportChanged(); });
                break;

            case Event::surfaceKnob:
                listeners.call ([&e] (Listener& l) { l.surfaceKnob (e.input, e.a, e.b, e.c != 0, e.d > 0 ? (float) (e.d - 1) / 16777215.0f : -1.0f); });
                break;
            case Event::surfaceButton:
                listeners.call ([&e] (Listener& l) { l.surfaceButton (e.input, e.a); });
                break;
            case Event::surfaceLearn:
                listeners.call ([&e] (Listener& l) { l.surfaceLearned (e.input, e.a, e.b); });
                break;

            case Event::mappedSwitch:
            {
                // a = program, b = slot (or chain address), c = effect (-1 = the instrument), d = on / off or a level in dB
                if (! validInput (e.input) || ! validProgram (e.a)) break;
                auto& def = setup.inputs[(size_t) e.input].programs[(size_t) e.a];
                if (e.c < 0)
                {
                    if (e.b < 0 || e.b >= (int) def.slots.size()) break;
                    auto& sd = def.slots[(size_t) e.b];
                    if (e.d >= 900) sd.gainDb = (float) (e.d - 1000);      // a level, packed as 1000 + dB
                    else            sd.enabled = e.d != 0;
                }
                else if (auto* chain = defChainFor (def, e.b); chain != nullptr && e.c < (int) chain->size())
                    (*chain)[(size_t) e.c].bypassed = e.d == 0;
                notifyContent (e.input, e.a);
                break;
            }

            case Event::sectionEntered:
                if (trackSong >= 0 && trackSong < (int) setup.songs.size())
                {
                    const int mk = getTrackSectionMarker (e.input);
                    const auto& song = setup.songs[(size_t) trackSong];
                    if (mk >= 0 && mk < (int) song.markers.size())
                    {
                        const auto actions = song.markers[(size_t) mk].actions;      // a copy: an action may edit the setup
                        for (const auto& a : actions) performAction (trackSong, a);
                    }
                }
                break;

            case Event::songStep:
            {
                if (setup.currentSet < 0)
                {
                    listeners.call ([] (Listener& l)
                        { l.statusMessage ("No set list chosen: choose one under Set list..."); });
                    break;
                }
                if (e.a > 0) nextSong(); else previousSong();

                // Say where we are: a footswitch gives no feedback of its own.
                const auto& st = setup.sets[(size_t) setup.currentSet];
                const int pos = setup.currentSongInSet;
                if (pos >= 0 && pos < (int) st.songs.size())
                {
                    const auto& sg = setup.songs[(size_t) st.songs[(size_t) pos]];
                    const String msg = String (pos + 1) + "/" + String ((int) st.songs.size())
                                     + "  " + sg.name;
                    listeners.call ([msg] (Listener& l) { l.statusMessage (msg); });
                }
                else if (e.a > 0)
                {
                    listeners.call ([] (Listener& l) { l.statusMessage ("End of the set list."); });
                }
                break;
            }

            case Event::learn:
                // c: 1 + a keyboard knob; d: 1 + a rig pedal -- a controller with a job already, said and not taken.
                if (e.d > 0)
                {
                    const String msg = "CC " + String (e.b) + " is the " + pedalName ((Pedal) (e.d - 1)) + " pedal (Controls, Everywhere): press another. Still waiting...";
                    listeners.call ([&] (Listener& l) { l.statusMessage (msg); });
                }
                else if (e.c > 0) listeners.call ([&] (Listener& l) { l.learnedKeyboardKnob (e.input, e.c - 1); });
                else              listeners.call ([&] (Listener& l) { l.learnReceived (e.input, (MappingDef::Source) e.a, e.b); });
                break;

            case Event::touched:
                lastTouched = { e.input, e.a, e.b, e.c, e.d };
                listeners.call ([&] (Listener& l) { l.parameterTouched (e.input, e.a, e.b, e.c, e.d); });
                break;

            case Event::editorClosed:
                /* The window is where a plugin's own settings get changed, and
                   closing it is as near to "done" as there is. Capture the
                   program's state now and say the program changed, which marks
                   the setup dirty for the next autosave. Byte-comparing the
                   state would not do: SWAM and Kontakt return a different blob
                   on every read, so a real change and no change look alike. */
                if (auto* rt = getLoaded (e.input, e.a))
                {
                    captureProgramState (e.input, *rt, 5000);
                    listeners.call ([&] (Listener& l) { l.programContentChanged (e.input, e.a); });
                }
                break;

            case Event::pluginLoaded:
            {
                std::vector<LoadResult> done;
                {
                    std::lock_guard<std::mutex> l (loaderMutex);
                    done.swap (loadResults);
                }
                for (auto& r : done) attachLoaded (r);
                break;
            }

            case Event::pluginDied:
                if (auto* node = getNode (e.input, e.a, e.b, e.c))
                    if (node->plugin != nullptr && ! node->plugin->isAlive())
                    {
                        /* Started again at once, with its saved state, so the
                           sound comes back without anyone leaving the keys.
                           One that keeps dying is left off: a plugin that
                           crashes on load would otherwise restart for ever. */
                        const auto name = node->plugin->getName();
                        const auto why = node->plugin->getLastError();
                        const auto now = Time::getMillisecondCounter();
                        auto& times = restartTimes[node->id];
                        times.erase (std::remove_if (times.begin(), times.end(), [now] (uint32 t) { return now - t > 60000; }), times.end());
                        if ((int) times.size() < maxAutoRestartsPerMinute)
                        {
                            times.push_back (now);
                            ++autoRestarts;
                            listeners.call ([&] (Listener& l) { l.statusMessage (name + " stopped (" + why + "). Restarting it..."); });
                            reloadPlugin (e.input, e.a, e.b, e.c);
                        }
                        else
                        {
                            listeners.call ([&] (Listener& l) { l.statusMessage (name + " stopped again (" + why + "), " + String (maxAutoRestartsPerMinute)
                                                                                 + " times in a minute, so it is left off. Use Reload on that plugin to try again."); });
                            notifyContent (e.input, e.a);
                        }
                    }
                break;
        }
    }
}

//==============================================================================
// Housekeeping: unload idle programs, or preload everything.
//==============================================================================
void Engine::timerCallback() { housekeeping(); }

void Engine::housekeeping()
{
    if (auto* l = liveness) l->heartbeatMs.store (livenessNowMs(), std::memory_order_relaxed);
    watchForStalls();
    watchAudioDevice();
    watchUsbReconnects();
    /* The Windows plugins' audio threads, raised above their own background
       threads (LowLatency::raiseBridgedAudioThreads): looked for every few
       seconds, as a plugin's appears only once it has loaded and started. */
    if (++bridgedRaiseTick % 6 == 0 && LowLatency::realtimeSchedulingAvailable())
        if (const int n = LowLatency::raiseBridgedAudioThreads (80); n > 0)
            std::fprintf (stderr, "[audio] %d Windows plugin audio thread%s raised to realtime priority 80\n", n, n == 1 ? "" : "s");
    // Restarts that did not load, tried again.
    for (size_t i = 0; i < loadRetries.size();)
    {
        if ((int32) (Time::getMillisecondCounter() - loadRetries[i].second) < 0) { ++i; continue; }
        const auto id = loadRetries[i].first;
        loadRetries.erase (loadRetries.begin() + (long) i);
        if (auto it = nodeRegistry.find (id); it != nodeRegistry.end())
            if (auto* node = it->second; node->plugin == nullptr && ! node->loading.load())
                reloadPlugin (node->inputIndex.load(), node->program, node->slotIndex.load(), node->effectIndex.load());
    }
    {
        // A minute of late-block counts, for the pre-show check.
        const auto now = Time::getMillisecondCounter();
        lateHistory.emplace_back (now, lateBlocks.load());
        while (lateHistory.size() > 1 && now - lateHistory.front().first > 60000)
            lateHistory.erase (lateHistory.begin());
    }

    // A Play held for Link until the files were ready: place it on the next shared bar line now.
    if (player != nullptr && player->isHoldingStart() && player->isReady())
        player->releaseStart (linkStartDelay (linkStartBeats, linkStartBpb));


    if (! trackProblemsReported && player != nullptr && player->isReady())
    {
        trackProblemsReported = true;
        if (const auto p = player->getProblems(); p.isNotEmpty())
            listeners.call ([p] (Listener& l) { l.statusMessage ("Backing track: " + p); });
        listeners.call ([] (Listener& l) { l.transportChanged(); });
    }
    ensureLentLoaded();                 // a program named by a slot but not yet loaded (cleared and refilled, say)
    const auto lent = lentPrograms();
    for (int i = 0; i < (int) runtimes.size(); ++i)
    {
        auto& in = *runtimes[(size_t) i];
        auto& def = setup.inputs[(size_t) i];

        if (setup.preloadAllPrograms)
        {
            // Keep the loader pool fed (about two programs per worker in flight), a few
            // programs per tick so the message thread stays responsive.
            int started = 0;
            for (int p = 0; p < InputDef::numPrograms && started < loaderThreadCount
                            && pendingLoads.load() < loaderThreadCount * 2; ++p)
                if (! def.programs[(size_t) p].isEmpty() && getLoaded (i, p) == nullptr)
                {
                    ensureProgramLoaded (i, p);
                    ++started;
                }
        }
        else
        {
            std::vector<int> idle;
            {
                const ScopedLock sl (lock);
                for (auto& [p, rt] : in.loaded)
                    if (rt.get() != in.active && ! in.isReleasing (rt.get()) && lent.count ({ i, p }) == 0)
                        idle.push_back (p);
            }
            for (int p : idle)
            {
                unloadProgram (in, p);
                notifyContent (i, p);
            }
        }
    }
}

//==============================================================================
// MIDI thread
//==============================================================================
void Engine::handleIncomingMidiMessage (MidiInput* source, const MidiMessage& m)
{
    receiveMidi (source->getIdentifier(), m, -1);
}

void Engine::receiveMidi (const String& id, const MidiMessage& m, int64 exact)
{
    if (m.getChannel() == 0) return;    // sysex / realtime: ignored

    const ScopedLock sl (lock);
    for (int i = 0; i < (int) runtimes.size(); ++i)
        if (runtimes[(size_t) i]->deviceIdentifier == id)
        {
            recorder.writeMidi (i, m);
            routeMidi (*runtimes[(size_t) i], i, m, exact);
        }
}

void Engine::injectMidi (int inputIndex, const MidiMessage& m)
{
    if (m.getChannel() == 0) return;
    MidiMessage stamped (m);
    if (stamped.getTimeStamp() <= 0.0)
        stamped.setTimeStamp (Time::getMillisecondCounterHiRes() * 0.001);

    const ScopedLock sl (lock);
    if (inputIndex >= 0 && inputIndex < (int) runtimes.size()) recorder.writeMidi (inputIndex, stamped);
    if (inputIndex >= 0 && inputIndex < (int) runtimes.size())
        routeMidi (*runtimes[(size_t) inputIndex], inputIndex, stamped);
}

void Engine::injectMidi2 (int inputIndex, const universal_midi_packets::View& v)
{
    universal_midi_packets::SingleGroupMidi1ToBytestreamTranslator translator (256);
    Midi2Inputs::translate (v, Time::getMillisecondCounterHiRes() * 0.001, translator, [this, inputIndex] (const MidiMessage& m, int64 exact)
    {
        if (m.getChannel() == 0) return;
        const ScopedLock sl (lock);
        if (inputIndex < 0 || inputIndex >= (int) runtimes.size()) return;
        recorder.writeMidi (inputIndex, m);
        routeMidi (*runtimes[(size_t) inputIndex], inputIndex, m, exact);
    });
}

/** Does a Program Change on `ch` belong to this input?  Same-as-notes follows the
    note channel (and omni accepts everything, as it already does for notes); Any
    accepts the whole port; otherwise it must match exactly. */
bool Engine::acceptsProgramChangeOn (const InputRuntime& in, int ch)
{
    const int pc = in.programChangeChannel.load();
    if (pc == InputDef::pcChannelAny) return true;
    if (pc == InputDef::pcChannelSameAsNotes)
    {
        const int want = in.channel.load();
        return want == 0 || ch == want;
    }
    return ch == pc;
}

void Engine::routeMidi (InputRuntime& in, int i, const MidiMessage& m, int64 exact)
{
    const int ch = m.getChannel();
    const int want = in.channel.load();
    if (exact >= 0) in.exact.store (m, (uint32_t) exact);
    const float exactValue = exact >= 0 ? (float) ((double) exact / 4294967295.0) : -1.0f;

    // Program Change is filtered on its own channel, which is not always the one the
    // keyboard plays on, so this is tested before the note filter would drop it.
    /* Learning the tap controller comes first, and outside the channel filter
       for the same reason the tap itself is: the pedal you press to teach it is
       the pedal you will press to use it, whatever channel it happens to send
       on. Ahead of the tap check so an assigned controller can be reassigned. */
    if (m.isController() && m.getControllerValue() >= 64)
    {
        const int cc = m.getControllerNumber();
        if (const int learning = pedalLearning.load(); learning >= 0)
        {
            pedalLearning.store (-1);
            postEvent ({ Event::pedalLearn, i, learning, cc, 0, 0 });
            return;
        }

        /* The pedals themselves, outside the channel filter and whatever is
           loaded -- a footswitch has to work all night. Only the press counts.
           While a mapping or a keyboard's control is being learned, a pedal's
           press is reported, not acted on: that controller has a job already. */
        for (int p = 0; p < numPedals && cc > 0; ++p)      // CC 0 is bank select, never a pedal
            if (pedalCC[(size_t) p].load (std::memory_order_relaxed) == cc)
            {
                if (learnArmed.load() || surfaceLearnInput.load() == i)
                    postEvent ({ Event::learn, i, (int) MappingDef::Source::CC, cc, 0, 1 + p });
                else
                    pedalPressed (i, in, (Pedal) p);
                return;
            }
    }

    if (m.isController())
    {
        /* An output's volume control, and learning one: like the pedals, it
           works on any input and channel, because a master fader belongs to
           the rig and not to one sound. Any value: a fader is not a press. */
        if (const int learn = outputLearn.load (std::memory_order_relaxed); learn >= 0)
        {
            outputLearn.store (-1);
            postEvent ({ Event::outputLearn, learn, m.getControllerNumber(), 0, 0, 0 });
            return;
        }
        const int cc = m.getControllerNumber();
        for (int o = 0; o < (int) outputs.size(); ++o)
            if (cc > 0 && outputs[(size_t) o]->volumeCC.load (std::memory_order_relaxed) == cc)
            {
                const float f = exactValue >= 0.0f ? exactValue : (float) m.getControllerValue() / 127.0f;
                outputs[(size_t) o]->gain.store (f * f);          // a fader's taper: half way is -12 dB
                postEvent ({ Event::outputVolume, o, m.getControllerValue(), exact >= 0 ? (int) (exact >> 8) + 1 : 0, 0, 0 });
                return;
            }
    }

    if (m.isProgramChange())
    {
        if (in.respondToProgramChange.load() && acceptsProgramChangeOn (in, ch))
            postEvent ({ Event::programChange, i, m.getProgramChangeNumber(), 0, 0, 0 });
        return;
    }

    if (want != 0 && ch != want) return;

    /* The keyboard's own knobs and buttons, as it was taught them: they work
       the instrument (Surfaces.h), and never reach it as controllers. A
       button's release is swallowed with its press. */
    if (m.isController())
    {
        const int cc = m.getControllerNumber(), value = m.getControllerValue();
        // Teaching: CC 0 is Bank Select, sent with a program change -- never a knob or button, so it is not the one.
        if (cc > 0 && surfaceLearnInput.load (std::memory_order_relaxed) == i)
        {
            surfaceLearnInput.store (-1);
            postEvent ({ Event::surfaceLearn, i, cc, value, 0, 0 });
            return;
        }
        if (cc > 0)
        {
            for (int k = 0; k < SurfaceControls::numKnobs; ++k)
                if (const int taught = in.knobCC[(size_t) k].load (std::memory_order_relaxed);
                    cc == in.heardCC[(size_t) k].load (std::memory_order_relaxed))
                {
                    // Waiting to learn a mapping: this is a keyboard knob, which is never one. Said, not taken.
                    if (learnArmed.load()) { learnArmed = false; postEvent ({ Event::learn, i, (int) MappingDef::Source::CC, cc, 1 + k, 0 }); return; }
                    // A knob told what to send (the spare controllers) sends where it is.
                    const bool relative = taught == cc && in.knobsRelative.load (std::memory_order_relaxed);
                    // d: the exact place, in 24 bits plus one, from a MIDI 2.0 keyboard.
                    postEvent ({ Event::surfaceKnob, i, k, relative ? value - 64 : value, relative ? 1 : 0,
                                 exact >= 0 && ! relative ? (int) (exact >> 8) + 1 : 0 });
                    return;
                }
            for (int a = 0; a < SurfaceControls::numActions; ++a)
                if (in.buttonCC[(size_t) a].load (std::memory_order_relaxed) == cc)
                {
                    if (value >= 64) postEvent ({ Event::surfaceButton, i, a, 0, 0, 0 });
                    return;
                }
        }
    }

    if (learnArmed.load())
    {
        if (m.isController())            { learnArmed = false; postEvent ({ Event::learn, i, (int) MappingDef::Source::CC, m.getControllerNumber(), 0, 0 }); }
        else if (m.isPitchWheel())       { learnArmed = false; postEvent ({ Event::learn, i, (int) MappingDef::Source::PitchBend, 0, 0, 0 }); }
        else if (m.isChannelPressure())  { learnArmed = false; postEvent ({ Event::learn, i, (int) MappingDef::Source::ChannelPressure, 0, 0, 0 }); }
    }

    in.collector.addMessageToQueue (m);
}

// A test asks for blocks when it pleases: not a device running late (lastCallbackMs).
void Engine::renderBlockForTesting (float* const* out, int numOut, int numSamples)
{
    lastCallbackMs = 0.0;
    audioDeviceIOCallbackWithContext (nullptr, 0, out, numOut, numSamples, {});
}

void Engine::renderBlockForTesting (const float* const* in, int numIn, float* const* out, int numOut, int numSamples)
{
    lastCallbackMs = 0.0;
    audioDeviceIOCallbackWithContext (in, numIn, out, numOut, numSamples, {});
}

//==============================================================================
// Audio thread
//==============================================================================
void Engine::audioDeviceAboutToStart (AudioIODevice* device)
{
    outputLatencyMs = device->getCurrentSampleRate() > 0.0
                    ? (device->getOutputLatencyInSamples() + device->getCurrentBufferSizeSamples()) * 1000.0 / device->getCurrentSampleRate()
                    : 0.0;
    sampleRate = device->getCurrentSampleRate();
    blockSize  = device->getCurrentBufferSizeSamples();
    audioStartedAt.store (Time::getMillisecondCounter());
    /* A device starting (again): not an underrun, but whatever was held while
       there was no sound is let go, as its note-offs may be lost. Its thread
       is a new one, to be made realtime whatever its handle looks like. */
    promoteAudioThread.store (true);
    lastCallbackMs = 0.0;
    {
        const ScopedLock sl (lock);
        for (auto& rt : runtimes) rt->deviceLost.store (true, std::memory_order_release);
    }

    /* The same rate and buffer as last time (a device reconnected, a port
       linked or unlinked): the plugins are ready as they are. Preparing them
       again is a round trip to every plugin host in turn, with the sound off,
       and would throw away the note-offs queued meanwhile. */
    if (sampleRate == preparedRate && blockSize == preparedBlock) return;
    preparedRate = sampleRate;
    preparedBlock = blockSize;

    const ScopedLock sl (lock);
    for (auto& in : runtimes)
    {
        in->collector.reset (sampleRate);
        for (auto& [p, rt] : in->loaded)
        {
            rt->mix.setSize (2, blockSize, false, false, true);
            auto prep = [this] (PluginNode& node) { if (node.alive()) node.plugin->prepare (sampleRate, blockSize); };
            for (auto& s : rt->slots)
            {
                s->stereo.setSize (2, blockSize, false, false, true);
                if (s->isGm && ! s->loading.load()) queueGmLoad (*s);     // a synth is made for one rate
                prep (*s);
                for (auto* ch : { &s->midiEffects, &s->effects }) for (auto& e : *ch) prep (*e);
            }
            for (auto& e : rt->effects) prep (*e);
        }
    }
    for (auto& o : outputs)
    {
        o->mix.setSize (2, blockSize, false, false, true);
        o->chain.mix.setSize (2, blockSize, false, false, true);
        for (auto& e : o->chain.effects)
            if (e->alive()) e->plugin->prepare (sampleRate, blockSize);
    }
    if (player != nullptr) player->prepare (sampleRate, blockSize);
}

void Engine::audioDeviceStopped() {}

void Engine::audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                               float* const* out, int numOut, int numSamples,
                                               const AudioIODeviceCallbackContext&)
{
    /* JUCE's ALSA thread is an ordinary high-priority thread, while the plugin
       hosts' audio threads are realtime and wait on it -- so the rig ran at the
       priority of its least urgent part, and a busy desktop could hold the
       whole block up: the occasional burst of static on a direct ALSA device.
       Promoted here, on the thread itself, the first time it calls, and again
       whenever the device comes back on a new thread. PipeWire's thread is
       already realtime and is left alone. */
    /* For testing the stall watch only: STAGERIG_TEST_STALL=<seconds> makes the
       callbacks go quiet for ten seconds from then, as a stuck device would. */
    if (stallTestFrom != 0)
    {
        const auto t = Time::getMillisecondCounter();
        if (t >= stallTestFrom && t < stallTestFrom + 10000)
        {
            for (int c = 0; c < numOut; ++c) if (out[c] != nullptr) FloatVectorOperations::clear (out[c], numSamples);
            return;
        }
    }
    /* Only a block with sound in it counts as the audio moving: a device stuck
       calling with nothing to play (see SteadyBufferDevice) is a stall. */
    if (numSamples <= 0) return;
    audioCallbacks.fetch_add (1, std::memory_order_relaxed);
    /* A block asked for much later than the one before it was due: the device
       ran dry meanwhile (an underrun -- a click, or a gap). Counted here, where
       it is heard, as the device types count only some of theirs. After half a
       second of nothing, the keys released meanwhile may never have been
       heard of (the collector keeps a second of messages and no more): every
       note and pedal held is let go, as when a keyboard goes away. */
    bool longGap = false;
    {
        const double nowMs = Time::getMillisecondCounterHiRes();
        const double dueMs = sampleRate > 0.0 ? 1000.0 * numSamples / sampleRate : 0.0;
        if (lastCallbackMs > 0.0 && dueMs > 0.0)
        {
            const double gap = nowMs - lastCallbackMs;
            if (gap > 3.0 * dueMs + 3.0) deviceXruns.fetch_add (1, std::memory_order_relaxed);
            longGap = gap > 500.0;
        }
        lastCallbackMs = nowMs;
    }
    if (auto* l = liveness) l->audioCalls.fetch_add (1, std::memory_order_relaxed);
    /* A device opened again runs on a new thread, and a new thread is often
       given the very handle the old one had: told apart by the handle alone,
       it was taken for the old, promoted one, and the rig played at desktop
       priority from its first reconnection until it was restarted -- clicks
       whenever anything else was busy. So: asked for at every device start,
       and looked at again every few hundred blocks whatever was concluded
       before (PipeWire, too, raises its own thread a moment after its first
       cycles). */
    if (promoteAudioThread.exchange (false, std::memory_order_acq_rel)
        || ! callbackThreadKnown || ! pthread_equal (callbackThread, pthread_self())
        || ++realtimeRecheck % 512 == 0)
    {
        callbackThread = pthread_self();
        callbackThreadKnown = true;
        audioThreadRealtime.store (LowLatency::currentThreadIsRealtime() || LowLatency::makeCurrentThreadRealtime (85));
    }

    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr)
            FloatVectorOperations::clear (out[c], numSamples);

    // Every plugin process must answer within this block; late ones are silenced for it.
    blockDeadline = ipc::monotonicDeadline (jmax (0.001, 0.85 * numSamples / sampleRate));
    lastBlockSamples.store (numSamples, std::memory_order_relaxed);

    /* External instruments' notes are timed to be heard with the audio this
       callback is making: it leaves the speakers one block and the device's
       output latency from now. */
    const double callbackStartMs = Time::getMillisecondCounterHiRes();

    const ScopedLock sl (lock);
    if (longGap)
        for (auto& rt : runtimes) rt->deviceLost.store (true, std::memory_order_release);
    bool panicNow = panicRequested.exchange (false);

    // With Link on, the tempo the audio side uses is the session's, this block.
    if (link != nullptr && link->isEnabled())
        tempoAtomic.store (link->captureAudioSessionState().tempo(), std::memory_order_relaxed);

    // PipeWire's JACK client may deliver a larger block than jack_get_buffer_size
    // reported at start (another app forcing the graph quantum), so split anything
    // bigger than what the buffers and plugins were prepared for.
    const int maxChunk = jmax (1, blockSize);
    numOut = jmin (numOut, 32);
    numIn = in != nullptr ? jmin (numIn, 32) : 0;
    for (int start = 0; start < numSamples; start += maxChunk)
    {
        const int n = jmin (maxChunk, numSamples - start);
        float* chunk[32];
        const float* inChunk[32];
        for (int c = 0; c < numOut; ++c)
            chunk[c] = out[c] != nullptr ? out[c] + start : nullptr;
        for (int c = 0; c < numIn; ++c)
            inChunk[c] = in[c] != nullptr ? in[c] + start : nullptr;
        chunkIn = inChunk;
        chunkNumIn = numIn;
        chunkStartMs = callbackStartMs + outputLatencyMs + start * 1000.0 / sampleRate;

        /* Each input lands on its output's mix; each output then runs its
           master chain, takes its volume, and goes to its two device channels.
           With the one default output on channels 1-2 this is what the single
           sum used to be, one chain and one multiply later. */
        for (auto& o : outputs)
            o->mix.clear (0, n);
        /* The backing track first: its audio onto the outputs, ahead of their
           master effects, and its MIDI into the inputs this same block. */
        if (player != nullptr)
        {
            float* trackOut[16][2];
            const int numTrackOutputs = jmin ((int) outputs.size(), 16);
            for (int o = 0; o < numTrackOutputs; ++o)
            {
                trackOut[o][0] = outputs[(size_t) o]->mix.getWritePointer (0);
                trackOut[o][1] = outputs[(size_t) o]->mix.getWritePointer (1);
            }
            MidiBuffer* trackMidi = nullptr;
            for (auto& rt : runtimes) if (rt->track.load (std::memory_order_relaxed)) trackMidi = &rt->trackMidi;
            const auto happened = player->render (trackOut, numTrackOutputs, trackMidi, n);
            if (happened == SongPlayer::Happened::stemsStarted)
                resetPluginPositions();
            else if (happened == SongPlayer::Happened::ended || happened == SongPlayer::Happened::stopped)
                postEvent ({ Event::trackFinished, 0, 0, 0, 0, 0 });
            sendClock (n, chunkStartMs, happened == SongPlayer::Happened::stemsStarted,
                       happened == SongPlayer::Happened::ended || happened == SongPlayer::Happened::stopped);

            /* Entering a section -- at Play, at a jump, or by playing into it
               -- fires its actions. A vamp going round stays in its section
               and fires nothing more. */
            const int sec = player->getState() == SongPlayer::State::playing ? player->getCurrentSection() : -1;
            if (sec != lastTrackSection)
            {
                lastTrackSection = sec;
                if (sec >= 0) postEvent ({ Event::sectionEntered, sec, 0, 0, 0, 0 });
            }
        }
        else
        {
            lastTrackSection = -1;
            sendClock (n, chunkStartMs, false, false);
        }

        /* Which programs are lent this block: named by a slot of a program
           that is current (or still sounding). Those play whether or not
           their own input is on them. */
        for (auto& rt : runtimes)
            for (auto& [p, prog] : rt->loaded) prog->lent = false;
        for (auto& rt : runtimes)
        {
            auto mark = [this] (const ProgramRuntime* prog)
            {
                if (prog == nullptr) return;
                for (auto& slot : prog->slots)
                {
                    if (! slot->isInputSlot) continue;
                    const int t = slot->targetInput.load (std::memory_order_relaxed), p = slot->targetProgram.load (std::memory_order_relaxed);
                    if (t < 0 || t >= (int) runtimes.size() || p < 0) continue;
                    if (auto it = runtimes[(size_t) t]->loaded.find (p); it != runtimes[(size_t) t]->loaded.end()) it->second->lent = true;
                }
            };
            mark (rt->active);
            for (auto* prog : rt->releasing) mark (prog);
        }

        for (auto& rt : runtimes)
        {
            if (outputs.empty()) break;
            int idx = rt->output.load (std::memory_order_relaxed);
            if (idx < 0 || idx >= (int) outputs.size()) idx = 0;
            float* busOut[2] = { outputs[(size_t) idx]->mix.getWritePointer (0), outputs[(size_t) idx]->mix.getWritePointer (1) };
            processInput (*rt, inChunk, numIn, busOut, 2, n, panicNow);
        }

        static const MidiBuffer noMidi;
        for (auto& o : outputs)
        {
            AudioBuffer<float> view (o->mix.getArrayOfWritePointers(), 2, n);
            runChain (o->chain.effects, noMidi, view, n);
            const float g = o->gain.load (std::memory_order_relaxed);
            const int l = o->left.load (std::memory_order_relaxed);
            const int r = o->right.load (std::memory_order_relaxed);
            if (l >= 0 && l < numOut && chunk[l] != nullptr) FloatVectorOperations::addWithMultiply (chunk[l], view.getReadPointer (0), g, n);
            if (r >= 0 && r < numOut && chunk[r] != nullptr) FloatVectorOperations::addWithMultiply (chunk[r], view.getReadPointer (1), g, n);
            recorder.writeAudio ((int) (&o - outputs.data()), view.getReadPointer (0), view.getReadPointer (1), g, n);
        }
        panicNow = false;
    }
    guardOutputs (out, numOut, numSamples);
}

void Engine::guardOutputs (float* const* out, int numOut, int numSamples)
{
    const float release = 1.0f - std::exp (-1.0f / (float) (0.1 * jmax (8000.0, sampleRate)));
    bool limited = false, silenced = false;
    for (int c = 0; c < jmin (numOut, 32); ++c)
    {
        float* d = out[c];
        if (d == nullptr) continue;
        float g = guardGain[c];
        for (int i = 0; i < numSamples; ++i)
        {
            float x = d[i];
            if (! std::isfinite (x)) { x = 0.0f; silenced = true; }
            if (g < 1.0f) g = g > 0.9999f ? 1.0f : g + (1.0f - g) * release;      // let go first...
            const float a = std::abs (x);
            if (a * g > safetyCeiling) { g = safetyCeiling / a; limited = true; } // ...then never over
            d[i] = x * g;
        }
        guardGain[c] = g;
    }
    if (limited)  guardLimited.fetch_add (1, std::memory_order_relaxed);
    if (silenced) guardSilenced.fetch_add (1, std::memory_order_relaxed);
}

void Engine::processInput (InputRuntime& in, const float* const* audioIn, int numIn,
                           float* const* out, int numOut, int numSamples, bool panicNow)
{
    in.block.clear();
    in.collector.removeNextBlockOfMessages (in.block, numSamples);
    if (in.deviceLost.exchange (false, std::memory_order_acq_rel))
    {
        // Its keyboard went away mid-note: every note and pedal it held is let go, here and in a tail.
        std::memset (in.noteShift, 0, sizeof (in.noteShift));
        in.shiftedHeld = 0;
        if (in.active != nullptr) in.active->needsNotesOff = true;
        for (auto* p : in.releasing) p->needsNotesOff = true;
    }
    if (! in.trackMidi.isEmpty())
    {
        // The songs' MIDI files, every channel as the file has it: the slots sort them out.
        in.block.addEvents (in.trackMidi, 0, -1, 0);
        in.trackMidi.clear();
    }
    /* The transpose: the keys shifted on the way in -- never the backing
       track, whose parts are its own, and never an audio input. A note held
       across a change is let go at the key it started on. */
    if (! in.track.load (std::memory_order_relaxed) && ! in.audio.load (std::memory_order_relaxed))
    {
        if (panicNow) { std::memset (in.noteShift, 0, sizeof (in.noteShift)); in.shiftedHeld = 0; }
        const int t = liveTranspose.load (std::memory_order_relaxed);
        if (t != 0 || in.shiftedHeld > 0)
        {
            in.shifted.clear();
            for (const auto meta : in.block)
            {
                auto m = meta.getMessage();
                const int c = m.getChannel() - 1;
                if (c >= 0 && c < 16 && m.isNoteOn())
                {
                    const int k = m.getNoteNumber(), nk = k + t;
                    if (nk < 0 || nk > 127) continue;
                    if (in.noteShift[c][k] != 0) --in.shiftedHeld;     // retriggered while held: the old shift is forgotten
                    in.noteShift[c][k] = (int8_t) t;
                    if (t != 0) ++in.shiftedHeld;
                    m.setNoteNumber (nk);
                }
                else if (c >= 0 && c < 16 && m.isNoteOff())
                {
                    const int k = m.getNoteNumber(), s = in.noteShift[c][k];
                    if (s != 0) { --in.shiftedHeld; in.noteShift[c][k] = 0; }
                    m.setNoteNumber (jlimit (0, 127, k + s));
                }
                in.shifted.addEvent (m, meta.samplePosition);
            }
            in.block.swapWith (in.shifted);
        }
    }

    // The looper: records what the keys just played, or plays it round. Not for the track or an audio input.
    if (! in.track.load (std::memory_order_relaxed) && ! in.audio.load (std::memory_order_relaxed))
        loopProcess (in, numSamples, panicNow);

    /* Another input's "its sounds" slot sent these, on this input's own
       channel so they play as its keys would. Kept apart from the keys'
       own MIDI: played, never passed on to a third input. What was sent
       to the program that is current is the input's; what was sent to a
       program lent by name is that program's. */
    const int ch = in.channel.load (std::memory_order_relaxed);
    auto receive = [ch] (MidiBuffer& sent, MidiBuffer& received)
    {
        received.clear();
        if (sent.isEmpty()) return;
        for (const auto meta : sent)
        {
            auto m = meta.getMessage();
            if (ch > 0 && m.getChannel() > 0) m.setChannel (ch);
            received.addEvent (m, meta.samplePosition);
        }
        sent.clear();
    };
    receive (in.sentMidi, in.received);
    for (auto& [p, rt] : in.loaded)
        if (rt.get() != in.active) receive (rt->sentMidi, rt->received);

    /* An audio input's signal goes to the program that is current. One left
       behind by a change gets silence instead and rings out: a delay or
       reverb finishing is the tail, exactly as a released note is. */
    const float* inL = nullptr;
    const float* inR = nullptr;
    if (in.audio.load (std::memory_order_relaxed))
    {
        const int l = in.audioLeft.load (std::memory_order_relaxed);
        const int r = in.audioRight.load (std::memory_order_relaxed);
        if (l >= 0 && l < numIn) inL = audioIn[l];
        if (r >= 0 && r < numIn) inR = audioIn[r];
        if (inL == nullptr) inL = inR;          // a stale left channel: at least pass something

        // The meter: what arrives, before the program's trim.
        if (inL != nullptr)
        {
            auto peakOf = [numSamples] (const float* p)
            {
                const auto mm = FloatVectorOperations::findMinAndMax (p, numSamples);
                const float peak = jmax (std::abs (mm.getStart()), std::abs (mm.getEnd()));
                return std::isfinite (peak) ? peak : 1.0f;      // garbage reads as clipping, never sticks at infinity
            };
            const float pl = peakOf (inL);
            const float pr = inR != nullptr ? peakOf (inR) : pl;
            const float fall = std::exp (-2.3026f * (float) numSamples / (float) jmax (1.0, sampleRate));   // 20 dB per second
            in.levelL.store (jmax (pl, in.levelL.load (std::memory_order_relaxed) * fall), std::memory_order_relaxed);
            in.levelR.store (jmax (pr, in.levelR.load (std::memory_order_relaxed) * fall), std::memory_order_relaxed);
            if (pl >= 0.999f || pr >= 0.999f)
                in.clipMs.store (Time::getMillisecondCounter(), std::memory_order_relaxed);
        }
    }

    const bool fromTrack = in.track.load (std::memory_order_relaxed);
    if (in.active != nullptr)
        processProgram (*in.active, in.block, &in.received, inL, inR, out, numOut, numSamples, panicNow, fromTrack);

    static const MidiBuffer emptyMidi;
    bool holdBlockBuilt = false;
    for (auto it = in.releasing.begin(); it != in.releasing.end();)
    {
        auto* p = *it;
        const MidiBuffer* midi = &emptyMidi;
        if (p->holding)
        {
            // Everything but new notes: the note-offs and pedal it is waiting for, and the wheels.
            if (! holdBlockBuilt)
            {
                in.holdBlock.clear();
                for (const auto meta : in.block)
                    if (const auto m = meta.getMessage(); ! m.isNoteOn())
                        in.holdBlock.addEvent (m, meta.samplePosition);
                holdBlockBuilt = true;
            }
            midi = &in.holdBlock;
        }
        processProgram (*p, *midi, &p->received, nullptr, nullptr, out, numOut, numSamples, panicNow, fromTrack);

        if (p->holding && ! panicNow && p->anythingHeld()) { ++it; continue; }    // still held: the tail waits
        p->holding = false;
        p->releaseSamplesLeft -= numSamples;
        if (p->releaseSamplesLeft <= 0 || panicNow)
            it = in.releasing.erase (it);
        else
            ++it;
    }

    /* The programs lent to other inputs play on, beside the current one,
       hearing only what was sent to them: a MIDI file's part on "Upper:
       Strings" while Upper's keys are on the piano. */
    for (auto& [p, rt] : in.loaded)
        if (rt->lent && rt.get() != in.active && ! in.isReleasing (rt.get()))
            processProgram (*rt, emptyMidi, &rt->received, nullptr, nullptr, out, numOut, numSamples, panicNow, false);
}

static inline float normalisedValueOf (const MidiMessage& m, MappingDef::Source s, const ExactValues* exact)
{
    if (exact != nullptr)
        if (const float e = exact->read (m); e >= 0.0f) return e;
    switch (s)
    {
        case MappingDef::Source::CC:              return m.getControllerValue() / 127.0f;
        case MappingDef::Source::PitchBend:       return m.getPitchWheelValue() / 16383.0f;
        case MappingDef::Source::ChannelPressure: return m.getChannelPressureValue() / 127.0f;
    }
    return 0.0f;
}

static inline bool mappingMatches (const MappingDef& d, const MidiMessage& m)
{
    switch (d.source)
    {
        case MappingDef::Source::CC:              return m.isController() && m.getControllerNumber() == d.number;
        case MappingDef::Source::PitchBend:       return m.isPitchWheel();
        case MappingDef::Source::ChannelPressure: return m.isChannelPressure();
    }
    return false;
}

static void addNotesOff (MidiBuffer& midi, bool allSoundOff)
{
    for (int ch = 1; ch <= 16; ++ch)
    {
        midi.addEvent (MidiMessage::controllerEvent (ch, 64, 0), 0);   // sustain off
        midi.addEvent (MidiMessage::allNotesOff (ch), 0);
        if (allSoundOff)
            midi.addEvent (MidiMessage::allSoundOff (ch), 0);
    }
}

void Engine::runChain (std::vector<std::unique_ptr<EffectRuntime>>& chain, const MidiBuffer& midi, AudioBuffer<float>& stereo, int numSamples)
{
    for (auto& fx : chain)
    {
        if (! fx->alive() || fx->bypassed.load()) continue;
        fx->plugin->beginProcess (stereo.getReadPointer (0), stereo.getReadPointer (1), midi, numSamples);
        /* Late, or stuck: the sound goes past it as it came, for this block.
           Silence here was every instrument behind an output's effect gone
           quiet for as long as one effect took to be found dead. */
        if (! fx->plugin->finishProcess (stereo.getWritePointer (0), stereo.getWritePointer (1), blockDeadline))
            lateBlocks.fetch_add (1);
    }
}

void Engine::runMidiChains (ProgramRuntime& prog, int numSamples, bool notesOffNow, bool panicNow)
{
    /* Block k of every slot's chain runs at once -- each processor is its own
       process, so they work in parallel, as the instruments do -- and the
       slots then hand on what came out to block k + 1. A chain of three costs
       three round trips, however many slots have one. */
    size_t depth = 0;
    for (auto& slot : prog.slots) depth = jmax (depth, slot->midiEffects.size());

    for (size_t k = 0; k < depth; ++k)
    {
        for (auto& slot : prog.slots)
            if (k < slot->midiEffects.size())
                if (auto& fx = *slot->midiEffects[k]; ! fx.send && ! fx.bypassed.load() && fx.alive())
                    fx.plugin->beginProcess (nullptr, nullptr, slot->midi, numSamples);

        for (auto& slot : prog.slots)
        {
            if (k >= slot->midiEffects.size()) continue;
            auto& fx = *slot->midiEffects[k];
            if (fx.bypassed.load()) continue;

            if (fx.builtIn != nullptr)
            {
                fx.produced.clear();
                fx.builtIn->process (slot->midi, fx.produced, numSamples, sampleRate, tempoAtomic.load (std::memory_order_relaxed));
                slot->midi.swapWith (fx.produced);
                if (notesOffNow) { fx.builtIn->reset(); addNotesOff (slot->midi, panicNow); }
                continue;
            }

            if (fx.send)
            {
                // A copy goes out of the port; the MIDI carries on unchanged.
                const int port = fx.outPort.load (std::memory_order_relaxed);
                if (port < 0) continue;
                const int ch = fx.sendChannel.load (std::memory_order_relaxed);
                for (const auto meta : slot->midi)
                {
                    auto mm = meta.getMessage();
                    if (ch > 0 && mm.getChannel() > 0) mm.setChannel (ch);
                    midiOuts.push (port, mm, chunkStartMs + meta.samplePosition * 1000.0 / sampleRate);
                }
                continue;
            }

            // Not running (still loading, or crashed): the notes go past it, as if bypassed.
            if (! fx.alive()) continue;
            if (fx.plugin->finishProcess (nullptr, nullptr, blockDeadline, &fx.produced))
                slot->midi.swapWith (fx.produced);
            else
            {
                /* Late: what it was given goes on past it, less the note-ons
                   (it has not said what to play for them) -- the note-offs
                   and pedals must reach the sound, or its notes hang. */
                fx.produced.clear();
                for (const auto meta : slot->midi)
                    if (! ((meta.data[0] & 0xf0) == 0x90 && meta.numBytes == 3 && meta.data[2] != 0))
                        fx.produced.addEvent (meta.data, meta.numBytes, meta.samplePosition);
                slot->midi.swapWith (fx.produced);
                lateBlocks.fetch_add (1);
            }
            /* A processor need not pass on notes-off it was given (an
               arpeggiator answers with silence, not with CC 123): what comes
               after must hear them all the same. */
            if (notesOffNow) addNotesOff (slot->midi, panicNow);
        }
    }
}

void Engine::processProgram (ProgramRuntime& prog, const MidiBuffer& in, const MidiBuffer* received, const float* inL, const float* inR,
                             float* const* out, int numOut, int numSamples, bool panicNow, bool fromTrack)
{
    for (auto& slot : prog.slots)
        slot->midi.clear();
    prog.programMidi.clear();

    const bool notesOffNow = prog.needsNotesOff || panicNow;
    if (notesOffNow)
    {
        prog.needsNotesOff = false;
        prog.holding = false;
        prog.clearHeld();
        for (auto& slot : prog.slots)
            addNotesOff (slot->midi, panicNow);
        addNotesOff (prog.programMidi, panicNow);
    }

    // Route incoming MIDI: mappings first, then per-slot filtering.
    auto route = [&] (const MidiMessage& m, int pos, bool fromOtherInput)
    {
        prog.track (m);

        bool consumed = false;
        for (auto& map : prog.mappings)
        {
            if (! mappingMatches (map.def, m)) continue;
            if (map.builtIn != MappingRuntime::none)
            {
                /* The instrument or effect itself. Changed here, at once, and
                   told to the message thread so the setup and the panel agree. */
                if (map.slotNode == nullptr && map.effectNode == nullptr) continue;
                const float norm = normalisedValueOf (m, map.def.source, prog.exact);
                const float value = jlimit (0.0f, 1.0f, map.def.minValue + (map.def.maxValue - map.def.minValue) * norm);
                const bool high = value >= 0.5f;
                bool apply = false, on = false;
                int levelDb = 0;
                if (map.builtIn == MappingRuntime::onOff) { apply = true; on = high; }
                else if (map.builtIn == MappingRuntime::toggle)
                {
                    if (high && ! map.wasHigh)
                    {
                        apply = true;
                        on = map.slotNode != nullptr ? ! map.slotNode->enabled.load() : map.effectNode->bypassed.load();
                    }
                    map.wasHigh = high;
                }
                else if (map.builtIn == MappingRuntime::level)
                {
                    apply = true;
                    levelDb = roundToInt (-60.0f + 72.0f * value);          // the fader's travel: -60 dB to +12
                }
                if (apply)
                {
                    if (map.builtIn == MappingRuntime::level) map.slotNode->gain.store (Decibels::decibelsToGain ((float) levelDb, -60.0f));
                    else if (map.slotNode != nullptr) map.slotNode->enabled.store (on);
                    else map.effectNode->bypassed.store (! on);
                    const PluginNode* node = map.slotNode != nullptr ? static_cast<const PluginNode*> (map.slotNode) : map.effectNode;
                    postEvent ({ Event::mappedSwitch, node->inputIndex.load (std::memory_order_relaxed), prog.program, map.def.slot, map.def.effect,
                                 map.builtIn == MappingRuntime::level ? 1000 + levelDb : (on ? 1 : 0) });
                }
                if (! map.def.passThrough) consumed = true;
                continue;
            }
            if (map.plugin == nullptr || map.paramIndex < 0 || ! map.plugin->isAlive()) continue;
            const float norm = normalisedValueOf (m, map.def.source, prog.exact);
            const float value = map.def.minValue + (map.def.maxValue - map.def.minValue) * norm;
            map.plugin->queueParameterChange (map.paramIndex, jlimit (0.0f, 1.0f, value));
            if (! map.def.passThrough) consumed = true;
        }
        if (consumed) return;

        prog.programMidi.addEvent (m, pos);

        for (auto& slot : prog.slots)
        {
            if (! slot->playable() || ! slot->enabled.load()) continue;
            // Only the channels it listens to; channel-less messages reach every slot.
            if (! slot->hears (m.getChannel())) continue;
            // What another keyboard's program handed us is played here, not handed on again.
            if (fromOtherInput && slot->isInputSlot) continue;
            /* A MIDI file's program changes and bank selects choose the GM
               player's instruments; your own sounds keep their patches. */
            if (fromTrack && ! slot->isGm && (m.isProgramChange() || (m.isController() && (m.getControllerNumber() == 0 || m.getControllerNumber() == 32))))
                continue;

            MidiMessage mm (m);
            if (mm.isNoteOnOrOff())
            {
                int note = mm.getNoteNumber();
                if (note < slot->lowKey.load() || note > slot->highKey.load()) continue;
                note += slot->transpose.load();
                if (note < 0 || note > 127) continue;
                mm.setNoteNumber (note);
                if (mm.isNoteOn())
                {
                    // Velocity layer: note-ons outside the range never reach this slot.
                    // Note-offs always pass, so nothing can hang.
                    const int v = mm.getVelocity();
                    if (v < slot->lowVelocity.load() || v > slot->highVelocity.load()) continue;
                    const float curve = slot->velocityCurve.load();
                    if (curve != 0.0f) mm.setVelocity ((float) curveVelocity (v, curve) / 127.0f);
                }
            }
            const int oc = slot->outChannel.load();
            if (oc > 0) mm.setChannel (oc);
            slot->midi.addEvent (mm, pos);
        }
    };
    for (const auto meta : in) route (meta.getMessage(), meta.samplePosition, false);
    if (received != nullptr)
        for (const auto meta : *received) route (meta.getMessage(), meta.samplePosition, true);

    // Each slot's MIDI chain, then its sound hears what comes out of it.
    runMidiChains (prog, numSamples, notesOffNow, panicNow);

    // Render each slot through its chain and sum into the program mix. Instruments run
    // in parallel (one process each); each slot's effect chain then runs in order.
    if (prog.mix.getNumSamples() < numSamples) prog.mix.setSize (2, numSamples, false, false, true);
    AudioBuffer<float> mix (prog.mix.getArrayOfWritePointers(), 2, numSamples);
    mix.clear();

    // An audio input: the signal, trimmed, is what the program's chain works on.
    if (inL != nullptr)
    {
        const float g = prog.inputGain.load (std::memory_order_relaxed);
        mix.addFrom (0, 0, inL, numSamples, g);
        mix.addFrom (1, 0, inR != nullptr ? inR : inL, numSamples, g);
    }

    for (auto& slot : prog.slots)
    {
        if (slot->stereo.getNumSamples() < numSamples) slot->stereo.setSize (2, numSamples, false, false, true);
        if (slot->alive())
            slot->plugin->beginProcess (nullptr, nullptr, slot->midi, numSamples);
    }

    for (auto& slot : prog.slots)
    {
        AudioBuffer<float> stereo (slot->stereo.getArrayOfWritePointers(), 2, numSamples);

        if (slot->isGm)
        {
            if (slot->gm == nullptr) continue;
            prepareGmMidi (*slot, numSamples);
            stereo.clear();
            slot->gm->renderAdding (slot->gmMidi, stereo.getWritePointer (0), stereo.getWritePointer (1), numSamples, 1.0f, 1.0f);
        }
        else if (slot->isInputSlot)
        {
            /* Its notes go to another input, heard in that input's next
               block (or this one, if it comes later): by its current program,
               or by the program named here when that is a different one. */
            if (const int t = slot->targetInput.load (std::memory_order_relaxed); t >= 0 && t < (int) runtimes.size() && ! slot->midi.isEmpty())
            {
                auto& target = *runtimes[(size_t) t];
                const int p = slot->targetProgram.load (std::memory_order_relaxed);
                if (p < 0 || (target.active != nullptr && target.active->program == p))
                    target.sentMidi.addEvents (slot->midi, 0, -1, 0);
                else if (auto it = target.loaded.find (p); it != target.loaded.end())
                    it->second->sentMidi.addEvents (slot->midi, 0, -1, 0);
            }
            continue;
        }
        else if (slot->external)
        {
            /* Its notes leave for the port, each at the moment its sample
               will be heard; its sound, if it comes back on interface
               inputs, joins the mix from here like a plugin's would. */
            if (const int port = slot->outPort.load (std::memory_order_relaxed); port >= 0)
                for (const auto meta : slot->midi)
                    midiOuts.push (port, meta.getMessage(), chunkStartMs + meta.samplePosition * 1000.0 / sampleRate);

            stereo.clear();
            const int l = slot->returnLeft.load (std::memory_order_relaxed);
            const int r = slot->returnRight.load (std::memory_order_relaxed);
            if (l < 0 || l >= chunkNumIn || chunkIn == nullptr || chunkIn[l] == nullptr) continue;
            stereo.copyFrom (0, 0, chunkIn[l], numSamples);
            stereo.copyFrom (1, 0, (r >= 0 && r < chunkNumIn && chunkIn[r] != nullptr) ? chunkIn[r] : chunkIn[l], numSamples);
        }
        else
        {
            if (! slot->alive()) continue;
            if (! slot->plugin->finishProcess (stereo.getWritePointer (0), stereo.getWritePointer (1), blockDeadline))
            {
                stereo.clear();
                lateBlocks.fetch_add (1);
                continue;
            }
        }
        runChain (slot->effects, slot->midi, stereo, numSamples);

        const float g = slot->enabled.load() ? slot->gain.load() : 0.0f;
        if (g <= 0.0f) continue;
        float pl = 1.0f, pr = 1.0f;
        panGains (slot->pan.load(), pl, pr);
        mix.addFrom (0, 0, stereo, 0, 0, numSamples, g * pl);
        mix.addFrom (1, 0, stereo, 1, 0, numSamples, g * pr);
    }

    runChain (prog.effects, prog.programMidi, mix, numSamples);

    for (int c = 0; c < jmin (numOut, 2); ++c)
        if (out[c] != nullptr)
            FloatVectorOperations::add (out[c], mix.getReadPointer (c), numSamples);
}

void Engine::prepareGmMidi (SlotRuntime& slot, int numSamples)
{
    ignoreUnused (numSamples);
    auto& out = slot.gmMidi;
    out.clear();
    /* The mixer's choices first, when they have changed: the instrument
       (bank 0 and a program change, the kit on channel 10), volume, pan,
       and every note off on a channel that was just muted. */
    if (slot.gmDirty.exchange (false))
        for (int c = 0; c < 16; ++c)
        {
            if (const int p = slot.gmProgram[c].load (std::memory_order_relaxed); p >= 0)
            {
                if (c != 9) { out.addEvent (MidiMessage::controllerEvent (c + 1, 0, 0), 0); out.addEvent (MidiMessage::controllerEvent (c + 1, 32, 0), 0); }
                out.addEvent (MidiMessage::programChange (c + 1, p), 0);
            }
            if (const int v = slot.gmVolume[c].load (std::memory_order_relaxed); v >= 0) out.addEvent (MidiMessage::controllerEvent (c + 1, 7, v), 0);
            if (const int p = slot.gmPan[c].load (std::memory_order_relaxed); p >= 0)    out.addEvent (MidiMessage::controllerEvent (c + 1, 10, p), 0);
            if (slot.gmMuted[c].load (std::memory_order_relaxed)) out.addEvent (MidiMessage::allNotesOff (c + 1), 0);
        }
    for (const auto meta : slot.midi)
    {
        const auto m = meta.getMessage();
        const int c = m.getChannel() - 1;
        if (c >= 0 && c < 16)
        {
            if (slot.gmMuted[c].load (std::memory_order_relaxed) && m.isNoteOn()) continue;
            // What the mixer sets, the file does not get to change.
            if (slot.gmProgram[c].load (std::memory_order_relaxed) >= 0
                && (m.isProgramChange() || (m.isController() && (m.getControllerNumber() == 0 || m.getControllerNumber() == 32)))) continue;
            if (m.isController() && m.getControllerNumber() == 7  && slot.gmVolume[c].load (std::memory_order_relaxed) >= 0) continue;
            if (m.isController() && m.getControllerNumber() == 10 && slot.gmPan[c].load (std::memory_order_relaxed) >= 0) continue;
        }
        out.addEvent (m, meta.samplePosition);
    }
}


//==============================================================================
// Staying up
//==============================================================================
void Engine::watchForStalls()
{
    auto check = [this] (PluginNode& node)
    {
        if (node.plugin == nullptr || node.loading.load()) return;
        node.plugin->killIfStalled (node.plugin->isEditorOpen() ? stalledPluginEditorOpenMs : stalledPluginMs);
        // A kill is a death: the plugin's listener posts it, and the restart follows from there.
    };
    auto walk = [&] (ProgramRuntime& rt)
    {
        for (auto& sl : rt.slots)
        {
            check (*sl);
            for (auto* ch : { &sl->midiEffects, &sl->effects }) for (auto& e : *ch) check (*e);
        }
        for (auto& e : rt.effects) check (*e);
    };
    for (auto& in : runtimes)
        for (auto& [p, rt] : in->loaded) walk (*rt);
    for (auto& o : outputs) walk (o->chain);
}

int Engine::getRecentLateBlocks() const
{
    if (lateHistory.empty()) return 0;
    return lateBlocks.load() - lateHistory.front().second;
}

String Engine::LivePosition::toString() const
{
    StringArray progs;
    for (int p : programs) progs.add (String (p));
    return String (set) + ";" + String (songInSet) + ";" + String (song) + ";" + String (scene) + ";"
         + progs.joinIntoString (",") + ";" + String (tempoBpm, 3);
}

Engine::LivePosition Engine::LivePosition::fromString (const String& text)
{
    LivePosition p;
    const auto parts = StringArray::fromTokens (text, ";", "");
    if (parts.size() < 6) return p;
    p.set = parts[0].getIntValue();
    p.songInSet = parts[1].getIntValue();
    p.song = parts[2].getIntValue();
    p.scene = parts[3].getIntValue();
    for (auto& t : StringArray::fromTokens (parts[4], ",", ""))
        if (t.isNotEmpty()) p.programs.push_back (t.getIntValue());
    p.tempoBpm = parts[5].getDoubleValue();
    return p;
}

Engine::LivePosition Engine::getLivePosition() const
{
    LivePosition p;
    p.set = setup.currentSet;
    p.songInSet = setup.currentSongInSet;
    p.song = songInPlay;
    p.scene = currentScene;
    for (auto& in : setup.inputs) p.programs.push_back (in.currentProgram);
    p.tempoBpm = setup.tempoBpm;
    return p;
}

void Engine::restoreLivePosition (const LivePosition& p)
{
    /* The song first, as Next would choose it -- its programs, its tempo,
       its track armed -- then the scene, then whatever each keyboard had
       been moved to since, then the tempo as it was last tapped. */
    if (p.set >= 0 && p.set < (int) setup.sets.size())
    {
        setup.currentSet = p.set;
        const auto& st = setup.sets[(size_t) p.set];
        setup.currentSongInSet = p.songInSet >= 0 && p.songInSet < (int) st.songs.size() ? p.songInSet : -1;
    }
    if (p.song >= 0 && p.song < (int) setup.songs.size())
        selectSong (p.song, jmax (0, p.scene));
    if (p.scene >= 0 && p.scene != currentScene && p.scene < getSceneCount())
        selectScene (p.scene);
    for (int i = 0; i < (int) p.programs.size() && i < (int) setup.inputs.size(); ++i)
        if (validProgram (p.programs[(size_t) i]) && setup.inputs[(size_t) i].currentProgram != p.programs[(size_t) i])
            selectProgram (i, p.programs[(size_t) i]);
    if (p.tempoBpm > 0.0 && std::abs (p.tempoBpm - setup.tempoBpm) > 0.001)
        setTempoBpm (p.tempoBpm);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

std::vector<Engine::CheckItem> Engine::showCheck() const
{
    std::vector<CheckItem> items;
    auto add = [&items] (CheckItem::Level l, const String& t) { items.push_back ({ l, t }); };

    // The audio interface, first: nothing else matters without it.
    auto* dev = const_cast<DeviceManager&> (deviceManager).getCurrentAudioDevice();
    if (dev == nullptr)
        add (CheckItem::problem, "No audio interface is open. Open Audio... and choose one.");
    else
    {
        const int block = lastBlockSamples.load() > 0 ? lastBlockSamples.load() : dev->getCurrentBufferSizeSamples();
        // By the name it is chosen by in Audio..., not the driver's ("...; Direct hardware device without any conversions").
        const auto chosen = const_cast<DeviceManager&> (deviceManager).getAudioDeviceSetup().outputDeviceName;
        add (CheckItem::good, "Audio: " + (chosen.isNotEmpty() ? chosen : dev->getName()) + ", " + String (roundToInt (dev->getCurrentSampleRate())) + " Hz, "
                              + String (block) + " samples (" + String (1000.0 * block / jmax (1.0, dev->getCurrentSampleRate()), 1) + " ms)");
        if (! audioThreadRealtime.load())
            add (CheckItem::problem, "The audio is not running at realtime priority, so a busy computer can break it up. "
                                     "Add yourself to the audio group (or install the realtime limits) and log in again.");
    }
    for (auto& w : deviceWarnings())
        if (audioThreadRealtime.load() || ! w.containsIgnoreCase ("realtime"))    // said once, above
            add (CheckItem::warning, w);

    if (const int late = getRecentLateBlocks(); late > 0)
        add (CheckItem::warning, String (late) + (late == 1 ? " audio block was" : " audio blocks were") + " late in the last minute: "
                                 "a plugin is not keeping up. Raise the buffer size in Audio..., or look for the plugin that is loading.");
    if (const double cpu = getCpuUsage(); cpu > 0.7)
        add (CheckItem::warning, "The audio is using " + String (roundToInt (cpu * 100.0)) + "% of its time. A heavy patch could tip it over.");

    // The keyboards and the MIDI devices the rig sends to.
    const auto available = MidiInput::getAvailableDevices();
    auto present = [&available] (const String& id, const String& name)
    {
        for (auto& d : available) if (name.isNotEmpty() ? d.name == name : d.identifier == id) return true;    // as openMidiDevices finds it
        return false;
    };
    int keyboardsOk = 0;
    for (auto& in : setup.inputs)
    {
        if (in.isTrack() || in.midiDeviceIdentifier.isEmpty()) continue;
        if (present (in.midiDeviceIdentifier, in.midiDeviceName)) ++keyboardsOk;
        else add (CheckItem::problem, in.name + ": its MIDI device, " + (in.midiDeviceName.isNotEmpty() ? in.midiDeviceName : in.midiDeviceIdentifier)
                                      + ", is not connected. Plug it in or switch it on: StageRig picks it up by itself.");
    }
    if (keyboardsOk > 0)
        add (CheckItem::good, String (keyboardsOk) + (keyboardsOk == 1 ? " MIDI device" : " MIDI devices") + " connected");
    const auto outs = MidiOutput::getAvailableDevices();
    for (auto& c : setup.clockOut)
    {
        bool found = false;
        for (auto& d : outs) if (d.identifier == c.identifier || d.name == c.name) found = true;
        if (! found) add (CheckItem::warning, "MIDI clock goes to " + c.name + ", which is not connected.");
    }

    // Every plugin in the show: the ones loaded, and the files of the ones that are not.
    int pluginsOk = 0, notLoaded = 0, loadingNow = 0;
    StringArray missingFiles;
    auto fileMissing = [] (const PluginDescription& d)
    {
        if (d.fileOrIdentifier.isEmpty() || ! File::isAbsolutePath (d.fileOrIdentifier)) return false;   // LV2 URIs and the like
        return ! File (d.fileOrIdentifier).exists();
    };
    auto inspect = [&] (int input, int program, ProgramRuntime* rt, const ProgramDef& cdef, const String& where)
    {
        auto& def = const_cast<ProgramDef&> (cdef);
        auto look = [&] (const PluginDescription& d, PluginNode* node, int slot, int effect)
        {
            if (d.name.isEmpty() && d.fileOrIdentifier.isEmpty()) return;     // a built-in, an external instrument
            if (fileMissing (d)) { missingFiles.addIfNotAlreadyThere (d.name + " (" + where + ")"); return; }
            if (rt == nullptr || node == nullptr) { ++notLoaded; return; }
            if (node->loading.load()) { ++loadingNow; return; }
            const auto err = getPluginLoadError (input, program, slot, effect);
            if (err.isNotEmpty()) add (CheckItem::problem, d.name + " in " + where + ": " + err + ". Press Reload on it.");
            else if (node->plugin != nullptr) ++pluginsOk;
        };
        for (int si = 0; si < (int) def.slots.size(); ++si)
        {
            const auto& sd = def.slots[(size_t) si];
            PluginNode* sn = rt != nullptr && si < (int) rt->slots.size() ? rt->slots[(size_t) si].get() : nullptr;
            look (sd.plugin, sn, si, -1);
            for (int chainSlot : { si, ProgramDef::midiChainOf (si) })
                if (auto* chain = defChainFor (def, chainSlot))
                {
                    auto* rchain = rt != nullptr ? runtimeChainFor (*rt, chainSlot) : nullptr;
                    for (int ei = 0; ei < (int) chain->size(); ++ei)
                        look ((*chain)[(size_t) ei].plugin, rchain != nullptr && ei < (int) rchain->size() ? (*rchain)[(size_t) ei].get() : nullptr, chainSlot, ei);
                }
        }
        if (auto* chain = defChainFor (def, -1))
        {
            auto* rchain = rt != nullptr ? runtimeChainFor (*rt, -1) : nullptr;
            for (int ei = 0; ei < (int) chain->size(); ++ei)
                look ((*chain)[(size_t) ei].plugin, rchain != nullptr && ei < (int) rchain->size() ? (*rchain)[(size_t) ei].get() : nullptr, -1, ei);
        }
    };
    for (int i = 0; i < (int) setup.inputs.size(); ++i)
        for (int p = 0; p < InputDef::numPrograms; ++p)
        {
            const auto& def = setup.inputs[(size_t) i].programs[(size_t) p];
            if (def.isEmpty()) continue;
            inspect (i, p, getLoaded (i, p), def, setup.inputs[(size_t) i].name + " " + String (p + 1).paddedLeft ('0', 3)
                                                   + (def.name.isNotEmpty() ? " " + def.name : String()));
        }
    for (int o = 0; o < (int) setup.outputs.size(); ++o)
        inspect (outputAddress (o), 0, getLoaded (outputAddress (o), 0), setup.outputs[(size_t) o].chain, setup.outputs[(size_t) o].name + "'s master effects");

    for (auto& m : missingFiles) add (CheckItem::problem, "Plugin not installed: " + m + ".");
    if (pluginsOk > 0) add (CheckItem::good, String (pluginsOk) + (pluginsOk == 1 ? " plugin" : " plugins") + " running");
    if (loadingNow > 0) add (CheckItem::warning, String (loadingNow) + (loadingNow == 1 ? " plugin is" : " plugins are") + " still loading. Check again in a moment.");
    if (notLoaded > 0 && ! setup.preloadAllPrograms)
        add (CheckItem::warning, String (notLoaded) + (notLoaded == 1 ? " plugin loads" : " plugins load") + " only when its program is first chosen, "
                                 "which can take seconds on stage. Turn on Options > Preload all programs.");

    // The songs about to be played: the set if one is chosen, else all of them.
    std::vector<int> songs;
    if (setup.currentSet >= 0 && setup.currentSet < (int) setup.sets.size()) songs = setup.sets[(size_t) setup.currentSet].songs;
    else for (int i = 0; i < (int) setup.songs.size(); ++i) songs.push_back (i);
    const auto folder = getChartsFolder();
    int filesOk = 0;
    for (int si : songs)
    {
        if (si < 0 || si >= (int) setup.songs.size()) continue;
        const auto& song = setup.songs[(size_t) si];
        for (auto& st : song.stems)
            if (folder.getChildFile (st.file).existsAsFile()) ++filesOk;
            else add (CheckItem::problem, song.name + ": its backing-track file " + st.file + " is missing.");
        for (auto& c : song.charts)
            if (folder.getChildFile (c.file).existsAsFile()) ++filesOk;
            else add (CheckItem::warning, song.name + ": its chart " + c.displayName() + " is missing.");
    }
    if (filesOk > 0) add (CheckItem::good, String (filesOk) + (filesOk == 1 ? " backing-track or chart file found" : " backing-track and chart files found"));

    // Tonight so far.
    if (autoRestarts > 0)
        add (CheckItem::warning, String (autoRestarts) + (autoRestarts == 1 ? " plugin has" : " plugins have") + " crashed and been restarted since StageRig started. "
                                 "The status bar named it; consider a different plugin for that sound.");
    if (const int g = guardSilenced.load(); g > 0)
        add (CheckItem::warning, "A plugin sent out broken audio (not a number) " + String (g) + " times; the safety guard silenced it.");
    if (const int g = guardLimited.load(); g > 0)
        add (CheckItem::warning, "The safety guard turned the output down in " + String (g) + " audio blocks: something is reaching full scale. "
                                 "Lower the master volume, or the loudest program.");

   #if JUCE_LINUX
    // On battery: a laptop that dies at the encore, or slows itself down to save power.
    for (auto& ps : File ("/sys/class/power_supply").findChildFiles (File::findDirectories, false))
        if (ps.getChildFile ("type").loadFileAsString().trim() == "Mains" && ps.getChildFile ("online").loadFileAsString().trim() == "0")
            add (CheckItem::warning, "The computer is running on battery. Plug in the power supply.");
   #endif

    // Room for the recording, if there is one.
    const auto recFolder = getRecordingsFolder();
    const auto free = recFolder.getParentDirectory().getBytesFreeOnVolume();
    if (free > 0 && free < (int64) 2 * 1024 * 1024 * 1024)
        add (CheckItem::warning, "Only " + File::descriptionOfSizeInBytes (free) + " free on the disk: a recording of the gig would run out.");

    std::stable_sort (items.begin(), items.end(), [] (const CheckItem& a, const CheckItem& b) { return a.level > b.level; });
    return items;
}

} // namespace perf
