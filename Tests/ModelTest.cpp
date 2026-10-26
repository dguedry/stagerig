// Round-trip test for the StageRig setup file format.
#include "Model.h"
#include "ProgramMap.h"
#include "MappingSuggestions.h"
#include <juce_events/juce_events.h>
#include <cstdio>
#include "ClapPluginFormat.h"

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
    {
        SlotDef ext; ext.kind = SlotDef::Kind::external;
        ext.midiOutIdentifier = "20-0"; ext.midiOutName = "Montage"; ext.outChannel = 3; ext.listensTo = 1 << 1;
        ext.bankMsb = 63; ext.bankLsb = 1; ext.patch = 9; ext.returnLeft = 2; ext.returnRight = 3;
        EffectDef arp; arp.plugin.name = "Arp"; arp.plugin.pluginFormatName = "CLAP"; arp.plugin.fileOrIdentifier = "/x/arp.clap|a";
        EffectDef send; send.kind = EffectDef::Kind::midiSend; send.portIdentifier = "24-0"; send.portName = "MicroFreak";
        send.channel = 4; send.patch = 12; send.bypassed = true;
        ext.midiEffects = { arp, send };
        upper.programs[7].slots.push_back (ext);
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
        sg.click = true; sg.beatsPerBar = 3; sg.firstBeat = 0.25; sg.startSeconds = 1.79;
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
    CHECK (loaded.songs[0].click && loaded.songs[0].beatsPerBar == 3 && std::abs (loaded.songs[0].firstBeat - 0.25) < 1e-9);
    CHECK (std::abs (loaded.songs[0].startSeconds - 1.79) < 1e-9);
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

    std::printf (failures == 0 ? "ModelTest: all checks passed\n" : "ModelTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
