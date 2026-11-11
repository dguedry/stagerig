#include "MidiOuts.h"
#include <algorithm>
#include <cstring>
#include <vector>

using namespace juce;

namespace perf
{

MidiOuts::MidiOuts() : Thread ("stagerig-midi-out")
{
    startThread (Thread::Priority::highest);
}

MidiOuts::~MidiOuts()
{
    signalThreadShouldExit();
    wake.signal();
    stopThread (2000);
}

int MidiOuts::portFor (const String& identifier, const String& name)
{
    const ScopedLock sl (deviceLock);
    const int n = numPorts.load();
    // The same device as one known: by number and name, or by name when the number has moved on.
    for (int i = 0; i < n; ++i)
        if (ports[i].identifier == identifier && (name.isEmpty() || ports[i].name == name))
        {
            if (ports[i].device == nullptr) ports[i].device = MidiOutput::openDevice (identifier);
            return i;
        }
    if (name.isNotEmpty())
        for (int i = 0; i < n; ++i)
            if (ports[i].name == name) return i;
    if (n >= kMaxPorts) return -1;

    // New: under its number if that is still the device of this name, otherwise wherever that name is now.
    String id = identifier;
    if (name.isNotEmpty())
    {
        const auto available = MidiOutput::getAvailableDevices();
        bool there = false;
        for (const auto& d : available) if (d.identifier == identifier && d.name == name) there = true;
        if (! there)
            for (const auto& d : available) if (d.name == name) { id = d.identifier; break; }
    }
    ports[n].identifier = id;
    ports[n].name = name;
    ports[n].device = MidiOutput::openDevice (id);
    numPorts.store (n + 1);
    return n;
}

bool MidiOuts::isOpen (int port) const
{
    const ScopedLock sl (deviceLock);
    return port >= 0 && port < numPorts.load() && ports[port].device != nullptr;
}

String MidiOuts::getName (int port) const
{
    const ScopedLock sl (deviceLock);
    return port >= 0 && port < numPorts.load() ? ports[port].name : String();
}

void MidiOuts::refresh (const StringArray& reconnected)
{
    const auto available = MidiOutput::getAvailableDevices();
    const ScopedLock sl (deviceLock);
    const int n = numPorts.load();
    for (int i = 0; i < n; ++i)
    {
        auto& p = ports[i];
        bool there = false;
        for (const auto& d : available)
            if (d.identifier == p.identifier && (p.name.isEmpty() || d.name == p.name)) there = true;
        if (! there && p.name.isNotEmpty())
        {
            // Back under another number: followed there, unless another known port has that one.
            for (const auto& d : available)
            {
                if (d.name != p.name) continue;
                bool taken = false;
                for (int j = 0; j < n; ++j) if (j != i && ports[j].identifier == d.identifier) taken = true;
                if (taken) continue;
                p.identifier = d.identifier;
                p.device.reset();
                there = true;
                break;
            }
        }
        /* Opened again after being away: what was sent while it was (the
           note-offs, the pedal coming up) never arrived, and the synth beyond
           it, if it stayed switched on, still holds those notes. Every pedal
           and note let go, as the first thing it hears. */
        auto openQuiet = [&p]
        {
            p.device = MidiOutput::openDevice (p.identifier);
            if (p.device != nullptr)
                for (int ch = 1; ch <= 16; ++ch)
                {
                    p.device->sendMessageNow (MidiMessage::controllerEvent (ch, 64, 0));
                    p.device->sendMessageNow (MidiMessage::allNotesOff (ch));
                }
        };
        if (! there)                               p.device.reset();
        else if (reconnected.contains (p.identifier)) { p.device.reset(); openQuiet(); }
        else if (p.device == nullptr)               openQuiet();
    }
}

bool MidiOuts::push (int port, const MidiMessage& m, double timeMs)
{
    if (port < 0 || port >= kMaxPorts || m.getRawDataSize() > 3) return false;
    int s1, n1, s2, n2;
    fifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 + n2 < 1) return false;
    auto& e = queue[n1 > 0 ? s1 : s2];
    e.timeMs = timeMs;
    e.port = port;
    e.size = (uint8) m.getRawDataSize();
    std::memcpy (e.data, m.getRawData(), e.size);
    fifo.finishedWrite (1);
    wake.signal();
    return true;
}

void MidiOuts::sendNow (int port, const MidiMessage& m)
{
    const ScopedLock sl (deviceLock);
    if (port >= 0 && port < numPorts.load() && ports[port].device != nullptr)
        ports[port].device->sendMessageNow (m);
}

void MidiOuts::run()
{
    /* Events come out of the queue in the order they went in, which within a
       block is time order; across blocks it can differ by a little, so what
       is taken is held here and sent by time. */
    std::vector<Event> pending;
    pending.reserve (1024);

    while (! threadShouldExit())
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (fifo.getNumReady(), s1, n1, s2, n2);
        for (int i = 0; i < n1; ++i) pending.push_back (queue[s1 + i]);
        for (int i = 0; i < n2; ++i) pending.push_back (queue[s2 + i]);
        fifo.finishedRead (n1 + n2);
        std::stable_sort (pending.begin(), pending.end(), [] (const Event& a, const Event& b) { return a.timeMs < b.timeMs; });

        const double now = Time::getMillisecondCounterHiRes();
        size_t sent = 0;
        {
            const ScopedLock sl (deviceLock);
            for (; sent < pending.size() && pending[sent].timeMs <= now + 0.5; ++sent)
            {
                const auto& e = pending[sent];
                if (e.port < numPorts.load() && ports[e.port].device != nullptr)
                    ports[e.port].device->sendMessageNow (MidiMessage (e.data, e.size));
            }
        }
        pending.erase (pending.begin(), pending.begin() + (long) sent);

        // Sleep until the next is due, or until something new arrives.
        const int waitMs = pending.empty() ? 50 : jlimit (0, 50, (int) (pending.front().timeMs - Time::getMillisecondCounterHiRes()));
        if (waitMs > 0) wake.wait (waitMs);
    }
}

} // namespace perf
