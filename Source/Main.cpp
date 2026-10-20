#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginHost.h"
#include "Engine.h"
#include "MainComponent.h"
#include "HelpWindow.h"
#include "LowLatency.h"
#include "BinaryData.h"
#include <cstdio>
#include <csignal>
#include <atomic>

namespace
{
    /* A termination signal becomes an ordinary quit. Without this, SIGTERM
       ends the process without running shutdown(), so the save that happens
       on quit never does -- and anything a plugin was told since the last
       autosave is gone. Only a flag is set here, because a signal handler may
       do almost nothing; the message thread notices it and quits properly. */
    std::atomic<bool> quitSignalled { false };
    void onQuitSignal (int) { quitSignalled.store (true); }
}

class StageRigApplication : public juce::JUCEApplication,
                             private juce::Timer
{
public:
    const juce::String getApplicationName() override       { return "StageRig"; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override             { return false; }

    void initialise (const juce::String& commandLine) override
    {
        /* A tablet that drops its connection while a reply is still being
           written would otherwise end the whole program: JUCE's socket write
           is a plain send(), and SIGPIPE's default is to terminate, silently,
           with no core. Treat it as the write error it is. */
       #ifdef SIGPIPE
        std::signal (SIGPIPE, SIG_IGN);
       #endif

        juce::PropertiesFile::Options opts;
        opts.applicationName = "StageRig";
        opts.filenameSuffix = "settings";
       #if JUCE_LINUX || JUCE_BSD
        opts.folderName = ".config/StageRig";      // JUCE would otherwise use ~/StageRig
        migrateOldSettingsFolder (opts);
        migrateFromPerformer (opts);
       #else
        opts.folderName = "StageRig";
       #endif
        opts.osxLibrarySubFolder = "Application Support";
        opts.storageFormat = juce::PropertiesFile::storeAsXML;
        settings = std::make_unique<juce::PropertiesFile> (opts);

        // Optional: `StageRig path/to/setup.stagerig.json` opens that setup instead of the last one.
        juce::File initialSetup;
        juce::ArgumentList args ("StageRig", commandLine);
        for (int i = 0; i < args.size(); ++i)
        {
            auto f = args[i].resolveAsFile();
            if (f.existsAsFile()) { initialSetup = f; break; }
        }

        // Low latency: PipeWire through its JACK API, at the quantum the user chose.
        const bool pwJack = perf::LowLatency::preloadPipeWireJack();
        const int quantum = settings->getIntValue ("pipewireQuantum", 128);
        perf::LowLatency::setRequestedQuantum (quantum, 48000);
        const bool takeover = settings->getBoolValue ("pipewireTakeover", true);
        if (takeover)
        {
            previousClock = perf::LowLatency::readClock();
            clockTakenOver = perf::LowLatency::forceClock (quantum, 48000);
        }
        std::fprintf (stderr, "[audio] pipewire-jack %s, requested quantum %d @ 48 kHz, clock takeover %s%s\n",
                      pwJack ? "loaded" : "not found", quantum, takeover ? (clockTakenOver ? "applied" : "FAILED") : "off",
                      takeover ? (" (was force-quantum '" + previousClock.forceQuantum + "')").toRawUTF8() : "");

        host   = std::make_unique<perf::PluginHost> (*settings);
        engine = std::make_unique<perf::Engine> (*host, *settings);
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *engine, *settings, initialSetup);

        std::signal (SIGTERM, onQuitSignal);
        std::signal (SIGINT,  onQuitSignal);
       #ifdef SIGHUP
        std::signal (SIGHUP,  onQuitSignal);
       #endif
        startTimer (250);
    }

    void timerCallback() override
    {
        if (quitSignalled.exchange (false))
            systemRequestedQuit();
    }

    /** The program was called Performer until October 2026 and kept its
        settings in ~/.config/Performer. The first run under the new name
        copies them -- settings, autosave, mapping templates, crash list,
        icon cache -- so nothing is set up twice. Copied rather than moved,
        so an older build still finds its own. */
    static void migrateFromPerformer (const juce::PropertiesFile::Options& opts)
    {
        auto newFile = opts.getDefaultFile();
        auto oldDir  = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".config/Performer");
        if (newFile.existsAsFile() || ! oldDir.isDirectory())
            return;

        auto newDir = newFile.getParentDirectory();
        newDir.createDirectory();
        const std::pair<const char*, juce::String> renamed[] = {
            { "Performer.settings",      newFile.getFileName() },
            { "autosave.performer.json", "autosave.stagerig.json" },
        };
        for (auto& [from, to] : renamed)
            if (auto src = oldDir.getChildFile (from); src.existsAsFile())
                src.copyFileTo (newDir.getChildFile (to));
        for (auto& name : { "mapping-templates.json", "RecentlyCrashedPluginsList" })
            if (auto src = oldDir.getChildFile (name); src.existsAsFile())
                src.copyFileTo (newDir.getChildFile (name));
        if (auto icons = oldDir.getChildFile ("icons"); icons.isDirectory())
            icons.copyDirectoryTo (newDir.getChildFile ("icons"));
    }

    /** Early builds stored settings in ~/StageRig; move them to ~/.config/StageRig once. */
    static void migrateOldSettingsFolder (const juce::PropertiesFile::Options& opts)
    {
        auto newFile = opts.getDefaultFile();
        auto oldDir  = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("StageRig");
        auto oldFile = oldDir.getChildFile (newFile.getFileName());
        if (newFile.existsAsFile() || ! oldFile.existsAsFile())
            return;

        newFile.getParentDirectory().createDirectory();
        for (auto& name : { newFile.getFileName(), juce::String ("autosave.stagerig.json"), juce::String ("RecentlyCrashedPluginsList") })
        {
            auto src = oldDir.getChildFile (name);
            if (src.existsAsFile())
                src.moveFileTo (newFile.getSiblingFile (name));
        }
        if (oldDir.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0)
            oldDir.deleteFile();
    }

    void shutdown() override
    {
        stopTimer();
        perf::HelpWindow::close();   // a help window must not outlive the app
        mainWindow.reset();     // closes plugin editors and autosaves
        engine.reset();
        host.reset();
        if (clockTakenOver) perf::LowLatency::restoreClock (previousClock);
        if (settings != nullptr) settings->saveIfNeeded();
        settings.reset();
    }

    void systemRequestedQuit() override { quit(); }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, perf::Engine& engine, juce::PropertiesFile& settings, const juce::File& initialSetup)
            : DocumentWindow (name, juce::Colour (0xff1e1f24), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            content = new perf::MainComponent (engine, settings, initialSetup);
            setContentOwned (content, true);
            setResizable (true, false);
            setResizeLimits (960, 600, 10000, 10000);
            centreWithSize (getWidth(), getHeight());

            /* The X11 window icon (_NET_WM_ICON, what the panel shows) has to be
               on the window BEFORE it is mapped. Cinnamon's window list reads
               a window's icon when the window appears and does not look again;
               set afterwards, as it was, the panel kept the generic gear it had
               found at map time. Adding to the desktop while still invisible
               creates the X window unmapped, so the icon goes on first and the
               map comes with it. DocumentWindow::setIcon only feeds JUCE's own
               title bar, so both are set. */
            const auto icon = juce::ImageCache::getFromMemory (BinaryData::stagerig256_png, BinaryData::stagerig256_pngSize);
            setIcon (icon);
            addToDesktop (getDesktopWindowStyleFlags());
            if (auto* peer = getPeer()) peer->setIcon (icon);
            setVisible (true);
        }

        ~MainWindow() override
        {
            if (content != nullptr) content->saveOnQuit();
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        perf::MainComponent* content = nullptr;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<juce::PropertiesFile> settings;
    perf::LowLatency::ClockState previousClock;
    bool clockTakenOver = false;
    std::unique_ptr<perf::PluginHost> host;
    std::unique_ptr<perf::Engine> engine;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (StageRigApplication)
