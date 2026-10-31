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
    /** A panic: forget everything held and start the count again. The caller sends the notes-off. */
    virtual void reset() = 0;
    /** The backing track started: bar one, beat one. Only the timing starts again --
        what is held stays held, and lets go when its key does. */
    virtual void restartTiming() {}
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
    /** A chord typed for the step on `rowRoot` (a pitch class). Notes with their octaves place each exactly, the
        key pressed being octave 4 ("G3 C4 E4 A4": the G under the key); notes without climb from the key ("C E G A");
        or a name ("C6", "Dm7", "G7", "Bm7b5", "C6/E" with the bass under). False when it reads as none of these. Blank: no chord. */
    static bool parseBandChord (const juce::String& text, int rowRoot, std::vector<int>& offsets);
    /** The chord on `rowRoot` by name when it has one ("Dm7"), else its notes. Blank for no chord. */
    static juce::String bandChordName (int rowRoot, const std::vector<int>& offsets, bool flats);
    /** The chord's notes: "D4 F4 A4 C5" with octaves (the key pressed in octave 4), or "D F A C" without. */
    static juce::String bandChordNotes (int rowRoot, const std::vector<int>& offsets, bool flats, bool octaves = true);
    static constexpr int maxBandNotes = 8;    // notes in one chord
    /** Phrase: reads a MIDI file's notes into `def` (every track, from its first note, at the file's beat), naming it after the file.
        False, with a reason, when it cannot be read or has no notes. Up to 512 notes. */
    static bool importPhrase (const juce::File& file, EffectDef& def, juce::String& error);
    /** The folder of built-in phrases (Licks, Comping, Drums, each a folder of MIDI files): installed beside the
        program, in share/stagerig/phrases, or in the source tree. A non-existent file when there is none. */
    static juce::File phraseLibraryFolder();
    static int numArpRates();
    static int numArpModes();
    static int numChordTypes();
    static int numScaleModes();
};

} // namespace perf
