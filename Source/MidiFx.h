#pragma once

#include "Model.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>

namespace perf
{

/** A MIDI effect built into StageRig, in a slot's MIDI chain beside the
    plugins: an arpeggiator, a chord, a scale. No plugin to find, no window
    to open; its few settings sit on its row. Audio thread: process() never
    allocates. Settings arrive through configure(), as atomics. */
class MidiFx
{
public:
    virtual ~MidiFx() = default;

    /** Which built-in `def` asks for, or null for a plugin or a send. */
    static std::unique_ptr<MidiFx> create (const EffectDef& def);

    virtual void configure (const EffectDef& def) = 0;
    /** The track started, or a panic: let go of everything and start the count again. */
    virtual void reset() = 0;
    /** This block's MIDI in, the effect's MIDI out; `bpm` is the rig's tempo now. */
    virtual void process (const juce::MidiBuffer& in, juce::MidiBuffer& out, int numSamples, double sampleRate, double bpm) = 0;

    /** The words on the row for `def`'s settings: "1/16, up, 2 octaves". */
    static juce::String describe (const EffectDef& def);
    static const char* const arpRates[];      // "1/4", "1/8", ...
    static const char* const arpModes[];      // "Up", "Down", ...
    static const char* const chordTypes[];    // "Major", "Minor", ...
    static const char* const scaleModes[];    // "Major", "Natural minor", ...
    static const char* const noteNames[];     // "C", "C#", ...
    static int numArpRates();
    static int numArpModes();
    static int numChordTypes();
    static int numScaleModes();
};

} // namespace perf
