#include "SongPlayer.h"
#include <cmath>

using namespace juce;

namespace perf
{

SongPlayer::SongPlayer (TimeSliceThread& readAhead, AudioFormatManager& formats,
                        const File& folder, const SongDef& song, double sr, int bs,
                        StringArray& problems)
    : sampleRate (sr > 0.0 ? sr : 48000.0), blockSize (jmax (16, bs))
{
    for (int index = 0; index < (int) song.stems.size(); ++index)
    {
        const auto& def = song.stems[(size_t) index];
        const auto file = folder.getChildFile (def.file);
        auto* reader = file.existsAsFile() ? formats.createReaderFor (file) : nullptr;
        if (reader == nullptr)
        {
            problems.add (def.displayName() + ": " + (file.existsAsFile() ? "not an audio file StageRig can read" : "file missing"));
            continue;
        }

        auto stem = std::make_unique<Stem>();
        stem->defIndex = index;
        stem->fileRate = reader->sampleRate;
        stem->fileLength = reader->lengthInSamples;
        stem->reader = std::make_unique<AudioFormatReaderSource> (reader, true);
        /* Four seconds ahead: enough to ride out a busy disk, small enough that
           a dozen stems cost a few megabytes. */
        stem->buffered = std::make_unique<BufferingAudioSource> (stem->reader.get(), readAhead, false,
                                                                 (int) jmax (8192.0, reader->sampleRate * 4.0), 2, true);
        stem->resampled = std::make_unique<ResamplingAudioSource> (stem->buffered.get(), false, 2);
        stem->output.store (jmax (0, def.output));
        stem->gain.store (Decibels::decibelsToGain (def.gainDb, -60.0f));
        stems.push_back (std::move (stem));
    }
    prepare (sampleRate, blockSize);
}

SongPlayer::~SongPlayer()
{
    for (auto& s : stems)
    {
        s->resampled->releaseResources();
        s->buffered->releaseResources();
    }
}

void SongPlayer::prepare (double sr, int bs)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;
    blockSize = jmax (16, bs);
    lengthSamples = 0;
    fadeLength = jmax (1, (int) (0.03 * sampleRate));
    for (auto& s : stems)
    {
        s->resampled->setResamplingRatio (s->fileRate / sampleRate);
        s->resampled->prepareToPlay (blockSize, sampleRate);
        s->scratch.setSize (2, blockSize, false, true, true);
        lengthSamples = jmax (lengthSamples, (int64) ((double) s->fileLength * sampleRate / s->fileRate));
    }
}

void SongPlayer::rewind()
{
    jassert (getState() == State::idle);
    for (auto& s : stems)
    {
        s->buffered->setNextReadPosition (0);
        s->resampled->flushBuffers();
    }
    position.store (0);
}

bool SongPlayer::waitUntilReady (int timeoutMs)
{
    bool all = true;
    for (auto& s : stems)
    {
        AudioBuffer<float> probe (2, blockSize);
        AudioSourceChannelInfo info (&probe, 0, blockSize);
        all = s->buffered->waitForNextAudioBlockReady (info, (uint32) timeoutMs) && all;
    }
    return all;
}

void SongPlayer::start (int beats, double beatSeconds, int output)
{
    if (getState() != State::idle) return;
    countInTotalBeats = jmax (0, beats);
    beatSamples = jmax ((int64) 1, (int64) (beatSeconds * sampleRate));
    countInRemaining = (int64) countInTotalBeats * beatSamples;
    countInDone = 0;
    countInOutput.store (jmax (0, output));
    fadeRemaining = 0;
    state.store ((int) (countInRemaining > 0 ? State::countIn : State::playing));
}

void SongPlayer::stop()
{
    int expected = (int) State::playing;
    if (state.compare_exchange_strong (expected, (int) State::stopping)) return;
    expected = (int) State::countIn;
    state.compare_exchange_strong (expected, (int) State::idle);    // nothing sounding yet but clicks
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

SongPlayer::Happened SongPlayer::render (float* (*outputs)[2], int numOutputs, int n)
{
    auto st = getState();
    if (st == State::idle || stems.empty()) return Happened::nothing;

    int offset = 0;
    Happened happened = Happened::nothing;

    if (st == State::countIn)
    {
        const int now = (int) jmin ((int64) n, countInRemaining);
        if (const int o = countInOutput.load(); o >= 0 && o < numOutputs)
            addClicks (outputs[o][0], outputs[o][1], now);
        countInDone += now;
        countInRemaining -= now;
        offset = now;
        if (countInRemaining > 0) return Happened::nothing;
        // The stems start inside this block, right where the count-in ends.
        state.store ((int) State::playing);
        st = State::playing;
        happened = Happened::stemsStarted;
    }
    else if (st == State::playing && position.load() == 0)
    {
        happened = Happened::stemsStarted;
    }

    const int len = n - offset;
    if (len <= 0) return happened;

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
        if (s->scratch.getNumSamples() < len) continue;       // a device block larger than prepared: skip, never allocate
        AudioSourceChannelInfo info (&s->scratch, 0, len);
        s->resampled->getNextAudioBlock (info);
        const int o = s->output.load (std::memory_order_relaxed);
        if (o < 0 || o >= numOutputs) continue;
        const float g = s->gain.load (std::memory_order_relaxed);
        for (int c = 0; c < 2; ++c)
            if (auto* dest = outputs[o][c])
            {
                if (startGain == endGain)
                    FloatVectorOperations::addWithMultiply (dest + offset, s->scratch.getReadPointer (c), g * startGain, len);
                else
                    for (int i = 0; i < len; ++i)
                        dest[offset + i] += s->scratch.getSample (c, i) * g
                                          * (startGain + (endGain - startGain) * (float) i / (float) len);
            }
    }

    const auto pos = position.load() + len;
    position.store (pos);

    if (st == State::stopping && fadeRemaining <= 0)
    {
        state.store ((int) State::idle);
        return Happened::stopped;
    }
    if (pos >= lengthSamples)
    {
        state.store ((int) State::idle);
        return Happened::ended;
    }
    return happened;
}

} // namespace perf
