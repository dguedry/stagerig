#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>

namespace perf
{

/** A General MIDI synth for backing-track MIDI files: FluidSynth playing a
    GM soundfont (FluidR3_GM by default).

    FluidSynth is loaded when first needed rather than linked, so StageRig
    builds and runs without it; a MIDI file's channels then have to be sent
    to your own sounds. One soundfont stays in memory once loaded, shared by
    every synth made from it, so changing songs does not read 140 MB again.

    Threads: create() and the destructor anywhere but the audio thread (it
    reads the soundfont); handle() and render() on the audio thread only. */
class GmSynth
{
public:
    ~GmSynth();

    /** Is FluidSynth there to be loaded? */
    static bool isAvailable();
    /** The soundfont to use: `preferred` if it exists, else the system's GM
        soundfont (FluidR3_GM, then default-GM, then TimGM6mb). None found: empty. */
    static juce::File findSoundfont (const juce::File& preferred = {});
    /** Why there is no GM player, in words for the status line; empty when there is one. */
    static juce::String unavailableReason (const juce::File& preferred = {});

    /** A synth at this rate with the soundfont loaded, or null (and `error` set). */
    static std::unique_ptr<GmSynth> create (double sampleRate, const juce::File& soundfont, juce::String& error);

    /** Audio thread: one MIDI message. */
    void handle (const juce::MidiMessage&);
    /** Audio thread: every note and sound off, now (a stop; not a vamp's seam, which lets notes go). */
    void silence();
    /** Audio thread: back to how it started, reverb and chorus emptied (before playing again). */
    void reset();
    /** Audio thread: renders `numSamples` with the events in `midi` at their
        sample positions, adding gain * the result into left and right. */
    void renderAdding (const juce::MidiBuffer& midi, float* left, float* right, int numSamples, float startGain, float endGain);

    /** The 128 General MIDI program names, 0-based. */
    static const char* programName (int program);

private:
    GmSynth() = default;
    void* settings = nullptr;
    void* synth = nullptr;
    juce::AudioBuffer<float> scratch { 2, 2048 };
};

} // namespace perf
