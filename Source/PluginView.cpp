#include "PluginView.h"
#include <juce_events/juce_events.h>

using namespace juce;

namespace perf
{

/* Kill and collect. JUCE's ChildProcess::kill() sends the signal and no
   more, and its destructor does not wait either, so a helper stopped that
   way stays in the process table as a zombie for as long as StageRig runs. */
static void endProcess (std::unique_ptr<juce::ChildProcess>& p)
{
    if (p == nullptr) return;
    /* Only a child still running is signalled: kill() uses the pid it
       remembers, and once the child has been reaped that pid could belong to
       someone else. */
    if (p->isRunning()) p->kill();
    p->waitForProcessToFinish (2000);
    p.reset();
}


namespace
{
    struct Running
    {
        PluginView::Session session;
        std::unique_ptr<ChildProcess> vnc, web;
    };

    CriticalSection lock;
    OwnedArray<Running> sessions;

    /* Ports above the tablet's own. One pair per window, so two plugin windows
       can be open at once without fighting over a port. */
    constexpr int kFirstVncPort = 5910;
    constexpr int kFirstWebPort = 6910;
    constexpr int kMaxSessions  = 10;      // ports 5910-5919 and 6910-6919 are ours

    File findTool (const String& name)
    {
        for (auto* dir : { "/usr/bin", "/usr/local/bin", "/bin" })
            if (auto f = File (dir).getChildFile (name); f.existsAsFile())
                return f;
        return {};
    }

    /** The noVNC web client, wherever the distribution put it. */
    File findNoVncRoot()
    {
        for (auto* dir : { "/usr/share/novnc", "/usr/share/webapps/novnc", "/usr/local/share/novnc" })
            if (auto d = File (dir); d.isDirectory() && d.getChildFile ("vnc.html").existsAsFile())
                return d;
        return {};
    }

    /** Is there an X display these tools can actually work with?

        Installed is not the same as usable. Everything here reads the screen
        through X: x11vnc mirrors an X window and xdotool finds it by name. On
        a Wayland session without Xwayland they are present, they start, and
        they see nothing -- the tablet would show a black rectangle instead of
        a plugin, with no hint as to why. JUCE itself is X11-only on Linux, so
        if StageRig is running at all there is normally an X display here;
        this catches the case where there is not. */
    bool hasUsableXDisplay()
    {
        if (SystemStats::getEnvironmentVariable ("DISPLAY", {}).isEmpty())
            return false;

        /* Ask xdotool to do the simplest thing it can. If it cannot reach the
           display it fails here rather than when a plugin window is open. */
        auto xdotool = findTool ("xdotool");
        if (xdotool == File()) return false;

        ChildProcess p;
        if (! p.start (StringArray { xdotool.getFullPathName(), "search", "--name", "^$" },
                       ChildProcess::wantStdOut | ChildProcess::wantStdErr))
            return false;

        const auto out = p.readAllProcessOutput();
        if (! p.waitForProcessToFinish (3000))
        {
            p.kill();
            return false;
        }

        /* A search that matches nothing still talks to X successfully, so the
           test is whether the display opened at all. xdotool says so on
           stderr and exits non-zero; both are checked because the wording has
           changed between versions. */
        return ! out.containsIgnoreCase ("open display")
            && ! out.containsIgnoreCase ("xdo instance");
    }
}

//==============================================================================
bool PluginView::available (String& whatIsMissing)
{
    StringArray missing;
    if (findTool ("x11vnc") == File())     missing.add ("x11vnc");
    if (findTool ("websockify") == File()) missing.add ("websockify");
    if (findTool ("xdotool") == File())    missing.add ("xdotool");
    if (findNoVncRoot() == File())         missing.add ("novnc");

    if (! missing.isEmpty())
    {
        whatIsMissing = "Showing a plugin's window on the tablet needs "
                      + missing.joinIntoString (", ") + ". Install "
                      + (missing.size() == 1 ? "it" : "them") + " and try again.";
        return false;
    }

    /* The tools are there; can they see anything? Checked second so the
       message names the real problem rather than sending someone off to
       install something they already have. */
    if (! hasUsableXDisplay())
    {
        whatIsMissing = "Showing a plugin's window on the tablet needs an X display, "
                        "and there is not one here. On a Wayland session this means "
                        "Xwayland is missing: install it (package xwayland) and log "
                        "back in, or use an X11 session instead.";
        return false;
    }

    return true;
}

static unsigned long findWindowImpl (const String& title, bool visibleOnly);

unsigned long PluginView::findWindow (const String& title)         { return findWindowImpl (title, false); }
unsigned long PluginView::findVisibleWindow (const String& title)  { return findWindowImpl (title, true); }

static unsigned long findWindowImpl (const String& title, bool visibleOnly)
{
    /* Ask the window manager rather than the plugin: the editor belongs to a
       helper process, and its X window is not ours to look up directly. The
       title is one StageRig chose and is unique per slot. */
    auto xdotool = findTool ("xdotool");
    if (xdotool == File()) return 0;

    /* xdotool reads --name as a regular expression. A program called
       "Strings (Pad)" would then be looking for "Strings Pad" and never find
       its own window, so every character that means something to a regex is
       escaped first. */
    String pattern;
    for (auto c : title)
    {
        if (c < 128 && String ("\\^$.|?*+()[]{}").containsChar (c))
            pattern << '\\';
        pattern << c;
    }

    StringArray args { xdotool.getFullPathName(), "search" };
    if (visibleOnly) args.add ("--onlyvisible");
    args.addArray ({ "--name", pattern });
    ChildProcess p;
    if (! p.start (args))
        return 0;

    const auto out = p.readAllProcessOutput();
    p.waitForProcessToFinish (4000);

    for (const auto& line : StringArray::fromLines (out))
        if (const auto id = line.trim().getLargeIntValue(); id > 0)
            return (unsigned long) id;
    return 0;
}

//==============================================================================
/* Brings the editor to the front. x11vnc injects the tablet's clicks with
   XTest at screen coordinates, so they land on whatever window is on top at
   that spot -- if the browser, or StageRig's own window, overlaps the editor,
   the pointer visibly moves to the right place and the click goes to the wrong
   window. Measured: a covered button never fires; uncover it and the same
   click does. Raising the editor whenever the tablet asks for it is the most
   that can be done from here; the manual says the rest. */
static void raiseWindow (unsigned long windowId)
{
    auto xdotool = findTool ("xdotool");
    if (xdotool == File() || windowId == 0) return;

    for (const char* verb : { "windowactivate", "windowraise" })
    {
        ChildProcess p;
        if (p.start (StringArray { xdotool.getFullPathName(), verb, String ((int64) windowId) }))
            p.waitForProcessToFinish (1500);
    }
}

//==============================================================================
PluginView::Session PluginView::start (const String& title, String& error)
{
    const ScopedLock sl (lock);

    for (auto* r : sessions)
        if (r->session.title == title)
        {
            /* Already serving, unless the window has gone since -- or either
               helper has. A half-dead session is worse than none: the port
               answers, so the tab opens and then hangs. */
            if (findWindow (title) == r->session.windowId
                && r->vnc != nullptr && r->vnc->isRunning()
                && r->web != nullptr && r->web->isRunning())
            {
                raiseWindow (r->session.windowId);
                return r->session;
            }

            endProcess (r->web);
            endProcess (r->vnc);
            sessions.removeObject (r);
            break;
        }

    if (String missing; ! available (missing)) { error = missing; return {}; }

    const auto window = findWindow (title);
    if (window == 0)
    {
        error = "The plugin's window could not be opened on the computer. Try again, or open it on the computer first.";
        return {};
    }
    raiseWindow (window);

    /* A free pair of ports. Probed, not assumed: a stray from a crash, or
       anything else on the machine, may be sitting on one, and starting x11vnc
       on a busy port fails silently from here -- the session looks fine and
       the tablet connects to nothing. */
    auto portFree = [] (int port)
    {
        StreamingSocket probe;
        const bool ok = probe.createListener (port, "127.0.0.1");
        probe.close();
        return ok;
    };
    auto inUse = [] (int vnc, int web)
    {
        for (auto* r : sessions)
            if (r->session.vncPort == vnc || r->session.webPort == web) return true;
        return false;
    };

    int vncPort = 0, webPort = 0;
    for (int i = 0; i < kMaxSessions; ++i)
    {
        const int v = kFirstVncPort + i, w = kFirstWebPort + i;
        if (inUse (v, w) || ! portFree (v) || ! portFree (w)) continue;
        vncPort = v; webPort = w;
        break;
    }
    if (vncPort == 0)
    {
        error = "No free port for the plugin window.";
        return {};
    }

    auto running = std::make_unique<Running>();
    running->session = { title, window, vncPort, webPort };

    /* -localhost: the stream itself never leaves this machine. The tablet talks
       to websockify, which is what we expose, so there is one door rather than
       two. -nopw is safe for the same reason. */
    running->vnc = std::make_unique<ChildProcess>();
    if (! running->vnc->start (StringArray {
            findTool ("x11vnc").getFullPathName(),
            "-id", String ((int64) window),
            "-localhost", "-rfbport", String (vncPort),
            "-nopw", "-forever", "-shared", "-quiet", "-noxdamage" }))
    {
        error = "Could not start x11vnc.";
        return {};
    }

    Thread::sleep (400);            // let it bind before websockify dials in

    running->web = std::make_unique<ChildProcess>();
    if (! running->web->start (StringArray {
            findTool ("websockify").getFullPathName(),
            "--web=" + findNoVncRoot().getFullPathName(),
            // Loopback only: the tablet reaches this through StageRig's own
            // port, so the bridge itself never needs to be on the network.
            "127.0.0.1:" + String (webPort),
            "127.0.0.1:" + String (vncPort) }))
    {
        endProcess (running->vnc);
        error = "Could not start websockify.";
        return {};
    }

    const auto session = running->session;
    sessions.add (running.release());
    return session;
}

void PluginView::stop (const String& title)
{
    const ScopedLock sl (lock);
    for (auto* r : sessions)
        if (r->session.title == title)
        {
            endProcess (r->web);
            endProcess (r->vnc);
            sessions.removeObject (r);
            return;
        }
}

void PluginView::stopAll()
{
    {
        const ScopedLock sl (lock);
        for (auto* r : sessions)
        {
            endProcess (r->web);
            endProcess (r->vnc);
        }
        sessions.clear();
    }
    sweepStrays();
}

void PluginView::sweepStrays()
{
    /* Anything of ours still holding a port after its ChildProcess has gone --
       after a crash, or a websockify that outlived StageRig, which once kept
       port 7777 bound so the next run could not start its tablet server at
       all. The patterns are the exact argument shapes this file launches, on
       our port range only, so nothing else on the machine can match: a VNC
       server someone runs for their own desktop is on other ports and has
       other arguments. */
    auto pkill = findTool ("pkill");
    if (pkill == File()) return;

    /* Anchored at both ends to the exact command lines start() builds. pkill
       -f matches anywhere in a process's whole command line, so an unanchored
       pattern also matches a terminal, an editor or a script that merely
       mentions those words -- which is how a test shell got killed while this
       was being checked. With ^ and $ only a process that IS one of ours fits.
       websockify is a Python script, so its argv[0] is the interpreter. */
    const String vncRange = "59[1][0-9]";        // 5910-5919
    const String webRange = "69[1][0-9]";        // 6910-6919
    for (const String pattern : {
            "^\\S*x11vnc -id [0-9]+ -localhost -rfbport " + vncRange
                + " -nopw -forever -shared -quiet -noxdamage$",
            "^(\\S+ )?\\S*websockify --web=\\S+ 127\\.0\\.0\\.1:" + webRange
                + " 127\\.0\\.0\\.1:" + vncRange + "$" })
    {
        ChildProcess p;
        if (p.start (StringArray { pkill.getFullPathName(), "-f", pattern }))
            p.waitForProcessToFinish (2000);
    }
}

void PluginView::dropDeadSessions()
{
    const ScopedLock sl (lock);
    // Once a second: each look is an xdotool call per session.
    static uint32 lastMs = 0;
    const auto now = Time::getMillisecondCounter();
    if (! sessions.isEmpty() && now - lastMs < 1000) return;
    lastMs = now;

    for (int i = sessions.size(); --i >= 0;)
    {
        auto* r = sessions.getUnchecked (i);
        /* The window must be on screen, not just exist: an editor closed on
           the computer is hidden, and a mirror of it shows the last picture
           while every tap lands on whatever is under it. Ending the session
           lets the tablet page say the window was closed. GUI reopens it. */
        if (findVisibleWindow (r->session.title) == r->session.windowId
            && r->vnc != nullptr && r->vnc->isRunning()
            && r->web != nullptr && r->web->isRunning())
            continue;

        endProcess (r->web);
        endProcess (r->vnc);
        sessions.remove (i);
    }
}

Array<PluginView::Session> PluginView::active()
{
    const ScopedLock sl (lock);
    Array<Session> out;
    for (auto* r : sessions) out.add (r->session);
    return out;
}

} // namespace perf
