#include "Model.h"

using namespace juce;

namespace perf
{

namespace
{
    DynamicObject* obj (const var& v) { return v.getDynamicObject(); }

    template <typename T>
    T prop (const var& v, const char* name, T def)
    {
        if (auto* o = obj (v))
            if (o->hasProperty (name))
                return static_cast<T> (o->getProperty (name));
        return def;
    }

    String propStr (const var& v, const char* name, const String& def = {})
    {
        if (auto* o = obj (v))
            if (o->hasProperty (name))
                return o->getProperty (name).toString();
        return def;
    }
}

//==============================================================================
static var effectsToVar (const std::vector<EffectDef>& effects)
{
    Array<var> arr;
    for (auto& e : effects) arr.add (e.toVar());
    return arr;
}

static std::vector<EffectDef> effectsFromVar (const var& parent, const char* key = "effects")
{
    std::vector<EffectDef> out;
    if (auto* arr = obj (parent) != nullptr ? obj (parent)->getProperty (key).getArray() : nullptr)
        for (auto& e : *arr) out.push_back (EffectDef::fromVar (e));
    return out;
}

var EffectDef::toVar() const
{
    auto* o = new DynamicObject();
    if (isSend())
    {
        o->setProperty ("kind", "midiSend");
        o->setProperty ("port", portIdentifier);
        o->setProperty ("portName", portName);
        if (channel > 0)  o->setProperty ("channel", channel);
        if (bankMsb >= 0) o->setProperty ("bankMsb", bankMsb);
        if (bankLsb >= 0) o->setProperty ("bankLsb", bankLsb);
        if (patch >= 0)   o->setProperty ("patch", patch);
        o->setProperty ("bypassed", bypassed);
        return var (o);
    }
    if (auto xml = plugin.createXml())
        o->setProperty ("plugin", xml->toString (XmlElement::TextFormat().singleLine()));
    o->setProperty ("state", state.toBase64Encoding());
    o->setProperty ("bypassed", bypassed);
    return var (o);
}

EffectDef EffectDef::fromVar (const var& v)
{
    EffectDef e;
    if (propStr (v, "kind") == "midiSend")
    {
        e.kind = Kind::midiSend;
        e.portIdentifier = propStr (v, "port");
        e.portName = propStr (v, "portName");
        e.channel = jlimit (0, 16, prop<int> (v, "channel", 0));
        e.bankMsb = jlimit (-1, 127, prop<int> (v, "bankMsb", -1));
        e.bankLsb = jlimit (-1, 127, prop<int> (v, "bankLsb", -1));
        e.patch   = jlimit (-1, 127, prop<int> (v, "patch", -1));
        e.bypassed = prop<bool> (v, "bypassed", false);
        return e;
    }
    if (auto xml = parseXML (propStr (v, "plugin")))
        e.plugin.loadFromXml (*xml);
    e.state.fromBase64Encoding (propStr (v, "state"));
    e.bypassed = prop<bool> (v, "bypassed", false);
    return e;
}

//==============================================================================
var SlotDef::toVar() const
{
    auto* o = new DynamicObject();
    if (auto xml = plugin.createXml())
        o->setProperty ("plugin", xml->toString (XmlElement::TextFormat().singleLine()));
    o->setProperty ("state", state.toBase64Encoding());
    o->setProperty ("enabled", enabled);
    o->setProperty ("gainDb", gainDb);
    o->setProperty ("transpose", transpose);
    o->setProperty ("lowKey", lowKey);
    o->setProperty ("highKey", highKey);
    o->setProperty ("lowVelocity", lowVelocity);
    o->setProperty ("highVelocity", highVelocity);
    o->setProperty ("velocityCurve", velocityCurve);
    o->setProperty ("pan", pan);
    o->setProperty ("outChannel", outChannel);
    if (receiveChannel > 0) o->setProperty ("receiveChannel", receiveChannel);
    if (isExternal())
    {
        o->setProperty ("kind", "external");
        o->setProperty ("midiOut", midiOutIdentifier);
        o->setProperty ("midiOutName", midiOutName);
        if (bankMsb >= 0) o->setProperty ("bankMsb", bankMsb);
        if (bankLsb >= 0) o->setProperty ("bankLsb", bankLsb);
        if (patch >= 0)   o->setProperty ("patch", patch);
    }
    if (! isPlugin() && returnLeft >= 0)
    {
        o->setProperty ("returnLeft", returnLeft);
        o->setProperty ("returnRight", returnRight);
    }
    if (! midiEffects.empty()) o->setProperty ("midiEffects", effectsToVar (midiEffects));
    o->setProperty ("effects", effectsToVar (effects));
    return var (o);
}

SlotDef SlotDef::fromVar (const var& v)
{
    SlotDef s;
    if (auto xml = parseXML (propStr (v, "plugin")))
        s.plugin.loadFromXml (*xml);
    s.state.fromBase64Encoding (propStr (v, "state"));
    s.enabled    = prop<bool>  (v, "enabled", true);
    s.gainDb     = prop<float> (v, "gainDb", 0.0f);
    s.transpose  = prop<int>   (v, "transpose", 0);
    s.lowKey     = prop<int>   (v, "lowKey", 0);
    s.highKey    = prop<int>   (v, "highKey", 127);
    s.lowVelocity   = juce::jlimit (1, 127, prop<int> (v, "lowVelocity", 1));
    s.highVelocity  = juce::jlimit (s.lowVelocity, 127, prop<int> (v, "highVelocity", 127));
    s.velocityCurve = juce::jlimit (-1.0f, 1.0f, prop<float> (v, "velocityCurve", 0.0f));
    s.pan           = juce::jlimit (-1.0f, 1.0f, prop<float> (v, "pan", 0.0f));
    s.outChannel = prop<int>   (v, "outChannel", 0);
    s.receiveChannel = juce::jlimit (0, 16, prop<int> (v, "receiveChannel", 0));
    if (propStr (v, "kind") == "external")
    {
        s.kind = Kind::external;
        s.midiOutIdentifier = propStr (v, "midiOut");
        s.midiOutName = propStr (v, "midiOutName");
        s.bankMsb = juce::jlimit (-1, 127, prop<int> (v, "bankMsb", -1));
        s.bankLsb = juce::jlimit (-1, 127, prop<int> (v, "bankLsb", -1));
        s.patch   = juce::jlimit (-1, 127, prop<int> (v, "patch", -1));
    }
    if (! s.isPlugin())
    {
        s.returnLeft  = juce::jlimit (-1, 63, prop<int> (v, "returnLeft", -1));
        s.returnRight = juce::jlimit (-1, 63, prop<int> (v, "returnRight", -1));
    }
    s.midiEffects = effectsFromVar (v, "midiEffects");
    // A send belongs to the MIDI chain only: one found among the effects is dropped.
    for (auto& e : effectsFromVar (v))
        if (! e.isSend()) s.effects.push_back (std::move (e));
    return s;
}

//==============================================================================
String MappingDef::sourceDescription() const
{
    switch (source)
    {
        case Source::CC:              return "CC " + String (number);
        case Source::PitchBend:       return "Pitch Bend";
        case Source::ChannelPressure: return "Aftertouch";
    }
    return {};
}

var MappingDef::toVar() const
{
    auto* o = new DynamicObject();
    o->setProperty ("source", (int) source);
    o->setProperty ("number", number);
    o->setProperty ("slot", slot);
    o->setProperty ("effect", effect);
    o->setProperty ("paramId", paramId);
    o->setProperty ("paramName", paramName);
    o->setProperty ("min", minValue);
    o->setProperty ("max", maxValue);
    o->setProperty ("passThrough", passThrough);
    return var (o);
}

MappingDef MappingDef::fromVar (const var& v)
{
    MappingDef m;
    m.source      = (Source) prop<int> (v, "source", 0);
    m.number      = prop<int>   (v, "number", 1);
    m.slot        = prop<int>   (v, "slot", 0);
    m.effect      = prop<int>   (v, "effect", -1);
    m.paramId     = propStr (v, "paramId");
    m.paramName   = propStr (v, "paramName");
    m.minValue    = prop<float> (v, "min", 0.0f);
    m.maxValue    = prop<float> (v, "max", 1.0f);
    m.passThrough = prop<bool>  (v, "passThrough", false);
    return m;
}

//==============================================================================
var ProgramDef::toVar() const
{
    auto* o = new DynamicObject();
    o->setProperty ("name", name);
    // Only written when set, so setups without groups stay byte-identical.
    if (group.isNotEmpty()) o->setProperty ("group", group);

    Array<var> slotArr;
    for (auto& s : slots) slotArr.add (s.toVar());
    o->setProperty ("slots", slotArr);
    o->setProperty ("effects", effectsToVar (effects));
    if (inputGainDb != 0.0f) o->setProperty ("inputGainDb", inputGainDb);

    Array<var> mapArr;
    for (auto& m : mappings) mapArr.add (m.toVar());
    o->setProperty ("mappings", mapArr);
    return var (o);
}

ProgramDef ProgramDef::fromVar (const var& v)
{
    ProgramDef p;
    p.name = propStr (v, "name");
    p.group = propStr (v, "group");      // absent in setups written before groups
    if (auto* arr = obj (v) != nullptr ? obj (v)->getProperty ("slots").getArray() : nullptr)
        for (auto& s : *arr) p.slots.push_back (SlotDef::fromVar (s));
    p.effects = effectsFromVar (v);
    p.inputGainDb = (float) prop<double> (v, "inputGainDb", 0.0);
    if (auto* arr = obj (v) != nullptr ? obj (v)->getProperty ("mappings").getArray() : nullptr)
        for (auto& m : *arr) p.mappings.push_back (MappingDef::fromVar (m));
    return p;
}

//==============================================================================
var OutputDef::toVar() const
{
    auto* o = new DynamicObject();
    o->setProperty ("name", name);
    o->setProperty ("left", left);
    o->setProperty ("right", right);
    if (gainDb != 0.0f) o->setProperty ("gainDb", gainDb);
    if (volumeCC > 0)   o->setProperty ("volumeCC", volumeCC);
    o->setProperty ("effects", effectsToVar (chain.effects));
    return var (o);
}

OutputDef OutputDef::fromVar (const var& v)
{
    OutputDef d;
    d.name     = propStr (v, "name", "Main");
    d.left     = jlimit (0, 63, prop<int> (v, "left", 0));
    d.right    = jlimit (-1, 63, prop<int> (v, "right", 1));
    d.gainDb   = (float) prop<double> (v, "gainDb", 0.0);
    d.volumeCC = jlimit (0, 127, prop<int> (v, "volumeCC", 0));
    d.chain.effects = effectsFromVar (v);
    return d;
}

//==============================================================================
var InputDef::toVar() const
{
    auto* o = new DynamicObject();
    o->setProperty ("name", name);
    // Only written for an audio input, so keyboard setups stay byte-identical.
    if (kind == Kind::audio)
    {
        o->setProperty ("kind", "audio");
        o->setProperty ("audioLeft", audioLeft);
        o->setProperty ("audioRight", audioRight);
    }
    if (output != 0) o->setProperty ("output", output);
    o->setProperty ("midiDeviceIdentifier", midiDeviceIdentifier);
    o->setProperty ("midiDeviceName", midiDeviceName);
    o->setProperty ("channel", channel);
    o->setProperty ("respondToProgramChange", respondToProgramChange);
    o->setProperty ("programChangeChannel", programChangeChannel);
    o->setProperty ("currentProgram", currentProgram);

    // Only store non-empty programs to keep files small.
    Array<var> progs;
    for (int i = 0; i < (int) programs.size(); ++i)
    {
        if (programs[(size_t) i].isEmpty() && programs[(size_t) i].name.isEmpty() && programs[(size_t) i].inputGainDb == 0.0f)
            continue;
        auto pv = programs[(size_t) i].toVar();
        pv.getDynamicObject()->setProperty ("index", i);
        progs.add (pv);
    }
    o->setProperty ("programs", progs);
    return var (o);
}

InputDef InputDef::fromVar (const var& v)
{
    InputDef in;
    in.name                   = propStr (v, "name", "Input");
    in.kind                   = propStr (v, "kind", "midi") == "audio" ? Kind::audio : Kind::midi;
    in.audioLeft              = jlimit (0, 63, prop<int> (v, "audioLeft", 0));
    in.audioRight             = jlimit (-1, 63, prop<int> (v, "audioRight", 1));
    in.output                 = jmax (0, prop<int> (v, "output", 0));
    in.midiDeviceIdentifier   = propStr (v, "midiDeviceIdentifier");
    in.midiDeviceName         = propStr (v, "midiDeviceName");
    in.channel                = prop<int>  (v, "channel", 0);
    in.respondToProgramChange = prop<bool> (v, "respondToProgramChange", true);
    in.programChangeChannel   = juce::jlimit (-1, 16, prop<int> (v, "programChangeChannel", InputDef::pcChannelSameAsNotes));
    in.currentProgram         = jlimit (0, numPrograms - 1, prop<int> (v, "currentProgram", 0));

    if (auto* arr = obj (v) != nullptr ? obj (v)->getProperty ("programs").getArray() : nullptr)
        for (auto& pv : *arr)
        {
            int idx = prop<int> (pv, "index", -1);
            if (idx >= 0 && idx < numPrograms)
                in.programs[(size_t) idx] = ProgramDef::fromVar (pv);
        }
    return in;
}

//==============================================================================
//==============================================================================
var ChartRef::toVar() const
{
    DynamicObject::Ptr o (new DynamicObject());
    o->setProperty ("file", file);
    if (title.isNotEmpty()) o->setProperty ("title", title);
    return var (o.get());
}

ChartRef ChartRef::fromVar (const var& v)
{
    ChartRef c;
    if (auto* o = v.getDynamicObject())
    {
        c.file  = o->getProperty ("file").toString();
        c.title = o->getProperty ("title").toString();
    }
    return c;
}

File chartsFolderFor (const File& setupFile)
{
    if (setupFile == File()) return {};
    // "MySetup.stagerig.json" -> "MySetup.stagerig.charts"
    return setupFile.getSiblingFile (setupFile.getFileNameWithoutExtension() + ".charts");
}

var MarkerDef::toVar() const
{
    auto* o = new DynamicObject();
    o->setProperty ("name", name);
    o->setProperty ("seconds", seconds);
    if (vamp) o->setProperty ("vamp", true);
    return var (o);
}

MarkerDef MarkerDef::fromVar (const var& v)
{
    MarkerDef m;
    m.name    = propStr (v, "name");
    m.seconds = jmax (0.0, prop<double> (v, "seconds", 0.0));
    m.vamp    = prop<bool> (v, "vamp", false);
    return m;
}

var StemDef::toVar() const
{
    auto* o = new DynamicObject();
    o->setProperty ("file", file);
    if (name.isNotEmpty()) o->setProperty ("name", name);
    o->setProperty ("output", output);
    if (gainDb != 0.0f) o->setProperty ("gainDb", gainDb);
    if (isMidi())
    {
        // Only the channels that do not go to the GM player.
        Array<var> routes;
        for (int ch = 1; ch <= 16; ++ch)
        {
            const auto& r = route (ch);
            if (r.kind == MidiRoute::Kind::gm) continue;
            auto* ro = new DynamicObject();
            ro->setProperty ("ch", ch);
            ro->setProperty ("kind", r.kind == MidiRoute::Kind::keys ? "keys" : r.kind == MidiRoute::Kind::slot ? "slot" : "mute");
            if (r.kind != MidiRoute::Kind::mute) ro->setProperty ("input", r.input);
            if (r.kind == MidiRoute::Kind::slot)
            {
                ro->setProperty ("slot", r.slot);
                if (r.program >= 0) ro->setProperty ("program", r.program);
            }
            routes.add (var (ro));
        }
        o->setProperty ("channels", routes);
    }
    return var (o);
}

StemDef StemDef::fromVar (const var& v)
{
    StemDef d;
    d.file   = propStr (v, "file");
    d.name   = propStr (v, "name");
    d.output = jmax (0, prop<int> (v, "output", 0));
    d.gainDb = (float) prop<double> (v, "gainDb", 0.0);
    if (! d.isMidi()) return d;

    if (auto* arr = obj (v) != nullptr ? obj (v)->getProperty ("channels").getArray() : nullptr)
    {
        for (auto& rv : *arr)
        {
            const int ch = prop<int> (rv, "ch", 0);
            if (ch < 1 || ch > 16) continue;
            auto& r = d.route (ch);
            const auto kind = propStr (rv, "kind");
            r.kind = kind == "keys" ? MidiRoute::Kind::keys : kind == "slot" ? MidiRoute::Kind::slot
                   : kind == "mute" ? MidiRoute::Kind::mute : MidiRoute::Kind::gm;
            r.input   = jmax (0, prop<int> (rv, "input", 0));
            r.slot    = jlimit (-1, 255, prop<int> (rv, "slot", -1));
            r.program = jlimit (-1, InputDef::numPrograms - 1, prop<int> (rv, "program", -1));
            if (r.kind == MidiRoute::Kind::slot && r.slot < 0) r.kind = MidiRoute::Kind::keys;
        }
    }
    else if (obj (v) != nullptr && obj (v)->hasProperty ("input"))
    {
        /* Written before the GM player: the whole file (or its one chosen
           channel) through a keyboard, or straight into one of its plugins. */
        const int input = jmax (0, prop<int> (v, "input", 0));
        const int only = jlimit (0, 16, prop<int> (v, "midiChannel", 0));
        const int slot = jlimit (-1, 255, prop<int> (v, "slot", -1));
        const int program = jlimit (-1, InputDef::numPrograms - 1, prop<int> (v, "program", -1));
        for (int ch = 1; ch <= 16; ++ch)
        {
            auto& r = d.route (ch);
            r.input = input;
            if (only > 0 && ch != only)   r.kind = MidiRoute::Kind::mute;
            else if (slot >= 0)           { r.kind = MidiRoute::Kind::slot; r.slot = slot; r.program = program; }
            else                          r.kind = MidiRoute::Kind::keys;
        }
    }
    return d;
}

var SongDef::toVar() const
{
    auto* o = new DynamicObject();
    o->setProperty ("name", name);
    Array<var> progArr;
    for (int p : programs) progArr.add (p);
    o->setProperty ("programs", progArr);
    // Only written when set, so a setup without songs stays byte-identical.
    if (tempoBpm > 0.0)    o->setProperty ("tempoBpm", tempoBpm);
    if (notes.isNotEmpty()) o->setProperty ("notes", notes);
    if (! charts.empty())
    {
        juce::Array<juce::var> arr;
        for (const auto& c : charts) arr.add (c.toVar());
        o->setProperty ("charts", arr);
    }
    if (! stems.empty())
    {
        juce::Array<juce::var> arr;
        for (const auto& st : stems) arr.add (st.toVar());
        o->setProperty ("stems", arr);
    }
    if (countInBeats > 0)
    {
        o->setProperty ("countInBeats", countInBeats);
        o->setProperty ("countInOutput", countInOutput);
    }
    if (! markers.empty())
    {
        juce::Array<juce::var> arr;
        for (const auto& m : markers) arr.add (m.toVar());
        o->setProperty ("markers", arr);
    }
    return var (o);
}

SongDef SongDef::fromVar (const var& v)
{
    SongDef s;
    if (auto* o = v.getDynamicObject())
    {
        s.name     = o->getProperty ("name").toString();
        s.tempoBpm = (double) o->getProperty ("tempoBpm");
        s.notes    = o->getProperty ("notes").toString();
        if (auto* arr = o->getProperty ("charts").getArray())
            for (const auto& c : *arr)
                if (auto ref = ChartRef::fromVar (c); ref.file.isNotEmpty())
                    s.charts.push_back (ref);
        if (auto* arr = o->getProperty ("programs").getArray())
            for (auto& p : *arr)
                s.programs.push_back ((int) p);
        if (auto* arr = o->getProperty ("stems").getArray())
            for (const auto& st : *arr)
                if (auto stem = StemDef::fromVar (st); stem.file.isNotEmpty())
                    s.stems.push_back (stem);
        if (auto* arr = o->getProperty ("markers").getArray())
            for (const auto& m : *arr)
                s.markers.push_back (MarkerDef::fromVar (m));
        s.countInBeats  = jlimit (0, 16, (int) o->getProperty ("countInBeats"));
        s.countInOutput = jmax (0, (int) o->getProperty ("countInOutput"));
    }
    return s;
}

//==============================================================================
var SetDef::toVar() const
{
    auto* o = new DynamicObject();
    o->setProperty ("name", name);
    Array<var> songArr;
    for (int i : songs) songArr.add (i);
    o->setProperty ("songs", songArr);
    return var (o);
}

SetDef SetDef::fromVar (const var& v)
{
    SetDef s;
    if (auto* o = v.getDynamicObject())
    {
        s.name = o->getProperty ("name").toString();
        if (auto* arr = o->getProperty ("songs").getArray())
            for (auto& i : *arr)
                s.songs.push_back ((int) i);
    }
    return s;
}

//==============================================================================
var Setup::toVar() const
{
    auto* o = new DynamicObject();
    o->setProperty ("format", "stagerig-setup");
    o->setProperty ("version", 1);
    o->setProperty ("preloadAllPrograms", preloadAllPrograms);
    o->setProperty ("releaseTailSeconds", releaseTailSeconds);
    o->setProperty ("holdAcrossChange", holdAcrossChange);
    o->setProperty ("tempoBpm", tempoBpm);
    o->setProperty ("tapTempoCC", tapTempoCC);
    Array<var> arr;
    for (auto& i : inputs) arr.add (i.toVar());
    o->setProperty ("inputs", arr);

    // Only when there is more to say than "Main on 1-2": older setups stay byte-identical.
    if (! (outputs.size() == 1 && outputs[0].isDefault()))
    {
        Array<var> outArr;
        for (auto& out : outputs) outArr.add (out.toVar());
        o->setProperty ("outputs", outArr);
    }

    /* Songs and sets are only written when there are any, so every setup made
       before this existed still saves and loads byte-identically. */
    if (! songs.empty())
    {
        Array<var> songArr;
        for (auto& sg : songs) songArr.add (sg.toVar());
        o->setProperty ("songs", songArr);
    }
    if (! sets.empty())
    {
        Array<var> setArr;
        for (auto& st : sets) setArr.add (st.toVar());
        o->setProperty ("sets", setArr);
        o->setProperty ("currentSet", currentSet);
        o->setProperty ("currentSongInSet", currentSongInSet);
    }
    if (nextSongCC > 0) o->setProperty ("nextSongCC", nextSongCC);
    if (prevSongCC > 0) o->setProperty ("prevSongCC", prevSongCC);
    if (transportCC > 0) o->setProperty ("transportCC", transportCC);
    if (sectionCC > 0)   o->setProperty ("sectionCC", sectionCC);
    return var (o);
}

Setup Setup::fromVar (const var& v)
{
    Setup s;
    s.preloadAllPrograms = prop<bool> (v, "preloadAllPrograms", false);
    s.releaseTailSeconds = prop<double> (v, "releaseTailSeconds", 4.0);
    s.holdAcrossChange = prop<bool> (v, "holdAcrossChange", true);
    s.tempoBpm           = prop<double> (v, "tempoBpm", 120.0);   // absent in older setups
    s.tapTempoCC         = prop<int>    (v, "tapTempoCC", 0);
    if (auto* arr = obj (v) != nullptr ? obj (v)->getProperty ("inputs").getArray() : nullptr)
        for (auto& iv : *arr) s.inputs.push_back (InputDef::fromVar (iv));
    if (auto* arr = obj (v) != nullptr ? obj (v)->getProperty ("outputs").getArray() : nullptr)
        if (arr->size() > 0)
        {
            s.outputs.clear();
            for (auto& ov : *arr) s.outputs.push_back (OutputDef::fromVar (ov));
        }
    // An input on an output that is not there lands on the first one rather than nowhere.
    for (auto& in : s.inputs)
        if (in.output >= (int) s.outputs.size()) in.output = 0;

    s.nextSongCC = prop<int> (v, "nextSongCC", 0);
    s.prevSongCC = prop<int> (v, "prevSongCC", 0);
    s.transportCC = jlimit (0, 127, prop<int> (v, "transportCC", 0));
    s.sectionCC   = jlimit (0, 127, prop<int> (v, "sectionCC", 0));
    if (auto* o = obj (v))
    {
        if (auto* arr = o->getProperty ("songs").getArray())
            for (auto& sv : *arr) s.songs.push_back (SongDef::fromVar (sv));
        if (auto* arr = o->getProperty ("sets").getArray())
            for (auto& sv : *arr) s.sets.push_back (SetDef::fromVar (sv));
    }
    s.currentSet       = prop<int> (v, "currentSet", -1);
    s.currentSongInSet = prop<int> (v, "currentSongInSet", -1);

    /* A set or song index that no longer exists -- a setup hand-edited, or a
       song deleted by an older build -- must not be able to crash a gig. */
    if (s.currentSet >= (int) s.sets.size()) { s.currentSet = -1; s.currentSongInSet = -1; }
    for (auto& st : s.sets)
        st.songs.erase (std::remove_if (st.songs.begin(), st.songs.end(),
                                        [&] (int i) { return i < 0 || i >= (int) s.songs.size(); }),
                        st.songs.end());
    return s;
}

Result Setup::saveToFile (const File& f) const
{
    if (! f.getParentDirectory().createDirectory())
        return Result::fail ("Could not create directory " + f.getParentDirectory().getFullPathName());

    TemporaryFile temp (f);
    {
        FileOutputStream out (temp.getFile());
        if (! out.openedOk())
            return Result::fail ("Could not write " + f.getFullPathName());
        JSON::writeToStream (out, toVar());
    }
    if (! temp.overwriteTargetFileWithTemporary())
        return Result::fail ("Could not replace " + f.getFullPathName());
    return Result::ok();
}

Result Setup::loadFromFile (const File& f, Setup& out)
{
    if (! f.existsAsFile())
        return Result::fail ("File not found: " + f.getFullPathName());

    var parsed;
    auto r = JSON::parse (f.loadFileAsString(), parsed);
    if (r.failed())
        return r;
    // "performer-setup" is what the program wrote under its old name; those files are the same thing.
    if (propStr (parsed, "format") != "stagerig-setup" && propStr (parsed, "format") != "performer-setup")
        return Result::fail ("Not a StageRig setup file");

    out = fromVar (parsed);
    return Result::ok();
}

Setup Setup::makeDefault()
{
    Setup s;
    InputDef upper; upper.name = "Upper"; upper.channel = 1;
    InputDef lower; lower.name = "Lower"; lower.channel = 2;
    s.inputs.push_back (upper);
    s.inputs.push_back (lower);
    return s;
}

} // namespace perf
