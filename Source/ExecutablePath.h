#pragma once

#include <juce_core/juce_core.h>

namespace perf
{

/** This program's file, always as a full path. JUCE gives it as it was
    started, resolved against the current working directory each time it is
    asked: started as ./build/.../StageRig, and after a file chooser has
    moved the working directory (JUCE's Linux chooser moves it to the folder
    it opens at, and leaves it there), that names a file that is not there --
    and the plugin host beside it is "not found". */
inline juce::File executableFile()
{
   #if JUCE_LINUX || JUCE_BSD
    // The kernel's link: absolute. Rebuilt while running, it ends " (deleted)"; its folder is still right.
    if (const auto target = juce::File ("/proc/self/exe").getLinkedTarget(); juce::File::isAbsolutePath (target.getFullPathName()))
    {
        const auto path = target.getFullPathName();
        return juce::File (path.endsWith (" (deleted)") ? path.dropLastCharacters (10) : path);
    }
   #endif
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile);
}

} // namespace perf
