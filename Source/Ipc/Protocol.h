#pragma once

/*
    Wire protocol between StageRig and its plugin host processes.

    Real-time data (audio, MIDI, parameter changes) goes through a shared memory
    block with two POSIX semaphores. Everything else (loading, state, parameter
    lists, editor windows, notifications) goes through a socketpair with
    length-prefixed frames.
*/

#include <juce_core/juce_core.h>
#include <semaphore.h>
#include <atomic>
#include <cstdint>

namespace perf::ipc
{

constexpr uint32_t kMagic         = 0x50524631;   // "PRF1"
constexpr uint32_t kVersion       = 3;            // 2 added tempo to the block, 3 the plugin's MIDI out
constexpr int      kMaxBlock      = 8192;         // samples per block we can exchange
constexpr int      kMaxMidi       = 2048;
constexpr int      kMaxParamChanges = 512;

struct MidiEvent
{
    int32_t samplePosition;
    uint8_t size;
    uint8_t data[3];
};

struct ParamChange
{
    uint32_t index;
    float value;
};

/** One block of shared memory per hosted plugin. */
struct SharedBlock
{
    uint32_t magic;
    uint32_t version;

    sem_t request;                        // host -> plugin: a block is ready
    sem_t done;                           // plugin -> host: block processed
    std::atomic<uint32_t> requestSeq;     // host increments before posting
    std::atomic<uint32_t> completedSeq;   // plugin sets to requestSeq when done
    std::atomic<uint32_t> quit;           // host sets to make the plugin's RT loop exit

    // Block description (host writes, plugin reads)
    int32_t  numSamples;
    uint32_t midiCount;
    uint32_t paramChangeCount;

    /* Tempo, so a delay or arpeggiator that syncs to the host has something to
       sync to. StageRig has no transport -- there is nothing to play along to
       on stage -- so the position advances continuously and "playing" is always
       true: a plugin that waits for a running transport before it will run its
       LFO would otherwise sit silent. */
    double   bpm;
    double   ppqPosition;             // musical position, advanced per block
    int32_t  timeSigNumerator;
    int32_t  timeSigDenominator;

    MidiEvent   midi[kMaxMidi];
    ParamChange paramChanges[kMaxParamChanges];

    /* What the plugin played (plugin writes, host reads): an arpeggiator's
       notes, a chord generator's chords -- what a MIDI processor in a slot's
       chain hands on to the sound after it. */
    uint32_t    midiOutCount;
    MidiEvent   midiOut[kMaxMidi];

    // Audio: stereo in (effects), stereo out
    float inL[kMaxBlock], inR[kMaxBlock];
    float outL[kMaxBlock], outR[kMaxBlock];
};

//==============================================================================
/** Control-channel message types. */
enum class Msg : uint32_t
{
    // host -> plugin requests
    load = 1,             // PluginDescription XML, sampleRate(double), blockSize(int)
    prepare,              // sampleRate(double), blockSize(int)
    getParameters,        // -> count, then per param: index, id, name, automatable, discrete, boolean, value
    getParameterValue,    // index -> value
    setParameterValue,    // index, value
    getState,             // -> blob
    setState,             // blob
    showEditor,           // title
    hideEditor,
    keepEditorOnTop,      // bool: while a tablet mirrors the window, nothing may cover it
    getInfo,              // -> name, isInstrument, hasEditor, latency
    quit,

    // plugin -> host
    response = 100,       // requestId matches; payload: ok(bool), then request-specific data
    notifyParamTouched,   // index, value
    notifyParamChanged,   // index, value
    notifyEditorClosed,
    notifyLog,            // text
};

/** [uint32 payloadSize][uint32 type][uint32 requestId][payload] */
struct FrameHeader
{
    uint32_t payloadSize;
    uint32_t type;
    uint32_t requestId;
};

//==============================================================================
/** Blocking full write / read on a socket fd. Return false on error/EOF. */
bool writeAll (int fd, const void* data, size_t size);
bool readAll  (int fd, void* data, size_t size);

bool sendFrame (int fd, Msg type, uint32_t requestId, const juce::MemoryBlock& payload);
/** Reads one frame; returns false if the connection is gone. */
bool readFrame (int fd, FrameHeader& header, juce::MemoryBlock& payload);

/** Creates (host) or opens (plugin) the shared block. Returns nullptr on failure. */
SharedBlock* createSharedBlock (const juce::String& name);
SharedBlock* openSharedBlock (const juce::String& name);
void closeSharedBlock (SharedBlock*);
void unlinkSharedBlock (const juce::String& name);

/** Waits on `sem` until it's posted or the absolute CLOCK_MONOTONIC deadline passes. */
bool waitSemaphoreUntil (sem_t& sem, const timespec& deadline);
timespec monotonicDeadline (double secondsFromNow);

} // namespace perf::ipc
