#pragma once

#include "Model.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>
#include <vector>

namespace perf
{

/** A song's backing track: its stems, read from disk and played together.

    Files are streamed, never loaded whole: a background thread keeps a few
    seconds of each stem ready, and the audio thread only ever takes what is
    already there -- a slow disk costs a moment of silence in one stem, never
    a stall of the whole rig. Each stem is resampled to the device rate if its
    file differs, and mixed onto its own output, ahead of that output's master
    effects and volume.

    Threads: the constructor, rewind(), waitUntilReady(), start() and stop()
    belong to the message thread; prepare() and render() to the audio side,
    under the engine's lock. State moves between them through atomics only. */
class SongPlayer
{
public:
    SongPlayer (juce::TimeSliceThread& readAhead, juce::AudioFormatManager& formats,
                const juce::File& folder, const SongDef& song, double sampleRate, int blockSize,
                juce::StringArray& problems);
    ~SongPlayer();

    bool hasStems() const                         { return ! stems.empty(); }
    int  numStems() const                         { return (int) stems.size(); }
    double getLengthSeconds() const               { return sampleRate > 0.0 ? (double) lengthSamples / sampleRate : 0.0; }
    double getPositionSeconds() const             { return sampleRate > 0.0 ? (double) position.load() / sampleRate : 0.0; }

    enum class State { idle, countIn, playing, stopping };
    State getState() const                        { return (State) state.load(); }
    bool isRunning() const                        { return getState() != State::idle; }

    /** Device changed: every stem is prepared again at the new rate. */
    void prepare (double sampleRate, int blockSize);

    /** Back to the top. Only while idle -- the audio thread is not reading then. */
    void rewind();
    /** Waits until every stem has its first block buffered, up to `timeoutMs`. */
    bool waitUntilReady (int timeoutMs);

    /** Starts: `countInBeats` clicks of `beatSeconds` each on `countInOutput`,
        then the stems. With no count-in, the stems start at once. */
    void start (int countInBeats, double beatSeconds, int countInOutput);
    /** Fades out over a few milliseconds and goes idle. */
    void stop();

    /** A stem's output and level, changed while it plays. `stemIndex` is the
        stem's place in the song's list. */
    void setStemMix (int stemIndex, int output, float gainDb);

    /** What happened in a render, for the engine to act on. */
    enum class Happened { nothing, stemsStarted, ended, stopped };

    /** Audio thread: adds this block of the track to the outputs' mixes.
        `outputs[i]` holds output i's left and right mix buffers. */
    Happened render (float* (*outputs)[2], int numOutputs, int numSamples);

private:
    struct Stem
    {
        std::unique_ptr<juce::AudioFormatReaderSource> reader;
        std::unique_ptr<juce::BufferingAudioSource> buffered;
        std::unique_ptr<juce::ResamplingAudioSource> resampled;
        int defIndex = 0;                         // its place in SongDef::stems
        double fileRate = 0.0;
        juce::int64 fileLength = 0;
        std::atomic<int> output { 0 };
        std::atomic<float> gain { 1.0f };
        juce::AudioBuffer<float> scratch;
    };

    void addClicks (float* l, float* r, int numSamples);

    std::vector<std::unique_ptr<Stem>> stems;
    double sampleRate = 48000.0;
    int blockSize = 512;
    juce::int64 lengthSamples = 0;                // the longest stem, at the device rate

    std::atomic<int> state { (int) State::idle };
    std::atomic<juce::int64> position { 0 };      // device samples into the stems

    // count-in, audio thread once started
    std::atomic<int> countInOutput { 0 };
    juce::int64 countInRemaining = 0, beatSamples = 0, countInDone = 0;
    int countInTotalBeats = 0;

    // fade-out, audio thread
    int fadeRemaining = 0, fadeLength = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongPlayer)
};

} // namespace perf
