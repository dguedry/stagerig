#include "MidiFx.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <bitset>
#include <cstring>

namespace perf
{

using namespace juce;

const char* const MidiFx::arpRates[]   = { "1/4", "1/8", "1/16", "1/8 triplet", "1/32" };
const char* const MidiFx::arpModes[]   = { "Up", "Down", "Up and down", "Random", "As played" };
const char* const MidiFx::chordTypes[] = { "Major", "Minor", "Dominant 7th", "Major 7th", "Minor 7th", "Sus 2", "Sus 4", "Power chord (5th)", "Octave", "Octaves above and below" };
const char* const MidiFx::scaleModes[] = { "Major", "Natural minor", "Harmonic minor", "Dorian", "Mixolydian", "Lydian", "Phrygian",
                                           "Major pentatonic", "Minor pentatonic", "Blues", "Whole tone" };
const char* const MidiFx::noteNames[]  = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
const char* const MidiFx::bandDegrees[] = { "I", "bII", "II", "bIII", "III", "IV", "#IV", "V", "bVI", "VI", "bVII", "VII" };
int MidiFx::numArpRates()   { return 5; }
int MidiFx::numArpModes()   { return 5; }
int MidiFx::numChordTypes() { return 10; }
int MidiFx::numScaleModes() { return 11; }

namespace
{
    const double kRateBeats[] = { 1.0, 0.5, 0.25, 1.0 / 3.0, 0.125 };

    const std::vector<int> kChordIntervals[] = {
        { 0, 4, 7 }, { 0, 3, 7 }, { 0, 4, 7, 10 }, { 0, 4, 7, 11 }, { 0, 3, 7, 10 },
        { 0, 2, 7 }, { 0, 5, 7 }, { 0, 7 }, { 0, 12 }, { -12, 0, 12 } };

    const std::vector<int> kScaleSteps[] = {
        { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 }, { 0, 2, 3, 5, 7, 8, 11 }, { 0, 2, 3, 5, 7, 9, 10 },
        { 0, 2, 4, 5, 7, 9, 10 }, { 0, 2, 4, 6, 7, 9, 11 }, { 0, 1, 3, 5, 7, 8, 10 },
        { 0, 2, 4, 7, 9 }, { 0, 3, 5, 7, 10 }, { 0, 3, 5, 6, 7, 10 }, { 0, 2, 4, 6, 8, 10 } };

    //==========================================================================
    class Arpeggiator : public MidiFx
    {
    public:
        void configure (const EffectDef& d) override
        {
            rate.store (jlimit (0, numArpRates() - 1, d.arpRate));
            mode.store (jlimit (0, numArpModes() - 1, d.arpMode));
            octaves.store (jlimit (1, 4, d.arpOctaves));
            gate.store (jlimit (5, 100, d.arpGate));
            latch.store (d.arpLatch);
        }

        void reset() override
        {
            numHeld = 0; numPressed = 0; sounding = -1; step = 0; samplesToStep = 0; samplesToOff = -1;
        }
        void restartTiming() override { step = 0; samplesToStep = 0; }       // the pattern from its top, on the track's beat

        void process (const MidiBuffer& in, MidiBuffer& out, int numSamples, double sampleRate, double bpm) override
        {
            // Notes are taken; everything else carries on.
            for (const auto meta : in)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOn())
                {
                    if (latch.load() && numPressed == 0) numHeld = 0;      // a new chord after letting go replaces the old
                    add (m.getNoteNumber(), (int) m.getVelocity(), m.getChannel());
                    ++numPressed;
                }
                else if (m.isNoteOff())
                {
                    numPressed = jmax (0, numPressed - 1);
                    if (! latch.load()) remove (m.getNoteNumber());
                }
                else if (m.isAllNotesOff() || m.isAllSoundOff())
                {
                    numHeld = 0; numPressed = 0;
                    if (sounding >= 0) { out.addEvent (MidiMessage::noteOff (soundingChannel, sounding), meta.samplePosition); sounding = -1; }
                    out.addEvent (m, meta.samplePosition);
                }
                else
                    out.addEvent (m, meta.samplePosition);
            }

            const double stepSamples = jmax (1.0, sampleRate * 60.0 / jmax (20.0, bpm) * kRateBeats[rate.load()]);
            int at = 0;
            while (at < numSamples)
            {
                if (numHeld == 0)
                {
                    if (sounding >= 0) { out.addEvent (MidiMessage::noteOff (soundingChannel, sounding), at); sounding = -1; }
                    samplesToStep = 0;
                    samplesToOff = -1;
                    break;
                }
                // The next thing to happen in this block: the note's end, or the next step.
                int next = numSamples - at;
                if (samplesToOff >= 0) next = jmin (next, samplesToOff);
                next = jmin (next, jmax (0, (int) std::ceil (samplesToStep)));
                if (samplesToOff >= 0 && samplesToOff <= 0 && sounding >= 0)
                {
                    out.addEvent (MidiMessage::noteOff (soundingChannel, sounding), at);
                    sounding = -1;
                    samplesToOff = -1;
                }
                if (samplesToStep <= 0.0)
                {
                    if (sounding >= 0) { out.addEvent (MidiMessage::noteOff (soundingChannel, sounding), at); sounding = -1; }
                    int note, velocity, channel;
                    pick (note, velocity, channel);
                    if (note >= 0 && note <= 127)
                    {
                        out.addEvent (MidiMessage::noteOn (channel, note, (uint8) velocity), at);
                        sounding = note;
                        soundingChannel = channel;
                        samplesToOff = jmax (1, (int) (stepSamples * gate.load() / 100.0));
                    }
                    samplesToStep += stepSamples;
                    continue;
                }
                if (next <= 0) next = 1;
                at += next;
                samplesToStep -= next;
                if (samplesToOff >= 0) samplesToOff -= next;
            }
        }

    private:
        static constexpr int kMax = 32;
        void add (int note, int velocity, int channel)
        {
            for (int i = 0; i < numHeld; ++i) if (heldNote[i] == note) { heldVel[i] = velocity; return; }
            if (numHeld >= kMax) return;
            heldNote[numHeld] = note; heldVel[numHeld] = velocity; heldCh[numHeld] = channel;
            ++numHeld;
        }
        void remove (int note)
        {
            for (int i = 0; i < numHeld; ++i)
                if (heldNote[i] == note)
                {
                    for (int j = i; j + 1 < numHeld; ++j) { heldNote[j] = heldNote[j + 1]; heldVel[j] = heldVel[j + 1]; heldCh[j] = heldCh[j + 1]; }
                    --numHeld;
                    return;
                }
        }
        void pick (int& note, int& velocity, int& channel)
        {
            // The pattern: the held notes in order (as played, or sorted), over the octaves.
            int order[kMax];
            for (int i = 0; i < numHeld; ++i) order[i] = i;
            const int md = mode.load();
            if (md != 4) std::sort (order, order + numHeld, [this] (int a, int b) { return heldNote[a] < heldNote[b]; });
            const int oct = octaves.load();
            const int total = numHeld * oct;
            int idx;
            if (md == 3) idx = random.nextInt (total);
            else if (md == 2 && total > 1)
            {
                const int span = 2 * total - 2;
                const int p = step % span;
                idx = p < total ? p : span - p;
            }
            else if (md == 1) idx = total - 1 - (step % total);
            else idx = step % total;
            ++step;
            const int which = order[idx % numHeld];
            note = heldNote[which] + 12 * (idx / numHeld);
            velocity = heldVel[which];
            channel = heldCh[which];
        }

        std::atomic<int> rate { 2 }, mode { 0 }, octaves { 1 }, gate { 80 };
        std::atomic<bool> latch { false };
        int heldNote[kMax] {}, heldVel[kMax] {}, heldCh[kMax] {};
        int numHeld = 0, numPressed = 0;
        int sounding = -1, soundingChannel = 1, step = 0;
        double samplesToStep = 0.0;
        int samplesToOff = -1;
        Random random;
    };

    //==========================================================================
    class Chord : public MidiFx
    {
    public:
        void configure (const EffectDef& d) override { type.store (jlimit (0, numChordTypes() - 1, d.chordType)); }
        void reset() override { for (auto& c : held) for (auto& n : c) n = -1; }
        void process (const MidiBuffer& in, MidiBuffer& out, int, double, double) override
        {
            for (const auto meta : in)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOn())
                {
                    const int ch = jlimit (1, 16, m.getChannel()), key = m.getNoteNumber();
                    held[ch - 1][key] = type.load();           // the chord it was given, so its note-off matches
                    for (int iv : kChordIntervals[held[ch - 1][key]])
                        if (const int n = key + iv; n >= 0 && n <= 127) out.addEvent (MidiMessage::noteOn (ch, n, m.getVelocity()), meta.samplePosition);
                }
                else if (m.isNoteOff())
                {
                    const int ch = jlimit (1, 16, m.getChannel()), key = m.getNoteNumber();
                    const int t = held[ch - 1][key] >= 0 ? held[ch - 1][key] : type.load();
                    held[ch - 1][key] = -1;
                    for (int iv : kChordIntervals[t])
                        if (const int n = key + iv; n >= 0 && n <= 127) out.addEvent (MidiMessage::noteOff (ch, n), meta.samplePosition);
                }
                else
                    out.addEvent (m, meta.samplePosition);
            }
        }
    private:
        std::atomic<int> type { 0 };
        int held[16][128];
    };

    //==========================================================================
    class Scale : public MidiFx
    {
    public:
        Scale() { reset(); }
        void configure (const EffectDef& d) override
        {
            root.store (jlimit (0, 11, d.scaleRoot));
            mode.store (jlimit (0, numScaleModes() - 1, d.scaleMode));
        }
        void reset() override
        {
            for (auto& c : mapped) for (auto& n : c) n = -1;
            for (auto& c : sounding) for (auto& n : c) n = 0;
        }
        void process (const MidiBuffer& in, MidiBuffer& out, int, double, double) override
        {
            for (const auto meta : in)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOn())
                {
                    /* Two keys can land on one note (C and C# both on C): the
                       note sounds while any of them is down, retriggered by the
                       later one, and lets go only when the last is released. */
                    const int ch = jlimit (1, 16, m.getChannel()), key = m.getNoteNumber();
                    const int to = snap (key);
                    if (mapped[ch - 1][key] >= 0 && sounding[ch - 1][mapped[ch - 1][key]] > 0) --sounding[ch - 1][mapped[ch - 1][key]];   // the same key again
                    if (sounding[ch - 1][to] > 0) out.addEvent (MidiMessage::noteOff (ch, to), meta.samplePosition);
                    mapped[ch - 1][key] = to;
                    ++sounding[ch - 1][to];
                    out.addEvent (MidiMessage::noteOn (ch, to, m.getVelocity()), meta.samplePosition);
                }
                else if (m.isNoteOff())
                {
                    const int ch = jlimit (1, 16, m.getChannel()), key = m.getNoteNumber();
                    const int to = mapped[ch - 1][key] >= 0 ? mapped[ch - 1][key] : snap (key);
                    mapped[ch - 1][key] = -1;
                    if (sounding[ch - 1][to] > 0 && --sounding[ch - 1][to] == 0)
                        out.addEvent (MidiMessage::noteOff (ch, to), meta.samplePosition);
                }
                else
                    out.addEvent (m, meta.samplePosition);
            }
        }
    private:
        int snap (int key) const
        {
            // Down to the nearest note of the scale: a wrong note becomes the one just under it.
            const auto& steps = kScaleSteps[mode.load()];
            const int r = root.load();
            for (int n = key; n >= 0 && n > key - 12; --n)
            {
                const int pc = ((n - r) % 12 + 12) % 12;
                if (std::find (steps.begin(), steps.end(), pc) != steps.end()) return n;
            }
            return key;
        }
        std::atomic<int> root { 0 }, mode { 0 };
        int mapped[16][128];
        uint8 sounding[16][128];
    };
}

namespace
{
    /** A scale run from each key, as a string section plays one: press the
        third and the notes climb the scale from it, in time, and the last
        one sits there while the key is held. A chord pressed is a run in
        each voice, harmonised in the scale. */
    class Run : public MidiFx
    {
    public:
        Run() { reset(); }
        void configure (const EffectDef& d) override
        {
            root.store (jlimit (0, 11, d.scaleRoot));
            mode.store (jlimit (0, MidiFx::numScaleModes() - 1, d.scaleMode));
            notes.store (jlimit (2, 16, d.runNotes));
            beats.store (jlimit (0.25f, 16.0f, d.runBeats));
            degree.store (jlimit (0, 6, d.runDegree));
            down.store (d.runDown != 0);
            toKey.store (d.runToKey != 0);
            holdLast.store (d.runHoldLast);
        }
        void reset() override { for (auto& v : voices) v.key = -1; }
        void process (const MidiBuffer& in, MidiBuffer& out, int numSamples, double sampleRate, double bpm) override
        {
            // The whole run takes `beats`; its last note lands at the end of them.
            const double stepSamples = jmax (1.0, sampleRate * 60.0 / jmax (20.0, bpm) * (double) beats.load() / (double) jmax (1, notes.load() - 1));
            for (const auto meta : in)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOn())
                {
                    const int ch = jlimit (1, 16, m.getChannel());
                    auto* v = find (ch, m.getNoteNumber());
                    if (v == nullptr) v = free();
                    if (v == nullptr) continue;
                    if (v->key >= 0 && v->sounding >= 0) out.addEvent (MidiMessage::noteOff (v->channel, v->sounding), meta.samplePosition);
                    plan (*v, ch, m.getNoteNumber(), (int) m.getVelocity());
                    advance (out, *v, meta.samplePosition);                 // the first note, as the key goes down
                    v->samplesToStep = meta.samplePosition + stepSamples;   // the next one a step later
                }
                else if (m.isNoteOff())
                {
                    if (auto* v = find (jlimit (1, 16, m.getChannel()), m.getNoteNumber()))
                    {
                        /* A key let go mid-run does not stop it: a run is a
                           gesture, and a tap plays the whole of it. It carries
                           on to its last note, which then lifts a step later
                           like the others. Let go after the run has landed,
                           the held last note lifts now. */
                        if (v->index < v->count) v->keyUp = true;
                        else
                        {
                            if (v->sounding >= 0) out.addEvent (MidiMessage::noteOff (v->channel, v->sounding), meta.samplePosition);
                            v->key = -1;
                        }
                    }
                }
                else if (m.isAllNotesOff() || m.isAllSoundOff())
                {
                    reset();
                    out.addEvent (m, meta.samplePosition);
                }
                else
                    out.addEvent (m, meta.samplePosition);
            }
            // Each voice steps on its own clock; a step inside this block lands at its sample.
            for (auto& v : voices)
            {
                if (v.key < 0 || v.index >= v.count) continue;
                while (v.samplesToStep < (double) numSamples && v.index < v.count)
                {
                    advance (out, v, (int) jmax (0.0, v.samplesToStep));
                    if (v.index >= v.count)
                    {
                        if (v.keyUp) v.key = -1;      // ran on after the key came up: done with it, nothing sounding
                        break;
                    }
                    v.samplesToStep += stepSamples;
                }
                if (v.index < v.count) v.samplesToStep -= numSamples;
            }
        }

    private:
        struct Voice { int key = -1, channel = 1, velocity = 100, sounding = -1; int seq[17]; int count = 0, index = 0; double samplesToStep = 0.0; bool keyUp = false; };
        static constexpr int kVoices = 16;
        Voice voices[kVoices];

        Voice* find (int ch, int key) { for (auto& v : voices) if (v.key == key && v.channel == ch) return &v; return nullptr; }
        Voice* free() { for (auto& v : voices) if (v.key < 0) return &v; return nullptr; }

        bool inScale (int note) const
        {
            const auto& steps = kScaleSteps[mode.load()];
            const int pc = ((note - root.load()) % 12 + 12) % 12;
            return std::find (steps.begin(), steps.end(), pc) != steps.end();
        }
        int snapDown (int note) const { for (int n = note; n >= 0 && n > note - 12; --n) if (inScale (n)) return n; return note; }
        int step (int note, int dir) const { for (int n = note + dir; n >= 0 && n <= 127; n += dir) if (inScale (n)) return n; return -1; }

        void plan (Voice& v, int ch, int key, int velocity)
        {
            v.key = key; v.channel = ch; v.velocity = velocity; v.sounding = -1; v.index = 0; v.count = 0; v.keyUp = false;
            const int n = notes.load(), dir = down.load() ? -1 : 1;
            int start = snapDown (key);
            for (int i = 0; i < degree.load(); ++i) { const int p = step (start, 1); if (p < 0) break; start = p; }   // from a degree above the key
            if (toKey.load())
            {
                // Lands on the key: begin n - 1 scale notes before it, against the run's direction.
                for (int i = 1; i < n; ++i) { const int p = step (start, -dir); if (p < 0) break; start = p; }
            }
            int cur = start;
            for (int i = 0; i < n && cur >= 0; ++i)
            {
                v.seq[v.count++] = cur;
                cur = step (cur, dir);
            }
        }

        void advance (MidiBuffer& out, Voice& v, int at)
        {
            if (v.sounding >= 0) { out.addEvent (MidiMessage::noteOff (v.channel, v.sounding), at); v.sounding = -1; }
            if (v.index >= v.count) return;
            const int note = v.seq[v.index++];
            if (note < 0) return;                                      // the step that only lets the last note go
            out.addEvent (MidiMessage::noteOn (v.channel, note, (uint8) v.velocity), at);
            v.sounding = note;
            // The last note: held while the key is, or let go at the next step like the rest (as it is when the key is already up).
            if (v.index >= v.count && (! holdLast.load() || v.keyUp) && v.count < 17) v.seq[v.count++] = -1;
        }

        std::atomic<int> root { 0 }, mode { 0 }, notes { 8 }, degree { 0 };
        std::atomic<float> beats { 1.0f };
        std::atomic<bool> down { false }, toKey { false }, holdLast { true };
    };
}

namespace
{
    /** A big band section: each key plays the chord on that step of the key,
        a 6th on the tonic, 7ths on the others, voiced as the player set
        them. The melody is the key pressed; the band fills in under it. */
    class BigBand : public MidiFx
    {
    public:
        BigBand() { reset(); }
        void configure (const EffectDef& d) override
        {
            root.store (MidiFx::bandMajorRoot (jlimit (0, 11, d.scaleRoot), jlimit (0, MidiFx::numScaleModes() - 1, d.scaleMode)));
            for (int r = 0; r < 12; ++r)
            {
                const auto& row = (size_t) r < d.bandChords.size() ? d.bandChords[(size_t) r] : std::vector<int>();
                const int n = jmin (kMaxNotes, (int) row.size());
                for (int i = 0; i < n; ++i) table[r][i].store (row[(size_t) i]);
                count[r].store (n);
            }
        }
        void reset() override
        {
            for (auto& c : notes) for (auto& k : c) for (auto& n : k) n = kNone;
            for (auto& c : sounding) for (auto& n : c) n = 0;
        }
        void process (const MidiBuffer& in, MidiBuffer& out, int, double, double) override
        {
            for (const auto meta : in)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOn())
                {
                    const int ch = jlimit (1, 16, m.getChannel()), key = m.getNoteNumber();
                    auto* mine = notes[ch - 1][key];
                    if (mine[0] != kNone) release (out, ch, key, meta.samplePosition);      // the same key again
                    const int pc = ((key - root.load()) % 12 + 12) % 12;
                    const int n = count[pc].load();
                    int slot = 0;
                    auto play = [&] (int note)
                    {
                        if (note < 0 || note > 127 || slot >= kMaxNotes) return;
                        for (int i = 0; i < slot; ++i) if (mine[i] == note) return;          // a doubled note in the voicing
                        /* Two chords can share a note (C6 and Am7 share all four):
                           it sounds while either is down, retriggered by the later. */
                        if (sounding[ch - 1][note] > 0) out.addEvent (MidiMessage::noteOff (ch, note), meta.samplePosition);
                        ++sounding[ch - 1][note];
                        mine[slot++] = (uint8) note;
                        out.addEvent (MidiMessage::noteOn (ch, note, m.getVelocity()), meta.samplePosition);
                    };
                    if (n == 0) play (key);                                                  // no chord on this step: the key alone
                    for (int i = 0; i < n; ++i) play (key + table[pc][i].load());
                }
                else if (m.isNoteOff())
                {
                    release (out, jlimit (1, 16, m.getChannel()), m.getNoteNumber(), meta.samplePosition);
                }
                else if (m.isAllNotesOff() || m.isAllSoundOff())
                {
                    // That channel's notes are gone: forget them, and only them.
                    const int ch = jlimit (1, 16, m.getChannel());
                    for (auto& k : notes[ch - 1]) for (auto& n : k) n = kNone;
                    for (auto& n : sounding[ch - 1]) n = 0;
                    out.addEvent (m, meta.samplePosition);
                }
                else
                    out.addEvent (m, meta.samplePosition);
            }
        }
    private:
        static constexpr int kMaxNotes = MidiFx::maxBandNotes;
        static constexpr uint8 kNone = 255;
        void release (MidiBuffer& out, int ch, int key, int at)
        {
            auto* mine = notes[ch - 1][key];
            for (int i = 0; i < kMaxNotes && mine[i] != kNone; ++i)
            {
                const int note = mine[i];
                if (sounding[ch - 1][note] > 0 && --sounding[ch - 1][note] == 0) out.addEvent (MidiMessage::noteOff (ch, note), at);
                mine[i] = kNone;
            }
        }
        std::atomic<int> root { 0 };
        std::atomic<int> table[12][kMaxNotes];
        std::atomic<int> count[12];
        uint8 notes[16][128][kMaxNotes];
        uint8 sounding[16][128];
    };

    //==========================================================================
    // Chord names and their notes, for the big band's table.
    struct ChordQuality { const char* name; std::vector<int> notes; };
    const ChordQuality kQualities[] = {
        { "", { 0, 4, 7 } }, { "m", { 0, 3, 7 } }, { "6", { 0, 4, 7, 9 } }, { "m6", { 0, 3, 7, 9 } },
        { "7", { 0, 4, 7, 10 } }, { "maj7", { 0, 4, 7, 11 } }, { "m7", { 0, 3, 7, 10 } }, { "m7b5", { 0, 3, 6, 10 } },
        { "dim", { 0, 3, 6 } }, { "dim7", { 0, 3, 6, 9 } }, { "aug", { 0, 4, 8 } }, { "7#5", { 0, 4, 8, 10 } }, { "7b5", { 0, 4, 6, 10 } },
        { "9", { 0, 4, 7, 10, 14 } }, { "m9", { 0, 3, 7, 10, 14 } }, { "maj9", { 0, 4, 7, 11, 14 } }, { "6/9", { 0, 4, 7, 9, 14 } },
        { "7b9", { 0, 4, 7, 10, 13 } }, { "7#9", { 0, 4, 7, 10, 15 } }, { "13", { 0, 4, 7, 10, 21 } }, { "11", { 0, 7, 10, 14, 17 } },
        { "sus4", { 0, 5, 7 } }, { "sus2", { 0, 2, 7 } }, { "7sus4", { 0, 5, 7, 10 } }, { "mMaj7", { 0, 3, 7, 11 } },
        { "add9", { 0, 4, 7, 14 } }, { "madd9", { 0, 3, 7, 14 } }, { "5", { 0, 7 } }, { "m6/9", { 0, 3, 7, 9, 14 } } };

    /** The pitch class a note or chord name starts with, and how long that root is; -1 when it is not one. */
    int rootOf (const String& s, int& length, int* raw = nullptr)
    {
        length = 0;
        if (s.isEmpty()) return -1;
        const juce_wchar c = CharacterFunctions::toUpperCase (s[0]);
        if (c < 'A' || c > 'G') return -1;
        static const int base[] = { 9, 11, 0, 2, 4, 5, 7 };
        int pc = base[c - 'A'];
        length = 1;
        while (length < s.length())
        {
            const juce_wchar a = s[length];
            if (a == '#' || a == 0x266F) ++pc;
            else if (a == 'b' || a == 0x266D) --pc;
            else break;
            ++length;
        }
        if (raw != nullptr) *raw = pc;                    // unwrapped: Cb is -1, B# is 12
        return (pc % 12 + 12) % 12;
    }

    String normaliseQuality (String q)
    {
        q = q.removeCharacters (" ()");
        q = q.replace ("M7", "maj7").replace ("M9", "maj9").replace (String (CharPointer_UTF8 ("\xce\x94")), "maj7")
             .replace (String (CharPointer_UTF8 ("\xc2\xb0")), "dim").replace (String (CharPointer_UTF8 ("\xc3\xb8")), "m7b5").replace ("+", "aug")
             .replace ("min", "m").replace ("Maj", "maj").replace ("MAJ", "maj").replace ("-", "m").replace ("dom", "").replace ("69", "6/9");
        if (q.startsWith ("mmaj7")) return "mMaj7";
        if (q == "M") return "";                          // CM is C major, not C minor
        if (q == "M6") return "6";
        return q;
    }

    /** The quality whose notes these are, with the first note the root: "m7", or null. */
    const ChordQuality* qualityOf (const std::vector<int>& offsets)
    {
        if (offsets.empty()) return nullptr;
        std::vector<int> mine;
        for (int o : offsets) { const int pc = ((o - offsets[0]) % 12 + 12) % 12; if (std::find (mine.begin(), mine.end(), pc) == mine.end()) mine.push_back (pc); }
        std::sort (mine.begin(), mine.end());
        for (const auto& q : kQualities)
        {
            std::vector<int> theirs;
            for (int o : q.notes) { const int pc = o % 12; if (std::find (theirs.begin(), theirs.end(), pc) == theirs.end()) theirs.push_back (pc); }
            std::sort (theirs.begin(), theirs.end());
            if (theirs == mine) return &q;
        }
        return nullptr;
    }
}

int MidiFx::bandMajorRoot (int root, int mode)
{
    static const int up[] = { 0, 3, 3, 10, 5, 7, 8, 0, 3, 3, 0 };    // major, natural minor, harmonic minor, dorian, mixolydian, lydian, phrygian, major pent, minor pent, blues, whole tone
    return (root + up[jlimit (0, 10, mode)]) % 12;
}

bool MidiFx::parseBandChord (const String& text, int rowRoot, std::vector<int>& offsets)
{
    offsets.clear();
    auto tokens = StringArray::fromTokens (text.replaceCharacter (',', ' '), " \t", "");
    tokens.removeEmptyStrings();
    if (tokens.isEmpty()) return true;
    auto nearest = [rowRoot] (int pc) { int d = ((pc - rowRoot) % 12 + 12) % 12; return d >= 6 ? d - 12 : d; };   // -6..5 from the step's root: a tritone away sits below
    /* One token that reads as a chord name is the chord: G7 and C6 are
       chords here, not notes in the seventh and sixth octaves. */
    if (tokens.size() == 1)
    {
        int len;
        const int pc = rootOf (tokens[0], len);
        if (pc < 0) return false;
        const auto rest = tokens[0].substring (len);
        const bool sixNine = rest.contains ("6/9") || rest.contains ("69");
        const bool minorSixNine = sixNine && rest.startsWith ("m") && ! rest.startsWithIgnoreCase ("maj");
        const auto quality = sixNine ? String (minorSixNine ? "m6/9" : "6/9") : normaliseQuality (rest.upToFirstOccurrenceOf ("/", false, false));
        for (const auto& q : kQualities)
            if (quality == q.name || (quality != "M" && quality.toLowerCase() == String (q.name).toLowerCase()))
            {
                const int base = nearest (pc);
                if (! sixNine && rest.contains ("/"))
                {
                    // A slash bass goes under the chord, the nearest one below its root: C6/E puts the E below, F/C the C.
                    int bl;
                    const int bass = rootOf (rest.fromFirstOccurrenceOf ("/", false, false), bl);
                    if (bass < 0) return false;
                    const int bassPc = ((bass - rowRoot) % 12 + 12) % 12;
                    int down = ((base - bassPc) % 12 + 12) % 12;
                    if (down == 0) down = 12;
                    offsets.push_back (base - down);
                }
                for (int n : q.notes) offsets.push_back (base + n);
                return offsets.size() <= (size_t) maxBandNotes;
            }
    }
    /* Notes: every token a note name, with an octave or without. With one
       the note is placed exactly, the key pressed standing in octave 4;
       without, the first sits nearest the key and each next one climbs. */
    auto octaveOf = [] (const String& t, int len) -> int
    {
        const auto rest = t.substring (len);
        if (rest.isEmpty()) return 99;                                       // none given
        if (rest.length() <= 2 && rest.containsOnly ("-0123456789") && rest != "-") return rest.getIntValue();
        return 98;                                                           // not a note
    };
    for (const auto& t : tokens) { int len; if (rootOf (t, len) < 0 || octaveOf (t, len) == 98) return false; }
    if (tokens.size() > maxBandNotes) return false;
    int len, prev = 0;
    for (int i = 0; i < tokens.size(); ++i)
    {
        int raw = 0;
        const int pc = rootOf (tokens[i], len, &raw);
        const int oct = octaveOf (tokens[i], len);
        if (oct != 99) prev = raw + 12 * (oct + 1) - (60 + rowRoot);                                       // placed: the key is 60 + its pitch class; Cb4 is B3
        else if (i == 0) prev = nearest (pc);
        else { int d = ((pc - rowRoot - prev) % 12 + 12) % 12; if (d == 0) d = 12; prev += d; }        // each the next one up
        if (prev < -36 || prev > 36) return false;
        offsets.push_back (prev);
    }
    return true;
}

String MidiFx::bandChordNotes (int rowRoot, const std::vector<int>& offsets, bool flats, bool octaves)
{
    static const char* sharps[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char* flatNames[] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
    StringArray names;
    for (int o : offsets)
    {
        const int note = 60 + rowRoot + o;                                   // the key pressed in octave 4
        names.add (String ((flats ? flatNames : sharps)[(note % 12 + 12) % 12]) + (octaves ? String (note / 12 - 1) : String()));
    }
    return names.joinIntoString (" ");
}

String MidiFx::bandChordName (int rowRoot, const std::vector<int>& offsets, bool flats)
{
    if (offsets.empty()) return {};
    if (const auto* q = qualityOf (offsets))
    {
        static const char* sharps[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        static const char* flatNames[] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
        return String ((flats ? flatNames : sharps)[((rowRoot + offsets[0]) % 12 + 12) % 12]) + q->name;
    }
    return bandChordNotes (rowRoot, offsets, flats);
}

namespace
{
    /** A player's unevenness, put back: each note lands a little late, by a
        different amount every time, and a little louder or softer. The note
        off waits by the same amount as its note on, so lengths are kept and
        an off can never overtake its on. */
    class Humanize : public MidiFx
    {
    public:
        Humanize() { reset(); }
        void configure (const EffectDef& d) override
        {
            timingMs.store (jlimit (0, 100, d.humanTimingMs));
            velocity.store (jlimit (0, 64, d.humanVelocity));
        }
        void reset() override
        {
            numPending = 0;
            for (auto& c : delayOf) for (auto& n : c) n = 0;
            for (auto& c : lastAt) for (auto& n : c) n = 0;
        }
        void restartTiming() override {}              // what is waiting still goes out: a delayed note-off must arrive
        void process (const MidiBuffer& in, MidiBuffer& out, int numSamples, double sampleRate, double) override
        {
            // What earlier blocks left for this one.
            int kept = 0;
            for (int i = 0; i < numPending; ++i)
            {
                auto& p = pending[i];
                if (p.samplesLeft < numSamples) out.addEvent (p.data, p.size, p.samplesLeft);
                else { p.samplesLeft -= numSamples; pending[kept++] = p; }
            }
            numPending = kept;

            const int maxDelay = (int) (sampleRate * timingMs.load() / 1000.0);
            const int vel = velocity.load();
            blockStart = clock;
            clock += numSamples;
            for (const auto meta : in)
            {
                const auto m = meta.getMessage();
                /* Each event of a pitch goes out no earlier than the last one
                   of that pitch did: a second press of a note still waiting, or
                   a chord effect's off-and-on retrigger, keeps its order. */
                if (m.isNoteOn())
                {
                    const int ch = jlimit (1, 16, m.getChannel()), note = m.getNoteNumber();
                    const int delay = maxDelay > 0 ? random.nextInt (maxDelay + 1) : 0;
                    delayOf[ch - 1][note] = delay;
                    const int v = vel > 0 ? jlimit (1, 127, (int) m.getVelocity() + random.nextInt (2 * vel + 1) - vel) : (int) m.getVelocity();
                    schedule (out, MidiMessage::noteOn (ch, note, (uint8) v), inOrder (ch, note, meta.samplePosition + delay), numSamples);
                }
                else if (m.isNoteOff())
                {
                    const int ch = jlimit (1, 16, m.getChannel()), note = m.getNoteNumber();
                    schedule (out, m, inOrder (ch, note, meta.samplePosition + delayOf[ch - 1][note]), numSamples);
                    delayOf[ch - 1][note] = 0;
                }
                else if (m.isAllNotesOff() || m.isAllSoundOff())
                {
                    // Everything waiting goes now, then the panic itself.
                    for (int i = 0; i < numPending; ++i) out.addEvent (pending[i].data, pending[i].size, meta.samplePosition);
                    numPending = 0;
                    out.addEvent (m, meta.samplePosition);
                }
                else
                    out.addEvent (m, meta.samplePosition);
            }
        }
    private:
        struct Pending { uint8 data[3]; int size; int samplesLeft; };
        static constexpr int kMaxPending = 512;
        void schedule (MidiBuffer& out, const MidiMessage& m, int at, int numSamples)
        {
            if (at < numSamples) { out.addEvent (m, at); return; }
            if (numPending >= kMaxPending || m.getRawDataSize() > 3) { out.addEvent (m, numSamples - 1); return; }    // full: now rather than never
            auto& p = pending[numPending++];
            p.size = m.getRawDataSize();
            std::memcpy (p.data, m.getRawData(), (size_t) p.size);
            p.samplesLeft = at - numSamples;
        }
        /** The block position for an event of this pitch: `at`, or later if one of the pitch is already due later. */
        int inOrder (int ch, int note, int at)
        {
            juce::int64 when = blockStart + at;
            when = jmax (when, lastAt[ch - 1][note]);
            lastAt[ch - 1][note] = when;
            return (int) (when - blockStart);
        }
        std::atomic<int> timingMs { 12 }, velocity { 10 };
        Pending pending[kMaxPending];
        int numPending = 0;
        int delayOf[16][128];
        juce::int64 lastAt[16][128] {};
        juce::int64 clock = 0, blockStart = 0;          // samples since the effect began
        Random random;
    };

    //==========================================================================
    /** A short MIDI file from each key: a lick, a fill, a riff, moved so that
        it begins on the key pressed (or sits where it was written), at the
        rig's tempo, at the key's velocity. A tap plays the whole of it, or
        it stops when the key does; it can repeat while the key is held. */
    class Phrase : public MidiFx
    {
    public:
        Phrase() { reset(); }
        void configure (const EffectDef& d) override
        {
            /* Two copies of the notes: the audio thread reads one while this
               fills the other, then they swap. Before writing, wait for the
               audio thread to have taken up the last swap -- otherwise two
               changes in one block would write the table being played. It
               takes it up within a block; a program that is not playing never
               will, so the wait is short and then gives up. */
            for (int i = 0; i < 50 && reading.load() != active.load(); ++i) Thread::sleep (1);
            auto& t = tables[1 - active.load()];
            t.count = jmin (kMaxNotes, (int) d.phraseNotes.size());
            float end = 0.0f;
            for (int i = 0; i < t.count; ++i)
            {
                const auto& n = d.phraseNotes[(size_t) i];
                t.beat[i] = jmax (0.0f, n.beat); t.length[i] = jmax (1.0f / 32.0f, n.length); t.note[i] = jlimit (0, 127, n.note); t.velocity[i] = jlimit (1, 127, n.velocity);
                end = jmax (end, t.beat[i] + t.length[i]);
            }
            t.beats = jmax (0.25f, d.phraseBeats > 0.0f ? d.phraseBeats : std::ceil (end));
            t.root = d.phraseRoot >= 0 ? d.phraseRoot : t.count > 0 ? t.note[0] : 60;
            t.fixed = d.phraseRoot == -2;                                   // drums: every key plays it as written
            active.store (1 - active.load());
            stop.store (d.phraseStop);
            loop.store (d.phraseLoop);
        }
        void reset() override
        {
            for (auto& v : voices) { v.key = -1; v.numSounding = 0; }
            for (auto& c : sounding) for (auto& n : c) n = 0;
            for (auto& c : passed) for (auto& n : c) n = false;
        }
        void process (const MidiBuffer& in, MidiBuffer& out, int numSamples, double sampleRate, double bpm) override
        {
            const int which = active.load();
            reading.store (which);
            const auto& t = tables[which];
            const double spb = sampleRate * 60.0 / jmax (20.0, bpm);          // samples per beat, at the tempo now
            for (const auto meta : in)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOn())
                {
                    const int ch = jlimit (1, 16, m.getChannel());
                    auto* v = find (ch, m.getNoteNumber());
                    if (v == nullptr) v = free();
                    if (v == nullptr) continue;
                    if (v->key >= 0) release (out, *v, meta.samplePosition);
                    v->key = m.getNoteNumber(); v->channel = ch; v->velocity = (int) m.getVelocity();
                    v->transpose = t.fixed ? 0 : v->key - t.root; v->elapsed = -(double) meta.samplePosition; v->keyUp = false;
                    v->onDone.reset(); v->offDone.reset();
                    if (t.count == 0)                                                                 // nothing loaded: the key itself
                    {
                        out.addEvent (m, meta.samplePosition);
                        passed[ch - 1][m.getNoteNumber()] = true;
                        v->key = -1;
                    }
                }
                else if (m.isNoteOff())
                {
                    const int ch = jlimit (1, 16, m.getChannel());
                    if (passed[ch - 1][m.getNoteNumber()])
                    {
                        // Its note-on went straight through, whatever has been loaded since: so does its note-off.
                        passed[ch - 1][m.getNoteNumber()] = false;
                        out.addEvent (m, meta.samplePosition);
                    }
                    else if (auto* v = find (ch, m.getNoteNumber()))
                    {
                        if (stop.load()) { release (out, *v, meta.samplePosition); v->key = -1; }
                        else v->keyUp = true;
                    }
                }
                else if (m.isAllNotesOff() || m.isAllSoundOff())
                {
                    // That channel's phrases stop and are forgotten; the others play on.
                    const int ch = jlimit (1, 16, m.getChannel());
                    for (auto& v : voices) if (v.key >= 0 && v.channel == ch) { v.key = -1; v.numSounding = 0; }
                    for (auto& n : sounding[ch - 1]) n = 0;
                    for (auto& n : passed[ch - 1]) n = false;
                    out.addEvent (m, meta.samplePosition);
                }
                else
                    out.addEvent (m, meta.samplePosition);
            }
            // Each voice plays the notes whose time falls in this block; at the phrase's end it lets go, and goes round again if it repeats.
            const double lengthSamples = jmax (1.0, (double) t.beats * spb);
            for (auto& v : voices)
            {
                if (v.key < 0) continue;
                double from = v.elapsed;          // phrase time at the block sample `offset`
                int offset = 0, remaining = numSamples;
                while (remaining > 0 && v.key >= 0)
                {
                    const double to = from + remaining, windowEnd = jmin (to, lengthSamples);
                    for (int i = 0; i < t.count; ++i)
                    {
                        const double onAt = t.beat[i] * spb, offAt = (double) (t.beat[i] + t.length[i]) * spb;
                        // Due by the end of this window: a tempo raised mid-phrase brings notes forward, never skips them.
                        if (! v.onDone[(size_t) i] && onAt < windowEnd)
                        {
                            v.onDone[(size_t) i] = true;
                            const int note = t.note[i] + v.transpose;
                            if (note >= 0 && note <= 127 && v.numSounding < kMaxSounding)
                            {
                                const int at = jlimit (0, numSamples - 1, offset + jmax (0, (int) (onAt - from)));
                                if (sounding[v.channel - 1][note] > 0) out.addEvent (MidiMessage::noteOff (v.channel, note), at);   // another voice has it: retrigger
                                ++sounding[v.channel - 1][note];
                                v.soundingNote[v.numSounding] = (uint8) note; v.soundingIndex[v.numSounding++] = (int16) i;
                                out.addEvent (MidiMessage::noteOn (v.channel, note, (uint8) jlimit (1, 127, t.velocity[i] * v.velocity / 100)), at);
                            }
                        }
                        if (v.onDone[(size_t) i] && ! v.offDone[(size_t) i] && offAt < windowEnd)
                        {
                            v.offDone[(size_t) i] = true;
                            endNote (out, v, i, jlimit (0, numSamples - 1, offset + jmax (0, (int) (offAt - from))));
                        }
                    }
                    if (to < lengthSamples) { v.elapsed = to; break; }
                    // The end of the phrase falls in this block: whatever is still sounding lets go there.
                    const int consumed = jmax (1, (int) std::ceil (lengthSamples - from));
                    release (out, v, jlimit (0, numSamples - 1, offset + consumed - 1));
                    if (loop.load() && ! v.keyUp && t.count > 0)
                    {
                        v.onDone.reset(); v.offDone.reset();
                        offset += consumed; remaining -= consumed; from = 0.0; v.elapsed = 0.0;
                    }
                    else
                        v.key = -1;                // played out; a tap plays the whole of it, so nothing waits for the key
                }
            }
        }

    private:
        static constexpr int kMaxNotes = 512, kVoices = 16, kMaxSounding = 32;
        struct Table { int count = 0; float beat[kMaxNotes], length[kMaxNotes]; int note[kMaxNotes], velocity[kMaxNotes]; float beats = 0.0f; int root = 60; bool fixed = false; };
        struct Voice
        {
            int key = -1, channel = 1, velocity = 100, transpose = 0;
            double elapsed = 0.0;                 // samples since the key went down, at the start of this block
            bool keyUp = false;
            std::bitset<kMaxNotes> onDone, offDone;
            uint8 soundingNote[kMaxSounding]; int16 soundingIndex[kMaxSounding]; int numSounding = 0;
        };
        Table tables[2];
        std::atomic<int> active { 0 }, reading { 0 };    // the table to play, and the one the audio thread last took up
        bool passed[16][128] {};                          // keys that went straight through while nothing was loaded
        std::atomic<bool> stop { false }, loop { false };
        Voice voices[kVoices];
        uint8 sounding[16][128];

        Voice* find (int ch, int key) { for (auto& v : voices) if (v.key == key && v.channel == ch) return &v; return nullptr; }
        Voice* free() { for (auto& v : voices) if (v.key < 0) return &v; return nullptr; }
        void endNote (MidiBuffer& out, Voice& v, int index, int at)
        {
            for (int k = 0; k < v.numSounding; ++k)
                if (v.soundingIndex[k] == index)
                {
                    const int note = v.soundingNote[k];
                    if (sounding[v.channel - 1][note] > 0 && --sounding[v.channel - 1][note] == 0) out.addEvent (MidiMessage::noteOff (v.channel, note), at);
                    v.soundingNote[k] = v.soundingNote[v.numSounding - 1]; v.soundingIndex[k] = v.soundingIndex[v.numSounding - 1]; --v.numSounding;
                    return;
                }
        }
        void release (MidiBuffer& out, Voice& v, int at)
        {
            while (v.numSounding > 0) endNote (out, v, v.soundingIndex[0], at);
        }
    };
}

std::unique_ptr<MidiFx> MidiFx::create (const EffectDef& def)
{
    std::unique_ptr<MidiFx> fx;
    switch (def.kind)
    {
        case EffectDef::Kind::arpeggio: fx = std::make_unique<Arpeggiator>(); break;
        case EffectDef::Kind::chord:    fx = std::make_unique<Chord>(); break;
        case EffectDef::Kind::scale:    fx = std::make_unique<Scale>(); break;
        case EffectDef::Kind::run:      fx = std::make_unique<Run>(); break;
        case EffectDef::Kind::bigBand:  fx = std::make_unique<BigBand>(); break;
        case EffectDef::Kind::humanize: fx = std::make_unique<Humanize>(); break;
        case EffectDef::Kind::phrase:   fx = std::make_unique<Phrase>(); break;
        default: return nullptr;
    }
    fx->configure (def);
    fx->reset();
    return fx;
}

String MidiFx::keyName (int root, int mode)
{
    return String (noteNames[jlimit (0, 11, root)]) + " " + String (scaleModes[jlimit (0, numScaleModes() - 1, mode)]).toLowerCase();
}

bool MidiFx::keyIsMinor (int mode)
{
    return mode == 1 || mode == 2 || mode == 3 || mode == 6 || mode == 8 || mode == 9;    // natural and harmonic minor, dorian, phrygian, minor pentatonic, blues
}

bool MidiFx::keyPrefersFlats (int root, int mode)
{
    const int major = ((keyIsMinor (mode) ? root + 3 : root) % 12 + 12) % 12;
    return major == 5 || major == 10 || major == 3 || major == 8 || major == 1 || major == 6;
}

String MidiFx::describe (const EffectDef& d)
{
    switch (d.kind)
    {
        case EffectDef::Kind::arpeggio:
            return String (arpRates[jlimit (0, numArpRates() - 1, d.arpRate)]) + ", " + String (arpModes[jlimit (0, numArpModes() - 1, d.arpMode)]).toLowerCase()
                   + ", " + String (jlimit (1, 4, d.arpOctaves)) + (d.arpOctaves == 1 ? " octave" : " octaves") + (d.arpLatch ? ", latched" : "");
        case EffectDef::Kind::chord:
            return String (chordTypes[jlimit (0, numChordTypes() - 1, d.chordType)]);
        case EffectDef::Kind::scale:
            return String (noteNames[jlimit (0, 11, d.scaleRoot)]) + " " + String (scaleModes[jlimit (0, numScaleModes() - 1, d.scaleMode)]).toLowerCase();
        case EffectDef::Kind::run:
        {
            static const char* ordinals[] = { "", "2nd", "3rd", "4th", "5th", "6th", "7th" };
            const int n = jlimit (2, 16, d.runNotes);
            const String length = n == 8 ? "an octave" : n == 15 ? "two octaves" : String (n) + " notes";
            const float b = d.runBeats;
            const String takes = std::abs (b - 0.5f) < 0.01f ? "half a beat" : std::abs (b - 1.0f) < 0.01f ? "a beat" : std::abs (b - 2.0f) < 0.01f ? "two beats"
                               : std::abs (b - 4.0f) < 0.01f ? "a bar" : String (b, 2) + " beats";
            return String (noteNames[jlimit (0, 11, d.scaleRoot)]) + " " + String (scaleModes[jlimit (0, numScaleModes() - 1, d.scaleMode)]).toLowerCase()
                   + ", " + length + " " + (d.runDown ? "down" : "up") + (d.runToKey ? " to the key" : d.runDegree > 0 ? " from the " + String (ordinals[jlimit (1, 6, d.runDegree)]) + " above the key" : " from the key")
                   + " in " + takes;
        }
        case EffectDef::Kind::bigBand:
        {
            // The key, then the chords on its scale steps: "C major: C6 Dm7 Em7 F6 G7 Am7 Bm7b5".
            const int major = bandMajorRoot (jlimit (0, 11, d.scaleRoot), jlimit (0, numScaleModes() - 1, d.scaleMode));
            const bool flats = keyPrefersFlats (d.scaleRoot, d.scaleMode);
            StringArray chords;
            for (int step : { 0, 2, 4, 5, 7, 9, 11 })
                if ((size_t) step < d.bandChords.size() && ! d.bandChords[(size_t) step].empty())
                    chords.add (bandChordName ((major + step) % 12, d.bandChords[(size_t) step], flats));
            return keyName (d.scaleRoot, d.scaleMode) + ": " + (chords.isEmpty() ? String ("no chords") : chords.joinIntoString (" "));
        }
        case EffectDef::Kind::humanize:
        {
            const int t = jlimit (0, 100, d.humanTimingMs), v = jlimit (0, 64, d.humanVelocity);
            if (t == 0 && v == 0) return "as played";
            return (t > 0 ? "up to " + String (t) + " ms late" : String()) + (t > 0 && v > 0 ? ", " : "") + (v > 0 ? "velocity " + String (CharPointer_UTF8 ("\xc2\xb1")) + String (v) : String());
        }
        case EffectDef::Kind::phrase:
        {
            if (d.phraseNotes.empty()) return "no MIDI file yet: Settings... loads one";
            const int n = (int) d.phraseNotes.size();
            const float b = d.phraseBeats;
            const String beats = std::abs (b - 1.0f) < 0.01f ? "a beat" : std::abs (b - 4.0f) < 0.01f ? "a bar" : std::abs (b - std::round (b)) < 0.01f ? String ((int) std::round (b)) + " beats" : String (b, 2) + " beats";
            return d.phraseName + ": " + String (n) + (n == 1 ? " note" : " notes") + " over " + beats
                   + (d.phraseRoot == -2 ? ", as written whatever the key" : d.phraseRoot < 0 ? ", starting on the key" : ", as written from " + MidiMessage::getMidiNoteName (d.phraseRoot, true, true, 3))
                   + (d.phraseLoop ? ", repeating" : "") + (d.phraseStop ? ", stops with the key" : "");
        }
        default: return {};
    }
}

bool MidiFx::importPhrase (const File& file, EffectDef& def, String& error)
{
    MidiFile mf;
    FileInputStream in (file);
    if (! file.existsAsFile() || ! in.openedOk() || ! mf.readFrom (in)) { error = file.getFileName() + ": not a MIDI file StageRig can read"; return false; }
    /* Beats, not seconds: the phrase plays at the rig's tempo. A file timed
       in SMPTE frames has no beats, so its seconds are read at 120. */
    const short format = mf.getTimeFormat();
    double ticksPerBeat = format > 0 ? (double) format : 0.0;
    if (ticksPerBeat <= 0.0) { mf.convertTimestampTicksToSeconds(); ticksPerBeat = 0.5; }
    std::vector<EffectDef::PhraseNote> notes;
    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        MidiMessageSequence track (*mf.getTrack (t));
        track.updateMatchedPairs();
        for (int i = 0; i < track.getNumEvents(); ++i)
        {
            const auto* e = track.getEventPointer (i);
            if (! e->message.isNoteOn()) continue;
            const double on = e->message.getTimeStamp() / ticksPerBeat;
            const double off = e->noteOffObject != nullptr ? e->noteOffObject->message.getTimeStamp() / ticksPerBeat : on + 1.0;
            notes.push_back ({ (float) on, (float) jmax (1.0 / 32.0, off - on), e->message.getNoteNumber(), jmax (1, (int) e->message.getVelocity()) });
        }
    }
    if (notes.empty()) { error = file.getFileName() + ": no notes in it"; return false; }
    std::stable_sort (notes.begin(), notes.end(), [] (const EffectDef::PhraseNote& a, const EffectDef::PhraseNote& b) { return a.beat < b.beat || (a.beat == b.beat && a.note < b.note); });
    if (notes.size() > 512) notes.resize (512);
    // From its first note: the phrase begins as the key goes down.
    const float first = notes.front().beat;
    float end = 0.0f;
    for (auto& n : notes) { n.beat -= first; end = jmax (end, n.beat + n.length); }
    def.phraseNotes = std::move (notes);
    def.phraseBeats = jmax (1.0f, std::ceil (end - 0.001f));
    def.phraseName = file.getFileNameWithoutExtension();
    return true;
}

File MidiFx::phraseLibraryFolder()
{
    const File exe = File::getSpecialLocation (File::currentExecutableFile);
    std::vector<File> candidates;
    candidates.push_back (exe.getParentDirectory().getParentDirectory().getChildFile ("share/stagerig/phrases"));   // bin/../share
    candidates.push_back (File ("/usr/local/share/stagerig/phrases"));
    candidates.push_back (File ("/usr/share/stagerig/phrases"));
    File dir = exe.getParentDirectory();
    for (int up = 0; up < 6; ++up, dir = dir.getParentDirectory())                              // the source tree, from a build folder
        candidates.push_back (dir.getChildFile ("Resources/phrases"));
    for (const auto& c : candidates) if (c.isDirectory()) return c;
    return {};
}

} // namespace perf
