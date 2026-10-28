#include "PluginHost.h"
#include "RemotePlugin.h"
#include <cstdio>
#include <functional>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ClapPluginFormat.h"

extern char** environ;

using namespace juce;

namespace perf
{

struct PluginHost::ListListener : public ChangeListener
{
    explicit ListListener (PluginHost& h) : host (h) {}
    void changeListenerCallback (ChangeBroadcaster*) override { host.saveKnownPlugins(); }
    PluginHost& host;
};

PluginHost::PluginHost (PropertiesFile& s) : settings (s)
{
    addDefaultFormatsToManager (formatManager);
    // JUCE has no CLAP format; ours goes alongside its own.
    formatManager.addFormat (new ClapPluginFormat());

    deadMansPedal = settings.getFile().getSiblingFile ("RecentlyCrashedPluginsList");
    templates = std::make_unique<MappingTemplates> (settings.getFile().getSiblingFile ("mapping-templates.json"));
    icons = std::make_unique<PluginIcons> (settings.getFile().getSiblingFile ("icons"));

    if (auto xml = settings.getXmlValue ("pluginList"))
        knownPlugins.recreateFromXml (*xml);

    // Anything that crashed during a previous scan gets blacklisted.
    for (auto& line : StringArray::fromLines (deadMansPedal.loadFileAsString()))
        if (line.isNotEmpty())
            knownPlugins.addToBlacklist (line);

    listListener = std::make_unique<ListListener> (*this);
    knownPlugins.addChangeListener (listListener.get());
    knownPlugins.setCustomScanner (std::make_unique<OutOfProcessScanner>());
    scanner = std::make_unique<PluginScanner> (*this);
}

FileSearchPath PluginHost::getScanPaths (AudioPluginFormat& format) const
{
    auto paths = format.getDefaultLocationsToSearch();
    for (auto& p : StringArray::fromTokens (settings.getValue ("extraScanFolders"), "\n", {}))
        if (p.trim().isNotEmpty())
            paths.addIfNotAlreadyThere (File (p.trim()));
    return paths;
}

void PluginHost::addScanFolder (const File& folder)
{
    auto folders = StringArray::fromTokens (settings.getValue ("extraScanFolders"), "\n", {});
    folders.removeEmptyStrings();
    folders.addIfNotAlreadyThere (folder.getFullPathName());
    settings.setValue ("extraScanFolders", folders.joinIntoString ("\n"));
    settings.saveIfNeeded();
}

// Unlike juce::ChildProcess this never blocks inside a read: the scanner thread pool
// kills threads that don't stop, and a thread killed inside fread() aborts the whole
// application.
bool PluginHost::isWineBridged (const String& fileOrIdentifier)
{
    File f (fileOrIdentifier);
    if (! f.exists()) return false;
    if (f.getFullPathName().containsIgnoreCase ("yabridge")) return true;
    // A yabridge VST3 bundle contains a .so that is a symlink to libyabridge-vst3.so.
    for (const auto& so : f.findChildFiles (File::findFiles, true, "*.so"))
        if (so.isSymbolicLink() && so.getLinkedTarget().getFileName().containsIgnoreCase ("yabridge"))
            return true;
    return false;
}

String PluginHost::brokenBridgeTarget (const String& fileOrIdentifier)
{
    /* Only the plugin's own binary matters: yabridge mirrors a Windows bundle
       with symlinks, and a Resources link left dangling by an update that
       dropped the folder (Serum 2) must not hide a plugin that loads. */
    File f (fileOrIdentifier);
    if (! f.isDirectory()) return {};
    for (const auto& link : f.findChildFiles (File::findFiles, true))
    {
        if (! link.isSymbolicLink() || link.getLinkedTarget().exists()) continue;
        const auto target = link.getLinkedTarget();
        const bool binary = target.hasFileExtension ("vst3;dll;clap;so")
                            || target.getParentDirectory().getFileName().containsIgnoreCase ("x86_64-win")
                            || target.getParentDirectory().getFileName().containsIgnoreCase ("x86-win");
        if (binary) return target.getFullPathName();
        std::fprintf (stderr, "[scan] %s: ignoring dangling link %s (not the plugin's binary)\n",
                      fileOrIdentifier.toRawUTF8(), target.getFullPathName().toRawUTF8());
    }
    return {};
}

PluginHost::HelperResult PluginHost::runHelperWithTimeout (const File& exe, const StringArray& args, int timeoutMs,
                                                           int graceAfterStopMs, const std::function<bool()>& shouldStop,
                                                           String& output, const std::function<bool()>& hardStop,
                                                           const String& endMarker)
{
    {
        int fds[2];
        if (::pipe (fds) != 0) return HelperResult::timedOut;

        posix_spawn_file_actions_t actions;
        ::posix_spawn_file_actions_init (&actions);
        ::posix_spawn_file_actions_adddup2 (&actions, fds[1], STDOUT_FILENO);
        ::posix_spawn_file_actions_addclose (&actions, fds[0]);
        ::posix_spawn_file_actions_addclose (&actions, fds[1]);

        std::vector<std::string> storage { exe.getFullPathName().toStdString() };
        for (auto& a : args) storage.push_back (a.toStdString());
        std::vector<char*> argv;
        for (auto& a : storage) argv.push_back (const_cast<char*> (a.c_str()));
        argv.push_back (nullptr);

        std::vector<char*> envp;
        const auto envStorage = RemotePlugin::buildHelperEnvironment (envp);
        pid_t pid = -1;
        const int rc = ::posix_spawn (&pid, storage[0].c_str(), &actions, nullptr, argv.data(), envp.data());
        ::posix_spawn_file_actions_destroy (&actions);
        ::close (fds[1]);
        if (rc != 0) { ::close (fds[0]); return HelperResult::timedOut; }

        MemoryOutputStream collected;
        const auto start = Time::getMillisecondCounterHiRes();
        double stopRequestedAt = -1.0;
        auto result = HelperResult::finished;

        bool sawEndMarker = false;
        for (;;)
        {
            pollfd pfd { fds[0], POLLIN, 0 };
            const int pr = ::poll (&pfd, 1, 100);
            if (pr > 0)
            {
                char buf[8192];
                const auto n = ::read (fds[0], buf, sizeof (buf));
                if (n > 0)
                {
                    collected.write (buf, (size_t) n);
                    // The child signals completion explicitly: don't wait for EOF, which
                    // a grandchild holding the pipe open could delay indefinitely.
                    if (endMarker.isNotEmpty() && collected.toString().contains (endMarker)) { sawEndMarker = true; break; }
                    continue;
                }
                if (n == 0 || (errno != EINTR && errno != EAGAIN)) break;   // EOF: child closed stdout
            }

            const auto now = Time::getMillisecondCounterHiRes();
            if (stopRequestedAt < 0.0 && shouldStop())
                stopRequestedAt = now;

            if (hardStop && hardStop())                                       { result = HelperResult::stopped; break; }
            if (now - start > timeoutMs)                                      { result = HelperResult::timedOut; break; }
            if (stopRequestedAt >= 0.0 && now - stopRequestedAt > graceAfterStopMs) { result = HelperResult::stopped; break; }
        }
        ::close (fds[0]);

        if (result != HelperResult::finished)
            ::kill (pid, SIGKILL);

        // Reap; a child that finished its work but can't tear down gets a few seconds.
        int status = 0;
        if (sawEndMarker)
        {
            for (int i = 0; i < 100 && ::waitpid (pid, &status, WNOHANG) == 0; ++i)
                Thread::sleep (50);
            if (::waitpid (pid, &status, WNOHANG) == 0) { ::kill (pid, SIGKILL); ::waitpid (pid, &status, 0); }
        }
        else
        {
            ::waitpid (pid, &status, 0);
        }

        output = collected.toString();
        return result;
    }
}

bool PluginHost::OutOfProcessScanner::findPluginTypesFor (AudioPluginFormat& format, OwnedArray<PluginDescription>& result, const String& fileOrIdentifier)
{
    const auto exe = RemotePlugin::findHostExecutable();
    if (exe == File())
    {
        format.findAllTypesForFile (result, fileOrIdentifier);
        return true;
    }

    // A bridge whose Windows plugin has gone missing can't load; don't spend two Wine
    // start-ups finding that out.
    if (const auto missing = brokenBridgeTarget (fileOrIdentifier); missing.isNotEmpty())
    {
        std::fprintf (stderr, "[scan] %s: bridge target missing: %s\n", fileOrIdentifier.toRawUTF8(), missing.toRawUTF8());
        return true;
    }

    // Wine-bridged plugins are probed one at a time: concurrent Wine start-ups are what
    // makes them stall. Native plugins run in parallel.
    static std::mutex wineMutex;
    std::unique_lock<std::mutex> wineLock (wineMutex, std::defer_lock);
    const bool bridged = isWineBridged (fileOrIdentifier);
    if (bridged) wineLock.lock();

    // JUCE's scan dialog asks every still-running probe to exit the moment one thread
    // runs out of files, then waits up to a minute for them. Give slow (Wine-bridged)
    // plugins a real chance to finish in that window instead of treating it as failure.
    // A probe that hangs is retried once: Wine occasionally stalls and recovers.
    String output;
    const auto started = Time::getMillisecondCounterHiRes();
    auto res = HelperResult::timedOut;
    for (int attempt = 0; attempt < 2 && res == HelperResult::timedOut; ++attempt)
    {
        if (attempt > 0) Thread::sleep (2000);
        output.clear();
        res = runHelperWithTimeout (exe, StringArray { "--scan", format.getName(), fileOrIdentifier }, 60000, 45000,
                                    [this] { return shouldExit(); }, output,
                                    [] { return PluginScanner::hardStop.load(); }, "<SCAN-DONE>");
    }
    const auto elapsedMs = Time::getMillisecondCounterHiRes() - started;

    int parsed = 0;
    for (auto& line : StringArray::fromLines (output))
        if (line.contains ("<PLUGIN"))
            if (auto xml = parseXML (line))
            {
                auto d = std::make_unique<PluginDescription>();
                if (d->loadFromXml (*xml)) { result.add (d.release()); ++parsed; }
            }

    if (SystemStats::getEnvironmentVariable ("STAGERIG_SCAN_DEBUG", {}).isNotEmpty())
        std::fprintf (stderr, "[scan] %s %s: %s in %.1f s, %d bytes of output, %d descriptions\n",
                      format.getName().toRawUTF8(), fileOrIdentifier.toRawUTF8(),
                      res == HelperResult::finished ? "ok" : res == HelperResult::timedOut ? "TIMED OUT" : "stopped before finishing",
                      elapsedMs / 1000.0, (int) output.length(), parsed);

    // Only a plugin that hung for the whole timeout on its own is reported as failed
    // (and blacklisted). A probe that was asked to stop is simply not a result: the
    // plugin will be scanned again next time.
    return res != HelperResult::timedOut;
}

PluginHost::~PluginHost()
{
    scanner->shutdown();
    knownPlugins.removeChangeListener (listListener.get());
}

void PluginHost::saveKnownPlugins()
{
    if (auto xml = knownPlugins.createXml())
        settings.setValue ("pluginList", xml.get());
    settings.saveIfNeeded();
}

std::unique_ptr<AudioPluginInstance> PluginHost::createInstance (const PluginDescription& desc,
                                                                 double sampleRate, int blockSize,
                                                                 String& errorMessage)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto instance = formatManager.createPluginInstance (desc, sampleRate, blockSize, errorMessage);
    if (instance == nullptr && errorMessage.isEmpty())
        errorMessage = "Unknown error creating plugin " + desc.name;
    return instance;
}

} // namespace perf
