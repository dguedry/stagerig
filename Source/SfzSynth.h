#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <memory>
#include <vector>

namespace sfz { class Sfizz; }

namespace perf
{

/** The built-in sampler: an SFZ instrument -- a sampled piano, say -- played
    with nothing to install. The engine is sfizz, compiled in
    (external/sfizz, BSD); it keeps the start of every sample in memory and
    reads the rest from disk as notes are played, so a piano of a gigabyte
    or two costs a small part of that.

    Made on a loader thread, played on the audio thread, like the General
    MIDI player: one synth is one instrument at one sample rate, and a
    change of either is a new synth swapped in. */
class SfzSynth
{
public:
    ~SfzSynth();

    /** An instrument found on this computer. */
    struct Instrument
    {
        juce::String name;      // as a setup names it: the file's name without ".sfz" (or ".dspreset")
        juce::File file;
        juce::String category;  // what kind of sound it is, from its name: "Pianos & Keys" (categoryOf)
        juce::String family;    // its name without the playing style: "Baroque Alto Recorder"
        juce::String style;     // the playing style, or empty: "Staccato" (splitStyle)
        juce::String library;   // the library it is part of: the folder it was found in
    };
    /** Every SFZ instrument (and Decent Sampler preset) on this computer, by name: the system's folders,
        the "Instruments" folder beside the settings and the sample libraries
        chosen in StageRig (libraries()), a few folders deep. A file reached
        twice (a library linked into Instruments and chosen as well) is listed once. */
    static juce::Array<Instrument> installedInstruments (const juce::File& settingsFolder);

    /** The kinds of sound instruments are sorted into, in the order menus list them. */
    static juce::StringArray categories();
    /** The kind of sound a name is, read from the instrument words in it
        ("Grand Piano, Steinway B" is Pianos & Keys, "Conga" Drums &
        Percussion). The same rule for every library: no list per library,
        and its folders are not read, since libraries name them as they like.
        "Other" when no word is known. */
    static juce::String categoryOf (const juce::String& name);
    /** "Baroque Alto Recorder - Staccato" is the Baroque Alto Recorder played
        staccato. The part after the last " - " is a playing style only when
        every word of it is one (Staccato, Sustain, Soft Mallet...): "TX81Z -
        FM Piano" is a sound of its own, and stays whole. */
    static void splitStyle (const juce::String& name, juce::String& family, juce::String& style);
    /** Of a family's files, the one to start with: no style, else Normal or Sustain, a keyswitch last. */
    static int styleRank (const juce::String& style);

    /** The sample libraries chosen in StageRig (Options): folders anywhere,
        read where they are. Kept one a line in "Sample libraries.txt" beside
        the settings, which the plugin host reads too. */
    static juce::Array<juce::File> libraries (const juce::File& settingsFolder);
    static bool addLibrary (const juce::File& settingsFolder, const juce::File& folder);
    static void removeLibrary (const juce::File& settingsFolder, const juce::File& folder);
    /** The file a setup means by a name and a path: the path if it is there,
        else the installed instrument of that name. None: File(). */
    static juce::File find (const juce::File& settingsFolder, const juce::String& name, const juce::String& path);

    /** A synth at this rate with the instrument loaded, or null (and `error` set). */
    static std::unique_ptr<SfzSynth> create (double sampleRate, int blockSize, const juce::File& instrument, juce::String& error);

    /** A control the instrument's maker named: a MIDI controller that
        changes its sound (reverb, release, a microphone's level). */
    struct Control
    {
        int cc = 0;
        juce::String name;
        float value = 0.0f;     // 0..1, as the instrument starts
    };
    /** The instrument's named controls, in controller order. Before it is played. */
    std::vector<Control> controls() const;
    /** Audio thread: a control moved, 0..1. */
    void setControl (int cc, float value);

    /** Audio thread: every note and sound off, now. */
    void silence();
    /** Audio thread: renders `numSamples` with the events in `midi` at their
        sample positions, adding the result into left and right. Channels are
        not told apart: an SFZ instrument is one instrument. */
    void renderAdding (const juce::MidiBuffer& midi, float* left, float* right, int numSamples);

private:
    SfzSynth();
    std::unique_ptr<sfz::Sfizz> synth;
    juce::AudioBuffer<float> scratch;
    int maxBlock = 0;
};

} // namespace perf
