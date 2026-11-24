#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <memory>

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
        juce::String name;      // as it is listed and as a setup names it: the file's name without ".sfz"
        juce::File file;
    };
    /** Every SFZ instrument on this computer, by name: the system's folders
        and the "Instruments" folder beside the settings, a few folders deep. */
    static juce::Array<Instrument> installedInstruments (const juce::File& settingsFolder);
    /** The file a setup means by a name and a path: the path if it is there,
        else the installed instrument of that name. None: File(). */
    static juce::File find (const juce::File& settingsFolder, const juce::String& name, const juce::String& path);

    /** A synth at this rate with the instrument loaded, or null (and `error` set). */
    static std::unique_ptr<SfzSynth> create (double sampleRate, int blockSize, const juce::File& instrument, juce::String& error);

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
