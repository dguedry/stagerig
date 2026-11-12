#pragma once
#include <functional>

#include "Engine.h"
#include "PluginView.h"
#include <juce_core/juce_core.h>
#include <map>
#include <vector>

namespace perf
{

/** A tablet as a program selector.

    Serves a small web app on the local network: each input with its current
    program, the list of programs that have something in them, and a tap to
    switch. It is a progressive web app, so it can be added to a home screen and
    opens fullscreen with no installing.

    Security: this binds to the local network, where a venue's wifi is not
    friendly. Every request must carry a token that is generated per run and
    embedded in the URL you scan; without it the server answers 403. That stops
    someone on the same network changing your sounds by guessing the address.
    It is not protection against someone who can read your screen, which is the
    right level for a stage tool. */
class SurfaceHub;

class RemoteServer : private juce::Thread,
                     private Engine::Listener
{
public:
    /** Called on the message thread when the stand has changed the setup (a knob, a scene),
        so the computer counts it as unsaved and the autosave picks it up. */
    std::function<void()> onEdited;
    /** The setup file, from the stand: saved, saved under a name, another one opened.
        Set by the window, called on the message thread.
        onSetupList: { name, files: [{ name, when, current }] } -- the setups beside the open one.
        onSetupSave: no name saves the open file. Says what went wrong, or nothing; "name" when
        the setup has no file yet, "exists" when the name is another file's and over is not set.
        onSetupOpen: says what went wrong, or nothing. */
    std::function<juce::String()> onSetupName;       // the open setup's name; nothing for one never saved
    std::function<juce::var()> onSetupList;
    std::function<juce::String (const juce::String& name, bool over)> onSetupSave;
    std::function<juce::String (const juce::String& name)> onSetupOpen;
    /** Whether a request changes how the show is built (set lists, scenes, what a song
        loads) rather than playing it. The one list the editing lock is checked against:
        a new route that edits goes here, and is locked with the rest. */
    static bool isEditingRoute (const juce::String& path);
    /** Something the page shows changed outside the engine (the editing lock): it redraws. */
    void touch()                              { ++revision; }
    RemoteServer (Engine&, juce::PropertiesFile&);
    ~RemoteServer() override;

    bool start (int port);
    void stop();
    bool isRunning() const                 { return running.load(); }
    int getPort() const                    { return port; }
    /** The address to open on the tablet, or empty when stopped. The code is not
        in the URL: the page asks for it, so a shortcut survives a restart. */
    juce::String getUrl() const;

    /** The address a tablet should use to reach this machine, chosen over
        container and virtual-machine bridges. Public so tests can check it. */
    static juce::String getHostAddress();

    juce::String getToken() const          { return token; }
    /** The hub the front panel is read from and turned through (the page the keyboard's knobs are on); none while it is not there. */
    void setHub (SurfaceHub* h)            { hub = h; }


private:
    void run() override;
    /** `takeOver` is set when the connection has been handed to a relay
        thread, which then owns and closes it. */
    void handle (juce::StreamingSocket&, bool& takeOver);

    /** Relays a connection to a plugin window's local web bridge, so the whole
        feature travels on the one port the tablet already uses. Without this,
        every plugin window would need its own hole in the firewall. */
    bool relayToPluginView (juce::StreamingSocket&, const juce::String& firstChunk, int webPort);

    SurfaceHub* hub = nullptr;
    juce::String stateJson() const;
    /** The audio inputs' meters, for the tablet to poll while a page shows them. */
    juce::String levelsJson() const;
    /** The backing track of the song in play: whether there is one, running, where. */
    juce::String transportJson() const;
    juce::String panelJson (int input) const;      // the knobs of an input's current program
    juce::String controlsJson() const;             // what every pedal, knob and button does: the Controls panel, to read from the stand
    /** The armed song's MIDI-file parts, read once per file (by path and date). */
    struct FilePart { int channel = 1; juce::String name; int program = 0; };
    mutable std::map<juce::String, std::vector<FilePart>> partCache;
    const std::vector<FilePart>& partsOf (const juce::File&) const;
    juce::String slotsJson (int inputIndex) const;
    bool authorised (const juce::String& request) const;

    void programChanged (int, int) override
    {
        ++revision;
        /* The outgoing program's editor closes with it, so any bridge serving
           that window is now pointing at nothing. */
        PluginView::dropDeadSessions();
    }
    void setupChanged() override              { ++revision; }
    void programContentChanged (int, int) override { ++revision; }


    Engine& engine;
    juce::PropertiesFile& settings;
    std::unique_ptr<juce::StreamingSocket> listener;
    juce::String token;
    int port = 0;
    std::atomic<bool> running { false };
    std::atomic<int> revision { 0 };     // bumped on any change, so the page can poll cheaply

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RemoteServer)
};

} // namespace perf
