#include "MidiFx.h"
#include <algorithm>
#include <cmath>

namespace perf
{

using namespace juce;

const char* const MidiFx::arpRates[]   = { "1/4", "1/8", "1/16", "1/8 triplet", "1/32" };
const char* const MidiFx::arpModes[]   = { "Up", "Down", "Up and down", "Random", "As played" };
const char* const MidiFx::chordTypes[] = { "Major", "Minor", "Dominant 7th", "Major 7th", "Minor 7th", "Sus 2", "Sus 4", "Power chord (5th)", "Octave", "Octaves above and below" };
const char* const MidiFx::scaleModes[] = { "Major", "Natural minor", "Harmonic minor", "Dorian", "Mixolydian", "Lydian", "Phrygian",
                                           "Major pentatonic", "Minor pentatonic", "Blues", "Whole tone" };
const char* const MidiFx::noteNames[]  = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
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
            numHeld = 0; numPressed = 0; sounding = -1; step = 0; samplesToStep = 0; samplesToOff = -1; goingUp = true;
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
            // Keep the step clock honest when the block ended inside a step.
            if (numHeld > 0 && at >= numSamples) {}
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
        bool goingUp = true;
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
        void reset() override { for (auto& c : mapped) for (auto& n : c) n = -1; }
        void process (const MidiBuffer& in, MidiBuffer& out, int, double, double) override
        {
            for (const auto meta : in)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOn())
                {
                    const int ch = jlimit (1, 16, m.getChannel()), key = m.getNoteNumber();
                    const int to = snap (key);
                    mapped[ch - 1][key] = to;
                    out.addEvent (MidiMessage::noteOn (ch, to, m.getVelocity()), meta.samplePosition);
                }
                else if (m.isNoteOff())
                {
                    const int ch = jlimit (1, 16, m.getChannel()), key = m.getNoteNumber();
                    const int to = mapped[ch - 1][key] >= 0 ? mapped[ch - 1][key] : snap (key);
                    mapped[ch - 1][key] = -1;
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
        default: return nullptr;
    }
    fx->configure (def);
    fx->reset();
    return fx;
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
        default: return {};
    }
}

} // namespace perf
