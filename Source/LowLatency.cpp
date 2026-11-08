#include "LowLatency.h"
#include <pthread.h>
#include <sched.h>
#include <dlfcn.h>
#include <sys/resource.h>
#include <cstdlib>
#include <unistd.h>
#include <memory>

using namespace juce;

namespace perf::LowLatency
{

static bool pipeWireJack = false;

bool preloadPipeWireJack()
{
    ChildProcess jackd;
    const bool realJack = jackd.start (StringArray { "pgrep", "-x", "jackd" })
                          && jackd.readAllProcessOutput().trim().isNotEmpty();
    jackd.waitForProcessToFinish (1000);    // collect it; ChildProcess alone leaves a zombie
    if (realJack)
        return false;   // a real JACK server: leave JUCE to its normal libjack

    for (auto* dir : { "/usr/lib/x86_64-linux-gnu/pipewire-0.3/jack", "/usr/lib64/pipewire-0.3/jack", "/usr/lib/pipewire-0.3/jack",
                       "/usr/local/lib/x86_64-linux-gnu/pipewire-0.3/jack", "/usr/local/lib/pipewire-0.3/jack" })
    {
        const File lib = File (dir).getChildFile ("libjack.so.0");
        if (! lib.existsAsFile()) continue;
        // RTLD_GLOBAL with the real soname: JUCE's later dlopen("libjack.so.0") resolves to this copy.
        if (::dlopen (lib.getFullPathName().toRawUTF8(), RTLD_NOW | RTLD_GLOBAL) != nullptr)
        {
            pipeWireJack = true;
            return true;
        }
    }
    return false;
}

bool pipeWireJackLoaded() { return pipeWireJack; }

void setRequestedQuantum (int quantum, int sampleRate)
{
    const auto v = String (jmax (16, quantum)) + "/" + String (jmax (8000, sampleRate));
    ::setenv ("PIPEWIRE_LATENCY", v.toRawUTF8(), 1);
}

bool realtimeSchedulingAvailable()
{
    rlimit rl {};
    if (::getrlimit (RLIMIT_RTPRIO, &rl) != 0) return false;
    return rl.rlim_cur > 0;
}

bool currentThreadIsRealtime()
{
    int policy = 0;
    sched_param sp {};
    if (::pthread_getschedparam (::pthread_self(), &policy, &sp) != 0) return false;
    return policy == SCHED_FIFO || policy == SCHED_RR;
}

bool makeCurrentThreadRealtime (int priority)
{
    if (! realtimeSchedulingAvailable()) return false;
    sched_param sp {};
    sp.sched_priority = jlimit (sched_get_priority_min (SCHED_RR), sched_get_priority_max (SCHED_RR), priority);
    return ::pthread_setschedparam (::pthread_self(), SCHED_RR, &sp) == 0;
}

String realtimeSchedulingHint()
{
    if (realtimeSchedulingAvailable()) return "realtime scheduling available";
    return "no realtime scheduling: add your user to the 'pipewire' group (sudo usermod -aG pipewire $USER), then log out and back in";
}

static String pwMetadata (const StringArray& args, int timeoutMs = 3000)
{
    ChildProcess p;
    StringArray cmd { "pw-metadata", "-n", "settings" };
    cmd.addArray (args);
    if (! p.start (cmd)) return {};
    p.waitForProcessToFinish (timeoutMs);
    return p.readAllProcessOutput();
}

ClockState readClock()
{
    ClockState s;
    for (auto& line : StringArray::fromLines (pwMetadata ({})))
    {
        // update: id:0 key:'clock.force-quantum' value:'1024' type:''
        const auto key = line.fromFirstOccurrenceOf ("key:'", false, false).upToFirstOccurrenceOf ("'", false, false);
        const auto value = line.fromFirstOccurrenceOf ("value:'", false, false).upToFirstOccurrenceOf ("'", false, false);
        if (key == "clock.force-quantum") s.forceQuantum = value;
        else if (key == "clock.force-rate") s.forceRate = value;
        else if (key == "clock.quantum") s.quantum = value;
        else if (key == "clock.rate") s.rate = value;
    }
    return s;
}

bool forceClock (int quantum, int sampleRate)
{
    if (quantum > 0) pwMetadata ({ "0", "clock.force-quantum", String (quantum) });
    if (sampleRate > 0) pwMetadata ({ "0", "clock.force-rate", String (sampleRate) });
    const auto now = readClock();
    return now.forceQuantum == String (quantum) && (sampleRate <= 0 || now.forceRate == String (sampleRate));
}

bool restoreClock (const ClockState& previous)
{
    // A force value of 0 means "not forced".
    pwMetadata ({ "0", "clock.force-quantum", previous.forceQuantum.isEmpty() ? "0" : previous.forceQuantum });
    pwMetadata ({ "0", "clock.force-rate", previous.forceRate.isEmpty() ? "0" : previous.forceRate });
    return true;
}

static std::unique_ptr<ChildProcess> performanceHold;

bool holdPerformanceMode (bool hold)
{
    if (! hold)
    {
        if (performanceHold != nullptr) performanceHold->kill();   // ending it releases the hold
        performanceHold.reset();
        return true;
    }
    if (performanceHold != nullptr && performanceHold->isRunning()) return true;

    /* powerprofilesctl holds the mode for as long as the command it launches runs;
       that command waits for StageRig to end. */
    auto p = std::make_unique<ChildProcess>();
    if (! p->start (StringArray { "powerprofilesctl", "launch", "--profile", "performance",
                                  "--reason", "StageRig is playing live", "--appid", "stagerig",
                                  "tail", "--pid=" + String ((int) getpid()), "-f", "/dev/null" }, 0))
        return false;
    Thread::sleep (300);    // an unknown profile or a missing service ends it at once
    if (! p->isRunning()) return false;
    performanceHold = std::move (p);
    return true;
}

bool performanceModeHeld() { return performanceHold != nullptr && performanceHold->isRunning(); }

} // namespace perf::LowLatency
