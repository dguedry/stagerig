#include "LowLatency.h"
#include <cstdlib>
#if JUCE_WINDOWS
 #include <windows.h>
 #include <avrt.h>
 #pragma comment (lib, "avrt.lib")
#else
 #include <pthread.h>
 #include <sched.h>
 #include <dlfcn.h>
 #include <sys/resource.h>
#endif

using namespace juce;

namespace perf::LowLatency
{

#if ! JUCE_WINDOWS

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

#else
//==============================================================================
/* Windows: no PipeWire, no JACK by default, no pw-metadata. The audio device's
   own driver (ASIO or WASAPI) decides the block size, set in Audio... The
   audio thread is put in the "Pro Audio" MMCSS class, Windows' equivalent of
   realtime scheduling, which needs no permission. */
static bool pipeWireJack = false;
bool preloadPipeWireJack()                    { return false; }
bool pipeWireJackLoaded()                     { return pipeWireJack; }
void setRequestedQuantum (int, int)           {}
bool realtimeSchedulingAvailable()            { return true; }
String realtimeSchedulingHint()               { return "realtime scheduling available (MMCSS Pro Audio)"; }

static thread_local bool mmcssJoined = false;
bool currentThreadIsRealtime()                { return mmcssJoined; }

bool makeCurrentThreadRealtime (int)
{
    if (mmcssJoined) return true;
    DWORD taskIndex = 0;
    mmcssJoined = ::AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex) != nullptr;
    return mmcssJoined;
}

ClockState readClock()                        { return {}; }
bool forceClock (int, int)                    { return false; }
bool restoreClock (const ClockState&)         { return true; }
#endif

} // namespace perf::LowLatency
