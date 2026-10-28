// Headless integration test: loads a real LV2 instrument, routes MIDI through the
// engine and checks audio, program changes, mappings and learn.
#include "Engine.h"
#include "TempoDetect.h"
#include "Stretch.h"
#include "MidiFx.h"
#include <ableton/Link.hpp>
#include "PluginIcons.h"
#include "MappingSuggestions.h"
#include "ClapPluginFormat.h"
#include "GmSynth.h"
#include "MidiEditor.h"
#include <juce_events/juce_events.h>
#include <cstdio>
#include <cmath>
#include <set>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#if defined (SYS_pidfd_open) && defined (SYS_pidfd_getfd)
 #define HAVE_PIDFD 1
#endif

using namespace perf;
using namespace juce;

static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } else std::printf ("ok   %s\n", #cond); } while (0)

namespace
{
    constexpr int kBlock = 512;

    void pump (int ms)
    {
        MessageManager::getInstance()->runDispatchLoopUntil (ms);
    }

    /** Renders `blocks` blocks and returns the peak RMS across them. */
    float render (Engine& e, int blocks)
    {
        AudioBuffer<float> buf (2, kBlock);
        float peakRms = 0.0f;
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear();
            e.renderBlockForTesting (buf.getArrayOfWritePointers(), 2, kBlock);
            peakRms = jmax (peakRms, buf.getRMSLevel (0, 0, kBlock), buf.getRMSLevel (1, 0, kBlock));
        }
        return peakRms;
    }

    struct TestListener : public Engine::Listener
    {
        int learnInput = -1, learnNumber = -1; MappingDef::Source learnSource = MappingDef::Source::CC;
        int programChangedInput = -1, programChangedTo = -1;
        void learnReceived (int i, MappingDef::Source s, int n) override { learnInput = i; learnSource = s; learnNumber = n; }
        void programChanged (int i, int p) override { programChangedInput = i; programChangedTo = p; }
        void statusMessage (const String& s) override { std::printf ("     [status] %s\n", s.toRawUTF8()); }
    };
}

int main()
{
    ScopedJuceInitialiser_GUI init;

    // Own scratch folder: PluginHost keeps its crash list ("dead man's pedal") next to
    // the settings file, and a plain temp file would share /tmp with every other run,
    // so an aborted run could blacklist a plugin for the next one.
    const File scratchDir = File::getSpecialLocation (File::tempDirectory)
                                .getChildFile ("StageRigEngineTest-" + String (Time::currentTimeMillis()));
    scratchDir.createDirectory();
    struct ScratchCleanup { File d; ~ScratchCleanup() { d.deleteRecursively(); } } scratchCleanup { scratchDir };
    TemporaryFile settingsTmp (scratchDir.getChildFile ("test.settings"));
    PropertiesFile::Options opts;
    opts.applicationName = "StageRigEngineTest";
    opts.filenameSuffix = "settings";
    opts.folderName = "StageRigEngineTest";
    opts.storageFormat = PropertiesFile::storeAsXML;
    PropertiesFile settings (settingsTmp.getFile(), opts);

    PluginHost host (settings);

    // --- scan LV2 plugins and pick an instrument -------------------------------
    AudioPluginFormat* lv2 = nullptr;
    for (auto* f : host.getFormatManager().getFormats())
        if (f->getName() == "LV2") lv2 = f;
    CHECK (lv2 != nullptr);
    if (lv2 == nullptr) return 1;

    auto ids = lv2->searchPathsForPlugins (lv2->getDefaultLocationsToSearch(), true, false);
    ids.removeDuplicates (false);        // LV2 reports the bundle path once per plugin
    std::printf ("     %d LV2 bundles found\n", ids.size());
    for (auto& id : ids)
        if (id.containsIgnoreCase ("calf"))
        {
            OwnedArray<PluginDescription> found;
            host.getKnownPlugins().scanAndAddFile (id, true, found, *lv2);
        }

    PluginDescription synth;
    bool haveSynth = false;
    for (auto& d : host.getKnownPlugins().getTypes())
        if (d.isInstrument && d.name.containsIgnoreCase ("Monosynth")) { synth = d; haveSynth = true; }
    if (! haveSynth)
        for (auto& d : host.getKnownPlugins().getTypes())
            if (d.isInstrument) { synth = d; haveSynth = true; break; }
    CHECK (haveSynth);
    if (! haveSynth) { std::printf ("SKIP: no LV2 instrument available (install calf-plugins on Debian/Ubuntu, lv2-calf-plugins on Fedora)\n"); return 77; }
    std::printf ("     using instrument: %s (%s)\n", synth.name.toRawUTF8(), synth.fileOrIdentifier.toRawUTF8());

    // --- engine ------------------------------------------------------------------
    Engine engine (host, settings, /*startAudioDevice*/ false);
    TestListener listener;
    engine.addListener (&listener);
    engine.setReleaseTailSeconds (0.1);

    // Plugins load on a background thread; wait for them (pumping the message loop so
    // the loaded processes get attached) before checking anything that needs them.
    auto settle = [&engine]
    {
        const auto t0 = Time::getMillisecondCounterHiRes();
        pump (30);
        while (engine.hasPendingLoads() && Time::getMillisecondCounterHiRes() - t0 < 120000.0) pump (30);
    };

    CHECK (engine.getSetup().inputs.size() == 2);         // Upper (ch1), Lower (ch2)
    CHECK (engine.isProgramLoaded (0, 0));

    CHECK (RemotePlugin::findHostExecutable().existsAsFile());
    std::printf ("     plugin host: %s\n", RemotePlugin::findHostExecutable().getFullPathName().toRawUTF8());

    String err;
    CHECK (engine.addSlot (0, 0, synth, err));
    settle();
    if (err.isNotEmpty()) std::printf ("     addSlot error: %s\n", err.toRawUTF8());
    auto* inst = engine.getPlugin (0, 0, 0);
    CHECK (inst != nullptr && inst->isAlive());
    if (inst == nullptr) return 1;
    std::printf ("     plugin '%s' pid-hosted, params=%d\n", inst->getName().toRawUTF8(), (int) inst->getParameters().size());

    // Silence before any note.
    const float silence = render (engine, 4);
    CHECK (silence < 1e-4f);

    // --- a note held through a program change keeps sounding until it is let go ------------
    {
        engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
        render (engine, 4);
        engine.selectProgram (0, 1); settle();                       // program 1 is empty
        const float heldAfterChange = render (engine, 24);           // about a quarter of a second later
        std::printf ("     held through a change: rms %.3f\n", heldAfterChange);
        CHECK (heldAfterChange > 0.02f);
        engine.injectMidi (0, MidiMessage::noteOff (1, 60));
        render (engine, 40);                                         // release, then the 0.1 s tail
        CHECK (render (engine, 4) < 1e-4f);

        // Hold off: the change releases everything, and only the tail remains.
        engine.setHoldAcrossChange (false);
        engine.selectProgram (0, 0); settle();
        engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
        render (engine, 4);
        engine.selectProgram (0, 1); settle();
        render (engine, 24);
        CHECK (render (engine, 4) < 1e-4f);
        engine.injectMidi (0, MidiMessage::noteOff (1, 60));
        engine.setHoldAcrossChange (true);
        engine.selectProgram (0, 0); settle();
        render (engine, 10);
    }

    // --- note on channel 1 -> Upper sounds -----------------------------------------
    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
    const float sounding = render (engine, 20);
    std::printf ("     rms while note held: %f\n", sounding);
    CHECK (sounding > 0.01f);

    engine.injectMidi (0, MidiMessage::noteOff (1, 60));
    render (engine, 200);                                    // ~2.3 s of release
    const float afterRelease = render (engine, 4);
    std::printf ("     rms after release: %f\n", afterRelease);
    CHECK (afterRelease < sounding * 0.5f);

    // --- channel filter: ch 2 must not reach Upper (ch 1) ---------------------------
    engine.injectMidi (0, MidiMessage::noteOn (2, 60, (uint8) 100));
    CHECK (render (engine, 20) < 1e-4f);
    engine.injectMidi (0, MidiMessage::noteOff (2, 60));

    // --- key range + transpose ------------------------------------------------------
    engine.setSlotKeyRange (0, 0, 0, 60, 72);
    engine.injectMidi (0, MidiMessage::noteOn (1, 48, (uint8) 100));   // below range
    CHECK (render (engine, 20) < 1e-4f);
    engine.injectMidi (0, MidiMessage::noteOff (1, 48));
    engine.setSlotKeyRange (0, 0, 0, 0, 127);

    // --- Program Change channel ---------------------------------------------------------
    // Input 0 plays on channel 1. A keyboard may send Program Change somewhere else
    // entirely (a Jupiter-50 sends registrations on 16 while playing on 1/3/4).
    {
        engine.selectProgram (0, 0); settle();
        const int start = engine.getSetup().inputs[0].currentProgram;
        CHECK (start == 0);

        // default (same as notes): a change on the note channel selects, one elsewhere does not
        engine.injectMidi (0, MidiMessage::programChange (1, 5)); pump (40);
        CHECK (engine.getSetup().inputs[0].currentProgram == 5);
        engine.injectMidi (0, MidiMessage::programChange (16, 9)); pump (40);
        CHECK (engine.getSetup().inputs[0].currentProgram == 5);      // ignored: wrong channel

        // a specific channel: only that one is accepted
        engine.setInputProgramChangeChannel (0, 16);
        engine.injectMidi (0, MidiMessage::programChange (1, 7)); pump (40);
        CHECK (engine.getSetup().inputs[0].currentProgram == 5);      // the note channel no longer selects
        engine.injectMidi (0, MidiMessage::programChange (16, 9)); pump (40);
        CHECK (engine.getSetup().inputs[0].currentProgram == 9);
        std::printf ("     program change on ch16 while playing ch1: program %d\n", engine.getSetup().inputs[0].currentProgram);

        // any channel
        engine.setInputProgramChangeChannel (0, InputDef::pcChannelAny);
        engine.injectMidi (0, MidiMessage::programChange (4, 3)); pump (40);
        CHECK (engine.getSetup().inputs[0].currentProgram == 3);

        // notes still obey the note channel, whatever the PC channel is
        engine.setInputProgramChangeChannel (0, 16);
        engine.selectProgram (0, 0); settle();
        engine.injectMidi (0, MidiMessage::noteOn (16, 60, (uint8) 100));   // wrong channel for notes
        CHECK (render (engine, 20) < 1e-4f);
        engine.injectMidi (0, MidiMessage::noteOff (16, 60));
        engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));    // the note channel
        CHECK (render (engine, 20) > 0.01f);
        engine.injectMidi (0, MidiMessage::noteOff (1, 60)); render (engine, 60);

        // respondToProgramChange still wins over any of this
        engine.setInputRespondToProgramChange (0, false);
        engine.injectMidi (0, MidiMessage::programChange (16, 11)); pump (40);
        CHECK (engine.getSetup().inputs[0].currentProgram == 0);
        engine.setInputRespondToProgramChange (0, true);
        engine.setInputProgramChangeChannel (0, InputDef::pcChannelSameAsNotes);
    }

    // --- velocity range, velocity curve, pan -----------------------------------------
    engine.setSlotVelocityRange (0, 0, 0, 90, 127);
    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 50));    // too soft for this layer
    CHECK (render (engine, 20) < 1e-4f);
    engine.injectMidi (0, MidiMessage::noteOff (1, 60));
    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 110));   // inside
    CHECK (render (engine, 20) > 0.01f);
    engine.injectMidi (0, MidiMessage::noteOff (1, 60));
    render (engine, 100);
    engine.setSlotVelocityRange (0, 0, 0, 1, 127);

    CHECK (Engine::curveVelocity (127, 0.7f) == 127 && Engine::curveVelocity (1, -0.7f) >= 1);
    CHECK (Engine::curveVelocity (64, 0.0f) == 64);
    CHECK (Engine::curveVelocity (64, 1.0f) > 100 && Engine::curveVelocity (64, -1.0f) < 20);
    for (int v = 2; v <= 127; ++v) CHECK (Engine::curveVelocity (v, 0.5f) >= Engine::curveVelocity (v - 1, 0.5f));   // monotonic

    {
        float l = 0, r = 0;
        Engine::panGains (0.0f, l, r);  CHECK (l == 1.0f && r == 1.0f);
        Engine::panGains (-1.0f, l, r); CHECK (l == 1.0f && r == 0.0f);
        Engine::panGains (0.5f, l, r);  CHECK (std::abs (l - 0.5f) < 1e-6f && r == 1.0f);
        // Hard left: the right channel of the mix must be silent while the left sounds.
        engine.setSlotPan (0, 0, 0, -1.0f);
        engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
        AudioBuffer<float> buf (2, 512);
        double sl = 0, sr = 0;
        for (int b = 0; b < 20; ++b)
        {
            buf.clear();
            float* outs[2] = { buf.getWritePointer (0), buf.getWritePointer (1) };
            engine.renderBlockForTesting (outs, 2, 512);
            sl += buf.getRMSLevel (0, 0, 512); sr += buf.getRMSLevel (1, 0, 512);
        }
        std::printf ("     pan hard left: L %.4f  R %.4f\n", sl / 20, sr / 20);
        CHECK (sl / 20 > 0.005 && sr / 20 < 1e-5);
        engine.injectMidi (0, MidiMessage::noteOff (1, 60));
        engine.setSlotPan (0, 0, 0, 0.0f);
        render (engine, 100);
    }

    // --- slot disabled -> silent ------------------------------------------------------
    engine.setSlotEnabled (0, 0, 0, false);
    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
    CHECK (render (engine, 20) < 1e-4f);
    engine.injectMidi (0, MidiMessage::noteOff (1, 60));
    engine.setSlotEnabled (0, 0, 0, true);
    render (engine, 100);

    // --- CC mapping -----------------------------------------------------------------
    const ParamInfo* target = nullptr;
    for (auto& p : inst->getParameters())
        if (p.automatable && ! p.discrete && p.name.containsIgnoreCase ("cutoff")) { target = &p; break; }
    if (target == nullptr)
        for (auto& p : inst->getParameters())
            if (p.automatable && ! p.discrete) { target = &p; break; }
    CHECK (target != nullptr);
    if (target != nullptr)
    {
        std::printf ("     mapping CC 74 -> '%s' (id %s)\n", target->name.toRawUTF8(), target->id.toRawUTF8());
        MappingDef m;
        m.source = MappingDef::Source::CC; m.number = 74; m.slot = 0;
        m.paramId = target->id;
        m.minValue = 0.25f; m.maxValue = 0.75f;
        engine.addMapping (0, 0, m);
        CHECK (engine.getSetup().inputs[0].programs[0].mappings.size() == 1);

        // Values are read back from the plugin process, not from the host's cache.
        float v = -1.0f;
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 74, 127));
        render (engine, 2);
        CHECK (inst->fetchParameterValue (target->index, v));
        std::printf ("     param after CC=127: %f\n", v);
        CHECK (std::abs (v - 0.75f) < 0.02f);

        engine.injectMidi (0, MidiMessage::controllerEvent (1, 74, 0));
        render (engine, 2);
        CHECK (inst->fetchParameterValue (target->index, v));
        std::printf ("     param after CC=0: %f\n", v);
        CHECK (std::abs (v - 0.25f) < 0.02f);

        // A different CC is not affected by the mapping.
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 75, 127));
        render (engine, 2);
        CHECK (inst->fetchParameterValue (target->index, v));
        CHECK (std::abs (v - 0.25f) < 0.02f);
    }

    // --- a pedal on the instrument itself: on / off, toggled, and a level ------------------
    {
        MappingDef onOff;
        onOff.source = MappingDef::Source::CC; onOff.number = 80; onOff.slot = 0; onOff.paramId = "#on"; onOff.paramName = "On / off";
        engine.addMapping (0, 0, onOff);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 80, 0));
        render (engine, 2);
        pump (50);
        std::printf ("     CC 80 = 0 -> instrument enabled %d\n", (int) engine.getSetup().inputs[0].programs[0].slots[0].enabled);
        CHECK (! engine.getSetup().inputs[0].programs[0].slots[0].enabled);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 80, 127));
        render (engine, 2);
        pump (50);
        CHECK (engine.getSetup().inputs[0].programs[0].slots[0].enabled);
        engine.removeMapping (0, 0, (int) engine.getSetup().inputs[0].programs[0].mappings.size() - 1);

        MappingDef tog = onOff; tog.number = 81; tog.paramId = "#toggle";
        engine.addMapping (0, 0, tog);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 81, 127));           // press: off
        render (engine, 2);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 81, 0));             // release: nothing
        render (engine, 2);
        pump (50);
        CHECK (! engine.getSetup().inputs[0].programs[0].slots[0].enabled);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 81, 127));           // press again: on
        render (engine, 2);
        pump (50);
        std::printf ("     CC 81 pressed twice -> instrument enabled %d\n", (int) engine.getSetup().inputs[0].programs[0].slots[0].enabled);
        CHECK (engine.getSetup().inputs[0].programs[0].slots[0].enabled);
        engine.removeMapping (0, 0, (int) engine.getSetup().inputs[0].programs[0].mappings.size() - 1);

        MappingDef lev = onOff; lev.number = 82; lev.paramId = "#level";
        engine.addMapping (0, 0, lev);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 82, 64));            // halfway up the fader: about -24 dB
        render (engine, 2);
        pump (50);
        std::printf ("     CC 82 = 64 -> level %.1f dB\n", engine.getSetup().inputs[0].programs[0].slots[0].gainDb);
        CHECK (std::abs (engine.getSetup().inputs[0].programs[0].slots[0].gainDb + 24.0f) < 1.5f);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 82, 106));           // 0 dB
        render (engine, 2);
        pump (50);
        CHECK (std::abs (engine.getSetup().inputs[0].programs[0].slots[0].gainDb) < 1.0f);
        engine.removeMapping (0, 0, (int) engine.getSetup().inputs[0].programs[0].mappings.size() - 1);
        engine.setSlotGainDb (0, 0, 0, 0.0f);
    }

    // --- the looper: a bar of playing goes round on its own ---------------------------------
    {
        auto peakBlock = [&]
        {
            AudioBuffer<float> b (2, kBlock);
            b.clear();
            engine.renderBlockForTesting (b.getArrayOfWritePointers(), 2, kBlock);
            return b.getMagnitude (0, kBlock);
        };
        engine.setTempoBpm (120.0);                                      // a bar is two seconds
        const int blocksPerBar = (int) std::ceil (engine.getSampleRate() * 2.0 / kBlock);
        CHECK (engine.getLoopState (0) == Engine::LoopState::empty);
        engine.loopPedal (0);                                            // record
        render (engine, 1);
        CHECK (engine.getLoopState (0) == Engine::LoopState::recording);
        engine.injectMidi (0, MidiMessage::noteOn (1, 64, (uint8) 100));
        render (engine, 10);
        engine.injectMidi (0, MidiMessage::noteOff (1, 64));
        render (engine, blocksPerBar - 11);
        engine.injectMidi (0, MidiMessage::noteOn (1, 67, (uint8) 100));  // a second note, still down when the loop closes
        render (engine, 2);
        engine.loopPedal (0);                                            // play: one bar, the nearest
        render (engine, 1);
        engine.injectMidi (0, MidiMessage::noteOff (1, 67));
        std::printf ("     loop: state %d, %d bar(s)\n", (int) engine.getLoopState (0), engine.getLoopBars (0));
        CHECK (engine.getLoopState (0) == Engine::LoopState::playing && engine.getLoopBars (0) == 1);
        // Round it goes: in the second pass the note sounds again with nothing played.
        render (engine, blocksPerBar - 2);
        float loud = 0.0f;
        for (int b = 0; b < 12; ++b) loud = jmax (loud, peakBlock());
        std::printf ("     the loop playing the note again: peak %.3f\n", loud);
        CHECK (loud > 0.01f);
        /* The note that was still down when the loop closed has its note-off at
           the loop's last sample now, so each pass lets it go: at the end of a
           pass the program is quiet again rather than holding it for ever. */
        render (engine, blocksPerBar - 14);
        float seam = 0.0f;
        for (int b = 0; b < 2; ++b) seam = jmax (seam, peakBlock());
        render (engine, 8);
        float afterSeam = 0.0f;
        for (int b = 0; b < 2; ++b) afterSeam = jmax (afterSeam, peakBlock());
        std::printf ("     around the seam: %.3f then %.3f (the held note is let go, the loop's note restarts)\n", seam, afterSeam);
        engine.loopStop (0);
        render (engine, 30);
        float quiet = 0.0f;
        for (int b = 0; b < 4; ++b) quiet = jmax (quiet, peakBlock());
        std::printf ("     stopped: peak %.4f\n", quiet);
        CHECK (engine.getLoopState (0) == Engine::LoopState::stopped && quiet < 0.01f);
        engine.loopClear (0);
        render (engine, 1);
        CHECK (engine.getLoopState (0) == Engine::LoopState::empty && engine.getLoopBars (0) == 0);
    }

    // --- learn ------------------------------------------------------------------------
    engine.setLearnArmed (true);
    engine.injectMidi (0, MidiMessage::controllerEvent (1, 11, 64));
    pump (50);
    CHECK (! engine.isLearnArmed());
    CHECK (listener.learnInput == 0 && listener.learnSource == MappingDef::Source::CC && listener.learnNumber == 11);

    // --- program change via MIDI ------------------------------------------------------
    engine.injectMidi (0, MidiMessage::programChange (1, 5));
    pump (50);
    CHECK (listener.programChangedInput == 0 && listener.programChangedTo == 5);
    CHECK (engine.getSetup().inputs[0].currentProgram == 5);
    CHECK (engine.isProgramLoaded (0, 5));
    CHECK (engine.isProgramLoaded (0, 0));                  // still releasing
    render (engine, 20);                                     // > 0.1 s tail
    pump (1600);                                             // housekeeping tick unloads it
    CHECK (! engine.isProgramLoaded (0, 0));
    // Definition of program 0 survives unloading, with captured plugin state.
    CHECK (engine.getSetup().inputs[0].programs[0].slots.size() == 1);
    CHECK (engine.getSetup().inputs[0].programs[0].slots[0].state.getSize() > 0);

    // Program 5 is empty: notes produce silence.
    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
    CHECK (render (engine, 10) < 1e-4f);
    engine.injectMidi (0, MidiMessage::noteOff (1, 60));

    // Switching back reloads program 0 from its definition and it plays again.
    engine.selectProgram (0, 0);
    settle();
    CHECK (engine.isProgramLoaded (0, 0));
    CHECK (engine.isPluginAlive (0, 0, 0));
    CHECK (engine.getSetup().inputs[0].programs[0].mappings.size() == 1);
    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
    const float again = render (engine, 20);
    std::printf ("     rms after reload: %f\n", again);
    CHECK (again > 0.01f);
    engine.injectMidi (0, MidiMessage::noteOff (1, 60));

    // --- preload mode loads every used program ----------------------------------------
    CHECK (engine.addSlot (0, 7, synth, err));               // program 7 not loaded: def only
    CHECK (! engine.isProgramLoaded (0, 7));
    engine.setPreloadAllPrograms (true);
    pump (1600);
    settle();
    CHECK (engine.isProgramLoaded (0, 7));
    CHECK (engine.isPluginAlive (0, 7, 0));

    // --- copy / clear / removeSlot ------------------------------------------------------
    engine.copyProgram (0, 0, 9);
    settle();
    CHECK (engine.getSetup().inputs[0].programs[9].slots.size() == 1);
    CHECK (engine.getSetup().inputs[0].programs[9].mappings.size() == 1);
    engine.clearProgram (0, 9);
    settle();
    CHECK (engine.getSetup().inputs[0].programs[9].isEmpty());
    engine.removeSlot (0, 0, 0);
    CHECK (engine.getSetup().inputs[0].programs[0].slots.empty());
    CHECK (engine.getSetup().inputs[0].programs[0].mappings.empty());   // mapping to removed slot gone
    CHECK (render (engine, 4) >= 0.0f);                       // still renders fine

    // --- panic + save round trip ----------------------------------------------------------
    engine.panic();
    render (engine, 2);
    TemporaryFile setupTmp (".stagerig.json");
    CHECK (engine.captureSetup().saveToFile (setupTmp.getFile()).wasOk());
    Setup reloaded;
    CHECK (Setup::loadFromFile (setupTmp.getFile(), reloaded).wasOk());
    CHECK (reloaded.inputs[0].programs[7].slots.size() == 1 && reloaded.inputs[0].programs[7].slots[0].plugin.name == synth.name);
    CHECK (reloaded.preloadAllPrograms);

    // --- effect chains ----------------------------------------------------------------------
    {
        // Pick a Calf effect with a controllable input level; fall back to any effect.
        PluginDescription fx; bool haveFx = false;
        for (auto& d : host.getKnownPlugins().getTypes())
            if (! d.isInstrument && d.name.containsIgnoreCase ("Stereo Tools")) { fx = d; haveFx = true; }
        if (! haveFx)
            for (auto& d : host.getKnownPlugins().getTypes())
                if (! d.isInstrument && d.name.containsIgnoreCase ("Compressor")) { fx = d; haveFx = true; break; }
        CHECK (haveFx);
        if (haveFx)
        {
            std::printf ("     using effect: %s\n", fx.name.toRawUTF8());
            engine.setPreloadAllPrograms (false);
            engine.selectProgram (0, 0);
            settle();
            CHECK (engine.addSlot (0, 0, synth, err));
            settle();
            CHECK (engine.isPluginAlive (0, 0, 0));

            // Baseline level, instrument only.
            engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
            render (engine, 10);
            const float dry = render (engine, 10);
            std::printf ("     dry rms: %f\n", dry);
            CHECK (dry > 0.01f);

            // Slot insert chain: audio must still pass with the effect in place.
            CHECK (engine.addEffect (0, 0, 0, fx, err));
            settle();
            if (err.isNotEmpty()) std::printf ("     addEffect error: %s\n", err.toRawUTF8());
            CHECK (engine.getSetup().inputs[0].programs[0].slots[0].effects.size() == 1);
            auto* fxInst = engine.getPlugin (0, 0, 0, 0);
            CHECK (fxInst != nullptr && fxInst->isAlive());
            render (engine, 5);
            const float wet = render (engine, 10);
            std::printf ("     rms through slot effect: %f\n", wet);
            CHECK (wet > 0.01f);

            // Find a level/gain parameter and turn it down: output must drop.
            const ParamInfo* level = nullptr;
            if (fxInst != nullptr)
                for (auto& p : fxInst->getParameters())
                {
                    const auto n = p.name;
                    if (n.containsIgnoreCase ("Level In") || n.containsIgnoreCase ("Input Gain") || n.containsIgnoreCase ("Input Level")) { level = &p; break; }
                }
            if (level == nullptr && fxInst != nullptr)
                for (auto& p : fxInst->getParameters())
                    if (p.name.containsIgnoreCase ("Level Out") || p.name.containsIgnoreCase ("Output")) { level = &p; break; }
            CHECK (level != nullptr);
            if (level != nullptr)
            {
                std::printf ("     effect level parameter: '%s'\n", level->name.toRawUTF8());
                const String levelId = level->id;

                // Via a mapping to the effect (slot 0, effect 0): CC 7 -> level, min 0.
                MappingDef m;
                m.source = MappingDef::Source::CC; m.number = 7; m.slot = 0; m.effect = 0;
                m.paramId = levelId; m.minValue = 0.0f; m.maxValue = 1.0f;
                engine.addMapping (0, 0, m);
                engine.injectMidi (0, MidiMessage::controllerEvent (1, 7, 0));
                render (engine, 5);
                const float muted = render (engine, 10);
                std::printf ("     rms with effect level at min: %f\n", muted);
                CHECK (muted < wet * 0.2f);

                // Bypass restores the dry signal even though the level is still down.
                engine.setEffectBypassed (0, 0, 0, 0, true);
                CHECK (engine.getSetup().inputs[0].programs[0].slots[0].effects[0].bypassed);
                render (engine, 5);
                const float bypassed = render (engine, 10);
                std::printf ("     rms with effect bypassed: %f\n", bypassed);
                CHECK (bypassed > 0.01f);
                engine.setEffectBypassed (0, 0, 0, 0, false);

                // Removing the effect drops its mapping and leaves the instrument alone.
                engine.removeEffect (0, 0, 0, 0);
                level = nullptr;   // its process is gone with the effect
                CHECK (engine.getSetup().inputs[0].programs[0].slots[0].effects.empty());
                CHECK (engine.getSetup().inputs[0].programs[0].mappings.empty());
                render (engine, 5);
                CHECK (render (engine, 10) > 0.01f);

                // Program-level chain: same effect after the mix, mapped via slot == -1.
                CHECK (engine.addEffect (0, 0, -1, fx, err));
                settle();
                CHECK (engine.getSetup().inputs[0].programs[0].effects.size() == 1);
                auto* progFx = engine.getPlugin (0, 0, -1, 0);
                CHECK (progFx != nullptr && progFx->isAlive());
                if (progFx != nullptr)
                {
                    const int pLevel = progFx->findParameterIndex (levelId);
                    CHECK (pLevel >= 0);
                    float unity = 0.0f;
                    if (pLevel >= 0) progFx->fetchParameterValue (pLevel, unity);
                    MappingDef pm = m; pm.slot = -1; pm.effect = 0; pm.number = 8;
                    engine.addMapping (0, 0, pm);
                    render (engine, 5);
                    CHECK (render (engine, 10) > 0.01f);
                    engine.injectMidi (0, MidiMessage::controllerEvent (1, 8, 0));
                    render (engine, 5);
                    const float progMuted = render (engine, 10);
                    std::printf ("     rms with program effect level at min: %f\n", progMuted);
                    CHECK (progMuted < wet * 0.2f);
                    if (pLevel >= 0) progFx->setParameterValue (pLevel, unity);   // back to unity gain
                    render (engine, 2);
                }

                // Reordering: add a second program effect, move it first, mapping follows.
                CHECK (engine.addEffect (0, 0, -1, fx, err));
                settle();
                engine.moveEffect (0, 0, -1, 1, 0);
                CHECK (engine.getSetup().inputs[0].programs[0].mappings.size() == 1);
                CHECK (engine.getSetup().inputs[0].programs[0].mappings[0].effect == 1);   // the mapped one moved to index 1
                engine.removeEffect (0, 0, -1, 0);
                CHECK (engine.getSetup().inputs[0].programs[0].mappings[0].effect == 0);
            }

            engine.injectMidi (0, MidiMessage::noteOff (1, 60));
            render (engine, 20);

            // Effects survive a save/load round trip, including on the program chain.
            TemporaryFile fxTmp (".stagerig.json");
            CHECK (engine.captureSetup().saveToFile (fxTmp.getFile()).wasOk());
            Setup rl;
            CHECK (Setup::loadFromFile (fxTmp.getFile(), rl).wasOk());
            CHECK (rl.inputs[0].programs[0].effects.size() == 1);
            CHECK (rl.inputs[0].programs[0].effects[0].plugin.name == fx.name);
            CHECK (rl.inputs[0].programs[0].mappings.size() == 1 && rl.inputs[0].programs[0].mappings[0].slot == -1);

            // Loading that setup rebuilds the chain live.
            engine.loadSetup (rl);
            settle();
            CHECK (engine.isPluginAlive (0, 0, -1, 0));
            engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
            render (engine, 10);
            const float afterLoad = render (engine, 10);
            std::printf ("     rms after reloading setup with chain: %f\n", afterLoad);
            CHECK (afterLoad > 0.01f);
            CHECK (afterLoad < dry * 1.5f);   // effect state (unity gain) survived the round trip
            engine.injectMidi (0, MidiMessage::noteOff (1, 60));
            render (engine, 20);
        }
    }

    // --- mapping suggestions ------------------------------------------------------------------
    {
        engine.setPreloadAllPrograms (false);
        engine.selectProgram (0, 30);
        settle();
        CHECK (engine.addSlot (0, 30, synth, err));
        settle();
        auto* mono = engine.getPlugin (0, 30, 0);
        CHECK (mono != nullptr && mono->isAlive());
        if (mono != nullptr)
        {
            auto byNames = suggestMappingsFromNames (mono->getParameters());
            std::printf ("     %d name-based suggestions for %s:\n", (int) byNames.size(), synth.name.toRawUTF8());
            for (auto& sg : byNames)
                std::printf ("       %-12s -> %-28s (%s)\n", sg.mapping.sourceDescription().toRawUTF8(), sg.mapping.paramName.toRawUTF8(), sg.reason.toRawUTF8());

            auto find = [&] (int cc) -> const MappingSuggestion* { for (auto& sg : byNames) if (sg.mapping.number == cc) return &sg; return nullptr; };
            CHECK (find (74) != nullptr && find (74)->mapping.paramName.containsIgnoreCase ("cutoff"));
            CHECK (find (71) != nullptr && find (71)->mapping.paramName.containsIgnoreCase ("reso"));
            CHECK (find (73) != nullptr && find (73)->mapping.paramName.containsIgnoreCase ("attack"));
            CHECK (find (72) != nullptr && find (72)->mapping.paramName.containsIgnoreCase ("release"));
            CHECK (find (5)  != nullptr && find (5)->mapping.paramName.containsIgnoreCase ("portamento"));
            CHECK (find (1) == nullptr);                          // mod wheel is left to the plugin

            // No CC and no parameter is used twice.
            std::set<int> ccs; std::set<String> ids;
            for (auto& sg : byNames) { ccs.insert (sg.mapping.number); ids.insert (sg.mapping.paramId); }
            CHECK (ccs.size() == byNames.size() && ids.size() == byNames.size());

            // Full pipeline without a template: same set, targeted at slot 0, skipping existing ones.
            auto& templates = host.getMappingTemplates();
            CHECK (! templates.has (synth));
            std::vector<MappingDef> existing;
            existing.push_back (find (74)->mapping); existing.back().slot = 0; existing.back().effect = -1;
            auto pipeline = suggestMappings (mono->getParameters(), synth, templates, 0, -1, existing);
            CHECK (pipeline.size() == byNames.size() - 1);
            for (auto& sg : pipeline) { CHECK (sg.mapping.slot == 0 && sg.mapping.effect == -1); CHECK (sg.mapping.number != 74); }

            // Template: save two mappings, one of them for a parameter that doesn't exist.
            std::vector<MappingDef> tmpl;
            MappingDef a; a.number = 20; a.paramId = find (74)->mapping.paramId; a.minValue = 0.1f; a.maxValue = 0.9f;
            MappingDef b; b.number = 21; b.paramId = "sym:does_not_exist";
            tmpl.push_back (a); tmpl.push_back (b);
            templates.set (synth, tmpl);
            CHECK (templates.has (synth));
            auto fromTemplate = suggestMappings (mono->getParameters(), synth, templates, 2, 1, {});
            CHECK (fromTemplate.size() == 1);
            if (fromTemplate.size() == 1)
            {
                CHECK (fromTemplate[0].mapping.number == 20 && fromTemplate[0].mapping.slot == 2 && fromTemplate[0].mapping.effect == 1);
                CHECK (std::abs (fromTemplate[0].mapping.minValue - 0.1f) < 1e-6f);
                CHECK (fromTemplate[0].mapping.paramName.containsIgnoreCase ("cutoff"));
            }
            // Persisted: a fresh instance reads the same file.
            MappingTemplates reloaded (settings.getFile().getSiblingFile ("mapping-templates.json"));
            CHECK (reloaded.has (synth) && reloaded.get (synth).size() == 2);
            templates.remove (synth);
            CHECK (! templates.has (synth));
        }
        engine.clearProgram (0, 30);
        settle();
    }

    // --- helper environment -----------------------------------------------------------------
    {
        std::vector<char*> envp;
        auto env = RemotePlugin::buildHelperEnvironment (envp);
        CHECK (envp.size() == env.size() + 1 && envp.back() == nullptr);
        String path;
        int pathEntries = 0;
        for (auto& e : env) if (String (e).startsWith ("PATH=")) { path = String (e).substring (5); ++pathEntries; }
        CHECK (pathEntries == 1);
        const auto localBin = File::getSpecialLocation (File::userHomeDirectory).getChildFile (".local/bin");
        if (localBin.getChildFile ("wine").existsAsFile())
        {
            std::printf ("     ~/.local/bin/wine shim present: PATH starts with %s\n", StringArray::fromTokens (path, ":", {})[0].toRawUTF8());
            CHECK (StringArray::fromTokens (path, ":", {})[0] == localBin.getFullPathName());
        }
        for (auto& e : env) if (String (e).startsWith ("WINELOADER=")) std::printf ("     %s\n", e.c_str());
    }

    // --- scanner robustness -----------------------------------------------------------------
    {
        using HR = PluginHost::HelperResult;
        // A helper that never finishes must be killed at the timeout, not waited for.
        String out;
        auto t0 = Time::getMillisecondCounterHiRes();
        const auto r1 = PluginHost::runHelperWithTimeout (File ("/bin/sleep"), StringArray { "30" }, 600, 100, [] { return false; }, out);
        const auto ms1 = Time::getMillisecondCounterHiRes() - t0;
        std::printf ("     hung helper: result %d after %.0f ms\n", (int) r1, ms1);
        CHECK (r1 == HR::timedOut && ms1 < 2000.0);

        // A stop request (dialog closing) gives the child a grace period, then kills it.
        t0 = Time::getMillisecondCounterHiRes();
        const auto r2 = PluginHost::runHelperWithTimeout (File ("/bin/sleep"), StringArray { "30" }, 60000, 300,
                                                          [t0] { return Time::getMillisecondCounterHiRes() - t0 > 250.0; }, out);
        const auto ms2 = Time::getMillisecondCounterHiRes() - t0;
        std::printf ("     stopped helper: result %d after %.0f ms\n", (int) r2, ms2);
        CHECK (r2 == HR::stopped && ms2 > 500.0 && ms2 < 2000.0);

        // A stop request while the child is about to finish still yields its output.
        t0 = Time::getMillisecondCounterHiRes();
        const auto r4 = PluginHost::runHelperWithTimeout (File ("/bin/sh"), StringArray { "-c", "sleep 0.5; echo late-result" }, 60000, 5000,
                                                          [] { return true; }, out);
        CHECK (r4 == HR::finished && out.trim() == "late-result");

        // Output is collected completely from a process that finishes.
        const auto r3 = PluginHost::runHelperWithTimeout (File ("/bin/echo"), StringArray { "hello", "scanner" }, 5000, 100, [] { return false; }, out);
        CHECK (r3 == HR::finished && out.trim() == "hello scanner");

        // A hard stop (application exiting) ignores the grace period.
        t0 = Time::getMillisecondCounterHiRes();
        const auto r5 = PluginHost::runHelperWithTimeout (File ("/bin/sleep"), StringArray { "30" }, 60000, 45000, [] { return false; }, out,
                                                          [t0] { return Time::getMillisecondCounterHiRes() - t0 > 200.0; });
        const auto ms5 = Time::getMillisecondCounterHiRes() - t0;
        std::printf ("     hard-stopped helper: result %d after %.0f ms\n", (int) r5, ms5);
        CHECK (r5 == HR::stopped && ms5 < 1500.0);

        // Nothing left behind.
        ChildProcess pgrep;
        pgrep.start (StringArray { "pgrep", "-P", String ((int) ::getpid()), "sleep" });
        CHECK (pgrep.readAllProcessOutput().trim().isEmpty());

        // Real bridged VST3s, if this machine has them (yabridge + Wine): replay exactly
        // what PluginListComponent does -- four pool threads, and as soon as one runs
        // out of files the pool is torn down with removeAllJobs (true, ...), which asks
        // the still-running probes to exit. Nothing may end up blacklisted.
        // Any yabridge bundles this machine has; skipped where there are none.
        File yabridgeDir (File::getSpecialLocation (File::userHomeDirectory).getChildFile (".vst3/yabridge"));
        if (yabridgeDir.isDirectory())
        {
            AudioPluginFormat* vst3 = nullptr;
            for (auto* fmt : host.getFormatManager().getFormats()) if (fmt->getName() == "VST3") vst3 = fmt;

            KnownPluginList list;
            list.setCustomScanner (std::make_unique<PluginHost::OutOfProcessScanner>());
            TemporaryFile dmp (".txt");
            PluginDirectoryScanner ds (list, *vst3, FileSearchPath (yabridgeDir.getFullPathName()), true, dmp.getFile(), false);
            /* Bundles whose Windows plugin is currently missing (being reinstalled)
               can't load. Counted recursively, because the scanner searches that
               way: yabridge puts a vendor folder beside the bundles when a plugin
               installs into one (Steinberg/HALion Sonic.vst3), and counting only
               the top level made the scanner look like it had found one too many. */
            int expected = 0;
            for (const auto& bundle : yabridgeDir.findChildFiles (File::findFilesAndDirectories, true, "*.vst3"))
            {
                /* A VST3 bundle is a directory that itself ends in .vst3 and
                   contains Contents/, so a recursive search also turns up the
                   Windows DLL inside each one. Those are not plugins to scan. */
                if (bundle.getParentDirectory().getFileName() != "yabridge"
                    && bundle.getParentDirectory().getParentDirectory().getFileName() != "yabridge")
                    continue;

                if (PluginHost::brokenBridgeTarget (bundle.getFullPathName()).isEmpty()) ++expected;
                else std::printf ("     (skipping %s: bridge target missing)\n", bundle.getFileName().toRawUTF8());
            }

            struct Job : public ThreadPoolJob
            {
                Job (PluginDirectoryScanner& s, std::atomic<bool>& f) : ThreadPoolJob ("scan"), ds (s), finished (f) {}
                JobStatus runJob() override
                {
                    String name;
                    while (! finished.load() && ds.scanNextFile (true, name) && ! shouldExit()) {}
                    finished.store (true);
                    return jobHasFinished;
                }
                PluginDirectoryScanner& ds; std::atomic<bool>& finished;
            };
            std::atomic<bool> finished { false };
            t0 = Time::getMillisecondCounterHiRes();
            {
                ThreadPool pool (ThreadPoolOptions{}.withNumberOfThreads (4));
                for (int i = 0; i < 4; ++i) pool.addJob (new Job (ds, finished), true);
                while (! finished.load()) Thread::sleep (20);
                pool.removeAllJobs (true, 60000);        // what ~Scanner does
            }
            std::printf ("     dialog-style scan of %s: %d plugin(s) expected, %d found, %d blacklisted, %d failed, %.1f s\n",
                         yabridgeDir.getFullPathName().toRawUTF8(), expected, list.getNumTypes(),
                         list.getBlacklistedFiles().size(), ds.getFailedFiles().size(), (Time::getMillisecondCounterHiRes() - t0) / 1000.0);
            for (auto& b : list.getBlacklistedFiles()) std::printf ("       BLACKLISTED %s\n", b.toRawUTF8());
            // A probe stopped by the teardown may legitimately yield nothing (it will be
            // scanned next time); what must never happen is a blacklist entry.
            CHECK (list.getBlacklistedFiles().isEmpty());
            /* One bundle can hold more than one plugin -- a VST3 "shell" exposes
               several, and at least one here does -- so the count found may
               exceed the number of bundles. What matters is that nothing went
               missing: every bundle produced something, or is accounted for as
               a failure. */
            CHECK (list.getNumTypes() + ds.getFailedFiles().size() >= expected);
            CHECK (list.getNumTypes() >= 1);

            // The app-owned background scanner (what the Plugins window uses).
            auto& sc = host.getScanner();
            CHECK (! sc.isScanning());
            CHECK (sc.startScan (*vst3, FileSearchPath (yabridgeDir.getFullPathName())));
            CHECK (sc.isScanning());
            CHECK (! sc.startScan (*vst3, FileSearchPath (yabridgeDir.getFullPathName())));   // busy
            t0 = Time::getMillisecondCounterHiRes();
            double lastReport = 0.0;
            /* Long enough for a real library. Each bridged plugin is a Wine
               subprocess that can take a second or more, so a hundred of them
               is minutes, not seconds -- and a timeout here reads as a scanner
               that stopped working rather than one that was still going. */
            const double scanBudgetMs = jmax (120000.0, 3000.0 * (double) expected);
            while (sc.isScanning() && Time::getMillisecondCounterHiRes() - t0 < scanBudgetMs)
            {
                pump (50);
                const auto el = Time::getMillisecondCounterHiRes() - t0;
                if (el - lastReport > 5000.0)
                {
                    lastReport = el;
                    std::printf ("       ... %.0f s: progress %.2f, current '%s'\n", el / 1000.0, sc.getProgress(), sc.getCurrentFile().toRawUTF8());
                }
            }
            int fromYabridge = 0;
            for (auto& d : host.getKnownPlugins().getTypes()) if (d.fileOrIdentifier.startsWith (yabridgeDir.getFullPathName())) ++fromYabridge;
            std::printf ("     background scanner: %d/%d yabridge plugins, %d newly blacklisted, %d failed, %.1f s\n",
                         fromYabridge, expected, sc.getNewlyBlacklistedFiles().size(), sc.getFailedFiles().size(), (Time::getMillisecondCounterHiRes() - t0) / 1000.0);
            CHECK (! sc.isScanning());
            CHECK (sc.getNewlyBlacklistedFiles().isEmpty());
            CHECK (fromYabridge >= expected);      // shells can exceed one per bundle
        }
    }

    // --- crash isolation --------------------------------------------------------------------
    {
        engine.setPreloadAllPrograms (false);
        engine.selectProgram (0, 40);
        settle();
        CHECK (engine.addSlot (0, 40, synth, err));
        settle();
        auto* victim = engine.getPlugin (0, 40, 0);
        CHECK (victim != nullptr && victim->isAlive());
        if (victim != nullptr && victim->isAlive())
        {
            /* No host may hold another host's control socket. The pair is made
               on one loader thread while others are spawning; a copy that
               leaked into a sibling would keep the socket from ever reaching
               end-of-file when its owner dies. */
            {
                ChildProcess pgrep;
                pgrep.start (StringArray { "pgrep", "-P", String ((int) ::getpid()), "-f", "plugin-host" });
                auto hosts = StringArray::fromLines (pgrep.readAllProcessOutput().trim());
                auto socketInodes = [] (const String& pidText)
                {
                    std::set<String> out;
                    for (auto& f : File ("/proc/" + pidText.trim() + "/fd").findChildFiles (File::findFiles, false))
                    {
                        char buf[64] = {};
                        const auto n = ::readlink (f.getFullPathName().toRawUTF8(), buf, sizeof (buf) - 1);
                        if (n > 0 && String (buf).startsWith ("socket:")) out.insert (String (buf));
                    }
                    return out;
                };
                int shared = 0;
                for (auto& a : hosts)
                {
                    char buf[64] = {};
                    const auto n = ::readlink (("/proc/" + a.trim() + "/fd/3").toRawUTF8(), buf, sizeof (buf) - 1);
                    if (n <= 0) continue;
                    const String control (buf);
                    for (auto& b : hosts)
                        if (b != a && socketInodes (b).count (control) > 0) ++shared;
                }
                std::printf ("     %d plugin hosts; control sockets leaked into siblings: %d\n", hosts.size(), shared);
                CHECK (hosts.size() >= 2);
                CHECK (shared == 0);
            }

            /* A host that stops answering is put down after the state timeout,
               not left to cost every later request the same wait. Freeze it:
               alive, silent, like a bridged plugin whose Wine half has died. */
            {
                const auto frozen = victim->getHostPid();
                CHECK (frozen > 0);
                ::kill (frozen, SIGSTOP);
                MemoryBlock mb;
                const auto t0 = Time::getMillisecondCounterHiRes();
                const bool got = victim->getState (mb, 1500);
                const auto ms = Time::getMillisecondCounterHiRes() - t0;
                std::printf ("     frozen host: getState returned %s after %.0f ms; %s\n", got ? "state" : "failure", ms,
                             victim->getLastError().toRawUTF8());
                CHECK (! got);
                CHECK (ms >= 1400 && ms < 4000);
                CHECK (! victim->isAlive());
                CHECK (victim->getHostPid() == -1);
                CHECK (::kill (frozen, 0) != 0 && errno == ESRCH);     // killed and reaped, not a zombie
                pump (300);
                CHECK (! engine.isPluginAlive (0, 40, 0));
                engine.reloadPlugin (0, 40, 0);
                settle();
                victim = engine.getPlugin (0, 40, 0);
                CHECK (victim != nullptr && victim->isAlive());
            }

            /* A host that dies while something else still holds its end of the
               control socket -- no end-of-file will ever arrive -- is still
               noticed within a moment, from the process table. */
           #if HAVE_PIDFD
            if (victim != nullptr && victim->isAlive())
            {
                const auto doomed = victim->getHostPid();
                const int pidfd = (int) ::syscall (SYS_pidfd_open, doomed, 0u);
                const int held  = pidfd >= 0 ? (int) ::syscall (SYS_pidfd_getfd, pidfd, 3, 0u) : -1;
                if (held < 0)
                {
                    std::printf ("     skip held-socket check (pidfd_getfd: %s)\n", strerror (errno));
                }
                else
                {
                    ::kill (doomed, SIGKILL);
                    MemoryBlock mb;
                    const auto t0 = Time::getMillisecondCounterHiRes();
                    const bool got = victim->getState (mb, 10000);
                    const auto ms = Time::getMillisecondCounterHiRes() - t0;
                    std::printf ("     killed host with its socket held open: getState returned %s after %.0f ms; %s\n",
                                 got ? "state" : "failure", ms, victim->getLastError().toRawUTF8());
                    CHECK (! got);
                    CHECK (ms < 3000);
                    CHECK (! victim->isAlive());
                    CHECK (victim->getHostPid() == -1);
                    ::close (held);
                    pump (300);
                    engine.reloadPlugin (0, 40, 0);
                    settle();
                    victim = engine.getPlugin (0, 40, 0);
                    CHECK (victim != nullptr && victim->isAlive());
                }
                if (pidfd >= 0) ::close (pidfd);
            }
           #endif
        }
        if (victim != nullptr && victim->isAlive())
        {
            engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
            CHECK (render (engine, 10) > 0.01f);

            // Find and kill our plugin host children the hard way, like a plugin crash would.
            ChildProcess pgrep;
            pgrep.start (StringArray { "pgrep", "-P", String ((int) ::getpid()), "-f", "plugin-host" });
            auto pids = StringArray::fromLines (pgrep.readAllProcessOutput().trim());
            std::printf ("     %d plugin host processes running; killing them all\n", pids.size());
            CHECK (pids.size() >= 1);
            for (auto& pidText : pids)
                if (pidText.trim().isNotEmpty()) ::kill ((pid_t) pidText.getIntValue(), SIGKILL);

            pump (300);
            CHECK (! victim->isAlive());
            CHECK (! engine.isPluginAlive (0, 40, 0));
            CHECK (engine.getPluginLoadError (0, 40, 0).isNotEmpty());
            std::printf ("     after crash: %s\n", engine.getPluginLoadError (0, 40, 0).toRawUTF8());

            // The engine keeps rendering (silence from the dead slot) instead of hanging.
            CHECK (render (engine, 5) < 1e-4f);

            // Reload brings a fresh process and sound comes back.
            engine.reloadPlugin (0, 40, 0);
            settle();
            CHECK (engine.isPluginAlive (0, 40, 0));
            engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
            const float back = render (engine, 20);
            std::printf ("     rms after reload of crashed plugin: %f\n", back);
            CHECK (back > 0.01f);
            engine.injectMidi (0, MidiMessage::noteOff (1, 60));
            render (engine, 10);
        }
        engine.clearProgram (0, 40);
        settle();
    }

    // --- optional: write a demo setup for eyeballing the UI ----------------------------------
    if (auto outPath = SystemStats::getEnvironmentVariable ("STAGERIG_TEST_WRITE_SETUP", {}); outPath.isNotEmpty())
    {
        PluginDescription fx; bool haveFx = false;
        for (auto& d : host.getKnownPlugins().getTypes())
            if (! d.isInstrument && (d.name.containsIgnoreCase ("Stereo Tools") || d.name.containsIgnoreCase ("Reverb"))) { fx = d; haveFx = true; }
        Setup demo = Setup::makeDefault();
        auto& prog = demo.inputs[0].programs[0];
        prog.name = "Demo Lead";
        SlotDef sd; sd.plugin = synth; sd.gainDb = -3.0f; sd.lowKey = 48;
        if (haveFx) { EffectDef e; e.plugin = fx; sd.effects.push_back (e); e.bypassed = true; sd.effects.push_back (e); }
        prog.slots.push_back (sd);
        SlotDef pad = sd; pad.effects.clear(); pad.transpose = -12; pad.highKey = 47; pad.lowKey = 0;
        prog.slots.push_back (pad);
        if (haveFx) { EffectDef e; e.plugin = fx; prog.effects.push_back (e); }
        MappingDef mm; mm.number = 74; mm.slot = 0; mm.effect = -1; mm.paramId = "sym:cutoff"; mm.paramName = "Cutoff";
        prog.mappings.push_back (mm);
        if (haveFx) { MappingDef fm; fm.number = 7; fm.slot = -1; fm.effect = 0; fm.paramId = "sym:level_in"; fm.paramName = "Input Gain"; prog.mappings.push_back (fm); }
        demo.inputs[0].programs[3].name = "Organ split";
        CHECK (demo.saveToFile (File (outPath)).wasOk());
        std::printf ("     demo setup written to %s\n", outPath.toRawUTF8());
    }

    // --- optional: an arbitrary VST3 (e.g. a yabridge-bridged plugin) --------------------
    // STAGERIG_TEST_VST3=/path/to/Plugin.vst3 loads it, plays a note and renders blocks;
    // this reproduces the multi-out / inactive-bus crash seen with Kontakt via yabridge.
    if (auto vst3Path = SystemStats::getEnvironmentVariable ("STAGERIG_TEST_VST3", {}); vst3Path.isNotEmpty())
    {
        // A tempo no default would produce, so the helper's report proves this
        // value reached the plugin rather than 120 being a coincidence.
        engine.setTempoBpm (137.0);

        AudioPluginFormat* vst3 = nullptr;
        for (auto* f : host.getFormatManager().getFormats())
            if (f->getName() == "VST3") vst3 = f;
        CHECK (vst3 != nullptr);
        if (vst3 != nullptr)
        {
            OwnedArray<PluginDescription> found;
            host.getKnownPlugins().scanAndAddFile (vst3Path, true, found, *vst3);
            CHECK (found.size() > 0);
            if (found.size() > 0)
            {
                const PluginDescription desc (*found[0]);
                std::printf ("     extra VST3: %s (%s)\n", desc.name.toRawUTF8(), desc.manufacturerName.toRawUTF8());
                engine.setPreloadAllPrograms (false);
                engine.selectProgram (0, 20);
                settle();
                CHECK (engine.addSlot (0, 20, desc, err));
                settle();
                if (err.isNotEmpty()) std::printf ("     addSlot error: %s\n", err.toRawUTF8());
                if (auto* x = engine.getPlugin (0, 20, 0); x != nullptr && x->isAlive())
                {
                    std::printf ("     loaded in its own process: %s, %d parameters\n", x->getName().toRawUTF8(), (int) x->getParameters().size());
                    {
                        // VST3 MIDI controller proxies: the helper asks IMidiMapping which copy is which channel.
                        int proxies = 0, vol1 = -1, vol2 = -1;
                        for (auto& p : x->getParameters())
                        {
                            if (p.midiChannel > 0) ++proxies;
                            if (p.midiController == 7 && p.midiChannel == 1) vol1 = p.index;
                            if (p.midiController == 7 && p.midiChannel == 2) vol2 = p.index;
                        }
                        std::printf ("     %d MIDI controller proxies; CC7 ch1 -> #%d, CC7 ch2 -> #%d\n", proxies, vol1, vol2);
                        if (vol1 >= 0 && vol2 >= 0)
                        {
                            // Send CC 7 on channel 1 (the Upper input's channel): the ch1 copy must move, the ch2 copy must not.
                            float a1 = 0, a2 = 0, b1 = 0, b2 = 0;
                            CHECK (x->fetchParameterValue (vol1, a1) && x->fetchParameterValue (vol2, a2));
                            const float target = a1 > 0.5f ? 0.1f : 0.9f;
                            engine.injectMidi (0, MidiMessage::controllerEvent (1, 7, (int) std::lround (target * 127.0f)));
                            render (engine, 5);
                            CHECK (x->fetchParameterValue (vol1, b1) && x->fetchParameterValue (vol2, b2));
                            std::printf ("     CC7 ch1 -> %d: ch1 copy %.3f -> %.3f, ch2 copy %.3f -> %.3f\n", (int) std::lround (target * 127.0f), a1, b1, a2, b2);
                            CHECK (std::abs (b1 - target) < 0.02f);
                            CHECK (std::abs (b2 - a2) < 1e-3f);
                        }
                    }
                    // STAGERIG_TEST_TWO_INPUTS=1 loads the same plugin into a second input as
                    // well: two instances of one bridged plugin, which is what a two-manual rig does.
                    if (SystemStats::getEnvironmentVariable ("STAGERIG_TEST_TWO_INPUTS", "0").getIntValue() > 0)
                    {
                        std::printf ("     loading a SECOND instance of %s on input 1\n", desc.name.toRawUTF8()); std::fflush (stdout);
                        const auto t0b = Time::getMillisecondCounterHiRes();
                        engine.selectProgram (1, 20);
                        settle();
                        String err2;
                        const bool added = engine.addSlot (1, 20, desc, err2);
                        settle();
                        std::printf ("     second instance: added=%d err='%s' after %.0f ms\n",
                                     (int) added, err2.toRawUTF8(), Time::getMillisecondCounterHiRes() - t0b);
                        auto* second = engine.getPlugin (1, 20, 0);
                        std::printf ("     second alive=%d  first alive=%d\n",
                                     (int) (second != nullptr && second->isAlive()),
                                     (int) (x != nullptr && x->isAlive()));
                        CHECK (added && second != nullptr && second->isAlive());
                        CHECK (x != nullptr && x->isAlive());      // the first must survive the second
                        engine.injectMidi (1, MidiMessage::noteOn (2, 60, (uint8) 100));
                        std::printf ("     rms with both loaded: %f\n", render (engine, 60));
                        engine.injectMidi (1, MidiMessage::noteOff (2, 60));
                    }

                    // STAGERIG_TEST_EDITOR_SECONDS=N keeps the plugin's editor open for N seconds so a
                    // script can drive and screenshot it (GUI/mouse investigations).
                    if (const int hold = SystemStats::getEnvironmentVariable ("STAGERIG_TEST_EDITOR_SECONDS", "0").getIntValue(); hold > 0)
                    {
                        std::printf ("     opening editor 'PERFTEST %s' for %d s\n", desc.name.toRawUTF8(), hold); std::fflush (stdout);
                        CHECK (x->showEditor ("PERFTEST " + desc.name));
                        const auto tEd = Time::getMillisecondCounterHiRes();
                        while (Time::getMillisecondCounterHiRes() - tEd < hold * 1000.0) { pump (20); render (engine, 1); }
                        x->hideEditor();
                        pump (200);
                    }
                    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
                    const float r = render (engine, 100);                      // ~1.2 s; would segfault before the fix
                    std::printf ("     rms with extra VST3 (may be 0 if no preset loaded): %f\n", r);
                    engine.injectMidi (0, MidiMessage::noteOff (1, 60));
                    render (engine, 20);
                    CHECK (true);   // survived processing
                }
            }
        }
    }

    // --- parallel loading: several plugins load at once ---------------------------------
    {
        std::printf ("     loader pool: %d threads\n", engine.getParallelLoads());
        CHECK (engine.getParallelLoads() >= 2);
        engine.selectProgram (0, 60);
        settle();
        int maxInFlight = 0;
        for (int k = 0; k < 6; ++k) { String e2; engine.addSlot (0, 60, synth, e2); }
        const auto t0p = Time::getMillisecondCounterHiRes();
        while (engine.hasPendingLoads() && Time::getMillisecondCounterHiRes() - t0p < 60000.0)
        {
            maxInFlight = jmax (maxInFlight, engine.getLoadsInFlight());
            pump (2);
        }
        std::printf ("     6 loads: max %d in flight, %.0f ms total\n", maxInFlight, Time::getMillisecondCounterHiRes() - t0p);
        CHECK (maxInFlight >= 2);
        int alive = 0;
        for (int k = 0; k < 6; ++k) if (engine.isPluginAlive (0, 60, k)) ++alive;
        CHECK (alive == 6);
        CHECK (render (engine, 3) < 1e-4f);     // silent until played
    }

    // --- a CLAP synth that takes CLAP note events: PANIC silences it (Surge XT, when installed) ---
    {
        File surgeFile;
        for (const char* p : { "/usr/lib/clap/Surge XT.clap", "/usr/local/lib/clap/Surge XT.clap" })
            if (File (p).exists()) { surgeFile = File (p); break; }
        if (surgeFile == File()) surgeFile = File::getSpecialLocation (File::userHomeDirectory).getChildFile (".clap/Surge XT.clap");
        if (! surgeFile.exists())
            std::printf ("     skip Surge XT PANIC check (not installed)\n");
        else
        {
            perf::ClapPluginFormat clapFmt;
            OwnedArray<PluginDescription> found;
            clapFmt.findAllTypesForFile (found, surgeFile.getFullPathName());
            const PluginDescription* surge = nullptr;
            for (auto* d : found) if (d->isInstrument && d->name == "Surge XT") surge = d;
            CHECK (surge != nullptr);
            if (surge != nullptr)
            {
                engine.clearProgram (0, 61);
                engine.selectProgram (0, 61); settle();
                String sErr;
                CHECK (engine.addSlot (0, 61, *surge, sErr));
                settle();
                CHECK (engine.isPluginAlive (0, 61, 0));
                render (engine, 10);
                engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
                engine.injectMidi (0, MidiMessage::noteOn (1, 64, (uint8) 100));
                const float sounding = render (engine, 20);
                std::printf ("     Surge XT playing two notes: %.4f\n", sounding);
                CHECK (sounding > 1e-3f);
                engine.panic();                                          // the keys are still held
                render (engine, 10);
                const float afterPanic = render (engine, 10);
                std::printf ("     after PANIC with the keys held: %.6f\n", afterPanic);
                CHECK (afterPanic < 1e-4f);
                engine.injectMidi (0, MidiMessage::noteOff (1, 60));
                engine.injectMidi (0, MidiMessage::noteOff (1, 64));
                render (engine, 5);
                engine.clearProgram (0, 61);
            }
        }
    }

    // --- plugin icons: generated badges always, embedded Windows icons when present ------
    {
        PluginDescription d; d.name = "Kontakt 8"; d.manufacturerName = "Native Instruments"; d.pluginFormatName = "VST3";
        CHECK (PluginIcons::initials ("Kontakt 8") == "K8");
        CHECK (PluginIcons::initials ("Calf Monosynth") == "CM");
        CHECK (PluginIcons::initials ("Dirt") == "D");
        CHECK (PluginIcons::badgeColour ("Native Instruments") == PluginIcons::badgeColour ("native instruments "));
        auto b = PluginIcons::badge (d, 48);
        CHECK (b.isValid() && b.getWidth() == 48 && b.getPixelAt (24, 24).getAlpha() > 200);
        TemporaryFile iconCache;
        PluginIcons icons (iconCache.getFile());
        CHECK (icons.get (d, 32).getWidth() == 32);
        d.fileOrIdentifier = File::getSpecialLocation (File::userHomeDirectory).getChildFile (".vst3/yabridge/Kontakt 8.vst3").getFullPathName();
        if (File (d.fileOrIdentifier).isDirectory())
        {
            const auto pe = PluginIcons::windowsBinaryFor (d);
            std::printf ("     Kontakt windows binary: %s\n", pe.getFullPathName().toRawUTF8());
            CHECK (pe.existsAsFile());
            const auto t0i = Time::getMillisecondCounterHiRes();
            const auto img = PluginIcons::fromWindowsBinary (pe);
            std::printf ("     embedded icon: %dx%d in %.1f ms\n", img.getWidth(), img.getHeight(), Time::getMillisecondCounterHiRes() - t0i);
            CHECK (img.isValid() && img.getWidth() >= 128);
            CHECK (icons.native (d).isValid());
            CHECK (iconCache.getFile().getNumberOfChildFiles (File::findFiles, "*.png") == 1);   // cached
        }
        PluginDescription none; none.name = "No Icon"; none.manufacturerName = "X"; none.fileOrIdentifier = "/nonexistent";
        CHECK (! icons.native (none).isValid() && icons.get (none, 24).isValid());
    }

    engine.removeListener (&listener);
    // ---- tempo and tap tempo ---------------------------------------------------
    /* Plugins were handed no playhead at all, so a tempo-synced delay had
       nothing to sync to. One tempo for the whole rig, and a tap that settles
       fast enough to be useful between songs. */
    {
        engine.setTempoBpm (120.0);
        CHECK (std::abs (engine.getTempoBpm() - 120.0) < 0.001);

        engine.setTempoBpm (10.0);
        CHECK (engine.getTempoBpm() >= 20.0);          // clamped, not accepted
        engine.setTempoBpm (10000.0);
        CHECK (engine.getTempoBpm() <= 300.0);

        engine.setTempoBpm (120.0);
        engine.resetTapTempo();
        CHECK (engine.tapTempo() == 0.0);              // one tap implies nothing

        // Four taps at 500ms is 120 bpm. Real taps are not exact, so allow a
        // little slop -- and this is the interval a player would actually use.
        engine.resetTapTempo();
        engine.tapTempo();
        double tapped = 0.0;
        for (int i = 0; i < 3; ++i) { Thread::sleep (500); tapped = engine.tapTempo(); }
        std::printf ("     four taps at 500ms -> %.1f bpm\n", tapped);
        CHECK (tapped > 100.0 && tapped < 140.0);

        // A long pause starts a fresh count rather than averaging across it.
        engine.resetTapTempo();
        engine.tapTempo();
        Thread::sleep (2100);
        CHECK (engine.tapTempo() == 0.0);              // treated as a first tap

        // The tap controller is remembered and clamped.
        engine.setTapTempoCC (64);
        CHECK (engine.getTapTempoCC() == 64);
        engine.setTapTempoCC (999);
        CHECK (engine.getTapTempoCC() == 127);
        engine.setTapTempoCC (0);                      // none

        // Tempo and the tap controller survive a save and load.
        engine.setTempoBpm (143.0);
        engine.setTapTempoCC (80);
        const auto saved = Setup::fromVar (engine.captureSetup().toVar());
        CHECK (std::abs (saved.tempoBpm - 143.0) < 0.001);
        CHECK (saved.tapTempoCC == 80);

        /* The footswitch path: a CC arriving on any input taps the tempo. This is
           the part that runs on the audio thread and posts to the message
           thread, so it exercises more than tapTempo() alone. */
        engine.setTempoBpm (120.0);
        engine.setTapTempoCC (80);
        engine.resetTapTempo();
        for (int i = 0; i < 4; ++i)
        {
            engine.injectMidi (0, MidiMessage::controllerEvent (1, 80, 127));
            MessageManager::getInstance()->runDispatchLoopUntil (20);
            if (i < 3) Thread::sleep (480);
        }
        MessageManager::getInstance()->runDispatchLoopUntil (50);
        std::printf ("     four CC 80 taps at ~500ms -> %.1f bpm\n", engine.getTempoBpm());
        CHECK (engine.getTempoBpm() > 100.0 && engine.getTempoBpm() < 140.0);

        // The release half of a footswitch press must not count as a tap.
        engine.setTempoBpm (120.0);
        engine.resetTapTempo();
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 80, 0));
        MessageManager::getInstance()->runDispatchLoopUntil (30);
        CHECK (std::abs (engine.getTempoBpm() - 120.0) < 0.001);

        // A different CC does nothing.
        engine.resetTapTempo();
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 81, 127));
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 81, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (30);
        CHECK (std::abs (engine.getTempoBpm() - 120.0) < 0.001);
        engine.setTapTempoCC (0);

        /* Learning the tap controller: press the pedal rather than look up what
           it sends. Must work outside the channel filter, and must be able to
           replace a controller that is already assigned. */
        engine.setTapTempoCC (0);
        engine.armTapTempoLearn (true);
        CHECK (engine.isTapTempoLearnArmed());
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 67, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (40);
        CHECK (engine.getTapTempoCC() == 67);
        CHECK (! engine.isTapTempoLearnArmed());       // disarms itself

        // The release half must not be what gets learned.
        engine.setTapTempoCC (0);
        engine.armTapTempoLearn (true);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 70, 0));
        MessageManager::getInstance()->runDispatchLoopUntil (40);
        CHECK (engine.getTapTempoCC() == 0);           // ignored, still waiting
        CHECK (engine.isTapTempoLearnArmed());
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 70, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (40);
        CHECK (engine.getTapTempoCC() == 70);

        // Reassigning while a controller is already assigned: learning wins,
        // rather than the old controller swallowing the press as a tap.
        engine.armTapTempoLearn (true);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 70, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (40);
        engine.armTapTempoLearn (true);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 64, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (40);
        CHECK (engine.getTapTempoCC() == 64);
        engine.setTapTempoCC (0);

        // A setup written before tempo existed loads at a sane default.
        auto older = engine.captureSetup().toVar();
        if (auto* o = older.getDynamicObject()) { o->removeProperty ("tempoBpm"); o->removeProperty ("tapTempoCC"); }
        const auto legacy = Setup::fromVar (older);
        CHECK (std::abs (legacy.tempoBpm - 120.0) < 0.001);
        CHECK (legacy.tapTempoCC == 0);
    }

    // ---- songs and sets ---------------------------------------------------
    /* A song sets every keyboard at once and brings its own tempo; a set is the
       order they are played in. The footswitch path matters most: on stage that
       is how the set is driven, and it runs through the MIDI thread. */
    {
        std::printf ("  songs and sets\n");

        while ((int) engine.getSetup().inputs.size() < 2) engine.addInput ("Lower");
        CHECK (engine.getSetup().inputs.size() >= 2);

        engine.setProgramName (0, 4, "Oye Como");
        engine.setProgramName (1, 1, "FM EP");
        engine.setProgramName (0, 2, "B3");

        // A song captured from what is loaded right now.
        engine.selectProgram (0, 4);
        engine.selectProgram (1, 1);
        engine.setTempoBpm (116.0);
        const int oye = engine.addSong ("Oye Como Va");
        engine.captureSong (oye);
        CHECK (engine.getSongs()[(size_t) oye].programFor (0) == 4);
        CHECK (engine.getSongs()[(size_t) oye].programFor (1) == 1);
        CHECK (std::abs (engine.getSongs()[(size_t) oye].tempoBpm - 116.0) < 0.001);

        // A second song, built by hand, that leaves Lower alone.
        const int onions = engine.addSong ("Green Onions");
        engine.setSongProgram (onions, 0, 2);
        engine.setSongProgram (onions, 1, -1);
        engine.setSongTempo (onions, 132.0);

        // Selecting a song moves every input it names, and only those.
        engine.selectProgram (0, 0);
        engine.selectProgram (1, 1);
        engine.setTempoBpm (90.0);
        engine.selectSong (onions);
        CHECK (engine.getSetup().inputs[0].currentProgram == 2);
        CHECK (engine.getSetup().inputs[1].currentProgram == 1);      // untouched
        CHECK (std::abs (engine.getTempoBpm() - 132.0) < 0.001);

        // A song with no tempo of its own leaves the tempo where it is.
        const int quiet = engine.addSong ("No tempo");
        engine.setSongProgram (quiet, 0, 4);
        engine.setTempoBpm (101.0);
        engine.selectSong (quiet);
        CHECK (std::abs (engine.getTempoBpm() - 101.0) < 0.001);

        // A set orders songs, and stepping through it stops at the end.
        const int first = engine.addSet ("First set");
        engine.setSetSongs (first, { oye, onions, quiet });
        engine.selectSet (first);
        CHECK (engine.getCurrentSet() == first);
        CHECK (engine.getCurrentSongInSet() == -1);      // chosen, not started

        engine.nextSong();
        CHECK (engine.getCurrentSongInSet() == 0);
        CHECK (engine.getSetup().inputs[0].currentProgram == 4);
        engine.nextSong();
        CHECK (engine.getCurrentSongInSet() == 1);
        engine.nextSong();
        CHECK (engine.getCurrentSongInSet() == 2);
        engine.nextSong();                                // past the end
        CHECK (engine.getCurrentSongInSet() == 2);        // stays put, does not wrap
        engine.previousSong();
        CHECK (engine.getCurrentSongInSet() == 1);
        while (engine.getCurrentSongInSet() > 0) engine.previousSong();
        engine.previousSong();
        CHECK (engine.getCurrentSongInSet() == 0);        // and does not run off the front

        /* The footswitch: a CC steps the set whatever is loaded. This goes
           through the MIDI thread and the event queue, like the real thing. */
        engine.setNextSongCC (80);
        engine.setPrevSongCC (81);
        engine.selectSongInSet (0);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 80, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (60);
        CHECK (engine.getCurrentSongInSet() == 1);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 81, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (60);
        CHECK (engine.getCurrentSongInSet() == 0);

        // The release half must not step again.
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 80, 0));
        MessageManager::getInstance()->runDispatchLoopUntil (40);
        CHECK (engine.getCurrentSongInSet() == 0);

        // Learning a footswitch, like the tap.
        engine.setNextSongCC (0);
        engine.armSongLearn (true);
        CHECK (engine.isSongLearnArmed());
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 67, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (60);
        CHECK (! engine.isSongLearnArmed());
        CHECK (engine.getNextSongCC() == 67);

        /* Deleting a song must not leave a set playing the wrong one: the
           indices after it shift, and any set using it drops it. */
        const int before = (int) engine.getSets()[(size_t) first].songs.size();
        engine.removeSong (onions);                       // the middle song
        const auto& songs = engine.getSets()[(size_t) first].songs;
        CHECK ((int) songs.size() == before - 1);
        for (int i : songs) CHECK (i >= 0 && i < (int) engine.getSongs().size());
        CHECK (engine.getSongs()[(size_t) songs[0]].name == "Oye Como Va");
        CHECK (engine.getSongs()[(size_t) songs[1]].name == "No tempo");

        /* Reordering the songs list must not change what any set plays: sets
           hold indices, so a swap has to rewrite them. This is the one that
           would quietly ruin a gig -- the set list would look the same and
           play something else. */
        {
            const int a = engine.addSong ("Alpha");
            const int b = engine.addSong ("Bravo");
            const int c = engine.addSong ("Charlie");
            const int st = engine.addSet ("Order test");
            engine.setSetSongs (st, { c, a, b });          // deliberately not in order

            auto namesInSet = [&]
            {
                StringArray out;
                for (int i : engine.getSets()[(size_t) st].songs)
                    out.add (engine.getSongs()[(size_t) i].name);
                return out.joinIntoString (",");
            };
            const auto before = namesInSet();
            CHECK (before == "Charlie,Alpha,Bravo");

            const int moved = engine.moveSong (b, -1);     // Bravo up one
            CHECK (moved == b - 1);
            CHECK (engine.getSongs()[(size_t) moved].name == "Bravo");
            CHECK (namesInSet() == before);                // the set plays the same order

            engine.moveSong (moved, 1);                    // and back
            CHECK (namesInSet() == before);

            // Moving off either end does nothing rather than corrupting anything.
            CHECK (engine.moveSong (0, -1) == 0);
            const int last = (int) engine.getSongs().size() - 1;
            CHECK (engine.moveSong (last, 1) == last);
            CHECK (namesInSet() == before);

            // Sets reorder too, and keep playing the one you were playing.
            const int other = engine.addSet ("Second");
            engine.selectSet (st);
            const int newPos = engine.moveSet (st, 1);
            CHECK (engine.getCurrentSet() == newPos);
            CHECK (engine.getSets()[(size_t) newPos].name == "Order test");
            CHECK (engine.getSets()[(size_t) st].name == "Second");
            engine.removeSet (jmax (newPos, st));
            engine.removeSet (jmin (newPos, st));
        }

        // Charts attach to a song and come off it again.
        {
            const int withChart = engine.addSong ("Charted");
            engine.addSongChart (withChart, { "charted.pdf", "Sheet" });
            engine.addSongChart (withChart, { "charted.cho", "" });
            CHECK (engine.getSongs()[(size_t) withChart].charts.size() == 2);
            engine.removeSongChart (withChart, 0);
            CHECK (engine.getSongs()[(size_t) withChart].charts.size() == 1);
            CHECK (engine.getSongs()[(size_t) withChart].charts[0].file == "charted.cho");
            engine.removeSongChart (withChart, 5);                    // out of range: ignored
            CHECK (engine.getSongs()[(size_t) withChart].charts.size() == 1);
            engine.removeSong (withChart);
        }

        // All of it survives a save and load.
        engine.setNextSongCC (80);
        engine.setPrevSongCC (81);
        const auto saved = Setup::fromVar (engine.captureSetup().toVar());
        CHECK (saved.songs.size() == engine.getSongs().size());
        CHECK (saved.sets.size() == engine.getSets().size());
        CHECK (saved.nextSongCC == 80 && saved.prevSongCC == 81);
        CHECK (saved.songs[0].name == "Oye Como Va");
        CHECK (std::abs (saved.songs[0].tempoBpm - 116.0) < 0.001);
    }

    // --- two instances of one plugin keep their own state -----------------------------
    /* Reported: with the same plugin in two programs, saving left both with the
       state of the one edited last. Walk the reporter's steps: load, set each
       differently, switch between them (which captures the one being left),
       save, reload, and ask each plugin what it has. Run for the LV2 synth and,
       when one is installed, for a native VST3 -- the reporter's format. */
    auto twoInstances = [&] (const PluginDescription& desc, const char* label)
    {
        std::printf ("     --- two instances of %s (%s) ---\n", desc.name.toRawUTF8(), label);
        engine.setPreloadAllPrograms (false);
        engine.clearProgram (0, 50); engine.clearProgram (0, 51);
        engine.selectProgram (0, 50); settle();
        CHECK (engine.addSlot (0, 50, desc, err)); settle();
        engine.selectProgram (0, 51); settle();
        CHECK (engine.addSlot (0, 51, desc, err)); settle();
        auto* a = engine.getPlugin (0, 50, 0);
        auto* b = engine.getPlugin (0, 51, 0);
        CHECK (a != nullptr && b != nullptr && a != b);
        if (a == nullptr || b == nullptr) return;

        int idx = -1;
        for (auto& p : a->getParameters())
            if (p.automatable && ! p.discrete && p.name.containsIgnoreCase ("cutoff")) { idx = p.index; break; }
        if (idx < 0)
            for (auto& p : a->getParameters())
                if (p.automatable && ! p.discrete) { idx = p.index; break; }
        CHECK (idx >= 0);
        if (idx < 0) return;
        std::printf ("     parameter %d '%s'\n", idx, a->getParameters()[(size_t) idx].name.toRawUTF8());

        /* Set each with its program sounding and a block rendered, as a hand on
           a knob would: an LV2 plugin's new value reaches it with the next block,
           and its saved state reads the plugin's ports, not the host's cache. */
        engine.selectProgram (0, 50); settle();
        CHECK (a->setParameterValue (idx, 0.2f)); render (engine, 2);
        engine.selectProgram (0, 51); settle();
        CHECK (b->setParameterValue (idx, 0.8f)); render (engine, 2);
        {
            float fa = -1, fb = -1; a->fetchParameterValue (idx, fa); b->fetchParameterValue (idx, fb);
            MemoryBlock ga, gb; a->getState (ga); b->getState (gb);
            std::printf ("     right after set: a=%.3f b=%.3f  direct getState a %d bytes, b %d bytes, %s\n",
                         fa, fb, (int) ga.getSize(), (int) gb.getSize(), ga == gb ? "IDENTICAL" : "different");
        }
        engine.selectProgram (0, 50); settle();        // leaving 51 captures it
        engine.selectProgram (0, 51); settle();        // leaving 50 captures it

        /* Captured once, a plugin is left alone by the autosave until
           something could have changed it: it is the asking that costs. */
        CHECK (! a->stateMayHaveChanged() && ! b->stateMayHaveChanged());
        CHECK (b->setParameterValue (idx, 0.75f));
        CHECK (b->stateMayHaveChanged() && ! a->stateMayHaveChanged());
        engine.captureSetup (-1, true);
        CHECK (! b->stateMayHaveChanged());
        CHECK (b->setParameterValue (idx, 0.8f));
        engine.selectProgram (0, 50); settle(); engine.selectProgram (0, 51); settle();

        TemporaryFile two (".stagerig.json");
        CHECK (engine.captureSetup().saveToFile (two.getFile()).wasOk());
        Setup rl;
        CHECK (Setup::loadFromFile (two.getFile(), rl).wasOk());
        const auto& sa = rl.inputs[0].programs[50].slots[0].state;
        const auto& sb = rl.inputs[0].programs[51].slots[0].state;
        std::printf ("     saved states: program 50 %d bytes, program 51 %d bytes, %s\n",
                     (int) sa.getSize(), (int) sb.getSize(), sa == sb ? "IDENTICAL" : "different");
        CHECK (sa.getSize() > 0 && sb.getSize() > 0 && sa != sb);

        engine.loadSetup (rl); settle();
        engine.selectProgram (0, 50); settle();
        engine.selectProgram (0, 51); settle();
        float va = -1.0f, vb = -1.0f;
        auto* ra = engine.getPlugin (0, 50, 0);
        auto* rb = engine.getPlugin (0, 51, 0);
        CHECK (ra != nullptr && rb != nullptr);
        if (ra == nullptr || rb == nullptr) return;
        CHECK (ra->fetchParameterValue (idx, va));
        CHECK (rb->fetchParameterValue (idx, vb));
        std::printf ("     after reload: program 50 -> %.3f (want 0.2), program 51 -> %.3f (want 0.8)\n", va, vb);
        CHECK (std::abs (va - 0.2f) < 0.05f);
        CHECK (std::abs (vb - 0.8f) < 0.05f);
    };
    twoInstances (synth, "LV2");
    {
        AudioPluginFormat* vst3 = nullptr;
        for (auto* f : host.getFormatManager().getFormats())
            if (f->getName() == "VST3") vst3 = f;
        PluginDescription nativeVst3; bool haveVst3 = false;
        if (vst3 != nullptr)
            for (const char* path : { "/usr/lib/vst3/Surge XT.vst3", "/usr/local/lib/vst3/Surge XT.vst3" })
                if (File (path).exists() && ! haveVst3)
                {
                    OwnedArray<PluginDescription> found;
                    host.getKnownPlugins().scanAndAddFile (path, true, found, *vst3);
                    for (auto* d : found)
                        if (d->isInstrument) { nativeVst3 = *d; haveVst3 = true; break; }
                }
        if (haveVst3) twoInstances (nativeVst3, "VST3");
        else std::printf ("     skip VST3 two-instance check (no native VST3 instrument found)\n");

        /* More of the ways two instances of one plugin come about, with the
           VST3: a layer (two slots in one program), copy-and-paste of a program,
           and the editor path -- a change made with the window open, captured
           when the window closes -- followed by a save with no program switch. */
        if (haveVst3)
        {
            const auto& desc = nativeVst3;
            auto param = [] (RemotePlugin* p)
            {
                for (auto& q : p->getParameters()) if (q.automatable && ! q.discrete && q.name.containsIgnoreCase ("cutoff")) return q.index;
                return 0;
            };
            auto saveReload = [&] (const char* label, std::function<void()> checks)
            {
                TemporaryFile f (".stagerig.json");
                CHECK (engine.captureSetup().saveToFile (f.getFile()).wasOk());
                Setup rl; CHECK (Setup::loadFromFile (f.getFile(), rl).wasOk());
                engine.loadSetup (rl); settle();
                std::printf ("     --- %s: saved and reloaded ---\n", label);
                checks();
            };

            // Layer: two slots in one program.
            engine.clearProgram (0, 60); engine.selectProgram (0, 60); settle();
            CHECK (engine.addSlot (0, 60, desc, err)); settle();
            CHECK (engine.addSlot (0, 60, desc, err)); settle();
            if (auto* s0 = engine.getPlugin (0, 60, 0)) if (auto* s1 = engine.getPlugin (0, 60, 1))
            {
                const int idx = param (s0);
                s0->setParameterValue (idx, 0.3f); s1->setParameterValue (idx, 0.7f);
                saveReload ("layer", [&]
                {
                    engine.selectProgram (0, 60); settle();
                    float v0 = -1, v1 = -1;
                    if (auto* r0 = engine.getPlugin (0, 60, 0)) r0->fetchParameterValue (idx, v0);
                    if (auto* r1 = engine.getPlugin (0, 60, 1)) r1->fetchParameterValue (idx, v1);
                    std::printf ("     layer: slot0 %.3f (want 0.3) slot1 %.3f (want 0.7)\n", v0, v1);
                    CHECK (std::abs (v0 - 0.3f) < 0.05f && std::abs (v1 - 0.7f) < 0.05f);
                });
            }

            // Copy and paste: program 61 from 60, then change the copy.
            engine.selectProgram (0, 60); settle();
            engine.copyProgram (0, 60, 61);
            engine.selectProgram (0, 61); settle();
            if (auto* c = engine.getPlugin (0, 61, 0))
            {
                const int idx = param (c);
                c->setParameterValue (idx, 0.9f);
                saveReload ("copy-paste", [&]
                {
                    engine.selectProgram (0, 60); settle(); engine.selectProgram (0, 61); settle();
                    float v60 = -1, v61 = -1;
                    if (auto* r = engine.getPlugin (0, 60, 0)) r->fetchParameterValue (idx, v60);
                    if (auto* r = engine.getPlugin (0, 61, 0)) r->fetchParameterValue (idx, v61);
                    std::printf ("     copy-paste: program 60 %.3f (want 0.3) program 61 %.3f (want 0.9)\n", v60, v61);
                    CHECK (std::abs (v60 - 0.3f) < 0.05f && std::abs (v61 - 0.9f) < 0.05f);
                });
            }

            // Editor path, no program switch before saving.
            engine.clearProgram (0, 62); engine.clearProgram (0, 63);
            engine.selectProgram (0, 62); settle(); CHECK (engine.addSlot (0, 62, desc, err)); settle();
            engine.selectProgram (0, 63); settle(); CHECK (engine.addSlot (0, 63, desc, err)); settle();
            auto* e62 = engine.getPlugin (0, 62, 0); auto* e63 = engine.getPlugin (0, 63, 0);
            if (e62 != nullptr && e63 != nullptr)
            {
                const int idx = param (e62);
                CHECK (e62->showEditor ("62")); pump (1500);
                e62->setParameterValue (idx, 0.15f);
                CHECK (e62->hideEditor()); pump (800);
                CHECK (e63->showEditor ("63")); pump (1500);
                e63->setParameterValue (idx, 0.85f);
                CHECK (e63->hideEditor()); pump (800);
                const auto& d62 = engine.getSetup().inputs[0].programs[62].slots[0].state;
                const auto& d63 = engine.getSetup().inputs[0].programs[63].slots[0].state;
                std::printf ("     after closing editors, model holds %d / %d bytes (%s)\n", (int) d62.getSize(), (int) d63.getSize(),
                             d62 == d63 ? "IDENTICAL" : "different");
                saveReload ("editor path", [&]
                {
                    engine.selectProgram (0, 62); settle(); engine.selectProgram (0, 63); settle();
                    float v62 = -1, v63 = -1;
                    if (auto* r = engine.getPlugin (0, 62, 0)) r->fetchParameterValue (idx, v62);
                    if (auto* r = engine.getPlugin (0, 63, 0)) r->fetchParameterValue (idx, v63);
                    std::printf ("     editor path: program 62 %.3f (want 0.15) program 63 %.3f (want 0.85)\n", v62, v63);
                    CHECK (std::abs (v62 - 0.15f) < 0.05f && std::abs (v63 - 0.85f) < 0.05f);
                });
            }
        }
    }

    // --- an audio input: the signal through a program's chain, trimmed, with a tail ----------
    {
        const int ai = engine.addInput ("Guitar", InputDef::Kind::audio);
        CHECK (engine.getSetup().inputs[(size_t) ai].isAudio());
        engine.setInputAudioChannels (ai, 0, -1);                  // mono, device channel 1
        settle();

        // A 1 kHz tone at 0.5 peak: 0.354 RMS if it comes through untouched.
        AudioBuffer<float> tone (1, kBlock), outBuf (2, kBlock);
        double phase = 0.0;
        auto feed = [&] (int blocks)
        {
            float peak = 0.0f;
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < kBlock; ++i) { tone.setSample (0, i, 0.5f * (float) std::sin (phase)); phase += MathConstants<double>::twoPi * 1000.0 / 48000.0; }
                outBuf.clear();
                engine.renderBlockForTesting (tone.getArrayOfReadPointers(), 1, outBuf.getArrayOfWritePointers(), 2, kBlock);
                peak = jmax (peak, outBuf.getRMSLevel (0, 0, kBlock), outBuf.getRMSLevel (1, 0, kBlock));
            }
            return peak;
        };
        const float through = feed (4);
        std::printf ("     audio input, no effects: %.3f rms (tone 0.354)\n", through);
        CHECK (std::abs (through - 0.354f) < 0.03f);

        // The meter reads the arriving peak, both sides for a mono input, before the trim.
        {
            const auto lv = engine.getInputLevel (ai);
            std::printf ("     meter: L %.3f R %.3f clip %d (tone peak 0.5)\n", lv.left, lv.right, (int) lv.clipped);
            CHECK (std::abs (lv.left - 0.5f) < 0.02f && std::abs (lv.right - 0.5f) < 0.02f && ! lv.clipped);
            const auto none = engine.getInputLevel (0);
            CHECK (none.left == 0.0f && none.right == 0.0f);               // a keyboard input has no meter
        }

        engine.setProgramInputGainDb (ai, 0, -6.0f);
        const float trimmed = feed (4);
        std::printf ("     trimmed -6 dB: %.3f rms (want 0.177)\n", trimmed);
        CHECK (std::abs (trimmed - 0.177f) < 0.02f);
        CHECK (std::abs (engine.getInputLevel (ai).left - 0.5f) < 0.02f);    // the trim does not move the meter

        // Full scale lights the clip mark; silence lets the meter fall.
        for (int i = 0; i < kBlock; ++i) tone.setSample (0, i, i % 2 ? 1.0f : -1.0f);
        outBuf.clear();
        engine.renderBlockForTesting (tone.getArrayOfReadPointers(), 1, outBuf.getArrayOfWritePointers(), 2, kBlock);
        CHECK (engine.getInputLevel (ai).clipped);
        tone.clear();
        for (int b = 0; b < 48; ++b) { outBuf.clear(); engine.renderBlockForTesting (tone.getArrayOfReadPointers(), 1, outBuf.getArrayOfWritePointers(), 2, kBlock); }
        std::printf ("     after half a second of silence: %.3f\n", engine.getInputLevel (ai).left);
        CHECK (engine.getInputLevel (ai).left < 0.4f && engine.getInputLevel (ai).left > 0.2f);   // 20 dB/s: about -10 dB in 0.5 s

        // An effect in the chain still carries the signal.
        PluginDescription anyFx; bool haveAnyFx = false;
        for (auto& d : host.getKnownPlugins().getTypes())
            if (! d.isInstrument && d.name.containsIgnoreCase ("Stereo Tools")) { anyFx = d; haveAnyFx = true; }
        if (haveAnyFx)
        {
            CHECK (engine.addEffect (ai, 0, -1, anyFx, err)); settle();
            const float withFx = feed (6);
            std::printf ("     through %s: %.3f rms\n", anyFx.name.toRawUTF8(), withFx);
            CHECK (withFx > 0.08f);
        }

        // A program change: the new program carries the input at its own trim; the old one rings out.
        engine.selectProgram (ai, 1); settle();
        const float switched = feed (6);
        std::printf ("     after a change to an empty program: %.3f rms (want 0.354)\n", switched);
        CHECK (std::abs (switched - 0.354f) < 0.03f);

        // Nothing leaks to a keyboard input's programs.
        engine.removeInput (ai);
        CHECK (feed (2) < 1e-3f);
    }

    // --- outputs: an input on its own device channels, through a master chain, at a master volume --
    {
        CHECK (engine.getSetup().outputs.size() == 1);
        const int mon = engine.addOutput ("Monitors", 2, 3);
        CHECK (mon == 1);
        engine.clearProgram (0, 70);
        engine.selectProgram (0, 70); settle();
        CHECK (engine.addSlot (0, 70, synth, err)); settle();
        engine.setInputOutput (0, mon);

        AudioBuffer<float> out4 (4, kBlock);
        auto levels = [&] (int blocks)
        {
            std::array<float, 4> peak {};
            for (int b = 0; b < blocks; ++b)
            {
                out4.clear();
                engine.renderBlockForTesting (out4.getArrayOfWritePointers(), 4, kBlock);
                for (int c = 0; c < 4; ++c) peak[(size_t) c] = jmax (peak[(size_t) c], out4.getRMSLevel (c, 0, kBlock));
            }
            return peak;
        };
        engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
        const auto p = levels (8);
        std::printf ("     on Monitors (3+4): ch1 %.3f ch2 %.3f ch3 %.3f ch4 %.3f\n", p[0], p[1], p[2], p[3]);
        CHECK (p[2] > 0.02f && p[3] > 0.02f && p[0] < 1e-4f && p[1] < 1e-4f);

        engine.setOutputGainDb (mon, -6.0f);
        const auto q = levels (8);
        std::printf ("     master -6 dB: ch3 %.3f (was %.3f)\n", q[2], p[2]);
        CHECK (std::abs (q[2] - p[2] * 0.5f) < p[2] * 0.15f);

        PluginDescription masterFx; bool haveMasterFx = false;
        for (auto& d : host.getKnownPlugins().getTypes())
            if (! d.isInstrument && d.name.containsIgnoreCase ("Stereo Tools")) { masterFx = d; haveMasterFx = true; }
        if (haveMasterFx)
        {
            CHECK (engine.addEffect (Engine::outputAddress (mon), 0, -1, masterFx, err)); settle();
            CHECK (engine.isPluginAlive (Engine::outputAddress (mon), 0, -1, 0));
            const auto r = levels (8);
            std::printf ("     through the master chain: ch3 %.3f\n", r[2]);
            CHECK (r[2] > 0.01f);
            CHECK (engine.getSetup().outputs[(size_t) mon].chain.effects.size() == 1);
        }

        // A volume controller, on any input: CC 7 at 0 is silence, at 127 full.
        engine.setOutputVolumeCC (mon, 7);
        engine.injectMidi (1, MidiMessage::controllerEvent (3, 7, 0));
        pump (60);
        CHECK (levels (4)[2] < 1e-3f);
        engine.injectMidi (1, MidiMessage::controllerEvent (3, 7, 127));
        pump (60);
        CHECK (levels (4)[2] > 0.02f);
        CHECK (std::abs (engine.getSetup().outputs[(size_t) mon].gainDb) < 0.01f);     // the model followed the pedal

        // Removing the output puts its input back on Main (1+2).
        engine.removeOutput (mon); settle();
        CHECK (engine.getSetup().outputs.size() == 1 && engine.getSetup().inputs[0].output == 0);
        const auto back = levels (8);
        std::printf ("     after removing Monitors: ch1 %.3f ch3 %.3f\n", back[0], back[2]);
        CHECK (back[0] > 0.02f && back[2] < 1e-4f);

        engine.injectMidi (0, MidiMessage::noteOff (1, 60));
        render (engine, 30);
    }

    // --- the built-in MIDI effects ----------------------------------------------------------
    {
        auto notesOn = [] (const MidiBuffer& b) { StringArray out; for (const auto m : b) if (m.getMessage().isNoteOn()) out.add (String (m.getMessage().getNoteNumber())); return out.joinIntoString (" "); };
        // Chord: a C becomes C E G, and letting go lets all three go.
        {
            EffectDef d; d.kind = EffectDef::Kind::chord; d.chordType = 0;
            auto fx = MidiFx::create (d);
            CHECK (fx != nullptr);
            MidiBuffer in, out;
            in.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 0);
            fx->process (in, out, 512, 48000.0, 120.0);
            std::printf ("     chord of C: %s\n", notesOn (out).toRawUTF8());
            CHECK (notesOn (out) == "60 64 67");
            in.clear(); out.clear();
            in.addEvent (MidiMessage::noteOff (1, 60), 0);
            fx->process (in, out, 512, 48000.0, 120.0);
            int offs = 0; for (const auto m : out) if (m.getMessage().isNoteOff()) ++offs;
            CHECK (offs == 3);
        }
        // Scale: C# in C major goes down to C; E stays.
        {
            EffectDef d; d.kind = EffectDef::Kind::scale; d.scaleRoot = 0; d.scaleMode = 0;
            auto fx = MidiFx::create (d);
            MidiBuffer in, out;
            in.addEvent (MidiMessage::noteOn (1, 61, (uint8) 100), 0);
            in.addEvent (MidiMessage::noteOn (1, 64, (uint8) 100), 10);
            fx->process (in, out, 512, 48000.0, 120.0);
            std::printf ("     C# and E in C major: %s\n", notesOn (out).toRawUTF8());
            CHECK (notesOn (out) == "60 64");
            // C and C# both land on C: letting go of C# must not cut the C that is still held.
            in.clear(); out.clear();
            in.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 0);
            fx->process (in, out, 512, 48000.0, 120.0);
            in.clear(); out.clear();
            in.addEvent (MidiMessage::noteOff (1, 61), 0);
            fx->process (in, out, 512, 48000.0, 120.0);
            int offs = 0; for (const auto m : out) if (m.getMessage().isNoteOff()) ++offs;
            in.clear(); out.clear();
            in.addEvent (MidiMessage::noteOff (1, 60), 0);
            fx->process (in, out, 512, 48000.0, 120.0);
            int offs2 = 0; for (const auto m : out) if (m.getMessage().isNoteOff()) ++offs2;
            std::printf ("     release C# then C: note-offs %d then %d\n", offs, offs2);
            CHECK (offs == 0 && offs2 == 1);
        }
        // Run: E pressed in C major, an octave up in one beat (120 bpm: eight notes across half a second), the last one held.
        {
            EffectDef d; d.kind = EffectDef::Kind::run; d.scaleRoot = 0; d.scaleMode = 0; d.runNotes = 8; d.runBeats = 1.0f; d.runDown = 0; d.runToKey = 0;
            auto fx = MidiFx::create (d);
            MidiBuffer in, out;
            in.addEvent (MidiMessage::noteOn (1, 64, (uint8) 90), 0);
            StringArray played;
            int offs = 0;
            for (int b = 0; b < (int) (1.2 * 48000 / 512); ++b)
            {
                out.clear();
                fx->process (in, out, 512, 48000.0, 120.0);
                in.clear();
                for (const auto m : out) { if (m.getMessage().isNoteOn()) played.add (String (m.getMessage().getNoteNumber())); if (m.getMessage().isNoteOff()) ++offs; }
            }
            std::printf ("     run from E: %s (%d note-offs so far)\n", played.joinIntoString (" ").toRawUTF8(), offs);
            CHECK (played.joinIntoString (" ") == "64 65 67 69 71 72 74 76" && offs == 7);     // the last note is still held
            // The timing: all eight within the beat, the last one landing on it (0.5 s at 120).
            {
                auto fx2 = MidiFx::create (d);
                MidiBuffer in2, out2;
                in2.addEvent (MidiMessage::noteOn (1, 64, (uint8) 90), 0);
                int ons = 0, lastAt = -1, blocks = 0;
                for (int b = 0; b < 60; ++b)
                {
                    out2.clear();
                    fx2->process (in2, out2, 512, 48000.0, 120.0);
                    in2.clear();
                    for (const auto m : out2) if (m.getMessage().isNoteOn()) { ++ons; lastAt = b * 512 + m.samplePosition; }
                    ++blocks;
                }
                std::printf ("     octave in a beat: %d notes, the last at %.3f s\n", ons, lastAt / 48000.0);
                CHECK (ons == 8 && std::abs (lastAt / 48000.0 - 0.5) < 0.012);
            }
            // From the 3rd above the key: C pressed starts the run on E.
            {
                EffectDef d3 = d; d3.runDegree = 2;
                auto fx3 = MidiFx::create (d3);
                MidiBuffer in3, out3;
                in3.addEvent (MidiMessage::noteOn (1, 60, (uint8) 90), 0);
                StringArray got;
                for (int b = 0; b < 60; ++b) { out3.clear(); fx3->process (in3, out3, 512, 48000.0, 120.0); in3.clear(); for (const auto m : out3) if (m.getMessage().isNoteOn()) got.add (String (m.getMessage().getNoteNumber())); }
                std::printf ("     C pressed, from the 3rd: %s\n", got.joinIntoString (" ").toRawUTF8());
                CHECK (got.joinIntoString (" ") == "64 65 67 69 71 72 74 76");
            }
            in.addEvent (MidiMessage::noteOff (1, 64), 0);
            out.clear();
            fx->process (in, out, 512, 48000.0, 120.0);
            int lastOff = -1; for (const auto m : out) if (m.getMessage().isNoteOff()) lastOff = m.getMessage().getNoteNumber();
            CHECK (lastOff == 76);
            // Up to the key: the same eight notes land on E.
            d.runToKey = 1;
            fx->configure (d);
            in.clear(); out.clear(); played.clear();
            in.addEvent (MidiMessage::noteOn (1, 76, (uint8) 90), 0);
            for (int b = 0; b < (int) (1.2 * 48000 / 512); ++b)
            {
                out.clear();
                fx->process (in, out, 512, 48000.0, 120.0);
                in.clear();
                for (const auto m : out) if (m.getMessage().isNoteOn()) played.add (String (m.getMessage().getNoteNumber()));
            }
            std::printf ("     run up to E: %s\n", played.joinIntoString (" ").toRawUTF8());
            CHECK (played.joinIntoString (" ") == "64 65 67 69 71 72 74 76");
        }
        // Arpeggiator: C and E held, eighths at 120 (a step every 0.25 s): four notes a second, alternating.
        {
            EffectDef d; d.kind = EffectDef::Kind::arpeggio; d.arpRate = 1; d.arpMode = 0; d.arpOctaves = 1; d.arpGate = 50;
            auto fx = MidiFx::create (d);
            MidiBuffer in, out;
            in.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 0);
            in.addEvent (MidiMessage::noteOn (1, 64, (uint8) 100), 0);
            StringArray played;
            int offs = 0;
            for (int b = 0; b < 48000 / 512; ++b)
            {
                out.clear();
                fx->process (in, out, 512, 48000.0, 120.0);
                in.clear();
                for (const auto m : out) { if (m.getMessage().isNoteOn()) played.add (String (m.getMessage().getNoteNumber())); if (m.getMessage().isNoteOff()) ++offs; }
            }
            std::printf ("     arpeggio over a second: %s (%d note-offs)\n", played.joinIntoString (" ").toRawUTF8(), offs);
            CHECK (played.size() == 4 && played[0] == "60" && played[1] == "64" && played[2] == "60" && offs >= 3);
        }
    }

    // --- stretching a recording: longer without changing its pitch, higher without its length --
    {
        auto tone = [] (double freq, double seconds, double rate)
        {
            std::vector<int16_t> v ((size_t) (seconds * rate));
            for (size_t i = 0; i < v.size(); ++i) v[i] = (int16_t) (20000.0 * std::sin (MathConstants<double>::twoPi * freq * (double) i / rate));
            return v;
        };
        auto hzOf = [] (const std::vector<int16_t>& v, double rate)
        {
            // Zero crossings over the middle half, where the stretch has settled.
            int crossings = 0;
            const size_t a = v.size() / 4, b = v.size() * 3 / 4;
            for (size_t i = a + 1; i < b; ++i) if ((v[i - 1] < 0) != (v[i] < 0)) ++crossings;
            return crossings * rate / (2.0 * (double) (b - a));
        };
        const double rate = 48000.0;
        auto l = tone (440.0, 2.0, rate), r = l;
        stretchAndShift (l, r, 1.5, 0.0, rate);
        std::printf ("     stretched x1.5: %.3f s, %.1f Hz\n", l.size() / rate, hzOf (l, rate));
        CHECK (std::abs (l.size() / rate - 3.0) < 0.01 && std::abs (hzOf (l, rate) - 440.0) < 5.0);
        l = tone (440.0, 2.0, rate); r = l;
        stretchAndShift (l, r, 1.0, 12.0, rate);
        std::printf ("     up an octave: %.3f s, %.1f Hz\n", l.size() / rate, hzOf (l, rate));
        CHECK (std::abs (l.size() / rate - 2.0) < 0.01 && std::abs (hzOf (l, rate) - 880.0) < 10.0);
        l = tone (440.0, 2.0, rate); r = l;
        stretchAndShift (l, r, 0.8, -2.0, rate);
        std::printf ("     shorter and down two: %.3f s, %.1f Hz\n", l.size() / rate, hzOf (l, rate));
        CHECK (std::abs (l.size() / rate - 1.6) < 0.01 && std::abs (hzOf (l, rate) - 392.0) < 6.0);
    }

    // --- recording the gig: every output to WAV, the keyboards to MIDI ----------------------
    {
        engine.setSetupFile (scratchDir.getChildFile ("rec.stagerig.json"));
        String err;
        CHECK (engine.startRecording (err));
        CHECK (engine.isRecording());
        const auto take = engine.getRecordingFolder();
        std::printf ("     recording into %s\n", take.getFullPathName().toRawUTF8());
        CHECK (take.isDirectory() && take.getParentDirectory().getFileName() == "rec.stagerig.recordings");
        engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
        AudioBuffer<float> rec (2, kBlock);
        for (int b = 0; b < 40; ++b) { rec.clear(); engine.renderBlockForTesting (rec.getArrayOfWritePointers(), 2, kBlock); }
        engine.injectMidi (0, MidiMessage::noteOff (1, 60));
        for (int b = 0; b < 10; ++b) { rec.clear(); engine.renderBlockForTesting (rec.getArrayOfWritePointers(), 2, kBlock); }
        pump (600);                                                      // the MIDI queue is drained twice a second
        engine.stopRecording();
        CHECK (! engine.isRecording());

        const auto wav = take.getChildFile ("Main.wav");
        const auto mid = take.getChildFile ("keyboards.mid");
        CHECK (wav.existsAsFile() && mid.existsAsFile());
        AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<AudioFormatReader> reader (fm.createReaderFor (wav));
        CHECK (reader != nullptr);
        if (reader != nullptr)
        {
            AudioBuffer<float> got (2, (int) reader->lengthInSamples);
            reader->read (&got, 0, got.getNumSamples(), 0, true, true);
            std::printf ("     take: %lld samples at %.0f Hz, peak %.3f\n", (long long) reader->lengthInSamples, reader->sampleRate, got.getMagnitude (0, got.getNumSamples()));
            CHECK (reader->lengthInSamples == 50 * kBlock && reader->numChannels == 2 && got.getMagnitude (0, got.getNumSamples()) > 0.01f);
        }
        MidiFile mf;
        FileInputStream in (mid);
        CHECK (mf.readFrom (in) && mf.getNumTracks() >= 1);
        int notes = 0;
        for (int t = 0; t < mf.getNumTracks(); ++t)
            for (const auto* e : *mf.getTrack (t)) if (e->message.isNoteOn()) ++notes;
        std::printf ("     MIDI file: %d tracks, %d note-ons\n", mf.getNumTracks(), notes);
        CHECK (notes == 1);
        engine.setSetupFile (File());
    }

    // --- Ableton Link: the tempo shared with a peer ----------------------------------------
    {
        engine.setLinkEnabled (true);
        CHECK (engine.isLinkEnabled());
        engine.setTempoBpm (100.0);
        std::printf ("     Link on: session tempo %.1f\n", engine.getLinkTempoForTesting());
        CHECK (std::abs (engine.getLinkTempoForTesting() - 100.0) < 0.01);

        /* A second Link in this process is another app on the network. It
           needs multicast on the loopback, which not every machine allows;
           without it there is nothing to test but no fault either. */
        ableton::Link peer (120.0);
        peer.enable (true);
        for (int i = 0; i < 120 && engine.getLinkPeers() == 0; ++i) pump (50);
        if (engine.getLinkPeers() > 0)
        {
            auto st = peer.captureAppSessionState();
            st.setTempo (95.0, peer.clock().micros());
            peer.commitAppSessionState (st);
            for (int i = 0; i < 100 && std::abs (engine.getTempoBpm() - 95.0) > 0.01; ++i) pump (50);
            std::printf ("     a peer set 95: the rig is at %.1f\n", engine.getTempoBpm());
            CHECK (std::abs (engine.getTempoBpm() - 95.0) < 0.01);
            engine.setTempoBpm (110.0);
            for (int i = 0; i < 100 && std::abs (peer.captureAppSessionState().tempo() - 110.0) > 0.01; ++i) pump (50);
            std::printf ("     the rig set 110: the peer is at %.1f\n", peer.captureAppSessionState().tempo());
            CHECK (std::abs (peer.captureAppSessionState().tempo() - 110.0) < 0.01);
        }
        else
            std::printf ("     (no Link peer found on this machine's loopback: skipping the two-way check)\n");
        peer.enable (false);
        engine.setLinkEnabled (false);
        engine.setTempoBpm (120.0);
    }

    // --- MIDI clock out: 24 ticks a beat at the rig's tempo ---------------------------------
    {
        /* No device need be open for the ticks to be made; a port that is not
           there just drops them. 120 bpm is 48 ticks a second; 60 bpm, 24. */
        AudioBuffer<float> out2 (2, kBlock);
        const int blocksPerSecond = roundToInt (engine.getSampleRate() / kBlock);
        auto second = [&]
        {
            const int before = engine.getClockTicksForTesting();
            for (int b = 0; b < blocksPerSecond; ++b)
            {
                out2.clear();
                engine.renderBlockForTesting (out2.getArrayOfWritePointers(), 2, kBlock);
            }
            return engine.getClockTicksForTesting() - before;
        };
        CHECK (second() == 0);                                           // nothing asked for: no ticks
        engine.setTempoBpm (120.0);
        engine.setClockTo ({ "Nowhere", "test:nowhere" }, true);
        CHECK (engine.sendsClockTo ("test:nowhere") && engine.getClockOut().size() == 1);
        const int at120 = second();
        engine.setTempoBpm (60.0);
        const int at60 = second();
        const double seconds = blocksPerSecond * kBlock / engine.getSampleRate();
        std::printf ("     MIDI clock: %d ticks in %.3f s at 120 bpm, %d at 60\n", at120, seconds, at60);
        CHECK (std::abs (at120 - 48.0 * seconds) <= 1.0 && std::abs (at60 - 24.0 * seconds) <= 1.0);
        engine.setClockTo ({ "Nowhere", "test:nowhere" }, false);
        CHECK (second() == 0 && engine.getClockOut().empty());
        engine.setTempoBpm (120.0);
    }

    // --- finding the tempo of a recording -------------------------------------------------
    {
        /* A drum pattern at an awkward tempo with a lead-in before the first
           downbeat: kick on 1 and 3, snare on 2 and 4, hats on the eighths,
           a little noise under it all. The detector should find the tempo
           to within a fraction of a bpm and the first downbeat to within a
           frame or two. */
        auto drumWav = [&] (const File& f, double bpm, double leadIn, double seconds, double rate)
        {
            f.deleteFile();
            WavAudioFormat wav;
            std::unique_ptr<OutputStream> os (f.createOutputStream().release());
            std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (os.get(), rate, 2, 16, {}, 0));
            if (w == nullptr) return false;
            os.release();
            AudioBuffer<float> b (2, (int) (seconds * rate));
            b.clear();
            Random rng (7);
            const double beat = 60.0 / bpm;
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const float noise = 0.02f * (rng.nextFloat() * 2.0f - 1.0f);
                b.addSample (0, i, noise); b.addSample (1, i, noise);
            }
            auto hit = [&] (double t, double freq, double len, float amp, bool noise)
            {
                const int s = (int) (t * rate), n = (int) (len * rate);
                for (int i = 0; i < n && s + i < b.getNumSamples(); ++i)
                {
                    const float env = std::exp (-6.0f * (float) i / (float) n);
                    const float v = amp * env * (noise ? (rng.nextFloat() * 2.0f - 1.0f) : (float) std::sin (MathConstants<double>::twoPi * freq * i / rate));
                    b.addSample (0, s + i, v); b.addSample (1, s + i, v);
                }
            };
            for (int k = 0; leadIn + k * beat < seconds; ++k)
            {
                const double t = leadIn + k * beat;
                if (k % 4 == 0)      hit (t, 60.0, 0.15, 0.9f, false);       // a heavy kick on 1
                else if (k % 2 == 0) hit (t, 60.0, 0.15, 0.6f, false);       // a lighter one on 3
                else                 hit (t, 0.0, 0.10, 0.5f, true);         // snare on 2 and 4
                hit (t, 0.0, 0.03, 0.2f, true);                              // hats
                hit (t + beat / 2, 0.0, 0.03, 0.2f, true);
            }
            return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        };
        AudioFormatManager fm;
        fm.registerBasicFormats();
        for (auto [bpm, leadIn, rate] : { std::tuple { 123.4, 0.37, 44100.0 }, std::tuple { 97.0, 1.21, 48000.0 }, std::tuple { 168.0, 0.0, 48000.0 } })
        {
            const auto f = scratchDir.getChildFile ("drums.wav");
            CHECK (drumWav (f, bpm, leadIn, 40.0, rate));
            std::unique_ptr<AudioFormatReader> reader (fm.createReaderFor (f));
            CHECK (reader != nullptr);
            const auto t0 = Time::getMillisecondCounterHiRes();
            const auto est = detectTempo (*reader, 4);
            std::printf ("     %.1f bpm, first beat %.3f s: found %.2f bpm, first beat %.3f s, confidence %.2f, in %.0f ms\n",
                         bpm, leadIn, est.bpm, est.firstBeat, est.confidence, Time::getMillisecondCounterHiRes() - t0);
            CHECK (est.ok);
            /* Half or double the tempo is the one honest ambiguity -- a
               backbeat at 168 is also a slow 84 -- so either octave passes,
               and the downbeat must then fall on one of the heavy kicks. */
            const double ratio = est.bpm / bpm;
            const bool octave = std::abs (ratio - 1.0) < 0.003 || std::abs (ratio - 0.5) < 0.0015 || std::abs (ratio - 2.0) < 0.006;
            CHECK (octave);
            /* At the true tempo the downbeat must be a heavy kick, which
               comes round every four beats; heard at half the tempo the grid
               may sit on the snares instead, so any beat will do there. */
            const double heavy = (std::abs (ratio - 1.0) < 0.003 ? 4.0 : 1.0) * 60.0 / bpm;
            const double off = std::fmod (est.firstBeat - leadIn + 100.0 * heavy, heavy);
            CHECK (jmin (off, heavy - off) < 0.025);
            CHECK (est.confidence > 0.3);
        }
        // Too short to say anything about.
        {
            const auto f = scratchDir.getChildFile ("short.wav");
            CHECK (drumWav (f, 120.0, 0.0, 2.0, 48000.0));
            std::unique_ptr<AudioFormatReader> reader (fm.createReaderFor (f));
            CHECK (reader != nullptr && ! detectTempo (*reader, 4).ok);
        }
    }

    // --- backing tracks: stems from disk onto their outputs, a count-in, stop, end ----------
    {
        // A setup file of our own, so the track's folder is a scratch one.
        const auto setupFile = scratchDir.getChildFile ("tracks.stagerig.json");
        engine.setSetupFile (setupFile);
        const auto folder = engine.getChartsFolder();
        folder.createDirectory();

        // A one-second 440 Hz stem at 44.1 kHz, so the player has to resample to the engine's rate.
        auto writeTone = [] (const File& f, double seconds, double rate, float amp)
        {
            f.deleteFile();
            WavAudioFormat wav;
            std::unique_ptr<OutputStream> os (f.createOutputStream().release());
            std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (os.get(), rate, 1, 16, {}, 0));
            if (w == nullptr) return false;
            os.release();
            AudioBuffer<float> b (1, (int) (seconds * rate));
            for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (0, i, amp * (float) std::sin (MathConstants<double>::twoPi * 440.0 * i / rate));
            return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        };
        CHECK (writeTone (folder.getChildFile ("stem.wav"), 1.0, 44100.0, 0.5f));

        const int click = engine.addOutput ("Click", 2, 3);
        const int song = engine.addSong ("Backing");
        engine.addSongStem (song, { "stem.wav", "Tone", 0, 0.0f });
        auto waitReady = [&]
        {
            for (int i = 0; i < 200 && ! engine.isTrackReady(); ++i) pump (25);
            return engine.isTrackReady();
        };
        engine.selectSong (song);
        CHECK (engine.getTrackSong() == song && engine.hasTrack());
        CHECK (waitReady());
        CHECK (! engine.isTrackRunning());
        std::printf ("     track length %.2f s\n", engine.getTrackLength());
        CHECK (std::abs (engine.getTrackLength() - 1.0) < 0.01);

        AudioBuffer<float> out4 (4, kBlock);
        auto levels = [&] (int blocks)
        {
            std::array<float, 4> peak {};
            for (int b = 0; b < blocks; ++b)
            {
                out4.clear();
                engine.renderBlockForTesting (out4.getArrayOfWritePointers(), 4, kBlock);
                for (int c = 0; c < 4; ++c) peak[(size_t) c] = jmax (peak[(size_t) c], out4.getMagnitude (c, 0, kBlock));
            }
            return peak;
        };

        CHECK (levels (4)[0] < 1e-4f);                                   // armed, not playing: silent
        engine.startTrack();
        CHECK (engine.isTrackRunning());
        const auto playing = levels (8);
        std::printf ("     playing: main %.3f click %.3f\n", playing[0], playing[2]);
        CHECK (playing[0] > 0.4f && playing[1] > 0.4f && playing[2] < 1e-4f);   // mono stem on both sides of Main
        CHECK (engine.getTrackPosition() > 0.05);

        // A level change takes effect while it plays.
        engine.setSongStem (song, 0, { "stem.wav", "Tone", 0, -6.0f });
        const auto quieter = levels (4);
        std::printf ("     at -6 dB: main %.3f\n", quieter[0]);
        CHECK (std::abs (quieter[0] - 0.25f) < 0.03f);

        engine.stopTrack();
        levels (4);                                                      // the fade
        pump (50);
        CHECK (! engine.isTrackRunning());
        CHECK (levels (4)[0] < 1e-4f);

        // Count-in: two beats at 120 bpm on the click output, then the stem on Main.
        engine.setSongTempo (song, 120.0);
        engine.setSongCountIn (song, 2, click);
        engine.selectSong (song);
        CHECK (waitReady());
        engine.startTrack();
        levels (1);                                                      // the first block begins the count-in
        CHECK (engine.isTrackCountingIn());
        const auto countIn = levels (40);                                // 0.43 s at 48 kHz: inside the count-in
        std::printf ("     count-in: main %.3f click %.3f\n", countIn[0], countIn[2]);
        CHECK (countIn[2] > 0.2f && countIn[0] < 1e-4f);
        const auto after = levels (80);                                  // past the second beat
        std::printf ("     after the count-in: main %.3f\n", after[0]);
        CHECK (after[0] > 0.1f);

        // It ends by itself at the end of the file.
        levels (120);
        pump (50);
        std::printf ("     ended: running %d\n", (int) engine.isTrackRunning());
        CHECK (! engine.isTrackRunning());

        /* The click through the song: at 120 bpm a beat is 24000 samples, a
           click 1440, so of 47 blocks of 512 (one beat) the first three carry
           the click and the rest are silent. Without the count-in it starts
           on beat 1 of the file. */
        engine.setSongCountIn (song, 0, click);
        engine.setSongClick (song, true, 4);
        CHECK (engine.hasTrack());
        engine.startTrack();
        const auto onBeat = levels (3), offBeat = levels (40), nextBeat = levels (6);
        std::printf ("     click through the song: on the beat %.3f, between %.3f, next beat %.3f\n", onBeat[2], offBeat[2], nextBeat[2]);
        CHECK (onBeat[2] > 0.2f && offBeat[2] < 1e-4f && nextBeat[2] > 0.2f);
        CHECK (onBeat[0] > 0.2f);                                        // the stem plays too (at -6 dB from above)
        engine.stopTrack();
        levels (4);
        pump (50);

        // Moving the first beat moves the click: a quarter of a second in, the first click comes 23 blocks later.
        engine.setSongFirstBeat (song, 0.25);
        engine.startTrack();
        const auto early = levels (20), onFirst = levels (6);
        std::printf ("     first beat at 0.25 s: before %.3f, on it %.3f\n", early[2], onFirst[2]);
        CHECK (early[2] < 1e-4f && onFirst[2] > 0.2f);
        engine.stopTrack();
        levels (4);
        pump (50);
        engine.setSongFirstBeat (song, 0.0);
        engine.setSongClick (song, false, 4);
        engine.setSongCountIn (song, 2, click);                          // as the save below expects

        /* A file with silence at its head starts at its first sound: half a
           second of nothing, then the tone, and Play is loud at once. A start
           point of the song's own wins over that. */
        {
            auto writeLeadIn = [&] (const File& f)
            {
                f.deleteFile();
                WavAudioFormat wav;
                std::unique_ptr<OutputStream> os (f.createOutputStream().release());
                std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (os.get(), 48000.0, 1, 16, {}, 0));
                if (w == nullptr) return false;
                os.release();
                AudioBuffer<float> b (1, 72000);
                b.clear();
                for (int i = 24000; i < 72000; ++i) b.setSample (0, i, 0.5f * (float) std::sin (MathConstants<double>::twoPi * 440.0 * i / 48000.0));
                return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            };
            CHECK (writeLeadIn (folder.getChildFile ("leadin.wav")));
            const int lead = engine.addSong ("Lead-in");
            engine.addSongStem (lead, { "leadin.wav", {}, 0, 0.0f });
            engine.selectSong (lead);
            CHECK (waitReady());
            std::printf ("     lead-in: cue %.3f s\n", engine.getTrackCue());
            CHECK (std::abs (engine.getTrackCue() - 0.49) < 0.02);
            engine.startTrack();
            const auto atOnce = levels (3);
            std::printf ("     lead-in: at once %.3f, position %.3f\n", atOnce[0], engine.getTrackPosition());
            CHECK (atOnce[0] > 0.4f && engine.getTrackPosition() > 0.5);
            engine.stopTrack();
            levels (4);
            pump (50);
            engine.setSongStart (lead, 1.0);
            CHECK (std::abs (engine.getTrackCue() - 1.0) < 0.01);
            engine.startTrack();
            levels (1);
            std::printf ("     start point 1.0: position %.3f\n", engine.getTrackPosition());
            CHECK (engine.getTrackPosition() >= 1.0);
            engine.stopTrack();
            levels (4);
            pump (50);
            engine.selectSong (song);
            CHECK (waitReady());
        }

        /* Files that know their tempo follow the song's: the one-second
           stem, made "at 120", plays for two seconds when the song is at 60,
           and every time on the panel still reads in the file's seconds. */
        {
            const int slow = engine.addSong ("Half speed");
            engine.addSongStem (slow, { "stem.wav", "Tone", 0, 0.0f });
            engine.setSongTrackBpm (slow, 120.0);
            engine.setSongTempo (slow, 60.0);
            engine.selectSong (slow);
            CHECK (waitReady());
            std::printf ("     stretched song: time scale %.2f, length reads %.2f s\n", engine.getTrackTimeScale(), engine.getTrackLength());
            CHECK (std::abs (engine.getTrackTimeScale() - 2.0) < 1e-6 && std::abs (engine.getTrackLength() - 1.0) < 0.02);
            engine.startTrack();
            levels (150);                                                 // 1.6 s: past where the file would have ended
            CHECK (engine.isTrackRunning());
            std::printf ("     still playing at 1.6 s: position reads %.2f s (file time)\n", engine.getTrackPosition());
            CHECK (engine.getTrackPosition() > 0.7 && engine.getTrackPosition() < 1.0);
            engine.stopTrack();
            levels (4);
            pump (50);
            engine.selectSong (song);
            CHECK (waitReady());
        }

        // A song with no files but a click is a metronome: Play runs until stopped.
        {
            const int metro = engine.addSong ("Metronome");
            engine.setSongTempo (metro, 120.0);
            engine.setSongCountIn (metro, 0, click);
            engine.setSongClick (metro, true, 4);
            engine.selectSong (metro);
            CHECK (engine.hasTrack());
            CHECK (waitReady());
            engine.startTrack();
            const auto m1 = levels (3), m2 = levels (40), m3 = levels (6);
            std::printf ("     metronome: %.3f %.3f %.3f, running %d\n", m1[2], m2[2], m3[2], (int) engine.isTrackRunning());
            CHECK (m1[2] > 0.2f && m2[2] < 1e-4f && m3[2] > 0.2f && engine.isTrackRunning());
            CHECK (m1[0] < 1e-4f);                                       // nothing on Main
            // Its click turned off mid-play: nothing left to play, so it stops rather than sit "playing".
            engine.setSongClick (metro, false, 4);
            levels (2);
            pump (50);
            std::printf ("     click off while playing: running %d\n", (int) engine.isTrackRunning());
            CHECK (! engine.isTrackRunning());
            engine.setSongClick (metro, true, 4);
            CHECK (waitReady());
            engine.startTrack();
            levels (2);
            CHECK (engine.isTrackRunning());
            engine.stopTrack();
            levels (4);
            pump (50);
            CHECK (! engine.isTrackRunning());
            engine.selectSong (song);
            CHECK (waitReady());
        }

        // Sections: a three-second stem whose level steps up each second -- A 0.2, B 0.5, C 0.8.
        {
            const auto f = folder.getChildFile ("sections.wav");
            f.deleteFile();
            WavAudioFormat wav;
            std::unique_ptr<OutputStream> os (f.createOutputStream().release());
            std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (os.get(), 48000.0, 1, 16, {}, 0));
            CHECK (w != nullptr);
            os.release();
            AudioBuffer<float> b (1, 3 * 48000);
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const float amp = i < 48000 ? 0.2f : (i < 96000 ? 0.5f : 0.8f);
                b.setSample (0, i, amp * (float) std::sin (MathConstants<double>::twoPi * 440.0 * i / 48000.0));
            }
            CHECK (w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()));
        }
        const int secSong = engine.addSong ("Sections");
        engine.addSongStem (secSong, { "sections.wav", {}, 0, 0.0f });
        /* Section B carries a cue sheet: Upper goes to program 3 when the
           track enters it; nothing happens again while the vamp goes round. */
        {
            ActionDef toThree; toThree.kind = ActionDef::Kind::program; toThree.input = 0; toThree.program = 3;
            std::vector<MarkerDef> ms = { { "A", 0.0, false }, { "B", 1.0, true }, { "C", 2.0, false } };
            ms[1].actions = { toThree };
            engine.setSongMarkers (secSong, ms);
        }
        engine.selectProgram (0, 0);
        engine.selectSong (secSong);
        CHECK (waitReady());
        CHECK (engine.getTrackSectionCount() == 3 && engine.getTrackSectionName (1) == "B" && engine.isTrackSectionVamp (1));

        auto peakFor = [&] (double seconds) { return levels ((int) (seconds * 48000.0 / kBlock))[0]; };
        engine.startTrack();
        peakFor (1.4);                                                   // into B
        CHECK (engine.getTrackSection() == 1 && engine.isTrackVamping());
        const auto inVamp = peakFor (1.5);                               // B's end has come and gone: still B
        std::printf ("     vamping: section %d, level %.2f, position %.2f s\n", engine.getTrackSection(), inVamp, engine.getTrackPosition());
        CHECK (engine.getTrackSection() == 1 && std::abs (inVamp - 0.5f) < 0.05f && engine.getTrackPosition() < 2.0);
        pump (30);
        std::printf ("     entering B put Upper on program %d\n", engine.getSetup().inputs[0].currentProgram);
        CHECK (engine.getSetup().inputs[0].currentProgram == 3);
        engine.selectProgram (0, 0);
        levels (100);                                                    // the vamp goes round again: no second firing
        pump (30);
        CHECK (engine.getSetup().inputs[0].currentProgram == 0);

        engine.advanceTrack();                                           // the pedal: release
        peakFor (1.1);                                                   // to the end of this pass, then C
        CHECK (engine.getTrackSection() == 2);
        std::printf ("     released into C: section %d, level %.2f\n", engine.getTrackSection(), levels (4)[0]);

        engine.queueTrackSection (0);                                    // at C's end, back to A instead of stopping
        peakFor (1.1);
        std::printf ("     queued A: section %d, running %d\n", engine.getTrackSection(), (int) engine.isTrackRunning());
        CHECK (engine.isTrackRunning() && engine.getTrackSection() == 0);
        CHECK (std::abs (levels (4)[0] - 0.2f) < 0.05f);

        engine.stopTrack();
        levels (4); pump (50);
        CHECK (! engine.isTrackRunning());

        // Cued while stopped: Play starts at C.
        engine.queueTrackSection (2);
        engine.startTrack();
        levels (2);
        std::printf ("     cued C: section %d, position %.2f s\n", engine.getTrackSection(), engine.getTrackPosition());
        CHECK (engine.getTrackSection() == 2 && engine.getTrackPosition() >= 2.0);
        engine.stopTrack();
        levels (4); pump (50);

        /* A MIDI file plays into the backing-track input. The first file
           makes that input, and gives the song a program there named after
           it, with the General MIDI player: the file plays as it is. */
        {
            MidiMessageSequence seq;
            seq.addEvent (MidiMessage::noteOn  (1, 60, (uint8) 100), 0);       // ticks; 960 per quarter, 120 bpm
            seq.addEvent (MidiMessage::noteOff (1, 60), 960 * 3);              // held for 1.5 s
            seq.addEvent (MidiMessage::programChange (1, 5), 10);              // for the GM player only
            seq.updateMatchedPairs();
            MidiFile mf;
            mf.setTicksPerQuarterNote (960);
            mf.addTrack (seq);
            const auto f = folder.getChildFile ("part.mid");
            f.deleteFile();
            FileOutputStream os (f);
            CHECK (os.openedOk() && mf.writeTo (os));
        }
        CHECK (engine.getTrackInput() < 0);
        const int midiSong = engine.addSong ("MIDI part");
        StemDef part; part.file = "part.mid";
        engine.addSongStem (midiSong, part);
        const int track = engine.getTrackInput();
        CHECK (track >= 0 && engine.getSetup().inputs[(size_t) track].isTrack() && engine.getSetup().inputs[(size_t) track].channel == 0);
        const int trackProg = engine.getSongs()[(size_t) midiSong].programFor (track);
        CHECK (trackProg >= 0);
        auto trackProgram = [&]() -> const ProgramDef& { return engine.getSetup().inputs[(size_t) track].programs[(size_t) trackProg]; };
        CHECK (trackProgram().name == "MIDI part" && trackProgram().slots.size() == 1 && trackProgram().slots[0].isGm() && trackProgram().slots[0].hearsAll());

        const int programBefore = engine.getSetup().inputs[0].currentProgram;
        engine.selectSong (midiSong);
        CHECK (waitReady() && engine.hasTrack());
        CHECK (engine.getSetup().inputs[(size_t) track].currentProgram == trackProg);
        settle();                                                        // the GM player reads its soundfont
        std::printf ("     MIDI track length %.2f s\n", engine.getTrackLength());
        CHECK (std::abs (engine.getTrackLength() - 1.5) < 0.05);

        auto playAndStop = [&] (int blocks)
        {
            engine.startTrack();
            const float l = levels (blocks)[0];                          // about a quarter of a second in
            engine.stopTrack();
            levels (4); pump (50); levels (60);
            return l;
        };

        const bool gmAvailable = GmSynth::unavailableReason().isEmpty();
        if (! gmAvailable)
            std::printf ("     skip General MIDI check (%s)\n", GmSynth::unavailableReason().toRawUTF8());
        else
        {
            CHECK (engine.isPluginAlive (track, trackProg, 0));
            const float gmLevel = playAndStop (24);
            std::printf ("     General MIDI player (%s): %.3f\n", GmSynth::findSoundfont().getFileName().toRawUTF8(), gmLevel);
            CHECK (gmLevel > 0.01f);
            const float gmAfter = levels (8)[0];
            std::printf ("     after stopping the GM player: %.5f\n", gmAfter);
            CHECK (gmAfter < 1e-3f);
        }
        CHECK (engine.getSetup().inputs[0].currentProgram == programBefore);    // the file's program change is not Upper's business

        /* The part to Upper's sounds instead -- an instrument that hands the
           notes to that keyboard's program -- and the GM player stops
           hearing it: one instrument per part, as the editor assigns them. */
        CHECK (engine.addInputSlot (track, trackProg, 0));
        engine.assignPart (track, trackProg, 1, 1);
        CHECK (! trackProgram().slots[0].hears (1) && trackProgram().slots[1].hears (1) && trackProgram().slots[1].isInput()
               && trackProgram().slots[1].targetInputName == "Upper" && trackProgram().slots[1].displayName() == "Upper's sounds");
        CHECK (engine.partInstrument (track, trackProg, 1) == 1 && engine.partInstrument (track, trackProg, 2) == 0);
        engine.startTrack();
        const float viaKeys = levels (24)[0];
        std::printf ("     channel 1 to Upper's sounds: %.3f\n", viaKeys);
        CHECK (viaKeys > 0.01f);
        CHECK (engine.getSetup().inputs[0].currentProgram == programBefore);    // its program change was dropped on the way
        engine.stopTrack();                                              // with the note still held
        levels (4); pump (50);
        levels (120);                                                    // the synth's release
        const float afterStop = levels (8)[0];
        std::printf ("     after stopping with a note held: %.5f\n", afterStop);
        CHECK (afterStop < 1e-3f);

        /* The part on one of Upper's programs by name -- program 6, "Pad",
           which Upper itself is not on. It is loaded for the part and plays
           beside Upper's own program; Upper's keys still play that one. */
        {
            engine.clearProgram (0, 6);
            engine.setProgramName (0, 6, "Pad");
            CHECK (engine.addSlot (0, 6, synth, err));
            CHECK (! engine.isProgramLoaded (0, 6));
            CHECK (engine.addInputSlot (track, trackProg, 0, 6));
            const int padSlot = (int) trackProgram().slots.size() - 1;
            CHECK (trackProgram().slots[(size_t) padSlot].targetProgram == 6 && trackProgram().slots[(size_t) padSlot].displayName() == "Upper: Pad");
            engine.assignPart (track, trackProg, 1, padSlot);
            CHECK (engine.isProgramLoaded (0, 6));                      // named by a current program: kept loaded
            settle();
            CHECK (engine.isPluginAlive (0, 6, 0));
            CHECK (engine.getSetup().inputs[0].currentProgram == programBefore);
            engine.startTrack();
            const float viaPad = levels (24)[0];
            std::printf ("     channel 1 to Upper: Pad (Upper on another program): %.3f\n", viaPad);
            CHECK (viaPad > 0.01f);
            CHECK (engine.getSetup().inputs[0].currentProgram == programBefore);
            engine.stopTrack();
            levels (4); pump (50); levels (120);
            const float padQuiet = levels (8)[0];
            std::printf ("     after stopping: %.5f\n", padQuiet);
            CHECK (padQuiet < 1e-3f);
            // Renamed, the part follows; housekeeping leaves the program loaded while a part plays it.
            engine.setProgramName (0, 6, "Big pad");
            CHECK (trackProgram().slots[(size_t) padSlot].displayName() == "Upper: Big pad");
            pump (900);
            CHECK (engine.isProgramLoaded (0, 6));
            // Nothing names it any more: it is let go like any idle program.
            engine.removeSlot (track, trackProg, padSlot);
            pump (900);
            CHECK (! engine.isProgramLoaded (0, 6));
        }

        // Nothing plays the part: silence.
        engine.assignPart (track, trackProg, 1, -1);
        CHECK (engine.partInstrument (track, trackProg, 1) < 0);
        const float nobody = playAndStop (24);
        std::printf ("     no instrument on channel 1: %.5f\n", nobody);
        CHECK (nobody < 1e-3f);

        if (gmAvailable)
        {
            // Back on the GM player, with its mixer: volume 0 or mute silences the part; an instrument of our choosing still sounds.
            engine.assignPart (track, trackProg, 1, 0);
            auto quietWith = [&] (SlotDef::GmChannel c) { engine.setGmChannel (track, trackProg, 0, 1, c); return playAndStop (24); };
            SlotDef::GmChannel atZeroC; atZeroC.volume = 0;
            SlotDef::GmChannel mutedC; mutedC.muted = true;
            SlotDef::GmChannel stringsC; stringsC.program = 48;          // String Ensemble 1, over the file's
            const float atZero = quietWith (atZeroC), muted = quietWith (mutedC), strings = quietWith (stringsC), asFile = quietWith ({});
            std::printf ("     GM mixer: volume 0 %.4f, muted %.4f, strings %.3f, as the file has it %.3f\n", atZero, muted, strings, asFile);
            CHECK (atZero < 3e-3f && muted < 3e-3f && strings > 0.005f && asFile > 0.01f);
            engine.setGmChannel (track, trackProg, 0, 1, stringsC);

            // Saved and read back: the track input, the song's program there, the GM mixer and the keyboard instrument.
            TemporaryFile gf (".stagerig.json");
            CHECK (engine.captureSetup().saveToFile (gf.getFile()).wasOk());
            Setup gback;
            CHECK (Setup::loadFromFile (gf.getFile(), gback).wasOk());
            CHECK (gback.trackInput() == track && gback.songs[(size_t) midiSong].programFor (track) == trackProg);
            const auto& tp = gback.inputs[(size_t) track].programs[(size_t) trackProg];
            CHECK (tp.slots.size() == 2 && tp.slots[0].isGm() && tp.slots[0].gmChannels[0].program == 48 && tp.slots[0].hears (1)
                   && tp.slots[1].isInput() && tp.slots[1].targetInput == 0 && ! tp.slots[1].hears (1) && tp.slots[1].listensToText() == "No channel");
            CHECK (gback.songs[(size_t) midiSong].stems[0].isMidi() && ! gback.songs[(size_t) midiSong].stems[0].hasLegacyRoutes);

            // The editor's file round trip: a note added and saved, the first version kept, the tempo and program change intact.
            {
                const auto f = folder.getChildFile ("part.mid");
                MidiDoc doc;
                String err;
                CHECK (doc.load (f, err));
                CHECK (doc.notes.size() == 1 && doc.notes[0].key == 60 && doc.firstProgram (1) == 5);
                const int before = (int) doc.notes.size();
                MidiDoc::Note n; n.channel = 1; n.key = 67; n.velocity = 90; n.start = 960; n.end = 1920; n.track = doc.trackFor (1);
                doc.notes.push_back (n);
                doc.notes[0].velocity = 55;
                CHECK (doc.save (f, err));
                CHECK (f.getSiblingFile ("part.orig.mid").existsAsFile());
                MidiDoc back;
                CHECK (back.load (f, err));
                bool found = false;
                for (auto& x : back.notes) if (x.key == 67 && x.start == 960 && x.end == 1920 && x.velocity == 90) found = true;
                CHECK ((int) back.notes.size() == before + 1 && found && back.firstProgram (1) == 5);
                CHECK (std::abs (back.tickToSeconds (960) - 0.5) < 1e-6);          // 120 bpm: a quarter is half a second
                engine.reloadTrack (midiSong);
                CHECK (waitReady());
                std::printf ("     edited MIDI file: %d notes, length %.2f s\n", (int) back.notes.size(), engine.getTrackLength());
            }
        }

        // --- one keyboard layering the other, both ways: a layer, never an echo -------------------
        {
            engine.selectProgram (0, 0); settle();                      // Upper on its synth
            engine.clearProgram (1, 7);
            engine.selectProgram (1, 7); settle();
            CHECK (engine.addInputSlot (1, 7, 0));                      // Lower's program plays Upper's sounds
            CHECK (engine.addInputSlot (0, 0, 1));                      // and Upper's plays Lower's: a cycle
            const int upperSlots = (int) engine.getSetup().inputs[0].programs[0].slots.size();
            CHECK (engine.isPluginAlive (1, 7, 0) && engine.getSetup().inputs[1].programs[7].slots[0].displayName() == "Upper's sounds");
            engine.injectMidi (1, MidiMessage::noteOn (2, 64, (uint8) 100));   // played on Lower
            const float layered = levels (8)[0];
            std::printf ("     Lower's note through Upper's sounds: %.3f\n", layered);
            CHECK (layered > 0.01f);
            engine.injectMidi (1, MidiMessage::noteOff (2, 64));
            levels (120);
            const float quiet = levels (8)[0];
            std::printf ("     after letting go, with both keyboards layering each other: %.5f\n", quiet);
            CHECK (quiet < 1e-3f);
            engine.removeSlot (0, 0, upperSlots - 1);
            engine.clearProgram (1, 7);
        }

        // Removing an input: every song's programs shift with the inputs that are left.
        {
            const int extra = engine.addInput ("Extra");
            const int shifted = engine.addSong ("Shifted");
            engine.setSongProgram (shifted, 0, 3);
            engine.setSongProgram (shifted, extra, 9);
            CHECK (engine.getSongs()[(size_t) shifted].programFor (extra) == 9);
            engine.removeInput (extra);
            CHECK (engine.getSongs()[(size_t) shifted].programFor (0) == 3 && (int) engine.getSongs()[(size_t) shifted].programs.size() == extra);
            engine.removeSong (shifted);
        }

        // A song with no stems arms nothing.
        const int bare = engine.addSong ("No track");
        engine.selectSong (bare);
        CHECK (! engine.hasTrack());

        // Round trip through the setup file.
        TemporaryFile tf (".stagerig.json");
        CHECK (engine.captureSetup().saveToFile (tf.getFile()).wasOk());
        Setup back;
        CHECK (Setup::loadFromFile (tf.getFile(), back).wasOk());
        CHECK (back.songs[(size_t) song].stems.size() == 1 && back.songs[(size_t) song].stems[0].file == "stem.wav");
        CHECK (back.songs[(size_t) song].countInBeats == 2 && back.songs[(size_t) song].countInOutput == click);
        engine.removeOutput (click);
    }

    // --- routing: per-slot receive channel, and an external instrument on a MIDI port ----------
    {
        struct Catcher : public MidiInputCallback
        {
            std::mutex m;
            std::vector<MidiMessage> got;
            void handleIncomingMidiMessage (MidiInput*, const MidiMessage& msg) override { std::lock_guard<std::mutex> l (m); got.push_back (msg); }
            std::vector<MidiMessage> take() { std::lock_guard<std::mutex> l (m); auto r = got; got.clear(); return r; }
        } catcher;
        /* ALSA's Midi Through port gives back whatever is sent to it: send to
           its output side, listen on its input side. (A virtual port of our
           own would do, but JUCE does not list a process's own ports.) */
        MidiDeviceInfo port;
        std::unique_ptr<MidiInput> virt;
        for (const auto& d : MidiOutput::getAvailableDevices())
            if (d.name.containsIgnoreCase ("Midi Through")) port = d;
        if (port.identifier.isNotEmpty())
            for (const auto& d : MidiInput::getAvailableDevices())
                if (d.name == port.name) { virt = MidiInput::openDevice (d.identifier, &catcher); break; }
        if (virt == nullptr) port = {};
        else { virt->start(); std::printf ("     looping through %s (%s)\n", port.name.toRawUTF8(), port.identifier.toRawUTF8()); }
        if (port.identifier.isEmpty())
        {
            std::printf ("     skip external-instrument check (no Midi Through port here)\n");
        }
        else
        {
            const int oldChannel = engine.getSetup().inputs[0].channel;
            engine.setInputChannel (0, 0);                                   // Omni: the slot sorts the channels
            engine.clearProgram (0, 90);
            engine.selectProgram (0, 90); settle();
            CHECK (engine.addExternalSlot (0, 90, port));
            CHECK (engine.isPluginAlive (0, 90, 0));
            engine.setSlotPatch (0, 90, 0, 0, 3, 17);                        // playing: sent at once
            auto renderAndWait = [&] { AudioBuffer<float> b (2, kBlock); for (int i = 0; i < 6; ++i) { b.clear(); engine.renderBlockForTesting (b.getArrayOfWritePointers(), 2, kBlock); } Thread::sleep (150); return catcher.take(); };
            auto has = [] (const std::vector<MidiMessage>& v, std::function<bool (const MidiMessage&)> f) { for (auto& m : v) if (f (m)) return true; return false; };

            auto patch = renderAndWait();
            CHECK (has (patch, [] (const MidiMessage& m) { return m.isProgramChange() && m.getProgramChangeNumber() == 17 && m.getChannel() == 1; }));
            CHECK (has (patch, [] (const MidiMessage& m) { return m.isController() && m.getControllerNumber() == 32 && m.getControllerValue() == 3; }));

            engine.injectMidi (0, MidiMessage::noteOn (5, 64, (uint8) 90));   // plays on channel 1, as the slot is set
            auto notes = renderAndWait();
            std::printf ("     external instrument received %d messages\n", (int) notes.size());
            CHECK (has (notes, [] (const MidiMessage& m) { return m.isNoteOn() && m.getNoteNumber() == 64 && m.getChannel() == 1; }));
            engine.injectMidi (0, MidiMessage::noteOff (5, 64));
            renderAndWait();

            // Listens to channel 2 only.
            engine.setSlotListensTo (0, 90, 0, 1 << 1);
            engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 90));
            CHECK (! has (renderAndWait(), [] (const MidiMessage& m) { return m.isNoteOn(); }));
            engine.injectMidi (0, MidiMessage::noteOn (2, 62, (uint8) 90));
            CHECK (has (renderAndWait(), [] (const MidiMessage& m) { return m.isNoteOn() && m.getNoteNumber() == 62; }));

            // PANIC reaches the hardware too.
            engine.panic();
            CHECK (has (renderAndWait(), [] (const MidiMessage& m) { return m.isAllNotesOff() || (m.isController() && m.getControllerNumber() == 123); }));

            // Its sound back through an interface input joins the program's mix.
            engine.setSlotReturn (0, 90, 0, 0, -1);
            AudioBuffer<float> tone (1, kBlock), outBuf (2, kBlock);
            for (int i = 0; i < kBlock; ++i) tone.setSample (0, i, 0.4f * (float) std::sin (MathConstants<double>::twoPi * 330.0 * i / 48000.0));
            float back = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                outBuf.clear();
                engine.renderBlockForTesting (tone.getArrayOfReadPointers(), 1, outBuf.getArrayOfWritePointers(), 2, kBlock);
                back = jmax (back, outBuf.getMagnitude (0, 0, kBlock));
            }
            std::printf ("     audio return in the mix: %.3f (input peak 0.4)\n", back);
            CHECK (std::abs (back - 0.4f) < 0.05f);

            // Saved and read back.
            TemporaryFile ef (".stagerig.json");
            CHECK (engine.captureSetup().saveToFile (ef.getFile()).wasOk());
            Setup back2;
            CHECK (Setup::loadFromFile (ef.getFile(), back2).wasOk());
            const auto& sd = back2.inputs[0].programs[90].slots[0];
            CHECK (sd.isExternal() && sd.midiOutIdentifier == port.identifier && sd.patch == 17 && sd.bankLsb == 3
                   && sd.hears (2) && ! sd.hears (1) && sd.listensToText() == "Channel 2" && sd.returnLeft == 0 && sd.returnRight == -1);

            engine.clearProgram (0, 90);

            // --- a slot's MIDI chain: a processor plugin and a send, before the sound ---
            perf::ClapPluginFormat clapFmt;
            OwnedArray<PluginDescription> found;
            clapFmt.findAllTypesForFile (found, STAGERIG_TEST_NOTE_EFFECT);
            CHECK (found.size() == 1);
            if (found.size() == 1)
            {
                const int midiChain = ProgramDef::midiChainOf (0);
                engine.clearProgram (0, 91);
                engine.selectProgram (0, 91); settle();
                CHECK (engine.addExternalSlot (0, 91, port));               // the sound: plays on channel 1
                String fxErr;
                CHECK (engine.addEffect (0, 91, midiChain, *found[0], fxErr));
                settle();
                auto* octave = engine.getPlugin (0, 91, midiChain, 0);
                CHECK (octave != nullptr && octave->isAlive());
                renderAndWait();

                auto noteOn = [] (int n, int ch) { return [n, ch] (const MidiMessage& m) { return m.isNoteOn() && m.getNoteNumber() == n && m.getChannel() == ch; }; };
                auto play = [&] (int note) { engine.injectMidi (0, MidiMessage::noteOn (1, note, (uint8) 90)); auto got = renderAndWait();
                                             engine.injectMidi (0, MidiMessage::noteOff (1, note)); renderAndWait(); return got; };

                // Through the processor: the sound hears the octave, not the key.
                auto got = play (60);
                std::printf ("     MIDI processor -> external: %d messages\n", (int) got.size());
                CHECK (has (got, noteOn (72, 1)));
                CHECK (! has (got, noteOn (60, 1)));

                // Bypassed, the notes go past it.
                engine.setEffectBypassed (0, 91, midiChain, 0, true);
                got = play (60);
                CHECK (has (got, noteOn (60, 1)) && ! has (got, noteOn (72, 1)));
                engine.setEffectBypassed (0, 91, midiChain, 0, false);

                // A send after it gets the octave too, on its own channel; the sound still plays.
                CHECK (engine.addMidiSend (0, 91, 0, port));
                engine.setMidiSendChannel (0, 91, 0, 1, 5);
                CHECK (engine.isPluginAlive (0, 91, midiChain, 1));
                got = play (62);
                CHECK (has (got, noteOn (74, 5)) && has (got, noteOn (74, 1)));

                // Moved ahead of the processor, the send hears the key as played.
                engine.moveEffect (0, 91, midiChain, 1, 0);
                got = play (62);
                CHECK (has (got, noteOn (62, 5)) && has (got, noteOn (74, 1)) && ! has (got, noteOn (74, 5)));

                // The send recalls a patch on its channel.
                engine.setMidiSendPatch (0, 91, 0, 0, -1, -1, 9);
                got = renderAndWait();
                CHECK (has (got, [] (const MidiMessage& m) { return m.isProgramChange() && m.getProgramChangeNumber() == 9 && m.getChannel() == 5; }));

                // PANIC gets past the processor to the sound.
                engine.panic();
                CHECK (has (renderAndWait(), [] (const MidiMessage& m) { return m.getChannel() == 1 && (m.isAllNotesOff() || (m.isController() && m.getControllerNumber() == 123)); }));

                /* The transpose shifts the keys on the way in -- the send hears
                   62, the sound 74 through the octave -- and a note held across
                   a change is let go at the key it started on. */
                engine.setTranspose (2);
                engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 90));
                got = renderAndWait();
                CHECK (has (got, noteOn (62, 5)) && has (got, noteOn (74, 1)) && ! has (got, noteOn (60, 5)));
                engine.setTranspose (0);
                engine.injectMidi (0, MidiMessage::noteOff (1, 60));
                got = renderAndWait();
                CHECK (has (got, [] (const MidiMessage& m) { return m.isNoteOff() && m.getNoteNumber() == 62 && m.getChannel() == 5; }));
                CHECK (has (got, [] (const MidiMessage& m) { return m.isNoteOff() && m.getNoteNumber() == 74 && m.getChannel() == 1; }));
                CHECK (engine.getTranspose() == 0);

                TemporaryFile cf (".stagerig.json");
                CHECK (engine.captureSetup().saveToFile (cf.getFile()).wasOk());
                Setup back3;
                CHECK (Setup::loadFromFile (cf.getFile(), back3).wasOk());
                const auto& chainBack = back3.inputs[0].programs[91].slots[0].midiEffects;
                CHECK (chainBack.size() == 2 && chainBack[0].isSend() && chainBack[0].channel == 5 && chainBack[0].patch == 9
                       && chainBack[0].portIdentifier == port.identifier && ! chainBack[1].isSend()
                       && chainBack[1].plugin.name == found[0]->name);

                // Removing the slot takes its chain with it.
                engine.removeSlot (0, 91, 0);
                CHECK (engine.getPlugin (0, 91, midiChain, 0) == nullptr);
                engine.clearProgram (0, 91);
            }
            // --- a MIDI file part to a MIDI device: an instrument in the track's program listens to the file's channel ---
            {
                const auto folder = engine.getChartsFolder();
                folder.createDirectory();
                MidiMessageSequence seq;
                seq.addEvent (MidiMessage::noteOn  (5, 67, (uint8) 100), 0);      // the file's channel 5
                seq.addEvent (MidiMessage::noteOff (5, 67), 960 * 2);
                seq.addEvent (MidiMessage::programChange (5, 40), 5);             // the file's idea of a sound: not the synth's business
                seq.updateMatchedPairs();
                MidiFile mf;
                mf.setTicksPerQuarterNote (960);
                mf.addTrack (seq);
                const auto f = folder.getChildFile ("strings.mid");
                f.deleteFile();
                { FileOutputStream os (f); CHECK (os.openedOk() && mf.writeTo (os)); }

                const int song = engine.addSong ("Strings to the synth");
                StemDef st; st.file = "strings.mid";
                engine.addSongStem (song, st);
                const int track = engine.getTrackInput();
                const int tp = engine.getSongs()[(size_t) song].programFor (track);
                CHECK (track >= 0 && tp >= 0);
                CHECK (engine.addExternalSlot (track, tp, port));                 // slot 1; slot 0 is the GM player
                engine.setSlotOutChannel (track, tp, 1, 3);                       // the synth listens on 3
                engine.assignPart (track, tp, 5, 1);                              // the file's channel 5 is its part
                engine.selectSong (song);
                for (int i = 0; i < 400 && ! engine.isTrackReady(); ++i) pump (10);
                CHECK (engine.isTrackReady());
                settle();
                CHECK (engine.getSetup().inputs[(size_t) track].currentProgram == tp);
                renderAndWait();
                engine.startTrack();
                auto got = renderAndWait();
                std::printf ("     MIDI file channel 5 to the synth on channel 3: %d messages\n", (int) got.size());
                CHECK (has (got, [] (const MidiMessage& m) { return m.isNoteOn() && m.getNoteNumber() == 67 && m.getChannel() == 3; }));
                CHECK (! has (got, [] (const MidiMessage& m) { return m.isProgramChange(); }));
                engine.stopTrack();
                renderAndWait(); pump (50);

                TemporaryFile sf (".stagerig.json");
                CHECK (engine.captureSetup().saveToFile (sf.getFile()).wasOk());
                Setup back4;
                CHECK (Setup::loadFromFile (sf.getFile(), back4).wasOk());
                const auto& tpd = back4.inputs[(size_t) track].programs[(size_t) tp];
                CHECK (tpd.slots.size() == 2 && tpd.slots[1].isExternal() && tpd.slots[1].hears (5) && ! tpd.slots[1].hears (1)
                       && tpd.slots[0].isGm() && ! tpd.slots[0].hears (5) && tpd.slots[0].hears (1)
                       && back4.songs[(size_t) song].programFor (track) == tp);
                engine.removeSong (song);
            }
            engine.setInputChannel (0, oldChannel);
            virt->stop();
        }
    }

    // CLAP has to be loadable by the synchronous API, because that is the one
    // the out-of-process host calls. A format claiming it needs an unblocked
    // message thread is refused outright by JUCE with "cannot be instantiated
    // synchronously", which silently broke every CLAP plugin once already.
    {
        perf::ClapPluginFormat clap;
        PluginDescription anyClap;
        anyClap.pluginFormatName = perf::ClapPluginFormat::getFormatName();
        CHECK (! clap.requiresUnblockedMessageThreadDuringCreation (anyClap));

        // The format must be registered where the host looks for it, by name.
        AudioPluginFormatManager fm;
        addDefaultFormatsToManager (fm);
        fm.addFormat (new perf::ClapPluginFormat());
        bool foundClap = false;
        for (auto* f : fm.getFormats())
            if (f->getName() == perf::ClapPluginFormat::getFormatName())
                foundClap = true;
        CHECK (foundClap);

        // An identifier carries both path and plugin id, since one bundle can
        // hold several plugins; round-tripping it must not lose either.
        const auto id = perf::ClapPluginFormat::makeIdentifier ("/a/b/Thing.clap", "com.x.thing");
        CHECK (perf::ClapPluginFormat::pathFromIdentifier (id) == "/a/b/Thing.clap");
        CHECK (perf::ClapPluginFormat::pluginIdFromIdentifier (id) == "com.x.thing");
    }

    std::printf (failures == 0 ? "EngineTest: all checks passed\n" : "EngineTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
