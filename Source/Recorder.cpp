#include "Recorder.h"
#include <cstring>

namespace perf
{

using namespace juce;

Recorder::Recorder()
{
    queue.resize ((size_t) fifo.getTotalSize());
}

Recorder::~Recorder()
{
    stop();
}

bool Recorder::start (const File& where, const StringArray& outputNames, const StringArray& inputNames,
                      double rate, int maxBlockSize, String& error)
{
    stop();
    folder = where;
    if (! folder.createDirectory())
    {
        error = "Could not make " + folder.getFullPathName();
        return false;
    }
    sampleRate = rate > 0.0 ? rate : 48000.0;
    scratchL.assign ((size_t) jmax (64, maxBlockSize * 2), 0.0f);
    scratchR.assign (scratchL.size(), 0.0f);

    WavAudioFormat wav;
    for (int i = 0; i < outputNames.size(); ++i)
    {
        auto name = File::createLegalFileName (outputNames[i].trim());
        if (name.isEmpty()) name = "Output " + String (i + 1);
        const File f = folder.getNonexistentChildFile (name, ".wav", false);
        std::unique_ptr<FileOutputStream> os (f.createOutputStream());
        if (os == nullptr || ! os->openedOk())
        {
            error = "Could not write " + f.getFullPathName();
            writers.clear();
            return false;
        }
        std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (os.get(), sampleRate, 2, 24, {}, 0));
        if (w == nullptr) { error = "Could not make a WAV writer"; writers.clear(); return false; }
        os.release();
        // Four seconds of buffer: a disk that pauses for a moment loses nothing.
        writers.push_back (std::make_unique<AudioFormatWriter::ThreadedWriter> (w.release(), thread, (int) (sampleRate * 4.0)));
    }

    trackNames = inputNames;
    midiTracks.assign ((size_t) jmax (1, inputNames.size()), MidiMessageSequence());
    fifo.reset();
    midiDropped = 0;

    thread.startThread (Thread::Priority::normal);
    startMs = Time::getMillisecondCounterHiRes();
    recording.store (true);
    startTimer (500);
    return true;
}

void Recorder::stop()
{
    if (! recording.exchange (false)) return;
    stopTimer();
    drainMidi();
    writers.clear();            // each flushes its buffer and closes the file
    thread.stopThread (5000);

    /* The MIDI: one track per input, type 1, times kept in seconds -- the
       file says 120 bpm and places every event by the clock, so it reads
       back exactly when it happened whatever the tempo was. */
    bool any = false;
    for (auto& t : midiTracks) if (t.getNumEvents() > 0) any = true;
    if (any)
    {
        MidiFile mf;
        mf.setTicksPerQuarterNote (960);
        for (size_t i = 0; i < midiTracks.size(); ++i)
        {
            MidiMessageSequence track;
            const String name = i < (size_t) trackNames.size() ? trackNames[(int) i] : "Input " + String ((int) i + 1);
            track.addEvent (MidiMessage::textMetaEvent (3, name), 0.0);
            if (i == 0) track.addEvent (MidiMessage::tempoMetaEvent (500000), 0.0);     // 120 bpm: one beat is half a second
            for (int e = 0; e < midiTracks[i].getNumEvents(); ++e)
            {
                const auto& ev = midiTracks[i].getEventPointer (e)->message;
                track.addEvent (ev, ev.getTimeStamp() * 2.0 * 960.0);          // seconds -> ticks at 120 bpm
            }
            track.addEvent (MidiMessage::endOfTrack(), track.getEndTime() + 1.0);
            track.updateMatchedPairs();
            mf.addTrack (track);
        }
        const File f = folder.getChildFile ("keyboards.mid");
        if (auto os = std::unique_ptr<FileOutputStream> (f.createOutputStream()); os != nullptr && os->openedOk())
        {
            os->setPosition (0);
            os->truncate();
            mf.writeTo (*os, 1);
        }
    }
    midiTracks.clear();
}

double Recorder::elapsedSeconds() const
{
    return recording.load() ? (Time::getMillisecondCounterHiRes() - startMs) * 0.001 : 0.0;
}

void Recorder::writeAudio (int index, const float* l, const float* r, float gain, int n)
{
    if (! recording.load (std::memory_order_relaxed)) return;
    if (index < 0 || index >= (int) writers.size() || n <= 0 || (size_t) n > scratchL.size()) return;
    FloatVectorOperations::copyWithMultiply (scratchL.data(), l, gain, n);
    FloatVectorOperations::copyWithMultiply (scratchR.data(), r, gain, n);
    const float* chans[2] = { scratchL.data(), scratchR.data() };
    writers[(size_t) index]->write (chans, n);          // false when the disk is behind: that block is lost, nothing waits
}

void Recorder::writeMidi (int inputIndex, const MidiMessage& m)
{
    if (! recording.load (std::memory_order_relaxed) || m.getRawDataSize() > 3) return;
    int s1, n1, s2, n2;
    fifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 + n2 < 1) return;
    auto& e = queue[(size_t) (n1 > 0 ? s1 : s2)];
    e.seconds = (Time::getMillisecondCounterHiRes() - startMs) * 0.001;
    e.input = inputIndex;
    e.size = (uint8) m.getRawDataSize();
    std::memcpy (e.data, m.getRawData(), e.size);
    fifo.finishedWrite (1);
}

void Recorder::drainMidi()
{
    int s1, n1, s2, n2;
    fifo.prepareToRead (fifo.getNumReady(), s1, n1, s2, n2);
    auto take = [this] (int start, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            const auto& e = queue[(size_t) (start + i)];
            if (e.input < 0 || e.input >= (int) midiTracks.size()) continue;
            midiTracks[(size_t) e.input].addEvent (MidiMessage (e.data, (int) e.size, jmax (0.0, e.seconds)));
        }
    };
    take (s1, n1);
    take (s2, n2);
    fifo.finishedRead (n1 + n2);
}

void Recorder::timerCallback()
{
    drainMidi();
}

} // namespace perf
