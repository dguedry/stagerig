// Raise RLIMIT_NOFILE's soft limit to the hard limit before JUCE's static
// initialisers run.
//
// juce_core (juce_SharedCode_posix.h, MaxNumFileHandlesInitialiser) runs at
// static-init time: it first asks for RLIM_INFINITY, which fails on any normal
// desktop (systemd's hard limit is 524288), then falls back to 8192 and sets
// BOTH rlim_cur and rlim_max to it. That lowers the hard limit, so neither this
// process nor anything it starts (the plugin host, yabridge, wine) can raise it
// again. Kontakt and other sample players open a file per sample and wine needs
// extra descriptors of its own, so 8192 runs out ("Too many open files").
//
// JUCE leaves the limit alone when rlim_cur is already >= 8192, so lifting the
// soft limit to the hard limit first keeps the hard limit intact. A priority
// constructor runs before ordinary (unprioritised) static initialisers on
// GCC/Clang ELF targets, which is what JUCE's is.

#include <sys/resource.h>

namespace
{
__attribute__ ((constructor (101))) void raiseFileHandleSoftLimit()
{
    rlimit lim {};
    if (::getrlimit (RLIMIT_NOFILE, &lim) != 0)
        return;

    if (lim.rlim_cur < lim.rlim_max)
    {
        lim.rlim_cur = lim.rlim_max;
        ::setrlimit (RLIMIT_NOFILE, &lim);
    }
}
} // namespace
