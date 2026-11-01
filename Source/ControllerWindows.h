#pragma once

#include "Surfaces.h"

namespace perf
{

/** The controller setup of one instrument: which setup it uses, and the
    setup itself -- pages of eight knobs, each a parameter with a name for
    the screen, and coloured runs of keys -- with import and export, so a
    setup made once can be passed on. `changed` is called when the setup's
    file or the instrument's choice changed. Returns the window, which holds
    on to the hub and the library: it must be closed before they go. */
juce::Component* showControlMapEditor (Engine&, SurfaceHub&, ControlMapLibrary&, int input, int program, int slot, std::function<void()> changed);

/** The keyboards StageRig talks to, and what each input's knobs and
    buttons do: taught by turning or pressing them. Returns the window, as above. */
juce::Component* showControllers (Engine&, SurfaceHub&, int selectedInput, std::function<void()> changed);

} // namespace perf
