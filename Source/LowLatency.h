#pragma once

#include <juce_core/juce_core.h>

namespace perf::LowLatency
{

/** Makes JUCE's "JACK" device type talk to PipeWire: loads PipeWire's libjack
    (normally only reachable through `pw-jack`) before JUCE looks for libjack.so.0.
    Returns true if PipeWire's JACK library was found and loaded. Skipped when a
    real jackd is running. */
bool preloadPipeWireJack();
bool pipeWireJackLoaded();

/** Requests a graph position for our JACK/PipeWire client: PIPEWIRE_LATENCY=quantum/rate.
    Takes effect when the audio device is (re)opened. */
void setRequestedQuantum (int quantum, int sampleRate);

/** True if this process may use SCHED_FIFO/RR (RLIMIT_RTPRIO > 0). Without it the
    audio threads run at normal priority and small buffers will drop out. */
bool realtimeSchedulingAvailable();
juce::String realtimeSchedulingHint();
/** Whether the calling thread runs under SCHED_FIFO or SCHED_RR. */
bool currentThreadIsRealtime();
/** Puts the calling thread under SCHED_RR at `priority` (1..99). False when the
    process may not (RLIMIT_RTPRIO), in which case nothing changes. */
bool makeCurrentThreadRealtime (int priority);

/** Windows plugins run through yabridge make their sound on a thread inside
    Wine ("audio-0"), which yabridge puts at realtime priority 5 -- the same
    as every other thread of the plugin: Kontakt's disk loading, its database
    scan, its analytics. The sound then waits its turn among them, and behind
    anything the desktop gives a higher realtime priority. Raises those audio
    threads, and only those, to `priority` (below StageRig's own audio thread,
    which waits on them). Returns how many were raised. Cheap enough to call
    every few seconds; any thread. Linux only. */
int raiseBridgedAudioThreads (int priority);

/** PipeWire's session clock settings, as pw-metadata reports them. Empty = not set. */
struct ClockState { juce::String forceQuantum, forceRate, quantum, rate; };
ClockState readClock();
/** Pins the whole PipeWire graph to quantum/rate (clock.force-quantum / force-rate).
    Affects every application; meant for a dedicated live machine. */
bool forceClock (int quantum, int sampleRate);
/** Restores the force settings captured by readClock() (clears them if they were unset). */
bool restoreClock (const ClockState& previous);

/** Holds the computer in its performance power mode (power-profiles-daemon) while
    StageRig runs, so the processor does not slow down between notes and wake late.
    The hold belongs to a helper that ends with StageRig, a crash included, and the
    computer then goes back to the mode it was in. False when there is no such
    service (no powerprofilesctl, or no performance mode on this machine). */
bool holdPerformanceMode (bool hold);
bool performanceModeHeld();

} // namespace perf::LowLatency
