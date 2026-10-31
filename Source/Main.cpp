#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginHost.h"
#include "Engine.h"
#include "TempoDetect.h"
#include "MainComponent.h"
#include "HelpWindow.h"
#include "LowLatency.h"
#include "BinaryData.h"
#include <cstdio>
#include <csignal>
#include <atomic>
#if JUCE_LINUX || JUCE_BSD
 #include <sys/wait.h>
 #include <sys/prctl.h>
 #include <unistd.h>
 #include <ctime>
 #include <cerrno>
 #include <cstring>
 #include <algorithm>
 #include <string>
 #include <vector>
#endif

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
    /* One copy at a time: two would fight over the MIDI devices and the
       setup. A copy with its own settings folder (STAGERIG_SETTINGS_DIR) is
       a separate rig -- a demo, a review copy -- and may run beside it. */
    bool moreThanOneInstanceAllowed() override             { return std::getenv ("STAGERIG_SETTINGS_DIR") != nullptr; }

    void initialise (const juce::String& commandLine) override
    {
        /* A tablet that drops its connection while a reply is still being
           written would otherwise end the whole program: JUCE's socket write
           is a plain send(), and SIGPIPE's default is to terminate, silently,
           with no core. Treat it as the write error it is. */
        std::signal (SIGPIPE, SIG_IGN);

        // `StageRig --tempo song.mp3 ...` prints what the tempo detector hears in each file, then quits.
        if (commandLine.contains ("--tempo"))
        {
            juce::ArgumentList tempoArgs ("StageRig", commandLine);
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            for (int i = 0; i < tempoArgs.size(); ++i)
            {
                if (tempoArgs[i].isLongOption()) continue;
                const auto f = tempoArgs[i].resolveAsFile();
                std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
                if (reader == nullptr) { std::printf ("%s: could not be read\n", f.getFileName().toRawUTF8()); continue; }
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                const auto est = perf::detectTempo (*reader, 4);
                if (est.ok)
                    std::printf ("%s: %.2f bpm, first beat at %.3f s, confidence %.2f  (%.0f ms)\n",
                                 f.getFileName().toRawUTF8(), est.bpm, est.firstBeat, est.confidence, juce::Time::getMillisecondCounterHiRes() - t0);
                else
                    std::printf ("%s: %s\n", f.getFileName().toRawUTF8(), est.problem.toRawUTF8());
            }
            quit();
            return;
        }

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
        if (const char* dir = std::getenv ("STAGERIG_SETTINGS_DIR"); dir != nullptr && *dir != 0)
            opts.folderName = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (dir)).getFullPathName();
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
            /* The clock as it was before StageRig pinned it, kept in the
               settings until a clean quit puts it back. A copy started after
               a crash finds it still there and keeps it: reading the clock
               now would read StageRig's own setting, and the quit would then
               "restore" the pin for good. */
            if (settings->getBoolValue ("clockSaved", false))
                previousClock = { settings->getValue ("clockForceQuantum"), settings->getValue ("clockForceRate"),
                                  settings->getValue ("clockQuantum"), settings->getValue ("clockRate") };
            else
            {
                previousClock = perf::LowLatency::readClock();
                settings->setValue ("clockForceQuantum", previousClock.forceQuantum);
                settings->setValue ("clockForceRate", previousClock.forceRate);
                settings->setValue ("clockQuantum", previousClock.quantum);
                settings->setValue ("clockRate", previousClock.rate);
                settings->setValue ("clockSaved", true);
                settings->saveIfNeeded();
            }
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
        std::signal (SIGHUP,  onQuitSignal);
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
        if (settings != nullptr)
        {
            settings->removeValue ("clockSaved");
            settings->saveIfNeeded();
        }
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

#if JUCE_LINUX || JUCE_BSD
namespace
{
    /* The watchman. StageRig runs as a child of this small process, which
       does nothing but wait; if the child dies -- a crash, the kernel's
       out-of-memory killer, anything but a quit -- it is started again at
       once, and it comes back on the song, scene and programs it was on
       (MainComponent reads STAGERIG_RECOVERED). Every plugin already runs
       in its own process, so this is the one part left that could take the
       show down, and now it cannot take it down for long.

       Forked before JUCE starts, while the process has one thread. A quit
       -- the window closed, Ctrl+C, a SIGTERM from the desktop -- ends the
       child normally and the watchman with it. Five crashes inside two
       minutes is a rig that cannot start; it stops trying and says so. */
    volatile pid_t watchedChild = -1;
    volatile sig_atomic_t watchQuit = 0;

    void forwardToChild (int sig)
    {
        watchQuit = 1;
        if (watchedChild > 0) ::kill (watchedChild, sig);
    }

    /** -1 in the process that goes on to be StageRig; otherwise the exit code
        the watchman finishes with. */
    int superviseLaunch (int argc, char* argv[])
    {
        if (const char* off = std::getenv ("STAGERIG_NO_SUPERVISOR"); off != nullptr && *off != 0 && *off != '0') return -1;
        for (int i = 1; i < argc; ++i)
            if (std::strcmp (argv[i], "--tempo") == 0) return -1;      // a one-shot command, not a show

        std::signal (SIGTERM, forwardToChild);
        std::signal (SIGINT,  forwardToChild);
        std::signal (SIGHUP,  forwardToChild);

        std::vector<std::time_t> crashes;
        for (;;)
        {
            const pid_t pid = ::fork();
            if (pid < 0) return -1;                      // cannot watch: just be StageRig
            if (pid == 0)
            {
                ::prctl (PR_SET_NAME, "StageRig", 0, 0, 0);    // not the watchman's name, after a restart
                std::signal (SIGTERM, SIG_DFL);
                std::signal (SIGINT,  SIG_DFL);
                std::signal (SIGHUP,  SIG_DFL);
                return -1;
            }
            watchedChild = pid;
            ::prctl (PR_SET_NAME, "stagerig-watch", 0, 0, 0);

            int status = 0;
            while (::waitpid (pid, &status, 0) < 0 && errno == EINTR) {}
            watchedChild = -1;

            if (watchQuit) return WIFEXITED (status) ? WEXITSTATUS (status) : 0;
            if (WIFEXITED (status)) return WEXITSTATUS (status);       // it quit: so do we

            const int sig = WIFSIGNALED (status) ? WTERMSIG (status) : 0;
            const auto now = std::time (nullptr);
            crashes.erase (std::remove_if (crashes.begin(), crashes.end(), [now] (std::time_t t) { return now - t > 120; }), crashes.end());
            crashes.push_back (now);
            if (crashes.size() >= 5)
            {
                std::fprintf (stderr, "[watch] StageRig has crashed %d times in two minutes (last: %s); not starting it again.\n",
                              (int) crashes.size(), strsignal (sig));
                return 1;
            }
            std::fprintf (stderr, "[watch] StageRig stopped (%s); starting it again where it was.\n", strsignal (sig));
            ::setenv ("STAGERIG_RECOVERED", std::to_string (crashes.size()).c_str(), 1);
        }
    }
}

JUCE_CREATE_APPLICATION_DEFINE (StageRigApplication)

int main (int argc, char* argv[])
{
    if (const int code = superviseLaunch (argc, argv); code >= 0) return code;
    juce::JUCEApplicationBase::createInstance = &juce_CreateApplication;
    return juce::JUCEApplicationBase::main (argc, (const char**) argv);
}
#else
START_JUCE_APPLICATION (StageRigApplication)
#endif
