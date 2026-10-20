/*
    stagerig-plugin-host: hosts exactly one plugin for StageRig in a separate
    process.

        stagerig-plugin-host --serve <shm-name> <socket-fd>
        stagerig-plugin-host --scan  <format-name> <file-or-identifier>

    See Source/Ipc/Protocol.h for the wire protocol.
*/

// For setWindowAbove: JUCE's X11 wrappers are only exposed on request.
#if JUCE_LINUX || defined (__linux__)
 #define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1
#endif
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "../Ipc/Protocol.h"
#include "BinaryData.h"
#if JUCE_PLUGINHOST_VST3
 // Headers only: the IIDs are defined inside JUCE's own VST3 host translation unit.
 #define JUCE_VST3HEADERS_INCLUDE_HEADERS_ONLY 1
 #include <juce_audio_processors_headless/format_types/juce_VST3Headers.h>
#endif
#include <iostream>
#include <mutex>

namespace
{
    /* Add or remove _NET_WM_STATE_ABOVE on a window that already exists, by
       the EWMH client message a window manager expects for a mapped window.

       Not Component::setAlwaysOnTop: on X11 JUCE cannot change that in place,
       so it destroys the X window and creates a new one with the hint set at
       birth. For an editor being mirrored that is the one thing that must not
       happen -- x11vnc is bound to the window id, and the plugin's own GUI is
       parented into that X window. */
    void setWindowAbove (juce::ComponentPeer* peer, bool above)
    {
       #if JUCE_LINUX
        if (peer == nullptr) return;
        auto* xws = juce::XWindowSystem::getInstance();
        auto* display = xws->getDisplay();
        if (display == nullptr) return;

        auto* sym = juce::X11Symbols::getInstance();
        const auto aboveAtom = juce::XWindowSystemUtilities::Atoms::getCreating (display, "_NET_WM_STATE_ABOVE");
        const auto root = sym->xRootWindow (display, sym->xDefaultScreen (display));

        XClientMessageEvent m {};
        m.type         = ClientMessage;
        m.display      = display;
        m.window       = (::Window) peer->getNativeHandle();
        m.message_type = xws->getAtoms().windowState;
        m.format       = 32;
        m.data.l[0]    = above ? 1 : 0;        // _NET_WM_STATE_ADD / _NET_WM_STATE_REMOVE
        m.data.l[1]    = (long) aboveAtom;
        m.data.l[2]    = 0;
        m.data.l[3]    = 1;                    // source: a normal application

        juce::XWindowSystemUtilities::ScopedXLock lock;
        sym->xSendEvent (display, root, false, SubstructureRedirectMask | SubstructureNotifyMask, (XEvent*) &m);
        sym->xFlush (display);
       #else
        juce::ignoreUnused (peer, above);
       #endif
    }
}
#include <map>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdio>
#if ! JUCE_WINDOWS
 #include <signal.h>
 #include <unistd.h>
 #include <fcntl.h>
 #include <dirent.h>
 #include <sys/types.h>
#endif
#include "ClapPluginFormat.h"

using namespace juce;
using namespace perf;

/** Loading a plugin is the slowest thing this process does, so the timings that
    went into tuning it are worth keeping -- but not on every load. Set
    STAGERIG_TIME_PARAMS=1 to see them. */
static bool timeParams()
{
    static const bool on = [] { auto* v = std::getenv ("STAGERIG_TIME_PARAMS"); return v != nullptr && *v != '0'; }();
    return on;
}

//==============================================================================
class PluginServer : private AudioProcessorListener
{
public:
    PluginServer (ipc::SharedBlock* block, int fd) : shm (block), socketFd (fd)
    {
        addDefaultFormatsToManager (formatManager);
    // JUCE has no CLAP format; ours goes alongside its own.
    formatManager.addFormat (new ClapPluginFormat());
        control = std::thread ([this] { controlLoop(); });
    }

    ~PluginServer()
    {
        /* Whether the plugin ever asked for tempo, and what it last got. Worth
           keeping: "my delay is not syncing" is otherwise impossible to tell
           apart from "this plugin never asks". STAGERIG_REPORT_TEMPO=1. */
        if (playHead != nullptr && std::getenv ("STAGERIG_REPORT_TEMPO") != nullptr)
            std::fprintf (stderr, "[tempo] plugin asked %d times, last bpm %.2f\n",
                          playHead->asked.load(), playHead->lastBpm.load());
        stopAudioThread();
        // Bridged plugins (yabridge) throw if their other half is already gone;
        // nothing here may escape, we're on our way out anyway.
        try { editorWindow.reset(); } catch (...) {}
        if (instance != nullptr)
        {
            try { instance->removeListener (this); } catch (...) {}
            try { instance->releaseResources(); } catch (...) {}
            try { instance.reset(); } catch (...) {}
        }
        if (control.joinable()) control.join();
    }

    std::function<void()> onQuit;

private:
    //==============================================================================
    // Control channel: one thread reads requests and runs them on the message thread.
    void controlLoop()
    {
        ipc::FrameHeader header;
        MemoryBlock payload;
        while (ipc::readFrame (socketFd, header, payload))
        {
            struct Ctx { PluginServer* self; const ipc::FrameHeader* h; const MemoryBlock* p; MemoryBlock reply; };
            Ctx ctx { this, &header, &payload, {} };
            MessageManager::getInstance()->callFunctionOnMessageThread ([] (void* c) -> void*
            {
                auto* x = static_cast<Ctx*> (c);
                try                          { x->reply = x->self->handleRequest ((ipc::Msg) x->h->type, *x->p); }
                catch (const std::exception& e) { x->reply = fail (String ("plugin threw: ") + e.what()); }
                catch (...)                  { x->reply = fail ("plugin threw an unknown exception"); }
                return nullptr;
            }, &ctx);

            {
                const std::lock_guard<std::mutex> l (sendMutex);
                ipc::sendFrame (socketFd, ipc::Msg::response, header.requestId, ctx.reply);
            }

            if ((ipc::Msg) header.type == ipc::Msg::quit)
                break;
        }
        // Host gone or told us to quit.
        MessageManager::callAsync ([this] { if (onQuit) onQuit(); });
    }

    static MemoryBlock ok (const MemoryOutputStream& data = MemoryOutputStream())
    {
        MemoryOutputStream out;
        out.writeBool (true);
        out.write (data.getData(), data.getDataSize());
        return out.getMemoryBlock();
    }

    static MemoryBlock fail (const String& why)
    {
        MemoryOutputStream out;
        out.writeBool (false);
        out.writeString (why);
        return out.getMemoryBlock();
    }

    MemoryBlock handleRequest (ipc::Msg type, const MemoryBlock& payload)
    {
        MemoryInputStream in (payload, false);
        switch (type)
        {
            case ipc::Msg::load:
            {
                const auto xmlText = in.readString();
                const double sr = in.readDouble();
                const int bs = in.readInt();
                PluginDescription desc;
                auto xml = parseXML (xmlText);
                if (xml == nullptr || ! desc.loadFromXml (*xml)) return fail ("bad plugin description");
                String error;
                auto inst = formatManager.createPluginInstance (desc, sr, bs, error);
                if (inst == nullptr) return fail (error.isEmpty() ? "could not create plugin" : error);

                inst->enableAllBuses();
                inst->setNonRealtime (false);
                playHead = std::make_unique<HostPlayHead> (shm);
                playHead->sampleRate = sr;
                inst->setPlayHead (playHead.get());
                inst->prepareToPlay (sr, bs);
                inst->addListener (this);
                sampleRate = sr; blockSize = bs;
                {
                    const std::lock_guard<std::mutex> l (processMutex);
                    instance = std::move (inst);
                    const int ch = jmax (2, instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels());
                    scratch.setSize (ch, ipc::kMaxBlock);
                    midi.ensureSize (4096);
                }
                startAudioThread();
                return ok();
            }

            case ipc::Msg::prepare:
            {
                if (instance == nullptr) return fail ("no plugin");
                const double sr = in.readDouble();
                const int bs = in.readInt();
                const std::lock_guard<std::mutex> l (processMutex);
                instance->releaseResources();
                instance->prepareToPlay (sr, bs);
                sampleRate = sr; blockSize = bs;
                return ok();
            }

            case ipc::Msg::getInfo:
            {
                if (instance == nullptr) return fail ("no plugin");
                MemoryOutputStream out;
                out.writeString (instance->getName());
                out.writeBool (instance->acceptsMidi() && instance->getMainBusNumInputChannels() == 0);
                out.writeBool (instance->hasEditor());
                out.writeInt (instance->getLatencySamples());
                return ok (out);
            }

            case ipc::Msg::getParameters:
            {
                if (instance == nullptr) return fail ("no plugin");
                MemoryOutputStream out;
                auto& params = instance->getParameters();
                const auto tAll = juce::Time::getMillisecondCounterHiRes();
                const auto midi = queryMidiAssignments (*instance);
                const auto tVals = juce::Time::getMillisecondCounterHiRes();
                out.writeInt (params.size());
                for (auto* p : params)
                {
                    out.writeInt (p->getParameterIndex());
                    out.writeString (parameterId (*p));
                    out.writeString (p->getName (128));
                    out.writeBool (p->isAutomatable());
                    out.writeBool (p->isDiscrete());
                    out.writeBool (p->isBoolean());
                    // How many positions the parameter has, when it says. A
                    // two-step parameter is a switch whatever isBoolean claims:
                    // B-3X reports every one of its drawbars and switches as
                    // continuous and non-boolean.
                    out.writeInt (p->getNumSteps());
                    out.writeFloat (p->getValue());
                    const auto& a = midi[(size_t) p->getParameterIndex()];
                    out.writeInt (a.channel);
                    out.writeInt (a.controller);
                }
                if (timeParams())
                    std::fprintf (stderr, "[params] %d parameters: midi probe %.0f ms, values+names %.0f ms\n",
                                  params.size(), tVals - tAll, juce::Time::getMillisecondCounterHiRes() - tVals);
                return ok (out);
            }

            case ipc::Msg::getParameterValue:
            {
                auto* p = paramAt (in.readInt());
                if (p == nullptr) return fail ("bad parameter");
                MemoryOutputStream out;
                out.writeFloat (p->getValue());
                return ok (out);
            }

            case ipc::Msg::setParameterValue:
            {
                auto* p = paramAt (in.readInt());
                const float v = in.readFloat();
                if (p == nullptr) return fail ("bad parameter");
                suppressNotifications = true;
                p->setValueNotifyingHost (jlimit (0.0f, 1.0f, v));
                suppressNotifications = false;
                return ok();
            }

            case ipc::Msg::getState:
            {
                if (instance == nullptr) return fail ("no plugin");
                MemoryBlock state;
                instance->getStateInformation (state);
                MemoryOutputStream out;
                out.write (state.getData(), state.getSize());
                return ok (out);
            }

            case ipc::Msg::setState:
            {
                if (instance == nullptr) return fail ("no plugin");
                suppressNotifications = true;
                {
                    const std::lock_guard<std::mutex> l (processMutex);
                    instance->setStateInformation (payload.getData(), (int) payload.getSize());
                }
                suppressNotifications = false;
                return ok();
            }

            case ipc::Msg::showEditor:
            {
                if (instance == nullptr) return fail ("no plugin");
                const auto title = in.readString();
                /* Already built: show it again rather than building it afresh.
                   Making the editor is the expensive part of opening a GUI --
                   1.9 s of the 2.5 s for Hammond B-3X, measured -- and it is
                   the same editor either way. */
                if (editorWindow != nullptr)
                {
                    editorWindow->setVisible (true);
                    editorWindow->toFront (true);
                    return ok();
                }
                editorWindow = std::make_unique<EditorWindow> (*this, title);
                {
                    MemoryOutputStream log;
                    log.writeString ("editor window " + editorWindow->getBounds().toString()
                                     + " visible=" + String ((int) editorWindow->isVisible())
                                     + " onDesktop=" + String ((int) editorWindow->isOnDesktop())
                                     + " peer=" + String ((int) (editorWindow->getPeer() != nullptr))
                                     + " messageThread=" + String ((int) MessageManager::getInstance()->isThisTheMessageThread()));
                    notify (ipc::Msg::notifyLog, log);
                }
                return ok();
            }

            case ipc::Msg::keepEditorOnTop:
            {
                /* A tablet drives this window through x11vnc, which delivers
                   each tap by moving the real pointer onto the window and
                   clicking. That only reaches the plugin if nothing on this
                   desktop covers it. With a browser on the same machine it is
                   worse than a lost tap: the browser sees the moved pointer as
                   a mouse move, sends it to the plugin again, and the cursor
                   runs away to the edge of the canvas. So while the window is
                   being mirrored it stays above everything else. */
                const bool onTop = in.readBool();
                if (editorWindow != nullptr) setWindowAbove (editorWindow->getPeer(), onTop);
                return ok();
            }

            case ipc::Msg::hideEditor:
                /* Hidden, not destroyed: destroying it takes the plugin's editor
                   with it (the window owns it), so the next open would pay the
                   whole cost again. A hidden window costs some memory and no
                   CPU -- plugins stop drawing when they are not visible. */
                if (editorWindow != nullptr)
                {
                    setWindowAbove (editorWindow->getPeer(), false);   // nothing is mirroring a hidden window
                    editorWindow->setVisible (false);
                }
                return ok();

            case ipc::Msg::quit:
                stopAudioThread();
                return ok();

            default:
                return fail ("unknown request");
        }
    }

    AudioProcessorParameter* paramAt (int index) const
    {
        if (instance == nullptr) return nullptr;
        auto& params = instance->getParameters();
        return (index >= 0 && index < params.size()) ? params[index] : nullptr;
    }

    static String parameterId (const AudioProcessorParameter& p)
    {
        if (auto* hosted = dynamic_cast<const HostedAudioProcessorParameter*> (&p))
            return hosted->getParameterID();
        return String (p.getParameterIndex());
    }

    /** Which (MIDI channel, controller) a parameter stands for, or 0 / -1. */
    struct MidiAssignment { int channel = 0; int controller = -1; };

    /** VST3 plugins receive no MIDI controllers; they publish one parameter per
        (channel, controller) and the host converts, which is why Kontakt shows
        16 copies of "Channel Volume(MSB)". IMidiMapping tells us which copy is
        which channel, so the UI can label them. Empty assignments when the
        plugin does not expose the interface (LV2, LADSPA, some native VST3s
        whose controller is a separate object). */
    static std::vector<MidiAssignment> queryMidiAssignments (AudioPluginInstance& inst)
    {
        std::vector<MidiAssignment> result ((size_t) inst.getParameters().size());
       #if JUCE_PLUGINHOST_VST3
        auto* client = inst.getVST3Client();
        auto* component = client != nullptr ? client->getIComponentPtr() : nullptr;
        if (component == nullptr) return result;

        Steinberg::FUnknownPtr<Steinberg::Vst::IMidiMapping> mapping (component);
        if (mapping == nullptr)
        {
            // Single-object plugins answer for the controller through the component.
            Steinberg::FUnknownPtr<Steinberg::Vst::IEditController> controller (component);
            if (controller != nullptr)
                mapping = Steinberg::FUnknownPtr<Steinberg::Vst::IMidiMapping> (controller.get());
        }
        if (mapping == nullptr)
        {
            std::fprintf (stderr, "[params] plugin exposes no IMidiMapping through its component; controller copies stay unlabelled\n");
            return result;
        }

        const auto tMap0 = juce::Time::getMillisecondCounterHiRes();
        std::map<String, int> indexById;
        for (auto* p : inst.getParameters())
            indexById[parameterId (*p)] = p->getParameterIndex();

        int found = 0;
        for (int ch = 0; ch < 16; ++ch)
            for (int cc = 0; cc <= (int) Steinberg::Vst::kCtrlProgramChange; ++cc)
            {
                Steinberg::Vst::ParamID id = 0;
                if (mapping->getMidiControllerAssignment (0, (Steinberg::int16) ch, (Steinberg::Vst::CtrlNumber) cc, id) != Steinberg::kResultTrue)
                    continue;
                const auto it = indexById.find (String ((uint32) id));
                if (it == indexById.end() || it->second < 0 || it->second >= (int) result.size()) continue;
                auto& a = result[(size_t) it->second];
                if (a.channel != 0) continue;       // first assignment wins
                a.channel = ch + 1;
                a.controller = cc;
                ++found;
            }
        if (timeParams())
            std::fprintf (stderr, "[params] %d parameters are MIDI controller proxies (probe took %.0f ms)\n", found, juce::Time::getMillisecondCounterHiRes() - tMap0);
       #endif
        return result;
    }

    void notify (ipc::Msg type, const MemoryOutputStream& data)
    {
        const std::lock_guard<std::mutex> l (sendMutex);
        ipc::sendFrame (socketFd, type, 0, data.getMemoryBlock());
    }

    // AudioProcessorListener --------------------------------------------------------
    void audioProcessorParameterChanged (AudioProcessor*, int index, float value) override
    {
        /* Report this unless we caused it ourselves, or we are on the audio
           thread -- notify() takes a lock and writes to a socket, which must
           never happen mid-block.

           This used to require the MESSAGE thread, which was too strict, and is
           why a plugin changing its own program left the tablet showing stale
           drawbars: Hammond B-3X reports a program change from one of its own
           worker threads, so every one of those updates was thrown away. */
        const bool suppressed = suppressNotifications.load (std::memory_order_relaxed);
        const bool onAudio = Thread::getCurrentThreadId() == audioThreadId.load (std::memory_order_relaxed);

        /* Log every one, including the ones we drop, with the parameter's name:
           "does this plugin report its own edits" cannot be answered without
           seeing what it reports and what we discard. */
        if (std::getenv ("STAGERIG_REPORT_PARAMS") != nullptr)
        {
            String pname;
            if (instance != nullptr)
                if (auto* p = paramAt (index)) pname = p->getName (64);
            std::fprintf (stderr, "[param] %d '%s' = %.3f  suppressed=%d audioThread=%d\n",
                          index, pname.toRawUTF8(), value, (int) suppressed, (int) onAudio);
        }

        if (suppressed || onAudio) return;

        MemoryOutputStream out; out.writeInt (index); out.writeFloat (value);
        notify (ipc::Msg::notifyParamChanged, out);
    }
    void audioProcessorParameterChangeGestureBegin (AudioProcessor*, int index) override
    {
        /* Logged because "my plugin's own edits do not reach the tablet" is
           otherwise indistinguishable from "this plugin never tells the host".
           STAGERIG_REPORT_PARAMS=1. */
        if (std::getenv ("STAGERIG_REPORT_PARAMS") != nullptr)
            std::fprintf (stderr, "[gesture] begin on param %d\n", index);
        if (! MessageManager::getInstance()->isThisTheMessageThread()) return;
        auto* p = paramAt (index);
        MemoryOutputStream out; out.writeInt (index); out.writeFloat (p != nullptr ? p->getValue() : 0.0f);
        notify (ipc::Msg::notifyParamTouched, out);
    }
    void audioProcessorChanged (AudioProcessor*, const ChangeDetails& d) override
    {
        if (std::getenv ("STAGERIG_REPORT_PARAMS") != nullptr)
            std::fprintf (stderr, "[changed] params=%d program=%d latency=%d\n",
                          (int) d.parameterInfoChanged, (int) d.programChanged, (int) d.latencyChanged);
    }

    //==============================================================================
    // Editor window
    struct EditorWindow : public DocumentWindow
    {
        EditorWindow (PluginServer& s, const String& title)
            : DocumentWindow (title, Colours::darkgrey, DocumentWindow::closeButton), server (s)
        {
            setUsingNativeTitleBar (true);
            AudioProcessorEditor* editor = server.instance->hasEditor() ? server.instance->createEditorIfNeeded() : nullptr;
            if (editor != nullptr && (editor->getWidth() < 10 || editor->getHeight() < 10)) { delete editor; editor = nullptr; }
            if (editor == nullptr)
            {
                auto* generic = new GenericAudioProcessorEditor (*server.instance);
                generic->setSize (jmax (400, generic->getWidth()), jmax (300, generic->getHeight()));
                editor = generic;
            }
            setContentOwned (editor, true);
            setResizable (editor->isResizable(), false);
            setResizeLimits (200, 100, 8192, 8192);
            centreWithSize (getWidth(), getHeight());
            // Icon before the map, for the same reason as the main window: the
            // panel reads it once, when the window appears.
            addToDesktop (getDesktopWindowStyleFlags());
            if (auto* peer = getPeer()) peer->setIcon (ImageCache::getFromMemory (BinaryData::stagerig256_png, BinaryData::stagerig256_pngSize));
            setVisible (true);

            // yabridge (Wine-bridged plugins) only learns where the embedded window is on
            // screen when the window the plugin's editor was attached to -- our content
            // component's X11 window -- is resized after the attach. A plugin whose editor
            // reports its final size straight away (IK Multimedia B-3X) never causes one,
            // so Wine keeps the window at (0,0) and clicks land offset by the window's
            // position until the user drags it. Resizing the *frame* is not enough: with a
            // native title bar the window manager reparents us, so those events reach the
            // frame rather than the editor's parent and yabridge reads a zero size from
            // them. Resize the editor component itself, which is that parent.
            MessageManager::callAsync ([sp = Component::SafePointer<EditorWindow> (this)]
            {
                if (sp == nullptr) return;
                auto* content = sp->getContentComponent();
                if (content == nullptr) return;
                const auto b = content->getBounds();
                content->setBounds (b.withHeight (b.getHeight() + 1));
                content->setBounds (b);

                // Report where the editor ended up, so a mouse-offset report can be
                // checked against the log instead of guessed at.
                if (auto* peer = sp->getPeer())
                {
                    const auto screen = peer->getBounds();
                    MemoryOutputStream log;
                    log.writeString ("editor mapped at " + screen.toString()
                                     + " (content " + b.toString() + ")");
                    sp->server.notify (ipc::Msg::notifyLog, log);
                }
            });
        }

        void closeButtonPressed() override
        {
            /* Hidden rather than deleted, so reopening is instant. StageRig is
               told it closed either way: as far as the rest of the app is
               concerned the editor is shut. */
            server.notify (ipc::Msg::notifyEditorClosed, MemoryOutputStream());
            MessageManager::callAsync ([&s = server]
            {
                if (s.editorWindow != nullptr) s.editorWindow->setVisible (false);
            });
        }

        bool keyPressed (const KeyPress& key) override
        {
            if (key == KeyPress::escapeKey) { closeButtonPressed(); return true; }
            return DocumentWindow::keyPressed (key);
        }

        PluginServer& server;
    };

    //==============================================================================
    /** Tells the plugin the tempo the host is keeping.

        StageRig has no transport: there is no timeline on stage, nothing to
        start or stop. But a plugin that syncs a delay or an arpeggiator asks the
        host for tempo, and with no playhead at all it either guesses 120 or does
        nothing. So this reports a position that advances continuously and a
        transport that is always playing, which is what a live rig looks like
        from the plugin's point of view.

        Read straight from shared memory: the host rewrites these before each
        block, and a torn read of a double would at worst be one block of a wrong
        tempo, not a crash. */
    struct HostPlayHead : public AudioPlayHead
    {
        explicit HostPlayHead (ipc::SharedBlock* s) : shm (s) {}

        Optional<PositionInfo> getPosition() const override
        {
            asked.fetch_add (1, std::memory_order_relaxed);
            lastBpm.store (shm->bpm, std::memory_order_relaxed);
            PositionInfo p;
            const double bpm = shm->bpm > 0.0 ? shm->bpm : 120.0;
            p.setBpm (bpm);
            p.setPpqPosition (shm->ppqPosition);
            p.setTimeSignature (TimeSignature { shm->timeSigNumerator  > 0 ? shm->timeSigNumerator  : 4,
                                                shm->timeSigDenominator > 0 ? shm->timeSigDenominator : 4 });
            p.setIsPlaying (true);
            p.setIsRecording (false);
            p.setTimeInSamples (samplesElapsed);
            p.setTimeInSeconds ((double) samplesElapsed / jmax (1.0, sampleRate));
            return p;
        }

        ipc::SharedBlock* shm;
        int64_t samplesElapsed = 0;
        double sampleRate = 48000.0;

        // Proof the plugin actually asked, and what it was told. Reported to
        // stderr when the process exits so a test can see it.
        mutable std::atomic<int> asked { 0 };
        mutable std::atomic<double> lastBpm { 0.0 };
    };

    //==============================================================================
    // Audio thread: waits for blocks from the host and renders them.
    struct AudioThread : public Thread
    {
        explicit AudioThread (PluginServer& s) : Thread ("plugin-rt"), server (s) {}
        void run() override
        {
            // So a parameter notification can tell "this is the render thread"
            // from "this is some other thread the plugin happens to use".
            server.audioThreadId.store (getThreadId(), std::memory_order_relaxed);
            server.audioLoop (*this);
            server.audioThreadId.store (nullptr, std::memory_order_relaxed);
        }
        PluginServer& server;
    };

    void startAudioThread()
    {
        if (audioThread != nullptr) return;
        audioThread = std::make_unique<AudioThread> (*this);
        if (! audioThread->startRealtimeThread (Thread::RealtimeOptions().withPriority (9)))
            audioThread->startThread (Thread::Priority::highest);
    }

    void stopAudioThread()
    {
        if (audioThread == nullptr) return;
        audioThread->signalThreadShouldExit();
        ipc::postSemaphore (shm->request);

        // Never let JUCE kill this thread. stopThread() force-kills after its timeout,
        // and killing a thread inside a plugin's process() unwinds through C++ frames,
        // which glibc turns into an abort: the whole helper dies, taking a working
        // plugin with it (seen as SIGABRT in unwind_cleanup while a second Kontakt was
        // loading). A plugin that is slow to finish a block is normal, so wait for it
        // as long as it takes; the watchdog in main() is what handles a genuinely
        // wedged plugin, by killing the *process* rather than a thread inside it.
        for (int waited = 0; audioThread->isThreadRunning(); waited += 50)
        {
            ipc::postSemaphore (shm->request);              // in case it is between waits
            audioThread->waitForThreadToExit (50);
            if (waited > 0 && waited % 5000 == 0)
                std::fprintf (stderr, "stagerig-plugin-host: waiting for the plugin to finish its block (%ds)\n", waited / 1000);
        }
        audioThread.reset();
    }

    void audioLoop (Thread& thread)
    {
        while (! thread.threadShouldExit())
        {
            if (! ipc::waitSemaphoreUntil (shm->request, ipc::monotonicDeadline (0.2)))
                continue;
            if (shm->quit.load() != 0 || thread.threadShouldExit())
                break;

            const auto seq = shm->requestSeq.load (std::memory_order_acquire);
            if (shm->completedSeq.load (std::memory_order_relaxed) == seq)
                continue;   // spurious wake

            try { renderBlock(); }
            catch (...)
            {
                // A throwing plugin produces silence for this block instead of killing the process.
                FloatVectorOperations::clear (shm->outL, ipc::kMaxBlock);
                FloatVectorOperations::clear (shm->outR, ipc::kMaxBlock);
            }

            shm->completedSeq.store (seq, std::memory_order_release);
            ipc::postSemaphore (shm->done);
        }
    }

    void renderBlock()
    {
        const int n = jlimit (1, ipc::kMaxBlock, shm->numSamples);
        FloatVectorOperations::clear (shm->outL, n);
        FloatVectorOperations::clear (shm->outR, n);

        const std::lock_guard<std::mutex> l (processMutex);
        if (instance == nullptr || instance->isSuspended()) return;

        midi.clear();
        for (uint32_t i = 0; i < shm->midiCount && i < ipc::kMaxMidi; ++i)
        {
            const auto& ev = shm->midi[i];
            midi.addEvent (ev.data, (int) ev.size, ev.samplePosition);
        }

        if (shm->paramChangeCount > 0)
        {
            suppressNotifications = true;
            auto& params = instance->getParameters();
            for (uint32_t i = 0; i < shm->paramChangeCount && i < ipc::kMaxParamChanges; ++i)
            {
                const auto& pc = shm->paramChanges[i];
                if ((int) pc.index < params.size())
                    params[(int) pc.index]->setValueNotifyingHost (jlimit (0.0f, 1.0f, pc.value));
            }
            suppressNotifications = false;
        }

        AudioBuffer<float> view (scratch.getArrayOfWritePointers(), scratch.getNumChannels(), n);
        view.clear();

        const int nIn = instance->getMainBusNumInputChannels();
        if (nIn >= 2)
        {
            view.copyFrom (0, 0, shm->inL, n);
            view.copyFrom (1, 0, shm->inR, n);
        }
        else if (nIn == 1)
        {
            view.copyFrom (0, 0, shm->inL, n);
            view.addFrom (0, 0, shm->inR, n);
            view.applyGain (0, 0, n, 0.5f);
        }

        instance->processBlock (view, midi);
        if (playHead != nullptr) playHead->samplesElapsed += n;

        const int nOut = instance->getMainBusNumOutputChannels();
        if (nOut <= 0) return;
        FloatVectorOperations::copy (shm->outL, view.getReadPointer (0), n);
        FloatVectorOperations::copy (shm->outR, view.getReadPointer (nOut == 1 ? 0 : 1), n);
    }

    //==============================================================================
    std::unique_ptr<HostPlayHead> playHead;
    ipc::SharedBlock* shm;
    int socketFd;
    AudioPluginFormatManager formatManager;
    std::unique_ptr<AudioPluginInstance> instance;
    std::unique_ptr<EditorWindow> editorWindow;
    std::unique_ptr<AudioThread> audioThread;
    std::thread control;
    std::mutex sendMutex, processMutex;
    std::atomic<bool> suppressNotifications { false };
    /* Which thread renders blocks. A parameter notification arriving on it is
       dropped, because sending one takes a lock; anything else -- the message
       thread, or a worker thread of the plugin's own -- is safe to report. */
    std::atomic<Thread::ThreadID> audioThreadId { nullptr };
    AudioBuffer<float> scratch;
    MidiBuffer midi;
    double sampleRate = 44100.0;
    int blockSize = 512;
};

//==============================================================================
#if ! JUCE_WINDOWS
/** Every process descended from `root` (not including root itself). */
static std::vector<pid_t> collectDescendants (pid_t root)
{
    std::vector<std::pair<pid_t, pid_t>> procs;   // (pid, ppid)
    if (DIR* d = ::opendir ("/proc"))
    {
        while (auto* e = ::readdir (d))
        {
            const pid_t pid = (pid_t) atoi (e->d_name);
            if (pid <= 0) continue;
            FILE* f = ::fopen (("/proc/" + std::string (e->d_name) + "/stat").c_str(), "r");
            if (f == nullptr) continue;
            char buf[512] = {};
            const auto n = ::fread (buf, 1, sizeof (buf) - 1, f);
            ::fclose (f);
            if (n == 0) continue;
            // "pid (comm) state ppid ..." -- comm may contain spaces, so find the last ')'
            const char* close = ::strrchr (buf, ')');
            if (close == nullptr) continue;
            int ppid = 0; char state = 0;
            if (::sscanf (close + 1, " %c %d", &state, &ppid) == 2)
                procs.emplace_back (pid, (pid_t) ppid);
        }
        ::closedir (d);
    }

    std::vector<pid_t> found { root };
    for (size_t i = 0; i < found.size(); ++i)
        for (auto& [pid, ppid] : procs)
            if (ppid == found[i] && std::find (found.begin(), found.end(), pid) == found.end())
                found.push_back (pid);
    found.erase (found.begin());
    return found;
}

/** Kills every process descended from us. Wine processes started by yabridge put
    themselves in their own session, so a process-group kill alone misses them. */
static void killDescendants (pid_t root)
{
    for (auto pid : collectDescendants (root))
        ::kill (pid, SIGKILL);
}

/** If StageRig dies while our message thread is stuck inside a plugin call, the
    control thread never sees the connection close. Watch the parent directly. */
static void armParentWatchdog()
{
    const pid_t parent = ::getppid();
    std::thread ([parent]
    {
        for (;;)
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (300));
            if (::getppid() != parent)
            {
                std::cerr << "stagerig-plugin-host: StageRig is gone, exiting\n";
                killDescendants (::getpid());
                ::kill (-::getpid(), SIGKILL);
            }
        }
    }).detach();
}

/** Plugin teardown can hang (yabridge waits for a Wine process that never exits).
    Once we've decided to exit, give cleanup a moment, then take our descendants and
    our whole process group down with us. */
static void armExitWatchdog (int graceMs)
{
    std::thread ([graceMs]
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (graceMs));
        killDescendants (::getpid());
        ::kill (-::getpid(), SIGKILL);
    }).detach();
}
#else
/* Windows: no Wine, no process groups, no /proc. Watching the parent comes
   with the port of the serve mode; until then a probe that hangs in teardown
   is ended after its grace period. */
static void killDescendants (int) {}
static void armParentWatchdog() {}
static void armExitWatchdog (int graceMs)
{
    std::thread ([graceMs]
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (graceMs));
        std::_Exit (0);
    }).detach();
}
#endif

class PluginHostApplication : public JUCEApplication
{
public:
    const String getApplicationName() override       { return "stagerig-plugin-host"; }
    const String getApplicationVersion() override    { return "1.0"; }
    bool moreThanOneInstanceAllowed() override       { return true; }

    void initialise (const String& commandLine) override
    {
        // Own process group: lets the watchdog take stuck Wine children down with us,
        // without touching StageRig.
       #if ! JUCE_WINDOWS
        ::setpgid (0, 0);
       #endif
        armParentWatchdog();

        ArgumentList args ("stagerig-plugin-host", commandLine);

        if (args.containsOption ("--scan"))
        {
            // --scan <format> <file-or-identifier>: print PluginDescription XML, one per
            // line, then an end marker. Our stdout must not leak into Wine processes the
            // plugin bridge spawns, or the parent would wait for them to exit.
           #if ! JUCE_WINDOWS
            ::fcntl (STDOUT_FILENO, F_SETFD, FD_CLOEXEC);
           #endif
            const int i = args.indexOfOption ("--scan");
            if (i < 0 || i + 2 >= args.size()) { setApplicationReturnValue (2); quit(); return; }
            const auto formatName = args[i + 1].text;
            const auto fileOrId = args[i + 2].text;

            // Bridged plugins sometimes hang in module unload, after their descriptions
            // have been collected, waiting for a Wine process that won't exit. If the
            // probe runs suspiciously long, kill our Wine descendants: the unload then
            // returns and the results still get printed.
            static std::atomic<bool> probeDone { false };
            std::thread ([]
            {
                std::this_thread::sleep_for (std::chrono::seconds (30));
                if (! probeDone.load())
                {
                    std::cerr << "stagerig-plugin-host: probe still running after 30 s, killing bridge processes\n";
                   #if ! JUCE_WINDOWS
                    killDescendants (::getpid());
                   #endif
                }
            }).detach();

            AudioPluginFormatManager fm;
            addDefaultFormatsToManager (fm);
            fm.addFormat (new ClapPluginFormat());
            for (auto* f : fm.getFormats())
                if (f->getName() == formatName)
                {
                    // LV2 only resolves plugin classes (and thus isInstrument / category)
                    // once the whole world has been loaded, which the path search does.
                    if (formatName == "LV2")
                        f->searchPathsForPlugins (f->getDefaultLocationsToSearch(), true, false);

                    OwnedArray<PluginDescription> found;
                    f->findAllTypesForFile (found, fileOrId);
                    for (auto* d : found)
                        if (auto xml = d->createXml())
                            std::cout << xml->toString (XmlElement::TextFormat().singleLine()).toRawUTF8() << "\n";
                }
            probeDone.store (true);
            std::cout << "<SCAN-DONE>\n";
            std::cout.flush();
           #if ! JUCE_WINDOWS
            ::close (STDOUT_FILENO);      // the parent has everything; don't make it wait for teardown
           #endif
            armExitWatchdog (3000);
            quit();
            return;
        }

        const int i = args.indexOfOption ("--serve");
        if (i < 0 || i + 2 >= args.size())
        {
            std::cerr << "usage: stagerig-plugin-host --serve <shm-name> <socket-fd> | --scan <format> <file>\n";
            setApplicationReturnValue (2);
            quit();
            return;
        }

        auto* shm = ipc::openSharedBlock (args[i + 1].text);
        const int fd = args[i + 2].text.getIntValue();
       #if ! JUCE_WINDOWS
        if (fd >= 0) ::fcntl (fd, F_SETFD, FD_CLOEXEC);   // Wine children must not hold the control socket open
       #endif
        if (shm == nullptr || fd < 0)
        {
            std::cerr << "stagerig-plugin-host: cannot open shared block\n";
            setApplicationReturnValue (1);
            quit();
            return;
        }

        server = std::make_unique<PluginServer> (shm, fd);
        server->onQuit = [this] { quit(); };
    }

    void shutdown() override
    {
        armExitWatchdog (3000);
        server.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    std::unique_ptr<PluginServer> server;
};

START_JUCE_APPLICATION (PluginHostApplication)
