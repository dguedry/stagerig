#pragma once

#include "ControlMaps.h"
#include "Engine.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace perf
{

/** What a keyboard shows for the program in play: the program, the
    instrument its knobs turn and the page they are on, the knobs' names and
    values, and a colour for every key. One view serves every keyboard: each
    shows what it can of it -- a screen the words, a light guide the keys. */
struct SurfaceView
{
    static constexpr int numKnobs = 8;
    juce::String inputName;
    int program = -1;
    juce::String programName;          // its name, or "Program 007" when it has none
    juce::String instrument;           // the instrument the knobs turn ("Kontakt 8: Alicia's Keys"), empty for none
    juce::String pageName;
    int page = 0, pageCount = 0;
    std::array<juce::String, numKnobs> knobNames, knobValues;
    std::array<float, numKnobs> knobLevels;        // 0..1, or -1 for no parameter
    std::array<juce::uint32, 128> keyColours {};    // per note, 0xRRGGBB; 0 = dark

    /** The eight programs this one is among (bankFirst..bankFirst + 7), by
        name, empty for an empty program; whether there is a program with
        something in it before and after this one; and before and after
        these eight. */
    int bankFirst = 0;
    std::array<juce::String, numKnobs> bankNames;
    bool programBefore = false, programAfter = false, bankBefore = false, bankAfter = false;
    /** The program's plugins by name, and which of them the knobs turn (-1: none). */
    juce::StringArray pluginNames;
    int focusPlugin = -1;

    SurfaceView()                                   { knobLevels.fill (-1.0f); }

    bool sameText (const SurfaceView& o) const
    {
        return inputName == o.inputName && program == o.program && programName == o.programName && instrument == o.instrument
            && pageName == o.pageName && page == o.page && pageCount == o.pageCount && knobNames == o.knobNames && knobValues == o.knobValues
            && knobLevels == o.knobLevels && bankFirst == o.bankFirst && bankNames == o.bankNames && programBefore == o.programBefore
            && programAfter == o.programAfter && bankBefore == o.bankBefore && bankAfter == o.bankAfter && pluginNames == o.pluginNames && focusPlugin == o.focusPlugin;
    }
    bool operator== (const SurfaceView& o) const   { return sameText (o) && keyColours == o.keyColours; }
    bool operator!= (const SurfaceView& o) const   { return ! (*this == o); }
};

/** A keyboard StageRig talks to directly, beyond its notes: its knobs,
    buttons, screen and lights. Drivers are told what to show and hand over
    what was touched; the hub (below) decides what any of it means. All
    calls are on the message thread. */
class ControlSurface
{
public:
    virtual ~ControlSurface() = default;

    /** "Komplete Kontrol S88 MK1". */
    virtual juce::String getName() const = 0;
    /** Words a MIDI port's name has when it is this keyboard's keys: finds
        the input it belongs to. */
    virtual juce::String getKeysPortName() const = 0;
    virtual bool isConnected() const = 0;
    virtual bool hasScreen() const                 { return false; }
    virtual bool hasLights() const                 { return false; }

    virtual void show (const SurfaceView&) = 0;
    /** Something wrong with it, in words, while it is plugged in ("stopped answering"); empty when all is well. */
    virtual juce::String getProblem() const        { return {}; }

    struct Touch
    {
        /* program: one of the eight around the one in play (SurfaceView::bankFirst + index);
           instrument: the program's plugins counted in order; action: a SurfaceControls::Action. */
        enum Type { knob, knobTouch, button, program, instrument, action } type = knob;
        int index = 0;            // the knob, program, instrument or action
        float amount = 0.0f;      // knob: how far it turned, as a fraction of a parameter's range
        bool down = false;        // knobTouch, button
        juce::String id;          // button: "kk:b12", "lk:104", "ni:16"
    };
    /** What was touched since the last call, in order. */
    virtual void takeTouches (std::vector<Touch>& out) = 0;
};

/** Ties keyboards to the program in play. Each input has an instrument the
    knobs turn -- the program's first plugin, or the next one after a press
    of Next instrument -- and a page of eight knobs on it, from the
    instrument's controller setup (ControlMaps.h) or, without one, the front
    panel's knobs and then the plugin's other parameters eight at a time.

    The knobs are a keyboard's own (a Launchkey's encoders, a Komplete
    Kontrol's) or MIDI controllers taught on the input (SurfaceControls).
    Buttons taught there step through programs, pages and instruments. A
    keyboard with a screen shows the program and the knobs; one with lights
    shows each instrument's keys in its colour, and a setup's keyswitches
    and ranges in theirs.

    It runs on a timer on the message thread: knob turns are gathered and
    sent to the plugin at most 30 times a second, and keyboards are looked
    for every two seconds, so one plugged in during the show comes up by
    itself. */
class SurfaceHub : private Engine::Listener,
                   private juce::Timer
{
public:
    SurfaceHub (Engine&, ControlMapLibrary&, juce::PropertiesFile& settings, bool lookForKeyboards = true);
    ~SurfaceHub() override;

    // What the knobs turn ---------------------------------------------------------
    struct Knob { int paramIndex = -1; juce::String name; };
    struct Page { juce::String name; std::array<Knob, SurfaceView::numKnobs> knobs; };
    struct Layout
    {
        const ControlMap* map = nullptr;   // the setup in use, or null for the automatic pages
        std::vector<Page> pages;
    };
    /** The pages of one instrument, as they are now (the plugin must be running). */
    Layout layoutFor (int input, int program, int slot) const;
    /** The instrument of the input's program the knobs turn (-1 for none), and their page. */
    int getFocusSlot (int input);
    int getFocusPage (int input);
    SurfaceView viewFor (int input);

    /** A colour for each instrument of a program, in order, for its keys. */
    static juce::uint32 instrumentColour (int slot);

    // What the controls do (the keyboards, and tests) ------------------------------
    /** Turns a knob of the page in play by a fraction of the parameter's range. */
    void turnKnob (int input, int knob, float delta);
    /** Puts a knob's parameter at a value, 0..1. */
    void setKnob (int input, int knob, float value);
    /** One of SurfaceControls::Action. */
    void doAction (int input, int action);

    // Teaching ---------------------------------------------------------------------
    /** Waits for a control on the input: what = 0..7 for a knob (a MIDI
        controller), or 100 + a SurfaceControls::Action for a button (a
        controller, or a button of a keyboard StageRig talks to). `done` gets
        what was taught, in words. */
    void learn (int input, int what, std::function<void (const juce::String&)> done);
    void cancelLearn();
    bool isLearning() const                         { return learning.input >= 0; }
    /** "Controller 21", "Komplete Kontrol button 12", "Launchkey button 104". */
    static juce::String describeControl (const juce::String& id);

    // Keyboards StageRig talks to ----------------------------------------------------
    struct SurfaceInfo { juce::String name; int input = -1; bool screen = false, lights = false; };
    std::vector<SurfaceInfo> getSurfaces() const;
    /** Problems finding keyboards, in words (a device that could not be opened). */
    juce::StringArray getProblems() const;
    bool isKompleteKontrolEnabled() const;
    void setKompleteKontrolEnabled (bool);
    bool isLaunchkeyEnabled() const;
    void setLaunchkeyEnabled (bool);
    bool isKontrolDawEnabled() const;
    void setKontrolDawEnabled (bool);
    /** The input a keyboard works with: the one its keys play on, or the first keyboard input. */
    int inputFor (const ControlSurface&) const;

    /** The setups changed on disk: layouts are made again. */
    void libraryChanged()                           { layouts.clear(); }

    void addSurfaceForTesting (std::unique_ptr<ControlSurface>);
    /** What the timer does: gather the keyboards' touches, send the knobs' turns, update what they show. */
    void tick();

    /** A control was taught or a knob turned: the setup has changed. */
    std::function<void()> onEdited;
    /** A keyboard came or went. */
    std::function<void()> onSurfacesChanged;
    /** A keyboard that is plugged in went wrong, in words. */
    std::function<void (const juce::String&)> onSurfaceProblem;

private:
    void timerCallback() override                   { tick(); }
    void lookForKeyboards();
    void handleTouch (ControlSurface&, const ControlSurface::Touch&);
    void taught (const juce::String& controlId, int cc);
    const Layout& currentLayout (int input);
    juce::uint32 dim (juce::uint32 colour, float amount) const;

    // Engine::Listener
    void setupChanged() override                                    { layouts.clear(); focus.clear(); }
    void programChanged (int, int) override                         { layouts.clear(); }
    void programContentChanged (int, int) override                  { layouts.clear(); }
    void surfaceKnob (int input, int knob, int value, bool relative) override;
    void surfaceButton (int input, int action) override             { doAction (input, action); }
    void surfaceLearned (int input, int cc, int value) override;

    struct Focus { int program = -1; int slot = -1; int page = 0; };
    struct Pending { float delta = 0.0f; float absolute = -1.0f; };
    struct LastSet { int paramIndex = -1; float value = 0.0f; juce::uint32 at = 0; };
    struct Learning { int input = -1; int what = -1; std::function<void (const juce::String&)> done; };

    Engine& engine;
    ControlMapLibrary& library;
    juce::PropertiesFile& settings;
    const bool searching;
    std::vector<std::unique_ptr<ControlSurface>> surfaces;
    std::map<ControlSurface*, SurfaceView> shown;
    std::map<int, Focus> focus;
    std::map<int, Layout> layouts;
    std::map<std::pair<int, int>, Pending> pending;
    std::map<std::pair<int, int>, LastSet> lastSet;
    Learning learning;
    juce::StringArray problems;
    std::map<ControlSurface*, juce::String> reportedProblems;
    juce::uint32 lastSearch = 0;
    bool editedSinceTick = false;
};

//==============================================================================
/** Novation Launchkey MK4 (25/37/49/61 and Mini), through its DAW port, from
    Novation's Programmer's Reference Guide: DAW mode on, the encoders
    relative, and the screen showing the program and each knob's name and
    value. Buttons and pads report as "lk:<controller>" and "lk:n<note>",
    to be taught. */
class LaunchkeyMk4 : public ControlSurface, private juce::MidiInputCallback
{
public:
    /** The keyboards of this kind that are plugged in, by their DAW ports. */
    struct Ports { juce::MidiDeviceInfo in, out; juce::String keysName; bool mini = false; };
    static std::vector<Ports> find();

    explicit LaunchkeyMk4 (const Ports&);
    ~LaunchkeyMk4() override;
    bool opened() const                             { return input != nullptr && output != nullptr; }
    const juce::String& getOutputIdentifier() const { return ports.out.identifier; }

    juce::String getName() const override           { return ports.mini ? "Launchkey Mini MK4" : "Launchkey MK4"; }
    juce::String getKeysPortName() const override   { return ports.keysName; }
    bool isConnected() const override;
    bool hasScreen() const override                 { return true; }
    void show (const SurfaceView&) override;
    void takeTouches (std::vector<Touch>& out) override;

    /** The messages that bring the keyboard into DAW mode, and out of it. */
    static std::vector<juce::MidiMessage> openingMessages (bool mini);
    static std::vector<juce::MidiMessage> closingMessages();
    /** The SysEx that sets the screen to `now`, given it showed `before` (nullptr: nothing yet). */
    static std::vector<juce::MidiMessage> screenMessages (const SurfaceView& now, const SurfaceView* before, bool mini);
    /** What a message from the DAW port means, if anything. */
    static bool parse (const juce::MidiMessage&, Touch& out);

private:
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    void send (const std::vector<juce::MidiMessage>&);

    Ports ports;
    std::unique_ptr<juce::MidiInput> input;
    std::unique_ptr<juce::MidiOutput> output;
    std::mutex touchMutex;
    std::vector<Touch> touches;
    SurfaceView last;
    bool shownOnce = false;
};

//==============================================================================
/** Native Instruments' keyboards that speak its DAW protocol -- Komplete
    Kontrol S MK2 and MK3, A- and M-series -- through their DAW port, as
    REAPER's ReaKontrol does. StageRig is the DAW, and a program is a track:
    the eight programs around the one in play are named across the screen,
    the track buttons step through them and pressing one plays it, and the
    knobs turn the instrument's knobs, named and valued in the plugin view.
    The MK3 has its DAW port itself; an MK2's, A's and M's is made by NI's
    Host Integration on Windows and macOS. Other buttons report as
    "ni:<command>", to be taught. */
class KompleteKontrolDaw : public ControlSurface, private juce::MidiInputCallback
{
public:
    struct Ports { juce::MidiDeviceInfo in, out; juce::String name, keysName; };
    static std::vector<Ports> find();
    /** "Komplete Kontrol S61 MK3" for a port's name, or empty when it is no DAW port of these. */
    static juce::String modelFor (const juce::String& portName, juce::String& keysName);

    explicit KompleteKontrolDaw (const Ports&);
    ~KompleteKontrolDaw() override;
    bool opened() const                             { return input != nullptr && output != nullptr; }
    const juce::String& getOutputIdentifier() const { return ports.out.identifier; }
    /** What the keyboard answered the hello with: 4 for an S MK3; 0 before it answers. */
    int getProtocolVersion() const                  { return version.load(); }
    bool areBanksForPrograms() const                { return banksForPrograms.load(); }

    juce::String getName() const override           { return ports.name; }
    juce::String getKeysPortName() const override   { return ports.keysName; }
    bool isConnected() const override;
    bool hasScreen() const override                 { return true; }
    void show (const SurfaceView&) override;
    void takeTouches (std::vector<Touch>& out) override;

    static juce::MidiMessage hello();
    static juce::MidiMessage goodbye();
    /** What the keyboard has told StageRig about itself: which version of the
        protocol it speaks (4 for an S MK3; 0 before it answers the hello), and
        what its bank arrows are for, from the view on its screen -- the
        programs eight at a time in the mixer view, as tracks are in a DAW,
        and the knob pages in the plugin view. */
    struct Heard { int version = 0; bool banksForPrograms = true; };

    /** What tells the keyboard the screen is `now`, given it showed `before` (nullptr: nothing yet). */
    static std::vector<juce::MidiMessage> screenMessages (const SurfaceView& now, const SurfaceView* before, const Heard&);
    /** What a message from the DAW port means. The hello's answer and the view's change set `heard` and are no touch. */
    static bool parse (const juce::MidiMessage&, Touch& out, Heard& heard);

private:
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    void send (const std::vector<juce::MidiMessage>&);

    Ports ports;
    std::unique_ptr<juce::MidiInput> input;
    std::unique_ptr<juce::MidiOutput> output;
    std::atomic<int> version { 0 };
    std::atomic<bool> banksForPrograms { true };
    Heard shownFor;
    std::mutex touchMutex;
    std::vector<Touch> touches;
    SurfaceView last;
    bool haveView = false, shownOnce = false;
};

//==============================================================================
/** Native Instruments Komplete Kontrol S-series MK1 (S25/49/61/88), over its
    USB HID interface (Linux): the light guide, and the eight knobs and the
    buttons, which report as "kk:b<n>" to be taught. Its keys are ordinary
    MIDI and need none of this. Worked out from the device, not from a
    published protocol, so it is off until turned on. */
class KompleteKontrolMk1 : public ControlSurface
{
public:
    struct Model { int productId; const char* name; int keys; int lowestNote; };
    static const Model* modelFor (int productId);
    struct Device { juce::String path; const Model* model = nullptr; };
    static std::vector<Device> find();

    /** `error` gets the reason when it cannot be opened. */
    KompleteKontrolMk1 (const Device&, juce::String& error);
    ~KompleteKontrolMk1() override;
    bool opened() const                             { return fd >= 0; }
    const juce::String& getPath() const             { return device.path; }

    juce::String getName() const override           { return juce::String ("Komplete Kontrol ") + device.model->name + " MK1"; }
    juce::String getKeysPortName() const override   { return juce::String ("KOMPLETE KONTROL ") + device.model->name; }
    bool isConnected() const override               { return fd >= 0 && ! gone.load(); }
    bool hasLights() const override                 { return true; }
    void show (const SurfaceView&) override;
    void takeTouches (std::vector<Touch>& out) override;
    juce::String getProblem() const override;

    /** The light guide's report for these key colours. */
    static std::vector<juce::uint8> lightReport (const std::array<juce::uint32, 128>& keyColours, const Model&);
    /** What changed between two of the keyboard's control reports (report id 1, without the id). */
    static void compareReports (const juce::uint8* before, const juce::uint8* now, int size, std::vector<Touch>& out);

private:
    void read();
    void write();

    Device device;
    int fd = -1;
    std::atomic<bool> gone { false }, stopping { false };
    /** A write failed: the keyboard is plugged in but its control part no
        longer answers. Nothing more is sent until it is plugged in again. */
    std::atomic<bool> stalled { false };
    std::thread reader, writer;
    std::mutex touchMutex;
    std::vector<Touch> touches;

    /* Writing waits for the keyboard (the kernel gives up after five
       seconds), so it is done on a thread of its own, never the message
       thread: only the newest light report is kept, and one the keyboard
       has not taken yet is replaced. */
    std::mutex writeMutex;
    std::condition_variable writeWake;
    std::vector<juce::uint8> nextLights;
    bool lightsWaiting = false;
    std::array<juce::uint32, 128> lit {};
    bool litOnce = false;
};

} // namespace perf
