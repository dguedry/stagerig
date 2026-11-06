#include "Midi2Inputs.h"

namespace perf
{

using namespace juce;
namespace ump = juce::universal_midi_packets;

struct Midi2Inputs::Connection : private ump::Consumer
{
    Connection (const String& id, const Where& where, ump::Session& session, const Receiver& r)
        : identifier (id), group (where.group), receiver (r),
          input (session.connectInput (where.endpoint, ump::PacketProtocol::MIDI_2_0))
    {
        if (input.isAlive()) input.addConsumer (*this);
    }

    ~Connection() override
    {
        if (input.isAlive()) input.removeConsumer (*this);
    }

    bool isAlive() const        { return input.isAlive(); }

    const String identifier;

private:
    void consume (ump::Iterator b, ump::Iterator e, double time) override
    {
        for (auto it = b; it != e; ++it)
        {
            const auto& v = *it;
            if (ump::Utils::hasGroup (ump::Utils::getMessageType (v[0])) && ump::Utils::getGroup (v[0]) != group) continue;
            translate (v, time, translator, [this] (const MidiMessage& m, int64 exact) { receiver (identifier, m, exact); });
        }
    }

    const uint8 group;
    const Receiver& receiver;
    ump::Input input;
    ump::SingleGroupMidi1ToBytestreamTranslator translator { 4096 };
};

Midi2Inputs::Midi2Inputs (Receiver r) : receiver (std::move (r)) {}

Midi2Inputs::~Midi2Inputs()
{
    connections.clear();
}

bool Midi2Inputs::find (const String& identifier, Where& where)
{
    if (identifier.isEmpty()) return false;
    auto* endpoints = ump::Endpoints::getInstance();
    for (const auto& id : endpoints->getEndpoints())
        if (const auto info = endpoints->getStaticDeviceInfo (id))
        {
            const auto ids = info->getLegacyIdentifiersSrc();
            for (size_t g = 0; g < ids.size(); ++g)
                if (ids[g] == identifier)
                {
                    where = { id, (uint8) g };
                    return true;
                }
        }
    return false;
}

bool Midi2Inputs::isMidi2 (const String& identifier)
{
    Where where;
    if (! find (identifier, where)) return false;
    const auto endpoint = ump::Endpoints::getInstance()->getEndpoint (where.endpoint);
    return endpoint.has_value() && endpoint->getProtocol() == ump::PacketProtocol::MIDI_2_0;
}

StringArray Midi2Inputs::open (const StringArray& identifiers)
{
    StringArray heard;
    std::vector<std::unique_ptr<Connection>> kept;
    for (const auto& id : identifiers)
    {
        Where where;
        if (heard.contains (id) || ! isMidi2 (id) || ! find (id, where)) continue;

        // One already open and still connected is kept, so nothing is lost while the rig is rescanned.
        auto it = std::find_if (connections.begin(), connections.end(), [&id] (const auto& c) { return c->identifier == id && c->isAlive(); });
        if (it != connections.end())
        {
            kept.push_back (std::move (*it));
            connections.erase (it);
            heard.add (id);
            continue;
        }
        if (session == nullptr || ! session->isAlive())
        {
            kept.clear();            // a dead session's connections first, then the session
            connections.clear();
            session.reset();
            auto s = ump::Endpoints::getInstance()->makeSession ("StageRig MIDI 2.0");
            if (s.isAlive()) session = std::make_unique<ump::Session> (std::move (s));
        }
        if (session == nullptr) break;
        auto c = std::make_unique<Connection> (id, where, *session, receiver);
        if (! c->isAlive()) continue;
        kept.push_back (std::move (c));
        heard.add (id);
    }
    connections = std::move (kept);     // the ones no longer wanted close here
    return heard;
}

void Midi2Inputs::translate (const ump::View& v, double time, ump::SingleGroupMidi1ToBytestreamTranslator& translator,
                             const std::function<void (const MidiMessage&, int64)>& out)
{
    /* What MIDI 1.0 rounds: a controller, channel pressure and pitch bend
       carry 32 bits in a MIDI 2.0 message. The value goes with the MIDI 1.0
       message made from it, and with nothing else. */
    int64 exact = -1;
    uint8 status = 0;
    if (ump::Utils::getMessageType (v[0]) == ump::Utils::MessageKind::channelVoice2)
    {
        status = (uint8) ump::Utils::getStatus (v[0]);
        if (status == 0xb || status == 0xd || status == 0xe) exact = (int64) v[1];
    }
    ump::Conversion::midi2ToMidi1DefaultTranslation (v, [&] (const ump::View& midi1)
    {
        translator.dispatch (midi1, time, [&] (const ump::BytesOnGroup& bytes, double t)
        {
            const MidiMessage m (bytes.bytes.data(), (int) bytes.bytes.size(), t);
            // A channel message's data bytes are 7 bits; a malformed packet's go no further.
            if (m.getChannel() != 0)
                for (int i = 1; i < m.getRawDataSize(); ++i)
                    if ((m.getRawData()[i] & 0x80) != 0) return;
            const bool same = (status == 0xb && m.isController()) || (status == 0xd && m.isChannelPressure()) || (status == 0xe && m.isPitchWheel());
            out (m, same ? exact : -1);
        });
    });
}

} // namespace perf
