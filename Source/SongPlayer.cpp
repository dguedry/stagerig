#include "SongPlayer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <juce_audio_basics/juce_audio_basics.h>

using namespace juce;

namespace perf
{

SongPlayer::SongPlayer (AudioFormatManager& f, const File& folder, const SongDef& song, double sr, int bs, const File& sf)
    : formats (f), soundfont (sf), sampleRate (sr > 0.0 ? sr : 48000.0), blockSize (jmax (16, bs))
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
        if (stem->midi)
        {
            stem->wantsGm = def.usesGm();
            stem->gmEvents.ensureSize (8192);
            for (int c = 0; c < 16; ++c) stem->lastDest[c] = -2;
            applyRoutes (*stem, def.channels);
        }
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
                /* Channel messages only: no sysex, no meta. Program changes
                   are kept for the GM player -- they are how the file says
                   which instrument plays -- and dropped on the way to your
                   own sounds (see dispatch). */
                if (! (m.isNoteOnOrOff() || m.isController() || m.isPitchWheel() || m.isAftertouch()
                       || m.isChannelPressure() || m.isProgramChange()))
                    continue;
                Stem::Event ev {};
                ev.at = (int64) (m.getTimeStamp() * sampleRate);
                ev.size = (uint8_t) jmin (3, m.getRawDataSize());
                std::memcpy (ev.data, m.getRawData(), ev.size);
                events.push_back (ev);
            }
    std::stable_sort (events.begin(), events.end(), [] (const Stem::Event& a, const Stem::Event& b) { return a.at < b.at; });
    s.midiLength = events.empty() ? 0 : events.back().at + 1;
    s.events = std::move (events);

    if (s.wantsGm)
    {
        String error;
        const auto sf = GmSynth::findSoundfont (soundfont);
        if (sf == File())
            problems.add (s.file.getFileName() + ": " + GmSynth::unavailableReason (soundfont));
        else if ((s.gm = GmSynth::create (sampleRate, sf, error)) == nullptr)
            problems.add (s.file.getFileName() + ": " + error);
    }
    return true;
}

void SongPlayer::dispatch (Stem& s, const MidiMessage& m, int at, const MidiDest& d)
{
    const int ch = m.getChannel();
    if (ch < 1 || ch > 16 || s.silent[ch - 1].load (std::memory_order_relaxed)) return;
    const auto kind = (MidiRoute::Kind) s.routeKind[ch - 1].load (std::memory_order_relaxed);
    if (kind == MidiRoute::Kind::gm)
    {
        // The editor's choices for this part replace the file's own.
        if (m.isProgramChange() && s.gmProgram[ch - 1].load (std::memory_order_relaxed) >= 0) return;
        if (m.isController())
        {
            const int cc = m.getControllerNumber();
            if ((cc == 0 || cc == 32) && s.gmProgram[ch - 1].load (std::memory_order_relaxed) >= 0) return;
            if (cc == 7  && s.volume[ch - 1].load (std::memory_order_relaxed) >= 0) return;
            if (cc == 10 && s.pan[ch - 1].load (std::memory_order_relaxed) >= 0) return;
        }
    }
    dispatchTo (s, m, at, kind, s.routeInput[ch - 1].load (std::memory_order_relaxed), d);
}

void SongPlayer::dispatchTo (Stem& s, const MidiMessage& m, int at, MidiRoute::Kind kind, int input, const MidiDest& d)
{
    const int ch = m.getChannel();
    if (kind == MidiRoute::Kind::gm)
    {
        if (s.gm != nullptr) s.gmEvents.addEvent (m, at);
        return;
    }
    if (kind == MidiRoute::Kind::mute) return;
    // Your own sounds keep their own patches: the file's instrument choices are for the GM player.
    if (m.isProgramChange() || (m.isController() && (m.getControllerNumber() == 0 || m.getControllerNumber() == 32))) return;
    if (kind == MidiRoute::Kind::keys)
    {
        if (input >= 0 && input < d.numInputs && d.inputMidi[input] != nullptr) d.inputMidi[input]->addEvent (m, at);
        return;
    }
    const int idx = s.defIndex * 16 + ch - 1;
    if (d.slotMidi != nullptr && idx < d.numSlotMidi && d.slotMidi[idx] != nullptr)
        d.slotMidi[idx]->addEvent (m, at);
}

void SongPlayer::midiForRun (Stem& s, int64 from, int count, int at, const MidiDest& d)
{
    // The first event at or after `from`; the list is in order.
    auto it = std::lower_bound (s.events.begin(), s.events.end(), from,
                                [] (const Stem::Event& e, int64 t) { return e.at < t; });
    for (; it != s.events.end() && it->at < from + count; ++it)
    {
        dispatch (s, MidiMessage (it->data, it->size), at + (int) (it->at - from), d);
        s.notesSounding = true;
    }
}

void SongPlayer::allNotesOff (Stem& s, int at, const MidiDest& d)
{
    for (int ch = 1; ch <= 16; ++ch)
    {
        dispatch (s, MidiMessage::allNotesOff (ch), at, d);
        dispatch (s, MidiMessage::controllerEvent (ch, 64, 0), at, d);     // a sustain held in the file must not ring on
    }
}

void SongPlayer::chase (Stem& s, int64 from, int at)
{
    if (s.gm == nullptr) return;
    /* Defaults first, then whatever the file set before `from`, latest wins:
       playing from a section in the middle must sound as it does when the
       song gets there from the top. */
    constexpr int ccs[] = { 0, 32, 1, 7, 10, 11, 91, 93 };
    int program[16], bend[16], cc[16][8];
    for (int c = 0; c < 16; ++c)
    {
        program[c] = 0; bend[c] = 8192;
        const int defaults[8] = { -1, -1, 0, 100, 64, 127, 40, 0 };
        for (int k = 0; k < 8; ++k) cc[c][k] = defaults[k];
    }
    for (const auto& e : s.events)
    {
        if (e.at >= from) break;
        const int status = e.data[0] & 0xf0, c = e.data[0] & 0x0f;
        if (status == 0xc0) program[c] = e.data[1];
        else if (status == 0xe0) bend[c] = e.data[1] | (e.data[2] << 7);
        else if (status == 0xb0)
            for (int k = 0; k < 8; ++k)
                if (ccs[k] == e.data[1]) cc[c][k] = e.data[2];
    }
    for (int c = 0; c < 16; ++c)
    {
        if ((MidiRoute::Kind) s.routeKind[c].load (std::memory_order_relaxed) != MidiRoute::Kind::gm) continue;
        for (int k = 0; k < 2; ++k)
            if (cc[c][k] >= 0) s.gmEvents.addEvent (MidiMessage::controllerEvent (c + 1, ccs[k], cc[c][k]), at);
        // The editor's instrument, volume and pan win over the file's.
        if (const int p = s.gmProgram[c].load (std::memory_order_relaxed); p >= 0)
        {
            if (c != 9) { s.gmEvents.addEvent (MidiMessage::controllerEvent (c + 1, 0, 0), at); s.gmEvents.addEvent (MidiMessage::controllerEvent (c + 1, 32, 0), at); }
            program[c] = p;
        }
        if (const int v = s.volume[c].load (std::memory_order_relaxed); v >= 0) cc[c][3] = v;
        if (const int p = s.pan[c].load (std::memory_order_relaxed); p >= 0)    cc[c][4] = p;
        s.gmEvents.addEvent (MidiMessage::programChange (c + 1, program[c]), at);
        for (int k = 2; k < 8; ++k)
            s.gmEvents.addEvent (MidiMessage::controllerEvent (c + 1, ccs[k], cc[c][k]), at);
        s.gmEvents.addEvent (MidiMessage::pitchWheel (c + 1, bend[c]), at);
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

void SongPlayer::cue (double seconds)
{
    if (getState() != State::idle) return;
    // Before loading has finished the length is not known yet: the cue is taken as it is.
    const auto wanted = jmax ((int64) 0, (int64) (seconds * sampleRate));
    const auto p = ready.load() ? jmin (wanted, lengthSamples) : wanted;
    cuedPosition.store (p);
    position.store (p);
    currentSection.store (sectionAt (p));
}

void SongPlayer::advance()
{
    if (vamping.load() && vampHold.load()) { releaseVamp(); return; }
    const int q = queuedSection.load();
    const int next = (q >= 0 ? q : currentSection.load()) + 1;
    if (next < numSections) queueSection (next);
}

void SongPlayer::setStemMix (int stemIndex, int output, float gainDb)
{
    for (auto& s : stems)
        if (s->defIndex == stemIndex)
        {
            s->output.store (jmax (0, output));
            s->gain.store (Decibels::decibelsToGain (gainDb, -60.0f));
        }
}

void SongPlayer::setStemRoutes (int stemIndex, const std::array<MidiRoute, 16>& routes)
{
    for (auto& s : stems)
        if (s->defIndex == stemIndex && s->midi)
        {
            applyRoutes (*s, routes);
            s->chaseRequested.store (true);
        }
}

void SongPlayer::applyRoutes (Stem& s, const std::array<MidiRoute, 16>& routes)
{
    bool anySolo = false;
    for (auto& r : routes) anySolo = anySolo || r.solo;
    for (int c = 0; c < 16; ++c)
    {
        const auto& r = routes[(size_t) c];
        s.routeKind[c].store ((int) r.kind);
        s.routeInput[c].store (jmax (0, r.input));
        s.gmProgram[c].store (r.gmProgram);
        s.volume[c].store (r.volume);
        s.pan[c].store (r.pan);
        s.silent[c].store (r.isSilent (anySolo));
    }
}

bool SongPlayer::stemHasGm (int stemIndex) const
{
    if (! ready.load()) return false;
    for (auto& s : stems)
        if (s->defIndex == stemIndex) return s->gm != nullptr;
    return false;
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

SongPlayer::Happened SongPlayer::render (float* (*outputs)[2], int numOutputs, MidiBuffer* const* inputMidi, int numInputs, int n,
                                         MidiBuffer* const* slotMidi, int numSlotMidi)
{
    auto st = getState();
    if (stems.empty()) return Happened::nothing;
    if (n > scratch.getNumSamples()) return Happened::nothing;    // larger than prepared: never allocate here
    if (st == State::idle)
    {
        // After the end: the GM player's last notes ring out rather than stop dead.
        if (gmTail > 0 && ready.load())
        {
            static const MidiBuffer none;
            for (auto& s : stems)
                if (s->gm != nullptr)
                    if (const int o = s->output.load (std::memory_order_relaxed); o >= 0 && o < numOutputs && outputs[o][0] != nullptr && outputs[o][1] != nullptr)
                    {
                        const float g = s->gain.load (std::memory_order_relaxed);
                        s->gm->renderAdding (none, outputs[o][0], outputs[o][1], n, g, g);
                    }
            gmTail -= n;
        }
        return Happened::nothing;
    }
    const MidiDest dest { inputMidi, numInputs, slotMidi, numSlotMidi };

    Happened happened = Happened::nothing;

    if (st == State::waiting)
    {
        if (! ready.load()) return Happened::nothing;
        countInTotalBeats = pendingCountInBeats.load();
        beatSamples = jmax ((int64) 1, (int64) (pendingBeatSeconds.load() * sampleRate));
        countInRemaining = (int64) countInTotalBeats * beatSamples;
        countInDone = 0;
        lastRunEnd = -1;
        gmTail = 0;
        // A fresh start: nothing left sounding from last time, and every GM channel set as it is at the start point.
        for (auto& stem : stems)
            if (stem->midi) { stem->needsChase = true; if (stem->gm != nullptr) stem->gm->reset(); }
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
            s->gmEvents.clear();
            if (s->chaseRequested.exchange (false)) s->needsChase = true;
            /* A part muted, soloed away or sent somewhere else: whatever it
               left sounding where it was is let go there. */
            for (int c = 0; c < 16; ++c)
            {
                const auto kind = (MidiRoute::Kind) s->routeKind[c].load (std::memory_order_relaxed);
                const int input = s->routeInput[c].load (std::memory_order_relaxed);
                const int now = s->silent[c].load (std::memory_order_relaxed) ? -1 : (int) kind * 256 + input;
                if (s->lastDest[c] >= 0 && s->lastDest[c] != now)
                {
                    const auto wasKind = (MidiRoute::Kind) (s->lastDest[c] / 256);
                    dispatchTo (*s, MidiMessage::allNotesOff (c + 1), offset, wasKind, s->lastDest[c] % 256, dest);
                    dispatchTo (*s, MidiMessage::controllerEvent (c + 1, 64, 0), offset, wasKind, s->lastDest[c] % 256, dest);
                }
                s->lastDest[c] = now;
            }
            for (int r = 0; r < numRuns; ++r)
            {
                /* A run that does not follow on from the one before -- a vamp
                   going round, a jump -- starts with every note let go, or a
                   note held across the seam would hang; the GM player then
                   catches up on what the file had set by the new place. */
                const bool seam = r > 0 || (lastRunEnd >= 0 && runs[0].from != lastRunEnd);
                if (seam && s->notesSounding) { allNotesOff (*s, runs[r].at, dest); s->notesSounding = false; }
                if (seam || s->needsChase) { chase (*s, runs[r].from, runs[r].at); s->needsChase = false; }
                if (st != State::stopping)
                    midiForRun (*s, runs[r].from, runs[r].count, runs[r].at, dest);
            }
            if ((st == State::stopping || ended) && s->notesSounding) { allNotesOff (*s, n - 1, dest); s->notesSounding = false; }

            if (s->gm != nullptr)
                if (const int o = s->output.load (std::memory_order_relaxed); o >= 0 && o < numOutputs && outputs[o][0] != nullptr && outputs[o][1] != nullptr)
                {
                    const float g = s->gain.load (std::memory_order_relaxed);
                    s->gm->renderAdding (s->gmEvents, outputs[o][0], outputs[o][1], n, g * startGain, g * endGain);
                }
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
        gmTail = (int64) (3.0 * sampleRate);
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
