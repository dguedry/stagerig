#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <memory>

namespace perf
{

/** The MIDI output ports external instruments play through.

    The audio thread never touches a device: it hands each event, stamped
    with the moment it should leave, to a lock-free queue, and a sender
    thread of its own delivers it then. A slow driver or a device unplugged
    mid-song can delay a note to that synth, never the audio.

    Ports are known by small indices so the audio thread can name one
    without a string; a port is opened the first time a slot asks for it and
    re-opened by identifier after a hot-plug. */
class MidiOuts : private juce::Thread
{
public:
    MidiOuts();
    ~MidiOuts() override;

    static constexpr int kMaxPorts = 16;

    /** Message thread: the index for this device, opening it if needed; -1 if
        there is no room. The index stays the device's for the life of this
        object, open or not. */
    int portFor (const juce::String& identifier, const juce::String& name);
    bool isOpen (int port) const;
    juce::String getName (int port) const;

    /** Message thread: re-opens every known port that has come back, closes
        those that have gone (a hot-plug, a Rescan). */
    void refresh();

    /** Audio thread: queue `m` for `port`, to leave at `timeMs` on the
        Time::getMillisecondCounterHiRes() clock. False when the queue is full. */
    bool push (int port, const juce::MidiMessage& m, double timeMs);

    /** Message thread: send at once (patch recall, notes-off on a change of setup). */
    void sendNow (int port, const juce::MidiMessage& m);

private:
    void run() override;

    struct Port
    {
        juce::String identifier, name;
        std::unique_ptr<juce::MidiOutput> device;
    };
    Port ports[kMaxPorts];
    std::atomic<int> numPorts { 0 };
    juce::CriticalSection deviceLock;            // opening and sending, never taken by the audio thread

    struct Event { double timeMs; int port; juce::uint8 size; juce::uint8 data[3]; };
    static constexpr int kQueueSize = 8192;
    juce::AbstractFifo fifo { kQueueSize };
    Event queue[kQueueSize];
    juce::WaitableEvent wake;
};

} // namespace perf
