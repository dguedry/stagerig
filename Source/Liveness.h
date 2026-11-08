#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace perf
{

/** A page shared between StageRig and its watchman (Main.cpp), made before the
    watchman forks. StageRig says it is alive here; the watchman restarts it
    when both the screen (the message thread) and the sound (the audio thread)
    have stood still for a while -- a hang, which a crash watch never sees.
    Null in a process with no watchman (tests, STAGERIG_NO_SUPERVISOR). */
struct Liveness
{
    std::atomic<int64_t>  heartbeatMs { 0 };    // the message thread, every housekeeping tick
    std::atomic<uint32_t> audioCalls { 0 };     // the audio thread, every callback
    std::atomic<int>      quitting { 0 };       // a quit is under way: a slow one is not a hang
};

inline Liveness* liveness = nullptr;

inline int64_t livenessNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace perf
