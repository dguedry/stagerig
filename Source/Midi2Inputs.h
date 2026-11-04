#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <functional>
#include <memory>
#include <vector>

namespace perf
{

/** Keyboards that speak MIDI 2.0, heard as MIDI 2.0.

    The system's MIDI 1.0 view of such a keyboard rounds every controller to
    128 steps. Opened here instead, its messages still reach the plugins as
    MIDI 1.0 -- what they understand -- but each controller, pressure and
    pitch bend comes with the exact value it was sent with, for StageRig's own
    use of it: a mapping turning a plugin parameter, the keyboard's knobs, an
    output's fader. A MIDI 1.0 keyboard is left to the usual path. */
class Midi2Inputs
{
public:
    /** A message as MIDI 1.0, and the exact value it carried, 0..2^32-1, or
        -1 for one with nothing finer than MIDI 1.0 has. Called on the MIDI
        thread, with the MIDI input identifier the keyboard is known by. */
    using Receiver = std::function<void (const juce::String& identifier, const juce::MidiMessage&, juce::int64 exact)>;

    explicit Midi2Inputs (Receiver);
    ~Midi2Inputs();

    /** Whether the system has this MIDI input as part of a keyboard speaking MIDI 2.0. */
    static bool isMidi2 (const juce::String& identifier);

    /** Hears these inputs (MIDI input identifiers) as MIDI 2.0, and stops
        hearing any others; those that are not MIDI 2.0 are passed over.
        Returns the ones it hears, which the MIDI 1.0 path should leave alone. */
    juce::StringArray open (const juce::StringArray& identifiers);

    /** One packet as MIDI 1.0 messages, each with its exact value: what the
        connections do with what comes in, here for the tests. */
    static void translate (const juce::universal_midi_packets::View&, double time,
                           juce::universal_midi_packets::SingleGroupMidi1ToBytestreamTranslator&,
                           const std::function<void (const juce::MidiMessage&, juce::int64 exact)>&);

private:
    struct Where { juce::universal_midi_packets::EndpointId endpoint; juce::uint8 group = 0; };
    static bool find (const juce::String& identifier, Where&);

    struct Connection;
    Receiver receiver;
    std::unique_ptr<juce::universal_midi_packets::Session> session;
    std::vector<std::unique_ptr<Connection>> connections;
};

} // namespace perf
