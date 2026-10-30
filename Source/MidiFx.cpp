#include "MidiFx.h"
#include <algorithm>
#include <cmath>
#include <vector>

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
                    reset();
                    out.addEvent (m, meta.samplePosition);
                }
                else
                    out.addEvent (m, meta.samplePosition);
            }
        }
    private:
        static constexpr int kMaxNotes = 6;
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
        { "add9", { 0, 4, 7, 14 } }, { "madd9", { 0, 3, 7, 14 } }, { "5", { 0, 7 } } };

    /** The pitch class a note or chord name starts with, and how long that root is; -1 when it is not one. */
    int rootOf (const String& s, int& length)
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
        return (pc % 12 + 12) % 12;
    }

    String normaliseQuality (String q)
    {
        q = q.removeCharacters (" ()");
        q = q.replace ("M7", "maj7").replace ("M9", "maj9").replace (String (CharPointer_UTF8 ("\xce\x94")), "maj7")
             .replace (String (CharPointer_UTF8 ("\xc2\xb0")), "dim").replace (String (CharPointer_UTF8 ("\xc3\xb8")), "m7b5").replace ("+", "aug")
             .replace ("min", "m").replace ("Maj", "maj").replace ("MAJ", "maj").replace ("-", "m").replace ("dom", "").replace ("69", "6/9");
        if (q.startsWith ("mmaj7")) return "mMaj7";
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
    // Notes: every token a note name and nothing more.
    bool allNotes = true;
    for (const auto& t : tokens) { int len; if (rootOf (t, len) < 0 || len != t.length()) { allNotes = false; break; } }
    if (allNotes)
    {
        int len, prev = 0;
        for (int i = 0; i < tokens.size(); ++i)
        {
            const int pc = rootOf (tokens[i], len);
            if (i == 0) prev = nearest (pc);
            else { int d = ((pc - rowRoot - prev) % 12 + 12) % 12; if (d == 0) d = 12; prev += d; }        // each the next one up
            offsets.push_back (prev);
        }
        return true;
    }
    if (tokens.size() != 1) return false;
    int len;
    const int pc = rootOf (tokens[0], len);
    if (pc < 0) return false;
    const auto quality = normaliseQuality (tokens[0].substring (len).upToFirstOccurrenceOf ("/", false, false)
                                           + (tokens[0].substring (len).contains ("6/9") ? "/9" : ""));
    for (const auto& q : kQualities)
        if (quality == q.name || quality.toLowerCase() == String (q.name).toLowerCase())
        {
            const int base = nearest (pc);
            for (int n : q.notes) offsets.push_back (base + n);
            return true;
        }
    return false;
}

String MidiFx::bandChordNotes (int rowRoot, const std::vector<int>& offsets, bool flats)
{
    static const char* sharps[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char* flatNames[] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
    StringArray names;
    for (int o : offsets) names.add ((flats ? flatNames : sharps)[((rowRoot + o) % 12 + 12) % 12]);
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
        default: return {};
    }
}

} // namespace perf
