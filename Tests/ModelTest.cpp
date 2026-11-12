// Round-trip test for the StageRig setup file format.
#include "Model.h"
#include "ProgramMap.h"
#include "MappingSuggestions.h"
#include <juce_events/juce_events.h>
#include <cstdio>
#include "ClapPluginFormat.h"
#include "ControlMaps.h"

using namespace perf;

static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    Setup s = Setup::makeDefault();
    // References into s.inputs are held while a third input is added below: room for it first.
    s.inputs.reserve (4);
    s.preloadAllPrograms = true;
    s.releaseTailSeconds = 2.5;
    s.holdAcrossChange = false;

    auto& upper = s.inputs[0];
    upper.midiDeviceIdentifier = "alsa:24:0";
    upper.midiDeviceName = "Nord Stage";
    upper.channel = 1;
    upper.currentProgram = 5;

    upper.programChangeChannel = 16;          // a workstation's global channel
    upper.midi1Only = true;                    // a MIDI 2.0 keyboard heard as MIDI 1.0
    {
        SlotDef ext; ext.kind = SlotDef::Kind::external;
        ext.midiOutIdentifier = "20-0"; ext.midiOutName = "Montage"; ext.outChannel = 3; ext.listensTo = 1 << 1;
        ext.bankMsb = 63; ext.bankLsb = 1; ext.patch = 9; ext.returnLeft = 2; ext.returnRight = 3;
        EffectDef arp; arp.plugin.name = "Arp"; arp.plugin.pluginFormatName = "CLAP"; arp.plugin.fileOrIdentifier = "/x/arp.clap|a";
        EffectDef send; send.kind = EffectDef::Kind::midiSend; send.portIdentifier = "24-0"; send.portName = "MicroFreak";
        send.channel = 4; send.patch = 12; send.bypassed = true;
        ext.midiEffects = { arp, send };
        upper.programs[7].slots.push_back (ext);

        // The built-in MIDI effects, with their settings, on a GM instrument in program 9.
        SlotDef gm; gm.kind = SlotDef::Kind::gm;
        EffectDef arpB; arpB.kind = EffectDef::Kind::arpeggio; arpB.arpRate = 3; arpB.arpMode = 2; arpB.arpOctaves = 2; arpB.arpGate = 50; arpB.arpLatch = true;
        EffectDef ch; ch.kind = EffectDef::Kind::chord; ch.chordType = 4; ch.bypassed = true;
        EffectDef sc; sc.kind = EffectDef::Kind::scale; sc.scaleRoot = 9; sc.scaleMode = 1; sc.followSongKey = false;
        EffectDef run; run.kind = EffectDef::Kind::run; run.scaleRoot = 2; run.scaleMode = 3; run.runNotes = 7; run.runBeats = 2.0f; run.runDegree = 2; run.runDown = 1; run.runToKey = 1; run.runHoldLast = false;
        EffectDef band; band.kind = EffectDef::Kind::bigBand; band.scaleRoot = 5; band.bandChords[1].clear(); band.bandChords[7] = { -12, 0, 4, 10, 14 };
        EffectDef hum; hum.kind = EffectDef::Kind::humanize; hum.humanTimingMs = 25; hum.humanVelocity = 0;
        EffectDef phr; phr.kind = EffectDef::Kind::phrase; phr.phraseName = "lick"; phr.phraseBeats = 2.0f; phr.phraseRoot = 60; phr.phraseStop = true; phr.phraseLoop = true;
        phr.phraseNotes = { { 0.0f, 0.5f, 60, 100 }, { 0.5f, 0.25f, 62, 90 }, { 1.0f, 1.0f, 64, 110 } };
        gm.midiEffects = { arpB, ch, sc, run, band, hum, phr };
        gm.panelParams = { "cutoff", "7" };
        upper.programs[9].slots.push_back (gm);
    }

    OutputDef mon; mon.name = "Monitors"; mon.left = 2; mon.right = 3; mon.gainDb = -3.0f; mon.volumeCC = 7;
    s.outputs.push_back (mon);
    upper.output = 1;

    {
        SongDef sg; sg.name = "With a track"; sg.transpose = -2;
        sg.stems.push_back ({ "mix.wav", "Mix", 0, -3.0f });
        sg.stems.push_back ({ "click.wav", {}, 1, 0.0f });
        StemDef midiStem; midiStem.file = "bass.mid";
        sg.stems.push_back (midiStem);
        sg.programs = { 5, -1, -1, 0 };
        sg.countInBeats = 4; sg.countInOutput = 1;
        sg.click = true; sg.beatsPerBar = 3; sg.firstBeat = 0.25; sg.startSeconds = 1.79; sg.trackBpm = 117.4; sg.transposeTrack = true;
        sg.keyRoot = 2; sg.keyMode = 1;                                           // D natural minor
        sg.markers = { { "Intro", 0.0, true }, { "Verse", 8.5, false } };
        {
            ActionDef prog; prog.kind = ActionDef::Kind::program; prog.input = 1; prog.program = 7;
            ActionDef part; part.kind = ActionDef::Kind::part; part.channel = 10; part.on = false;
            ActionDef fx; fx.kind = ActionDef::Kind::effect; fx.input = 0; fx.chain = ProgramDef::midiChainOf (2); fx.effect = 1; fx.on = false;
            ActionDef cue; cue.kind = ActionDef::Kind::midi; cue.portIdentifier = "id:lights"; cue.portName = "Lights"; cue.midiType = ActionDef::MidiType::controller; cue.channel = 16; cue.data1 = 20; cue.data2 = 64;
            sg.markers[1].actions = { prog, part, fx, cue };
        }
        s.songs.push_back (sg);
        s.transportCC = 85;
        s.clockOut = { { "alsa:20:0", "TR-8S" }, { "alsa:24:0", "Minilogue" } };
    }

    InputDef guitar; guitar.name = "Guitar"; guitar.kind = InputDef::Kind::audio;
    guitar.audioLeft = 2; guitar.audioRight = -1;          // mono on the third open channel
    guitar.programs[3].inputGainDb = -6.0f;
    s.inputs.push_back (guitar);
    // Where the songs' MIDI files play: the backing-track input's program for this song.
    InputDef track; track.name = InputDef::trackName; track.kind = InputDef::Kind::track; track.channel = 0; track.respondToProgramChange = false;
    SlotDef gm; gm.kind = SlotDef::Kind::gm; gm.listensTo = SlotDef::kAllChannels & ~(1 << 1);
    gm.gmChannels[9].program = 8; gm.gmChannels[9].volume = 90; gm.gmChannels[0].muted = true;
    SlotDef keys; keys.kind = SlotDef::Kind::input; keys.targetInput = 1; keys.targetInputName = "Lower"; keys.listensTo = 1 << 1;
    SlotDef pad; pad.kind = SlotDef::Kind::input; pad.targetInput = 0; pad.targetInputName = "Upper"; pad.targetProgram = 4; pad.targetProgramName = "Big pad"; pad.listensTo = 1 << 4;
    track.programs[0].name = "With a track";
    track.programs[0].slots = { gm, keys, pad };
    s.inputs.push_back (track);

    auto& prog = upper.programs[5];
    prog.name = "Rhodes + Pad";

    SlotDef slot;
    slot.plugin.name = "Test Synth";
    slot.plugin.pluginFormatName = "VST3";
    slot.plugin.fileOrIdentifier = "/usr/lib/vst3/Test.vst3";
    slot.plugin.uniqueId = 0x1234;
    slot.state.append ("\x00\x01\x02\xff state", 9);
    slot.gainDb = -6.5f;
    slot.transpose = -12;
    slot.lowKey = 36; slot.highKey = 72;
    slot.outChannel = 3;
    slot.enabled = false;
    slot.lowVelocity = 40; slot.highVelocity = 100;
    slot.velocityCurve = 0.5f;
    slot.pan = -0.3f;
    prog.slots.push_back (slot);

    MappingDef m;
    m.source = MappingDef::Source::CC; m.number = 74;
    m.slot = 0; m.paramId = "cutoff"; m.paramName = "Cutoff";
    m.minValue = 0.2f; m.maxValue = 0.9f; m.passThrough = true;
    prog.mappings.push_back (m);
    MappingDef pb; pb.source = MappingDef::Source::PitchBend; pb.paramId = "7";
    prog.mappings.push_back (pb);

    EffectDef fx;
    fx.plugin.name = "Test Reverb"; fx.plugin.pluginFormatName = "LV2"; fx.plugin.fileOrIdentifier = "urn:test:reverb";
    fx.bypassed = true;
    fx.state.append ("fx", 2);
    prog.slots[0].effects.push_back (fx);
    EffectDef progFx = fx; progFx.plugin.name = "Test Limiter"; progFx.bypassed = false;
    prog.effects.push_back (progFx);
    MappingDef fxMap; fxMap.number = 11; fxMap.slot = -1; fxMap.effect = 0; fxMap.paramId = "threshold";
    prog.mappings.push_back (fxMap);

    // Name-only program (no slots) must survive too.
    upper.programs[9].name = "Empty but named";

    juce::TemporaryFile tmp (".stagerig.json");
    CHECK (s.saveToFile (tmp.getFile()).wasOk());

    Setup loaded;
    auto r = Setup::loadFromFile (tmp.getFile(), loaded);
    CHECK (r.wasOk());
    if (r.failed()) std::printf ("  %s\n", r.getErrorMessage().toRawUTF8());

    CHECK (loaded.preloadAllPrograms == true);
    CHECK (std::abs (loaded.releaseTailSeconds - 2.5) < 1e-9);
    CHECK (loaded.holdAcrossChange == false);
    CHECK (loaded.outputs.size() == 2 && loaded.outputs[1].name == "Monitors" && loaded.outputs[1].left == 2 && loaded.outputs[1].right == 3);
    CHECK (std::abs (loaded.outputs[1].gainDb + 3.0f) < 1e-6f && loaded.outputs[1].volumeCC == 7);
    CHECK (loaded.inputs[0].output == 1 && loaded.inputs[1].output == 0);
    CHECK (Setup::makeDefault().outputs.size() == 1 && Setup::makeDefault().outputs[0].isDefault());
    {
        const auto& ext = loaded.inputs[0].programs[7].slots[0];
        CHECK (ext.isExternal() && ext.midiOutIdentifier == "20-0" && ext.midiOutName == "Montage" && ext.outChannel == 3
               && ext.hears (2) && ! ext.hears (1) && ext.listensToText() == "Channel 2" && ext.bankMsb == 63 && ext.bankLsb == 1 && ext.patch == 9
               && ext.returnLeft == 2 && ext.returnRight == 3 && ext.displayName() == "Montage (MIDI)");
        CHECK (! loaded.inputs[0].programs[5].slots[0].isExternal() && loaded.inputs[0].programs[5].slots[0].hearsAll());
        CHECK (ext.midiEffects.size() == 2 && ! ext.midiEffects[0].isSend() && ext.midiEffects[0].plugin.name == "Arp"
               && ext.midiEffects[1].isSend() && ext.midiEffects[1].portIdentifier == "24-0" && ext.midiEffects[1].portName == "MicroFreak"
               && ext.midiEffects[1].channel == 4 && ext.midiEffects[1].patch == 12 && ext.midiEffects[1].bankMsb == -1 && ext.midiEffects[1].bypassed);
        CHECK (loaded.inputs[0].programs[5].slots[0].midiEffects.empty());
        // The MIDI chain is addressed through the slot number.
        CHECK (loaded.inputs[0].programs[7].chainFor (ProgramDef::midiChainOf (0)) == &ext.midiEffects);
        CHECK (ProgramDef::slotOfChain (ProgramDef::midiChainOf (3)) == 3 && ! ProgramDef::isMidiChain (3));
    }
    CHECK (loaded.songs.size() == 1 && loaded.songs[0].stems.size() == 3);
    CHECK (loaded.songs[0].transpose == -2);
    {
        CHECK (loaded.songs[0].stems[2].isMidi() && ! loaded.songs[0].stems[2].hasLegacyRoutes);
        CHECK (loaded.trackInput() == 3 && loaded.inputs[3].isTrack() && loaded.inputs[3].channel == 0 && ! loaded.inputs[3].respondToProgramChange);
        CHECK (loaded.songs[0].programFor (3) == 0 && loaded.songs[0].programFor (1) == -1);
        const auto& tp = loaded.inputs[3].programs[0];
        CHECK (tp.name == "With a track" && tp.slots.size() == 3);
        CHECK (tp.slots[2].isInput() && tp.slots[2].targetInput == 0 && tp.slots[2].targetProgram == 4 && tp.slots[2].targetProgramName == "Big pad"
               && tp.slots[2].displayName() == "Upper: Big pad" && tp.slots[2].listensToText() == "Channel 5" && tp.slots[1].targetProgram == -1);
        CHECK (tp.slots[0].isGm() && ! tp.slots[0].hears (2) && tp.slots[0].hears (1) && tp.slots[0].hears (16)
               && tp.slots[0].gmChannels[9].program == 8 && tp.slots[0].gmChannels[9].volume == 90 && tp.slots[0].gmChannels[9].pan == -1
               && tp.slots[0].gmChannels[0].muted && tp.slots[0].gmChannels[1].isDefault() && tp.slots[0].displayName() == "General MIDI player");
        CHECK (tp.slots[1].isInput() && tp.slots[1].targetInput == 1 && tp.slots[1].targetInputName == "Lower" && tp.slots[1].hears (2)
               && ! tp.slots[1].hears (1) && tp.slots[1].displayName() == "Lower's sounds" && tp.slots[1].listensToText() == "Channel 2");

        /* Setups from before the backing track was an input: a MIDI file
           routed through a keyboard (all of it, or one channel), or by the
           short-lived per-channel table. They come back as a track program. */
        const auto oldOne = Setup::fromVar (juce::JSON::parse (R"({"format":"stagerig-setup",
            "inputs":[{"name":"Upper","channel":1},{"name":"Lower","channel":2}],
            "songs":[{"name":"Old","programs":[3,-1],"stems":[{"file":"a.mid","output":0,"input":1,"midiChannel":3}]}]})"));
        CHECK (oldOne.trackInput() == 2 && oldOne.songs[0].programFor (2) == 0);
        {
            const auto& p = oldOne.inputs[2].programs[0];
            CHECK (p.name == "Old" && p.slots.size() == 1 && p.slots[0].isInput() && p.slots[0].targetInput == 1
                   && p.slots[0].targetInputName == "Lower" && p.slots[0].listensToText() == "Channel 3");
            CHECK (! oldOne.songs[0].stems[0].hasLegacyRoutes);
        }
        const auto oldTable = Setup::fromVar (juce::JSON::parse (R"({"format":"stagerig-setup",
            "inputs":[{"name":"Upper","channel":1},{"name":"Lower","channel":2}],
            "songs":[{"name":"Table","programs":[],"stems":[{"file":"b.mid","output":0,"channels":[
               {"ch":2,"kind":"keys","input":0},{"ch":5,"kind":"slot","input":0,"slot":2,"program":7},{"ch":9,"kind":"mute"}]}]}]})"));
        {
            const auto& p = oldTable.inputs[2].programs[0];
            CHECK (p.slots.size() == 2 && p.slots[0].isGm() && p.slots[0].hears (1) && ! p.slots[0].hears (2) && ! p.slots[0].hears (5) && ! p.slots[0].hears (9)
                   && p.slots[1].isInput() && p.slots[1].targetInput == 0 && p.slots[1].hears (2) && p.slots[1].hears (5) && ! p.slots[1].hears (9));
            CHECK (oldTable.songs[0].programFor (2) == 0);
        }
        // A file with no routing at all plays on the GM player, and a slot's old single channel is a set of one.
        // The format version: written as this StageRig's, read back as the file says; old files are 1.
        CHECK ((int) Setup().toVar().getProperty ("version", 0) == Setup::formatVersion && Setup::formatVersion >= 2);
        CHECK (Setup::fromVar (juce::JSON::parse (R"({"format":"stagerig-setup"})")).loadedFormatVersion == 1);
        CHECK (Setup::fromVar (juce::JSON::parse (R"({"format":"stagerig-setup","version":9})")).loadedFormatVersion == 9);
        const auto plain = Setup::fromVar (juce::JSON::parse (R"({"format":"stagerig-setup","inputs":[{"name":"Upper","channel":1,
            "programs":[{"index":0,"slots":[{"receiveChannel":4}]}]}],"songs":[{"name":"Plain","programs":[],"stems":[{"file":"c.mid","output":0}]}]})"));
        CHECK (plain.trackInput() == 1 && plain.inputs[1].programs[0].slots.size() == 1 && plain.inputs[1].programs[0].slots[0].isGm()
               && plain.inputs[1].programs[0].slots[0].hearsAll());
        CHECK (plain.inputs[0].programs[0].slots.size() == 1 && plain.inputs[0].programs[0].slots[0].listensToText() == "Channel 4");
    }
    CHECK (! loaded.songs[0].stems[0].isMidi());
    CHECK (loaded.songs[0].stems[0].file == "mix.wav" && loaded.songs[0].stems[0].name == "Mix"
           && std::abs (loaded.songs[0].stems[0].gainDb + 3.0f) < 1e-6f && loaded.songs[0].stems[1].output == 1);
    CHECK (loaded.songs[0].stems[1].displayName() == "click");
    CHECK (loaded.songs[0].markers.size() == 2 && loaded.songs[0].markers[0].vamp && loaded.songs[0].markers[1].name == "Verse"
           && std::abs (loaded.songs[0].markers[1].seconds - 8.5) < 1e-9);
    CHECK (loaded.songs[0].countInBeats == 4 && loaded.songs[0].countInOutput == 1 && loaded.transportCC == 85);
    CHECK (loaded.clockOut.size() == 2 && loaded.clockOut[0].identifier == "alsa:20:0" && loaded.clockOut[1].name == "Minilogue");
    {
        const auto& fx = loaded.inputs[0].programs[9].slots[0].midiEffects;
        CHECK (fx.size() == 7 && fx[0].kind == EffectDef::Kind::arpeggio && fx[0].arpRate == 3 && fx[0].arpMode == 2 && fx[0].arpOctaves == 2 && fx[0].arpGate == 50 && fx[0].arpLatch);
        CHECK (fx[1].kind == EffectDef::Kind::chord && fx[1].chordType == 4 && fx[1].bypassed && fx[1].displayName() == "Chord");
        CHECK (fx[2].kind == EffectDef::Kind::scale && fx[2].scaleRoot == 9 && fx[2].scaleMode == 1 && fx[2].isBuiltIn() && ! fx[2].isSend());
        CHECK (! fx[2].followSongKey && fx[3].followSongKey && fx[0].followSongKey);
        CHECK (fx.size() == 7 && fx[3].kind == EffectDef::Kind::run && fx[3].scaleRoot == 2 && fx[3].scaleMode == 3 && fx[3].runNotes == 7
               && std::abs (fx[3].runBeats - 2.0f) < 1e-6f && fx[3].runDegree == 2 && fx[3].runDown == 1 && fx[3].runToKey == 1 && ! fx[3].runHoldLast && fx[3].displayName() == "Run");
        CHECK (fx[4].kind == EffectDef::Kind::bigBand && fx[4].scaleRoot == 5 && fx[4].followSongKey && fx[4].displayName() == "Big band" && fx[4].bandChords.size() == 12
               && fx[4].bandChords[1].empty() && fx[4].bandChords[7] == (std::vector<int> { -12, 0, 4, 10, 14 }) && fx[4].bandChords[0] == (std::vector<int> { 0, 4, 7, 9 }));
        CHECK (loaded.inputs[0].programs[9].slots[0].panelParams == (std::vector<juce::String> { "cutoff", "7" }));
        CHECK (fx[5].kind == EffectDef::Kind::humanize && fx[5].humanTimingMs == 25 && fx[5].humanVelocity == 0 && fx[5].displayName() == "Humanize");
        CHECK (fx[6].kind == EffectDef::Kind::phrase && fx[6].phraseName == "lick" && std::abs (fx[6].phraseBeats - 2.0f) < 1e-6f && fx[6].phraseRoot == 60 && fx[6].phraseStop && fx[6].phraseLoop
               && fx[6].phraseNotes.size() == 3 && std::abs (fx[6].phraseNotes[1].beat - 0.5f) < 1e-6f && std::abs (fx[6].phraseNotes[1].length - 0.25f) < 1e-6f
               && fx[6].phraseNotes[2].note == 64 && fx[6].phraseNotes[2].velocity == 110 && fx[6].displayName() == "Phrase");
    }
    CHECK (loaded.songs[0].click && loaded.songs[0].beatsPerBar == 3 && std::abs (loaded.songs[0].firstBeat - 0.25) < 1e-9);
    CHECK (std::abs (loaded.songs[0].startSeconds - 1.79) < 1e-9);
    CHECK (std::abs (loaded.songs[0].trackBpm - 117.4) < 1e-9 && loaded.songs[0].transposeTrack);
    CHECK (loaded.songs[0].hasKey() && loaded.songs[0].keyRoot == 2 && loaded.songs[0].keyMode == 1);
    CHECK (! SongDef::fromVar (SongDef().toVar()).hasKey() && SongDef::fromVar (SongDef().toVar()).keyRoot == -1);
    {
        const auto& acts = loaded.songs[0].markers[1].actions;
        CHECK (loaded.songs[0].markers[0].actions.empty() && acts.size() == 4);
        CHECK (acts[0].kind == ActionDef::Kind::program && acts[0].input == 1 && acts[0].program == 7);
        CHECK (acts[1].kind == ActionDef::Kind::part && acts[1].channel == 10 && ! acts[1].on);
        CHECK (acts[2].kind == ActionDef::Kind::effect && acts[2].chain == ProgramDef::midiChainOf (2) && acts[2].effect == 1 && ! acts[2].on);
        CHECK (acts[3].kind == ActionDef::Kind::midi && acts[3].portName == "Lights" && acts[3].midiType == ActionDef::MidiType::controller
               && acts[3].channel == 16 && acts[3].data1 == 20 && acts[3].data2 == 64);
    }
    CHECK (loaded.inputs.size() == 4 && loaded.inputs[2].isAudio());
    CHECK (loaded.inputs[2].audioLeft == 2 && loaded.inputs[2].audioRight == -1);
    CHECK (std::abs (loaded.inputs[2].programs[3].inputGainDb + 6.0f) < 1e-6f);
    CHECK (! loaded.inputs[0].isAudio() && loaded.inputs[0].programs[5].inputGainDb == 0.0f);
    CHECK (loaded.inputs.size() == 4);
    CHECK (loaded.inputs[1].name == "Lower");
    CHECK (loaded.inputs[1].channel == 2);
    CHECK (loaded.inputs[0].programChangeChannel == 16);                       // a workstation's global channel
    CHECK (loaded.inputs[1].programChangeChannel == InputDef::pcChannelSameAsNotes);   // the default
    CHECK (loaded.inputs[0].midi1Only && ! loaded.inputs[1].midi1Only);

    auto& u = loaded.inputs[0];
    CHECK (u.name == "Upper");
    CHECK (u.midiDeviceIdentifier == "alsa:24:0");
    CHECK (u.midiDeviceName == "Nord Stage");
    CHECK (u.currentProgram == 5);
    CHECK (u.programs.size() == 128);
    CHECK (u.programs[9].name == "Empty but named");
    CHECK (u.programs[4].isEmpty() && u.programs[4].name.isEmpty());

    auto& p = u.programs[5];
    CHECK (p.name == "Rhodes + Pad");
    CHECK (p.slots.size() == 1);
    if (p.slots.size() == 1)
    {
        auto& ls = p.slots[0];
        CHECK (ls.plugin.name == "Test Synth");
        CHECK (ls.plugin.pluginFormatName == "VST3");
        CHECK (ls.plugin.fileOrIdentifier == "/usr/lib/vst3/Test.vst3");
        CHECK (ls.plugin.uniqueId == 0x1234);
        CHECK (ls.state == slot.state);
        CHECK (std::abs (ls.gainDb - (-6.5f)) < 1e-6f);
        CHECK (ls.transpose == -12);
        CHECK (ls.lowKey == 36 && ls.highKey == 72);
        CHECK (ls.outChannel == 3);
        CHECK (ls.enabled == false);
        CHECK (ls.lowVelocity == 40 && ls.highVelocity == 100);
        CHECK (std::abs (ls.velocityCurve - 0.5f) < 1e-6f);
        CHECK (std::abs (ls.pan - (-0.3f)) < 1e-6f);
    }
    CHECK (p.slots.size() == 1 && p.slots[0].effects.size() == 1);
    if (p.slots.size() == 1 && p.slots[0].effects.size() == 1)
    {
        auto& e = p.slots[0].effects[0];
        CHECK (e.plugin.name == "Test Reverb" && e.plugin.pluginFormatName == "LV2");
        CHECK (e.bypassed == true);
        CHECK (e.state == fx.state);
    }
    CHECK (p.effects.size() == 1);
    if (p.effects.size() == 1)
    {
        CHECK (p.effects[0].plugin.name == "Test Limiter");
        CHECK (p.effects[0].bypassed == false);
    }
    CHECK (p.mappings.size() == 3);
    if (p.mappings.size() == 3)
    {
        CHECK (p.mappings[0].effect == -1);          // default target: the instrument
        CHECK (p.mappings[2].slot == -1 && p.mappings[2].effect == 0 && p.mappings[2].paramId == "threshold");
        auto& lm = p.mappings[0];
        CHECK (lm.source == MappingDef::Source::CC && lm.number == 74);
        CHECK (lm.slot == 0 && lm.paramId == "cutoff" && lm.paramName == "Cutoff");
        CHECK (std::abs (lm.minValue - 0.2f) < 1e-6f && std::abs (lm.maxValue - 0.9f) < 1e-6f);
        CHECK (lm.passThrough == true);
        CHECK (p.mappings[1].source == MappingDef::Source::PitchBend);
        CHECK (p.mappings[1].sourceDescription() == "Pitch Bend");
    }

    // Garbage file is rejected cleanly.
    juce::TemporaryFile bad (".json");
    bad.getFile().replaceWithText ("{ \"format\": \"something-else\" }");
    Setup ignored;
    CHECK (Setup::loadFromFile (bad.getFile(), ignored).failed());
    CHECK (Setup::loadFromFile (juce::File ("/nonexistent/x.json"), ignored).failed());

    // A maker's sound and the file it came from (its knob pages are that sound's) are kept with the instrument.
    {
        Setup st = Setup::makeDefault();
        SlotDef sd;
        sd.plugin.name = "Kontakt 8";
        sd.soundName = "Alicia's Keys";
        sd.soundFile = "/sounds/Alicias Keys.nksf";
        st.inputs[0].programs[0].slots.push_back (sd);
        juce::TemporaryFile t (".json");
        CHECK (st.saveToFile (t.getFile()).wasOk());
        Setup back;
        CHECK (Setup::loadFromFile (t.getFile(), back).wasOk());
        CHECK (! back.inputs[0].programs[0].slots.empty() && back.inputs[0].programs[0].slots.back().soundFile == "/sounds/Alicias Keys.nksf");
        CHECK (! back.inputs[0].programs[0].slots.empty() && back.inputs[0].programs[0].slots.back().soundName == "Alicia's Keys");
    }

    // The save before stays as <name>.bak: a damaged file has a readable one beside it.
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("stagerig-modeltest-bak");
        dir.deleteRecursively();
        const auto f = dir.getChildFile ("rig.stagerig.json");
        Setup first = Setup::makeDefault();
        first.tempoBpm = 97.0;
        CHECK (first.saveToFile (f).wasOk());
        CHECK (! Setup::backupOf (f).existsAsFile());          // nothing before the first save
        Setup second = first;
        second.tempoBpm = 133.0;
        CHECK (second.saveToFile (f).wasOk());
        Setup back;
        CHECK (Setup::loadFromFile (f, back).wasOk() && back.tempoBpm == 133.0);
        CHECK (Setup::loadFromFile (Setup::backupOf (f), back).wasOk() && back.tempoBpm == 97.0);
        CHECK (f.replaceWithText ("{ \"format\": \"stagerig"));       // cut short, as by a full disk
        CHECK (Setup::loadFromFile (f, back).failed());
        CHECK (Setup::loadFromFile (Setup::backupOf (f), back).wasOk());
        // A damaged file is not moved over the good save before it.
        CHECK (f.replaceWithText ("{"));
        CHECK (second.saveToFile (f).wasOk());
        CHECK (Setup::loadFromFile (Setup::backupOf (f), back).wasOk());
        dir.deleteRecursively();
    }

    // Mapping templates: store, reload, remove.
    {
        juce::TemporaryFile tf (".json");
        MappingTemplates t (tf.getFile());
        CHECK (t.size() == 0);
        juce::PluginDescription d; d.name = "Synth"; d.pluginFormatName = "VST3"; d.fileOrIdentifier = "/x/Synth.vst3"; d.uniqueId = 42;
        MappingDef m1; m1.number = 74; m1.paramId = "cut"; m1.slot = 3; m1.effect = 2; m1.minValue = 0.2f;
        MappingDef m2; m2.source = MappingDef::Source::PitchBend; m2.paramId = "bend";
        t.set (d, { m1, m2 });
        CHECK (t.has (d) && t.size() == 1);
        MappingTemplates t2 (tf.getFile());
        CHECK (t2.has (d));
        auto got = t2.get (d);
        CHECK (got.size() == 2);
        if (got.size() == 2)
        {
            CHECK (got[0].number == 74 && got[0].paramId == "cut" && std::abs (got[0].minValue - 0.2f) < 1e-6f);
            CHECK (got[0].slot == 0 && got[0].effect == -1);      // targets are stripped
            CHECK (got[1].source == MappingDef::Source::PitchBend && got[1].paramId == "bend");
        }
        juce::PluginDescription other = d; other.uniqueId = 43;
        CHECK (! t2.has (other));
        t2.remove (d);
        CHECK (! t2.has (d));
        MappingTemplates t3 (tf.getFile());
        CHECK (! t3.has (d) && t3.size() == 0);
    }

    // --- printable program map -------------------------------------------------------
    {
        Setup pm = Setup::makeDefault();
        pm.inputs[0].name = "Upper"; pm.inputs[0].midiDeviceName = "Keystation"; pm.inputs[0].channel = 1;
        auto& p7 = pm.inputs[0].programs[7];
        p7.name = "Rhodes & Strings";
        SlotDef a; a.plugin.name = "Kontakt 8"; p7.slots.push_back (a);
        SlotDef b; b.plugin.name = "Calf Monosynth"; p7.slots.push_back (b);
        auto& p9 = pm.inputs[0].programs[9];
        p9.name = "Organ <loud & proud>";                       // must be HTML-escaped
        SlotDef c; c.plugin.name = "Hammond B-3X"; p9.slots.push_back (c);

        const auto html = ProgramMap::toHtml (pm, "My Rig");
        CHECK (html.startsWith ("<!DOCTYPE html>"));
        CHECK (html.contains ("007") && html.contains ("Rhodes &amp; Strings"));
        CHECK (html.contains ("Kontakt 8 + Calf Monosynth"));
        CHECK (html.contains ("Organ &lt;loud &amp; proud&gt;"));      // escaped, not raw
        CHECK (! html.contains ("Organ <loud"));
        CHECK (html.contains ("Keystation") && html.contains ("channel 1"));
        CHECK (html.contains ("009"));
        CHECK (! html.contains ("<td class=\"n\">001</td>"));        // empty programs are omitted
        CHECK (html.contains ("No programs yet."));                    // the second, empty input
        std::printf ("     program map: %d bytes\n", html.length());

        // STAGERIG_MAP_FROM=setup.json STAGERIG_MAP_TO=out.html renders a real setup,
        // for eyeballing the printed sheet.
        const auto from = juce::SystemStats::getEnvironmentVariable ("STAGERIG_MAP_FROM", {});
        const auto to   = juce::SystemStats::getEnvironmentVariable ("STAGERIG_MAP_TO", {});
        if (from.isNotEmpty() && to.isNotEmpty())
        {
            Setup real;
            if (Setup::loadFromFile (juce::File (from), real).wasOk())
            {
                juce::File (to).replaceWithText (ProgramMap::toHtml (real, juce::File (from).getFileNameWithoutExtension()));
                std::printf ("     rendered %s -> %s\n", from.toRawUTF8(), to.toRawUTF8());
            }
            else std::printf ("     could not load %s\n", from.toRawUTF8());
        }
    }

    // ---- program groups --------------------------------------------------------
    /* A group is a label, never a container: a program's number is fixed by MIDI
       Program Change, so grouping must not move or renumber anything. */
    {
        Setup g;
        g.inputs.resize (2);
        g.inputs[0].programs[3].name  = "Jimmy";
        g.inputs[0].programs[3].group = "Organs";
        g.inputs[0].programs[7].name  = "Strings 1";
        g.inputs[0].programs[7].group = "Strings";
        g.inputs[0].programs[9].name  = "Ungrouped";
        g.inputs[1].programs[3].name  = "Bass";
        g.inputs[1].programs[3].group = "Basses";

        const auto back = Setup::fromVar (g.toVar());
        CHECK (back.inputs[0].programs[3].group == "Organs");
        CHECK (back.inputs[0].programs[7].group == "Strings");
        CHECK (back.inputs[0].programs[9].group.isEmpty());
        CHECK (back.inputs[1].programs[3].group == "Basses");   // per input, not shared

        // The number is the identity: a group must never shift it.
        CHECK (back.inputs[0].programs[3].name == "Jimmy");
        CHECK (back.inputs[0].programs[7].name == "Strings 1");

        /* A setup saved before groups existed simply has no group key. Programs
           are stored sparsely with an explicit "index", so find the entry by
           that rather than by its position in the array. */
        auto older = g.toVar();
        if (auto* root = older.getDynamicObject())
            if (auto* ins = root->getProperty ("inputs").getArray())
                if (auto* in0 = (*ins)[0].getDynamicObject())
                    if (auto* progs = in0->getProperty ("programs").getArray())
                        for (auto& entry : *progs)
                            if (auto* po = entry.getDynamicObject())
                                if ((int) po->getProperty ("index") == 3)
                                    po->removeProperty ("group");
        CHECK (Setup::fromVar (older).inputs[0].programs[3].group.isEmpty());
        CHECK (Setup::fromVar (older).inputs[0].programs[3].name == "Jimmy");

        // The printed map groups without losing anything.
        const auto html = ProgramMap::toHtml (g, "Grouped");
        CHECK (html.contains ("Organs") && html.contains ("Strings"));
        CHECK (html.contains ("Jimmy") && html.contains ("Ungrouped"));
        CHECK (html.contains ("Other"));            // the ungrouped run is labelled
    }

    // ---- songs and sets -------------------------------------------------
    /* A song names one program per input plus a tempo; a set orders songs.
       What matters on disk is that a setup made before any of this existed
       still loads, and that deleting a song cannot leave a set pointing at
       the wrong one. */
    {
        Setup s;
        s.inputs.resize (2);
        s.inputs[0].name = "Upper";
        s.inputs[1].name = "Lower";

        SongDef a; a.name = "Oye Como Va"; a.programs = { 4, 1 }; a.tempoBpm = 116.0; a.notes = "Am";
        SongDef b; b.name = "Green Onions"; b.programs = { 2, -1 };       // Lower left alone
        SongDef c; c.name = "Encore";       c.programs = { 0, 0 };
        s.songs = { a, b, c };

        SetDef first; first.name = "First set"; first.songs = { 0, 1, 2 };
        SetDef enc;   enc.name   = "Encores";   enc.songs = { 2, 0 };      // a song may repeat
        s.sets = { first, enc };
        s.currentSet = 0; s.currentSongInSet = 1;
        s.nextSongCC = 80; s.prevSongCC = 81;

        const auto back = Setup::fromVar (s.toVar());
        CHECK (back.songs.size() == 3 && back.sets.size() == 2);
        CHECK (back.songs[0].name == "Oye Como Va");
        CHECK (back.songs[0].programFor (0) == 4 && back.songs[0].programFor (1) == 1);
        CHECK (std::abs (back.songs[0].tempoBpm - 116.0) < 0.001);
        CHECK (back.songs[0].notes == "Am");
        CHECK (back.songs[1].programFor (1) == -1);          // "leave that keyboard alone"
        CHECK (std::abs (back.songs[1].tempoBpm) < 0.001);   // no tempo of its own
        CHECK (back.sets[0].songs.size() == 3 && back.sets[1].songs == std::vector<int> ({ 2, 0 }));
        CHECK (back.currentSet == 0 && back.currentSongInSet == 1);
        CHECK (back.nextSongCC == 80 && back.prevSongCC == 81);

        // An input the song never knew about reads as "leave it alone".
        CHECK (back.songs[0].programFor (5) == -1);
        CHECK (back.songs[0].programFor (-1) == -1);

        /* A setup from before songs existed has no songs/sets keys at all, and
           must load as an ordinary setup rather than anything surprising. */
        Setup plain;
        plain.inputs.resize (1);
        auto older = plain.toVar();
        CHECK (older.getDynamicObject()->getProperty ("songs").isVoid());
        CHECK (older.getDynamicObject()->getProperty ("sets").isVoid());
        const auto loadedPlain = Setup::fromVar (older);
        CHECK (loadedPlain.songs.empty() && loadedPlain.sets.empty());
        CHECK (loadedPlain.currentSet == -1 && loadedPlain.currentSongInSet == -1);

        /* A hand-edited file pointing a set at a song that is not there must
           not survive into a gig. */
        auto broken = s.toVar();
        if (auto* root = broken.getDynamicObject())
        {
            if (auto* setsArr = root->getProperty ("sets").getArray())
                if (auto* s0 = (*setsArr)[0].getDynamicObject())
                    s0->setProperty ("songs", juce::Array<juce::var> { juce::var (0), juce::var (99), juce::var (-3) });
            root->setProperty ("currentSet", 7);
        }
        const auto fixed = Setup::fromVar (broken);
        CHECK (fixed.sets[0].songs == std::vector<int> ({ 0 }));   // 99 and -3 dropped
        CHECK (fixed.currentSet == -1 && fixed.currentSongInSet == -1);
    }

    // ---- CLAP identifiers ------------------------------------------------
    /* A CLAP bundle can hold several plugins, so a description has to name the
       plugin as well as the file. Get this wrong and a saved setup reopens the
       wrong plugin out of the bundle, or none. */
    {
        const auto id = perf::ClapPluginFormat::makeIdentifier ("/usr/lib/clap/Surge XT.clap",
                                                                "org.surge-synth-team.surge-xt");
        CHECK (perf::ClapPluginFormat::pathFromIdentifier (id) == "/usr/lib/clap/Surge XT.clap");
        CHECK (perf::ClapPluginFormat::pluginIdFromIdentifier (id) == "org.surge-synth-team.surge-xt");

        // A path with no id is still a usable path, for a bundle holding one plugin.
        const auto bare = perf::ClapPluginFormat::makeIdentifier ("/usr/lib/clap/Thing.clap", {});
        CHECK (bare == "/usr/lib/clap/Thing.clap");
        CHECK (perf::ClapPluginFormat::pathFromIdentifier (bare) == "/usr/lib/clap/Thing.clap");
        CHECK (perf::ClapPluginFormat::pluginIdFromIdentifier (bare).isEmpty());

        // Spaces in a path are ordinary: every FabFilter bundle has one.
        const auto spaced = perf::ClapPluginFormat::makeIdentifier ("/home/x/.clap/FabFilter Pro-Q 4.clap",
                                                                    "com.fabfilter.proq.4");
        CHECK (perf::ClapPluginFormat::pathFromIdentifier (spaced) == "/home/x/.clap/FabFilter Pro-Q 4.clap");
        CHECK (perf::ClapPluginFormat::pluginIdFromIdentifier (spaced) == "com.fabfilter.proq.4");
    }

    // ---- charts on a song -----------------------------------------------
    {
        SongDef a; a.name = "With charts";
        a.charts.push_back ({ "sheet.pdf", "Lead sheet" });
        a.charts.push_back ({ "words.cho", "" });
        const auto back = SongDef::fromVar (a.toVar());
        CHECK (back.charts.size() == 2);
        CHECK (back.charts[0].file == "sheet.pdf" && back.charts[0].title == "Lead sheet");
        CHECK (back.charts[1].file == "words.cho" && back.charts[1].displayName() == "words.cho");

        // A song without charts writes no key, so older setups stay byte-identical.
        SongDef plain; plain.name = "Plain";
        CHECK (! plain.toVar().getDynamicObject()->hasProperty ("charts"));

        // The folder sits beside the setup, named after it.
        CHECK (chartsFolderFor (juce::File ("/x/MySetup.stagerig.json")).getFullPathName() == "/x/MySetup.stagerig.charts");
        CHECK (chartsFolderFor (juce::File()) == juce::File());
    }

    // ---- controller setups ------------------------------------------------
    {
        ControlMap m;
        m.name = "Alicia's Keys"; m.pluginName = "Kontakt 8"; m.pluginManufacturer = "Native Instruments"; m.author = "Someone";
        ControlPage sound; sound.name = "Sound";
        sound.knobs.push_back ({ "1001", "Reverb", "Room" });
        sound.knobs.push_back ({ {}, "Tone", {} });
        sound.knobs.push_back ({ "1002", "Sustain", "Pedal", true });      // on or off, though the plugin gives a range
        m.pages.push_back (sound);
        m.pages.push_back ({ "Empty", {} });
        m.keys.push_back ({ 24, 35, 0xff8a00, "Articulations" });
        m.keys.push_back ({ 108, 100, 0x00b4ff, {} });      // backwards: put right on load

        ControlMap back; juce::String error;
        CHECK (ControlMap::fromVar (juce::JSON::parse (juce::JSON::toString (m.toVar())), back, error));
        CHECK (back.name == m.name && back.pluginName == "Kontakt 8" && back.pluginManufacturer == "Native Instruments" && back.author == "Someone");
        CHECK (back.pages.size() == 2 && back.pages[0].name == "Sound" && back.pages[0].knobs.size() == 3);
        CHECK (back.pages[0].knobs[2].isSwitch && ! back.pages[0].knobs[0].isSwitch && ! back.pages[0].knobs[1].isSwitch);
        CHECK (back.pages[0].knobs[0].paramId == "1001" && back.pages[0].knobs[0].paramName == "Reverb" && back.pages[0].knobs[0].label == "Room");
        CHECK (back.pages[0].knobs[1].paramId.isEmpty() && back.pages[0].knobs[1].paramName == "Tone");
        CHECK (back.keys.size() == 2 && back.keys[0].low == 24 && back.keys[0].high == 35 && back.keys[0].colour == 0xff8a00 && back.keys[0].label == "Articulations");
        CHECK (back.keys[1].low == 100 && back.keys[1].high == 108);
        CHECK (ControlMap::colourToText (0x00b4ff) == "#00b4ff" && ControlMap::colourFromText ("#FF8A00", 0) == 0xff8a00 && ControlMap::colourFromText ("red", 7) == 7);

        // Not a setup, a newer one, one without a plugin: refused, with a reason.
        CHECK (! ControlMap::fromVar (juce::JSON::parse ("{\"name\":\"x\"}"), back, error) && error.isNotEmpty());
        CHECK (! ControlMap::fromVar (juce::JSON::parse ("{\"format\":\"stagerig-controller-setup\",\"version\":99,\"plugin\":\"A\"}"), back, error) && error.containsIgnoreCase ("newer"));
        CHECK (! ControlMap::fromVar (juce::JSON::parse ("{\"format\":\"stagerig-controller-setup\",\"version\":1}"), back, error));
        // Nine knobs on a page: the ninth is dropped. No name: the plugin's.
        {
            juce::String nine = "{\"format\":\"stagerig-controller-setup\",\"version\":1,\"plugin\":\"Synth\",\"pages\":[{\"name\":\"P\",\"knobs\":[";
            for (int k = 0; k < 9; ++k) nine << (k ? "," : "") << "{\"param\":\"p" << k << "\"}";
            nine << "]}]}";
            CHECK (ControlMap::fromVar (juce::JSON::parse (nine), back, error) && back.pages[0].knobs.size() == 8 && back.name == "Synth");
        }

        // Which plugin it is for: the name, and the maker when both say.
        juce::PluginDescription k8; k8.name = "Kontakt 8"; k8.manufacturerName = "Native Instruments";
        juce::PluginDescription k8bare; k8bare.name = "kontakt 8";
        juce::PluginDescription other; other.name = "Kontakt 8"; other.manufacturerName = "Someone Else";
        CHECK (m.appliesTo (k8) && m.appliesTo (k8bare) && ! m.appliesTo (other));

        // Note names, as StageRig writes them: middle C is C3.
        CHECK (parseNoteName ("C3") == 60 && parseNoteName ("A-1") == 21 && parseNoteName ("A-2") == 9 && parseNoteName ("F#2") == 54 && parseNoteName ("Bb1") == 46);
        CHECK (parseNoteName ("c-2") == 0 && parseNoteName ("G8") == 127 && parseNoteName ("61") == 61);
        CHECK (parseNoteName ("") == -1 && parseNoteName ("H2") == -1 && parseNoteName ("C") == -1 && parseNoteName ("200") == -1 && parseNoteName ("G#8") == -1);

        // The library: a folder of files.
        juce::TemporaryFile tmpDir;
        const auto dir = tmpDir.getFile();
        dir.createDirectory();
        {
            ControlMapLibrary lib (dir);
            CHECK (lib.size() == 0 && lib.find (k8, {}) == nullptr);
            CHECK (lib.save (m).wasOk() && lib.size() == 1);
            CHECK (lib.fileFor (m).getFileName() == juce::File::createLegalFileName ("Kontakt 8 - Alicia's Keys") + ".stagerig-controls");
            ControlMap whole = m; whole.name = "Kontakt 8"; whole.keys.clear();
            CHECK (lib.save (whole).wasOk() && lib.size() == 2);
            CHECK (lib.fileFor (whole).getFileName() == "Kontakt 8.stagerig-controls");
            CHECK (lib.forPlugin (k8).size() == 2 && lib.forPlugin (other).empty());
            CHECK (lib.find (k8, "alicia's keys") != nullptr && lib.find (k8, "alicia's keys")->keys.size() == 2);
            CHECK (lib.find (k8, "Nothing like it") == nullptr && lib.find (k8, "-") == nullptr);
            CHECK (lib.find (k8, {}) != nullptr);          // no name: the first there is

            // Saved again: replaced, not doubled.
            ControlMap changed = m; changed.author = "Me";
            CHECK (lib.save (changed).wasOk() && lib.size() == 2 && lib.find (k8, "Alicia's Keys")->author == "Me");

            // A file that is not a setup sits in the folder unread.
            dir.getChildFile ("junk.stagerig-controls").replaceWithText ("not json");
            lib.reload();
            CHECK (lib.size() == 2);

            // Import: a copy into the folder; the same setup imported again replaces it.
            ControlMap shared; shared.name = "Lead"; shared.pluginName = "Vital";
            const auto sharedFile = dir.getSiblingFile ("from-a-friend.stagerig-controls");
            CHECK (shared.saveToFile (sharedFile).wasOk());
            ControlMap got;
            CHECK (lib.importFile (sharedFile, &got).wasOk() && got.name == "Lead" && lib.size() == 3);
            CHECK (lib.importFile (sharedFile).wasOk() && lib.size() == 3);
            CHECK (lib.importFile (dir.getChildFile ("junk.stagerig-controls")).failed());
            CHECK (lib.importFile (dir.getChildFile ("missing.stagerig-controls")).failed());
            sharedFile.deleteFile();

            CHECK (lib.remove (changed).wasOk() && lib.size() == 2 && lib.find (k8, "Alicia's Keys") == nullptr);
            CHECK (lib.remove (changed).failed());

            // Another library on the same folder sees what this one saved.
            ControlMapLibrary again (dir);
            CHECK (again.size() == 2);
        }
        dir.deleteRecursively();
    }

    // ---- NKS presets: a maker's knob pages -----------------------------------
    {
        // A little MessagePack, enough to write what an .nksf holds.
        using Bytes = std::vector<juce::uint8>;
        auto str = [] (const juce::String& t) { Bytes b; const auto u = t.toStdString(); b.push_back ((juce::uint8) (0xa0 | u.size())); b.insert (b.end(), u.begin(), u.end()); return b; };
        auto num = [] (juce::int64 v) { Bytes b; if (v >= 0 && v < 128) b.push_back ((juce::uint8) v); else { b.push_back (0xce); for (int i = 3; i >= 0; --i) b.push_back ((juce::uint8) (v >> (8 * i))); } return b; };
        auto map = [] (std::vector<std::pair<juce::String, Bytes>> kv)
        {
            Bytes b { (juce::uint8) (0x80 | kv.size()) };
            for (auto& [k, v] : kv) { const auto u = k.toStdString(); b.push_back ((juce::uint8) (0xa0 | u.size())); b.insert (b.end(), u.begin(), u.end()); b.insert (b.end(), v.begin(), v.end()); }
            return b;
        };
        auto arr = [] (std::vector<Bytes> items) { Bytes b { (juce::uint8) (0x90 | items.size()) }; for (auto& i : items) b.insert (b.end(), i.begin(), i.end()); return b; };
        auto knob = [&] (const juce::String& section, juce::int64 id, const juce::String& name)
        {
            std::vector<std::pair<juce::String, Bytes>> kv { { "id", num (id) }, { "name", str (name) } };
            if (section.isNotEmpty()) kv.push_back ({ "section", str (section) });
            return map (kv);
        };
        auto riff = [] (std::vector<std::pair<const char*, Bytes>> chunks)
        {
            Bytes body { 'N', 'I', 'K', 'S' };
            for (auto& [id, data] : chunks)
            {
                Bytes c { 1, 0, 0, 0 };
                c.insert (c.end(), data.begin(), data.end());
                body.insert (body.end(), id, id + 4);
                for (int i = 0; i < 4; ++i) body.push_back ((juce::uint8) (c.size() >> (8 * i)));
                body.insert (body.end(), c.begin(), c.end());
                if (c.size() & 1) body.push_back (0);
            }
            Bytes f { 'R', 'I', 'F', 'F' };
            for (int i = 0; i < 4; ++i) f.push_back ((juce::uint8) (body.size() >> (8 * i)));
            f.insert (f.end(), body.begin(), body.end());
            return f;
        };

        // Super 8's kind: VST2 numbers, short names, the section on the first knob of a run.
        const auto synthFile = riff ({
            { "NISI", map ({ { "name", str ("Sling Cave") }, { "vendor", str ("Native Instruments") }, { "bankchain", arr ({ str ("Super 8 R2") }) } }) },
            { "NICA", map ({ { "ni8", arr ({ arr ({ knob ("Filter", 2, "Cutoff"), knob ({}, 3, "Reso"), knob ("Filter Env", 4, "Attack"), knob ("Amp Env", 5, "Attack"),
                                                    map ({ { "name", str ("Unassigned") } }) }) }) } }) },
            { "PLID", map ({ { "VST.magic", num (1313428280) } }) } });
        NksPreset synth;
        CHECK (NksPreset::parse (synthFile.data(), synthFile.size(), synth).wasOk());
        CHECK (synth.name == "Sling Cave" && synth.vendor == "Native Instruments" && synth.product == "Super 8 R2" && ! synth.vst3Ids);
        CHECK (synth.pages.size() == 1 && synth.pages[0].name == "Filter, Filter Env, Amp Env" && synth.pages[0].knobs.size() == 5);
        CHECK (synth.pages[0].knobs[1].paramId == "3" && synth.pages[0].knobs[1].label == "Reso" && synth.pages[0].knobs[1].paramName == "Filter");   // the section goes on
        CHECK (synth.pages[0].knobs[4].isEmpty());                                                                                                   // unassigned
        CHECK (NksPreset::parse ("RIFF\0\0\0\0WAVE", 12, synth).failed());

        juce::PluginDescription super8; super8.name = "Super 8"; super8.manufacturerName = "Native Instruments GmbH"; super8.pluginFormatName = "VST3";
        juce::PluginDescription other8 = super8; other8.manufacturerName = "Someone Else";
        CHECK (synth.matches (super8) == 1 && synth.matches (other8) == 0);

        auto param = [] (int index, juce::String id, juce::String name) { ParamInfo p; p.index = index; p.id = id; p.name = name; return p; };
        // The VST3 version kept the numbers: the parameter at each is the one meant, whatever its name now.
        const ParamInfoList kept { param (0, "100", "Global: Volume"), param (1, "101", "PMod: Env Attack"), param (2, "102", "Filter: Cutoff Frequency"),
                                   param (3, "103", "Filter: Resonance"), param (4, "104", "Filter Env: Attack"), param (5, "105", "Amp Env: Attack") };
        {
            const auto m = controlMapFromNks (synth, super8, kept);
            CHECK (m.pages.size() == 1 && m.pages[0].knobs.size() == 4 && m.pluginName == "Super 8" && m.author == "Native Instruments");
            CHECK (m.pages[0].knobs[0].paramId == "102" && m.pages[0].knobs[0].label == "Cutoff");
            CHECK (m.pages[0].knobs[2].paramId == "104" && m.pages[0].knobs[3].paramId == "105");   // each Attack its own
        }
        // Renumbered: found by section and name instead, and a bare "Attack" never guessed among several.
        const ParamInfoList moved { param (0, "7", "Filter Env: Attack"), param (1, "8", "Cutoff"), param (2, "9", "Resonance"),
                                    param (3, "10", "Amp Env Attack"), param (4, "11", "PMod: Env Attack"), param (5, "12", "Something") };
        {
            const auto m = controlMapFromNks (synth, super8, moved);
            CHECK (m.pages.size() == 1 && m.pages[0].knobs.size() == 4);
            CHECK (m.pages[0].knobs[0].paramId == "8" && m.pages[0].knobs[1].paramId == "9");         // "Reso" is short for Resonance
            CHECK (m.pages[0].knobs[2].paramId == "7" && m.pages[0].knobs[3].paramId == "10");
            NksPreset bare = synth;
            for (auto& k : bare.pages[0].knobs) k.paramName = {};
            const auto b = controlMapFromNks (bare, super8, moved);
            CHECK (b.pages[0].knobs.size() == 2);      // which Attack? neither, rather than a wrong one
        }

        // SWAM's kind: VST3 ids, and the plugin's own name in the file.
        const auto vst3File = riff ({
            { "NISI", map ({ { "name", str ("Jazz") }, { "vendor", str ("Audio Modeling") }, { "bankchain", arr ({ str ("SWAM Violin 3") }) } }) },
            { "NICA", map ({ { "ni8", arr ({ arr ({ knob ("Expressivity", 352031384, "Expression"), knob ({}, 1044572362, "Vibrato") }) }) } }) },
            { "PLID", map ({ { "VST3.uid", arr ({ num (1448301651), num (1986802547) }) }, { "pluginName", str ("SWAM Violin 3") }, { "pluginVendor", str ("Audio Modeling") } }) } });
        NksPreset violin;
        CHECK (NksPreset::parse (vst3File.data(), vst3File.size(), violin).wasOk() && violin.vst3Ids && violin.pluginName == "SWAM Violin 3");
        juce::PluginDescription swam; swam.name = "SWAM Violin 3"; swam.manufacturerName = "Audio Modeling"; swam.pluginFormatName = "VST3";
        juce::PluginDescription viola = swam; viola.name = "SWAM Viola 3";
        CHECK (violin.matches (swam) == 3 && violin.matches (viola) == 0);
        {
            const ParamInfoList ps { param (0, "1044572362", "Vibrato Depth"), param (1, "352031384", "Expression"), param (2, "5", "Vibrato") };
            const auto m = controlMapFromNks (violin, swam, ps);
            CHECK (m.pages.size() == 1 && m.pages[0].knobs[0].paramId == "352031384" && m.pages[0].knobs[0].label.isEmpty());   // same name: no label
            CHECK (m.pages[0].knobs[1].paramId == "1044572362" && m.pages[0].knobs[1].label == "Vibrato");                       // by id, not the name
        }

        // The search: the presets of each folder, the pages most of them share, found by plugin.
        juce::TemporaryFile tmpNks;
        const auto root = tmpNks.getFile();
        CHECK (root.getChildFile ("Super 8 R2/Presets").createDirectory() && root.getChildFile ("SWAM Violin 3").createDirectory());
        auto write = [] (const juce::File& f, const Bytes& b) { return f.replaceWithData (b.data(), b.size()); };
        CHECK (write (root.getChildFile ("Super 8 R2/Presets/A.nksf"), synthFile) && write (root.getChildFile ("Super 8 R2/Presets/B.nksf"), synthFile));
        const auto odd = riff ({ { "NISI", map ({ { "name", str ("Odd") }, { "vendor", str ("Native Instruments") }, { "bankchain", arr ({ str ("Super 8 R2") }) } }) },
                                 { "NICA", map ({ { "ni8", arr ({ arr ({ knob ("Other", 9, "Thing") }) }) } }) } });
        CHECK (write (root.getChildFile ("Super 8 R2/Presets/C.nksf"), odd));
        CHECK (write (root.getChildFile ("SWAM Violin 3/Jazz.nksf"), vst3File) && write (root.getChildFile ("SWAM Violin 3/notes.txt"), str ("x")));
        {
            ControlMapLibrary lib (root.getChildFile ("setups"));
            CHECK (lib.findNks (super8) == nullptr && lib.getNksGeneration() == 0);
            lib.startNksSearch ({ root });
            CHECK (lib.waitForNks (10000) && lib.getNksGeneration() == 1);
            const auto* s8 = lib.findNks (super8);
            CHECK (s8 != nullptr && s8->name == "Sling Cave" && s8->pages[0].knobs.size() == 5);      // two of three agree
            CHECK (lib.findNks (swam) != nullptr && lib.findNks (swam)->vst3Ids && lib.findNks (viola) == nullptr);
        }
        root.deleteRecursively();
    }

    // ---- an instrument's setup, an input's knobs and buttons --------------
    {
        SlotDef sd; sd.plugin.name = "Kontakt 8"; sd.controlMap = "Alicia's Keys";
        CHECK (SlotDef::fromVar (sd.toVar()).controlMap == "Alicia's Keys");
        SlotDef plain; plain.plugin.name = "x";
        CHECK (! plain.toVar().getDynamicObject()->hasProperty ("controlMap"));

        InputDef in; in.name = "Upper";
        CHECK (in.controls.isEmpty() && ! in.toVar().getDynamicObject()->hasProperty ("controls"));
        in.controls.knobCC = { 21, 22, 23, 24, 0, 0, 0, 28 };
        in.controls.knobsRelative = true;
        in.controls.buttons[SurfaceControls::programUp] = "cc:104";
        in.controls.buttons[SurfaceControls::pageDown] = "kk:b12";
        const auto back = InputDef::fromVar (juce::JSON::parse (juce::JSON::toString (in.toVar())));
        CHECK (back.controls.knobCC == in.controls.knobCC && back.controls.knobsRelative);
        CHECK (back.controls.buttons[SurfaceControls::programUp] == "cc:104" && back.controls.buttons[SurfaceControls::pageDown] == "kk:b12");
        CHECK (back.controls.buttons[SurfaceControls::programDown].isEmpty() && ! back.controls.isEmpty());
        CHECK (SurfaceControls::ccOf ("cc:104") == 104 && SurfaceControls::ccOf ("kk:b12") == 0 && SurfaceControls::ccOf ({}) == 0);

        // What each knob is heard on: taught ones as taught; untaught ones nothing, or on a keyboard told
        // what to send, the first spare from 102 + k that no knob already has -- one answer for everyone.
        SurfaceControls sc;
        CHECK (sc.heardCC (false) == (std::array<int, 8> { 0, 0, 0, 0, 0, 0, 0, 0 }));
        CHECK (sc.heardCC (true)  == (std::array<int, 8> { 102, 103, 104, 105, 106, 107, 108, 109 }));
        sc.knobCC = { 21, 104, 0, 0, 0, 0, 0, 0 };
        CHECK (sc.heardCC (false) == (std::array<int, 8> { 21, 104, 0, 0, 0, 0, 0, 0 }));
        CHECK (sc.heardCC (true)  == (std::array<int, 8> { 21, 104, 105, 106, 107, 108, 109, 110 }));   // knob 3's spare was taken
    }

    std::printf (failures == 0 ? "ModelTest: all checks passed\n" : "ModelTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
