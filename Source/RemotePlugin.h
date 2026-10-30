#pragma once

#include "ParamInfo.h"
#include "Ipc/Protocol.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace perf
{

/**
    A plugin running in its own stagerig-plugin-host process.

    Non-realtime methods (load, state, parameters, editor) talk over a socket and
    block the calling thread for at most their timeout. Real-time methods
    (queueParameterChange / beginProcess / finishProcess) use shared memory and
    never block past the deadline the caller supplies; a plugin that misses its
    deadline simply contributes silence for that block.
*/
class RemotePlugin
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        // All of these arrive on the plugin's reader thread.
        virtual void remoteParameterTouched (RemotePlugin&, int index, float value) { juce::ignoreUnused (index, value); }
        virtual void remoteParameterChanged (RemotePlugin&, int index, float value) { juce::ignoreUnused (index, value); }
        virtual void remoteEditorClosed (RemotePlugin&) {}
        virtual void remoteDied (RemotePlugin&) {}
    };

    RemotePlugin();
    ~RemotePlugin();

    /** Finds the helper executable: $STAGERIG_PLUGIN_HOST, next to this executable,
        or in the sibling JUCE artefacts folder of a build tree. */
    static juce::File findHostExecutable();

    /** Environment for helper processes.

        A plugin has to run with the wine that owns its prefix, or that wine will
        try to update the prefix and corrupt it. vstenv arranges this inside the
        prefix -- it records the loader there and teaches yabridge-host.exe to
        read it -- so nothing is needed from us. The older convention, which
        a hand-built prefix may still use, puts a ~/.local/bin/wine
        shim and WINELOADER/WINEFSYNC in the session, and a desktop launcher does
        not always have those. So they are applied here explicitly, which is
        harmless when the prefix does not need them.

        The returned strings are "NAME=value"; `pointers` receives a
        null-terminated envp. */
    static std::vector<std::string> buildHelperEnvironment (std::vector<char*>& pointers);

    /** Spawns the host process and loads the plugin. Blocks until done or timed out. */
    bool load (const juce::PluginDescription&, double sampleRate, int blockSize, juce::String& error);
    bool prepare (double sampleRate, int blockSize);

    const juce::PluginDescription& getDescription() const   { return description; }
    juce::String getName() const                            { return name; }
    bool isInstrument() const                               { return instrument; }
    bool hasEditor() const                                  { return editorAvailable; }
    bool isAlive() const                                    { return alive.load(); }
    juce::String getLastError() const;
    /** The host process, or -1 once it has been reaped. For tests and diagnostics. */
    pid_t getHostPid() const                                { const std::lock_guard<std::mutex> l (pidMutex); return pid; }

    // Parameters -------------------------------------------------------------------
    const ParamInfoList& getParameters() const              { return params; }
    int findParameterIndex (const juce::String& id) const;
    float getCachedParameterValue (int index) const;
    /** Round trip to the plugin for the current value. */
    bool fetchParameterValue (int index, float& value);

    /** The same, but gives up almost immediately. For polling on the message
        thread, where a plugin that has stopped answering must cost a frame
        rather than freeze the window for the usual ten-second timeout. */
    bool pollParameterValue (int index, float& value);
    /** Non-realtime set (UI / tests). A knob being turned passes a short
        timeout, so a plugin that has stopped answering costs a moment, not
        the usual ten seconds; -1 is the usual. */
    bool setParameterValue (int index, float value, int timeoutMs = -1);

    // State / editor --------------------------------------------------------------------
    /** `timeoutMs` < 0 means the usual 30 s. A quit passes something short:
        a host that has stopped answering must not hold the exit for long.
        A successful read marks the state as captured (see stateMayHaveChanged). */
    bool getState (juce::MemoryBlock&, int timeoutMs = -1);
    /** True since the last getState()/setState() if anything could have
        changed the plugin's state: a parameter moved (by us or by the plugin),
        its window shown or closed, or a fresh load with no state restored.
        Asking a plugin for its state is not free -- many stall their audio
        while serialising -- so the autosave asks only those that say yes. */
    bool stateMayHaveChanged() const                        { return stateDirty.load(); }
    bool setState (const juce::MemoryBlock&);
    bool showEditor (const juce::String& title);
    bool hideEditor();
    /** Keep the editor above every other window, or stop doing so. */
    bool keepEditorOnTop (bool onTop);
    bool isEditorOpen() const                               { return editorOpen.load(); }

    void setListener (Listener* l)                          { listener = l; }

    // Real-time ------------------------------------------------------------------------------
    /** Queue a parameter change to be applied with the next block. Audio thread only. */
    void queueParameterChange (int index, float value);
    /** Hands a block to the plugin. Input may be nullptr for instruments. Audio thread only. */
    void beginProcess (const float* inL, const float* inR, const juce::MidiBuffer&, int numSamples);

    /** The tempo reported to the plugin. Set from the message thread; the audio
        thread copies it into each block, so a change costs nothing per block. */
    void setTempo (double bpm, int timeSigNumerator, int timeSigDenominator);
    /** Back to bar one, beat one, from the next block: a backing track has just
        started, and a synced delay or arpeggiator should count from it.
        Safe from any thread. */
    void resetMusicalPosition()                             { positionReset.store (true, std::memory_order_relaxed); }
    /** Waits for the block until `deadline` (CLOCK_MONOTONIC). Returns false if the
        plugin is late or dead; the caller must then treat the output as silence.
        `midiOut`, when given, is replaced by the MIDI the plugin played. */
    bool finishProcess (float* outL, float* outR, const timespec& deadline, juce::MidiBuffer* midiOut = nullptr);
    int getMissedBlocks() const                             { return missedBlocks.load(); }

    /** Asks the process to quit and reaps it (or kills it after a grace period). */
    /** Tells the host to quit and does not wait. Call on every plugin first,
        then shutdown() on each: they then all exit at once rather than one
        after another, each with its own full wait. */
    void requestQuit();
    /** Reaps the host, giving it `patienceMs` to exit on its own before SIGKILL. */
    void shutdown (int patienceMs = 3000);
    /** Reaping runs in the background; a quit calls this last so no host is
        left behind. Returns when every reaper is done or after `maxWaitMs`. */
    static void waitForReapers (int maxWaitMs);

private:
    struct Pending
    {
        juce::WaitableEvent event;
        juce::MemoryBlock response;
        bool ok = false;
    };

    /** `killIfLate`: a host that stays silent for the whole timeout is wedged;
        kill it and mark the plugin dead rather than leave it to cost every
        later request the same wait. For the long state and editor requests. */
    bool request (ipc::Msg, const juce::MemoryBlock& payload, int timeoutMs, juce::MemoryBlock* response = nullptr,
                  bool killIfLate = false);
    bool waitForReply (Pending&, int timeoutMs);
    /** True once the host process has exited; reaps it the first time. */
    bool hostExited();
    void killHost();
    void readerLoop();
    void handleNotification (const ipc::FrameHeader&, const juce::MemoryBlock&);
    void markDead (const juce::String& why);
    bool spawn (juce::String& error);
    void drainDoneSemaphore();

    juce::PluginDescription description;
    juce::String name, shmName;
    bool instrument = false, editorAvailable = false;
    ParamInfoList params;
    std::unique_ptr<std::atomic<float>[]> paramValues;   // sized after load

    mutable std::mutex pidMutex;
    pid_t pid = -1;             // under pidMutex: reaped from whichever thread notices first
    bool hostGone = false;      // under pidMutex
    int socketFd = -1;
    ipc::SharedBlock* shm = nullptr;

    std::thread reader;
    std::mutex sendMutex, pendingMutex;
    mutable std::mutex errorMutex;
    std::map<uint32_t, Pending*> pending;
    std::atomic<uint32_t> nextRequestId { 1 };
    std::atomic<bool> alive { false }, editorOpen { false }, stateDirty { true };
    juce::String lastError;
    Listener* listener = nullptr;

    // audio-thread state
    bool blockInFlight = false;
    int inFlightSamples = 0;
    uint32_t pendingParamCount = 0;
    ipc::ParamChange pendingParams[ipc::kMaxParamChanges];
    std::atomic<int> missedBlocks { 0 };

    // Tempo: written by the message thread, read by the audio thread each block.
    std::atomic<double> tempoBpm { 120.0 };
    std::atomic<int> tempoNumerator { 4 }, tempoDenominator { 4 };
    double ppqPosition = 0.0;              // audio thread only
    std::atomic<bool> positionReset { false };
    double currentSampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RemotePlugin)
};

} // namespace perf
