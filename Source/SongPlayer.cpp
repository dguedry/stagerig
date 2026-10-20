#include "SongPlayer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <juce_audio_basics/juce_audio_basics.h>

using namespace juce;

namespace perf
{

SongPlayer::SongPlayer (AudioFormatManager& f, const File& folder, const SongDef& song, double sr, int bs)
    : formats (f), sampleRate (sr > 0.0 ? sr : 48000.0), blockSize (jmax (16, bs))
{
    for (int i = 0; i < (int) song.stems.size(); ++i)
    {
        const auto& def = song.stems[(size_t) i];
        auto stem = std::make_unique<Stem>();
        stem->defIndex = i;
        stem->file = folder.getChildFile (def.file);
        stem->output.store (jmax (0, def.output));
        stem->gain.store (Decibels::decibelsToGain (def.gainDb, -60.0f));
        stem->midi = def.isMidi();
        stem->midiChannel = def.midiChannel;
        stem->input.store (jmax (0, def.input));
        stems.push_back (std::move (stem));
    }
    numStemDefs = (int) stems.size();
    fadeLength = jmax (1, (int) (0.03 * sampleRate));
    scratch.setSize (2, blockSize, false, true, true);
    setMarkers (song.markers);
    startLoading();
}

SongPlayer::~SongPlayer()
{
    stopLoading();
}

String SongPlayer::getProblems() const
{
    return ready.load() ? problems.joinIntoString ("; ") : String();
}

//==============================================================================
void SongPlayer::startLoading()
{
    stopLoading();
    ready.store (false);
    loadProgress.store (0.0f);
    loaderStop.store (false);
    loader = std::thread ([this] { load(); });
}

void SongPlayer::stopLoading()
{
    loaderStop.store (true);
    if (loader.joinable()) loader.join();
}

void SongPlayer::load()
{
    problems.clear();
    int64 longest = 0;
    for (size_t i = 0; i < stems.size() && ! loaderStop.load(); ++i)
    {
        auto& s = *stems[i];
        if (s.midi ? loadMidiStem (s) : loadStem (s))
            longest = jmax (longest, s.midi ? s.midiLength : (int64) s.left.size());
        loadProgress.store ((float) (i + 1) / (float) stems.size());
    }
    if (loaderStop.load()) return;
    lengthSamples = longest;
    ready.store (true);
}

bool SongPlayer::loadStem (Stem& s)
{
    std::unique_ptr<AudioFormatReader> reader (s.file.existsAsFile() ? formats.createReaderFor (s.file) : nullptr);
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
    {
        problems.add (s.file.getFileName() + ": " + (s.file.existsAsFile() ? "not an audio file StageRig can read" : "file missing"));
        s.left.clear(); s.right.clear();
        return false;
    }

    const double ratio = reader->sampleRate / sampleRate;           // input samples per output sample
    const int64 outLength = (int64) std::ceil ((double) reader->lengthInSamples / ratio);
    s.left.assign ((size_t) outLength, 0);
    s.right.assign ((size_t) outLength, 0);

    /* Read in chunks and resample as we go, so a long stem never sits in
       memory as floats at the file's rate as well. */
    constexpr int chunk = 65536;
    AudioBuffer<float> in (2, chunk + 8);
    std::vector<float> outL ((size_t) ((double) chunk / ratio + 16.0)), outR (outL.size());
    LagrangeInterpolator interpL, interpR;
    int64 readPos = 0, written = 0;
    int carried = 0;                                                  // input samples left over from the last chunk

    auto store = [&s, &written, outLength] (const float* l, const float* r, int n)
    {
        for (int i = 0; i < n && written < outLength; ++i, ++written)
        {
            s.left [(size_t) written] = (int16_t) jlimit (-32767.0f, 32767.0f, l[i] * 32767.0f);
            s.right[(size_t) written] = (int16_t) jlimit (-32767.0f, 32767.0f, r[i] * 32767.0f);
        }
    };

    while (readPos < reader->lengthInSamples && ! loaderStop.load())
    {
        const int want = (int) jmin ((int64) (chunk - carried), reader->lengthInSamples - readPos);
        // A mono file fills both sides.
        reader->read (&in, carried, want, readPos, true, true);
        readPos += want;
        const int available = carried + want;
        const bool last = readPos >= reader->lengthInSamples;

        if (std::abs (ratio - 1.0) < 1e-9)
        {
            store (in.getReadPointer (0), in.getReadPointer (1), available);
            carried = 0;
            continue;
        }

        // Keep a few input samples back for the interpolator, except at the very end.
        const int usable = last ? available : jmax (0, available - 8);
        const int outCount = jmin ((int) outL.size(), (int) ((double) usable / ratio));
        if (outCount <= 0) { carried = available; continue; }
        const int used = interpL.process (ratio, in.getReadPointer (0), outL.data(), outCount);
        interpR.process (ratio, in.getReadPointer (1), outR.data(), outCount);
        store (outL.data(), outR.data(), outCount);

        carried = jmax (0, available - used);
        if (carried > 0)
            for (int c = 0; c < 2; ++c)
                std::memmove (in.getWritePointer (c), in.getReadPointer (c) + used, (size_t) carried * sizeof (float));
    }
    s.left.resize ((size_t) written);
    s.right.resize ((size_t) written);
    return ! loaderStop.load();
}

bool SongPlayer::loadMidiStem (Stem& s)
{
    MidiFile mf;
    FileInputStream in (s.file);
    if (! s.file.existsAsFile() || ! in.openedOk() || ! mf.readFrom (in))
    {
        problems.add (s.file.getFileName() + ": " + (s.file.existsAsFile() ? "not a MIDI file StageRig can read" : "file missing"));
        s.events.clear();
        return false;
    }
    // Times in seconds by the file's own tempo map, then device samples.
    mf.convertTimestampTicksToSeconds();

    std::vector<Stem::Event> events;
    for (int t = 0; t < mf.getNumTracks(); ++t)
        if (auto* track = mf.getTrack (t))
            for (const auto* e : *track)
            {
                const auto& m = e->message;
                /* What a keyboard sends, nothing else: no program changes (a
                   backing file must not swap the sounds), no sysex, no meta. */
                if (! (m.isNoteOnOrOff() || m.isController() || m.isPitchWheel() || m.isAftertouch() || m.isChannelPressure()))
                    continue;
                if (s.midiChannel > 0 && m.getChannel() != s.midiChannel) continue;
                Stem::Event ev {};
                ev.at = (int64) (m.getTimeStamp() * sampleRate);
                ev.size = (uint8_t) jmin (3, m.getRawDataSize());
                std::memcpy (ev.data, m.getRawData(), ev.size);
                events.push_back (ev);
            }
    std::stable_sort (events.begin(), events.end(), [] (const Stem::Event& a, const Stem::Event& b) { return a.at < b.at; });
    s.midiLength = events.empty() ? 0 : events.back().at + 1;
    s.events = std::move (events);
    return true;
}

void SongPlayer::midiForRun (Stem& s, MidiBuffer& out, int64 from, int count, int at)
{
    // The first event at or after `from`; the list is in order.
    auto it = std::lower_bound (s.events.begin(), s.events.end(), from,
                                [] (const Stem::Event& e, int64 t) { return e.at < t; });
    for (; it != s.events.end() && it->at < from + count; ++it)
    {
        out.addEvent (it->data, it->size, at + (int) (it->at - from));
        s.notesSounding = true;
    }
}

void SongPlayer::allNotesOff (MidiBuffer& out, int at)
{
    for (int ch = 1; ch <= 16; ++ch)
    {
        out.addEvent (MidiMessage::allNotesOff (ch), at);
        out.addEvent (MidiMessage::controllerEvent (ch, 64, 0), at);     // a sustain held in the file must not ring on
    }
}

//==============================================================================
void SongPlayer::prepare (double sr, int bs)
{
    const double newRate = sr > 0.0 ? sr : 48000.0;
    const bool rateChanged = std::abs (newRate - sampleRate) > 0.5;
    blockSize = jmax (16, bs);
    scratch.setSize (2, blockSize, false, true, true);
    if (! rateChanged) return;

    // A new rate: everything is stopped and loaded again at it.
    state.store ((int) State::idle);
    sampleRate = newRate;
    fadeLength = jmax (1, (int) (0.03 * sampleRate));
    for (int i = 0; i < numSections; ++i)
        sectionStart[i] = (int64) (sectionSeconds[i] * sampleRate);
    startLoading();
}

void SongPlayer::setMarkers (const std::vector<MarkerDef>& markers)
{
    std::vector<MarkerDef> sorted (markers);
    std::sort (sorted.begin(), sorted.end(), [] (const MarkerDef& a, const MarkerDef& b) { return a.seconds < b.seconds; });

    /* Whatever comes before the first marker is a section of its own -- the
       intro -- unless the first marker is at the very top. */
    int n = 0;
    if (sorted.empty() || sorted.front().seconds > 0.001)
    {
        sectionSeconds[0] = 0.0;
        sectionName[0] = sorted.empty() ? "Song" : "Start";
        sectionVamp[0] = false;
        n = 1;
    }
    for (const auto& m : sorted)
    {
        if (n >= kMaxSections) break;
        sectionSeconds[n] = jmax (0.0, m.seconds);
        sectionName[n] = m.name.isNotEmpty() ? m.name : "Section " + String (n + 1);
        sectionVamp[n] = m.vamp;
        ++n;
    }
    for (int i = 0; i < n; ++i)
        sectionStart[i] = (int64) (sectionSeconds[i] * sampleRate);
    numSections = n;
    if (queuedSection.load() >= numSections) queuedSection.store (-1);
    if (currentSection.load() >= numSections) currentSection.store (0);
}

String SongPlayer::getSectionName (int i) const     { return i >= 0 && i < numSections ? sectionName[i] : String(); }
double SongPlayer::getSectionStart (int i) const    { return i >= 0 && i < numSections ? sectionSeconds[i] : 0.0; }
bool SongPlayer::isSectionVamp (int i) const        { return i >= 0 && i < numSections && sectionVamp[i]; }

int SongPlayer::sectionAt (int64 pos) const
{
    int s = 0;
    for (int i = 0; i < numSections; ++i)
        if (sectionStart[i] <= pos) s = i;
    return s;
}

int64 SongPlayer::sectionEnd (int s) const
{
    return s + 1 < numSections ? jmin (sectionStart[s + 1], lengthSamples) : lengthSamples;
}

//==============================================================================
void SongPlayer::start (int beats, double beatSeconds, int output)
{
    if (getState() != State::idle) return;
    pendingCountInBeats.store (jmax (0, beats));
    pendingBeatSeconds.store (jmax (0.05, beatSeconds));
    countInOutput.store (jmax (0, output));
    position.store (cuedPosition.load());
    currentSection.store (sectionAt (cuedPosition.load()));
    vampHold.store (isSectionVamp (currentSection.load()));
    vamping.store (false);
    queuedSection.store (-1);
    fadeRemaining = 0;
    state.store ((int) State::waiting);            // the audio thread begins the count-in once loading is done
}

void SongPlayer::stop()
{
    int expected = (int) State::playing;
    if (! state.compare_exchange_strong (expected, (int) State::stopping))
    {
        expected = (int) State::countIn;
        if (! state.compare_exchange_strong (expected, (int) State::idle))
        {
            expected = (int) State::waiting;
            state.compare_exchange_strong (expected, (int) State::idle);
        }
    }
    cuedPosition.store (0);
    queuedSection.store (-1);
    vamping.store (false);
}

void SongPlayer::queueSection (int index)
{
    if (index < 0 || index >= numSections) { queuedSection.store (-1); return; }
    if (getState() == State::idle)
    {
        // Stopped: this is where Play will start.
        cuedPosition.store (sectionStart[index]);
        position.store (sectionStart[index]);
        currentSection.store (index);
        queuedSection.store (-1);
        return;
    }
    queuedSection.store (index);
}

void SongPlayer::advance()
{
    if (vamping.load() && vampHold.load()) { releaseVamp(); return; }
    const int q = queuedSection.load();
    const int next = (q >= 0 ? q : currentSection.load()) + 1;
    if (next < numSections) queueSection (next);
}

void SongPlayer::setStemMix (int stemIndex, int output, float gainDb, int input)
{
    for (auto& s : stems)
        if (s->defIndex == stemIndex)
        {
            s->input.store (jmax (0, input));
            s->output.store (jmax (0, output));
            s->gain.store (Decibels::decibelsToGain (gainDb, -60.0f));
        }
}

//==============================================================================
void SongPlayer::addClicks (float* l, float* r, int n)
{
    /* A short sine blip on every beat, higher on the first: the sound of a
       drummer's sticks, enough to count in by on an in-ear mix. */
    const int clickLen = (int) (0.03 * sampleRate);
    for (int i = 0; i < n; ++i)
    {
        const int64 t = countInDone + i;
        const int64 beat = t / beatSamples;
        const int64 inBeat = t % beatSamples;
        if (beat >= countInTotalBeats || inBeat >= clickLen) continue;
        const double freq = beat == 0 ? 1600.0 : 1000.0;
        const float env = 1.0f - (float) inBeat / (float) clickLen;
        const float v = 0.5f * env * (float) std::sin (MathConstants<double>::twoPi * freq * (double) inBeat / sampleRate);
        if (l != nullptr) l[i] += v;
        if (r != nullptr) r[i] += v;
    }
}

SongPlayer::Happened SongPlayer::render (float* (*outputs)[2], int numOutputs, MidiBuffer* const* inputMidi, int numInputs, int n)
{
    auto st = getState();
    if (st == State::idle || stems.empty()) return Happened::nothing;
    if (n > scratch.getNumSamples()) return Happened::nothing;    // larger than prepared: never allocate here

    Happened happened = Happened::nothing;

    if (st == State::waiting)
    {
        if (! ready.load()) return Happened::nothing;
        countInTotalBeats = pendingCountInBeats.load();
        beatSamples = jmax ((int64) 1, (int64) (pendingBeatSeconds.load() * sampleRate));
        countInRemaining = (int64) countInTotalBeats * beatSamples;
        countInDone = 0;
        lastRunEnd = -1;
        if (countInRemaining > 0) { state.store ((int) State::countIn); st = State::countIn; }
        else                      { state.store ((int) State::playing); st = State::playing; happened = Happened::stemsStarted; }
    }

    int offset = 0;
    if (st == State::countIn)
    {
        const int now = (int) jmin ((int64) n, countInRemaining);
        if (const int o = countInOutput.load(); o >= 0 && o < numOutputs)
            addClicks (outputs[o][0], outputs[o][1], now);
        countInDone += now;
        countInRemaining -= now;
        offset = now;
        if (countInRemaining > 0) return Happened::nothing;
        state.store ((int) State::playing);
        st = State::playing;
        happened = Happened::stemsStarted;
    }

    const int len = n - offset;
    if (len <= 0) return happened;

    /* Where each part of this block comes from. Usually one run; a section
       boundary inside the block -- a vamp going round, a queued jump, the
       end -- splits it, and each part reads from its own place. */
    struct Run { int at; int64 from; int count; };
    Run runs[16];
    int numRuns = 0;
    bool ended = false;
    int64 pos = position.load();
    int done = 0;
    while (done < len && numRuns < 16)
    {
        const int sec = sectionAt (pos);
        const int64 end = sectionEnd (sec);
        const int count = (int) jmin ((int64) (len - done), jmax ((int64) 0, end - pos));
        if (count > 0) { runs[numRuns++] = { offset + done, pos, count }; done += count; pos += count; }
        if (pos < end) break;

        // At the end of section `sec`: a queued section, a vamp going round, the next section, or the end.
        if (const int q = queuedSection.exchange (-1); q >= 0)
        {
            pos = sectionStart[q];
            vampHold.store (sectionVamp[q]);
        }
        else if (sectionVamp[sec] && vampHold.load())
        {
            pos = sectionStart[sec];
        }
        else if (end >= lengthSamples)
        {
            ended = true;
            break;
        }
        else
        {
            vampHold.store (sectionVamp[sectionAt (pos)]);
        }
        if (count == 0 && sectionEnd (sectionAt (pos)) <= pos) { ended = true; break; }   // nothing left to play
    }
    position.store (pos);
    const int sec = sectionAt (pos);
    currentSection.store (sec);
    vamping.store (sectionVamp[sec] && vampHold.load());

    // Fading out: a short ramp, so a stop never clicks.
    float startGain = 1.0f, endGain = 1.0f;
    if (st == State::stopping)
    {
        if (fadeRemaining <= 0) fadeRemaining = fadeLength;
        const int step = jmin (len, fadeRemaining);
        startGain = (float) fadeRemaining / (float) fadeLength;
        endGain = (float) (fadeRemaining - step) / (float) fadeLength;
        fadeRemaining -= step;
    }

    for (auto& s : stems)
    {
        if (s->midi)
        {
            const int in = s->input.load (std::memory_order_relaxed);
            if (in < 0 || in >= numInputs || inputMidi[in] == nullptr) continue;
            auto& out = *inputMidi[in];
            for (int r = 0; r < numRuns; ++r)
            {
                /* A run that does not follow on from the one before -- a vamp
                   going round, a jump -- starts with every note let go, or a
                   note held across the seam would hang. */
                const bool seam = r > 0 || (lastRunEnd >= 0 && runs[0].from != lastRunEnd);
                if (seam && s->notesSounding) { allNotesOff (out, runs[r].at); s->notesSounding = false; }
                if (st != State::stopping)
                    midiForRun (*s, out, runs[r].from, runs[r].count, runs[r].at);
            }
            if ((st == State::stopping || ended) && s->notesSounding) { allNotesOff (out, n - 1); s->notesSounding = false; }
            continue;
        }

        const int o = s->output.load (std::memory_order_relaxed);
        if (o < 0 || o >= numOutputs || s->left.empty()) continue;
        const float g = s->gain.load (std::memory_order_relaxed) / 32767.0f;
        const int64 stemLen = (int64) s->left.size();

        scratch.clear (0, n);
        for (int r = 0; r < numRuns; ++r)
        {
            const auto& run = runs[r];
            auto* l = scratch.getWritePointer (0) + run.at;
            auto* rr = scratch.getWritePointer (1) + run.at;
            const int count = (int) jmax ((int64) 0, jmin ((int64) run.count, stemLen - run.from));
            for (int i = 0; i < count; ++i)
            {
                l[i]  = (float) s->left [(size_t) (run.from + i)];
                rr[i] = (float) s->right[(size_t) (run.from + i)];
            }
        }
        for (int c = 0; c < 2; ++c)
            if (auto* dest = outputs[o][c])
            {
                const float* src = scratch.getReadPointer (c);
                if (startGain == endGain)
                    FloatVectorOperations::addWithMultiply (dest + offset, src + offset, g * startGain, len);
                else
                    for (int i = 0; i < len; ++i)
                        dest[offset + i] += src[offset + i] * g * (startGain + (endGain - startGain) * (float) i / (float) len);
            }
    }

    if (numRuns > 0) lastRunEnd = runs[numRuns - 1].from + runs[numRuns - 1].count;

    if (st == State::stopping && fadeRemaining <= 0)
    {
        state.store ((int) State::idle);
        position.store (0);
        currentSection.store (0);
        return Happened::stopped;
    }
    if (ended)
    {
        state.store ((int) State::idle);
        position.store (0);
        cuedPosition.store (0);
        currentSection.store (0);
        vamping.store (false);
        return Happened::ended;
    }
    return happened;
}

} // namespace perf
