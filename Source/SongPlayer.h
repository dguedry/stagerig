#pragma once

#include "Model.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace perf
{

/** A song's backing track: its stems, held in memory and played together.

    Held in memory rather than streamed, because a backing track on stage has
    to jump: a vamp that repeats its four bars until the singer is ready, a
    jump to the chorus. From memory those land on the exact sample with no
    gap; a stream would have to refill from disk first. Each stem is read on a
    background thread when the song is selected, converted once to the device
    rate, and kept at 16 bits -- about 46 MB for four minutes of stereo. Only
    the song in play is held.

    Sections: each marker starts one, running to the next marker (or the end).
    A section can be a vamp, which repeats until released. Asking for a
    section queues it: playback goes there when the current section ends, so
    it stays in time; while stopped, it sets where Play starts.

    Threads: construction, start(), stop(), queue/release and setMarkers()
    belong to the message thread (setMarkers() under the engine's lock);
    render() and prepare() to the audio side under that lock; the loading
    thread only fills the stems and then sets `ready`. */
class SongPlayer
{
public:
    SongPlayer (juce::AudioFormatManager& formats, const juce::File& folder,
                const SongDef& song, double sampleRate, int blockSize);
    ~SongPlayer();

    bool hasStems() const                         { return numStemDefs > 0; }
    /** A click through the song: with no stems, a plain metronome. */
    bool hasClick() const                         { return clickOn.load(); }
    /** The click, live: on or off, its accent, and where beat 1 of bar 1
        falls in the files (seconds). Also lines the count-in up with it. */
    void setClick (bool on, int beatsPerBar, double firstBeatSeconds);
    bool isReady() const                          { return ready.load(); }
    float getLoadProgress() const                 { return loadProgress.load(); }
    juce::String getProblems() const;             // files that could not be read; once ready

    /* Times given and taken here are the files' own seconds. Played at a
       tempo other than the files', the track is longer or shorter inside,
       by `timeScale`; the conversion happens at this edge, so sections,
       cues and the clock on the panel all read as the files read. */
    double getLengthSeconds() const               { return ready.load() ? (double) lengthSamples / (sampleRate * timeScale) : 0.0; }
    double getPositionSeconds() const             { return (double) position.load() / (sampleRate * timeScale); }
    double getTimeScale() const                   { return timeScale; }
    int    getPitchSemitones() const              { return pitchSemitones; }

    enum class State { idle, waiting, countIn, playing, stopping };
    State getState() const                        { return (State) state.load(); }
    bool isRunning() const                        { return getState() != State::idle; }

    /** Device changed: the stems are loaded again at a new rate. */
    void prepare (double sampleRate, int blockSize);

    /** Starts from the cued position (the top, unless a section was asked
        for while stopped): `countInBeats` clicks of `beatSeconds` each on
        `countInOutput` first. Before loading has finished it waits for it. */
    void start (int countInBeats, double beatSeconds, int countInOutput);
    /** Fades out over a few milliseconds and goes idle; the next start is from the top. */
    void stop();

    /** Sections, from the song's markers. Under the engine's lock. */
    void setMarkers (const std::vector<MarkerDef>&);
    int  getNumSections() const                   { return numSections; }
    juce::String getSectionName (int) const;
    double getSectionStart (int) const;           // seconds
    /** The index in the song's markers a section came from, or -1 for the stretch before the first. */
    int  getSectionMarker (int) const;
    bool isSectionVamp (int) const;
    int  getCurrentSection() const                { return currentSection.load(); }
    int  getQueuedSection() const                 { return queuedSection.load(); }
    /** Live: true while the current section is a vamp that has not been released. */
    bool isVamping() const                        { const int c = currentSection.load(); return getState() != State::idle && c >= 0 && c < numSections && sectionVamp[c] && vampHold.load(); }

    /** Goes to section `index` when the current one ends; while stopped,
        that is where Play will start. -1 cancels a queued section. */
    void queueSection (int index);
    /** While stopped: Play starts here (seconds). */
    void cue (double seconds);
    /** Where Play starts while stopped (seconds): the top of the song unless a section or a cue was asked for. */
    double getCueSeconds() const                  { return (double) cuedPosition.load() / (sampleRate * timeScale); }
    /** The song's own start point (seconds); 0 means the first sound in the files. */
    void setStart (double seconds);
    /** Lets a vamp play on at the end of the current repetition. */
    void releaseVamp()                            { vampHold.store (false); }
    /** The one-pedal control: releases a vamp, or else queues the next section. */
    void advance();

    void setStemMix (int stemIndex, int output, float gainDb);

    enum class Happened { nothing, stemsStarted, ended, stopped };
    /** Audio thread: adds this block of the track to the outputs' mixes, and
        its MIDI files' events to `trackMidi` -- the backing-track input's
        MIDI for this block, every channel as the file has it (null when
        there is no such input). `outputs[i]` holds output i's left and
        right mix buffers. */
    Happened render (float* (*outputs)[2], int numOutputs, juce::MidiBuffer* trackMidi, int numSamples);

private:
    struct Stem
    {
        int defIndex = 0;
        juce::File file;
        std::vector<int16_t> left, right;         // at the device rate; filled by the loader
        std::atomic<int> output { 0 };
        std::atomic<float> gain { 1.0f };

        // A MIDI stem: its events at device-rate sample positions, in order.
        bool midi = false;
        struct Event { juce::int64 at; uint8_t data[3]; uint8_t size; };
        std::vector<Event> events;
        juce::int64 midiLength = 0;
        juce::int64 firstSound = 0;               // where the file first makes a sound, device samples; set by the loader
        bool notesSounding = false;               // audio thread: notes may be held, so a jump sends notes-off
        bool needsChase = true;                   // audio thread: catch the instruments up on programs and controllers first
    };
    bool loadMidiStem (Stem&);
    void midiForRun (Stem&, juce::MidiBuffer&, juce::int64 from, int count, int at);
    static void allNotesOff (juce::MidiBuffer&, int at);
    /** What the file had set by `from` -- programs, banks, volumes, pans,
        wheels -- so starting or jumping mid-file sounds as it does arriving there. */
    static void chase (const Stem&, juce::MidiBuffer&, juce::int64 from, int at);

    void startLoading();
    void stopLoading();
    void load();                                  // loader thread
    bool loadStem (Stem&);
    void addCountInClicks (float* l, float* r, int numSamples);
    void addClickRun (float* l, float* r, int at, juce::int64 from, int count);
    /** One click's sample `inBeat` samples after its beat, or 0 past its length. */
    float clickSample (juce::int64 inBeat, bool accent) const;
    int  sectionAt (juce::int64 pos) const;
    juce::int64 sectionEnd (int section) const;

    juce::AudioFormatManager& formats;
    std::vector<std::unique_ptr<Stem>> stems;
    int numStemDefs = 0;
    juce::StringArray problems;                   // written by the loader before `ready`

    std::thread loader;
    std::atomic<bool> loaderStop { false }, ready { false };
    std::atomic<float> loadProgress { 0.0f };

    double sampleRate = 48000.0;
    int blockSize = 512;
    double timeScale = 1.0;                       // stretched length over the files' own: fixed at construction
    int pitchSemitones = 0;                       // the files' pitch moved by this much: fixed at construction
    juce::int64 lengthSamples = 0;                // the longest stem; written by the loader before `ready`

    // Sections, in device samples. Fixed capacity: changed under the engine's lock, never allocated by the audio thread.
    static constexpr int kMaxSections = 64;
    juce::int64 sectionStart[kMaxSections] {};
    bool sectionVamp[kMaxSections] {};
    juce::String sectionName[kMaxSections];
    double sectionSeconds[kMaxSections] {};
    int sectionMarker[kMaxSections] {};
    int numSections = 1;

    std::atomic<int> state { (int) State::idle };
    std::atomic<juce::int64> position { 0 };
    std::atomic<juce::int64> cuedPosition { 0 };
    std::atomic<bool> cuedByUser { false };       // a section or a cue was asked for: the loader leaves it alone
    /* The top of the song: its own start point if it has one, else the
       first sound in its files -- silence at the head of a file is never
       played. `leadIn` is written by the loader before `ready`. */
    std::atomic<juce::int64> startSamples { 0 }, leadIn { 0 };
    juce::int64 home() const                      { const auto s = startSamples.load(); return s > 0 ? s : leadIn.load(); }
    std::atomic<int> currentSection { 0 }, queuedSection { -1 };
    std::atomic<bool> vampHold { false }, vamping { false };

    // count-in, audio thread once started
    std::atomic<int> countInOutput { 0 };
    std::atomic<int> pendingCountInBeats { 0 };
    std::atomic<double> pendingBeatSeconds { 0.5 };
    juce::int64 countInRemaining = 0, beatSamples = 1, countInDone = 0;
    int countInTotalBeats = 0;

    // the click through the song
    std::atomic<bool> clickOn { false };
    std::atomic<int> clickBeatsPerBar { 4 };
    std::atomic<double> firstBeat { 0.0 };          // seconds into the files

    // fade-out, audio thread
    int fadeRemaining = 0, fadeLength = 1;
    juce::int64 lastRunEnd = -1;                  // audio thread: where the last block's playback ended, to see a jump between blocks

    // mixing scratch, sized in prepare()
    juce::AudioBuffer<float> scratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongPlayer)
};

} // namespace perf
