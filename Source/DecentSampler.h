#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace perf
{

/** Decent Sampler instruments (.dspreset), played by the built-in sampler.
    A preset is an XML file beside its samples, much like an SFZ file: it is
    turned into the SFZ that plays the same, in memory, as it is loaded.

    Carried over: the samples and where they sit on the keys and velocities,
    volume, tuning and pan at every level, the amplitude envelope, round
    robins, release, first and legato samples, pedal-triggered samples and
    loops. The preset's knobs start where its maker set them; a knob that
    moves the envelope or a volume (a mic mix, Release) is a control of the
    sampler, so it is a knob in StageRig too. Left out: the preset's effects
    (StageRig has its own), oscillators, background sounds that play with no
    key down, and buttons and menus that switch groups on and off (the groups
    play as the preset starts). */
namespace DecentSampler
{
    /** The SFZ for this preset, with its samples' paths as the preset gives
        them (relative to its folder). Empty, and `error` set, when it has no
        samples to play. */
    juce::String toSfz (const juce::File& preset, juce::String& error);

    /** A .dslibrary is a zip of a library's folder. Unpacks each one in
        `folder` (a few folders deep) beside itself, into a folder of its own
        name, once. Returns the folders made. */
    juce::Array<juce::File> unpackLibraries (const juce::File& folder);
}

} // namespace perf
