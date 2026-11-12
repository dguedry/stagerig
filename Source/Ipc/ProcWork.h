#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>

/** Whether a process and everything under it is doing something, to tell a
    plugin that is slow from one that is stuck. A Windows plugin in Wine can
    take a minute to start (KORG's TRITON reads some 400 MB of samples, a few
    megabytes a second, every time it is made): silent that long, but reading
    all the while. One that is wedged reads nothing. */
namespace perf::procwork
{
/** `root` and every process descended from it. */
inline std::vector<pid_t> tree (pid_t root)
{
    std::vector<std::pair<pid_t, pid_t>> procs;   // (pid, ppid)
    if (DIR* d = ::opendir ("/proc"))
    {
        while (auto* e = ::readdir (d))
        {
            const pid_t pid = (pid_t) std::atoi (e->d_name);
            if (pid <= 0) continue;
            FILE* f = std::fopen (("/proc/" + std::string (e->d_name) + "/stat").c_str(), "r");
            if (f == nullptr) continue;
            char buf[512] = {};
            const auto n = std::fread (buf, 1, sizeof (buf) - 1, f);
            std::fclose (f);
            if (n == 0) continue;
            const char* close = std::strrchr (buf, ')');      // "pid (comm) state ppid": comm may hold spaces
            if (close == nullptr) continue;
            int ppid = 0; char state = 0;
            if (std::sscanf (close + 1, " %c %d", &state, &ppid) == 2) procs.emplace_back (pid, (pid_t) ppid);
        }
        ::closedir (d);
    }
    std::vector<pid_t> found { root };
    for (size_t i = 0; i < found.size(); ++i)
        for (const auto& [pid, ppid] : procs)
            if (ppid == found[i]) found.push_back (pid);
    return found;
}

struct Work { unsigned long long cpuTicks = 0, bytesRead = 0; };

/** The processor time used and the bytes read so far by `root` and all under it. */
inline Work measure (pid_t root)
{
    Work w;
    for (const auto pid : tree (root))
    {
        const auto base = "/proc/" + std::to_string (pid);
        if (FILE* f = std::fopen ((base + "/stat").c_str(), "r"))
        {
            char buf[1024] = {};
            const auto n = std::fread (buf, 1, sizeof (buf) - 1, f);
            std::fclose (f);
            const char* close = n > 0 ? std::strrchr (buf, ')') : nullptr;
            unsigned long long ut = 0, st = 0;
            // after ')': state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime
            if (close != nullptr && std::sscanf (close + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2)
                w.cpuTicks += ut + st;
        }
        if (FILE* f = std::fopen ((base + "/io").c_str(), "r"))
        {
            char line[128];
            while (std::fgets (line, sizeof (line), f) != nullptr)
                if (std::strncmp (line, "rchar:", 6) == 0) { w.bytesRead += std::strtoull (line + 6, nullptr, 10); break; }
            std::fclose (f);
        }
    }
    return w;
}

/** Busy between two looks `seconds` apart: reading a megabyte a second or
    more, or using most of a processor. (A window's idle message loop in Wine
    uses a fifth of one, and is not work.) */
inline bool busyBetween (const Work& before, const Work& after, double seconds)
{
    const double ticks = (double) ::sysconf (_SC_CLK_TCK);
    const double cpu = ticks > 0 ? (double) (after.cpuTicks - before.cpuTicks) / ticks / seconds : 0.0;
    const double read = (double) (after.bytesRead - before.bytesRead) / seconds;
    return after.bytesRead >= before.bytesRead && after.cpuTicks >= before.cpuTicks && (read >= 1.0e6 || cpu >= 0.6);
}
}
