#pragma once

#include "Engine.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace perf
{

/** A backing-track MIDI file, held for editing: its notes as notes (start,
    end, key, velocity), everything else as the file had it, and the tempo
    map to place them in seconds. Times are in the file's own ticks, so a
    file saved unchanged is the same music. */
struct MidiDoc
{
    struct Note
    {
        int track = 0, channel = 1, key = 60, velocity = 100;
        juce::int64 start = 0, end = 0;
        bool operator== (const Note& o) const { return track == o.track && channel == o.channel && key == o.key && velocity == o.velocity && start == o.start && end == o.end; }
    };

    int ticksPerQuarter = 960;
    std::vector<Note> notes;
    std::vector<juce::MidiMessageSequence> others;    // per track: everything but the notes
    int timeSigNumerator = 4, timeSigDenominator = 4;

    bool load (const juce::File&, juce::String& error);
    /** Writes the file, keeping the first version beside it as <name>.orig.mid. */
    bool save (const juce::File&, juce::String& error) const;

    double tickToSeconds (juce::int64) const;
    juce::int64 secondsToTick (double) const;
    juce::int64 lengthTicks() const;
    juce::int64 ticksPerBar() const { return (juce::int64) ticksPerQuarter * 4 * timeSigNumerator / juce::jmax (1, timeSigDenominator); }

    /** What the file plays on a channel: its first program, and a track name if it has one. */
    int firstProgram (int channel) const;
    juce::String partName (int channel) const;
    bool channelUsed (int channel) const;
    int trackFor (int channel) const;             // the track new notes on this channel go to

private:
    struct Tempo { juce::int64 tick; double seconds; double secondsPerTick; };
    std::vector<Tempo> tempos;
    void buildTempoMap();
};

/** Opens the MIDI editor for one MIDI stem of a song: a lane per part, each
    with its sound and mix, and a piano roll for the notes. `changed` is
    called whenever the stem's settings change, so a list showing it can
    refresh. */
void showMidiEditor (Engine&, int songIndex, int stemIndex, std::function<void()> changed);

} // namespace perf
