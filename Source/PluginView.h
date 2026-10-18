#pragma once

#include <juce_core/juce_core.h>

namespace perf
{

/** A plugin's own window, shown on the tablet.

    Some plugins cannot be controlled any other way. Kontakt's host-visible
    parameters are its MIDI controller INPUTS: values go in, and nothing comes
    back out -- asking it what CC 12 is set to returns whatever was last written,
    not where the drawbar actually sits. Measured, not assumed. So a set of
    faders on the tablet can drive that organ but can never follow it.

    Mirroring the plugin's own window sidesteps that entirely: what you see is
    what the plugin is showing, because it IS what the plugin is showing.

    This is for setting up and adjusting, not for playing. A plugin window on a
    tablet is a picture of an interface built for a mouse, so expect to tap
    precisely and to miss occasionally. */
class PluginView
{
public:
    /** Everything needed to serve one plugin window. */
    struct Session
    {
        juce::String title;        // the editor window's title, as StageRig set it
        unsigned long windowId = 0;
        int vncPort = 0;
        int webPort = 0;
        bool isValid() const { return windowId != 0 && vncPort != 0 && webPort != 0; }
    };

    /** True when the tools to do this are installed. */
    static bool available (juce::String& whatIsMissing);

    /** Finds the X window of an editor StageRig opened with this title. */
    static unsigned long findWindow (const juce::String& title);
    /** The same, but only if the window is on screen: a hidden editor is one
        the player closed, and its mirror should end rather than show a stale
        picture whose taps land on nothing. */
    static unsigned long findVisibleWindow (const juce::String& title);

    /** Starts serving that window, or returns a session that is not valid with
        `error` set. Safe to call again for a window already being served. */
    static Session start (const juce::String& title, juce::String& error);

    /** Stops serving one window, or everything. */
    static void stop (const juce::String& title);
    static void stopAll();

    /** Kills any x11vnc or websockify of ours left over from an earlier run --
        after a crash they keep their ports, and every GUI press would fail until
        someone killed them by hand. Matches only our own argument shapes on our
        own port range. */
    static void sweepStrays();

    /** The sessions running right now. */
    static juce::Array<Session> active();

    /** Drops any session whose window has gone. A plugin editor closes when its
        program is swapped out, and a bridge left pointing at a dead window
        answers connections and then sends nothing -- which looks exactly like a
        broken stream rather than a stale one. */
    static void dropDeadSessions();
};

} // namespace perf
