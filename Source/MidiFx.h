#pragma once

#include "Model.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
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
    /** "D major", "A natural minor": a key by its root and scale. */
    static juce::String keyName (int root, int mode);
    /** Whether the scale is a minor one (its chords are named from the relative major). */
    static bool keyIsMinor (int mode);
    /** Whether chord names in this key are spelt with flats: F, Bb, Eb, Ab, Db, Gb and their minors. */
    static bool keyPrefersFlats (int root, int mode);
    /** Big band: the twelve steps above the key's root, "I", "bII", "II" ... "VII". */
    static const char* const bandDegrees[];
    /** The major key whose chords a key in this scale uses: A natural minor's are C major's. */
    static int bandMajorRoot (int root, int mode);
    /** A chord typed for the step on `rowRoot` (a pitch class): notes ("C E G A", the first one the key pressed)
        or a name ("C6", "Dm7", "G7", "Bm7b5"). False when it reads as neither. Blank: an empty chord. */
    static bool parseBandChord (const juce::String& text, int rowRoot, std::vector<int>& offsets);
    /** The chord on `rowRoot` by name when it has one ("Dm7"), else its notes ("D F A C"). Blank for no chord. */
    static juce::String bandChordName (int rowRoot, const std::vector<int>& offsets, bool flats);
    /** The chord's notes, "D F A C". */
    static juce::String bandChordNotes (int rowRoot, const std::vector<int>& offsets, bool flats);
    /** Phrase: reads a MIDI file's notes into `def` (every track, from its first note, at the file's beat), naming it after the file.
        False, with a reason, when it cannot be read or has no notes. Up to 512 notes. */
    static bool importPhrase (const juce::File& file, EffectDef& def, juce::String& error);
    static int numArpRates();
    static int numArpModes();
    static int numChordTypes();
    static int numScaleModes();
};

} // namespace perf
