#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <memory>
#include <vector>

namespace perf
{

/** Records the gig: every output to its own WAV, exactly as it left for the
    desk (after its master chain and volume), and everything the keyboards
    sent to a MIDI file with a track per input -- so a rehearsal can be
    listened back to, and a night that went well can be kept.

    One take is a folder: the setup's recordings folder, then the date and
    time, then "Main.wav", "In-ears.wav", "keyboards.mid". Audio is written
    by a background thread from a lock-free buffer; the audio thread only
    copies. MIDI goes through a lock-free queue drained by the same thread. */
class Recorder : private juce::Timer
{
public:
    Recorder();
    ~Recorder() override;

    /** Message thread: starts a take into `folder` (made if needed), one WAV
        per name in `outputNames`, at `sampleRate`. False, with why, if a
        file could not be made. */
    bool start (const juce::File& folder, const juce::StringArray& outputNames, const juce::StringArray& inputNames,
                double sampleRate, int maxBlockSize, juce::String& error);
    /** Message thread: finishes the files. */
    void stop();

    bool isRecording() const                      { return recording.load(); }
    double elapsedSeconds() const;
    juce::File getFolder() const                  { return folder; }

    /** Audio thread: output `index`'s stereo mix for this block, with the gain it leaves at. */
    void writeAudio (int index, const float* l, const float* r, float gain, int numSamples);
    /** Any thread: a MIDI message that arrived on input `inputIndex`, stamped now. */
    void writeMidi (int inputIndex, const juce::MidiMessage&);

private:
    void timerCallback() override;
    void drainMidi();

    juce::TimeSliceThread thread { "StageRig recording" };
    std::vector<std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>> writers;
    std::vector<float> scratchL, scratchR;

    struct MidiEv { double seconds; int input; juce::uint8 data[3]; juce::uint8 size; };
    juce::AbstractFifo fifo { 8192 };
    std::vector<MidiEv> queue;
    std::vector<juce::MidiMessageSequence> midiTracks;
    juce::StringArray trackNames;
    int midiDropped = 0;

    std::atomic<bool> recording { false };
    double startMs = 0.0, sampleRate = 48000.0;
    juce::File folder;
};

} // namespace perf
