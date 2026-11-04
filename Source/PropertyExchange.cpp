#include "PropertyExchange.h"

namespace perf
{

using namespace juce;
namespace ci = juce::midi_ci;

namespace
{
    var object (std::initializer_list<std::pair<const char*, var>> fields)
    {
        auto* o = new DynamicObject();
        for (const auto& [name, value] : fields) o->setProperty (name, value);
        return var (o);
    }

    var bankPC (int program)    { return Array<var> { 0, 0, jlimit (0, 127, program) }; }

    String json (const var& v)  { return JSON::toString (v, JSON::FormatOptions{}.withSpacing (JSON::Spacing::none)); }

    /* Korg's descriptions of its two resources, as its Keystage asks for
       them (Keystage Property Exchange Resource List, v1.0). */
    var parameterListSchema()
    {
        return JSON::parse (R"json({"type":"array","items":{"type":"object","properties":{
            "name":{"title":"Parameter Name","description":"Parameter Name","type":"string"},
            "controlcc":{"title":"Control CC","description":"CC number to control this parameter","type":"integer","minimum":0,"maximum":127},
            "default":{"title":"Default Value","description":"Default value for this parameter","type":"integer","minimum":0,"maximum":127}}}})json");
    }

    var programEditSchema()
    {
        return JSON::parse (R"json({"type":"object","properties":{"currentValues":{"type":"array","items":{"type":"object","properties":{
            "name":{"title":"Parameter Name","description":"Parameter Name","type":"string"},
            "value":{"title":"Current Value","description":"Current value (Control Change value)","type":"integer","minimum":0,"maximum":127},
            "displayValue":{"title":"Display Value","description":"Current Value displayed on the UI (actual value)","type":"string"},
            "displayUnit":{"title":"Display Unit","description":"Unit text for this parameter","type":"string"}}}}}})json");
    }

    const char* const programListId = "all";

    // StageRig's identity: 7D is MIDI's id for non-commercial use; StageRig has no registered one.
    ump::DeviceInfo stageRigInfo()
    {
        return { { std::byte { 0x7d }, std::byte { 0 }, std::byte { 0 } },
                 { std::byte { 0x53 }, std::byte { 0x52 } },     // "SR"
                 { std::byte { 0x01 }, std::byte { 0 } },
                 { std::byte { 0x01 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 } } };
    }
}

//==============================================================================
PropertyResponder::PropertyResponder (Send s) : send (std::move (s))
{
    Random random;
    const auto options = ci::DeviceOptions()
        .withOutputs ({ this })
        .withDeviceInfo (stageRigInfo())
        .withFeatures (ci::DeviceFeatures().withPropertyExchangeSupported())
        .withMaxSysExSize (4096)
        .withProductInstanceId (ci::DeviceOptions::makeProductInstanceId (random))
        .withPropertyDelegate (this);
    device = std::make_unique<ci::Device> (options);
}

PropertyResponder::~PropertyResponder()
{
    device.reset();     // says goodbye (Invalidate MUID) while `send` still works
}

void PropertyResponder::processMessage (ump::BytesOnGroup message)
{
    if (send) send (message.bytes);
}

void PropertyResponder::receive (Span<const std::byte> bytes)
{
    device->processMessage ({ 0, bytes });
    device->sendPendingMessages();
}

void PropertyResponder::announce()
{
    note ("asked who is there (Discovery)");
    device->sendDiscovery();
}

void PropertyResponder::note (const String& what)
{
    log.add (Time::getCurrentTime().toString (false, true, true, true) + "  " + what);
    if (log.size() > 40) log.remove (0);
}

String PropertyResponder::getRemoteName() const
{
    for (const auto& m : device->getDiscoveredMuids())
    {
        const auto info = device->getDeviceInfoForMuid (m);
        if (const auto model = info.getProperty ("model", {}).toString(); model.isNotEmpty())
        {
            const auto maker = info.getProperty ("manufacturer", {}).toString();
            return maker.isNotEmpty() && ! model.startsWithIgnoreCase (maker.upToFirstOccurrenceOf (" ", false, false)) ? maker + " " + model : model;
        }
    }
    return {};
}

//==============================================================================
var PropertyResponder::channelList (const SurfaceView& v)
{
    if (v.program < 0) return Array<var>();
    return Array<var> { object ({
        { "title", v.inputName.isNotEmpty() ? v.inputName : String ("StageRig") },
        { "channel", v.channel > 0 ? v.channel : 1 },
        { "programTitle", v.programName },
        { "bankPC", bankPC (v.program) },
        { "links", Array<var> { object ({ { "resource", "ProgramList" }, { "resId", programListId }, { "title", "Programs" } }),
                                object ({ { "resource", "X-ParameterList" } }),
                                object ({ { "resource", "X-ProgramEdit" } }) } } }) };
}

var PropertyResponder::programList (const SurfaceView& v)
{
    Array<var> list;
    for (const auto& [number, name] : v.programs)
        if (number < 128) list.add (object ({ { "title", name }, { "bankPC", bankPC (number) } }));
    return list;
}

var PropertyResponder::parameterList (const SurfaceView& v)
{
    Array<var> list;
    for (int k = 0; k < SurfaceView::numKnobs; ++k)
        list.add (object ({ { "name", v.knobNames[(size_t) k] },
                            { "controlcc", jlimit (0, 127, v.knobControllers[(size_t) k]) },
                            { "default", 0 } }));
    return list;
}

var PropertyResponder::programEdit (const SurfaceView& v)
{
    Array<var> values;
    for (int k = 0; k < SurfaceView::numKnobs; ++k)
    {
        const float level = v.knobLevels[(size_t) k];
        values.add (object ({ { "name", v.knobNames[(size_t) k] },
                              { "value", level >= 0.0f ? roundToInt (level * 127.0f) : 0 },
                              { "displayValue", v.knobValues[(size_t) k] },
                              { "displayUnit", String() } }));
    }
    return object ({ { "currentValues", values } });
}

var PropertyResponder::contentOf (const String& resource, const String& resId) const
{
    if (resource == "ResourceList")
    {
        // Only what differs from each resource's defaults is said (M2-103).
        return Array<var> {
            object ({ { "resource", "DeviceInfo" } }),
            object ({ { "resource", "ChannelList" }, { "canSubscribe", true } }),
            object ({ { "resource", "ProgramList" }, { "canSubscribe", true } }),
            object ({ { "resource", "JSONSchema" } }),
            object ({ { "resource", "X-ParameterList" }, { "canSubscribe", true },
                      { "schema", object ({ { "title", "Parameter List" }, { "type", "object" }, { "$ref", "midi+jsonschema://parameterListSchema" } }) } }),
            object ({ { "resource", "X-ProgramEdit" }, { "canSubscribe", true },
                      { "schema", object ({ { "title", "Program Edit" }, { "type", "object" }, { "$ref", "midi+jsonschema://programEditSchema" } }) } }) };
    }
    if (resource == "DeviceInfo")
        return object ({ { "manufacturerId", Array<var> { 0x7d, 0, 0 } }, { "manufacturer", "StageRig" },
                         { "familyId", Array<var> { 0x53, 0x52 } }, { "family", "StageRig" },
                         { "modelId", Array<var> { 1, 0 } }, { "model", "StageRig" },
                         { "versionId", Array<var> { 1, 0, 0, 0 } }, { "version", JUCEApplicationBase::getInstance() != nullptr
                                                                                    ? JUCEApplicationBase::getInstance()->getApplicationVersion() : String ("1") } });
    if (resource == "JSONSchema")
    {
        if (resId == "parameterListSchema") return parameterListSchema();
        if (resId == "programEditSchema")   return programEditSchema();
        return {};
    }
    if (! haveView) return {};
    if (resource == "ChannelList")      return channelList (view);
    if (resource == "ProgramList")      return programList (view);       // whatever resId: a Keystage sends none
    if (resource == "X-ParameterList")  return parameterList (view);
    if (resource == "X-ProgramEdit")    return programEdit (view);
    return {};
}

ci::PropertyReplyData PropertyResponder::propertyGetDataRequested (ci::MUID, const ci::PropertyRequestHeader& header)
{
    asked = true;
    ci::PropertyReplyData reply;
    auto content = contentOf (header.resource, header.resId);
    note ("asked for " + header.resource + (header.resId.isNotEmpty() ? " " + header.resId : String())
          + (header.pagination.has_value() ? " from " + String (header.pagination->offset) + ", " + String (header.pagination->limit) : String())
          + (content.isVoid() ? ": not there (404)" : String()));
    if (content.isVoid())
    {
        reply.header.status = 404;
        reply.header.message = "StageRig has no " + header.resource + (header.resId.isNotEmpty() ? " " + header.resId : String());
        return reply;
    }
    // A list asked for a page at a time: that page, and how many there are in all.
    if (header.pagination.has_value())
        if (const auto* all = content.getArray())
        {
            const int from = jmax (0, header.pagination->offset), count = jmax (1, header.pagination->limit);
            Array<var> page;
            for (int i = from; i < jmin (all->size(), from + count); ++i) page.add ((*all)[i]);
            reply.header.extended[Identifier ("totalCount")] = all->size();
            content = page;
        }
    const auto text = ci::Encodings::jsonTo7BitText (content);
    if (auto encoded = header.mutualEncoding != ci::Encoding::ascii ? ci::Encodings::tryEncode (text, header.mutualEncoding) : std::nullopt)
    {
        reply.header.mutualEncoding = header.mutualEncoding;
        reply.body = std::move (*encoded);
    }
    else reply.body = text;
    return reply;
}

ci::PropertyReplyHeader PropertyResponder::propertySetDataRequested (ci::MUID, const ci::PropertyRequestData& request)
{
    asked = true;
    note ("asked to set " + request.header.resource + ": refused (405)");
    ci::PropertyReplyHeader h;
    h.status = 405;
    h.message = request.header.resource + " is StageRig's to set: choose a program by sending its Program Change.";
    return h;
}

bool PropertyResponder::subscriptionStartRequested (ci::MUID, const ci::PropertySubscriptionHeader& header)
{
    asked = true;
    note ("asked to be told of " + header.resource);
    return header.resource == "ChannelList" || header.resource == "ProgramList"
        || header.resource == "X-ParameterList" || header.resource == "X-ProgramEdit";
}

void PropertyResponder::subscriptionDidStart (ci::MUID m, const String& subId, const ci::PropertySubscriptionHeader& header)
{
    subscribers[header.resource].insert ({ m, subId });
    // What it is told from now on is measured against what it has: what it is asking about now.
    if (haveView) sent[header.resource] = json (contentOf (header.resource, {}));
}

void PropertyResponder::subscriptionWillEnd (ci::MUID m, const ci::Subscription& sub)
{
    subscribers[sub.resource].erase ({ m, sub.subscribeId });
    note ("no longer told of " + sub.resource);
}

void PropertyResponder::tell (const String& resource, bool whole)
{
    auto it = subscribers.find (resource);
    if (it == subscribers.end() || it->second.empty()) return;
    const auto content = contentOf (resource, {});
    const auto text = json (content);
    if (sent[resource] == text) return;
    sent[resource] = text;
    const auto body = ci::Encodings::jsonTo7BitText (content);
    note ("told of " + resource + (whole ? String() : String (" (to ask again)")));
    for (const auto& s : it->second)
    {
        ci::PropertySubscriptionHeader h;
        h.resource = resource;
        h.subscribeId = s.id;
        // A long list is only said to have changed (notify), for the keyboard to ask again; the rest goes whole.
        h.command = whole ? ci::PropertySubscriptionCommand::full : ci::PropertySubscriptionCommand::notify;
        if (auto* host = device->getPropertyHost())
            host->sendSubscriptionUpdate (s.muid, h, whole ? Span<const std::byte> (body.data(), body.size()) : Span<const std::byte>(), {});
    }
}

void PropertyResponder::show (const SurfaceView& v)
{
    const bool knobsOnly = haveView && v.program == view.program && v.programName == view.programName && v.knobNames == view.knobNames
                        && v.knobControllers == view.knobControllers && v.programs == view.programs && v.channel == view.channel;
    const auto before = view;
    view = v;
    haveView = true;
    if (! knobsOnly)
    {
        tell ("ChannelList", true);
        tell ("ProgramList", false);
        tell ("X-ParameterList", true);
        tell ("X-ProgramEdit", true);
    }
    else
    {
        // Values the keyboard's own knobs have just changed: its screen shows them already.
        const auto now = Time::getMillisecondCounter();
        bool fromElsewhere = false;
        for (int k = 0; k < SurfaceView::numKnobs; ++k)
            if (v.knobValues[(size_t) k] != before.knobValues[(size_t) k] && now - turnedAt[(size_t) k] > 500)
                fromElsewhere = true;
        if (fromElsewhere) tell ("X-ProgramEdit", true);
        else               sent["X-ProgramEdit"] = json (programEdit (view));
    }
    device->sendPendingMessages();
}

void PropertyResponder::knobTurnedOnKeyboard (int knob)
{
    if (knob >= 0 && knob < SurfaceView::numKnobs) turnedAt[(size_t) knob] = Time::getMillisecondCounter();
}

//==============================================================================
std::vector<MidiCiKeyboard::Ports> MidiCiKeyboard::find (const StringArray& inputNames)
{
    std::vector<Ports> found;
    const auto ins = MidiInput::getAvailableDevices(), outs = MidiOutput::getAvailableDevices();
    for (const auto& in : ins)
    {
        if (! inputNames.contains (in.name) || in.name.containsIgnoreCase ("Midi Through")) continue;
        // A Komplete Kontrol MK1 ("KOMPLETE KONTROL S88 MIDI 1") predates MIDI-CI: it is not asked.
        if (in.name.containsIgnoreCase ("KOMPLETE KONTROL S") && ! in.name.containsIgnoreCase ("MK")) continue;
        for (const auto& out : outs)
            if (out.name == in.name) { found.push_back ({ in, out }); break; }
    }
    return found;
}

MidiCiKeyboard::MidiCiKeyboard (const Ports& p)
    : ports (p),
      responder ([this] (Span<const std::byte> bytes)
      {
          if (output != nullptr) output->sendMessageNow (MidiMessage::createSysExMessage (bytes));
      })
{
    output = MidiOutput::openDevice (ports.out.identifier);
    input = MidiInput::openDevice (ports.in.identifier, this);
    if (! opened()) { input.reset(); output.reset(); return; }
    input->start();
    responder.announce();
}

MidiCiKeyboard::~MidiCiKeyboard()
{
    if (input != nullptr) input->stop();
    cancelPendingUpdate();
}

String MidiCiKeyboard::getName() const
{
    const auto remote = responder.getRemoteName();
    return (remote.isNotEmpty() ? remote : ports.in.name) + " (MIDI-CI)";
}

String MidiCiKeyboard::getReport() const
{
    String r;
    r << "  ports: in " << ports.in.name << ", out " << ports.out.name << "\n";
    if (const auto remote = responder.getRemoteName(); remote.isNotEmpty()) r << "  says it is: " << remote << "\n";
    for (const auto& line : responder.getLog()) r << "  " << line << "\n";
    return r.trimEnd();
}

bool MidiCiKeyboard::isConnected() const
{
    if (! opened()) return false;
    for (const auto& d : MidiInput::getAvailableDevices())
        if (d.identifier == ports.in.identifier) return true;
    return false;
}

void MidiCiKeyboard::show (const SurfaceView& v)
{
    for (int k = 0; k < SurfaceView::numKnobs; ++k)
        knobControllers[(size_t) k].store (v.knobControllers[(size_t) k], std::memory_order_relaxed);
    for (int k = 0; k < SurfaceView::numKnobs; ++k)
        if (turned[(size_t) k].exchange (false)) responder.knobTurnedOnKeyboard (k);
    responder.show (v);
}

void MidiCiKeyboard::handleIncomingMidiMessage (MidiInput*, const MidiMessage& m)
{
    if (m.isController())
    {
        for (int k = 0; k < SurfaceView::numKnobs; ++k)
            if (knobControllers[(size_t) k].load (std::memory_order_relaxed) == m.getControllerNumber()) turned[(size_t) k] = true;
        return;
    }
    // Universal non-realtime (7E), any device, MIDI-CI (0D): the rest of the keyboard's SysEx is not ours.
    if (! m.isSysEx() || m.getSysExDataSize() < 4) return;
    const auto* d = m.getSysExData();
    if (d[0] != 0x7e || d[2] != 0x0d) return;
    {
        const std::lock_guard<std::mutex> l (queueMutex);
        if (queue.size() < 256)
            queue.emplace_back (reinterpret_cast<const std::byte*> (d), reinterpret_cast<const std::byte*> (d) + m.getSysExDataSize());
    }
    triggerAsyncUpdate();
}

void MidiCiKeyboard::handleAsyncUpdate()
{
    std::vector<std::vector<std::byte>> waiting;
    {
        const std::lock_guard<std::mutex> l (queueMutex);
        waiting.swap (queue);
    }
    for (const auto& message : waiting)
        responder.receive (Span<const std::byte> (message.data(), message.size()));
    // A turn noticed above, for the next show(): set here too, in case nothing else changes.
    for (int k = 0; k < SurfaceView::numKnobs; ++k)
        if (turned[(size_t) k].exchange (false)) responder.knobTurnedOnKeyboard (k);
}

} // namespace perf
