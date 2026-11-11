#include "DeviceReservation.h"

#if JUCE_LINUX
 #include <dlfcn.h>
 #include <atomic>
 #include <climits>
 #include <condition_variable>
 #include <deque>
 #include <functional>
 #include <future>
 #include <map>
 #include <mutex>
 #include <thread>
#endif

using namespace juce;

namespace perf::DeviceReservation
{

#if JUCE_LINUX
namespace
{
    /* libdbus, opened at run time: on every desktop, and StageRig still starts
       (holding nothing) on a system without it. Only what the protocol needs. */
    struct DBusErrorStorage { const char* name; const char* message; unsigned int bits; void* padding; char spare[32]; };
    using Conn = void;
    using Msg = void;

    constexpr int busSession = 0;
    constexpr unsigned flagAllowReplacement = 0x1, flagReplaceExisting = 0x2, flagDoNotQueue = 0x4;
    constexpr int primaryOwner = 1, alreadyOwner = 4;
    constexpr int typeInvalid = 0, typeBoolean = 'b', typeInt32 = 'i';
    constexpr int handled = 0, notYetHandled = 1;

    struct DBus
    {
        void* lib = nullptr;
        void (*errorInit) (DBusErrorStorage*) = nullptr;
        void (*errorFree) (DBusErrorStorage*) = nullptr;
        unsigned (*errorIsSet) (const DBusErrorStorage*) = nullptr;
        unsigned (*threadsInitDefault)() = nullptr;
        Conn* (*busGetPrivate) (int, DBusErrorStorage*) = nullptr;
        void (*setExitOnDisconnect) (Conn*, unsigned) = nullptr;
        int (*requestName) (Conn*, const char*, unsigned, DBusErrorStorage*) = nullptr;
        int (*releaseName) (Conn*, const char*, DBusErrorStorage*) = nullptr;
        Msg* (*newMethodCall) (const char*, const char*, const char*, const char*) = nullptr;
        unsigned (*appendArgs) (Msg*, int, ...) = nullptr;
        unsigned (*getArgs) (Msg*, DBusErrorStorage*, int, ...) = nullptr;
        Msg* (*sendWithReplyAndBlock) (Conn*, Msg*, int, DBusErrorStorage*) = nullptr;
        void (*unref) (Msg*) = nullptr;
        unsigned (*readWriteDispatch) (Conn*, int) = nullptr;
        unsigned (*addFilter) (Conn*, int (*) (Conn*, Msg*, void*), void*, void (*) (void*)) = nullptr;
        unsigned (*isMethodCall) (Msg*, const char*, const char*) = nullptr;
        Msg* (*newMethodReturn) (Msg*) = nullptr;
        unsigned (*send) (Conn*, Msg*, unsigned*) = nullptr;
        void (*flush) (Conn*) = nullptr;
        void (*close) (Conn*) = nullptr;          // these two are not needed to work: looked up, never required
        void (*connUnref) (Conn*) = nullptr;

        bool load()
        {
            lib = ::dlopen ("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
            if (lib == nullptr) return false;
            auto get = [this] (auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype (fn)>> (::dlsym (lib, name)); return fn != nullptr; };
            get (close, "dbus_connection_close");
            get (connUnref, "dbus_connection_unref");
            return get (errorInit, "dbus_error_init") && get (errorFree, "dbus_error_free") && get (errorIsSet, "dbus_error_is_set")
                && get (threadsInitDefault, "dbus_threads_init_default") && get (busGetPrivate, "dbus_bus_get_private")
                && get (setExitOnDisconnect, "dbus_connection_set_exit_on_disconnect")
                && get (requestName, "dbus_bus_request_name") && get (releaseName, "dbus_bus_release_name")
                && get (newMethodCall, "dbus_message_new_method_call") && get (appendArgs, "dbus_message_append_args")
                && get (getArgs, "dbus_message_get_args") && get (sendWithReplyAndBlock, "dbus_connection_send_with_reply_and_block")
                && get (unref, "dbus_message_unref") && get (readWriteDispatch, "dbus_connection_read_write_dispatch")
                && get (addFilter, "dbus_connection_add_filter") && get (isMethodCall, "dbus_message_is_method_call")
                && get (newMethodReturn, "dbus_message_new_method_return") && get (send, "dbus_connection_send")
                && get (flush, "dbus_connection_flush");
        }
    };

    /* Everything on the bus happens on one thread of its own: libdbus waiting
       for a reply on one thread while another reads the same connection never
       got the reply. The message thread hands it work and waits a bounded time. */
    struct Reservations
    {
        DBus dbus;
        Conn* conn = nullptr;
        bool tried = false;
        std::map<int, String> held;     // card -> bus name; the bus thread's
        std::atomic<int> heldCount { 0 };

        std::mutex jobsLock;
        std::condition_variable jobsWaiting;
        std::deque<std::function<void()>> jobs;
        std::atomic<bool> running { true };
        std::thread worker { [this] { run(); } };

        ~Reservations()
        {
            running = false;
            jobsWaiting.notify_all();
            if (worker.joinable()) worker.join();
            // The connection closing hands every name back.
        }

        /** Runs job on the bus thread; false if it has not finished in timeoutMs. */
        bool call (std::function<void()> job, int timeoutMs)
        {
            auto done = std::make_shared<std::promise<void>>();
            auto finished = done->get_future();
            {
                std::lock_guard<std::mutex> g (jobsLock);
                jobs.push_back ([job = std::move (job), done] { job(); done->set_value(); });
            }
            jobsWaiting.notify_all();
            return finished.wait_for (std::chrono::milliseconds (timeoutMs)) == std::future_status::ready;
        }

        void run()
        {
            while (running.load())
            {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> g (jobsLock);
                    if (jobs.empty() && conn == nullptr)
                        jobsWaiting.wait_for (g, std::chrono::milliseconds (200));
                    if (! jobs.empty()) { job = std::move (jobs.front()); jobs.pop_front(); }
                }
                if (job) { job(); continue; }
                /* Answers RequestRelease. False is the bus gone (the desktop
                   session's bus restarted): every name went with it, and asked
                   again and again it would answer at once for ever -- a core
                   spent on nothing. Let go of, and connected afresh when a card
                   is next asked for. */
                if (conn != nullptr && dbus.readWriteDispatch (conn, 100) == 0)
                {
                    std::fprintf (stderr, "[audio] the desktop's message bus has gone: the sound cards are no longer reserved\n");
                    if (dbus.close != nullptr) dbus.close (conn);
                    if (dbus.connUnref != nullptr) dbus.connUnref (conn);
                    conn = nullptr;
                    tried = false;
                    held.clear();
                    heldCount = 0;
                }
            }
        }

        bool connect()
        {
            if (conn != nullptr) return true;
            if (tried) return false;
            tried = true;
            if (dbus.lib == nullptr && ! dbus.load()) { std::fprintf (stderr, "[audio] no libdbus: sound cards cannot be asked for from PipeWire\n"); return false; }
            dbus.threadsInitDefault();
            DBusErrorStorage err {}; dbus.errorInit (&err);
            conn = dbus.busGetPrivate (busSession, &err);
            if (conn == nullptr || dbus.errorIsSet (&err))
            {
                std::fprintf (stderr, "[audio] no session bus: %s\n", err.message != nullptr ? err.message : "?");
                dbus.errorFree (&err);
                conn = nullptr;
                return false;
            }
            dbus.setExitOnDisconnect (conn, 0);
            dbus.addFilter (conn, &Reservations::filter, this, nullptr);
            return true;
        }

        /* Another program asking for a card StageRig holds: refused. A live rig
           does not give its sound card away mid-song; the card is handed back
           when the Audio window leaves ALSA, or StageRig quits. */
        static int filter (Conn* c, Msg* m, void* self)
        {
            auto& r = *static_cast<Reservations*> (self);
            if (! r.dbus.isMethodCall (m, "org.freedesktop.ReserveDevice1", "RequestRelease")) return notYetHandled;
            if (Msg* reply = r.dbus.newMethodReturn (m))
            {
                unsigned no = 0;
                r.dbus.appendArgs (reply, typeBoolean, &no, typeInvalid);
                r.dbus.send (c, reply, nullptr);
                r.dbus.unref (reply);
                r.dbus.flush (c);
            }
            return handled;
        }

        bool reserve (int card)
        {
            if (held.count (card) != 0) return true;
            if (! connect()) return false;
            const String device = "Audio" + String (card);
            const String name = "org.freedesktop.ReserveDevice1." + device;
            const String path = "/org/freedesktop/ReserveDevice1/" + device;

            DBusErrorStorage err {}; dbus.errorInit (&err);
            int r = dbus.requestName (conn, name.toRawUTF8(), flagDoNotQueue, &err);
            dbus.errorFree (&err);
            if (r != primaryOwner && r != alreadyOwner)
            {
                // Held by someone (PipeWire): asked to let go, at the highest priority, as jackd asks.
                bool granted = false;
                if (Msg* call = dbus.newMethodCall (name.toRawUTF8(), path.toRawUTF8(), "org.freedesktop.ReserveDevice1", "RequestRelease"))
                {
                    int priority = INT_MAX;
                    dbus.appendArgs (call, typeInt32, &priority, typeInvalid);
                    dbus.errorInit (&err);
                    if (Msg* reply = dbus.sendWithReplyAndBlock (conn, call, 3000, &err))
                    {
                        unsigned ok = 0;
                        DBusErrorStorage e2 {}; dbus.errorInit (&e2);
                        if (dbus.getArgs (reply, &e2, typeBoolean, &ok, typeInvalid)) granted = ok != 0;
                        dbus.errorFree (&e2);
                        dbus.unref (reply);
                    }
                    dbus.errorFree (&err);
                    dbus.unref (call);
                }
                if (! granted)
                {
                    std::fprintf (stderr, "[audio] the owner of sound card %d would not let it go\n", card);
                    return false;
                }
                dbus.errorInit (&err);
                r = dbus.requestName (conn, name.toRawUTF8(), flagDoNotQueue | flagReplaceExisting, &err);
                dbus.errorFree (&err);
                if (r != primaryOwner && r != alreadyOwner)
                {
                    std::fprintf (stderr, "[audio] sound card %d: reservation refused (%d)\n", card, r);
                    return false;
                }
            }
            (void) flagAllowReplacement;
            held[card] = name;
            heldCount = (int) held.size();
            std::fprintf (stderr, "[audio] sound card %d reserved for StageRig\n", card);
            return true;
        }

        void release (int card)
        {
            const auto it = held.find (card);
            if (it == held.end()) return;
            DBusErrorStorage err {}; dbus.errorInit (&err);
            dbus.releaseName (conn, it->second.toRawUTF8(), &err);
            dbus.errorFree (&err);
            held.erase (it);
            heldCount = (int) held.size();
            std::fprintf (stderr, "[audio] sound card %d handed back\n", card);
        }
    };

    Reservations& reservations() { static Reservations r; return r; }
}

bool cardIsClosed (int card)
{
    const File dir ("/proc/asound/card" + String (card));
    if (! dir.isDirectory()) return true;
    for (const auto& pcm : dir.findChildFiles (File::findDirectories, false, "pcm*"))
        for (const auto& sub : pcm.findChildFiles (File::findDirectories, false, "sub*"))
            if (! sub.getChildFile ("status").loadFileAsString().trim().startsWith ("closed"))
                return false;
    return true;
}

Array<int> holdOnly (const Array<int>& cards, int waitMs)
{
    auto& r = reservations();
    if (cards.isEmpty() && r.heldCount.load() == 0) return {};
    auto wanted = std::make_shared<Array<int>> (cards);
    const bool answered = r.call ([&r, wanted]
    {
        std::vector<int> drop;
        for (const auto& [card, name] : r.held)
            if (! wanted->contains (card)) drop.push_back (card);
        for (int card : drop) r.release (card);
        for (int card : *wanted) r.reserve (card);
    }, jmax (1000, waitMs));
    if (! answered)
        std::fprintf (stderr, "[audio] the desktop did not answer about the sound cards in time\n");

    // The old owner closes the card a moment after letting it go.
    const auto until = Time::getMillisecondCounter() + (uint32) jmax (0, waitMs);
    Array<int> busy;
    for (;;)
    {
        busy.clearQuick();
        for (int card : cards) if (! cardIsClosed (card)) busy.add (card);
        if (busy.isEmpty() || Time::getMillisecondCounter() >= until) break;
        Thread::sleep (50);
    }
    return busy;
}

#else
Array<int> holdOnly (const Array<int>&, int) { return {}; }
bool cardIsClosed (int) { return true; }
#endif

} // namespace perf::DeviceReservation
