#include "Engine.h"
#include "LowLatency.h"
#include <pthread.h>
#include <cstring>
#include <cmath>
#include <algorithm>

using namespace juce;

namespace perf
{

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
};

struct Engine::SlotRuntime : public PluginNode
{
    SlotRuntime (Engine& e, int in, int prog, int slot) : PluginNode (e, in, prog, slot, -1) {}

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
    }

    std::atomic<bool> enabled { true };
    std::atomic<float> gain { 1.0f }, velocityCurve { 0.0f }, pan { 0.0f };
    std::atomic<int> transpose { 0 }, lowKey { 0 }, highKey { 127 }, lowVelocity { 1 }, highVelocity { 127 }, outChannel { 0 };
    std::vector<std::unique_ptr<EffectRuntime>> effects;
    AudioBuffer<float> stereo;     // the slot's signal as it travels down the chain
};

struct Engine::MappingRuntime
{
    MappingDef def;
    RemotePlugin* plugin = nullptr;
    int paramIndex = -1;
};

struct Engine::ProgramRuntime
{
    int program = 0;
    std::vector<std::unique_ptr<SlotRuntime>> slots;
    std::vector<std::unique_ptr<EffectRuntime>> effects;   // program-level chain
    std::vector<MappingRuntime> mappings;
    AudioBuffer<float> mix;        // summed slots, before the program chain
    MidiBuffer programMidi;        // MIDI seen by the program chain
    int releaseSamplesLeft = 0;
    bool needsNotesOff = false;
    std::atomic<float> inputGain { 1.0f };    // audio inputs: the trim before the chain

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
    std::atomic<int> channel { 0 };
    std::atomic<bool> respondToProgramChange { true };
    std::atomic<int> programChangeChannel { InputDef::pcChannelSameAsNotes };
    std::atomic<bool> audio { false };                       // an audio input: the signal is the instrument
    std::atomic<int> audioLeft { 0 }, audioRight { 1 };      // device input channels; right -1 = mono
    std::atomic<float> levelL { 0.0f }, levelR { 0.0f };     // the meter: pre-trim peaks, falling 20 dB/s
    std::atomic<uint32_t> clipMs { 0 };                      // when the input last touched full scale
    std::atomic<int> output { 0 };                           // which output it lands on
    MidiMessageCollector collector;
    MidiBuffer block;
    MidiBuffer holdBlock;          // this block's MIDI less the note-ons, for programs left sounding
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
    if (startAudioDevice)
    {
        std::unique_ptr<XmlElement> savedAudio (settings.getXmlValue ("audioDeviceState"));
        deviceManager.initialise (0, 2, savedAudio.get(), true);

        // First run (or a saved setup that still uses the old ALSA-emulation
        // default): prefer the JACK type (PipeWire's graph, no resampling, small
        // quantum) at 48 kHz. Any explicit later choice in Audio Settings sticks.
        const bool oldDefault = savedAudio != nullptr
                                && savedAudio->getStringAttribute ("deviceType") == "ALSA"
                                && savedAudio->getStringAttribute ("audioOutputDeviceName").contains ("PipeWire")
                                && ! settings.getBoolValue ("jackMigrated", false);
        if (savedAudio == nullptr || oldDefault)
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

    // Backing tracks: WAV, AIFF, FLAC, Ogg and MP3, read ahead on their own thread.
    trackFormats.registerBasicFormats();
    trackReader.startThread (Thread::Priority::high);

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
    trackReader.stopThread (2000);

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

void Engine::changeListenerCallback (ChangeBroadcaster*)
{
    if (auto xml = deviceManager.createStateXml())
        settings.setValue ("audioDeviceState", xml.get());
    else
        settings.removeValue ("audioDeviceState");
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
    if (d->getTypeName() == "ALSA")
    {
        if (d->getCurrentBufferSizeSamples() < 128)
            out.add (String (d->getCurrentBufferSizeSamples()) + "-sample buffer on a direct ALSA device: expect dropouts and crackle. 256 is safe on a USB interface, 128 on a good PCI one.");
        if (s.inputDeviceName.isNotEmpty() && s.inputDeviceName != s.outputDeviceName && s.inputChannels.countNumberOfSetBits() > 0)
            out.add ("input (" + s.inputDeviceName + ") and output (" + s.outputDeviceName + ") are different devices, so their clocks drift and the audio crackles. Use the same interface for both, or untick the input channels.");
    }
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
    teardownRuntimes();
    setup = std::move (newSetup);
    // A setup with an audio input needs the device's inputs; a keyboard rig does not pay for them.
    for (auto& in : setup.inputs)
        if (in.isAudio()) { ensureAudioInputsOpen(); break; }
    int outChannels = 0;
    for (auto& o : setup.outputs) outChannels = jmax (outChannels, o.left + 1, o.right + 1);
    if (outChannels > 2) ensureAudioOutputsOpen (outChannels);
    // The audio thread reads the tap controller from its own copy.
    tapCC.store (jlimit (0, 127, setup.tapTempoCC), std::memory_order_relaxed);
    nextCC.store (jlimit (0, 127, setup.nextSongCC), std::memory_order_relaxed);
    prevCC.store (jlimit (0, 127, setup.prevSongCC), std::memory_order_relaxed);
    transportCCAtomic.store (jlimit (0, 127, setup.transportCC), std::memory_order_relaxed);
    tapTimes.clear();
    rebuildRuntimes();
    listeners.call ([] (Listener& l) { l.setupChanged(); });
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
                for (auto& e : s->effects)
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

void Engine::armTapTempoLearn (bool b) { tapLearnArmed.store (b); }

void Engine::setTapTempoCC (int cc)
{
    setup.tapTempoCC = jlimit (0, 127, cc);
    tapCC.store (setup.tapTempoCC, std::memory_order_relaxed);
}

void Engine::rebuildRuntimes()
{
    buildOutputs();
    for (int i = 0; i < (int) setup.inputs.size(); ++i)
    {
        auto rt = std::make_unique<InputRuntime>();
        rt->collector.reset (sampleRate);
        applyInputDef (*rt, setup.inputs[(size_t) i]);
        runtimes.push_back (std::move (rt));
    }

    openMidiDevices();

    for (int i = 0; i < (int) setup.inputs.size(); ++i)
    {
        auto* prog = ensureProgramLoaded (i, setup.inputs[(size_t) i].currentProgram);
        const ScopedLock sl (lock);
        runtimes[(size_t) i]->active = prog;
    }
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
                for (auto& e : s->effects) if (e->plugin != nullptr) e->plugin->requestQuit();
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
void Engine::applyInputDef (InputRuntime& rt, const InputDef& def)
{
    rt.channel.store (def.channel);
    rt.respondToProgramChange.store (def.respondToProgramChange);
    rt.programChangeChannel.store (def.programChangeChannel);
    rt.audio.store (def.isAudio());
    rt.audioLeft.store (def.audioLeft);
    rt.audioRight.store (def.audioRight);
    rt.output.store (def.output);
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
    setup.outputs[(size_t) output].volumeCC = cc;
    outputs[(size_t) output]->volumeCC.store (cc);
    listeners.call ([] (Listener& l) { l.outputsChanged(); });
}

void Engine::armOutputVolumeLearn (int output)
{
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

    auto rt = std::make_unique<InputRuntime>();
    rt->collector.reset (sampleRate);
    applyInputDef (*rt, def);
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
                    for (auto& e : s->effects) e->inputIndex.store (i);
                }
                for (auto& e : rt->effects) e->inputIndex.store (i);
            }
    }

    for (auto& [p, rt] : in->loaded)
        destroyProgramPlugins (*rt);

    setup.inputs.erase (setup.inputs.begin() + inputIndex);
    openMidiDevices();
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::setInputName (int inputIndex, const String& name)
{
    if (validInput (inputIndex))
        setup.inputs[(size_t) inputIndex].name = name;
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

void Engine::refreshMidiDevices()
{
    openMidiDevices();
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::openMidiDevices()
{
    auto available = MidiInput::getAvailableDevices();

    StringArray wanted;
    for (int i = 0; i < (int) setup.inputs.size(); ++i)
    {
        auto& def = setup.inputs[(size_t) i];

        // Re-find the device by identifier, then by name (identifiers can change on re-plug).
        bool found = false;
        for (auto& d : available)
            if (d.identifier == def.midiDeviceIdentifier && def.midiDeviceIdentifier.isNotEmpty())
                found = true;
        if (! found && def.midiDeviceName.isNotEmpty())
            for (auto& d : available)
                if (d.name == def.midiDeviceName) { def.midiDeviceIdentifier = d.identifier; found = true; break; }

        if (found)
            wanted.addIfNotAlreadyThere (def.midiDeviceIdentifier);

        const ScopedLock sl (lock);
        runtimes[(size_t) i]->deviceIdentifier = found ? def.midiDeviceIdentifier : String();
    }

    for (auto& d : available)
        deviceManager.setMidiInputDeviceEnabled (d.identifier, wanted.contains (d.identifier));
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

    listeners.call ([] (Listener& l) { l.setupChanged(); });
    return to;
}

void Engine::setSongName (int songIndex, const String& name)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    setup.songs[(size_t) songIndex].name = name;
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
    setup.songs[(size_t) songIndex].tempoBpm = bpm <= 0.0 ? 0.0 : jlimit (20.0, 300.0, bpm);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
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
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::selectSong (int songIndex)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;

    const auto& s = setup.songs[(size_t) songIndex];
    for (int i = 0; i < (int) setup.inputs.size(); ++i)
        if (const int p = s.programFor (i); p >= 0)
            selectProgram (i, p);

    // Tempo after the programs: a plugin that has just loaded is told the tempo
    // by setTempoBpm, so doing it in this order means no program starts at the
    // old one.
    if (s.tempoBpm > 0.0)
    {
        setTempoBpm (s.tempoBpm);
        resetTapTempo();
    }
    // Its backing track is made ready, not started: starting is always on purpose.
    armTrack (songIndex);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

//==============================================================================
// Backing tracks
//==============================================================================
void Engine::addSongStem (int songIndex, const StemDef& d)
{
    if (songIndex < 0 || songIndex >= (int) setup.songs.size()) return;
    setup.songs[(size_t) songIndex].stems.push_back (d);
    if (songIndex == trackSong) armTrack (songIndex);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
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

void Engine::armTrack (int songIndex)
{
    // Whatever is playing stops first, with its short fade, so a change of song never clicks.
    if (player != nullptr && player->isRunning())
    {
        player->stop();
        for (int i = 0; i < 20 && player->isRunning(); ++i) Thread::sleep (5);
    }

    std::unique_ptr<SongPlayer> fresh;
    if (songIndex >= 0 && songIndex < (int) setup.songs.size() && ! setup.songs[(size_t) songIndex].stems.empty())
    {
        StringArray problems;
        fresh = std::make_unique<SongPlayer> (trackReader, trackFormats, getChartsFolder(),
                                              setup.songs[(size_t) songIndex], sampleRate, blockSize, problems);
        if (! problems.isEmpty())
            listeners.call ([p = problems.joinIntoString ("; ")] (Listener& l) { l.statusMessage ("Backing track: " + p); });
        if (! fresh->hasStems()) fresh.reset();
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

void Engine::startTrack()
{
    if (player == nullptr) return;
    if (player->isRunning()) return;
    player->rewind();
    player->waitUntilReady (800);            // the first second is on disk until it is buffered

    int beats = 0, output = 0;
    double bpm = setup.tempoBpm > 0.0 ? setup.tempoBpm : 120.0;
    if (trackSong >= 0 && trackSong < (int) setup.songs.size())
    {
        const auto& s = setup.songs[(size_t) trackSong];
        beats = s.countInBeats;
        output = s.countInOutput;
        if (s.tempoBpm > 0.0) bpm = s.tempoBpm;
    }
    player->start (beats, 60.0 / bpm, output);
    listeners.call ([] (Listener& l) { l.transportChanged(); });
}

void Engine::stopTrack()
{
    if (player == nullptr || ! player->isRunning()) return;
    player->stop();
    listeners.call ([] (Listener& l) { l.transportChanged(); });
}

void Engine::toggleTrack()
{
    if (isTrackRunning()) stopTrack(); else startTrack();
}

void Engine::setTransportCC (int cc)
{
    setup.transportCC = jlimit (0, 127, cc);
    transportCCAtomic.store (setup.transportCC, std::memory_order_relaxed);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
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
                for (auto& e : s->effects) if (e->plugin != nullptr) e->plugin->resetMusicalPosition();
            }
            for (auto& e : rt->effects) if (e->plugin != nullptr) e->plugin->resetMusicalPosition();
        }
    for (auto& o : outputs)
        for (auto& e : o->chain.effects) if (e->plugin != nullptr) e->plugin->resetMusicalPosition();
}

int Engine::addSet (const String& name)
{
    SetDef s;
    s.name = name.isNotEmpty() ? name : "Set " + String ((int) setup.sets.size() + 1);
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

void Engine::setNextSongCC (int cc)
{
    setup.nextSongCC = jlimit (0, 127, cc);
    nextCC.store (setup.nextSongCC, std::memory_order_relaxed);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::setPrevSongCC (int cc)
{
    setup.prevSongCC = jlimit (0, 127, cc);
    prevCC.store (setup.prevSongCC, std::memory_order_relaxed);
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::armSongLearn (bool forNext)
{
    songLearnForNext = forNext;
    songLearnArmed.store (true);
}

//==============================================================================
void Engine::selectProgram (int inputIndex, int program)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;

    auto& def = setup.inputs[(size_t) inputIndex];
    auto& in = *runtimes[(size_t) inputIndex];
    def.currentProgram = program;

    auto* next = ensureProgramLoaded (inputIndex, program);

    /* Pull the outgoing program's state into the model before leaving it.
       Anything set inside a plugin's own window -- SWAM being told which
       controller is expression -- lives only in that process until it is
       captured, and switching away is the natural moment: the program change
       already marks the setup dirty and brings the autosave forward, so this
       costs no extra write, it just makes the write current. `active` only
       changes on this thread, so reading it outside the lock is safe. */
    if (in.active != nullptr && in.active != next)
        captureProgramState (inputIndex, *in.active);

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

    listeners.call ([=] (Listener& l) { l.programChanged (inputIndex, program); });
}

void Engine::setProgramName (int inputIndex, int program, const String& name)
{
    if (validInput (inputIndex) && validProgram (program))
        setup.inputs[(size_t) inputIndex].programs[(size_t) program].name = name;
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
    rt->mix.setSize (2, blockSize);
    rt->programMidi.ensureSize (2048);

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
    slot->midi.ensureSize (2048);
    nodeRegistry[slot->id] = slot.get();
    queueLoad (*slot, def.plugin, def.state);
    for (int e = 0; e < (int) def.effects.size(); ++e)
        slot->effects.push_back (buildEffect (inputIndex, program, slotIndex, e, def.effects[(size_t) e]));
    return slot;
}

std::unique_ptr<Engine::EffectRuntime> Engine::buildEffect (int inputIndex, int program, int slotIndex, int effectIndex, const EffectDef& def)
{
    auto fx = std::make_unique<EffectRuntime> (*this, inputIndex, program, slotIndex, effectIndex);
    fx->bypassed.store (def.bypassed);
    fx->midi.ensureSize (2048);
    nodeRegistry[fx->id] = fx.get();
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

        LoadResult result { job.nodeId, std::make_unique<RemotePlugin>(), {}, job.sampleRate, job.blockSize };
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

    if (r.plugin == nullptr)
    {
        node->loadError = r.error;
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
        const ScopedLock sl (lock);
        node->plugin = std::move (r.plugin);
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
    for (int i = 0; i < (int) runtimes.size(); ++i)
        if (runtimes[(size_t) i].get() == &in)
            captureProgramState (i, *rt);

    destroyProgramPlugins (*rt);
}

void Engine::destroyProgramPlugins (ProgramRuntime& rt, int patienceMs)
{
    for (auto& s : rt.slots)
    {
        for (auto& e : s->effects) destroyNode (e.get(), patienceMs);
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
        if (it->slot == slotIndex)       it = def.mappings.erase (it);
        else { if (it->slot > slotIndex) --it->slot; ++it; }
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
                }
            }
            resolveMappings (*rt, def);
        }
        if (removed != nullptr)
        {
            for (auto& e : removed->effects) destroyNode (e.get());
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

PERF_SLOT_SETTER (setSlotEnabled,    enabled,    enabled.store (v))
PERF_SLOT_SETTER (setSlotGainDb,     gainDb,     gain.store (Decibels::decibelsToGain (v, -60.0f)))
PERF_SLOT_SETTER (setSlotTranspose,  transpose,  transpose.store (v))
PERF_SLOT_SETTER (setSlotOutChannel, outChannel, outChannel.store (v))
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
    return slot < (int) rt.slots.size() ? &rt.slots[(size_t) slot]->effects : nullptr;
}

std::vector<EffectDef>* Engine::defChainFor (ProgramDef& def, int slot) const
{
    if (slot < 0) return &def.effects;
    return slot < (int) def.slots.size() ? &def.slots[(size_t) slot].effects : nullptr;
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

bool Engine::isPluginAlive (int inputIndex, int program, int slot, int effect) const
{
    auto* node = getNode (inputIndex, program, slot, effect);
    return node != nullptr && node->alive();
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
    if (node->plugin != nullptr && ! node->plugin->isAlive())
        return "Plugin process stopped: " + node->plugin->getLastError();
    return node->loadError;
}

void Engine::reloadPlugin (int inputIndex, int program, int slot, int effect)
{
    auto* node = getNode (inputIndex, program, slot, effect);
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

        if (node != nullptr && node->plugin != nullptr)
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
    std::vector<Event> events;
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

            case Event::tapLearn:
                setTapTempoCC (e.a);
                listeners.call ([cc = e.a] (Listener& l) { l.tapTempoLearned (cc); });
                break;

            case Event::outputLearn:
                setOutputVolumeCC (e.input, e.a);
                listeners.call ([o = e.input, cc = e.a] (Listener& l) { l.outputVolumeLearned (o, cc); });
                break;

            case Event::outputVolume:
                // The audio thread already moved the gain; this keeps the model and the fader with it.
                if (e.input >= 0 && e.input < (int) setup.outputs.size())
                {
                    const float f = (float) e.a / 127.0f;
                    setup.outputs[(size_t) e.input].gainDb = Decibels::gainToDecibels (f * f, -60.0f);
                    listeners.call ([] (Listener& l) { l.outputsChanged(); });
                }
                break;

            case Event::tapTempo:
                if (const double bpm = tapTempo(); bpm > 0.0)
                    listeners.call ([bpm] (Listener& l)
                        { l.statusMessage ("Tempo " + String (bpm, 1) + " bpm"); });
                break;

            case Event::songLearn:
            {
                if (songLearnForNext) setNextSongCC (e.a);
                else                  setPrevSongCC (e.a);
                const String what = songLearnForNext ? "Next song" : "Previous song";
                listeners.call ([what, cc = e.a] (Listener& l)
                    { l.statusMessage (what + ": CC " + String (cc)); });
                break;
            }

            case Event::transportLearn:
                setTransportCC (e.a);
                listeners.call ([cc = e.a] (Listener& l) { l.statusMessage ("Backing track start/stop: CC " + String (cc)); });
                break;

            case Event::transport:
                if (! hasTrack())
                    listeners.call ([] (Listener& l) { l.statusMessage ("This song has no backing track."); });
                else
                    toggleTrack();
                break;

            case Event::trackFinished:
                listeners.call ([] (Listener& l) { l.transportChanged(); });
                break;

            case Event::songStep:
            {
                if (setup.currentSet < 0)
                {
                    listeners.call ([] (Listener& l)
                        { l.statusMessage ("No set chosen -- pick one first."); });
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
                    listeners.call ([] (Listener& l) { l.statusMessage ("End of the set."); });
                }
                break;
            }

            case Event::learn:
                listeners.call ([&] (Listener& l) { l.learnReceived (e.input, (MappingDef::Source) e.a, e.b); });
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
                    captureProgramState (e.input, *rt);
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
                        const auto name = node->plugin->getName();
                        listeners.call ([&] (Listener& l) { l.statusMessage (name + " stopped (" + node->plugin->getLastError() + "). Use Reload on that plugin to restart it."); });
                        notifyContent (e.input, e.a);
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
                    if (rt.get() != in.active && ! in.isReleasing (rt.get()))
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
    const String id = source->getIdentifier();
    if (m.getChannel() == 0) return;    // sysex / realtime: ignored

    const ScopedLock sl (lock);
    for (int i = 0; i < (int) runtimes.size(); ++i)
        if (runtimes[(size_t) i]->deviceIdentifier == id)
            routeMidi (*runtimes[(size_t) i], i, m);
}

void Engine::injectMidi (int inputIndex, const MidiMessage& m)
{
    if (m.getChannel() == 0) return;
    MidiMessage stamped (m);
    if (stamped.getTimeStamp() <= 0.0)
        stamped.setTimeStamp (Time::getMillisecondCounterHiRes() * 0.001);

    const ScopedLock sl (lock);
    if (inputIndex >= 0 && inputIndex < (int) runtimes.size())
        routeMidi (*runtimes[(size_t) inputIndex], inputIndex, stamped);
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

void Engine::routeMidi (InputRuntime& in, int i, const MidiMessage& m)
{
    const int ch = m.getChannel();
    const int want = in.channel.load();

    // Program Change is filtered on its own channel, which is not always the one the
    // keyboard plays on, so this is tested before the note filter would drop it.
    /* Learning the tap controller comes first, and outside the channel filter
       for the same reason the tap itself is: the pedal you press to teach it is
       the pedal you will press to use it, whatever channel it happens to send
       on. Ahead of the tap check so an assigned controller can be reassigned. */
    if (tapLearnArmed.load() && m.isController() && m.getControllerValue() >= 64)
    {
        tapLearnArmed.store (false);
        postEvent ({ Event::tapLearn, i, m.getControllerNumber(), 0, 0, 0 });
        return;
    }

    /* Learning the song footswitches, same rule as the tap: the pedal you press
       to teach it is the pedal you will press to use it. */
    if (songLearnArmed.load() && m.isController() && m.getControllerValue() >= 64)
    {
        songLearnArmed.store (false);
        postEvent ({ Event::songLearn, i, m.getControllerNumber(), 0, 0, 0 });
        return;
    }
    if (transportLearnArmed.load() && m.isController() && m.getControllerValue() >= 64)
    {
        transportLearnArmed.store (false);
        postEvent ({ Event::transportLearn, i, m.getControllerNumber(), 0, 0, 0 });
        return;
    }

    /* Stepping through the set, outside the channel filter and whatever is
       loaded -- a footswitch has to work all night. Only the press counts. */
    if (m.isController() && m.getControllerValue() >= 64)
    {
        const int cc = m.getControllerNumber();
        if (const int n = nextCC.load (std::memory_order_relaxed); n > 0 && cc == n)
        { postEvent ({ Event::songStep, i, +1, 0, 0, 0 }); return; }
        if (const int p = prevCC.load (std::memory_order_relaxed); p > 0 && cc == p)
        { postEvent ({ Event::songStep, i, -1, 0, 0, 0 }); return; }
        if (const int t = transportCCAtomic.load (std::memory_order_relaxed); t > 0 && cc == t)
        { postEvent ({ Event::transport, i, 0, 0, 0, 0 }); return; }
    }

    /* Tap tempo, outside the channel filter: the tempo belongs to the rig, so a
       footswitch taps it whatever this input is set to play on and whatever
       program is loaded. Only the press counts, not the release. */
    if (m.isController())
    {
        /* An output's volume control, and learning one: like the tap pedal,
           it works on any input and channel, because a master fader belongs
           to the rig and not to one sound. */
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
                const float f = (float) m.getControllerValue() / 127.0f;
                outputs[(size_t) o]->gain.store (f * f);          // a fader's taper: half way is -12 dB
                postEvent ({ Event::outputVolume, o, m.getControllerValue(), 0, 0, 0 });
                return;
            }
    }

    if (const int tap = tapCC.load (std::memory_order_relaxed);
        tap > 0 && m.isController() && m.getControllerNumber() == tap && m.getControllerValue() >= 64)
    {
        postEvent ({ Event::tapTempo, i, 0, 0, 0, 0 });
        return;
    }

    if (m.isProgramChange())
    {
        if (in.respondToProgramChange.load() && acceptsProgramChangeOn (in, ch))
            postEvent ({ Event::programChange, i, m.getProgramChangeNumber(), 0, 0, 0 });
        return;
    }

    if (want != 0 && ch != want) return;

    if (learnArmed.load())
    {
        if (m.isController())            { learnArmed = false; postEvent ({ Event::learn, i, (int) MappingDef::Source::CC, m.getControllerNumber(), 0, 0 }); }
        else if (m.isPitchWheel())       { learnArmed = false; postEvent ({ Event::learn, i, (int) MappingDef::Source::PitchBend, 0, 0, 0 }); }
        else if (m.isChannelPressure())  { learnArmed = false; postEvent ({ Event::learn, i, (int) MappingDef::Source::ChannelPressure, 0, 0, 0 }); }
    }

    in.collector.addMessageToQueue (m);
}

void Engine::renderBlockForTesting (float* const* out, int numOut, int numSamples)
{
    audioDeviceIOCallbackWithContext (nullptr, 0, out, numOut, numSamples, {});
}

void Engine::renderBlockForTesting (const float* const* in, int numIn, float* const* out, int numOut, int numSamples)
{
    audioDeviceIOCallbackWithContext (in, numIn, out, numOut, numSamples, {});
}

//==============================================================================
// Audio thread
//==============================================================================
void Engine::audioDeviceAboutToStart (AudioIODevice* device)
{
    sampleRate = device->getCurrentSampleRate();
    blockSize  = device->getCurrentBufferSizeSamples();

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
                prep (*s);
                for (auto& e : s->effects) prep (*e);
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
    if (! callbackThreadKnown || ! pthread_equal (callbackThread, pthread_self()))
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

    const ScopedLock sl (lock);
    bool panicNow = panicRequested.exchange (false);

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

        /* Each input lands on its output's mix; each output then runs its
           master chain, takes its volume, and goes to its two device channels.
           With the one default output on channels 1-2 this is what the single
           sum used to be, one chain and one multiply later. */
        for (auto& o : outputs)
            o->mix.clear (0, n);
        for (auto& rt : runtimes)
        {
            if (outputs.empty()) break;
            int idx = rt->output.load (std::memory_order_relaxed);
            if (idx < 0 || idx >= (int) outputs.size()) idx = 0;
            float* busOut[2] = { outputs[(size_t) idx]->mix.getWritePointer (0), outputs[(size_t) idx]->mix.getWritePointer (1) };
            processInput (*rt, inChunk, numIn, busOut, 2, n, panicNow);
        }

        // The backing track, onto its outputs ahead of their master effects and volume.
        if (player != nullptr)
        {
            float* trackOut[16][2];
            const int numTrackOutputs = jmin ((int) outputs.size(), 16);
            for (int o = 0; o < numTrackOutputs; ++o)
            {
                trackOut[o][0] = outputs[(size_t) o]->mix.getWritePointer (0);
                trackOut[o][1] = outputs[(size_t) o]->mix.getWritePointer (1);
            }
            const auto happened = player->render (trackOut, numTrackOutputs, n);
            if (happened == SongPlayer::Happened::stemsStarted)
                resetPluginPositions();
            else if (happened == SongPlayer::Happened::ended || happened == SongPlayer::Happened::stopped)
                postEvent ({ Event::trackFinished, 0, 0, 0, 0, 0 });
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
        }
        panicNow = false;
    }
}

void Engine::processInput (InputRuntime& in, const float* const* audioIn, int numIn,
                           float* const* out, int numOut, int numSamples, bool panicNow)
{
    in.block.clear();
    in.collector.removeNextBlockOfMessages (in.block, numSamples);

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
            auto peakOf = [numSamples] (const float* p) { const auto mm = FloatVectorOperations::findMinAndMax (p, numSamples); return jmax (std::abs (mm.getStart()), std::abs (mm.getEnd())); };
            const float pl = peakOf (inL);
            const float pr = inR != nullptr ? peakOf (inR) : pl;
            const float fall = std::exp (-2.3026f * (float) numSamples / (float) jmax (1.0, sampleRate));   // 20 dB per second
            in.levelL.store (jmax (pl, in.levelL.load (std::memory_order_relaxed) * fall), std::memory_order_relaxed);
            in.levelR.store (jmax (pr, in.levelR.load (std::memory_order_relaxed) * fall), std::memory_order_relaxed);
            if (pl >= 0.999f || pr >= 0.999f)
                in.clipMs.store (Time::getMillisecondCounter(), std::memory_order_relaxed);
        }
    }

    if (in.active != nullptr)
        processProgram (*in.active, in.block, inL, inR, out, numOut, numSamples, panicNow);

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
        processProgram (*p, *midi, nullptr, nullptr, out, numOut, numSamples, panicNow);

        if (p->holding && ! panicNow && p->anythingHeld()) { ++it; continue; }    // still held: the tail waits
        p->holding = false;
        p->releaseSamplesLeft -= numSamples;
        if (p->releaseSamplesLeft <= 0 || panicNow)
            it = in.releasing.erase (it);
        else
            ++it;
    }
}

static inline float normalisedValueOf (const MidiMessage& m, MappingDef::Source s)
{
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
        if (! fx->plugin->finishProcess (stereo.getWritePointer (0), stereo.getWritePointer (1), blockDeadline))
        {
            stereo.clear();
            lateBlocks.fetch_add (1);
        }
    }
}

void Engine::processProgram (ProgramRuntime& prog, const MidiBuffer& in, const float* inL, const float* inR,
                             float* const* out, int numOut, int numSamples, bool panicNow)
{
    for (auto& slot : prog.slots)
        slot->midi.clear();
    prog.programMidi.clear();

    if (prog.needsNotesOff || panicNow)
    {
        prog.needsNotesOff = false;
        prog.holding = false;
        prog.clearHeld();
        for (auto& slot : prog.slots)
            addNotesOff (slot->midi, panicNow);
        addNotesOff (prog.programMidi, panicNow);
    }

    // Route incoming MIDI: mappings first, then per-slot filtering.
    for (const auto meta : in)
    {
        const auto m = meta.getMessage();
        const int pos = meta.samplePosition;
        prog.track (m);

        bool consumed = false;
        for (auto& map : prog.mappings)
        {
            if (map.plugin == nullptr || map.paramIndex < 0 || ! map.plugin->isAlive() || ! mappingMatches (map.def, m)) continue;
            const float norm = normalisedValueOf (m, map.def.source);
            const float value = map.def.minValue + (map.def.maxValue - map.def.minValue) * norm;
            map.plugin->queueParameterChange (map.paramIndex, jlimit (0.0f, 1.0f, value));
            if (! map.def.passThrough) consumed = true;
        }
        if (consumed) continue;

        prog.programMidi.addEvent (m, pos);

        for (auto& slot : prog.slots)
        {
            if (! slot->alive() || ! slot->enabled.load()) continue;

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
    }

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
        if (! slot->alive()) continue;
        AudioBuffer<float> stereo (slot->stereo.getArrayOfWritePointers(), 2, numSamples);

        if (! slot->plugin->finishProcess (stereo.getWritePointer (0), stereo.getWritePointer (1), blockDeadline))
        {
            stereo.clear();
            lateBlocks.fetch_add (1);
            continue;
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

} // namespace perf
