// Headless integration test: loads a real LV2 instrument, routes MIDI through the
// engine and checks audio, program changes, mappings and learn.
#include <cstring>
#include "Engine.h"
#include "TempoDetect.h"
#include "Stretch.h"
#include "MidiFx.h"
#include "Charts.h"
#include <ableton/Link.hpp>
#include "PluginIcons.h"
#include "MappingSuggestions.h"
#include "ClapPluginFormat.h"
#include "GmSynth.h"
#include "MidiEditor.h"
#include "StagePanel.h"
#include "PanelParams.h"
#include "PropertyExchange.h"
#include <juce_events/juce_events.h>
#include <cstdio>
#include <map>
#include <cmath>
#include <set>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#if defined (SYS_pidfd_open) && defined (SYS_pidfd_getfd)
 #define HAVE_PIDFD 1
#endif

#include "Surfaces.h"
#include "ControlMaps.h"

using namespace perf;
using namespace juce;

static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } else std::printf ("ok   %s\n", #cond); } while (0)

namespace
{
    constexpr int kBlock = 512;

    void pump (int ms)
    {
        MessageManager::getInstance()->runDispatchLoopUntil (ms);
    }

    /** Renders `blocks` blocks and returns the peak RMS across them. */
    float render (Engine& e, int blocks)
    {
        AudioBuffer<float> buf (2, kBlock);
        float peakRms = 0.0f;
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear();
            e.renderBlockForTesting (buf.getArrayOfWritePointers(), 2, kBlock);
            peakRms = jmax (peakRms, buf.getRMSLevel (0, 0, kBlock), buf.getRMSLevel (1, 0, kBlock));
        }
        return peakRms;
    }

    struct TestListener : public Engine::Listener
    {
        int learnInput = -1, learnNumber = -1; MappingDef::Source learnSource = MappingDef::Source::CC;
        int programChangedInput = -1, programChangedTo = -1;
        void learnReceived (int i, MappingDef::Source s, int n) override { learnInput = i; learnSource = s; learnNumber = n; }
        void programChanged (int i, int p) override { programChangedInput = i; programChangedTo = p; }
        void statusMessage (const String& s) override { std::printf ("     [status] %s\n", s.toRawUTF8()); }
        int knobTurned = -1, knobValue = -1;
        float knobExact = -2.0f;
        void surfaceKnob (int, int knob, int value, bool, float exact) override { knobTurned = knob; knobValue = value; knobExact = exact; }
    };
}

/** STAGERIG_TEST_KONTROL_SECONDS=n: drives a real Komplete Kontrol MK1 for n
    seconds instead of the tests -- the lights in moving colours, a program
    and eight knobs on its screens, every knob (moving its screen's bars),
    touch and button printed as it comes, and unplugging and plugging in again
    followed -- to try the driver on the keyboard itself. */
static int probeKompleteKontrol (int seconds)
{
    std::unique_ptr<KompleteKontrolMk1> kk;
    String said;
    const auto end = Time::getMillisecondCounter() + (uint32) seconds * 1000;
    uint32 nextLook = 0, nextColours = 0;
    int phase = 0;
    std::vector<ControlSurface::Touch> touches;
    // What its screens show: a program, and eight knobs to turn.
    SurfaceView view;
    view.program = 12; view.programName = "Grand Piano Ballad";
    const char* names[] = { "Filter Cutoff", "Resonance", "Attack", "Release", "Reverb Mix", "Chorus", "Volume", "Pan" };
    for (int k = 0; k < SurfaceView::numKnobs; ++k)
    {
        view.knobNames[(size_t) k] = names[k];
        view.knobLevels[(size_t) k] = 0.5f;
        view.knobValues[(size_t) k] = "50 %";
    }
    while (Time::getMillisecondCounter() < end)
    {
        const auto now = Time::getMillisecondCounter();
        if (kk != nullptr && ! kk->isConnected()) { std::printf ("kontrol: gone\n"); kk.reset(); }
        if (kk == nullptr && now >= nextLook)
        {
            nextLook = now + 1000;
            for (const auto& d : KompleteKontrolMk1::find())
            {
                String error;
                auto k = std::make_unique<KompleteKontrolMk1> (d, error);
                if (k->opened()) { std::printf ("kontrol: opened %s at %s\n", k->getName().toRawUTF8(), d.path.toRawUTF8()); kk = std::move (k); nextColours = 0; break; }
                if (error != said) std::printf ("kontrol: %s\n", error.toRawUTF8());
                said = error;
            }
        }
        if (kk != nullptr)
        {
            if (now >= nextColours)
            {
                // Every C red, the rest in a colour that changes every three seconds.
                static const uint32 others[] = { 0x0000ff, 0x00ff00, 0xffff00, 0xff00ff };
                for (int n = 0; n < 128; ++n) view.keyColours[(size_t) n] = n % 12 == 0 ? 0xff0000 : others[phase % 4];
                ++phase;
                nextColours = now + 3000;
            }
            touches.clear();
            kk->takeTouches (touches);
            for (const auto& t : touches)
            {
                if (t.type == ControlSurface::Touch::knob)
                {
                    // Each knob moves its screen's bars and value.
                    auto& level = view.knobLevels[(size_t) t.index];
                    level = jlimit (0.0f, 1.0f, level + t.amount);
                    view.knobValues[(size_t) t.index] = String (roundToInt (level * 100.0f)) + " %";
                }
                if (t.type == ControlSurface::Touch::knob)            std::printf ("kontrol: knob %d turned %+.3f\n", t.index + 1, t.amount);
                else if (t.type == ControlSurface::Touch::knobTouch)  std::printf ("kontrol: knob %d %s\n", t.index + 1, t.down ? "touched" : "let go");
                else                                                  std::printf ("kontrol: %s %s\n", SurfaceHub::describeControl (t.id).toRawUTF8(), t.down ? "pressed" : "released");
            }
            kk->show (view);
            if (const auto p = kk->getProblem(); p != said && p.isNotEmpty()) { std::printf ("kontrol: PROBLEM %s\n", p.toRawUTF8()); said = p; }
        }
        std::fflush (stdout);
        Thread::sleep (20);
    }
    std::printf ("kontrol: done\n");
    return 0;
}

/* STAGERIG_TEST_NKS=<part of a plugin's name>: finds this computer's NKS
   presets, then loads each plugin of that name from the rig's plugin list and
   prints how its maker's knobs were found among its parameters. */
static int probeNks (const String& wanted)
{
    const auto t0 = Time::getMillisecondCounterHiRes();
    ControlMapLibrary library (File::getSpecialLocation (File::tempDirectory).getChildFile ("stagerig-nks-probe"));
    const auto folders = ControlMapLibrary::defaultNksFolders();
    for (const auto& f : folders) std::printf ("nks: looking in %s\n", f.getFullPathName().toRawUTF8());
    library.startNksSearch (folders);
    if (! library.waitForNks (600000)) { std::printf ("nks: the search did not finish\n"); return 1; }
    std::printf ("nks: search done in %.1f s\n", (Time::getMillisecondCounterHiRes() - t0) / 1000.0);

    KnownPluginList list;
    File settingsFile = File::getSpecialLocation (File::userApplicationDataDirectory).getChildFile ("StageRig/StageRig.settings");
    if (auto xml = parseXML (settingsFile))
        for (auto* v : xml->getChildWithTagNameIterator ("VALUE"))
            if (v->getStringAttribute ("name") == "pluginList")
                if (auto* kp = v->getChildByName ("KNOWNPLUGINS")) list.recreateFromXml (*kp);
    for (const auto& d : list.getTypes())
    {
        if (! d.name.containsIgnoreCase (wanted)) continue;
        const auto* nks = library.findNks (d);
        if (nks == nullptr) { std::printf ("nks: %s (%s): no NKS presets\n", d.name.toRawUTF8(), d.pluginFormatName.toRawUTF8()); continue; }
        std::printf ("nks: %s (%s %s): pages of \"%s\" from %s, %s ids, matched %d\n", d.name.toRawUTF8(), d.pluginFormatName.toRawUTF8(),
                     d.manufacturerName.toRawUTF8(), nks->name.toRawUTF8(), nks->product.toRawUTF8(), nks->vst3Ids ? "VST3" : "VST2", nks->matches (d));
        RemotePlugin plugin;
        String error;
        if (! plugin.load (d, 48000.0, 256, error)) { std::printf ("nks:   could not load it: %s\n", error.toRawUTF8()); continue; }
        const auto& params = plugin.getParameters();
        const auto map = controlMapFromNks (*nks, d, params);
        int asked = 0, got = 0;
        for (const auto& np : nks->pages) for (const auto& k : np.knobs) if (k.paramId.isNotEmpty()) ++asked;
        for (const auto& mp : map.pages) for (const auto& k : mp.knobs) if (! k.isEmpty()) ++got;
        std::printf ("nks:   %d parameters; %d of %d knobs found\n", (int) params.size(), got, asked);
        const bool aligned = map.pages.size() == nks->pages.size();
        for (size_t pi = 0; pi < nks->pages.size(); ++pi)
        {
            std::printf ("nks:   page \"%s\"\n", nks->pages[pi].name.toRawUTF8());
            for (size_t ki = 0; ki < nks->pages[pi].knobs.size(); ++ki)
            {
                const auto& k = nks->pages[pi].knobs[ki];
                if (k.paramId.isEmpty()) continue;
                const ControlKnob* hit = aligned && ki < map.pages[pi].knobs.size() && ! map.pages[pi].knobs[ki].isEmpty() ? &map.pages[pi].knobs[ki] : nullptr;
                std::printf ("nks:     %-12s %-12s id %-11s -> %s\n", k.paramName.toRawUTF8(), k.label.toRawUTF8(), k.paramId.toRawUTF8(),
                             hit != nullptr ? ("\"" + hit->paramName + "\" (" + hit->paramId + ")").toRawUTF8() : aligned ? "NOT FOUND" : "(pages dropped)");
            }
        }
    }
    return 0;
}

/* A keyboard speaking MIDI-CI Property Exchange, played by JUCE's own
   MIDI-CI: it finds StageRig, reads the program in play, the programs and the
   knobs, and is told when the program changes. Each answer is checked against
   what the specifications say -- M2-101 MIDI-CI 1.2, M2-103 Common Rules for
   Property Exchange 1.2, M2-105 Foundational Resources, M2-107 ProgramList,
   and Korg's Keystage Property Exchange Resource List 1.0.
   STAGERIG_TEST_MIDICI=1 runs only this. Returns the failures. */
static int checkMidiCi()
{
    const int before = failures;
    {
        namespace ci = juce::midi_ci;
        std::deque<std::vector<std::byte>> toKeyboard, toRig;
        struct Out : ci::DeviceMessageHandler
        {
            std::deque<std::vector<std::byte>>& q;
            explicit Out (std::deque<std::vector<std::byte>>& d) : q (d) {}
            void processMessage (ump::BytesOnGroup m) override { q.emplace_back (m.bytes.begin(), m.bytes.end()); }
        } keyboardOut (toRig);
        struct Heard : ci::DeviceListener
        {
            struct Update { String command, subscribeId; var body; };
            std::vector<Update> updates;
            void propertySubscriptionDataReceived (ci::MUID, const ci::PropertySubscriptionData& d) override
            {
                updates.push_back ({ ci::PropertySubscriptionCommandUtils::toString (d.header.command), d.header.subscribeId,
                                     d.body.empty() ? var() : ci::Encodings::jsonFrom7BitText (d.body) });
            }
        } heard;
        Random random;
        // A keyboard whose SysEx messages hold 1024 bytes (one that asks must take 512 at least, M2-101 5.5.3).
        auto keyboard = std::make_unique<ci::Device> (ci::DeviceOptions().withOutputs ({ &keyboardOut })
                                 .withDeviceInfo ({ { std::byte { 0x42 }, std::byte {}, std::byte {} }, {}, {}, {} })
                                 .withFeatures (ci::DeviceFeatures().withPropertyExchangeSupported())
                                 .withMaxSysExSize (1024)
                                 .withProductInstanceId (ci::DeviceOptions::makeProductInstanceId (random)));
        keyboard->addListener (heard);
        PropertyResponder rig ([&toKeyboard] (Span<const std::byte> b) { toKeyboard.emplace_back (b.begin(), b.end()); });
        auto settleCi = [&]
        {
            for (int i = 0; i < 1000 && (! toKeyboard.empty() || ! toRig.empty()); ++i)
            {
                if (! toRig.empty())      { auto m = std::move (toRig.front()); toRig.pop_front(); rig.receive (Span<const std::byte> (m.data(), m.size())); }
                if (! toKeyboard.empty()) { auto m = std::move (toKeyboard.front()); toKeyboard.pop_front(); if (keyboard != nullptr) keyboard->processMessage ({ 0, m }); }
            }
        };
        auto entry = [] (const var& list, const String& resource) -> var
        {
            if (const auto* a = list.getArray())
                for (const auto& e : *a)
                    if (e.getProperty ("resource", {}) == var (resource)) return e;
            return {};
        };

        // The keyboard finds StageRig before StageRig has shown it anything, as one found at start-up does.
        CHECK (! rig.isAskedFor());
        keyboard->sendDiscovery();
        settleCi();
        const auto muids = keyboard->getDiscoveredMuids();
        CHECK (muids.size() == 1);
        if (muids.size() != 1) return failures - before;
        const auto m = muids[0];
        // As a keyboard does next: what can you tell me? Then JUCE reads ResourceList, DeviceInfo and ChannelList.
        keyboard->sendPropertyCapabilitiesInquiry (m);
        settleCi();
        const auto resources = keyboard->getResourceListForMuid (m);
        const auto info = keyboard->getDeviceInfoForMuid (m);
        std::printf ("     MIDI-CI: resources %s, device %s\n", JSON::toString (resources, true).toRawUTF8(), JSON::toString (info, true).toRawUTF8());
        CHECK (rig.isAskedFor());

        // ResourceList (M2-103 14): every resource but itself; a maker's resource with its schema's title, type and where to ask for the rest (14.4).
        CHECK (resources.size() == 6 && entry (resources, "ResourceList").isVoid());
        CHECK (entry (resources, "DeviceInfo").isObject() && entry (resources, "JSONSchema").isObject());
        CHECK (entry (resources, "ChannelList").getProperty ("canSubscribe", {}) == var (true));
        CHECK (entry (resources, "ProgramList").getProperty ("canSubscribe", {}) == var (true) && entry (resources, "ProgramList").getProperty ("canPaginate", {}) == var (true));
        for (const auto* x : { "X-ParameterList", "X-ProgramEdit" })
        {
            const auto schema = entry (resources, x).getProperty ("schema", {});
            CHECK (entry (resources, x).getProperty ("canSubscribe", {}) == var (true) && schema.hasProperty ("title") && schema.hasProperty ("type")
                   && schema.getProperty ("$ref", {}).toString().startsWith ("midi+jsonschema://"));
        }
        CHECK (entry (resources, "X-ParameterList").getProperty ("schema", {}).getProperty ("type", {}) == var ("array"));
        CHECK (entry (resources, "X-ProgramEdit").getProperty ("schema", {}).getProperty ("type", {}) == var ("object"));

        // DeviceInfo (M2-105 3.2): the ids as in the Discovery reply, 7D (125) being MIDI's non-commercial maker, and every required name.
        const auto discovery = keyboard->getDiscoveryInfoForMuid (m);
        CHECK (discovery.has_value() && discovery->maximumSysexSize >= 512);
        auto ids = [] (const var& a) { StringArray s; for (int i = 0; i < a.size(); ++i) s.add (a[i].toString()); return s.joinIntoString (","); };
        auto bytes = [] (const auto& arr) { StringArray s; for (auto b : arr) s.add (String ((int) b)); return s.joinIntoString (","); };
        CHECK (ids (info.getProperty ("manufacturerId", {})) == "125,0,0" && discovery.has_value() && ids (info.getProperty ("manufacturerId", {})) == bytes (discovery->device.manufacturer));
        CHECK (discovery.has_value() && ids (info.getProperty ("familyId", {})) == bytes (discovery->device.family) && ids (info.getProperty ("modelId", {})) == bytes (discovery->device.modelNumber)
               && ids (info.getProperty ("versionId", {})) == bytes (discovery->device.revision));
        for (const auto* field : { "manufacturer", "family", "model", "version" })
            CHECK (info.getProperty (field, {}).toString().isNotEmpty());
        CHECK (info.getProperty ("model", {}) == var ("StageRig"));

        // Asked before StageRig has anything to show: the resources are there, empty, not missing (404).
        const auto channelsBefore = keyboard->getChannelListForMuid (m);
        CHECK (channelsBefore.isArray() && channelsBefore.size() == 0);

        int status = 0; String message; var body; int total = 0;
        auto get = [&] (const String& resource, const String& resId, std::optional<ci::Pagination> page)
        {
            ci::PropertyRequestHeader h;
            h.resource = resource;
            h.resId = resId;
            h.pagination = page;
            status = 0; message.clear(); body = var(); total = -1;
            keyboard->sendPropertyGetInquiry (m, h, [&] (const ci::PropertyExchangeResult& r)
            {
                if (r.getError().has_value()) return;
                const auto reply = r.getHeaderAsReplyHeader();
                status = reply.status;
                message = reply.message;
                body = ci::Encodings::jsonFrom7BitText (r.getBody());
                const auto it = reply.extended.find (Identifier ("totalCount"));
                total = it != reply.extended.end() ? (int) it->second : -1;
            });
            settleCi();
            return status == 200;
        };
        CHECK (get ("X-ProgramEdit", {}, std::nullopt) && body.getProperty ("currentValues", {}).isArray() && body.getProperty ("currentValues", {}).size() == 0);
        CHECK (get ("ProgramList", {}, std::nullopt) && body.isArray() && body.size() == 0 && total == 0);

        // Now StageRig shows it something.
        SurfaceView pv;
        pv.inputName = "Upper"; pv.channel = 2; pv.program = 3; pv.programName = "Grand Piano";
        pv.programs = { { 0, "Organ" }, { 3, "Grand Piano" }, { 9, "Strings" } };
        pv.knobNames[0] = "Cutoff"; pv.knobValues[0] = "40%"; pv.knobLevels[0] = 0.4f;
        for (int k = 0; k < SurfaceView::numKnobs; ++k) pv.knobControllers[(size_t) k] = SurfaceControls::autoKnobCC + k;
        rig.show (pv);
        settleCi();

        // ChannelList (M2-105 4.3): the input, its channel (from 1), the program's name and [bank MSB, bank LSB, program], and links to the programs and to Korg's knobs.
        CHECK (get ("ChannelList", {}, std::nullopt) && body.size() == 1 && total == -1);
        std::printf ("     MIDI-CI: a keyboard found StageRig, read %s\n", JSON::toString (body, true).toRawUTF8());
        const auto ch = body[0];
        CHECK (ch.getProperty ("title", {}) == var ("Upper") && ch.getProperty ("channel", {}) == var (2) && ch.getProperty ("programTitle", {}) == var ("Grand Piano"));
        CHECK (ch.getProperty ("bankPC", {}).size() == 3 && ch.getProperty ("bankPC", {})[0] == var (0) && ch.getProperty ("bankPC", {})[2] == var (3));
        CHECK (entry (ch.getProperty ("links", {}), "ProgramList").getProperty ("resId", {}).toString().isNotEmpty()
               && entry (ch.getProperty ("links", {}), "X-ParameterList").isObject() && entry (ch.getProperty ("links", {}), "X-ProgramEdit").isObject());
        {
            // No program in play: the entry is still there, without one.
            SurfaceView none = pv; none.program = -1; none.programName.clear();
            const auto c = PropertyResponder::channelList (none);
            CHECK (c.size() == 1 && c[0].getProperty ("title", {}) == var ("Upper") && c[0].getProperty ("channel", {}) == var (2)
                   && ! c[0].hasProperty ("programTitle") && ! c[0].hasProperty ("bankPC") && c[0].hasProperty ("links"));
        }

        // ProgramList (M2-107; M2-103 8.6.2): every program with something in it; totalCount in every reply; a page from a 0-based offset; with or without a resId (a Keystage sends none).
        CHECK (get ("ProgramList", {}, std::nullopt) && body.size() == 3 && total == 3 && body[0].getProperty ("title", {}) == var ("Organ"));
        CHECK (get ("ProgramList", "all", ci::Pagination { 1, 5 }) && total == 3 && body.size() == 2
               && body[0].getProperty ("title", {}) == var ("Grand Piano") && body[1].getProperty ("bankPC", {})[2] == var (9));
        CHECK (get ("ProgramList", {}, ci::Pagination { 0, 1 }) && total == 3 && body.size() == 1 && body[0].getProperty ("title", {}) == var ("Organ"));
        CHECK (get ("ProgramList", {}, ci::Pagination { 5, 2 }) && total == 3 && body.size() == 0);

        // Korg's resources, as the Keystage Property Exchange Resource List has them: name, controlcc, default; currentValues of name, value, displayValue, displayUnit.
        CHECK (get ("X-ParameterList", {}, std::nullopt) && body.size() == 8 && total == -1
               && body[0].getProperty ("name", {}) == var ("Cutoff") && body[0].getProperty ("controlcc", {}) == var (102) && body[0].getProperty ("default", {}) == var (0)
               && body[7].getProperty ("controlcc", {}) == var (109));
        CHECK (get ("X-ProgramEdit", {}, std::nullopt) && body.getProperty ("currentValues", {}).size() == 8);
        {
            const auto v0 = body.getProperty ("currentValues", {})[0];
            CHECK (v0.getProperty ("name", {}) == var ("Cutoff") && v0.getProperty ("value", {}) == var (51)
                   && v0.getProperty ("displayValue", {}) == var ("40%") && v0.hasProperty ("displayUnit"));
        }
        // JSONSchema (M2-105 5): the schemas the ResourceList refers to, by resId.
        CHECK (get ("JSONSchema", "parameterListSchema", std::nullopt) && body.getProperty ("type", {}) == var ("array")
               && body.getProperty ("items", {}).getProperty ("properties", {}).hasProperty ("controlcc"));
        CHECK (get ("JSONSchema", "programEditSchema", std::nullopt) && body.getProperty ("properties", {}).hasProperty ("currentValues"));
        // Not there: 404, with a word of why (M2-103 7.4).
        CHECK (! get ("JSONSchema", "nothing", std::nullopt) && status == 404 && message.isNotEmpty());
        CHECK (! get ("X-Nothing", {}, std::nullopt) && status == 404 && message.isNotEmpty());
        {
            // StageRig's properties are not the keyboard's to set.
            ci::PropertyRequestHeader h;
            h.resource = "ChannelList";
            const auto text = ci::Encodings::jsonTo7BitText (PropertyResponder::channelList (pv));
            int setStatus = 0;
            keyboard->sendPropertySetInquiry (m, h, text, [&] (const ci::PropertyExchangeResult& r) { if (! r.getError().has_value()) setStatus = r.getHeaderAsReplyHeader().status; });
            settleCi();
            CHECK (setStatus == 405);
        }

        // Subscriptions (M2-103 11): each gets its subscribeId (8 characters at most), which names it in every update.
        std::map<String, ci::SubscriptionKey> keys;
        std::map<String, String> subIds;
        for (const auto* resource : { "ChannelList", "ProgramList", "X-ParameterList", "X-ProgramEdit" })
        {
            ci::PropertySubscriptionHeader h;
            h.resource = resource;
            h.command = ci::PropertySubscriptionCommand::start;
            keys.emplace (resource, keyboard->beginSubscription (m, h));
            settleCi();
            const auto id = keyboard->getSubscribeIdForKey (keys.at (resource));
            CHECK (id.has_value() && id->isNotEmpty() && id->length() <= 8);
            subIds[resource] = id.value_or (String());
        }
        CHECK (subIds["ChannelList"] != subIds["X-ProgramEdit"]);
        {
            // One not said to be subscribable is refused: no subscribeId.
            ci::PropertySubscriptionHeader h;
            h.resource = "DeviceInfo";
            h.command = ci::PropertySubscriptionCommand::start;
            const auto key = keyboard->beginSubscription (m, h);
            settleCi();
            CHECK (keyboard->getSubscribeIdForKey (key).value_or (String()).isEmpty());
        }
        auto updatesOf = [&] (const String& command, const String& id)
        {
            std::vector<var> bodies;
            for (const auto& u : heard.updates)
                if (u.command == command && u.subscribeId == id) bodies.push_back (u.body);
            return bodies;
        };
        auto countOf = [&] (const String& id) { int n = 0; for (const auto& u : heard.updates) n += u.subscribeId == id; return n; };

        // A program change goes whole (full) to the ChannelList's subscriber; the program list, read a page at a time, is only said to have changed (notify); the knobs, unchanged, are not mentioned.
        heard.updates.clear();
        pv.program = 9; pv.programName = "Strings";
        pv.programs = { { 0, "Organ" }, { 9, "Strings" } };
        rig.show (pv);
        settleCi();
        std::printf ("     MIDI-CI: %d updates after a program change\n", (int) heard.updates.size());
        {
            const auto full = updatesOf ("full", subIds["ChannelList"]);
            CHECK (full.size() == 1 && countOf (subIds["ChannelList"]) == 1
                   && full[0][0].getProperty ("programTitle", {}) == var ("Strings") && full[0][0].getProperty ("bankPC", {})[2] == var (9));
            CHECK (updatesOf ("notify", subIds["ProgramList"]).size() == 1 && countOf (subIds["ProgramList"]) == 1);
            CHECK (countOf (subIds["X-ParameterList"]) == 0 && countOf (subIds["X-ProgramEdit"]) == 0);
        }

        // A knob the keyboard itself turned is not echoed (its screen shows it already) ...
        heard.updates.clear();
        rig.knobTurnedOnKeyboard (0);
        pv.knobValues[0] = "45%"; pv.knobLevels[0] = 0.45f;
        rig.show (pv);
        settleCi();
        CHECK (heard.updates.empty());
        // ... one moved elsewhere is told, whole, since the eight knobs fit the keyboard's SysEx ...
        pv.knobValues[1] = "70%"; pv.knobLevels[1] = 0.7f;
        rig.show (pv);
        settleCi();
        {
            const auto full = updatesOf ("full", subIds["X-ProgramEdit"]);
            CHECK (full.size() == 1 && heard.updates.size() == 1 && full[0].getProperty ("currentValues", {})[1].getProperty ("value", {}) == var (89));
        }
        // ... a new knob name tells the knobs again, whole ...
        heard.updates.clear();
        pv.knobNames[1] = "Res";
        rig.show (pv);
        settleCi();
        {
            const auto full = updatesOf ("full", subIds["X-ParameterList"]);
            CHECK (full.size() == 1 && full[0][1].getProperty ("name", {}) == var ("Res") && full[0][1].getProperty ("controlcc", {}) == var (103));
            CHECK (updatesOf ("full", subIds["X-ProgramEdit"]).size() == 1 && countOf (subIds["ChannelList"]) == 0 && countOf (subIds["ProgramList"]) == 0);
        }
        // ... and names too long for one SysEx message are only said to have changed (notify), for the keyboard to ask again (M2-103 11.1.1).
        heard.updates.clear();
        for (int k = 0; k < SurfaceView::numKnobs; ++k) pv.knobNames[(size_t) k] = String::repeatedString ("Resonance ", 12).trim() + String (k);
        rig.show (pv);
        settleCi();
        CHECK (updatesOf ("notify", subIds["X-ParameterList"]).size() == 1 && updatesOf ("full", subIds["X-ParameterList"]).empty());
        CHECK (updatesOf ("notify", subIds["X-ProgramEdit"]).size() == 1 && updatesOf ("full", subIds["X-ProgramEdit"]).empty());
        CHECK (get ("X-ParameterList", {}, std::nullopt) && body[7].getProperty ("name", {}).toString().endsWith ("Resonance7"));

        // The keyboard ends one subscription (M2-103 11.5): that resource is no longer told, the others still are.
        keyboard->endSubscription (keys.at ("X-ProgramEdit"));
        settleCi();
        heard.updates.clear();
        pv.knobValues[2] = "10%"; pv.knobLevels[2] = 0.1f;
        rig.show (pv);
        settleCi();
        CHECK (heard.updates.empty());
        pv.program = 0; pv.programName = "Organ";
        rig.show (pv);
        settleCi();
        CHECK (countOf (subIds["ChannelList"]) == 1 && countOf (subIds["X-ProgramEdit"]) == 0);
        CHECK (rig.getLog().joinIntoString ("\n").contains ("no longer told of X-ProgramEdit"));

        // The keyboard goes away (Invalidate MUID): every subscription ends with it, and nothing more is sent its way.
        keyboard.reset();
        settleCi();
        CHECK (rig.getLog().joinIntoString ("\n").contains ("the keyboard left"));
        pv.program = 9; pv.programName = "Strings";
        rig.show (pv);
        CHECK (toKeyboard.empty());
    }
    return failures - before;
}

/* STAGERIG_TEST_MIDI2=1: what this computer offers for MIDI 2.0 -- every
   MIDI endpoint, the protocol it speaks and its MIDI inputs -- and whether a
   MIDI 2.0 keyboard can be made in software, as the tests make one. */
static int probeMidi2()
{
    namespace ump = juce::universal_midi_packets;
    auto* endpoints = ump::Endpoints::getInstance();
    const auto backend = endpoints->getBackend();
    std::printf ("midi2: backend %d, virtual MIDI 2.0 service %d\n", backend.has_value() ? (int) *backend : -1,
                 (int) endpoints->isVirtualMidiUmpServiceActive());
    auto list = [endpoints]
    {
        for (const auto& id : endpoints->getEndpoints())
        {
            const auto ep = endpoints->getEndpoint (id);
            const auto info = endpoints->getStaticDeviceInfo (id);
            const auto protocol = ep.has_value() ? ep->getProtocol() : std::nullopt;
            std::printf ("midi2: %-40s protocol %s, MIDI 2.0 support %d\n",
                         (ep.has_value() ? ep->getName() : info.has_value() ? info->getName() : String ("?")).toRawUTF8(),
                         ! protocol.has_value() ? "?" : *protocol == ump::PacketProtocol::MIDI_2_0 ? "2.0" : "1.0",
                         ep.has_value() ? (int) ep->hasMidi2Support() : -1);
            if (info.has_value())
                for (const auto& legacy : info->getLegacyIdentifiersSrc())
                    if (legacy.isNotEmpty())
                        std::printf ("midi2:     input %s, heard as MIDI 2.0: %d\n", legacy.toRawUTF8(), (int) Midi2Inputs::isMidi2 (legacy));
        }
    };
    list();
    auto session = endpoints->makeSession ("StageRig MIDI 2 probe");
    std::printf ("midi2: session %d\n", (int) session.isAlive());
    const auto block = ump::Block{}.withName ("Keys").withFirstGroup (0).withNumGroups (1).withEnabled (true)
                                   .withDirection (ump::BlockDirection::bidirectional);
    auto keys = session.createVirtualEndpoint ("StageRig MIDI 2 probe keys", ump::DeviceInfo{}, "stagerig-probe-1",
                                               ump::PacketProtocol::MIDI_2_0, Span<const ump::Block> (&block, 1), ump::BlocksAreStatic::yes);
    std::printf ("midi2: virtual MIDI 2.0 keyboard %d\n", (int) keys.isAlive());
    if (keys.isAlive())
    {
        MessageManager::getInstance()->runDispatchLoopUntil (500);
        list();
    }
    return 0;
}

int main()
{
    ScopedJuceInitialiser_GUI init;
    if (const char* m = std::getenv ("STAGERIG_TEST_MIDI2"); m != nullptr && *m != 0)
        return probeMidi2();
    if (const char* m = std::getenv ("STAGERIG_TEST_MIDICI"); m != nullptr && *m != 0)
        return checkMidiCi() == 0 ? 0 : 1;
    if (const char* k = std::getenv ("STAGERIG_TEST_KONTROL_SECONDS"); k != nullptr && *k != 0)
        return probeKompleteKontrol (String (k).getIntValue());
    if (const char* n = std::getenv ("STAGERIG_TEST_NKS"); n != nullptr && *n != 0)
        return probeNks (String (n));

    // Own scratch folder: PluginHost keeps its crash list ("dead man's pedal") next to
    // the settings file, and a plain temp file would share /tmp with every other run,
    // so an aborted run could blacklist a plugin for the next one.
    const File scratchDir = File::getSpecialLocation (File::tempDirectory)
                                .getChildFile ("StageRigEngineTest-" + String (Time::currentTimeMillis()));
    scratchDir.createDirectory();
    struct ScratchCleanup { File d; ~ScratchCleanup() { d.deleteRecursively(); } } scratchCleanup { scratchDir };
    TemporaryFile settingsTmp (scratchDir.getChildFile ("test.settings"));
    PropertiesFile::Options opts;
    opts.applicationName = "StageRigEngineTest";
    opts.filenameSuffix = "settings";
    opts.folderName = "StageRigEngineTest";
    opts.storageFormat = PropertiesFile::storeAsXML;
    PropertiesFile settings (settingsTmp.getFile(), opts);

    PluginHost host (settings);

    // --- scan LV2 plugins and pick an instrument -------------------------------
    AudioPluginFormat* lv2 = nullptr;
    for (auto* f : host.getFormatManager().getFormats())
        if (f->getName() == "LV2") lv2 = f;
    CHECK (lv2 != nullptr);
    if (lv2 == nullptr) return 1;

    auto ids = lv2->searchPathsForPlugins (lv2->getDefaultLocationsToSearch(), true, false);
    ids.removeDuplicates (false);        // LV2 reports the bundle path once per plugin
    std::printf ("     %d LV2 bundles found\n", ids.size());
    for (auto& id : ids)
        if (id.containsIgnoreCase ("calf"))
        {
            OwnedArray<PluginDescription> found;
            host.getKnownPlugins().scanAndAddFile (id, true, found, *lv2);
        }

    PluginDescription synth;
    bool haveSynth = false;
    for (auto& d : host.getKnownPlugins().getTypes())
        if (d.isInstrument && d.name.containsIgnoreCase ("Monosynth")) { synth = d; haveSynth = true; }
    if (! haveSynth)
        for (auto& d : host.getKnownPlugins().getTypes())
            if (d.isInstrument) { synth = d; haveSynth = true; break; }
    CHECK (haveSynth);
    if (! haveSynth) { std::printf ("SKIP: no LV2 instrument available (install calf-plugins on Debian/Ubuntu, lv2-calf-plugins on Fedora)\n"); return 77; }
    std::printf ("     using instrument: %s (%s)\n", synth.name.toRawUTF8(), synth.fileOrIdentifier.toRawUTF8());

    // --- engine ------------------------------------------------------------------
    Engine engine (host, settings, /*startAudioDevice*/ false);
    TestListener listener;
    engine.addListener (&listener);
    engine.setReleaseTailSeconds (0.1);

    // Plugins load on a background thread; wait for them (pumping the message loop so
    // the loaded processes get attached) before checking anything that needs them.
    auto settle = [&engine]
    {
        const auto t0 = Time::getMillisecondCounterHiRes();
        pump (30);
        while (engine.hasPendingLoads() && Time::getMillisecondCounterHiRes() - t0 < 120000.0) pump (30);
    };

    CHECK (engine.getSetup().inputs.size() == 2);         // Upper (ch1), Lower (ch2)
    CHECK (engine.isProgramLoaded (0, 0));

    CHECK (RemotePlugin::findHostExecutable().existsAsFile());
    std::printf ("     plugin host: %s\n", RemotePlugin::findHostExecutable().getFullPathName().toRawUTF8());

    String err;
    CHECK (engine.addSlot (0, 0, synth, err));
    settle();
    if (err.isNotEmpty()) std::printf ("     addSlot error: %s\n", err.toRawUTF8());
    auto* inst = engine.getPlugin (0, 0, 0);
    CHECK (inst != nullptr && inst->isAlive());
    if (inst == nullptr) return 1;
    std::printf ("     plugin '%s' pid-hosted, params=%d\n", inst->getName().toRawUTF8(), (int) inst->getParameters().size());

    // Silence before any note.
    const float silence = render (engine, 4);
    CHECK (silence < 1e-4f);

    // --- a note held through a program change keeps sounding until it is let go ------------
    {
        engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
        render (engine, 4);
        engine.selectProgram (0, 1); settle();                       // program 1 is empty
        const float heldAfterChange = render (engine, 24);           // about a quarter of a second later
        std::printf ("     held through a change: rms %.3f\n", heldAfterChange);
        CHECK (heldAfterChange > 0.02f);
        engine.injectMidi (0, MidiMessage::noteOff (1, 60));
        render (engine, 40);                                         // release, then the 0.1 s tail
        CHECK (render (engine, 4) < 1e-4f);

        // Hold off: the change releases everything, and only the tail remains.
        engine.setHoldAcrossChange (false);
        engine.selectProgram (0, 0); settle();
        engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
        render (engine, 4);
        engine.selectProgram (0, 1); settle();
        render (engine, 24);
        CHECK (render (engine, 4) < 1e-4f);
        engine.injectMidi (0, MidiMessage::noteOff (1, 60));
        engine.setHoldAcrossChange (true);
        engine.selectProgram (0, 0); settle();
        render (engine, 10);
    }

    // --- note on channel 1 -> Upper sounds -----------------------------------------
    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
    const float sounding = render (engine, 20);
    std::printf ("     rms while note held: %f\n", sounding);
    CHECK (sounding > 0.01f);

    engine.injectMidi (0, MidiMessage::noteOff (1, 60));
    render (engine, 200);                                    // ~2.3 s of release
    const float afterRelease = render (engine, 4);
    std::printf ("     rms after release: %f\n", afterRelease);
    CHECK (afterRelease < sounding * 0.5f);

    // --- channel filter: ch 2 must not reach Upper (ch 1) ---------------------------
    engine.injectMidi (0, MidiMessage::noteOn (2, 60, (uint8) 100));
    CHECK (render (engine, 20) < 1e-4f);
    engine.injectMidi (0, MidiMessage::noteOff (2, 60));

    // --- key range + transpose ------------------------------------------------------
    engine.setSlotKeyRange (0, 0, 0, 60, 72);
    engine.injectMidi (0, MidiMessage::noteOn (1, 48, (uint8) 100));   // below range
    CHECK (render (engine, 20) < 1e-4f);
    engine.injectMidi (0, MidiMessage::noteOff (1, 48));
    engine.setSlotKeyRange (0, 0, 0, 0, 127);

    // --- Program Change channel ---------------------------------------------------------
    // Input 0 plays on channel 1. A keyboard may send Program Change somewhere else
    // entirely (a Jupiter-50 sends registrations on 16 while playing on 1/3/4).
    {
        engine.selectProgram (0, 0); settle();
        const int start = engine.getSetup().inputs[0].currentProgram;
        CHECK (start == 0);

        // default (same as notes): a change on the note channel selects, one elsewhere does not
        engine.injectMidi (0, MidiMessage::programChange (1, 5)); pump (40);
        CHECK (engine.getSetup().inputs[0].currentProgram == 5);
        engine.injectMidi (0, MidiMessage::programChange (16, 9)); pump (40);
        CHECK (engine.getSetup().inputs[0].currentProgram == 5);      // ignored: wrong channel

        // a specific channel: only that one is accepted
        engine.setInputProgramChangeChannel (0, 16);
        engine.injectMidi (0, MidiMessage::programChange (1, 7)); pump (40);
        CHECK (engine.getSetup().inputs[0].currentProgram == 5);      // the note channel no longer selects
        engine.injectMidi (0, MidiMessage::programChange (16, 9)); pump (40);
        CHECK (engine.getSetup().inputs[0].currentProgram == 9);
        std::printf ("     program change on ch16 while playing ch1: program %d\n", engine.getSetup().inputs[0].currentProgram);

        // any channel
        engine.setInputProgramChangeChannel (0, InputDef::pcChannelAny);
        engine.injectMidi (0, MidiMessage::programChange (4, 3)); pump (40);
        CHECK (engine.getSetup().inputs[0].currentProgram == 3);

        // notes still obey the note channel, whatever the PC channel is
        engine.setInputProgramChangeChannel (0, 16);
        engine.selectProgram (0, 0); settle();
        engine.injectMidi (0, MidiMessage::noteOn (16, 60, (uint8) 100));   // wrong channel for notes
        CHECK (render (engine, 20) < 1e-4f);
        engine.injectMidi (0, MidiMessage::noteOff (16, 60));
        engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));    // the note channel
        CHECK (render (engine, 20) > 0.01f);
        engine.injectMidi (0, MidiMessage::noteOff (1, 60)); render (engine, 60);

        // respondToProgramChange still wins over any of this
        engine.setInputRespondToProgramChange (0, false);
        engine.injectMidi (0, MidiMessage::programChange (16, 11)); pump (40);
        CHECK (engine.getSetup().inputs[0].currentProgram == 0);
        engine.setInputRespondToProgramChange (0, true);
        engine.setInputProgramChangeChannel (0, InputDef::pcChannelSameAsNotes);
    }

    // --- velocity range, velocity curve, pan -----------------------------------------
    engine.setSlotVelocityRange (0, 0, 0, 90, 127);
    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 50));    // too soft for this layer
    CHECK (render (engine, 20) < 1e-4f);
    engine.injectMidi (0, MidiMessage::noteOff (1, 60));
    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 110));   // inside
    CHECK (render (engine, 20) > 0.01f);
    engine.injectMidi (0, MidiMessage::noteOff (1, 60));
    render (engine, 100);
    engine.setSlotVelocityRange (0, 0, 0, 1, 127);

    CHECK (Engine::curveVelocity (127, 0.7f) == 127 && Engine::curveVelocity (1, -0.7f) >= 1);
    CHECK (Engine::curveVelocity (64, 0.0f) == 64);
    CHECK (Engine::curveVelocity (64, 1.0f) > 100 && Engine::curveVelocity (64, -1.0f) < 20);
    for (int v = 2; v <= 127; ++v) CHECK (Engine::curveVelocity (v, 0.5f) >= Engine::curveVelocity (v - 1, 0.5f));   // monotonic

    {
        float l = 0, r = 0;
        Engine::panGains (0.0f, l, r);  CHECK (l == 1.0f && r == 1.0f);
        Engine::panGains (-1.0f, l, r); CHECK (l == 1.0f && r == 0.0f);
        Engine::panGains (0.5f, l, r);  CHECK (std::abs (l - 0.5f) < 1e-6f && r == 1.0f);
        // Hard left: the right channel of the mix must be silent while the left sounds.
        engine.setSlotPan (0, 0, 0, -1.0f);
        engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
        AudioBuffer<float> buf (2, 512);
        double sl = 0, sr = 0;
        for (int b = 0; b < 20; ++b)
        {
            buf.clear();
            float* outs[2] = { buf.getWritePointer (0), buf.getWritePointer (1) };
            engine.renderBlockForTesting (outs, 2, 512);
            sl += buf.getRMSLevel (0, 0, 512); sr += buf.getRMSLevel (1, 0, 512);
        }
        std::printf ("     pan hard left: L %.4f  R %.4f\n", sl / 20, sr / 20);
        CHECK (sl / 20 > 0.005 && sr / 20 < 1e-5);
        engine.injectMidi (0, MidiMessage::noteOff (1, 60));
        engine.setSlotPan (0, 0, 0, 0.0f);
        render (engine, 100);
    }

    // --- slot disabled -> silent ------------------------------------------------------
    engine.setSlotEnabled (0, 0, 0, false);
    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
    CHECK (render (engine, 20) < 1e-4f);
    engine.injectMidi (0, MidiMessage::noteOff (1, 60));
    engine.setSlotEnabled (0, 0, 0, true);
    render (engine, 100);

    // --- CC mapping -----------------------------------------------------------------
    const ParamInfo* target = nullptr;
    for (auto& p : inst->getParameters())
        if (p.automatable && ! p.discrete && p.name.containsIgnoreCase ("cutoff")) { target = &p; break; }
    if (target == nullptr)
        for (auto& p : inst->getParameters())
            if (p.automatable && ! p.discrete) { target = &p; break; }
    CHECK (target != nullptr);
    if (target != nullptr)
    {
        std::printf ("     mapping CC 74 -> '%s' (id %s)\n", target->name.toRawUTF8(), target->id.toRawUTF8());
        MappingDef m;
        m.source = MappingDef::Source::CC; m.number = 74; m.slot = 0;
        m.paramId = target->id;
        m.minValue = 0.25f; m.maxValue = 0.75f;
        engine.addMapping (0, 0, m);
        CHECK (engine.getSetup().inputs[0].programs[0].mappings.size() == 1);

        // Values are read back from the plugin process, not from the host's cache.
        float v = -1.0f;
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 74, 127));
        render (engine, 2);
        CHECK (inst->fetchParameterValue (target->index, v));
        std::printf ("     param after CC=127: %f\n", v);
        CHECK (std::abs (v - 0.75f) < 0.02f);

        engine.injectMidi (0, MidiMessage::controllerEvent (1, 74, 0));
        render (engine, 2);
        CHECK (inst->fetchParameterValue (target->index, v));
        std::printf ("     param after CC=0: %f\n", v);
        CHECK (std::abs (v - 0.25f) < 0.02f);

        // A different CC is not affected by the mapping.
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 75, 127));
        render (engine, 2);
        CHECK (inst->fetchParameterValue (target->index, v));
        CHECK (std::abs (v - 0.25f) < 0.02f);

        // The spare controllers a MIDI-CI keyboard is told to send are its knobs only while it is there.
        listener.knobTurned = -1;
        engine.injectMidi (0, MidiMessage::controllerEvent (1, SurfaceControls::autoKnobCC + 2, 90));
        pump (50);
        CHECK (listener.knobTurned == -1);
        engine.setInputAutoKnobs (0, true);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, SurfaceControls::autoKnobCC + 2, 90));
        pump (50);
        CHECK (listener.knobTurned == 2 && listener.knobValue == 90);

        // A knob taught a spare controller keeps it; the untaught knob whose spare that was is heard on the
        // next free one -- the same answer the keyboard is told (SurfaceControls::heardCC), so a turn of it arrives.
        {
            auto c = engine.getSetup().inputs[0].controls;
            c.knobCC[1] = SurfaceControls::autoKnobCC + 2;
            engine.setInputControls (0, c);
            CHECK (engine.keyboardKnobFor (0, SurfaceControls::autoKnobCC + 2) == 1);
            CHECK (engine.keyboardKnobFor (0, SurfaceControls::autoKnobCC + 3) == 2);     // its spare is taken: the next one
            CHECK (engine.keyboardKnobFor (0, SurfaceControls::autoKnobCC + 4) == 3);     // and the ones after shift along
            listener.knobTurned = -1;
            engine.injectMidi (0, MidiMessage::controllerEvent (1, SurfaceControls::autoKnobCC + 3, 70));
            pump (50);
            CHECK (listener.knobTurned == 2 && listener.knobValue == 70);
            c.knobCC[1] = 0;
            engine.setInputControls (0, c);
        }
        engine.setInputAutoKnobs (0, false);

        // MIDI 2.0: what the plugins get as MIDI 1.0, and the full value beside it.
        {
            namespace ump = juce::universal_midi_packets;
            ump::SingleGroupMidi1ToBytestreamTranslator translator (64);
            std::vector<std::pair<MidiMessage, int64>> got;
            auto keep = [&got] (const MidiMessage& mm, int64 exact) { got.emplace_back (mm, exact); };
            const auto cc = ump::Factory::makeControlChangeV2 (0, 2, 74, 0x9abcdef0u);
            Midi2Inputs::translate (ump::View (cc.data()), 0.0, translator, keep);
            CHECK (got.size() == 1 && got[0].first.isController() && got[0].first.getChannel() == 3 && got[0].first.getControllerNumber() == 74
                   && got[0].first.getControllerValue() == (0x9abcdef0 >> 25) && got[0].second == 0x9abcdef0);
            got.clear();
            const auto bend = ump::Factory::makePitchBendV2 (0, 0, 0x80000000u);
            Midi2Inputs::translate (ump::View (bend.data()), 0.0, translator, keep);
            CHECK (got.size() == 1 && got[0].first.isPitchWheel() && got[0].first.getPitchWheelValue() == 8192 && got[0].second == 0x80000000);
            got.clear();
            const auto note = ump::Factory::makeNoteOnV2 (0, 0, 60, ump::Factory::NoteAttributeKind::none, 0x8000, 0);
            Midi2Inputs::translate (ump::View (note.data()), 0.0, translator, keep);
            CHECK (got.size() == 1 && got[0].first.isNoteOn() && got[0].first.getVelocity() == 64 && got[0].second == -1);
        }

        /* MIDI 2.0, packet by packet, with the words written out: everything a
           keyboard sends, checked against the UMP specification's default
           translation (JUCE's Conversion::midi2ToMidi1DefaultTranslation, the
           same as the Linux kernel's ump_convert.c). A MIDI 2.0 channel voice
           word is 0x4 (type), group, status, channel, then two bytes (note or
           controller index, and a flag or attribute byte); the second word is
           the value. Narrowing to 7 or 14 bits is a shift: the top bits. */
        {
            namespace ump = juce::universal_midi_packets;
            ump::SingleGroupMidi1ToBytestreamTranslator translator (64);
            std::vector<std::pair<MidiMessage, int64>> got;
            auto keep = [&got] (const MidiMessage& mm, int64 exact) { got.emplace_back (mm, exact); };
            auto feed = [&] (std::vector<uint32> words) { got.clear(); Midi2Inputs::translate (ump::View (words.data()), 0.0, translator, keep); };
            auto exactOf = [] (int64 e) { return (float) ((double) e / 4294967295.0); };

            // Pitch bend, status 0xe: the centre 0x80000000 is 8192 and 0.5 to the digit, the ends are the ends.
            feed ({ 0x40E00000,       // type 4, group 0, pitch bend, channel 1, no index
                    0x80000000 });    // centre
            CHECK (got.size() == 1 && got[0].first.isPitchWheel() && got[0].first.getPitchWheelValue() == 8192 && got[0].second == 0x80000000 && exactOf (got[0].second) == 0.5f);
            feed ({ 0x40E00000, 0xFFFFFFFF });    // all the way up
            CHECK (got.size() == 1 && got[0].first.getPitchWheelValue() == 16383 && exactOf (got[0].second) == 1.0f);
            feed ({ 0x40E00000, 0x00000000 });    // all the way down
            CHECK (got.size() == 1 && got[0].first.getPitchWheelValue() == 0 && got[0].second == 0 && exactOf (got[0].second) == 0.0f);

            // A controller, status 0xb, index in the third byte: 32 bits to 7.
            feed ({ 0x40B04A00,       // CC 74 (0x4a) on channel 1
                    0xFFFFFFFF });
            CHECK (got.size() == 1 && got[0].first.isController() && got[0].first.getControllerNumber() == 74 && got[0].first.getControllerValue() == 127 && exactOf (got[0].second) == 1.0f);
            feed ({ 0x40B04A00, 0x80000000 });
            CHECK (got.size() == 1 && got[0].first.getControllerValue() == 64 && exactOf (got[0].second) == 0.5f);
            feed ({ 0x40B04A00, 0x00000000 });
            CHECK (got.size() == 1 && got[0].first.getControllerValue() == 0 && got[0].second == 0);
            // On another channel, group and controller: the channel is the low nibble of the second byte.
            feed ({ 0x43BF0100,       // group 3, CC 1 (mod wheel) on channel 16
                    0x12345678 });
            CHECK (got.size() == 1 && got[0].first.getChannel() == 16 && got[0].first.getControllerNumber() == 1 && got[0].first.getControllerValue() == (0x12345678 >> 25) && got[0].second == 0x12345678);

            // Channel pressure, status 0xd: the value is the second word, with its exact value.
            feed ({ 0x40D00000,       // channel pressure, channel 1
                    0xFFFFFFFF });
            CHECK (got.size() == 1 && got[0].first.isChannelPressure() && got[0].first.getChannelPressureValue() == 127 && exactOf (got[0].second) == 1.0f);
            feed ({ 0x40D00000, 0x80000000 });
            CHECK (got.size() == 1 && got[0].first.getChannelPressureValue() == 64 && exactOf (got[0].second) == 0.5f);

            // Note on, status 0x9, 16-bit velocity in the top half of the second word: no exact value, velocity to 7 bits.
            feed ({ 0x40903C00,       // note 60 (0x3c), attribute type 0 (none)
                    0xFFFF0000 });    // velocity 0xffff, no attribute
            CHECK (got.size() == 1 && got[0].first.isNoteOn() && got[0].first.getNoteNumber() == 60 && got[0].first.getVelocity() == 127 && got[0].second == -1);
            // In MIDI 2.0 a note on at velocity 0 is a note on, so it becomes velocity 1 and not a note off.
            feed ({ 0x40903C00, 0x00000000 });
            CHECK (got.size() == 1 && got[0].first.isNoteOn() && got[0].first.getVelocity() == 1);
            feed ({ 0x40903C00, 0x01FF0000 });    // velocity 0x01ff, under one 7-bit step: still a note on
            CHECK (got.size() == 1 && got[0].first.isNoteOn() && got[0].first.getVelocity() == 1);
            // Attribute type 3 (pitch 7.9) and type 1 (manufacturer specific): the note and velocity, the attribute set aside.
            feed ({ 0x40903C03,       // note 60, attribute type 3
                    0x80007A00 });    // velocity 0x8000, attribute 0x7a00 (pitch 61.0)
            CHECK (got.size() == 1 && got[0].first.isNoteOn() && got[0].first.getNoteNumber() == 60 && got[0].first.getVelocity() == 64 && got[0].first.getRawDataSize() == 3);
            feed ({ 0x40903C01, 0x4000ABCD });
            CHECK (got.size() == 1 && got[0].first.isNoteOn() && got[0].first.getVelocity() == 32);
            // Note off, status 0x8: release velocity scaled the same way.
            feed ({ 0x40803C00,       // note off 60
                    0x80000000 });    // release velocity 0x8000
            CHECK (got.size() == 1 && got[0].first.isNoteOff() && got[0].first.getNoteNumber() == 60 && got[0].first.getVelocity() == 64 && got[0].second == -1);

            // Poly pressure, status 0xa: 32 bits to 7, and nothing exact (StageRig keeps exact values per channel, not per note).
            feed ({ 0x40A03C00,       // poly pressure on note 60
                    0x80000000 });
            CHECK (got.size() == 1 && got[0].first.isAftertouch() && got[0].first.getNoteNumber() == 60 && got[0].first.getAfterTouchValue() == 64 && got[0].second == -1);

            // Registered controller (RPN), status 0x2: bank and index in the first word, 32-bit data in the second;
            // as MIDI 1.0 it is the four controllers 101, 100, 6, 38, in that order, none of them with an exact value.
            feed ({ 0x40200001,       // RPN bank 0, index 1 (fine tuning), channel 1
                    0x80000000 });    // 14-bit data 8192 = MSB 64, LSB 0
            CHECK (got.size() == 4 && got[0].first.getControllerNumber() == 101 && got[0].first.getControllerValue() == 0
                                   && got[1].first.getControllerNumber() == 100 && got[1].first.getControllerValue() == 1
                                   && got[2].first.getControllerNumber() == 6   && got[2].first.getControllerValue() == 64
                                   && got[3].first.getControllerNumber() == 38  && got[3].first.getControllerValue() == 0
                                   && got[0].second == -1 && got[1].second == -1 && got[2].second == -1 && got[3].second == -1);
            // Assignable controller (NRPN), status 0x3: controllers 99, 98, 6, 38.
            feed ({ 0x40300102,       // NRPN bank 1, index 2
                    0xFFFFFFFF });    // 14-bit data 16383
            CHECK (got.size() == 4 && got[0].first.getControllerNumber() == 99 && got[0].first.getControllerValue() == 1
                                   && got[1].first.getControllerNumber() == 98 && got[1].first.getControllerValue() == 2
                                   && got[2].first.getControllerNumber() == 6  && got[2].first.getControllerValue() == 127
                                   && got[3].first.getControllerNumber() == 38 && got[3].first.getControllerValue() == 127);
            // Relative registered and assignable controllers, status 0x4 and 0x5: nothing in MIDI 1.0 says this, so nothing is sent.
            feed ({ 0x40400001, 0x00000100 });
            CHECK (got.empty());
            feed ({ 0x40500102, 0xFFFFFF00 });
            CHECK (got.empty());
            // Per-note controllers (status 0x0 registered, 0x1 assignable), per-note pitch bend (0x6) and per-note
            // management (0xf): per note, which MIDI 1.0 has no words for; dropped, and never a channel controller.
            feed ({ 0x40003C01,       // registered per-note controller 1 on note 60
                    0x80000000 });
            CHECK (got.empty());
            feed ({ 0x40103C4A,       // assignable per-note controller 74 on note 60: not CC 74
                    0xFFFFFFFF });
            CHECK (got.empty());
            feed ({ 0x40603C00,       // per-note pitch bend on note 60
                    0x80000000 });
            CHECK (got.empty());
            feed ({ 0x40F03C01,       // per-note management, note 60, detach flag
                    0x00000000 });
            CHECK (got.empty());

            // Program change, status 0xc: with the bank valid flag (bit 0 of the first word) it is
            // Bank Select MSB (CC 0), LSB (CC 32), then the program; without, the program alone.
            feed ({ 0x40C00001,       // program change, channel 1, bank valid
                    0x05000102 });    // program 5, bank MSB 1, LSB 2
            CHECK (got.size() == 3 && got[0].first.isController() && got[0].first.getControllerNumber() == 0  && got[0].first.getControllerValue() == 1
                                   && got[1].first.isController() && got[1].first.getControllerNumber() == 32 && got[1].first.getControllerValue() == 2
                                   && got[2].first.isProgramChange() && got[2].first.getProgramChangeNumber() == 5 && got[2].second == -1);
            feed ({ 0x40C10000,       // program change, channel 2, no bank
                    0x07000000 });    // program 7
            CHECK (got.size() == 1 && got[0].first.isProgramChange() && got[0].first.getChannel() == 2 && got[0].first.getProgramChangeNumber() == 7);
            // The controllers MIDI 2.0 replaces with its own messages (0, 6, 32, 38, 98-101) are not passed on as controllers.
            feed ({ 0x40B00000, 0xFFFFFFFF });    // CC 0 as a plain controller
            CHECK (got.empty());
            feed ({ 0x40B00600, 0xFFFFFFFF });    // CC 6
            CHECK (got.empty());
            feed ({ 0x40B06500, 0xFFFFFFFF });    // CC 101
            CHECK (got.empty());
            // A malformed packet, its controller index with the top bit set: not MIDI, and goes no further.
            feed ({ 0x40B0CA00,       // CC "202"
                    0x12345678 });
            CHECK (got.empty());
            feed ({ 0x4090C000, 0xFFFF0000 });    // note "192"
            CHECK (got.empty());

            // MIDI 1.0 channel voice on a MIDI 2.0 endpoint, type 2: the bytes as they are, with no exact value.
            feed ({ 0x20B04A20 });    // type 2, group 0, CC 74 = 32
            CHECK (got.size() == 1 && got[0].first.isController() && got[0].first.getControllerNumber() == 74 && got[0].first.getControllerValue() == 32 && got[0].second == -1);
            feed ({ 0x20E00040 });    // pitch bend LSB 0, MSB 64: centre
            CHECK (got.size() == 1 && got[0].first.isPitchWheel() && got[0].first.getPitchWheelValue() == 8192 && got[0].second == -1);
            feed ({ 0x20903C64 });    // note on 60, velocity 100
            CHECK (got.size() == 1 && got[0].first.isNoteOn() && got[0].first.getVelocity() == 100 && got[0].second == -1);
            feed ({ 0x20D07F00 });    // channel pressure 127
            CHECK (got.size() == 1 && got[0].first.isChannelPressure() && got[0].first.getChannelPressureValue() == 127 && got[0].second == -1);

            // System real time, type 1: clock, start and stop come out as themselves (the engine, hearing no clock, lets them go).
            feed ({ 0x10F80000 });
            CHECK (got.size() == 1 && got[0].first.isMidiClock() && got[0].second == -1);
            feed ({ 0x10FA0000 });
            CHECK (got.size() == 1 && got[0].first.isMidiStart());
            feed ({ 0x10FC0000 });
            CHECK (got.size() == 1 && got[0].first.isMidiStop());
            // System exclusive, type 3: status (complete / begin / continue / end) and byte count in the third nibbles,
            // the data in the bytes after. One in a packet, then one over two packets, each a whole message.
            feed ({ 0x30047E7F,       // complete, 4 bytes: 7e 7f
                    0x09010000 });    //                    09 01
            CHECK (got.size() == 1 && got[0].first.isSysEx() && got[0].first.getSysExDataSize() == 4 && got[0].first.getSysExData()[2] == 0x09);
            feed ({ 0x30167E7F,       // begin, 6 bytes: 7e 7f
                    0x0D710100 });    //                 0d 71 01 00
            CHECK (got.empty());
            feed ({ 0x30320102,       // end, 2 bytes: 01 02
                    0x00000000 });
            CHECK (got.size() == 1 && got[0].first.isSysEx() && got[0].first.getSysExDataSize() == 8 && got[0].first.getSysExData()[7] == 0x02);
            // What has no MIDI 1.0 form at all: a utility NOOP (type 0), SysEx8 (type 5), flex data (type 0xd) and a stream message (type 0xf).
            feed ({ 0x00000000 });
            CHECK (got.empty());
            feed ({ 0x50030100, 0x7E7F0900, 0, 0 });
            CHECK (got.empty());
            feed ({ 0xD0100000, 0, 0, 0 });
            CHECK (got.empty());
            feed ({ 0xF0000000, 0, 0, 0 });
            CHECK (got.empty());
        }

        /* The same through the engine: the exact value reaches the keyboard's
           knobs, a mapping on pressure and on pitch bend, and what cannot be
           played stops short of the plugins. */
        {
            namespace ump = juce::universal_midi_packets;
            auto inject = [&] (std::vector<uint32> words) { engine.injectMidi2 (0, ump::View (words.data())); };

            // A knob the keyboard is told to send as a spare controller: its place in 24 bits, 0..1, beside the 7-bit value.
            engine.setInputAutoKnobs (0, true);
            listener.knobTurned = -1; listener.knobExact = -2.0f;
            inject ({ 0x40B00000u | (uint32) (SurfaceControls::autoKnobCC + 2) << 8, 0xFFFFFFFF });    // CC of knob 3 on channel 1, all the way up
            pump (50);
            CHECK (listener.knobTurned == 2 && listener.knobValue == 127 && listener.knobExact == 1.0f);
            inject ({ 0x40B00000u | (uint32) (SurfaceControls::autoKnobCC + 2) << 8, 0x80000000 });    // half way
            pump (50);
            CHECK (listener.knobValue == 64 && std::abs (listener.knobExact - 0.5f) < 1e-6f);
            inject ({ 0x40B00000u | (uint32) (SurfaceControls::autoKnobCC + 2) << 8, 0x00000000 });    // down
            pump (50);
            CHECK (listener.knobValue == 0 && listener.knobExact == 0.0f);
            // From MIDI 1.0 there is nothing exact to give.
            engine.injectMidi (0, MidiMessage::controllerEvent (1, SurfaceControls::autoKnobCC + 2, 90));
            pump (50);
            CHECK (listener.knobValue == 90 && listener.knobExact < 0.0f);
            engine.setInputAutoKnobs (0, false);

            // Channel pressure mapped to the parameter: a slow press within one 7-bit step still moves it.
            {
                MappingDef pm;
                pm.source = MappingDef::Source::ChannelPressure; pm.slot = 0; pm.paramId = target->id;
                pm.minValue = 0.25f; pm.maxValue = 0.75f;
                engine.addMapping (0, 0, pm);
                auto press = [&] (uint32 value)
                {
                    inject ({ 0x40D00000, value });    // channel pressure, channel 1
                    render (engine, 2);
                    float now = -1.0f;
                    CHECK (inst->fetchParameterValue (target->index, now));
                    return now;
                };
                const float low = press (0x41000000u), high = press (0x41ffffffu);
                const float wantLow = 0.25f + 0.5f * (float) (0x41000000u / 4294967295.0), wantHigh = 0.25f + 0.5f * (float) (0x41ffffffu / 4294967295.0);
                std::printf ("     MIDI 2.0 pressure: within one MIDI 1.0 step the parameter went %.5f -> %.5f (wanted %.5f -> %.5f)\n", low, high, wantLow, wantHigh);
                CHECK (std::abs (low - wantLow) < 2e-4f && std::abs (high - wantHigh) < 2e-4f && high > low + 0.001f);
                engine.removeMapping (0, 0, (int) engine.getSetup().inputs[0].programs[0].mappings.size() - 1);
            }
            // Pitch bend mapped to the parameter, 0..1: MIDI 1.0's centre is 8192 of 16383, a hair over half; MIDI 2.0's is half to the digit.
            {
                MappingDef bm;
                bm.source = MappingDef::Source::PitchBend; bm.slot = 0; bm.paramId = target->id;
                bm.minValue = 0.0f; bm.maxValue = 1.0f;
                engine.addMapping (0, 0, bm);
                auto read = [&] { render (engine, 2); float now = -1.0f; CHECK (inst->fetchParameterValue (target->index, now)); return now; };
                engine.injectMidi (0, MidiMessage::pitchWheel (1, 8192));
                const float centre1 = read();
                inject ({ 0x40E00000, 0x80000000 });    // pitch bend, channel 1, centre
                const float centre2 = read();
                inject ({ 0x40E00000, 0xC0000000 });    // three quarters up
                const float threeQuarters = read();
                std::printf ("     pitch bend centre: MIDI 1.0 %.6f, MIDI 2.0 %.6f; three quarters %.6f\n", centre1, centre2, threeQuarters);
                CHECK (std::abs (centre1 - 8192.0f / 16383.0f) < 1e-5f && std::abs (centre2 - 0.5f) < 1e-5f && std::abs (threeQuarters - 0.75f) < 1e-5f);
                engine.removeMapping (0, 0, (int) engine.getSetup().inputs[0].programs[0].mappings.size() - 1);
            }

            // Clock, a system exclusive, a per-note controller and a malformed controller: taken in, nothing to show for them.
            render (engine, 1);
            float before = -1.0f;
            CHECK (inst->fetchParameterValue (target->index, before));
            inject ({ 0x10F80000 });                    // clock
            inject ({ 0x10FA0000 });                    // start
            inject ({ 0x30047E7F, 0x09010000 });        // sysex
            inject ({ 0x40103C4A, 0xFFFFFFFF });        // per-note controller 74: not CC 74
            inject ({ 0x40B0CA00, 0x12345678 });        // CC "202"
            inject ({ 0x50030100, 0x7E7F0900, 0, 0 });  // sysex8
            render (engine, 2);
            float still = -1.0f;
            CHECK (inst->fetchParameterValue (target->index, still) && still == before);    // CC 74's mapping, untouched by any of them
        }

        /* A slow turn on a MIDI 2.0 keyboard, within one of MIDI 1.0's 128
           steps (0x41000000 and 0x41ffffff are both 32 of 127), still moves
           the plugin's parameter, while the plugin is sent MIDI 1.0. */
        {
            namespace ump = juce::universal_midi_packets;
            auto turn2 = [&] (uint32 value)
            {
                const auto p = ump::Factory::makeControlChangeV2 (0, 0, 74, value);
                engine.injectMidi2 (0, ump::View (p.data()));
                render (engine, 2);
                float now = -1.0f;
                CHECK (inst->fetchParameterValue (target->index, now));
                return now;
            };
            const float low = turn2 (0x41000000u), high = turn2 (0x41ffffffu);
            const float wantLow = 0.25f + 0.5f * (float) (0x41000000u / 4294967295.0), wantHigh = 0.25f + 0.5f * (float) (0x41ffffffu / 4294967295.0);
            std::printf ("     MIDI 2.0: within one MIDI 1.0 step the parameter went %.5f -> %.5f (wanted %.5f -> %.5f)\n", low, high, wantLow, wantHigh);
            CHECK (std::abs (low - wantLow) < 2e-4f && std::abs (high - wantHigh) < 2e-4f && high > low + 0.001f);
            // The same controller from MIDI 1.0 afterwards is its own 7 bits, not the last exact value.
            engine.injectMidi (0, MidiMessage::controllerEvent (1, 74, 100));
            render (engine, 2);
            float seven = -1.0f;
            CHECK (inst->fetchParameterValue (target->index, seven) && std::abs (seven - (0.25f + 0.5f * 100.0f / 127.0f)) < 2e-4f);
        }

        /* A keyboard speaking MIDI 2.0, made in software (where the system's
           ALSA can make one): heard through its own MIDI 2.0 connection. */
        {
            namespace ump = juce::universal_midi_packets;
            auto session = ump::Endpoints::getInstance()->makeSession ("StageRig test");
            const auto block = ump::Block{}.withName ("Keys").withFirstGroup (0).withNumGroups (1).withEnabled (true)
                                           .withDirection (ump::BlockDirection::bidirectional);
            auto keys = session.createVirtualEndpoint ("StageRig MIDI 2 test keys", ump::DeviceInfo{}, "stagerig-test-1",
                                                       ump::PacketProtocol::MIDI_2_0, Span<const ump::Block> (&block, 1), ump::BlocksAreStatic::yes);
            String id;
            for (int tries = 0; tries < 40 && keys.isAlive() && id.isEmpty(); ++tries)
            {
                pump (50);
                if (const auto info = ump::Endpoints::getInstance()->getStaticDeviceInfo (keys.getId()))
                    id = info->getLegacyIdentifiersSrc()[0];
            }
            if (! keys.isAlive() || id.isEmpty())
                std::printf ("     MIDI 2.0: no virtual MIDI 2.0 keyboard can be made here; skipped\n");
            else
            {
                const auto before = engine.getSetup().inputs[0];
                engine.setInputMidiDevice (0, MidiDeviceInfo ("StageRig MIDI 2 test keys", id));
                std::printf ("     MIDI 2.0: test keyboard %s, MIDI 2.0 %d, heard as MIDI 2.0 %d\n", id.toRawUTF8(),
                             (int) Engine::isMidi2Device (id), (int) engine.isInputHeardAsMidi2 (0));
                CHECK (Engine::isMidi2Device (id) && engine.isInputHeardAsMidi2 (0));
                auto out = session.connectOutput (keys.getId());
                auto turn = [&] (uint32 value)
                {
                    ump::Packets p;
                    p.add (ump::Factory::makeControlChangeV2 (0, 0, 74, value));
                    out.send (p.begin(), p.end());
                    pump (100);
                    render (engine, 2);
                    float now = -1.0f;
                    CHECK (inst->fetchParameterValue (target->index, now));
                    return now;
                };
                // 0x41000000 and 0x41ffffff are both 32 of 127 in MIDI 1.0.
                const float low = turn (0x41000000u), high = turn (0x41ffffffu);
                const float wantLow = 0.25f + 0.5f * (float) (0x41000000u / 4294967295.0), wantHigh = 0.25f + 0.5f * (float) (0x41ffffffu / 4294967295.0);
                std::printf ("     MIDI 2.0: within one MIDI 1.0 step the parameter went %.5f -> %.5f (wanted %.5f -> %.5f)\n", low, high, wantLow, wantHigh);
                CHECK (std::abs (low - wantLow) < 2e-4f && std::abs (high - wantHigh) < 2e-4f && high > low + 0.001f);

                // Heard as MIDI 1.0 at the input's asking: the same turn is one step.
                engine.setInputMidi1Only (0, true);
                CHECK (! engine.isInputHeardAsMidi2 (0));
                const float low1 = turn (0x41000000u), high1 = turn (0x41ffffffu);
                std::printf ("     MIDI 2.0 heard as MIDI 1.0: %.5f -> %.5f\n", low1, high1);
                CHECK (std::abs (low1 - high1) < 1e-5f && std::abs (low1 - (0.25f + 0.5f * 32.0f / 127.0f)) < 2e-4f);
                engine.setInputMidi1Only (0, false);
                engine.setInputMidiDevice (0, MidiDeviceInfo (before.midiDeviceName, before.midiDeviceIdentifier));
            }
        }
    }

    // --- a pedal on the instrument itself: on / off, toggled, and a level ------------------
    {
        MappingDef onOff;
        onOff.source = MappingDef::Source::CC; onOff.number = 80; onOff.slot = 0; onOff.paramId = "#on"; onOff.paramName = "On / off";
        engine.addMapping (0, 0, onOff);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 80, 0));
        render (engine, 2);
        pump (50);
        std::printf ("     CC 80 = 0 -> instrument enabled %d\n", (int) engine.getSetup().inputs[0].programs[0].slots[0].enabled);
        CHECK (! engine.getSetup().inputs[0].programs[0].slots[0].enabled);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 80, 127));
        render (engine, 2);
        pump (50);
        CHECK (engine.getSetup().inputs[0].programs[0].slots[0].enabled);
        engine.removeMapping (0, 0, (int) engine.getSetup().inputs[0].programs[0].mappings.size() - 1);

        MappingDef tog = onOff; tog.number = 81; tog.paramId = "#toggle";
        engine.addMapping (0, 0, tog);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 81, 127));           // press: off
        render (engine, 2);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 81, 0));             // release: nothing
        render (engine, 2);
        pump (50);
        CHECK (! engine.getSetup().inputs[0].programs[0].slots[0].enabled);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 81, 127));           // press again: on
        render (engine, 2);
        pump (50);
        std::printf ("     CC 81 pressed twice -> instrument enabled %d\n", (int) engine.getSetup().inputs[0].programs[0].slots[0].enabled);
        CHECK (engine.getSetup().inputs[0].programs[0].slots[0].enabled);
        engine.removeMapping (0, 0, (int) engine.getSetup().inputs[0].programs[0].mappings.size() - 1);

        MappingDef lev = onOff; lev.number = 82; lev.paramId = "#level";
        engine.addMapping (0, 0, lev);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 82, 64));            // halfway up the fader: about -24 dB
        render (engine, 2);
        pump (50);
        std::printf ("     CC 82 = 64 -> level %.1f dB\n", engine.getSetup().inputs[0].programs[0].slots[0].gainDb);
        CHECK (std::abs (engine.getSetup().inputs[0].programs[0].slots[0].gainDb + 24.0f) < 1.5f);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 82, 106));           // 0 dB
        render (engine, 2);
        pump (50);
        CHECK (std::abs (engine.getSetup().inputs[0].programs[0].slots[0].gainDb) < 1.0f);
        engine.removeMapping (0, 0, (int) engine.getSetup().inputs[0].programs[0].mappings.size() - 1);
        engine.setSlotGainDb (0, 0, 0, 0.0f);
    }

    // --- the looper: a bar of playing goes round on its own ---------------------------------
    {
        auto peakBlock = [&]
        {
            AudioBuffer<float> b (2, kBlock);
            b.clear();
            engine.renderBlockForTesting (b.getArrayOfWritePointers(), 2, kBlock);
            return b.getMagnitude (0, kBlock);
        };
        engine.setTempoBpm (120.0);                                      // a bar is two seconds
        const int blocksPerBar = (int) std::ceil (engine.getSampleRate() * 2.0 / kBlock);
        CHECK (engine.getLoopState (0) == Engine::LoopState::empty);
        engine.loopPedal (0);                                            // record
        render (engine, 1);
        CHECK (engine.getLoopState (0) == Engine::LoopState::recording);
        engine.injectMidi (0, MidiMessage::noteOn (1, 64, (uint8) 100));
        render (engine, 10);
        engine.injectMidi (0, MidiMessage::noteOff (1, 64));
        render (engine, blocksPerBar - 11);
        engine.injectMidi (0, MidiMessage::noteOn (1, 67, (uint8) 100));  // a second note, still down when the loop closes
        render (engine, 2);
        engine.loopPedal (0);                                            // play: one bar, the nearest
        render (engine, 1);
        engine.injectMidi (0, MidiMessage::noteOff (1, 67));
        std::printf ("     loop: state %d, %d bar(s)\n", (int) engine.getLoopState (0), engine.getLoopBars (0));
        CHECK (engine.getLoopState (0) == Engine::LoopState::playing && engine.getLoopBars (0) == 1);
        // Round it goes: in the second pass the note sounds again with nothing played.
        render (engine, blocksPerBar - 2);
        float loud = 0.0f;
        for (int b = 0; b < 12; ++b) loud = jmax (loud, peakBlock());
        std::printf ("     the loop playing the note again: peak %.3f\n", loud);
        CHECK (loud > 0.01f);
        /* The note that was still down when the loop closed has its note-off at
           the loop's last sample now, so each pass lets it go: at the end of a
           pass the program is quiet again rather than holding it for ever. */
        render (engine, blocksPerBar - 14);
        float seam = 0.0f;
        for (int b = 0; b < 2; ++b) seam = jmax (seam, peakBlock());
        render (engine, 8);
        float afterSeam = 0.0f;
        for (int b = 0; b < 2; ++b) afterSeam = jmax (afterSeam, peakBlock());
        std::printf ("     around the seam: %.3f then %.3f (the held note is let go, the loop's note restarts)\n", seam, afterSeam);
        engine.loopStop (0);
        render (engine, 30);
        float quiet = 0.0f;
        for (int b = 0; b < 4; ++b) quiet = jmax (quiet, peakBlock());
        std::printf ("     stopped: peak %.4f\n", quiet);
        CHECK (engine.getLoopState (0) == Engine::LoopState::stopped && quiet < 0.01f);
        engine.loopClear (0);
        render (engine, 1);
        CHECK (engine.getLoopState (0) == Engine::LoopState::empty && engine.getLoopBars (0) == 0);

        // An input added while running is made the same way as one loaded: its looper works too.
        const int added = engine.addInput ("Added while running");
        engine.loopPedal (added);
        render (engine, 1);
        CHECK (engine.getLoopState (added) == Engine::LoopState::recording);
        engine.loopClear (added);
        render (engine, 1);
        engine.removeInput (added);
    }

    // --- learn ------------------------------------------------------------------------
    engine.setLearnArmed (true);
    engine.injectMidi (0, MidiMessage::controllerEvent (1, 11, 64));
    pump (50);
    CHECK (! engine.isLearnArmed());
    CHECK (listener.learnInput == 0 && listener.learnSource == MappingDef::Source::CC && listener.learnNumber == 11);

    // --- program change via MIDI ------------------------------------------------------
    engine.injectMidi (0, MidiMessage::programChange (1, 5));
    pump (50);
    CHECK (listener.programChangedInput == 0 && listener.programChangedTo == 5);
    CHECK (engine.getSetup().inputs[0].currentProgram == 5);
    CHECK (engine.isProgramLoaded (0, 5));
    CHECK (engine.isProgramLoaded (0, 0));                  // still releasing
    render (engine, 20);                                     // > 0.1 s tail
    pump (1600);                                             // housekeeping tick unloads it
    CHECK (! engine.isProgramLoaded (0, 0));
    // Definition of program 0 survives unloading, with captured plugin state.
    CHECK (engine.getSetup().inputs[0].programs[0].slots.size() == 1);
    CHECK (engine.getSetup().inputs[0].programs[0].slots[0].state.getSize() > 0);

    // Program 5 is empty: notes produce silence.
    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
    CHECK (render (engine, 10) < 1e-4f);
    engine.injectMidi (0, MidiMessage::noteOff (1, 60));

    // Switching back reloads program 0 from its definition and it plays again.
    engine.selectProgram (0, 0);
    settle();
    CHECK (engine.isProgramLoaded (0, 0));
    CHECK (engine.isPluginAlive (0, 0, 0));
    CHECK (engine.getSetup().inputs[0].programs[0].mappings.size() == 1);
    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
    const float again = render (engine, 20);
    std::printf ("     rms after reload: %f\n", again);
    CHECK (again > 0.01f);
    engine.injectMidi (0, MidiMessage::noteOff (1, 60));

    // --- preload mode loads every used program ----------------------------------------
    CHECK (engine.addSlot (0, 7, synth, err));               // program 7 not loaded: def only
    CHECK (! engine.isProgramLoaded (0, 7));
    engine.setPreloadAllPrograms (true);
    pump (1600);
    settle();
    CHECK (engine.isProgramLoaded (0, 7));
    CHECK (engine.isPluginAlive (0, 7, 0));

    // --- copy / clear / removeSlot ------------------------------------------------------
    engine.copyProgram (0, 0, 9);
    settle();
    CHECK (engine.getSetup().inputs[0].programs[9].slots.size() == 1);
    CHECK (engine.getSetup().inputs[0].programs[9].mappings.size() == 1);
    engine.clearProgram (0, 9);
    settle();
    CHECK (engine.getSetup().inputs[0].programs[9].isEmpty());
    engine.removeSlot (0, 0, 0);
    CHECK (engine.getSetup().inputs[0].programs[0].slots.empty());
    CHECK (engine.getSetup().inputs[0].programs[0].mappings.empty());   // mapping to removed slot gone
    CHECK (render (engine, 4) >= 0.0f);                       // still renders fine

    // --- panic + save round trip ----------------------------------------------------------
    engine.panic();
    render (engine, 2);
    TemporaryFile setupTmp (".stagerig.json");
    CHECK (engine.captureSetup().saveToFile (setupTmp.getFile()).wasOk());
    Setup reloaded;
    CHECK (Setup::loadFromFile (setupTmp.getFile(), reloaded).wasOk());
    CHECK (reloaded.inputs[0].programs[7].slots.size() == 1 && reloaded.inputs[0].programs[7].slots[0].plugin.name == synth.name);
    CHECK (reloaded.preloadAllPrograms);

    // --- effect chains ----------------------------------------------------------------------
    {
        // Pick a Calf effect with a controllable input level; fall back to any effect.
        PluginDescription fx; bool haveFx = false;
        for (auto& d : host.getKnownPlugins().getTypes())
            if (! d.isInstrument && d.name.containsIgnoreCase ("Stereo Tools")) { fx = d; haveFx = true; }
        if (! haveFx)
            for (auto& d : host.getKnownPlugins().getTypes())
                if (! d.isInstrument && d.name.containsIgnoreCase ("Compressor")) { fx = d; haveFx = true; break; }
        CHECK (haveFx);
        if (haveFx)
        {
            std::printf ("     using effect: %s\n", fx.name.toRawUTF8());
            engine.setPreloadAllPrograms (false);
            engine.selectProgram (0, 0);
            settle();
            CHECK (engine.addSlot (0, 0, synth, err));
            settle();
            CHECK (engine.isPluginAlive (0, 0, 0));

            // Baseline level, instrument only.
            engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
            render (engine, 10);
            const float dry = render (engine, 10);
            std::printf ("     dry rms: %f\n", dry);
            CHECK (dry > 0.01f);

            // Slot insert chain: audio must still pass with the effect in place.
            CHECK (engine.addEffect (0, 0, 0, fx, err));
            settle();
            if (err.isNotEmpty()) std::printf ("     addEffect error: %s\n", err.toRawUTF8());
            CHECK (engine.getSetup().inputs[0].programs[0].slots[0].effects.size() == 1);
            auto* fxInst = engine.getPlugin (0, 0, 0, 0);
            CHECK (fxInst != nullptr && fxInst->isAlive());
            render (engine, 5);
            const float wet = render (engine, 10);
            std::printf ("     rms through slot effect: %f\n", wet);
            CHECK (wet > 0.01f);

            // Find a level/gain parameter and turn it down: output must drop.
            const ParamInfo* level = nullptr;
            if (fxInst != nullptr)
                for (auto& p : fxInst->getParameters())
                {
                    const auto n = p.name;
                    if (n.containsIgnoreCase ("Level In") || n.containsIgnoreCase ("Input Gain") || n.containsIgnoreCase ("Input Level")) { level = &p; break; }
                }
            if (level == nullptr && fxInst != nullptr)
                for (auto& p : fxInst->getParameters())
                    if (p.name.containsIgnoreCase ("Level Out") || p.name.containsIgnoreCase ("Output")) { level = &p; break; }
            CHECK (level != nullptr);
            if (level != nullptr)
            {
                std::printf ("     effect level parameter: '%s'\n", level->name.toRawUTF8());
                const String levelId = level->id;

                // Via a mapping to the effect (slot 0, effect 0): CC 7 -> level, min 0.
                MappingDef m;
                m.source = MappingDef::Source::CC; m.number = 7; m.slot = 0; m.effect = 0;
                m.paramId = levelId; m.minValue = 0.0f; m.maxValue = 1.0f;
                engine.addMapping (0, 0, m);
                engine.injectMidi (0, MidiMessage::controllerEvent (1, 7, 0));
                render (engine, 5);
                const float muted = render (engine, 10);
                std::printf ("     rms with effect level at min: %f\n", muted);
                CHECK (muted < wet * 0.2f);

                // Bypass restores the dry signal even though the level is still down.
                engine.setEffectBypassed (0, 0, 0, 0, true);
                CHECK (engine.getSetup().inputs[0].programs[0].slots[0].effects[0].bypassed);
                render (engine, 5);
                const float bypassed = render (engine, 10);
                std::printf ("     rms with effect bypassed: %f\n", bypassed);
                CHECK (bypassed > 0.01f);
                engine.setEffectBypassed (0, 0, 0, 0, false);

                // Removing the effect drops its mapping and leaves the instrument alone.
                engine.removeEffect (0, 0, 0, 0);
                level = nullptr;   // its process is gone with the effect
                CHECK (engine.getSetup().inputs[0].programs[0].slots[0].effects.empty());
                CHECK (engine.getSetup().inputs[0].programs[0].mappings.empty());
                render (engine, 5);
                CHECK (render (engine, 10) > 0.01f);

                // Program-level chain: same effect after the mix, mapped via slot == -1.
                CHECK (engine.addEffect (0, 0, -1, fx, err));
                settle();
                CHECK (engine.getSetup().inputs[0].programs[0].effects.size() == 1);
                auto* progFx = engine.getPlugin (0, 0, -1, 0);
                CHECK (progFx != nullptr && progFx->isAlive());
                if (progFx != nullptr)
                {
                    const int pLevel = progFx->findParameterIndex (levelId);
                    CHECK (pLevel >= 0);
                    float unity = 0.0f;
                    if (pLevel >= 0) progFx->fetchParameterValue (pLevel, unity);
                    MappingDef pm = m; pm.slot = -1; pm.effect = 0; pm.number = 8;
                    engine.addMapping (0, 0, pm);
                    render (engine, 5);
                    CHECK (render (engine, 10) > 0.01f);
                    engine.injectMidi (0, MidiMessage::controllerEvent (1, 8, 0));
                    render (engine, 5);
                    const float progMuted = render (engine, 10);
                    std::printf ("     rms with program effect level at min: %f\n", progMuted);
                    CHECK (progMuted < wet * 0.2f);
                    if (pLevel >= 0) progFx->setParameterValue (pLevel, unity);   // back to unity gain
                    render (engine, 2);
                }

                // Reordering: add a second program effect, move it first, mapping follows.
                CHECK (engine.addEffect (0, 0, -1, fx, err));
                settle();
                engine.moveEffect (0, 0, -1, 1, 0);
                CHECK (engine.getSetup().inputs[0].programs[0].mappings.size() == 1);
                CHECK (engine.getSetup().inputs[0].programs[0].mappings[0].effect == 1);   // the mapped one moved to index 1
                engine.removeEffect (0, 0, -1, 0);
                CHECK (engine.getSetup().inputs[0].programs[0].mappings[0].effect == 0);
            }

            engine.injectMidi (0, MidiMessage::noteOff (1, 60));
            render (engine, 20);

            // Effects survive a save/load round trip, including on the program chain.
            TemporaryFile fxTmp (".stagerig.json");
            CHECK (engine.captureSetup().saveToFile (fxTmp.getFile()).wasOk());
            Setup rl;
            CHECK (Setup::loadFromFile (fxTmp.getFile(), rl).wasOk());
            CHECK (rl.inputs[0].programs[0].effects.size() == 1);
            CHECK (rl.inputs[0].programs[0].effects[0].plugin.name == fx.name);
            CHECK (rl.inputs[0].programs[0].mappings.size() == 1 && rl.inputs[0].programs[0].mappings[0].slot == -1);

            // Loading that setup rebuilds the chain live.
            engine.loadSetup (rl);
            settle();
            CHECK (engine.isPluginAlive (0, 0, -1, 0));
            engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
            render (engine, 10);
            const float afterLoad = render (engine, 10);
            std::printf ("     rms after reloading setup with chain: %f\n", afterLoad);
            CHECK (afterLoad > 0.01f);
            CHECK (afterLoad < dry * 1.5f);   // effect state (unity gain) survived the round trip
            engine.injectMidi (0, MidiMessage::noteOff (1, 60));
            render (engine, 20);
        }
    }

    // --- mapping suggestions ------------------------------------------------------------------
    {
        engine.setPreloadAllPrograms (false);
        engine.selectProgram (0, 30);
        settle();
        CHECK (engine.addSlot (0, 30, synth, err));
        settle();
        auto* mono = engine.getPlugin (0, 30, 0);
        CHECK (mono != nullptr && mono->isAlive());
        if (mono != nullptr)
        {
            auto byNames = suggestMappingsFromNames (mono->getParameters());
            std::printf ("     %d name-based suggestions for %s:\n", (int) byNames.size(), synth.name.toRawUTF8());
            for (auto& sg : byNames)
                std::printf ("       %-12s -> %-28s (%s)\n", sg.mapping.sourceDescription().toRawUTF8(), sg.mapping.paramName.toRawUTF8(), sg.reason.toRawUTF8());

            auto find = [&] (int cc) -> const MappingSuggestion* { for (auto& sg : byNames) if (sg.mapping.number == cc) return &sg; return nullptr; };
            CHECK (find (74) != nullptr && find (74)->mapping.paramName.containsIgnoreCase ("cutoff"));
            CHECK (find (71) != nullptr && find (71)->mapping.paramName.containsIgnoreCase ("reso"));
            CHECK (find (73) != nullptr && find (73)->mapping.paramName.containsIgnoreCase ("attack"));
            CHECK (find (72) != nullptr && find (72)->mapping.paramName.containsIgnoreCase ("release"));
            CHECK (find (5)  != nullptr && find (5)->mapping.paramName.containsIgnoreCase ("portamento"));
            CHECK (find (1) == nullptr);                          // mod wheel is left to the plugin

            // No CC and no parameter is used twice.
            std::set<int> ccs; std::set<String> ids;
            for (auto& sg : byNames) { ccs.insert (sg.mapping.number); ids.insert (sg.mapping.paramId); }
            CHECK (ccs.size() == byNames.size() && ids.size() == byNames.size());

            // Full pipeline without a template: same set, targeted at slot 0, skipping existing ones.
            auto& templates = host.getMappingTemplates();
            CHECK (! templates.has (synth));
            std::vector<MappingDef> existing;
            existing.push_back (find (74)->mapping); existing.back().slot = 0; existing.back().effect = -1;
            auto pipeline = suggestMappings (mono->getParameters(), synth, templates, 0, -1, existing);
            CHECK (pipeline.size() == byNames.size() - 1);
            for (auto& sg : pipeline) { CHECK (sg.mapping.slot == 0 && sg.mapping.effect == -1); CHECK (sg.mapping.number != 74); }

            // Template: save two mappings, one of them for a parameter that doesn't exist.
            std::vector<MappingDef> tmpl;
            MappingDef a; a.number = 20; a.paramId = find (74)->mapping.paramId; a.minValue = 0.1f; a.maxValue = 0.9f;
            MappingDef b; b.number = 21; b.paramId = "sym:does_not_exist";
            tmpl.push_back (a); tmpl.push_back (b);
            templates.set (synth, tmpl);
            CHECK (templates.has (synth));
            auto fromTemplate = suggestMappings (mono->getParameters(), synth, templates, 2, 1, {});
            CHECK (fromTemplate.size() == 1);
            if (fromTemplate.size() == 1)
            {
                CHECK (fromTemplate[0].mapping.number == 20 && fromTemplate[0].mapping.slot == 2 && fromTemplate[0].mapping.effect == 1);
                CHECK (std::abs (fromTemplate[0].mapping.minValue - 0.1f) < 1e-6f);
                CHECK (fromTemplate[0].mapping.paramName.containsIgnoreCase ("cutoff"));
            }
            // Persisted: a fresh instance reads the same file.
            MappingTemplates reloaded (settings.getFile().getSiblingFile ("mapping-templates.json"));
            CHECK (reloaded.has (synth) && reloaded.get (synth).size() == 2);
            templates.remove (synth);
            CHECK (! templates.has (synth));
        }
        engine.clearProgram (0, 30);
        settle();
    }

    // --- helper environment -----------------------------------------------------------------
    {
        std::vector<char*> envp;
        auto env = RemotePlugin::buildHelperEnvironment (envp);
        CHECK (envp.size() == env.size() + 1 && envp.back() == nullptr);
        String path;
        int pathEntries = 0;
        for (auto& e : env) if (String (e).startsWith ("PATH=")) { path = String (e).substring (5); ++pathEntries; }
        CHECK (pathEntries == 1);
        const auto localBin = File::getSpecialLocation (File::userHomeDirectory).getChildFile (".local/bin");
        if (localBin.getChildFile ("wine").existsAsFile())
        {
            std::printf ("     ~/.local/bin/wine shim present: PATH starts with %s\n", StringArray::fromTokens (path, ":", {})[0].toRawUTF8());
            CHECK (StringArray::fromTokens (path, ":", {})[0] == localBin.getFullPathName());
        }
        for (auto& e : env) if (String (e).startsWith ("WINELOADER=")) std::printf ("     %s\n", e.c_str());
    }

    // --- scanner robustness -----------------------------------------------------------------
    {
        using HR = PluginHost::HelperResult;
        // A helper that never finishes must be killed at the timeout, not waited for.
        String out;
        auto t0 = Time::getMillisecondCounterHiRes();
        const auto r1 = PluginHost::runHelperWithTimeout (File ("/bin/sleep"), StringArray { "30" }, 600, 100, [] { return false; }, out);
        const auto ms1 = Time::getMillisecondCounterHiRes() - t0;
        std::printf ("     hung helper: result %d after %.0f ms\n", (int) r1, ms1);
        CHECK (r1 == HR::timedOut && ms1 < 2000.0);

        // A stop request (dialog closing) gives the child a grace period, then kills it.
        t0 = Time::getMillisecondCounterHiRes();
        const auto r2 = PluginHost::runHelperWithTimeout (File ("/bin/sleep"), StringArray { "30" }, 60000, 300,
                                                          [t0] { return Time::getMillisecondCounterHiRes() - t0 > 250.0; }, out);
        const auto ms2 = Time::getMillisecondCounterHiRes() - t0;
        std::printf ("     stopped helper: result %d after %.0f ms\n", (int) r2, ms2);
        CHECK (r2 == HR::stopped && ms2 > 500.0 && ms2 < 2000.0);

        // A stop request while the child is about to finish still yields its output.
        t0 = Time::getMillisecondCounterHiRes();
        const auto r4 = PluginHost::runHelperWithTimeout (File ("/bin/sh"), StringArray { "-c", "sleep 0.5; echo late-result" }, 60000, 5000,
                                                          [] { return true; }, out);
        CHECK (r4 == HR::finished && out.trim() == "late-result");

        // Output is collected completely from a process that finishes.
        const auto r3 = PluginHost::runHelperWithTimeout (File ("/bin/echo"), StringArray { "hello", "scanner" }, 5000, 100, [] { return false; }, out);
        CHECK (r3 == HR::finished && out.trim() == "hello scanner");

        // A hard stop (application exiting) ignores the grace period.
        t0 = Time::getMillisecondCounterHiRes();
        const auto r5 = PluginHost::runHelperWithTimeout (File ("/bin/sleep"), StringArray { "30" }, 60000, 45000, [] { return false; }, out,
                                                          [t0] { return Time::getMillisecondCounterHiRes() - t0 > 200.0; });
        const auto ms5 = Time::getMillisecondCounterHiRes() - t0;
        std::printf ("     hard-stopped helper: result %d after %.0f ms\n", (int) r5, ms5);
        CHECK (r5 == HR::stopped && ms5 < 1500.0);

        // Nothing left behind.
        ChildProcess pgrep;
        pgrep.start (StringArray { "pgrep", "-P", String ((int) ::getpid()), "sleep" });
        CHECK (pgrep.readAllProcessOutput().trim().isEmpty());

        // Real bridged VST3s, if this machine has them (yabridge + Wine): replay exactly
        // what PluginListComponent does -- four pool threads, and as soon as one runs
        // out of files the pool is torn down with removeAllJobs (true, ...), which asks
        // the still-running probes to exit. Nothing may end up blacklisted.
        // Any yabridge bundles this machine has; skipped where there are none.
        File yabridgeDir (File::getSpecialLocation (File::userHomeDirectory).getChildFile (".vst3/yabridge"));
        if (yabridgeDir.isDirectory())
        {
            AudioPluginFormat* vst3 = nullptr;
            for (auto* fmt : host.getFormatManager().getFormats()) if (fmt->getName() == "VST3") vst3 = fmt;

            /* Plugins the rig's owner switched off (Remove in the Plugins window
               puts them on the rig's blacklist) stay off here too: a bridged
               plugin with no licence puts a dialog on the desktop each time it
               is probed. STAGERIG_TEST_SKIP_PLUGINS names more, by substring,
               separated by colons. */
            StringArray skipped;
            {
                File settingsFile = File::getSpecialLocation (File::userApplicationDataDirectory).getChildFile ("StageRig/StageRig.settings");
                if (const char* dir = std::getenv ("STAGERIG_SETTINGS_DIR"); dir != nullptr && *dir != 0) settingsFile = File (dir).getChildFile ("StageRig.settings");
                if (auto xml = parseXML (settingsFile))
                    for (auto* v : xml->getChildWithTagNameIterator ("VALUE"))
                        if (v->getStringAttribute ("name") == "pluginList")
                            if (auto* kp = v->getChildByName ("KNOWNPLUGINS"))
                                for (auto* b : kp->getChildWithTagNameIterator ("BLACKLISTED")) skipped.add (b->getStringAttribute ("id"));
                if (const char* more = std::getenv ("STAGERIG_TEST_SKIP_PLUGINS"); more != nullptr && *more != 0)
                    for (const auto& part : StringArray::fromTokens (String (more), ":", ""))
                        for (const auto& bundle : yabridgeDir.findChildFiles (File::findFilesAndDirectories, true, "*.vst3"))
                            if (part.isNotEmpty() && bundle.getFileName().containsIgnoreCase (part)
                                && (bundle.getParentDirectory().getFileName() == "yabridge" || bundle.getParentDirectory().getParentDirectory().getFileName() == "yabridge"))
                                skipped.addIfNotAlreadyThere (bundle.getFullPathName());     // the bundle, not the DLL inside it
            }
            for (const auto& f : skipped) std::printf ("     (skipping %s: switched off on this rig)\n", File (f).getFileName().toRawUTF8());

            KnownPluginList list;
            list.setCustomScanner (std::make_unique<PluginHost::OutOfProcessScanner>());
            for (const auto& f : skipped) list.addToBlacklist (f);
            TemporaryFile dmp (".txt");
            PluginDirectoryScanner ds (list, *vst3, FileSearchPath (yabridgeDir.getFullPathName()), true, dmp.getFile(), false);
            /* Bundles whose Windows plugin is currently missing (being reinstalled)
               can't load. Counted recursively, because the scanner searches that
               way: yabridge puts a vendor folder beside the bundles when a plugin
               installs into one (Steinberg/HALion Sonic.vst3), and counting only
               the top level made the scanner look like it had found one too many. */
            int expected = 0;
            for (const auto& bundle : yabridgeDir.findChildFiles (File::findFilesAndDirectories, true, "*.vst3"))
            {
                /* A VST3 bundle is a directory that itself ends in .vst3 and
                   contains Contents/, so a recursive search also turns up the
                   Windows DLL inside each one. Those are not plugins to scan. */
                if (bundle.getParentDirectory().getFileName() != "yabridge"
                    && bundle.getParentDirectory().getParentDirectory().getFileName() != "yabridge")
                    continue;

                if (skipped.contains (bundle.getFullPathName())) continue;
                if (PluginHost::brokenBridgeTarget (bundle.getFullPathName()).isEmpty()) ++expected;
                else std::printf ("     (skipping %s: bridge target missing)\n", bundle.getFileName().toRawUTF8());
            }

            struct Job : public ThreadPoolJob
            {
                Job (PluginDirectoryScanner& s, std::atomic<bool>& f) : ThreadPoolJob ("scan"), ds (s), finished (f) {}
                JobStatus runJob() override
                {
                    String name;
                    while (! finished.load() && ds.scanNextFile (true, name) && ! shouldExit()) {}
                    finished.store (true);
                    return jobHasFinished;
                }
                PluginDirectoryScanner& ds; std::atomic<bool>& finished;
            };
            std::atomic<bool> finished { false };
            t0 = Time::getMillisecondCounterHiRes();
            {
                ThreadPool pool (ThreadPoolOptions{}.withNumberOfThreads (4));
                for (int i = 0; i < 4; ++i) pool.addJob (new Job (ds, finished), true);
                while (! finished.load()) Thread::sleep (20);
                pool.removeAllJobs (true, 60000);        // what ~Scanner does
            }
            std::printf ("     dialog-style scan of %s: %d plugin(s) expected, %d found, %d blacklisted, %d failed, %.1f s\n",
                         yabridgeDir.getFullPathName().toRawUTF8(), expected, list.getNumTypes(),
                         list.getBlacklistedFiles().size(), ds.getFailedFiles().size(), (Time::getMillisecondCounterHiRes() - t0) / 1000.0);
            for (auto& b : list.getBlacklistedFiles()) if (! skipped.contains (b)) std::printf ("       BLACKLISTED %s\n", b.toRawUTF8());
            // A probe stopped by the teardown may legitimately yield nothing (it will be
            // scanned next time); what must never happen is a blacklist entry.
            CHECK (list.getBlacklistedFiles().size() == skipped.size());
            /* One bundle can hold more than one plugin -- a VST3 "shell" exposes
               several, and at least one here does -- so the count found may
               exceed the number of bundles. What matters is that nothing went
               missing: every bundle produced something, or is accounted for as
               a failure. */
            CHECK (list.getNumTypes() + ds.getFailedFiles().size() >= expected);
            CHECK (list.getNumTypes() >= 1);

            // The app-owned background scanner (what the Plugins window uses).
            auto& sc = host.getScanner();
            for (const auto& f : skipped) host.getKnownPlugins().addToBlacklist (f);
            CHECK (! sc.isScanning());
            CHECK (sc.startScan (*vst3, FileSearchPath (yabridgeDir.getFullPathName())));
            CHECK (sc.isScanning());
            CHECK (! sc.startScan (*vst3, FileSearchPath (yabridgeDir.getFullPathName())));   // busy
            t0 = Time::getMillisecondCounterHiRes();
            double lastReport = 0.0;
            /* Long enough for a real library. Each bridged plugin is a Wine
               subprocess that can take a second or more, so a hundred of them
               is minutes, not seconds -- and a timeout here reads as a scanner
               that stopped working rather than one that was still going. */
            const double scanBudgetMs = jmax (120000.0, 3000.0 * (double) expected);
            while (sc.isScanning() && Time::getMillisecondCounterHiRes() - t0 < scanBudgetMs)
            {
                pump (50);
                const auto el = Time::getMillisecondCounterHiRes() - t0;
                if (el - lastReport > 5000.0)
                {
                    lastReport = el;
                    std::printf ("       ... %.0f s: progress %.2f, current '%s'\n", el / 1000.0, sc.getProgress(), sc.getCurrentFile().toRawUTF8());
                }
            }
            int fromYabridge = 0;
            for (auto& d : host.getKnownPlugins().getTypes()) if (d.fileOrIdentifier.startsWith (yabridgeDir.getFullPathName())) ++fromYabridge;
            std::printf ("     background scanner: %d/%d yabridge plugins, %d newly blacklisted, %d failed, %.1f s\n",
                         fromYabridge, expected, sc.getNewlyBlacklistedFiles().size(), sc.getFailedFiles().size(), (Time::getMillisecondCounterHiRes() - t0) / 1000.0);
            CHECK (! sc.isScanning());
            CHECK (sc.getNewlyBlacklistedFiles().isEmpty());
            CHECK (fromYabridge >= expected);      // shells can exceed one per bundle
        }
    }

    // --- crash isolation --------------------------------------------------------------------
    {
        engine.setPreloadAllPrograms (false);
        engine.selectProgram (0, 40);
        settle();
        CHECK (engine.addSlot (0, 40, synth, err));
        settle();
        auto* victim = engine.getPlugin (0, 40, 0);
        CHECK (victim != nullptr && victim->isAlive());
        if (victim != nullptr && victim->isAlive())
        {
            /* No host may hold another host's control socket. The pair is made
               on one loader thread while others are spawning; a copy that
               leaked into a sibling would keep the socket from ever reaching
               end-of-file when its owner dies. */
            {
                ChildProcess pgrep;
                pgrep.start (StringArray { "pgrep", "-P", String ((int) ::getpid()), "-f", "plugin-host" });
                auto hosts = StringArray::fromLines (pgrep.readAllProcessOutput().trim());
                auto socketInodes = [] (const String& pidText)
                {
                    std::set<String> out;
                    for (auto& f : File ("/proc/" + pidText.trim() + "/fd").findChildFiles (File::findFiles, false))
                    {
                        char buf[64] = {};
                        const auto n = ::readlink (f.getFullPathName().toRawUTF8(), buf, sizeof (buf) - 1);
                        if (n > 0 && String (buf).startsWith ("socket:")) out.insert (String (buf));
                    }
                    return out;
                };
                int shared = 0;
                for (auto& a : hosts)
                {
                    char buf[64] = {};
                    const auto n = ::readlink (("/proc/" + a.trim() + "/fd/3").toRawUTF8(), buf, sizeof (buf) - 1);
                    if (n <= 0) continue;
                    const String control (buf);
                    for (auto& b : hosts)
                        if (b != a && socketInodes (b).count (control) > 0) ++shared;
                }
                std::printf ("     %d plugin hosts; control sockets leaked into siblings: %d\n", hosts.size(), shared);
                CHECK (hosts.size() >= 2);
                CHECK (shared == 0);
            }

            /* A host that stops answering is put down after the state timeout,
               not left to cost every later request the same wait. Freeze it:
               alive, silent, like a bridged plugin whose Wine half has died. */
            {
                const auto frozen = victim->getHostPid();
                CHECK (frozen > 0);
                ::kill (frozen, SIGSTOP);
                MemoryBlock mb;
                const auto t0 = Time::getMillisecondCounterHiRes();
                const bool got = victim->getState (mb, 1500);
                const auto ms = Time::getMillisecondCounterHiRes() - t0;
                std::printf ("     frozen host: getState returned %s after %.0f ms; %s\n", got ? "state" : "failure", ms,
                             victim->getLastError().toRawUTF8());
                CHECK (! got);
                CHECK (ms >= 1400 && ms < 4000);
                CHECK (! victim->isAlive());
                CHECK (victim->getHostPid() == -1);
                CHECK (::kill (frozen, 0) != 0 && errno == ESRCH);     // killed and reaped, not a zombie
                const int restartsBefore = engine.getAutoRestartCount();
                pump (300);
                CHECK (engine.getAutoRestartCount() == restartsBefore + 1);   // on its way back by itself
                engine.reloadPlugin (0, 40, 0);                              // a Reload pressed meanwhile is harmless
                settle();
                victim = engine.getPlugin (0, 40, 0);
                CHECK (victim != nullptr && victim->isAlive());
            }

            /* A host that dies while something else still holds its end of the
               control socket -- no end-of-file will ever arrive -- is still
               noticed within a moment, from the process table. */
           #if HAVE_PIDFD
            if (victim != nullptr && victim->isAlive())
            {
                const auto doomed = victim->getHostPid();
                const int pidfd = (int) ::syscall (SYS_pidfd_open, doomed, 0u);
                const int held  = pidfd >= 0 ? (int) ::syscall (SYS_pidfd_getfd, pidfd, 3, 0u) : -1;
                if (held < 0)
                {
                    std::printf ("     skip held-socket check (pidfd_getfd: %s)\n", strerror (errno));
                }
                else
                {
                    ::kill (doomed, SIGKILL);
                    MemoryBlock mb;
                    const auto t0 = Time::getMillisecondCounterHiRes();
                    const bool got = victim->getState (mb, 10000);
                    const auto ms = Time::getMillisecondCounterHiRes() - t0;
                    std::printf ("     killed host with its socket held open: getState returned %s after %.0f ms; %s\n",
                                 got ? "state" : "failure", ms, victim->getLastError().toRawUTF8());
                    CHECK (! got);
                    CHECK (ms < 3000);
                    CHECK (! victim->isAlive());
                    CHECK (victim->getHostPid() == -1);
                    ::close (held);
                    pump (300);
                    engine.reloadPlugin (0, 40, 0);
                    settle();
                    victim = engine.getPlugin (0, 40, 0);
                    CHECK (victim != nullptr && victim->isAlive());
                }
                if (pidfd >= 0) ::close (pidfd);
            }
           #endif
        }
        if (victim != nullptr && victim->isAlive())
        {
            engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
            CHECK (render (engine, 10) > 0.01f);

            // Find and kill our plugin host children the hard way, like a plugin crash would.
            ChildProcess pgrep;
            pgrep.start (StringArray { "pgrep", "-P", String ((int) ::getpid()), "-f", "plugin-host" });
            auto pids = StringArray::fromLines (pgrep.readAllProcessOutput().trim());
            std::printf ("     %d plugin host processes running; killing them all\n", pids.size());
            CHECK (pids.size() >= 1);
            for (auto& pidText : pids)
                if (pidText.trim().isNotEmpty()) ::kill ((pid_t) pidText.getIntValue(), SIGKILL);

            /* The crash is noticed and the plugin is started again by
               itself -- no Reload pressed. `victim` is the old process's
               client, freed by the restart: not touched from here. */
            const int restartsBefore = engine.getAutoRestartCount();
            pump (300);
            CHECK (engine.getAutoRestartCount() > restartsBefore);   // on its way back, or back already

            // The engine keeps rendering (silence from the dead slot) instead of hanging.
            CHECK (render (engine, 5) < 1e-4f);

            settle();
            CHECK (engine.isPluginAlive (0, 40, 0));
            CHECK (engine.getAutoRestartCount() >= 1);
            std::printf ("     after crash: restarted by itself (%d automatic restarts so far)\n", engine.getAutoRestartCount());
            engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
            const float back = render (engine, 20);
            std::printf ("     rms after reload of crashed plugin: %f\n", back);
            CHECK (back > 0.01f);
            engine.injectMidi (0, MidiMessage::noteOff (1, 60));
            render (engine, 10);
        }
        engine.clearProgram (0, 40);

        /* A host stuck inside the plugin's processing -- alive, never dying,
           silent for the rest of the night -- is noticed by the watchdog and
           restarted. Stopped with SIGSTOP while a block is in flight. Then a
           plugin that keeps dying is left off after the per-minute limit
           instead of restarting for ever. A fresh program, so the earlier
           deaths do not count against it. */
        engine.selectProgram (0, 41);
        settle();
        CHECK (engine.addSlot (0, 41, synth, err));
        settle();
        if (auto* stuck = engine.getPlugin (0, 41, 0); stuck != nullptr && stuck->isAlive())
        {
            const auto frozen = stuck->getHostPid();
            const int before = engine.getAutoRestartCount();
            ::kill (frozen, SIGSTOP);
            render (engine, 3);                               // a block goes in and never comes back
            const auto t0 = Time::getMillisecondCounterHiRes();
            while (engine.getAutoRestartCount() == before && Time::getMillisecondCounterHiRes() - t0 < Engine::stalledPluginMs + 4000)
            {
                render (engine, 1);
                pump (100);
            }
            const auto ms = Time::getMillisecondCounterHiRes() - t0;
            std::printf ("     stuck host: restarted after %.0f ms\n", ms);
            CHECK (engine.getAutoRestartCount() == before + 1);
            CHECK (ms >= Engine::stalledPluginMs - 1000);
            CHECK (::kill (frozen, 0) != 0 && errno == ESRCH);
            settle();
            CHECK (engine.isPluginAlive (0, 41, 0));
            CHECK (engine.getPlugin (0, 41, 0) != nullptr && engine.getPlugin (0, 41, 0)->getHostPid() != frozen);
            engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
            CHECK (render (engine, 20) > 0.01f);
            engine.injectMidi (0, MidiMessage::noteOff (1, 60));
            render (engine, 10);

            // Two more deaths use up the minute's restarts; the next is left off.
            for (int k = 0; k < Engine::maxAutoRestartsPerMinute; ++k)
            {
                auto* p = engine.getPlugin (0, 41, 0);
                if (p == nullptr || ! p->isAlive()) break;
                ::kill (p->getHostPid(), SIGKILL);
                pump (300);
                settle();
            }
            std::printf ("     after %d deaths in a minute: %s\n", Engine::maxAutoRestartsPerMinute + 1,
                         engine.isPluginAlive (0, 41, 0) ? "still restarting (wrong)" : "left off");
            CHECK (! engine.isPluginAlive (0, 41, 0));
            CHECK (! engine.hasPendingLoads());
            engine.reloadPlugin (0, 41, 0);                   // Reload by hand still works
            settle();
            CHECK (engine.isPluginAlive (0, 41, 0));
        }
        else CHECK (false);
        engine.clearProgram (0, 41);
        engine.selectProgram (0, 0);
        settle();
        settle();
    }

    // --- optional: write a demo setup for eyeballing the UI ----------------------------------
    if (auto outPath = SystemStats::getEnvironmentVariable ("STAGERIG_TEST_WRITE_SETUP", {}); outPath.isNotEmpty())
    {
        PluginDescription fx; bool haveFx = false;
        for (auto& d : host.getKnownPlugins().getTypes())
            if (! d.isInstrument && (d.name.containsIgnoreCase ("Stereo Tools") || d.name.containsIgnoreCase ("Reverb"))) { fx = d; haveFx = true; }
        Setup demo = Setup::makeDefault();
        auto& prog = demo.inputs[0].programs[0];
        prog.name = "Demo Lead";
        SlotDef sd; sd.plugin = synth; sd.gainDb = -3.0f; sd.lowKey = 48;
        if (haveFx) { EffectDef e; e.plugin = fx; sd.effects.push_back (e); e.bypassed = true; sd.effects.push_back (e); }
        prog.slots.push_back (sd);
        SlotDef pad = sd; pad.effects.clear(); pad.transpose = -12; pad.highKey = 47; pad.lowKey = 0;
        prog.slots.push_back (pad);
        if (haveFx) { EffectDef e; e.plugin = fx; prog.effects.push_back (e); }
        MappingDef mm; mm.number = 74; mm.slot = 0; mm.effect = -1; mm.paramId = "sym:cutoff"; mm.paramName = "Cutoff";
        prog.mappings.push_back (mm);
        if (haveFx) { MappingDef fm; fm.number = 7; fm.slot = -1; fm.effect = 0; fm.paramId = "sym:level_in"; fm.paramName = "Input Gain"; prog.mappings.push_back (fm); }
        demo.inputs[0].programs[3].name = "Organ split";
        CHECK (demo.saveToFile (File (outPath)).wasOk());
        std::printf ("     demo setup written to %s\n", outPath.toRawUTF8());
    }

    // --- optional: an arbitrary VST3 (e.g. a yabridge-bridged plugin) --------------------
    // STAGERIG_TEST_VST3=/path/to/Plugin.vst3 loads it, plays a note and renders blocks;
    // this reproduces the multi-out / inactive-bus crash seen with Kontakt via yabridge.
    if (auto vst3Path = SystemStats::getEnvironmentVariable ("STAGERIG_TEST_VST3", {}); vst3Path.isNotEmpty())
    {
        // A tempo no default would produce, so the helper's report proves this
        // value reached the plugin rather than 120 being a coincidence.
        engine.setTempoBpm (137.0);

        AudioPluginFormat* vst3 = nullptr;
        for (auto* f : host.getFormatManager().getFormats())
            if (f->getName() == "VST3") vst3 = f;
        CHECK (vst3 != nullptr);
        if (vst3 != nullptr)
        {
            OwnedArray<PluginDescription> found;
            host.getKnownPlugins().scanAndAddFile (vst3Path, true, found, *vst3);
            CHECK (found.size() > 0);
            if (found.size() > 0)
            {
                const PluginDescription desc (*found[0]);
                std::printf ("     extra VST3: %s (%s)\n", desc.name.toRawUTF8(), desc.manufacturerName.toRawUTF8());
                engine.setPreloadAllPrograms (false);
                engine.selectProgram (0, 20);
                settle();
                CHECK (engine.addSlot (0, 20, desc, err));
                settle();
                if (err.isNotEmpty()) std::printf ("     addSlot error: %s\n", err.toRawUTF8());
                if (auto* x = engine.getPlugin (0, 20, 0); x != nullptr && x->isAlive())
                {
                    std::printf ("     loaded in its own process: %s, %d parameters\n", x->getName().toRawUTF8(), (int) x->getParameters().size());
                    {
                        // VST3 MIDI controller proxies: the helper asks IMidiMapping which copy is which channel.
                        int proxies = 0, vol1 = -1, vol2 = -1;
                        for (auto& p : x->getParameters())
                        {
                            if (p.midiChannel > 0) ++proxies;
                            if (p.midiController == 7 && p.midiChannel == 1) vol1 = p.index;
                            if (p.midiController == 7 && p.midiChannel == 2) vol2 = p.index;
                        }
                        std::printf ("     %d MIDI controller proxies; CC7 ch1 -> #%d, CC7 ch2 -> #%d\n", proxies, vol1, vol2);
                        if (vol1 >= 0 && vol2 >= 0)
                        {
                            // Send CC 7 on channel 1 (the Upper input's channel): the ch1 copy must move, the ch2 copy must not.
                            float a1 = 0, a2 = 0, b1 = 0, b2 = 0;
                            CHECK (x->fetchParameterValue (vol1, a1) && x->fetchParameterValue (vol2, a2));
                            const float target = a1 > 0.5f ? 0.1f : 0.9f;
                            engine.injectMidi (0, MidiMessage::controllerEvent (1, 7, (int) std::lround (target * 127.0f)));
                            render (engine, 5);
                            CHECK (x->fetchParameterValue (vol1, b1) && x->fetchParameterValue (vol2, b2));
                            std::printf ("     CC7 ch1 -> %d: ch1 copy %.3f -> %.3f, ch2 copy %.3f -> %.3f\n", (int) std::lround (target * 127.0f), a1, b1, a2, b2);
                            CHECK (std::abs (b1 - target) < 0.02f);
                            CHECK (std::abs (b2 - a2) < 1e-3f);
                        }
                    }
                    // STAGERIG_TEST_TWO_INPUTS=1 loads the same plugin into a second input as
                    // well: two instances of one bridged plugin, which is what a two-manual rig does.
                    if (SystemStats::getEnvironmentVariable ("STAGERIG_TEST_TWO_INPUTS", "0").getIntValue() > 0)
                    {
                        std::printf ("     loading a SECOND instance of %s on input 1\n", desc.name.toRawUTF8()); std::fflush (stdout);
                        const auto t0b = Time::getMillisecondCounterHiRes();
                        engine.selectProgram (1, 20);
                        settle();
                        String err2;
                        const bool added = engine.addSlot (1, 20, desc, err2);
                        settle();
                        std::printf ("     second instance: added=%d err='%s' after %.0f ms\n",
                                     (int) added, err2.toRawUTF8(), Time::getMillisecondCounterHiRes() - t0b);
                        auto* second = engine.getPlugin (1, 20, 0);
                        std::printf ("     second alive=%d  first alive=%d\n",
                                     (int) (second != nullptr && second->isAlive()),
                                     (int) (x != nullptr && x->isAlive()));
                        CHECK (added && second != nullptr && second->isAlive());
                        CHECK (x != nullptr && x->isAlive());      // the first must survive the second
                        engine.injectMidi (1, MidiMessage::noteOn (2, 60, (uint8) 100));
                        std::printf ("     rms with both loaded: %f\n", render (engine, 60));
                        engine.injectMidi (1, MidiMessage::noteOff (2, 60));
                    }

                    // STAGERIG_TEST_EDITOR_SECONDS=N keeps the plugin's editor open for N seconds so a
                    // script can drive and screenshot it (GUI/mouse investigations).
                    if (const int hold = SystemStats::getEnvironmentVariable ("STAGERIG_TEST_EDITOR_SECONDS", "0").getIntValue(); hold > 0)
                    {
                        std::printf ("     opening editor 'PERFTEST %s' for %d s\n", desc.name.toRawUTF8(), hold); std::fflush (stdout);
                        CHECK (x->showEditor ("PERFTEST " + desc.name));
                        const auto tEd = Time::getMillisecondCounterHiRes();
                        while (Time::getMillisecondCounterHiRes() - tEd < hold * 1000.0) { pump (20); render (engine, 1); }
                        x->hideEditor();
                        pump (200);
                    }
                    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
                    const float r = render (engine, 100);                      // ~1.2 s; would segfault before the fix
                    std::printf ("     rms with extra VST3 (may be 0 if no preset loaded): %f\n", r);
                    engine.injectMidi (0, MidiMessage::noteOff (1, 60));
                    render (engine, 20);
                    CHECK (true);   // survived processing
                }
            }
        }
    }

    // --- parallel loading: several plugins load at once ---------------------------------
    {
        std::printf ("     loader pool: %d threads\n", engine.getParallelLoads());
        CHECK (engine.getParallelLoads() >= 2);
        engine.selectProgram (0, 60);
        settle();
        int maxInFlight = 0;
        for (int k = 0; k < 6; ++k) { String e2; engine.addSlot (0, 60, synth, e2); }
        const auto t0p = Time::getMillisecondCounterHiRes();
        while (engine.hasPendingLoads() && Time::getMillisecondCounterHiRes() - t0p < 60000.0)
        {
            maxInFlight = jmax (maxInFlight, engine.getLoadsInFlight());
            pump (2);
        }
        std::printf ("     6 loads: max %d in flight, %.0f ms total\n", maxInFlight, Time::getMillisecondCounterHiRes() - t0p);
        CHECK (maxInFlight >= 2);
        int alive = 0;
        for (int k = 0; k < 6; ++k) if (engine.isPluginAlive (0, 60, k)) ++alive;
        CHECK (alive == 6);
        CHECK (render (engine, 3) < 1e-4f);     // silent until played
    }

    // --- a CLAP synth that takes CLAP note events: PANIC silences it (Surge XT, when installed) ---
    {
        File surgeFile;
        for (const char* p : { "/usr/lib/clap/Surge XT.clap", "/usr/local/lib/clap/Surge XT.clap" })
            if (File (p).exists()) { surgeFile = File (p); break; }
        if (surgeFile == File()) surgeFile = File::getSpecialLocation (File::userHomeDirectory).getChildFile (".clap/Surge XT.clap");
        if (! surgeFile.exists())
            std::printf ("     skip Surge XT PANIC check (not installed)\n");
        else
        {
            perf::ClapPluginFormat clapFmt;
            OwnedArray<PluginDescription> found;
            clapFmt.findAllTypesForFile (found, surgeFile.getFullPathName());
            const PluginDescription* surge = nullptr;
            for (auto* d : found) if (d->isInstrument && d->name == "Surge XT") surge = d;
            CHECK (surge != nullptr);
            if (surge != nullptr)
            {
                engine.clearProgram (0, 61);
                engine.selectProgram (0, 61); settle();
                String sErr;
                CHECK (engine.addSlot (0, 61, *surge, sErr));
                settle();
                CHECK (engine.isPluginAlive (0, 61, 0));
                render (engine, 10);
                engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
                engine.injectMidi (0, MidiMessage::noteOn (1, 64, (uint8) 100));
                const float sounding = render (engine, 20);
                std::printf ("     Surge XT playing two notes: %.4f\n", sounding);
                CHECK (sounding > 1e-3f);
                engine.panic();                                          // the keys are still held
                render (engine, 10);
                const float afterPanic = render (engine, 10);
                std::printf ("     after PANIC with the keys held: %.6f\n", afterPanic);
                CHECK (afterPanic < 1e-4f);
                engine.injectMidi (0, MidiMessage::noteOff (1, 60));
                engine.injectMidi (0, MidiMessage::noteOff (1, 64));
                render (engine, 5);
                engine.clearProgram (0, 61);
            }
        }
    }

    // --- plugin icons: generated badges always, embedded Windows icons when present ------
    {
        PluginDescription d; d.name = "Kontakt 8"; d.manufacturerName = "Native Instruments"; d.pluginFormatName = "VST3";
        CHECK (PluginIcons::initials ("Kontakt 8") == "K8");
        CHECK (PluginIcons::initials ("Calf Monosynth") == "CM");
        CHECK (PluginIcons::initials ("Dirt") == "D");
        CHECK (PluginIcons::badgeColour ("Native Instruments") == PluginIcons::badgeColour ("native instruments "));
        auto b = PluginIcons::badge (d, 48);
        CHECK (b.isValid() && b.getWidth() == 48 && b.getPixelAt (24, 24).getAlpha() > 200);
        TemporaryFile iconCache;
        PluginIcons icons (iconCache.getFile());
        CHECK (icons.get (d, 32).getWidth() == 32);
        d.fileOrIdentifier = File::getSpecialLocation (File::userHomeDirectory).getChildFile (".vst3/yabridge/Kontakt 8.vst3").getFullPathName();
        if (File (d.fileOrIdentifier).isDirectory())
        {
            const auto pe = PluginIcons::windowsBinaryFor (d);
            std::printf ("     Kontakt windows binary: %s\n", pe.getFullPathName().toRawUTF8());
            CHECK (pe.existsAsFile());
            const auto t0i = Time::getMillisecondCounterHiRes();
            const auto img = PluginIcons::fromWindowsBinary (pe);
            std::printf ("     embedded icon: %dx%d in %.1f ms\n", img.getWidth(), img.getHeight(), Time::getMillisecondCounterHiRes() - t0i);
            CHECK (img.isValid() && img.getWidth() >= 128);
            CHECK (icons.native (d).isValid());
            CHECK (iconCache.getFile().getNumberOfChildFiles (File::findFiles, "*.png") == 1);   // cached
        }
        PluginDescription none; none.name = "No Icon"; none.manufacturerName = "X"; none.fileOrIdentifier = "/nonexistent";
        CHECK (! icons.native (none).isValid() && icons.get (none, 24).isValid());
    }

    engine.removeListener (&listener);
    // ---- tempo and tap tempo ---------------------------------------------------
    /* Plugins were handed no playhead at all, so a tempo-synced delay had
       nothing to sync to. One tempo for the whole rig, and a tap that settles
       fast enough to be useful between songs. */
    {
        engine.setTempoBpm (120.0);
        CHECK (std::abs (engine.getTempoBpm() - 120.0) < 0.001);

        engine.setTempoBpm (10.0);
        CHECK (engine.getTempoBpm() >= 20.0);          // clamped, not accepted
        engine.setTempoBpm (10000.0);
        CHECK (engine.getTempoBpm() <= 300.0);

        engine.setTempoBpm (120.0);
        engine.resetTapTempo();
        CHECK (engine.tapTempo() == 0.0);              // one tap implies nothing

        // Four taps at 500ms is 120 bpm. Real taps are not exact, so allow a
        // little slop -- and this is the interval a player would actually use.
        engine.resetTapTempo();
        engine.tapTempo();
        double tapped = 0.0;
        for (int i = 0; i < 3; ++i) { Thread::sleep (500); tapped = engine.tapTempo(); }
        std::printf ("     four taps at 500ms -> %.1f bpm\n", tapped);
        CHECK (tapped > 100.0 && tapped < 140.0);

        // A long pause starts a fresh count rather than averaging across it.
        engine.resetTapTempo();
        engine.tapTempo();
        Thread::sleep (2100);
        CHECK (engine.tapTempo() == 0.0);              // treated as a first tap

        // The tap controller is remembered and clamped.
        engine.setTapTempoCC (64);
        CHECK (engine.getTapTempoCC() == 64);
        engine.setTapTempoCC (999);
        CHECK (engine.getTapTempoCC() == 127);
        engine.setTapTempoCC (0);                      // none

        // Tempo and the tap controller survive a save and load.
        engine.setTempoBpm (143.0);
        engine.setTapTempoCC (80);
        const auto saved = Setup::fromVar (engine.captureSetup().toVar());
        CHECK (std::abs (saved.tempoBpm - 143.0) < 0.001);
        CHECK (saved.tapTempoCC == 80);

        /* The footswitch path: a CC arriving on any input taps the tempo. This is
           the part that runs on the audio thread and posts to the message
           thread, so it exercises more than tapTempo() alone. */
        engine.setTempoBpm (120.0);
        engine.setTapTempoCC (80);
        engine.resetTapTempo();
        for (int i = 0; i < 4; ++i)
        {
            engine.injectMidi (0, MidiMessage::controllerEvent (1, 80, 127));
            MessageManager::getInstance()->runDispatchLoopUntil (20);
            if (i < 3) Thread::sleep (480);
        }
        MessageManager::getInstance()->runDispatchLoopUntil (50);
        std::printf ("     four CC 80 taps at ~500ms -> %.1f bpm\n", engine.getTempoBpm());
        CHECK (engine.getTempoBpm() > 100.0 && engine.getTempoBpm() < 140.0);

        // The release half of a footswitch press must not count as a tap.
        engine.setTempoBpm (120.0);
        engine.resetTapTempo();
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 80, 0));
        MessageManager::getInstance()->runDispatchLoopUntil (30);
        CHECK (std::abs (engine.getTempoBpm() - 120.0) < 0.001);

        // A different CC does nothing.
        engine.resetTapTempo();
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 81, 127));
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 81, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (30);
        CHECK (std::abs (engine.getTempoBpm() - 120.0) < 0.001);
        engine.setTapTempoCC (0);

        /* Learning the tap controller: press the pedal rather than look up what
           it sends. Must work outside the channel filter, and must be able to
           replace a controller that is already assigned. */
        engine.setTapTempoCC (0);
        engine.armTapTempoLearn (true);
        CHECK (engine.isTapTempoLearnArmed());
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 67, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (40);
        CHECK (engine.getTapTempoCC() == 67);
        CHECK (! engine.isTapTempoLearnArmed());       // disarms itself

        // The release half must not be what gets learned.
        engine.setTapTempoCC (0);
        engine.armTapTempoLearn (true);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 70, 0));
        MessageManager::getInstance()->runDispatchLoopUntil (40);
        CHECK (engine.getTapTempoCC() == 0);           // ignored, still waiting
        CHECK (engine.isTapTempoLearnArmed());
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 70, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (40);
        CHECK (engine.getTapTempoCC() == 70);

        // Reassigning while a controller is already assigned: learning wins,
        // rather than the old controller swallowing the press as a tap.
        engine.armTapTempoLearn (true);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 70, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (40);
        engine.armTapTempoLearn (true);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 64, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (40);
        CHECK (engine.getTapTempoCC() == 64);
        engine.setTapTempoCC (0);

        // One learn at a time, and none once the window is closed: cancelPedalLearns lets go of every pedal.
        engine.armTapTempoLearn (true);
        engine.armSongLearn (true);
        CHECK (engine.isSongLearnArmed (true) && ! engine.isSongLearnArmed (false));   // the next-song row waits, not the previous
        engine.armSongLearn (false);
        CHECK (! engine.isSongLearnArmed (true) && engine.isSongLearnArmed (false));
        engine.armTransportLearn();
        engine.armSectionLearn();
        engine.armLooperLearn (false);
        engine.armLooperLearn (true);
        engine.cancelPedalLearns();
        CHECK (! engine.isTapTempoLearnArmed() && ! engine.isSongLearnArmed() && ! engine.isTransportLearnArmed()
               && ! engine.isSectionLearnArmed() && ! engine.isLooperLearnArmed (false) && ! engine.isLooperLearnArmed (true)
               && ! engine.isOutputVolumeLearnArmed());
        const int wasNext = engine.getNextSongCC(), wasPrev = engine.getPrevSongCC(), wasTransport = engine.getTransportCC();
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 66, 127));      // nothing is waiting: a press teaches nothing
        MessageManager::getInstance()->runDispatchLoopUntil (40);
        CHECK (engine.getTapTempoCC() == 0 && engine.getNextSongCC() == wasNext && engine.getPrevSongCC() == wasPrev && engine.getTransportCC() == wasTransport);

        // One controller, one job: a pedal taught another pedal's controller takes it; so does an output's volume.
        engine.setNextSongCC (66);
        engine.setTapTempoCC (66);
        CHECK (engine.getTapTempoCC() == 66 && engine.getNextSongCC() == 0);
        engine.setOutputVolumeCC (0, 66);
        CHECK (engine.getTapTempoCC() == 0 && engine.getSetup().outputs[0].volumeCC == 66);
        engine.armPedalLearn (Engine::Pedal::section);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 66, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (40);
        CHECK (engine.getSectionCC() == 66 && engine.getSetup().outputs[0].volumeCC == 0);
        // While a mapping is being learned, a pedal's press is reported, not acted on, and the learn keeps waiting.
        engine.setLearnArmed (true);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 66, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (40);
        CHECK (engine.isLearnArmed());
        engine.setLearnArmed (false);
        engine.setSectionCC (0);
        engine.setNextSongCC (wasNext);
        engine.setPrevSongCC (wasPrev);
        engine.setTransportCC (wasTransport);

        // A setup written before tempo existed loads at a sane default.
        auto older = engine.captureSetup().toVar();
        if (auto* o = older.getDynamicObject()) { o->removeProperty ("tempoBpm"); o->removeProperty ("tapTempoCC"); }
        const auto legacy = Setup::fromVar (older);
        CHECK (std::abs (legacy.tempoBpm - 120.0) < 0.001);
        CHECK (legacy.tapTempoCC == 0);
    }

    // ---- songs and sets ---------------------------------------------------
    /* A song sets every keyboard at once and brings its own tempo; a set is the
       order they are played in. The footswitch path matters most: on stage that
       is how the set is driven, and it runs through the MIDI thread. */
    {
        std::printf ("  songs and sets\n");

        while ((int) engine.getSetup().inputs.size() < 2) engine.addInput ("Lower");
        CHECK (engine.getSetup().inputs.size() >= 2);

        engine.setProgramName (0, 4, "Oye Como");
        engine.setProgramName (1, 1, "FM EP");
        engine.setProgramName (0, 2, "B3");

        // A song captured from what is loaded right now.
        engine.selectProgram (0, 4);
        engine.selectProgram (1, 1);
        engine.setTempoBpm (116.0);
        const int oye = engine.addSong ("Oye Como Va");
        engine.captureSong (oye);
        CHECK (engine.getSongs()[(size_t) oye].programFor (0) == 4);
        CHECK (engine.getSongs()[(size_t) oye].programFor (1) == 1);
        CHECK (std::abs (engine.getSongs()[(size_t) oye].tempoBpm - 116.0) < 0.001);

        // A second song, built by hand, that leaves Lower alone.
        const int onions = engine.addSong ("Green Onions");
        engine.setSongProgram (onions, 0, 2);
        engine.setSongProgram (onions, 1, -1);
        engine.setSongTempo (onions, 132.0);

        // Selecting a song moves every input it names, and only those.
        engine.selectProgram (0, 0);
        engine.selectProgram (1, 1);
        engine.setTempoBpm (90.0);
        engine.selectSong (onions);
        CHECK (engine.getSetup().inputs[0].currentProgram == 2);
        CHECK (engine.getSetup().inputs[1].currentProgram == 1);      // untouched
        CHECK (std::abs (engine.getTempoBpm() - 132.0) < 0.001);

        // A song with no tempo of its own leaves the tempo where it is.
        const int quiet = engine.addSong ("No tempo");
        engine.setSongProgram (quiet, 0, 4);
        engine.setTempoBpm (101.0);
        engine.selectSong (quiet);
        CHECK (std::abs (engine.getTempoBpm() - 101.0) < 0.001);

        // A set orders songs, and stepping through it stops at the end.
        const int first = engine.addSet ("First set");
        engine.setSetSongs (first, { oye, onions, quiet });
        engine.selectSet (first);
        CHECK (engine.getCurrentSet() == first);
        CHECK (engine.getCurrentSongInSet() == -1);      // chosen, not started

        engine.nextSong();
        CHECK (engine.getCurrentSongInSet() == 0);
        CHECK (engine.getSetup().inputs[0].currentProgram == 4);
        engine.nextSong();
        CHECK (engine.getCurrentSongInSet() == 1);
        engine.nextSong();
        CHECK (engine.getCurrentSongInSet() == 2);
        engine.nextSong();                                // past the end
        CHECK (engine.getCurrentSongInSet() == 2);        // stays put, does not wrap
        engine.previousSong();
        CHECK (engine.getCurrentSongInSet() == 1);
        while (engine.getCurrentSongInSet() > 0) engine.previousSong();
        engine.previousSong();
        CHECK (engine.getCurrentSongInSet() == 0);        // and does not run off the front

        /* The footswitch: a CC steps the set whatever is loaded. This goes
           through the MIDI thread and the event queue, like the real thing. */
        engine.setNextSongCC (80);
        engine.setPrevSongCC (81);
        engine.selectSongInSet (0);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 80, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (60);
        CHECK (engine.getCurrentSongInSet() == 1);
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 81, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (60);
        CHECK (engine.getCurrentSongInSet() == 0);

        // The release half must not step again.
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 80, 0));
        MessageManager::getInstance()->runDispatchLoopUntil (40);
        CHECK (engine.getCurrentSongInSet() == 0);

        // Learning a footswitch, like the tap.
        engine.setNextSongCC (0);
        engine.armSongLearn (true);
        CHECK (engine.isSongLearnArmed());
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 67, 127));
        MessageManager::getInstance()->runDispatchLoopUntil (60);
        CHECK (! engine.isSongLearnArmed());
        CHECK (engine.getNextSongCC() == 67);

        /* Deleting a song must not leave a set playing the wrong one: the
           indices after it shift, and any set using it drops it. */
        const int before = (int) engine.getSets()[(size_t) first].songs.size();
        engine.removeSong (onions);                       // the middle song
        const auto& songs = engine.getSets()[(size_t) first].songs;
        CHECK ((int) songs.size() == before - 1);
        for (int i : songs) CHECK (i >= 0 && i < (int) engine.getSongs().size());
        CHECK (engine.getSongs()[(size_t) songs[0]].name == "Oye Como Va");
        CHECK (engine.getSongs()[(size_t) songs[1]].name == "No tempo");

        /* Reordering the songs list must not change what any set plays: sets
           hold indices, so a swap has to rewrite them. This is the one that
           would quietly ruin a gig -- the set list would look the same and
           play something else. */
        {
            const int a = engine.addSong ("Alpha");
            const int b = engine.addSong ("Bravo");
            const int c = engine.addSong ("Charlie");
            const int st = engine.addSet ("Order test");
            engine.setSetSongs (st, { c, a, b });          // deliberately not in order

            auto namesInSet = [&]
            {
                StringArray out;
                for (int i : engine.getSets()[(size_t) st].songs)
                    out.add (engine.getSongs()[(size_t) i].name);
                return out.joinIntoString (",");
            };
            const auto before = namesInSet();
            CHECK (before == "Charlie,Alpha,Bravo");

            const int moved = engine.moveSong (b, -1);     // Bravo up one
            CHECK (moved == b - 1);
            CHECK (engine.getSongs()[(size_t) moved].name == "Bravo");
            CHECK (namesInSet() == before);                // the set plays the same order

            engine.moveSong (moved, 1);                    // and back
            CHECK (namesInSet() == before);

            // Moving off either end does nothing rather than corrupting anything.
            CHECK (engine.moveSong (0, -1) == 0);
            const int last = (int) engine.getSongs().size() - 1;
            CHECK (engine.moveSong (last, 1) == last);
            CHECK (namesInSet() == before);

            // Edited a step at a time, as from the stand: the position being played follows its song.
            {
                const int a = engine.addSong ("Edit a"), b = engine.addSong ("Edit b"), c = engine.addSong ("Edit c");
                const int es = engine.addSet ("Edited");
                engine.addSongToSet (es, a); engine.addSongToSet (es, b); engine.addSongToSet (es, c);
                engine.addSongToSet (es, 9999);                                           // no such song: ignored
                CHECK (engine.getSets()[(size_t) es].songs == std::vector<int> ({ a, b, c }));
                engine.selectSet (es);
                engine.selectSongInSet (1);                                               // playing b
                engine.moveInSet (es, 1, 1);                                              // b moves last
                CHECK (engine.getSets()[(size_t) es].songs == std::vector<int> ({ a, c, b }) && engine.getCurrentSongInSet() == 2);
                engine.moveInSet (es, 0, 1);                                              // a and c swap around the playing one
                CHECK (engine.getSets()[(size_t) es].songs == std::vector<int> ({ c, a, b }) && engine.getCurrentSongInSet() == 2);
                engine.removeFromSet (es, 0);                                             // one before it goes: the position moves up
                CHECK (engine.getSets()[(size_t) es].songs == std::vector<int> ({ a, b }) && engine.getCurrentSongInSet() == 1);
                const int copy = engine.duplicateSet (es, "Edited again");
                CHECK (copy >= 0 && engine.getSets()[(size_t) copy].name == "Edited again" && engine.getSets()[(size_t) copy].songs == std::vector<int> ({ a, b }));
                const int copy2 = engine.duplicateSet (es, "");
                CHECK (engine.getSets()[(size_t) copy2].name == "Edited copy");
                engine.removeFromSet (es, 1);                                             // the one playing goes: no position, the song plays on
                CHECK (engine.getSets()[(size_t) es].songs == std::vector<int> ({ a }) && engine.getCurrentSongInSet() == -1 && engine.currentSongIndex() == b);
                engine.moveInSet (es, 0, 1);                                              // off the end: nothing
                CHECK (engine.getSets()[(size_t) es].songs == std::vector<int> ({ a }));
                engine.removeSet (copy2); engine.removeSet (copy); engine.removeSet (es);
                engine.removeSong (c); engine.removeSong (b); engine.removeSong (a);
                engine.selectSet (st);
            }

            // Sets reorder too, and keep playing the one you were playing.
            engine.addSet ("Second");
            engine.selectSet (st);
            const int newPos = engine.moveSet (st, 1);
            CHECK (engine.getCurrentSet() == newPos);
            CHECK (engine.getSets()[(size_t) newPos].name == "Order test");
            CHECK (engine.getSets()[(size_t) st].name == "Second");
            engine.removeSet (jmax (newPos, st));
            engine.removeSet (jmin (newPos, st));
        }

        // Charts attach to a song and come off it again.
        {
            const int withChart = engine.addSong ("Charted");
            engine.addSongChart (withChart, { "charted.pdf", "Sheet" });
            engine.addSongChart (withChart, { "charted.cho", "" });
            CHECK (engine.getSongs()[(size_t) withChart].charts.size() == 2);
            engine.removeSongChart (withChart, 0);
            CHECK (engine.getSongs()[(size_t) withChart].charts.size() == 1);
            CHECK (engine.getSongs()[(size_t) withChart].charts[0].file == "charted.cho");
            engine.removeSongChart (withChart, 5);                    // out of range: ignored
            CHECK (engine.getSongs()[(size_t) withChart].charts.size() == 1);
            engine.removeSong (withChart);
        }

        // All of it survives a save and load.
        engine.setNextSongCC (80);
        engine.setPrevSongCC (81);
        const auto saved = Setup::fromVar (engine.captureSetup().toVar());
        CHECK (saved.songs.size() == engine.getSongs().size());
        CHECK (saved.sets.size() == engine.getSets().size());
        CHECK (saved.nextSongCC == 80 && saved.prevSongCC == 81);
        CHECK (saved.songs[0].name == "Oye Como Va");
        CHECK (std::abs (saved.songs[0].tempoBpm - 116.0) < 0.001);
    }

    // --- two instances of one plugin keep their own state -----------------------------
    /* Reported: with the same plugin in two programs, saving left both with the
       state of the one edited last. Walk the reporter's steps: load, set each
       differently, switch between them (which captures the one being left),
       save, reload, and ask each plugin what it has. Run for the LV2 synth and,
       when one is installed, for a native VST3 -- the reporter's format. */
    auto twoInstances = [&] (const PluginDescription& desc, const char* label)
    {
        std::printf ("     --- two instances of %s (%s) ---\n", desc.name.toRawUTF8(), label);
        engine.setPreloadAllPrograms (false);
        engine.clearProgram (0, 50); engine.clearProgram (0, 51);
        engine.selectProgram (0, 50); settle();
        CHECK (engine.addSlot (0, 50, desc, err)); settle();
        engine.selectProgram (0, 51); settle();
        CHECK (engine.addSlot (0, 51, desc, err)); settle();
        auto* a = engine.getPlugin (0, 50, 0);
        auto* b = engine.getPlugin (0, 51, 0);
        CHECK (a != nullptr && b != nullptr && a != b);
        if (a == nullptr || b == nullptr) return;

        int idx = -1;
        for (auto& p : a->getParameters())
            if (p.automatable && ! p.discrete && p.name.containsIgnoreCase ("cutoff")) { idx = p.index; break; }
        if (idx < 0)
            for (auto& p : a->getParameters())
                if (p.automatable && ! p.discrete) { idx = p.index; break; }
        CHECK (idx >= 0);
        if (idx < 0) return;
        std::printf ("     parameter %d '%s'\n", idx, a->getParameters()[(size_t) idx].name.toRawUTF8());

        /* Set each with its program sounding and a block rendered, as a hand on
           a knob would: an LV2 plugin's new value reaches it with the next block,
           and its saved state reads the plugin's ports, not the host's cache. */
        engine.selectProgram (0, 50); settle();
        CHECK (a->setParameterValue (idx, 0.2f)); render (engine, 2);
        engine.selectProgram (0, 51); settle();
        CHECK (b->setParameterValue (idx, 0.8f)); render (engine, 2);
        {
            float fa = -1, fb = -1; a->fetchParameterValue (idx, fa); b->fetchParameterValue (idx, fb);
            MemoryBlock ga, gb; a->getState (ga); b->getState (gb);
            std::printf ("     right after set: a=%.3f b=%.3f  direct getState a %d bytes, b %d bytes, %s\n",
                         fa, fb, (int) ga.getSize(), (int) gb.getSize(), ga == gb ? "IDENTICAL" : "different");
        }
        engine.selectProgram (0, 50); settle();        // leaving 51 captures it
        engine.selectProgram (0, 51); settle();        // leaving 50 captures it

        /* Captured once, a plugin is left alone by the autosave until
           something could have changed it: it is the asking that costs. */
        CHECK (! a->stateMayHaveChanged() && ! b->stateMayHaveChanged());
        CHECK (b->setParameterValue (idx, 0.75f));
        CHECK (b->stateMayHaveChanged() && ! a->stateMayHaveChanged());
        engine.captureSetup (-1, true);
        CHECK (! b->stateMayHaveChanged());
        CHECK (b->setParameterValue (idx, 0.8f));
        engine.selectProgram (0, 50); settle(); engine.selectProgram (0, 51); settle();

        TemporaryFile two (".stagerig.json");
        CHECK (engine.captureSetup().saveToFile (two.getFile()).wasOk());
        Setup rl;
        CHECK (Setup::loadFromFile (two.getFile(), rl).wasOk());
        const auto& sa = rl.inputs[0].programs[50].slots[0].state;
        const auto& sb = rl.inputs[0].programs[51].slots[0].state;
        std::printf ("     saved states: program 50 %d bytes, program 51 %d bytes, %s\n",
                     (int) sa.getSize(), (int) sb.getSize(), sa == sb ? "IDENTICAL" : "different");
        CHECK (sa.getSize() > 0 && sb.getSize() > 0 && sa != sb);

        engine.loadSetup (rl); settle();
        engine.selectProgram (0, 50); settle();
        engine.selectProgram (0, 51); settle();
        float va = -1.0f, vb = -1.0f;
        auto* ra = engine.getPlugin (0, 50, 0);
        auto* rb = engine.getPlugin (0, 51, 0);
        CHECK (ra != nullptr && rb != nullptr);
        if (ra == nullptr || rb == nullptr) return;
        CHECK (ra->fetchParameterValue (idx, va));
        CHECK (rb->fetchParameterValue (idx, vb));
        std::printf ("     after reload: program 50 -> %.3f (want 0.2), program 51 -> %.3f (want 0.8)\n", va, vb);
        CHECK (std::abs (va - 0.2f) < 0.05f);
        CHECK (std::abs (vb - 0.8f) < 0.05f);
    };
    twoInstances (synth, "LV2");
    {
        AudioPluginFormat* vst3 = nullptr;
        for (auto* f : host.getFormatManager().getFormats())
            if (f->getName() == "VST3") vst3 = f;
        PluginDescription nativeVst3; bool haveVst3 = false;
        if (vst3 != nullptr)
            for (const char* path : { "/usr/lib/vst3/Surge XT.vst3", "/usr/local/lib/vst3/Surge XT.vst3" })
                if (File (path).exists() && ! haveVst3)
                {
                    OwnedArray<PluginDescription> found;
                    host.getKnownPlugins().scanAndAddFile (path, true, found, *vst3);
                    for (auto* d : found)
                        if (d->isInstrument) { nativeVst3 = *d; haveVst3 = true; break; }
                }
        if (haveVst3) twoInstances (nativeVst3, "VST3");
        else std::printf ("     skip VST3 two-instance check (no native VST3 instrument found)\n");

        /* More of the ways two instances of one plugin come about, with the
           VST3: a layer (two slots in one program), copy-and-paste of a program,
           and the editor path -- a change made with the window open, captured
           when the window closes -- followed by a save with no program switch. */
        if (haveVst3)
        {
            const auto& desc = nativeVst3;
            auto param = [] (RemotePlugin* p)
            {
                for (auto& q : p->getParameters()) if (q.automatable && ! q.discrete && q.name.containsIgnoreCase ("cutoff")) return q.index;
                return 0;
            };
            auto saveReload = [&] (const char* label, std::function<void()> checks)
            {
                TemporaryFile f (".stagerig.json");
                CHECK (engine.captureSetup().saveToFile (f.getFile()).wasOk());
                Setup rl; CHECK (Setup::loadFromFile (f.getFile(), rl).wasOk());
                engine.loadSetup (rl); settle();
                std::printf ("     --- %s: saved and reloaded ---\n", label);
                checks();
            };

            // Layer: two slots in one program.
            engine.clearProgram (0, 60); engine.selectProgram (0, 60); settle();
            CHECK (engine.addSlot (0, 60, desc, err)); settle();
            CHECK (engine.addSlot (0, 60, desc, err)); settle();
            if (auto* s0 = engine.getPlugin (0, 60, 0)) if (auto* s1 = engine.getPlugin (0, 60, 1))
            {
                const int idx = param (s0);
                s0->setParameterValue (idx, 0.3f); s1->setParameterValue (idx, 0.7f);
                saveReload ("layer", [&]
                {
                    engine.selectProgram (0, 60); settle();
                    float v0 = -1, v1 = -1;
                    if (auto* r0 = engine.getPlugin (0, 60, 0)) r0->fetchParameterValue (idx, v0);
                    if (auto* r1 = engine.getPlugin (0, 60, 1)) r1->fetchParameterValue (idx, v1);
                    std::printf ("     layer: slot0 %.3f (want 0.3) slot1 %.3f (want 0.7)\n", v0, v1);
                    CHECK (std::abs (v0 - 0.3f) < 0.05f && std::abs (v1 - 0.7f) < 0.05f);
                });
            }

            // Copy and paste: program 61 from 60, then change the copy.
            engine.selectProgram (0, 60); settle();
            engine.copyProgram (0, 60, 61);
            engine.selectProgram (0, 61); settle();
            if (auto* c = engine.getPlugin (0, 61, 0))
            {
                const int idx = param (c);
                c->setParameterValue (idx, 0.9f);
                saveReload ("copy-paste", [&]
                {
                    engine.selectProgram (0, 60); settle(); engine.selectProgram (0, 61); settle();
                    float v60 = -1, v61 = -1;
                    if (auto* r = engine.getPlugin (0, 60, 0)) r->fetchParameterValue (idx, v60);
                    if (auto* r = engine.getPlugin (0, 61, 0)) r->fetchParameterValue (idx, v61);
                    std::printf ("     copy-paste: program 60 %.3f (want 0.3) program 61 %.3f (want 0.9)\n", v60, v61);
                    CHECK (std::abs (v60 - 0.3f) < 0.05f && std::abs (v61 - 0.9f) < 0.05f);
                });
            }

            // Editor path, no program switch before saving.
            engine.clearProgram (0, 62); engine.clearProgram (0, 63);
            engine.selectProgram (0, 62); settle(); CHECK (engine.addSlot (0, 62, desc, err)); settle();
            engine.selectProgram (0, 63); settle(); CHECK (engine.addSlot (0, 63, desc, err)); settle();
            auto* e62 = engine.getPlugin (0, 62, 0); auto* e63 = engine.getPlugin (0, 63, 0);
            if (e62 != nullptr && e63 != nullptr)
            {
                const int idx = param (e62);
                CHECK (e62->showEditor ("62")); pump (1500);
                e62->setParameterValue (idx, 0.15f);
                CHECK (e62->hideEditor()); pump (800);
                CHECK (e63->showEditor ("63")); pump (1500);
                e63->setParameterValue (idx, 0.85f);
                CHECK (e63->hideEditor()); pump (800);
                const auto& d62 = engine.getSetup().inputs[0].programs[62].slots[0].state;
                const auto& d63 = engine.getSetup().inputs[0].programs[63].slots[0].state;
                std::printf ("     after closing editors, model holds %d / %d bytes (%s)\n", (int) d62.getSize(), (int) d63.getSize(),
                             d62 == d63 ? "IDENTICAL" : "different");
                saveReload ("editor path", [&]
                {
                    engine.selectProgram (0, 62); settle(); engine.selectProgram (0, 63); settle();
                    float v62 = -1, v63 = -1;
                    if (auto* r = engine.getPlugin (0, 62, 0)) r->fetchParameterValue (idx, v62);
                    if (auto* r = engine.getPlugin (0, 63, 0)) r->fetchParameterValue (idx, v63);
                    std::printf ("     editor path: program 62 %.3f (want 0.15) program 63 %.3f (want 0.85)\n", v62, v63);
                    CHECK (std::abs (v62 - 0.15f) < 0.05f && std::abs (v63 - 0.85f) < 0.05f);
                });
            }
        }
    }

    // --- an audio input: the signal through a program's chain, trimmed, with a tail ----------
    {
        const int ai = engine.addInput ("Guitar", InputDef::Kind::audio);
        CHECK (engine.getSetup().inputs[(size_t) ai].isAudio());
        engine.setInputAudioChannels (ai, 0, -1);                  // mono, device channel 1
        settle();

        // A 1 kHz tone at 0.5 peak: 0.354 RMS if it comes through untouched.
        AudioBuffer<float> tone (1, kBlock), outBuf (2, kBlock);
        double phase = 0.0;
        auto feed = [&] (int blocks)
        {
            float peak = 0.0f;
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < kBlock; ++i) { tone.setSample (0, i, 0.5f * (float) std::sin (phase)); phase += MathConstants<double>::twoPi * 1000.0 / 48000.0; }
                outBuf.clear();
                engine.renderBlockForTesting (tone.getArrayOfReadPointers(), 1, outBuf.getArrayOfWritePointers(), 2, kBlock);
                peak = jmax (peak, outBuf.getRMSLevel (0, 0, kBlock), outBuf.getRMSLevel (1, 0, kBlock));
            }
            return peak;
        };
        const float through = feed (4);
        std::printf ("     audio input, no effects: %.3f rms (tone 0.354)\n", through);
        CHECK (std::abs (through - 0.354f) < 0.03f);

        // The meter reads the arriving peak, both sides for a mono input, before the trim.
        {
            const auto lv = engine.getInputLevel (ai);
            std::printf ("     meter: L %.3f R %.3f clip %d (tone peak 0.5)\n", lv.left, lv.right, (int) lv.clipped);
            CHECK (std::abs (lv.left - 0.5f) < 0.02f && std::abs (lv.right - 0.5f) < 0.02f && ! lv.clipped);
            const auto none = engine.getInputLevel (0);
            CHECK (none.left == 0.0f && none.right == 0.0f);               // a keyboard input has no meter
        }

        engine.setProgramInputGainDb (ai, 0, -6.0f);
        const float trimmed = feed (4);
        std::printf ("     trimmed -6 dB: %.3f rms (want 0.177)\n", trimmed);
        CHECK (std::abs (trimmed - 0.177f) < 0.02f);
        CHECK (std::abs (engine.getInputLevel (ai).left - 0.5f) < 0.02f);    // the trim does not move the meter

        // Full scale lights the clip mark; silence lets the meter fall.
        for (int i = 0; i < kBlock; ++i) tone.setSample (0, i, i % 2 ? 1.0f : -1.0f);
        outBuf.clear();
        engine.renderBlockForTesting (tone.getArrayOfReadPointers(), 1, outBuf.getArrayOfWritePointers(), 2, kBlock);
        CHECK (engine.getInputLevel (ai).clipped);
        tone.clear();
        for (int b = 0; b < 48; ++b) { outBuf.clear(); engine.renderBlockForTesting (tone.getArrayOfReadPointers(), 1, outBuf.getArrayOfWritePointers(), 2, kBlock); }

        /* The safety guard: nothing over the ceiling, and nothing that is not
           a number, leaves for the PA -- and below the ceiling it does
           nothing at all. Four times full scale, with a NaN and an infinity
           in it, as a broken plugin or a feedback squeal might send. */
        {
            engine.setProgramInputGainDb (ai, 0, 0.0f);
            const int limitedBefore = engine.getGuardLimitedBlocks(), silencedBefore = engine.getGuardSilencedBlocks();
            for (int i = 0; i < kBlock; ++i) tone.setSample (0, i, i % 2 ? 4.0f : -4.0f);
            tone.setSample (0, 10, std::numeric_limits<float>::quiet_NaN());
            tone.setSample (0, 11, std::numeric_limits<float>::infinity());
            float worst = 0.0f;
            bool finite = true;
            for (int b = 0; b < 4; ++b)
            {
                outBuf.clear();
                engine.renderBlockForTesting (tone.getArrayOfReadPointers(), 1, outBuf.getArrayOfWritePointers(), 2, kBlock);
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < kBlock; ++i)
                    {
                        const float v = outBuf.getSample (c, i);
                        if (! std::isfinite (v)) finite = false;
                        else worst = jmax (worst, std::abs (v));
                    }
            }
            std::printf ("     safety guard: 4x full scale with NaN and inf in, peak %.3f out, all finite %d\n", worst, (int) finite);
            CHECK (finite);
            CHECK (worst <= Engine::safetyCeiling + 1e-5f);
            CHECK (worst > 0.9f);                                   // turned down, not shut off
            CHECK (engine.getGuardLimitedBlocks() > limitedBefore);
            CHECK (engine.getGuardSilencedBlocks() > silencedBefore);

            tone.clear();
            for (int b = 0; b < 48000 / kBlock; ++b)             // a second of silence: the guard lets go
            { outBuf.clear(); engine.renderBlockForTesting (tone.getArrayOfReadPointers(), 1, outBuf.getArrayOfWritePointers(), 2, kBlock); }
            const int limitedNow = engine.getGuardLimitedBlocks();
            const float clean = feed (4);
            std::printf ("     after the guard let go: %.4f rms (tone 0.354, untouched)\n", clean);
            CHECK (std::abs (clean - 0.3536f) < 0.006f);
            CHECK (engine.getGuardLimitedBlocks() == limitedNow);
            engine.setProgramInputGainDb (ai, 0, -6.0f);
            // Leave the meter as the checks below expect it: full scale, then half a second of silence.
            for (int i = 0; i < kBlock; ++i) tone.setSample (0, i, i % 2 ? 1.0f : -1.0f);
            outBuf.clear();
            engine.renderBlockForTesting (tone.getArrayOfReadPointers(), 1, outBuf.getArrayOfWritePointers(), 2, kBlock);
            tone.clear();
            for (int b = 0; b < 48; ++b) { outBuf.clear(); engine.renderBlockForTesting (tone.getArrayOfReadPointers(), 1, outBuf.getArrayOfWritePointers(), 2, kBlock); }
        }
        std::printf ("     after half a second of silence: %.3f\n", engine.getInputLevel (ai).left);
        CHECK (engine.getInputLevel (ai).left < 0.4f && engine.getInputLevel (ai).left > 0.2f);   // 20 dB/s: about -10 dB in 0.5 s

        // An effect in the chain still carries the signal.
        PluginDescription anyFx; bool haveAnyFx = false;
        for (auto& d : host.getKnownPlugins().getTypes())
            if (! d.isInstrument && d.name.containsIgnoreCase ("Stereo Tools")) { anyFx = d; haveAnyFx = true; }
        if (haveAnyFx)
        {
            CHECK (engine.addEffect (ai, 0, -1, anyFx, err)); settle();
            const float withFx = feed (6);
            std::printf ("     through %s: %.3f rms\n", anyFx.name.toRawUTF8(), withFx);
            CHECK (withFx > 0.08f);
        }

        // A program change: the new program carries the input at its own trim; the old one rings out.
        engine.selectProgram (ai, 1); settle();
        const float switched = feed (6);
        std::printf ("     after a change to an empty program: %.3f rms (want 0.354)\n", switched);
        CHECK (std::abs (switched - 0.354f) < 0.03f);

        // Nothing leaks to a keyboard input's programs.
        engine.removeInput (ai);
        CHECK (feed (2) < 1e-3f);
    }

    // --- outputs: an input on its own device channels, through a master chain, at a master volume --
    {
        CHECK (engine.getSetup().outputs.size() == 1);
        const int mon = engine.addOutput ("Monitors", 2, 3);
        CHECK (mon == 1);
        engine.clearProgram (0, 70);
        engine.selectProgram (0, 70); settle();
        CHECK (engine.addSlot (0, 70, synth, err)); settle();
        engine.setInputOutput (0, mon);

        AudioBuffer<float> out4 (4, kBlock);
        auto levels = [&] (int blocks)
        {
            std::array<float, 4> peak {};
            for (int b = 0; b < blocks; ++b)
            {
                out4.clear();
                engine.renderBlockForTesting (out4.getArrayOfWritePointers(), 4, kBlock);
                for (int c = 0; c < 4; ++c) peak[(size_t) c] = jmax (peak[(size_t) c], out4.getRMSLevel (c, 0, kBlock));
            }
            return peak;
        };
        engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
        const auto p = levels (8);
        std::printf ("     on Monitors (3+4): ch1 %.3f ch2 %.3f ch3 %.3f ch4 %.3f\n", p[0], p[1], p[2], p[3]);
        CHECK (p[2] > 0.02f && p[3] > 0.02f && p[0] < 1e-4f && p[1] < 1e-4f);

        engine.setOutputGainDb (mon, -6.0f);
        const auto q = levels (8);
        std::printf ("     master -6 dB: ch3 %.3f (was %.3f)\n", q[2], p[2]);
        CHECK (std::abs (q[2] - p[2] * 0.5f) < p[2] * 0.15f);

        PluginDescription masterFx; bool haveMasterFx = false;
        for (auto& d : host.getKnownPlugins().getTypes())
            if (! d.isInstrument && d.name.containsIgnoreCase ("Stereo Tools")) { masterFx = d; haveMasterFx = true; }
        if (haveMasterFx)
        {
            CHECK (engine.addEffect (Engine::outputAddress (mon), 0, -1, masterFx, err)); settle();
            CHECK (engine.isPluginAlive (Engine::outputAddress (mon), 0, -1, 0));
            const auto r = levels (8);
            std::printf ("     through the master chain: ch3 %.3f\n", r[2]);
            CHECK (r[2] > 0.01f);
            CHECK (engine.getSetup().outputs[(size_t) mon].chain.effects.size() == 1);
        }

        // A volume controller, on any input: CC 7 at 0 is silence, at 127 full.
        engine.setOutputVolumeCC (mon, 7);
        engine.injectMidi (1, MidiMessage::controllerEvent (3, 7, 0));
        pump (60);
        CHECK (levels (4)[2] < 1e-3f);
        engine.injectMidi (1, MidiMessage::controllerEvent (3, 7, 127));
        pump (60);
        CHECK (levels (4)[2] > 0.02f);
        CHECK (std::abs (engine.getSetup().outputs[(size_t) mon].gainDb) < 0.01f);     // the model followed the pedal

        /* From a MIDI 2.0 keyboard the fader lands exactly where the pedal is,
           and the model (what is saved, and what the fader shows) with it,
           not at the nearest of MIDI 1.0's 128 steps. */
        {
            namespace ump = juce::universal_midi_packets;
            const uint32 words[] = { 0x40B20700,       // type 4, group 0, control change, channel 3, CC 7
                                     0x41800000 };     // 0x41800000: 32 of 127 in MIDI 1.0, a little over a third of the way to 33
            engine.injectMidi2 (1, ump::View (words));
            pump (60);
            const float exact = (float) (0x418000 / 16777215.0), seven = 32.0f / 127.0f;
            const float gotDb = engine.getSetup().outputs[(size_t) mon].gainDb;
            std::printf ("     MIDI 2.0 volume: model %.3f dB, exact %.3f dB, MIDI 1.0 would be %.3f dB\n", gotDb,
                         Decibels::gainToDecibels (exact * exact, -60.0f), Decibels::gainToDecibels (seven * seven, -60.0f));
            CHECK (std::abs (gotDb - Decibels::gainToDecibels (exact * exact, -60.0f)) < 0.01f);
            CHECK (std::abs (gotDb - Decibels::gainToDecibels (seven * seven, -60.0f)) > 0.1f);
            engine.injectMidi (1, MidiMessage::controllerEvent (3, 7, 127));
            pump (60);
        }

        // Removing the output puts its input back on Main (1+2).
        engine.removeOutput (mon); settle();
        CHECK (engine.getSetup().outputs.size() == 1 && engine.getSetup().inputs[0].output == 0);
        const auto back = levels (8);
        std::printf ("     after removing Monitors: ch1 %.3f ch3 %.3f\n", back[0], back[2]);
        CHECK (back[0] > 0.02f && back[2] < 1e-4f);

        engine.injectMidi (0, MidiMessage::noteOff (1, 60));
        render (engine, 30);
    }

    // --- the built-in MIDI effects ----------------------------------------------------------
    {
        auto notesOn = [] (const MidiBuffer& b) { StringArray out; for (const auto m : b) if (m.getMessage().isNoteOn()) out.add (String (m.getMessage().getNoteNumber())); return out.joinIntoString (" "); };
        // Chord: a C becomes C E G, and letting go lets all three go.
        {
            EffectDef d; d.kind = EffectDef::Kind::chord; d.chordType = 0;
            auto fx = MidiFx::create (d);
            CHECK (fx != nullptr);
            MidiBuffer in, out;
            in.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 0);
            fx->process (in, out, 512, 48000.0, 120.0);
            std::printf ("     chord of C: %s\n", notesOn (out).toRawUTF8());
            CHECK (notesOn (out) == "60 64 67");
            in.clear(); out.clear();
            in.addEvent (MidiMessage::noteOff (1, 60), 0);
            fx->process (in, out, 512, 48000.0, 120.0);
            int offs = 0; for (const auto m : out) if (m.getMessage().isNoteOff()) ++offs;
            CHECK (offs == 3);
        }
        // Scale: C# in C major goes down to C; E stays.
        {
            EffectDef d; d.kind = EffectDef::Kind::scale; d.scaleRoot = 0; d.scaleMode = 0;
            auto fx = MidiFx::create (d);
            MidiBuffer in, out;
            in.addEvent (MidiMessage::noteOn (1, 61, (uint8) 100), 0);
            in.addEvent (MidiMessage::noteOn (1, 64, (uint8) 100), 10);
            fx->process (in, out, 512, 48000.0, 120.0);
            std::printf ("     C# and E in C major: %s\n", notesOn (out).toRawUTF8());
            CHECK (notesOn (out) == "60 64");
            // C and C# both land on C: letting go of C# must not cut the C that is still held.
            in.clear(); out.clear();
            in.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 0);
            fx->process (in, out, 512, 48000.0, 120.0);
            in.clear(); out.clear();
            in.addEvent (MidiMessage::noteOff (1, 61), 0);
            fx->process (in, out, 512, 48000.0, 120.0);
            int offs = 0; for (const auto m : out) if (m.getMessage().isNoteOff()) ++offs;
            in.clear(); out.clear();
            in.addEvent (MidiMessage::noteOff (1, 60), 0);
            fx->process (in, out, 512, 48000.0, 120.0);
            int offs2 = 0; for (const auto m : out) if (m.getMessage().isNoteOff()) ++offs2;
            std::printf ("     release C# then C: note-offs %d then %d\n", offs, offs2);
            CHECK (offs == 0 && offs2 == 1);
        }
        // Big band: in C major each key plays the chord on its step; a tap on C is C6, on D is Dm7, on G is G7, on C# a diminished 7th.
        {
            EffectDef d; d.kind = EffectDef::Kind::bigBand; d.scaleRoot = 0; d.scaleMode = 0;
            auto fx = MidiFx::create (d);
            auto chordFor = [&] (int key)
            {
                MidiBuffer in, out;
                in.addEvent (MidiMessage::noteOn (1, key, (uint8) 90), 0);
                fx->process (in, out, 64, 48000.0, 120.0);
                std::vector<int> ons;
                for (const auto m : out) if (m.getMessage().isNoteOn()) ons.push_back (m.getMessage().getNoteNumber());
                in.clear(); out.clear();
                in.addEvent (MidiMessage::noteOff (1, key), 0);
                fx->process (in, out, 64, 48000.0, 120.0);
                int offs = 0; for (const auto m : out) if (m.getMessage().isNoteOff()) ++offs;
                CHECK (offs == (int) ons.size());
                return ons;
            };
            CHECK (chordFor (60) == (std::vector<int> { 60, 64, 67, 69 }));     // C6
            CHECK (chordFor (62) == (std::vector<int> { 62, 65, 69, 72 }));     // Dm7
            CHECK (chordFor (67) == (std::vector<int> { 67, 71, 74, 77 }));     // G7
            CHECK (chordFor (71) == (std::vector<int> { 71, 74, 77, 81 }));     // Bm7b5
            CHECK (chordFor (61) == (std::vector<int> { 61, 64, 67, 70 }));     // C# dim7, the passing chord
            std::printf ("     big band in C: C6, Dm7, G7, Bm7b5, C#dim7 as expected\n");
            // The backing track starting mid-chord restarts timing only: the held chord still lets go with its key.
            {
                MidiBuffer in, out;
                in.addEvent (MidiMessage::noteOn (1, 60, (uint8) 90), 0);
                fx->process (in, out, 64, 48000.0, 120.0);
                fx->restartTiming();
                in.clear(); out.clear();
                in.addEvent (MidiMessage::noteOff (1, 60), 0);
                fx->process (in, out, 64, 48000.0, 120.0);
                int offs = 0; for (const auto m : out) if (m.getMessage().isNoteOff()) ++offs;
                CHECK (offs == 4);
            }

            // Shared notes: C6 and Am7 share C, E and G. Hold C, add A, let go of C: the band is still playing.
            {
                MidiBuffer in, out;
                in.addEvent (MidiMessage::noteOn (1, 60, (uint8) 90), 0);
                in.addEvent (MidiMessage::noteOn (1, 57, (uint8) 90), 10);
                fx->process (in, out, 64, 48000.0, 120.0);
                in.clear(); out.clear();
                in.addEvent (MidiMessage::noteOff (1, 60), 0);
                fx->process (in, out, 64, 48000.0, 120.0);
                int offs = 0; for (const auto m : out) if (m.getMessage().isNoteOff()) ++offs;
                in.clear(); out.clear();
                in.addEvent (MidiMessage::noteOff (1, 57), 0);
                fx->process (in, out, 64, 48000.0, 120.0);
                int offs2 = 0; for (const auto m : out) if (m.getMessage().isNoteOff()) ++offs2;
                std::printf ("     C6 then Am7, C let go: %d note-off (the A, theirs alone), then A let go: %d\n", offs, offs2);
                CHECK (offs == 1 && offs2 == 4);     // C E G are shared and keep sounding; only the A above lifts
            }

            // The song's key: in A minor the chords are C major's, so A is Am7 and C is C6; in F major, F is F6 and C is C7.
            d.scaleRoot = 9; d.scaleMode = 1; fx->configure (d);
            CHECK (chordFor (57) == (std::vector<int> { 57, 60, 64, 67 }));     // Am7
            CHECK (chordFor (60) == (std::vector<int> { 60, 64, 67, 69 }));     // C6
            d.scaleRoot = 5; d.scaleMode = 0; fx->configure (d);
            CHECK (chordFor (65) == (std::vector<int> { 65, 69, 72, 74 }));     // F6
            CHECK (chordFor (60) == (std::vector<int> { 60, 64, 67, 70 }));     // C7, the V
            // A blank step: the key alone. The player's own voicing: the II as a 9th, with the root below.
            d.scaleRoot = 0; d.bandChords[1].clear(); d.bandChords[2] = { -12, 0, 3, 7, 10, 14 }; fx->configure (d);
            CHECK (chordFor (61) == (std::vector<int> { 61 }));
            CHECK (chordFor (62) == (std::vector<int> { 50, 62, 65, 69, 72, 76 }));
            std::printf ("     keys and own voicings follow\n");

            // Reading the chords a player types: as notes or as names.
            std::vector<int> o;
            CHECK (MidiFx::parseBandChord ("C E G A", 0, o) && o == (std::vector<int> { 0, 4, 7, 9 }));
            CHECK (MidiFx::parseBandChord ("D F A C", 2, o) && o == (std::vector<int> { 0, 3, 7, 10 }));
            CHECK (MidiFx::parseBandChord ("Dm7", 2, o) && o == (std::vector<int> { 0, 3, 7, 10 }));
            CHECK (MidiFx::parseBandChord ("G7", 7, o) && o == (std::vector<int> { 0, 4, 7, 10 }));
            CHECK (MidiFx::parseBandChord ("Bm7b5", 11, o) && o == (std::vector<int> { 0, 3, 6, 10 }));
            CHECK (MidiFx::parseBandChord ("C#dim7", 1, o) && o == (std::vector<int> { 0, 3, 6, 9 }));
            CHECK (MidiFx::parseBandChord ("Db7", 7, o) && o == (std::vector<int> { -6, -2, 1, 4 }));     // a tritone substitute on the V
            CHECK (MidiFx::parseBandChord ("A C E G", 0, o) && o == (std::vector<int> { -3, 0, 4, 7 }));  // Am7 under the tonic, A below
            CHECK (MidiFx::parseBandChord ("Cmaj7", 0, o) && o == (std::vector<int> { 0, 4, 7, 11 }));
            CHECK (MidiFx::parseBandChord ("C6/9", 0, o) && o == (std::vector<int> { 0, 4, 7, 9, 14 }));
            CHECK (MidiFx::parseBandChord ("", 0, o) && o.empty());
            CHECK (! MidiFx::parseBandChord ("Hm7", 0, o));
            CHECK (! MidiFx::parseBandChord ("C E G nope", 0, o));
            CHECK (MidiFx::bandChordName (2, { 0, 3, 7, 10 }, false) == "Dm7");
            CHECK (MidiFx::bandChordName (0, { 0, 4, 7, 9 }, false) == "C6");
            CHECK (MidiFx::bandChordName (10, { 0, 4, 7, 10 }, true) == "Bb7");
            CHECK (MidiFx::bandChordName (7, { -6, -2, 1, 4 }, true) == "Db7");
            CHECK (MidiFx::bandChordName (0, { 0, 1, 2 }, false) == "C4 C#4 D4");             // no name for it: its notes
            CHECK (MidiFx::bandChordNotes (2, { 0, 3, 7, 10 }, false, false) == "D F A C");
            CHECK (MidiFx::bandChordNotes (2, { 0, 3, 7, 10 }, false) == "D4 F4 A4 C5");
            CHECK (MidiFx::bandChordNotes (7, { -12, 0, 4, 10, 14 }, false) == "G3 G4 B4 F5 A5");
            // Octaves place notes exactly, so a voicing reads back as it was written: the key under, the key on top, two octaves wide.
            CHECK (MidiFx::parseBandChord ("G3 C4 E4 A4", 0, o) && o == (std::vector<int> { -5, 0, 4, 9 }));
            CHECK (MidiFx::parseBandChord ("E3 G3 A3 C4", 0, o) && o == (std::vector<int> { -8, -5, -3, 0 }));
            CHECK (MidiFx::parseBandChord ("D3 D4 F#4 C5 E5", 2, o) && o == (std::vector<int> { -12, 0, 4, 10, 14 }));
            CHECK (MidiFx::parseBandChord ("C3 E4 G4 A5", 0, o) && o == (std::vector<int> { -12, 4, 7, 21 }));
            CHECK (MidiFx::parseBandChord ("G3 G4 B4 F5 A5", 7, o) && o == (std::vector<int> { -12, 0, 4, 10, 14 }));
            CHECK (MidiFx::parseBandChord ("Bb3 D4 F4", 10, o) && o == (std::vector<int> { -12, -8, -5 }));          // the key is Bb4: this whole chord sits under it
            CHECK (MidiFx::parseBandChord ("C4 E G A", 0, o) && o == (std::vector<int> { 0, 4, 7, 9 }));            // placed, then climbing
            CHECK (MidiFx::parseBandChord ("C6/E", 0, o) && o == (std::vector<int> { -8, 0, 4, 7, 9 }));            // slash bass under
            CHECK (MidiFx::parseBandChord ("G7/B", 7, o) && o == (std::vector<int> { -8, 0, 4, 7, 10 }));
            CHECK (! MidiFx::parseBandChord ("C4 E4 G4 A4 C5 E5 G5 A5 C6", 0, o));                               // nine: too many
            CHECK (! MidiFx::parseBandChord ("C9 E9", 0, o));                                                  // out of reach
            for (const auto& written : { "G3 C4 E4 A4", "E3 G3 A3 C4", "D3 D4 F#4 C5 E5", "C4 E4 G4 A4" })
            {
                std::vector<int> back;
                CHECK (MidiFx::parseBandChord (written, written[0] == 'D' ? 2 : 0, o) && MidiFx::parseBandChord (MidiFx::bandChordNotes (written[0] == 'D' ? 2 : 0, o, false), written[0] == 'D' ? 2 : 0, back) && back == o);
            }
            std::printf ("     voicings with octaves read back as written\n");
            // Review fixes: a slash bass under a chord off its own row, CM is major, a minor 6/9, Cb4 below C4.
            CHECK (MidiFx::parseBandChord ("F/C", 0, o) && o == (std::vector<int> { 0, 5, 9, 12 }));
            CHECK (MidiFx::parseBandChord ("Am7/C", 0, o) && o.front() == -12 + 0 + 0 && o[1] == -3);
            CHECK (MidiFx::parseBandChord ("CM", 0, o) && o == (std::vector<int> { 0, 4, 7 }));
            CHECK (MidiFx::parseBandChord ("CM6", 0, o) && o == (std::vector<int> { 0, 4, 7, 9 }));
            CHECK (MidiFx::parseBandChord ("Cm6/9", 0, o) && o == (std::vector<int> { 0, 3, 7, 9, 14 }));
            CHECK (MidiFx::parseBandChord ("Cb4 Eb4", 0, o) && o == (std::vector<int> { -1, 3 }));
            CHECK (MidiFx::parseBandChord ("B#3", 0, o) && o == (std::vector<int> { 0 }));
            CHECK (MidiFx::bandMajorRoot (9, 1) == 0 && MidiFx::bandMajorRoot (2, 3) == 0 && MidiFx::bandMajorRoot (7, 4) == 0 && MidiFx::bandMajorRoot (0, 0) == 0);
            EffectDef dd; dd.kind = EffectDef::Kind::bigBand; dd.scaleRoot = 0; dd.scaleMode = 0;
            CHECK (MidiFx::describe (dd) == "C major: C6 Dm7 Em7 F6 G7 Am7 Bm7b5");
            dd.scaleRoot = 9; dd.scaleMode = 1;
            CHECK (MidiFx::describe (dd) == "A natural minor: C6 Dm7 Em7 F6 G7 Am7 Bm7b5");
            std::printf ("     chords read and named: %s\n", MidiFx::describe (dd).toRawUTF8());
        }
        // Humanize: every note lands within the window, never early; its off follows by the note's own length; velocity stays within the band.
        {
            EffectDef d; d.kind = EffectDef::Kind::humanize; d.humanTimingMs = 25; d.humanVelocity = 10;
            auto fx = MidiFx::create (d);
            const int window = (int) (48000.0 * 0.025);
            std::map<int, int> onAt, offAt, velAt;
            int block = 0, maxDelay = 0, velSpread = 0, lateOffs = 0;
            auto run = [&] (MidiBuffer& in)
            {
                MidiBuffer out;
                fx->process (in, out, 256, 48000.0, 120.0);
                for (const auto m : out)
                {
                    const auto msg = m.getMessage();
                    const int t = block * 256 + m.samplePosition;
                    if (msg.isNoteOn()) { onAt[msg.getNoteNumber()] = t; velAt[msg.getNoteNumber()] = msg.getVelocity(); }
                    else if (msg.isNoteOff()) offAt[msg.getNoteNumber()] = t;
                }
                ++block;
                in.clear();
            };
            // 60 notes, one every 3 blocks (16 ms), each held 2 blocks: ons and offs interleave across blocks.
            std::map<int, int> keyDown;
            for (int n = 0; n < 60; ++n)
            {
                MidiBuffer in;
                in.addEvent (MidiMessage::noteOn (1, 30 + n, (uint8) 80), 100);
                keyDown[30 + n] = block * 256 + 100;
                run (in);
                run (in);
                in.addEvent (MidiMessage::noteOff (1, 30 + n), 100);
                run (in);
            }
            for (int i = 0; i < 20; ++i) { MidiBuffer in; run (in); }
            CHECK ((int) onAt.size() == 60 && (int) offAt.size() == 60);
            for (auto& [note, t] : onAt)
            {
                const int delay = t - keyDown[note];
                CHECK (delay >= 0 && delay <= window);
                maxDelay = jmax (maxDelay, delay);
                CHECK (offAt[note] - t == 512);                      // held two blocks, exactly, whatever the delay
                CHECK (velAt[note] >= 70 && velAt[note] <= 90);
                velSpread = jmax (velSpread, std::abs (velAt[note] - 80));
                if (offAt[note] < t) ++lateOffs;
            }
            std::printf ("     humanize: 60 notes, latest %d ms late (window 25), velocity spread %d (band 10), %d offs before their on\n", maxDelay * 1000 / 48000, velSpread, lateOffs);
            CHECK (maxDelay > window / 4 && velSpread > 2 && lateOffs == 0);        // it does something, and nothing impossible
            // The same pitch pressed twice while the first is still waiting: nothing passes anything, nothing is left on.
            {
                auto h = MidiFx::create (d);
                std::vector<std::pair<int, bool>> seen;      // (time, on)
                int b = 0;
                for (int round = 0; round < 40; ++round)
                {
                    MidiBuffer in, out;
                    if (round == 0) { in.addEvent (MidiMessage::noteOn (1, 64, (uint8) 80), 0); in.addEvent (MidiMessage::noteOn (1, 64, (uint8) 80), 10);
                                      in.addEvent (MidiMessage::noteOff (1, 64), 20); in.addEvent (MidiMessage::noteOff (1, 64), 30); }
                    h->process (in, out, 256, 48000.0, 120.0);
                    for (const auto m : out) if (m.getMessage().isNoteOnOrOff()) seen.push_back ({ b * 256 + m.samplePosition, m.getMessage().isNoteOn() });
                    ++b;
                }
                int depth = 0; bool ordered = true;
                for (auto& e : seen) { depth += e.second ? 1 : -1; if (depth < 0) ordered = false; }
                std::printf ("     humanize, one pitch twice: %d events, %s\n", (int) seen.size(), ordered && depth == 0 ? "in order, all let go" : "OUT OF ORDER");
                CHECK (seen.size() == 4 && ordered && depth == 0 && seen.back().second == false);
            }
            // Switched to "on time, as played": straight through.
            d.humanTimingMs = 0; d.humanVelocity = 0; fx->configure (d);
            MidiBuffer in, out;
            in.addEvent (MidiMessage::noteOn (1, 60, (uint8) 77), 5);
            fx->process (in, out, 256, 48000.0, 120.0);
            int straight = 0; for (const auto m : out) if (m.getMessage().isNoteOn() && m.samplePosition == 5 && m.getMessage().getVelocity() == 77) ++straight;
            CHECK (straight == 1);
        }
        // Phrase: a three-note MIDI file (C D E, a beat each), imported and played from G: G A B at 0, 0.5 and 1.0 s at 120.
        {
            MidiMessageSequence seq;
            const int tpq = 480;
            for (int i = 0; i < 3; ++i)
            {
                seq.addEvent (MidiMessage::noteOn (1, 60 + 2 * i, (uint8) 100), 960 + tpq * i);     // the file starts after two silent beats
                seq.addEvent (MidiMessage::noteOff (1, 60 + 2 * i), 960 + tpq * i + tpq / 2);      // half-beat notes
            }
            seq.updateMatchedPairs();
            MidiFile mf; mf.setTicksPerQuarterNote (tpq); mf.addTrack (seq);
            TemporaryFile tmp (".mid");
            { FileOutputStream os (tmp.getFile()); mf.writeTo (os, 1); }
            EffectDef d; d.kind = EffectDef::Kind::phrase;
            String error;
            CHECK (MidiFx::importPhrase (tmp.getFile(), d, error));
            CHECK (d.phraseNotes.size() == 3 && std::abs (d.phraseNotes[0].beat) < 1e-5f && std::abs (d.phraseNotes[1].beat - 1.0f) < 1e-5f
                   && std::abs (d.phraseNotes[2].length - 0.5f) < 1e-5f && d.phraseNotes[2].note == 64 && std::abs (d.phraseBeats - 3.0f) < 1e-5f);
            std::printf ("     imported %s: %s\n", tmp.getFile().getFileName().toRawUTF8(), MidiFx::describe (d).toRawUTF8());
            CHECK (MidiFx::describe (d).contains ("3 notes over 3 beats, starting on the key"));
            File bad = tmp.getFile().getSiblingFile ("not-midi.mid"); bad.replaceWithText ("hello");
            EffectDef junk; junk.kind = EffectDef::Kind::phrase;
            CHECK (! MidiFx::importPhrase (bad, junk, error) && error.isNotEmpty() && junk.phraseNotes.empty());
            bad.deleteFile();

            auto play = [&] (const EffectDef& def, int key, int keyUpBlock, int blocks)
            {
                auto fx = MidiFx::create (def);
                std::vector<std::pair<double, String>> events;
                MidiBuffer in, out;
                in.addEvent (MidiMessage::noteOn (1, key, (uint8) 100), 0);
                for (int b = 0; b < blocks; ++b)
                {
                    if (b == keyUpBlock) in.addEvent (MidiMessage::noteOff (1, key), 0);
                    out.clear();
                    fx->process (in, out, 480, 48000.0, 120.0);     // 10 ms blocks
                    in.clear();
                    for (const auto m : out)
                    {
                        const auto msg = m.getMessage();
                        events.push_back ({ (b * 480 + m.samplePosition) / 48000.0, (msg.isNoteOn() ? "on " : msg.isNoteOff() ? "off " : "? ") + String (msg.getNoteNumber()) });
                    }
                }
                return events;
            };
            auto text = [] (const std::vector<std::pair<double, String>>& ev) { StringArray a; for (auto& e : ev) a.add (e.second + "@" + String (e.first, 2)); return a.joinIntoString (" "); };
            // A tap (key up after 30 ms): the whole phrase still plays, moved to G.
            auto ev = play (d, 67, 3, 200);
            std::printf ("     phrase from G, tapped: %s\n", text (ev).toRawUTF8());
            CHECK (text (ev) == "on 67@0.00 off 67@0.25 on 69@0.50 off 69@0.75 on 71@1.00 off 71@1.25");
            // Stop with the key: let go at 0.6 s, the A is cut and the B never comes.
            d.phraseStop = true;
            ev = play (d, 67, 60, 200);
            std::printf ("     phrase stopped at 0.6 s: %s\n", text (ev).toRawUTF8());
            CHECK (text (ev) == "on 67@0.00 off 67@0.25 on 69@0.50 off 69@0.60");
            // Repeat while held: round again at 1.5 s, until the key lifts at 2.0 s (the second pass's A plays through to its end; nothing after the end of the pass).
            d.phraseStop = false; d.phraseLoop = true;
            ev = play (d, 67, 200, 400);
            std::printf ("     phrase repeating, key up at 2.0 s: %s\n", text (ev).toRawUTF8());
            CHECK (text (ev) == "on 67@0.00 off 67@0.25 on 69@0.50 off 69@0.75 on 71@1.00 off 71@1.25 on 67@1.50 off 67@1.75 on 69@2.00 off 69@2.25 on 71@2.50 off 71@2.75");
            // As written: the key C4 plays the file as it is; the velocity of the key scales the file's.
            d.phraseLoop = false; d.phraseRoot = 60;
            ev = play (d, 60, 3, 200);
            CHECK (text (ev) == "on 60@0.00 off 60@0.25 on 62@0.50 off 62@0.75 on 64@1.00 off 64@1.25");
            {
                auto fx = MidiFx::create (d);
                MidiBuffer in, out;
                in.addEvent (MidiMessage::noteOn (1, 62, (uint8) 50), 0);
                fx->process (in, out, 64, 48000.0, 120.0);
                int vel = -1, note = -1; for (const auto m : out) if (m.getMessage().isNoteOn()) { vel = m.getMessage().getVelocity(); note = m.getMessage().getNoteNumber(); }
                CHECK (note == 62 && vel == 50);     // file velocity 100 at a key velocity of 50
            }
            // Two keys at once share no notes here, and each voice ends on its own.
            {
                auto fx = MidiFx::create (d);
                MidiBuffer in, out;
                in.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 0);
                in.addEvent (MidiMessage::noteOn (1, 67, (uint8) 100), 0);
                int ons = 0, offs = 0;
                for (int b = 0; b < 200; ++b)
                {
                    out.clear();
                    fx->process (in, out, 480, 48000.0, 120.0);
                    in.clear();
                    for (const auto m : out) { if (m.getMessage().isNoteOn()) ++ons; if (m.getMessage().isNoteOff()) ++offs; }
                }
                CHECK (ons == 6 && offs == 6);
            }
            // Fixed pitch (drums): every key plays it as written, at the key's velocity.
            {
                d.phraseRoot = -2;
                auto fx = MidiFx::create (d);
                MidiBuffer in, out;
                in.addEvent (MidiMessage::noteOn (1, 40, (uint8) 100), 0);
                fx->process (in, out, 64, 48000.0, 120.0);
                int note = -1; for (const auto m : out) if (m.getMessage().isNoteOn()) note = m.getMessage().getNoteNumber();
                CHECK (note == 60);
                CHECK (MidiFx::describe (d).contains ("as written whatever the key"));
            }
            // The built-in library: found from the build tree, every file reads, each folder has some.
            {
                const auto folder = MidiFx::phraseLibraryFolder();
                CHECK (folder.isDirectory());
                int files = 0, groups = 0;
                for (const auto& g : folder.findChildFiles (File::findDirectories, false))
                {
                    ++groups;
                    for (const auto& f : g.findChildFiles (File::findFiles, false, "*.mid"))
                    {
                        EffectDef lib; lib.kind = EffectDef::Kind::phrase;
                        String err;
                        const bool ok = MidiFx::importPhrase (f, lib, err);
                        if (! ok) std::printf ("       %s: %s\n", f.getFileName().toRawUTF8(), err.toRawUTF8());
                        CHECK (ok && ! lib.phraseNotes.empty() && lib.phraseBeats >= 1.0f && lib.phraseBeats <= 4.0f);
                        ++files;
                    }
                }
                std::printf ("     built-in phrases: %d in %d folders at %s\n", files, groups, folder.getFullPathName().toRawUTF8());
                CHECK (groups == 3 && files >= 30);
            }
            // A key that went straight through while nothing was loaded still gets its note-off after a file is loaded.
            {
                EffectDef empty; empty.kind = EffectDef::Kind::phrase;
                auto fx = MidiFx::create (empty);
                MidiBuffer in, out;
                in.addEvent (MidiMessage::noteOn (1, 64, (uint8) 100), 0);
                fx->process (in, out, 64, 48000.0, 120.0);
                fx->configure (d);
                in.clear(); out.clear();
                in.addEvent (MidiMessage::noteOff (1, 64), 0);
                fx->process (in, out, 64, 48000.0, 120.0);
                int offs = 0; for (const auto m : out) if (m.getMessage().isNoteOff() && m.getMessage().getNoteNumber() == 64) ++offs;
                CHECK (offs == 1);
            }
            // Nothing loaded: the keys pass straight through.
            {
                EffectDef empty; empty.kind = EffectDef::Kind::phrase;
                auto fx = MidiFx::create (empty);
                MidiBuffer in, out;
                in.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 3);
                in.addEvent (MidiMessage::noteOff (1, 60), 40);
                fx->process (in, out, 64, 48000.0, 120.0);
                int n = 0; for (const auto m : out) ++n;
                CHECK (n == 2);
            }
        }
        // Run: E pressed in C major, an octave up in one beat (120 bpm: eight notes across half a second), the last one held.
        {
            EffectDef d; d.kind = EffectDef::Kind::run; d.scaleRoot = 0; d.scaleMode = 0; d.runNotes = 8; d.runBeats = 1.0f; d.runDown = 0; d.runToKey = 0;
            auto fx = MidiFx::create (d);
            MidiBuffer in, out;
            in.addEvent (MidiMessage::noteOn (1, 64, (uint8) 90), 0);
            StringArray played;
            int offs = 0;
            for (int b = 0; b < (int) (1.2 * 48000 / 512); ++b)
            {
                out.clear();
                fx->process (in, out, 512, 48000.0, 120.0);
                in.clear();
                for (const auto m : out) { if (m.getMessage().isNoteOn()) played.add (String (m.getMessage().getNoteNumber())); if (m.getMessage().isNoteOff()) ++offs; }
            }
            std::printf ("     run from E: %s (%d note-offs so far)\n", played.joinIntoString (" ").toRawUTF8(), offs);
            CHECK (played.joinIntoString (" ") == "64 65 67 69 71 72 74 76" && offs == 7);     // the last note is still held
            // The timing: all eight within the beat, the last one landing on it (0.5 s at 120).
            {
                auto fx2 = MidiFx::create (d);
                MidiBuffer in2, out2;
                in2.addEvent (MidiMessage::noteOn (1, 64, (uint8) 90), 0);
                int ons = 0, lastAt = -1, blocks = 0;
                for (int b = 0; b < 60; ++b)
                {
                    out2.clear();
                    fx2->process (in2, out2, 512, 48000.0, 120.0);
                    in2.clear();
                    for (const auto m : out2) if (m.getMessage().isNoteOn()) { ++ons; lastAt = b * 512 + m.samplePosition; }
                    ++blocks;
                }
                std::printf ("     octave in a beat: %d notes, the last at %.3f s\n", ons, lastAt / 48000.0);
                CHECK (ons == 8 && std::abs (lastAt / 48000.0 - 0.5) < 0.012);
            }
            // From the 3rd above the key: C pressed starts the run on E.
            {
                EffectDef d3 = d; d3.runDegree = 2;
                auto fx3 = MidiFx::create (d3);
                MidiBuffer in3, out3;
                in3.addEvent (MidiMessage::noteOn (1, 60, (uint8) 90), 0);
                StringArray got;
                for (int b = 0; b < 60; ++b) { out3.clear(); fx3->process (in3, out3, 512, 48000.0, 120.0); in3.clear(); for (const auto m : out3) if (m.getMessage().isNoteOn()) got.add (String (m.getMessage().getNoteNumber())); }
                std::printf ("     C pressed, from the 3rd: %s\n", got.joinIntoString (" ").toRawUTF8());
                CHECK (got.joinIntoString (" ") == "64 65 67 69 71 72 74 76");
            }
            in.addEvent (MidiMessage::noteOff (1, 64), 0);
            out.clear();
            fx->process (in, out, 512, 48000.0, 120.0);
            int lastOff = -1; for (const auto m : out) if (m.getMessage().isNoteOff()) lastOff = m.getMessage().getNoteNumber();
            CHECK (lastOff == 76);
            // Up to the key: the same eight notes land on E.
            d.runToKey = 1;
            fx->configure (d);
            in.clear(); out.clear(); played.clear();
            in.addEvent (MidiMessage::noteOn (1, 76, (uint8) 90), 0);
            for (int b = 0; b < (int) (1.2 * 48000 / 512); ++b)
            {
                out.clear();
                fx->process (in, out, 512, 48000.0, 120.0);
                in.clear();
                for (const auto m : out) if (m.getMessage().isNoteOn()) played.add (String (m.getMessage().getNoteNumber()));
            }
            std::printf ("     run up to E: %s\n", played.joinIntoString (" ").toRawUTF8());
            CHECK (played.joinIntoString (" ") == "64 65 67 69 71 72 74 76");
        }
        // Run, as on the rig: C phrygian, two octaves up from the key in a beat at 90, 128-sample blocks.
        // Every note the run starts must be let go, whether the key is held past the run or released mid-way.
        {
            auto runAndCount = [&] (double releaseAfterSeconds, bool holdLast, int& stuck, StringArray& played, double& lastOffSeconds)
            {
                /* Counted in the order the plugin hears them: a note-off that
                   comes before its note-on (the old build stepped the run at
                   sample 0, ahead of the key that started it) leaves that
                   note sounding, and counts as stuck here too. */
                EffectDef d; d.kind = EffectDef::Kind::run; d.scaleRoot = 0; d.scaleMode = 6; d.runNotes = 15; d.runBeats = 1.0f; d.runDown = 0; d.runToKey = 0; d.runHoldLast = holdLast;
                auto fx = MidiFx::create (d);
                lastOffSeconds = -1.0;
                int held[128] = {};
                MidiBuffer in, out;
                in.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 37);
                const int blocks = (int) (3.0 * 48000 / 128);
                const int releaseBlock = (int) (releaseAfterSeconds * 48000 / 128);
                for (int b = 0; b < blocks; ++b)
                {
                    if (b == releaseBlock) in.addEvent (MidiMessage::noteOff (1, 60), 5);
                    out.clear();
                    fx->process (in, out, 128, 48000.0, 90.0);
                    in.clear();
                    for (const auto m : out)
                    {
                        const auto msg = m.getMessage();
                        if (msg.isNoteOn()) { ++held[msg.getNoteNumber()]; played.add (String (msg.getNoteNumber())); }
                        else if (msg.isNoteOff() && held[msg.getNoteNumber()] > 0) { --held[msg.getNoteNumber()]; lastOffSeconds = (b * 128 + m.samplePosition) / 48000.0; }
                        else if (msg.isNoteOff()) held[msg.getNoteNumber()] = 1000;   // let go before it began: it will never be let go
                    }
                }
                stuck = 0;
                for (int n = 0; n < 128; ++n) if (held[n] != 0) ++stuck;
            };
            // The run takes a beat at 90: 0.667 s, a step of 0.048 s. The last note lifts with the key, or a step after landing.
            int stuck = -1; StringArray played; double lastOff = 0.0;
            runAndCount (2.0, true, stuck, played, lastOff);
            std::printf ("     rig run (key held 2 s): %s -> %d note(s) left sounding, last lifted at %.3f s\n", played.joinIntoString (" ").toRawUTF8(), stuck, lastOff);
            CHECK (played.size() == 15 && stuck == 0 && lastOff > 1.9);
            played.clear();
            runAndCount (2.0, false, stuck, played, lastOff);
            std::printf ("     rig run (key held 2 s, not holding the last): %d notes played, %d left sounding, last lifted at %.3f s\n", played.size(), stuck, lastOff);
            CHECK (played.size() == 15 && stuck == 0 && lastOff > 0.65 && lastOff < 0.8);
            played.clear();
            runAndCount (0.2, true, stuck, played, lastOff);
            std::printf ("     rig run (key let go at 0.2 s): %d notes played, %d left sounding, last lifted at %.3f s\n", played.size(), stuck, lastOff);
            CHECK (played.size() == 15 && stuck == 0 && lastOff > 0.65 && lastOff < 0.8);       // a tap plays the whole run
            played.clear();
            runAndCount (0.01, true, stuck, played, lastOff);
            std::printf ("     rig run (key let go at once): %d notes played, %d left sounding, last lifted at %.3f s\n", played.size(), stuck, lastOff);
            CHECK (played.size() == 15 && stuck == 0 && lastOff > 0.65 && lastOff < 0.8);
        }
        // Arpeggiator: C and E held, eighths at 120 (a step every 0.25 s): four notes a second, alternating.
        {
            EffectDef d; d.kind = EffectDef::Kind::arpeggio; d.arpRate = 1; d.arpMode = 0; d.arpOctaves = 1; d.arpGate = 50;
            auto fx = MidiFx::create (d);
            MidiBuffer in, out;
            in.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 0);
            in.addEvent (MidiMessage::noteOn (1, 64, (uint8) 100), 0);
            StringArray played;
            int offs = 0;
            for (int b = 0; b < 48000 / 512; ++b)
            {
                out.clear();
                fx->process (in, out, 512, 48000.0, 120.0);
                in.clear();
                for (const auto m : out) { if (m.getMessage().isNoteOn()) played.add (String (m.getMessage().getNoteNumber())); if (m.getMessage().isNoteOff()) ++offs; }
            }
            std::printf ("     arpeggio over a second: %s (%d note-offs)\n", played.joinIntoString (" ").toRawUTF8(), offs);
            CHECK (played.size() == 4 && played[0] == "60" && played[1] == "64" && played[2] == "60" && offs >= 3);
        }
    }

    // --- stretching a recording: longer without changing its pitch, higher without its length --
    {
        auto tone = [] (double freq, double seconds, double rate)
        {
            std::vector<int16_t> v ((size_t) (seconds * rate));
            for (size_t i = 0; i < v.size(); ++i) v[i] = (int16_t) (20000.0 * std::sin (MathConstants<double>::twoPi * freq * (double) i / rate));
            return v;
        };
        auto hzOf = [] (const std::vector<int16_t>& v, double rate)
        {
            // Zero crossings over the middle half, where the stretch has settled.
            int crossings = 0;
            const size_t a = v.size() / 4, b = v.size() * 3 / 4;
            for (size_t i = a + 1; i < b; ++i) if ((v[i - 1] < 0) != (v[i] < 0)) ++crossings;
            return crossings * rate / (2.0 * (double) (b - a));
        };
        const double rate = 48000.0;
        auto l = tone (440.0, 2.0, rate), r = l;
        stretchAndShift (l, r, 1.5, 0.0, rate);
        std::printf ("     stretched x1.5: %.3f s, %.1f Hz\n", l.size() / rate, hzOf (l, rate));
        CHECK (std::abs (l.size() / rate - 3.0) < 0.01 && std::abs (hzOf (l, rate) - 440.0) < 5.0);
        l = tone (440.0, 2.0, rate); r = l;
        stretchAndShift (l, r, 1.0, 12.0, rate);
        std::printf ("     up an octave: %.3f s, %.1f Hz\n", l.size() / rate, hzOf (l, rate));
        CHECK (std::abs (l.size() / rate - 2.0) < 0.01 && std::abs (hzOf (l, rate) - 880.0) < 10.0);
        l = tone (440.0, 2.0, rate); r = l;
        stretchAndShift (l, r, 0.8, -2.0, rate);
        std::printf ("     shorter and down two: %.3f s, %.1f Hz\n", l.size() / rate, hzOf (l, rate));
        CHECK (std::abs (l.size() / rate - 1.6) < 0.01 && std::abs (hzOf (l, rate) - 392.0) < 6.0);
    }

    // --- recording the gig: every output to WAV, the keyboards to MIDI ----------------------
    {
        engine.setSetupFile (scratchDir.getChildFile ("rec.stagerig.json"));
        String err;
        CHECK (engine.startRecording (err));
        CHECK (engine.isRecording());
        const auto take = engine.getRecordingFolder();
        std::printf ("     recording into %s\n", take.getFullPathName().toRawUTF8());
        CHECK (take.isDirectory() && take.getParentDirectory().getFileName() == "rec.stagerig.recordings");
        engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
        AudioBuffer<float> rec (2, kBlock);
        for (int b = 0; b < 40; ++b) { rec.clear(); engine.renderBlockForTesting (rec.getArrayOfWritePointers(), 2, kBlock); }
        engine.injectMidi (0, MidiMessage::noteOff (1, 60));
        for (int b = 0; b < 10; ++b) { rec.clear(); engine.renderBlockForTesting (rec.getArrayOfWritePointers(), 2, kBlock); }
        pump (600);                                                      // the MIDI queue is drained twice a second
        engine.stopRecording();
        CHECK (! engine.isRecording());

        const auto wav = take.getChildFile ("Main.wav");
        const auto mid = take.getChildFile ("keyboards.mid");
        CHECK (wav.existsAsFile() && mid.existsAsFile());
        AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<AudioFormatReader> reader (fm.createReaderFor (wav));
        CHECK (reader != nullptr);
        if (reader != nullptr)
        {
            AudioBuffer<float> got (2, (int) reader->lengthInSamples);
            reader->read (&got, 0, got.getNumSamples(), 0, true, true);
            std::printf ("     take: %lld samples at %.0f Hz, peak %.3f\n", (long long) reader->lengthInSamples, reader->sampleRate, got.getMagnitude (0, got.getNumSamples()));
            CHECK (reader->lengthInSamples == 50 * kBlock && reader->numChannels == 2 && got.getMagnitude (0, got.getNumSamples()) > 0.01f);
        }
        MidiFile mf;
        FileInputStream in (mid);
        CHECK (mf.readFrom (in) && mf.getNumTracks() >= 1);
        int notes = 0;
        for (int t = 0; t < mf.getNumTracks(); ++t)
            for (const auto* e : *mf.getTrack (t)) if (e->message.isNoteOn()) ++notes;
        std::printf ("     MIDI file: %d tracks, %d note-ons\n", mf.getNumTracks(), notes);
        CHECK (notes == 1);
        engine.setSetupFile (File());
    }

    // --- Ableton Link: the tempo shared with a peer ----------------------------------------
    {
        engine.setLinkEnabled (true);
        CHECK (engine.isLinkEnabled());
        engine.setTempoBpm (100.0);
        // Link resets its session on its own thread as it comes on: the rig's tempo must survive that.
        for (int i = 0; i < 40 && std::abs (engine.getLinkTempoForTesting() - 100.0) > 0.01; ++i) pump (50);
        const double sessionTempo = engine.getLinkTempoForTesting();
        std::printf ("     Link on: session tempo %.3f\n", sessionTempo);
        CHECK (std::abs (sessionTempo - 100.0) < 0.01);

        /* A second Link in this process is another app on the network. It
           needs multicast on the loopback, which not every machine allows;
           without it there is nothing to test but no fault either. */
        ableton::Link peer (120.0);
        peer.enable (true);
        for (int i = 0; i < 120 && engine.getLinkPeers() == 0; ++i) pump (50);
        if (engine.getLinkPeers() > 0)
        {
            auto st = peer.captureAppSessionState();
            st.setTempo (95.0, peer.clock().micros());
            peer.commitAppSessionState (st);
            for (int i = 0; i < 100 && std::abs (engine.getTempoBpm() - 95.0) > 0.01; ++i) pump (50);
            std::printf ("     a peer set 95: the rig is at %.1f\n", engine.getTempoBpm());
            CHECK (std::abs (engine.getTempoBpm() - 95.0) < 0.01);
            engine.setTempoBpm (110.0);
            for (int i = 0; i < 100 && std::abs (peer.captureAppSessionState().tempo() - 110.0) > 0.01; ++i) pump (50);
            std::printf ("     the rig set 110: the peer is at %.1f\n", peer.captureAppSessionState().tempo());
            CHECK (std::abs (peer.captureAppSessionState().tempo() - 110.0) < 0.01);
        }
        else
            std::printf ("     (no Link peer found on this machine's loopback: skipping the two-way check)\n");
        peer.enable (false);
        engine.setLinkEnabled (false);
        engine.setTempoBpm (120.0);
    }

    // --- MIDI clock out: 24 ticks a beat at the rig's tempo ---------------------------------
    {
        /* No device need be open for the ticks to be made; a port that is not
           there just drops them. 120 bpm is 48 ticks a second; 60 bpm, 24. */
        AudioBuffer<float> out2 (2, kBlock);
        const int blocksPerSecond = roundToInt (engine.getSampleRate() / kBlock);
        auto second = [&]
        {
            const int before = engine.getClockTicksForTesting();
            for (int b = 0; b < blocksPerSecond; ++b)
            {
                out2.clear();
                engine.renderBlockForTesting (out2.getArrayOfWritePointers(), 2, kBlock);
            }
            return engine.getClockTicksForTesting() - before;
        };
        CHECK (second() == 0);                                           // nothing asked for: no ticks
        engine.setTempoBpm (120.0);
        engine.setClockTo ({ "Nowhere", "test:nowhere" }, true);
        CHECK (engine.sendsClockTo ("test:nowhere") && engine.getClockOut().size() == 1);
        const int at120 = second();
        engine.setTempoBpm (60.0);
        const int at60 = second();
        const double seconds = blocksPerSecond * kBlock / engine.getSampleRate();
        std::printf ("     MIDI clock: %d ticks in %.3f s at 120 bpm, %d at 60\n", at120, seconds, at60);
        CHECK (std::abs (at120 - 48.0 * seconds) <= 1.0 && std::abs (at60 - 24.0 * seconds) <= 1.0);
        engine.setClockTo ({ "Nowhere", "test:nowhere" }, false);
        CHECK (second() == 0 && engine.getClockOut().empty());
        engine.setTempoBpm (120.0);
    }

    // --- finding the tempo of a recording -------------------------------------------------
    {
        /* A drum pattern at an awkward tempo with a lead-in before the first
           downbeat: kick on 1 and 3, snare on 2 and 4, hats on the eighths,
           a little noise under it all. The detector should find the tempo
           to within a fraction of a bpm and the first downbeat to within a
           frame or two. */
        auto drumWav = [&] (const File& f, double bpm, double leadIn, double seconds, double rate)
        {
            f.deleteFile();
            WavAudioFormat wav;
            std::unique_ptr<OutputStream> os (f.createOutputStream().release());
            std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (os.get(), rate, 2, 16, {}, 0));
            if (w == nullptr) return false;
            os.release();
            AudioBuffer<float> b (2, (int) (seconds * rate));
            b.clear();
            Random rng (7);
            const double beat = 60.0 / bpm;
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const float noise = 0.02f * (rng.nextFloat() * 2.0f - 1.0f);
                b.addSample (0, i, noise); b.addSample (1, i, noise);
            }
            auto hit = [&] (double t, double freq, double len, float amp, bool noise)
            {
                const int s = (int) (t * rate), n = (int) (len * rate);
                for (int i = 0; i < n && s + i < b.getNumSamples(); ++i)
                {
                    const float env = std::exp (-6.0f * (float) i / (float) n);
                    const float v = amp * env * (noise ? (rng.nextFloat() * 2.0f - 1.0f) : (float) std::sin (MathConstants<double>::twoPi * freq * i / rate));
                    b.addSample (0, s + i, v); b.addSample (1, s + i, v);
                }
            };
            for (int k = 0; leadIn + k * beat < seconds; ++k)
            {
                const double t = leadIn + k * beat;
                if (k % 4 == 0)      hit (t, 60.0, 0.15, 0.9f, false);       // a heavy kick on 1
                else if (k % 2 == 0) hit (t, 60.0, 0.15, 0.6f, false);       // a lighter one on 3
                else                 hit (t, 0.0, 0.10, 0.5f, true);         // snare on 2 and 4
                hit (t, 0.0, 0.03, 0.2f, true);                              // hats
                hit (t + beat / 2, 0.0, 0.03, 0.2f, true);
            }
            return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        };
        AudioFormatManager fm;
        fm.registerBasicFormats();
        for (auto [bpm, leadIn, rate] : { std::tuple { 123.4, 0.37, 44100.0 }, std::tuple { 97.0, 1.21, 48000.0 }, std::tuple { 168.0, 0.0, 48000.0 } })
        {
            const auto f = scratchDir.getChildFile ("drums.wav");
            CHECK (drumWav (f, bpm, leadIn, 40.0, rate));
            std::unique_ptr<AudioFormatReader> reader (fm.createReaderFor (f));
            CHECK (reader != nullptr);
            const auto t0 = Time::getMillisecondCounterHiRes();
            const auto est = detectTempo (*reader, 4);
            std::printf ("     %.1f bpm, first beat %.3f s: found %.2f bpm, first beat %.3f s, confidence %.2f, in %.0f ms\n",
                         bpm, leadIn, est.bpm, est.firstBeat, est.confidence, Time::getMillisecondCounterHiRes() - t0);
            CHECK (est.ok);
            /* Half or double the tempo is the one honest ambiguity -- a
               backbeat at 168 is also a slow 84 -- so either octave passes,
               and the downbeat must then fall on one of the heavy kicks. */
            const double ratio = est.bpm / bpm;
            const bool octave = std::abs (ratio - 1.0) < 0.003 || std::abs (ratio - 0.5) < 0.0015 || std::abs (ratio - 2.0) < 0.006;
            CHECK (octave);
            /* At the true tempo the downbeat must be a heavy kick, which
               comes round every four beats; heard at half the tempo the grid
               may sit on the snares instead, so any beat will do there. */
            const double heavy = (std::abs (ratio - 1.0) < 0.003 ? 4.0 : 1.0) * 60.0 / bpm;
            const double off = std::fmod (est.firstBeat - leadIn + 100.0 * heavy, heavy);
            CHECK (jmin (off, heavy - off) < 0.025);
            CHECK (est.confidence > 0.3);
        }
        // Too short to say anything about.
        {
            const auto f = scratchDir.getChildFile ("short.wav");
            CHECK (drumWav (f, 120.0, 0.0, 2.0, 48000.0));
            std::unique_ptr<AudioFormatReader> reader (fm.createReaderFor (f));
            CHECK (reader != nullptr && ! detectTempo (*reader, 4).ok);
        }
    }

    // --- backing tracks: stems from disk onto their outputs, a count-in, stop, end ----------
    {
        // A setup file of our own, so the track's folder is a scratch one.
        const auto setupFile = scratchDir.getChildFile ("tracks.stagerig.json");
        engine.setSetupFile (setupFile);
        const auto folder = engine.getChartsFolder();
        folder.createDirectory();

        // A one-second 440 Hz stem at 44.1 kHz, so the player has to resample to the engine's rate.
        auto writeTone = [] (const File& f, double seconds, double rate, float amp)
        {
            f.deleteFile();
            WavAudioFormat wav;
            std::unique_ptr<OutputStream> os (f.createOutputStream().release());
            std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (os.get(), rate, 1, 16, {}, 0));
            if (w == nullptr) return false;
            os.release();
            AudioBuffer<float> b (1, (int) (seconds * rate));
            for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (0, i, amp * (float) std::sin (MathConstants<double>::twoPi * 440.0 * i / rate));
            return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        };
        CHECK (writeTone (folder.getChildFile ("stem.wav"), 1.0, 44100.0, 0.5f));

        const int click = engine.addOutput ("Click", 2, 3);
        const int song = engine.addSong ("Backing");
        engine.addSongStem (song, { "stem.wav", "Tone", 0, 0.0f });
        auto waitReady = [&]
        {
            for (int i = 0; i < 200 && ! engine.isTrackReady(); ++i) pump (25);
            return engine.isTrackReady();
        };
        engine.selectSong (song);
        CHECK (engine.getTrackSong() == song && engine.hasTrack());
        CHECK (waitReady());
        CHECK (! engine.isTrackRunning());
        std::printf ("     track length %.2f s\n", engine.getTrackLength());
        CHECK (std::abs (engine.getTrackLength() - 1.0) < 0.01);

        AudioBuffer<float> out4 (4, kBlock);
        auto levels = [&] (int blocks)
        {
            std::array<float, 4> peak {};
            for (int b = 0; b < blocks; ++b)
            {
                out4.clear();
                engine.renderBlockForTesting (out4.getArrayOfWritePointers(), 4, kBlock);
                for (int c = 0; c < 4; ++c) peak[(size_t) c] = jmax (peak[(size_t) c], out4.getMagnitude (c, 0, kBlock));
            }
            return peak;
        };

        CHECK (levels (4)[0] < 1e-4f);                                   // armed, not playing: silent
        engine.startTrack();
        CHECK (engine.isTrackRunning());
        const auto playing = levels (8);
        std::printf ("     playing: main %.3f click %.3f\n", playing[0], playing[2]);
        CHECK (playing[0] > 0.4f && playing[1] > 0.4f && playing[2] < 1e-4f);   // mono stem on both sides of Main
        CHECK (engine.getTrackPosition() > 0.05);

        // A level change takes effect while it plays.
        engine.setSongStem (song, 0, { "stem.wav", "Tone", 0, -6.0f });
        const auto quieter = levels (4);
        std::printf ("     at -6 dB: main %.3f\n", quieter[0]);
        CHECK (std::abs (quieter[0] - 0.25f) < 0.03f);

        engine.stopTrack();
        levels (4);                                                      // the fade
        pump (50);
        CHECK (! engine.isTrackRunning());
        CHECK (levels (4)[0] < 1e-4f);

        // Count-in: two beats at 120 bpm on the click output, then the stem on Main.
        engine.setSongTempo (song, 120.0);
        engine.setSongCountIn (song, 2, click);
        engine.selectSong (song);
        CHECK (waitReady());
        engine.startTrack();
        levels (1);                                                      // the first block begins the count-in
        CHECK (engine.isTrackCountingIn());
        const auto countIn = levels (40);                                // 0.43 s at 48 kHz: inside the count-in
        std::printf ("     count-in: main %.3f click %.3f\n", countIn[0], countIn[2]);
        CHECK (countIn[2] > 0.2f && countIn[0] < 1e-4f);
        const auto after = levels (80);                                  // past the second beat
        std::printf ("     after the count-in: main %.3f\n", after[0]);
        CHECK (after[0] > 0.1f);

        // It ends by itself at the end of the file.
        levels (120);
        pump (50);
        std::printf ("     ended: running %d\n", (int) engine.isTrackRunning());
        CHECK (! engine.isTrackRunning());

        /* The click through the song: at 120 bpm a beat is 24000 samples, a
           click 1440, so of 47 blocks of 512 (one beat) the first three carry
           the click and the rest are silent. Without the count-in it starts
           on beat 1 of the file. */
        engine.setSongCountIn (song, 0, click);
        engine.setSongClick (song, true, 4);
        CHECK (engine.hasTrack());
        engine.startTrack();
        const auto onBeat = levels (3), offBeat = levels (40), nextBeat = levels (6);
        std::printf ("     click through the song: on the beat %.3f, between %.3f, next beat %.3f\n", onBeat[2], offBeat[2], nextBeat[2]);
        CHECK (onBeat[2] > 0.2f && offBeat[2] < 1e-4f && nextBeat[2] > 0.2f);
        CHECK (onBeat[0] > 0.2f);                                        // the stem plays too (at -6 dB from above)
        engine.stopTrack();
        levels (4);
        pump (50);

        // Moving the first beat moves the click: a quarter of a second in, the first click comes 23 blocks later.
        engine.setSongFirstBeat (song, 0.25);
        engine.startTrack();
        const auto early = levels (20), onFirst = levels (6);
        std::printf ("     first beat at 0.25 s: before %.3f, on it %.3f\n", early[2], onFirst[2]);
        CHECK (early[2] < 1e-4f && onFirst[2] > 0.2f);
        engine.stopTrack();
        levels (4);
        pump (50);
        engine.setSongFirstBeat (song, 0.0);
        engine.setSongClick (song, false, 4);
        engine.setSongCountIn (song, 2, click);                          // as the save below expects

        /* A file with silence at its head starts at its first sound: half a
           second of nothing, then the tone, and Play is loud at once. A start
           point of the song's own wins over that. */
        {
            auto writeLeadIn = [&] (const File& f)
            {
                f.deleteFile();
                WavAudioFormat wav;
                std::unique_ptr<OutputStream> os (f.createOutputStream().release());
                std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (os.get(), 48000.0, 1, 16, {}, 0));
                if (w == nullptr) return false;
                os.release();
                AudioBuffer<float> b (1, 72000);
                b.clear();
                for (int i = 24000; i < 72000; ++i) b.setSample (0, i, 0.5f * (float) std::sin (MathConstants<double>::twoPi * 440.0 * i / 48000.0));
                return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            };
            CHECK (writeLeadIn (folder.getChildFile ("leadin.wav")));
            const int lead = engine.addSong ("Lead-in");
            engine.addSongStem (lead, { "leadin.wav", {}, 0, 0.0f });
            engine.selectSong (lead);
            CHECK (waitReady());
            std::printf ("     lead-in: cue %.3f s\n", engine.getTrackCue());
            CHECK (std::abs (engine.getTrackCue() - 0.49) < 0.02);
            engine.startTrack();
            const auto atOnce = levels (3);
            std::printf ("     lead-in: at once %.3f, position %.3f\n", atOnce[0], engine.getTrackPosition());
            CHECK (atOnce[0] > 0.4f && engine.getTrackPosition() > 0.5);
            engine.stopTrack();
            levels (4);
            pump (50);
            engine.setSongStart (lead, 1.0);
            CHECK (std::abs (engine.getTrackCue() - 1.0) < 0.01);
            engine.startTrack();
            levels (1);
            std::printf ("     start point 1.0: position %.3f\n", engine.getTrackPosition());
            CHECK (engine.getTrackPosition() >= 1.0);
            engine.stopTrack();
            levels (4);
            pump (50);
            engine.selectSong (song);
            CHECK (waitReady());
        }

        /* Files that know their tempo follow the song's: the one-second
           stem, made "at 120", plays for two seconds when the song is at 60,
           and every time on the panel still reads in the file's seconds. */
        {
            const int slow = engine.addSong ("Half speed");
            engine.addSongStem (slow, { "stem.wav", "Tone", 0, 0.0f });
            engine.setSongTrackBpm (slow, 120.0);
            engine.setSongTempo (slow, 60.0);
            engine.selectSong (slow);
            CHECK (waitReady());
            std::printf ("     stretched song: time scale %.2f, length reads %.2f s\n", engine.getTrackTimeScale(), engine.getTrackLength());
            CHECK (std::abs (engine.getTrackTimeScale() - 2.0) < 1e-6 && std::abs (engine.getTrackLength() - 1.0) < 0.02);
            engine.startTrack();
            levels (150);                                                 // 1.6 s: past where the file would have ended
            CHECK (engine.isTrackRunning());
            std::printf ("     still playing at 1.6 s: position reads %.2f s (file time)\n", engine.getTrackPosition());
            CHECK (engine.getTrackPosition() > 0.7 && engine.getTrackPosition() < 1.0);
            engine.stopTrack();
            levels (4);
            pump (50);
            engine.selectSong (song);
            CHECK (waitReady());
        }

        // A song with no files but a click is a metronome: Play runs until stopped.
        {
            const int metro = engine.addSong ("Metronome");
            engine.setSongTempo (metro, 120.0);
            engine.setSongCountIn (metro, 0, click);
            engine.setSongClick (metro, true, 4);
            engine.selectSong (metro);
            CHECK (engine.hasTrack());
            CHECK (waitReady());
            engine.startTrack();
            const auto m1 = levels (3), m2 = levels (40), m3 = levels (6);
            std::printf ("     metronome: %.3f %.3f %.3f, running %d\n", m1[2], m2[2], m3[2], (int) engine.isTrackRunning());
            CHECK (m1[2] > 0.2f && m2[2] < 1e-4f && m3[2] > 0.2f && engine.isTrackRunning());
            CHECK (m1[0] < 1e-4f);                                       // nothing on Main
            // Its click turned off mid-play: nothing left to play, so it stops rather than sit "playing".
            engine.setSongClick (metro, false, 4);
            levels (2);
            pump (50);
            std::printf ("     click off while playing: running %d\n", (int) engine.isTrackRunning());
            CHECK (! engine.isTrackRunning());
            engine.setSongClick (metro, true, 4);
            CHECK (waitReady());
            engine.startTrack();
            levels (2);
            CHECK (engine.isTrackRunning());
            engine.stopTrack();
            levels (4);
            pump (50);
            CHECK (! engine.isTrackRunning());
            engine.selectSong (song);
            CHECK (waitReady());
        }

        // Sections: a three-second stem whose level steps up each second -- A 0.2, B 0.5, C 0.8.
        {
            const auto f = folder.getChildFile ("sections.wav");
            f.deleteFile();
            WavAudioFormat wav;
            std::unique_ptr<OutputStream> os (f.createOutputStream().release());
            std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (os.get(), 48000.0, 1, 16, {}, 0));
            CHECK (w != nullptr);
            os.release();
            AudioBuffer<float> b (1, 3 * 48000);
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const float amp = i < 48000 ? 0.2f : (i < 96000 ? 0.5f : 0.8f);
                b.setSample (0, i, amp * (float) std::sin (MathConstants<double>::twoPi * 440.0 * i / 48000.0));
            }
            CHECK (w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()));
        }
        const int secSong = engine.addSong ("Sections");
        engine.addSongStem (secSong, { "sections.wav", {}, 0, 0.0f });
        /* Section B carries a cue sheet: Upper goes to program 3 when the
           track enters it; nothing happens again while the vamp goes round. */
        {
            ActionDef toThree; toThree.kind = ActionDef::Kind::program; toThree.input = 0; toThree.program = 3;
            std::vector<MarkerDef> ms = { { "A", 0.0, false }, { "B", 1.0, true }, { "C", 2.0, false } };
            ms[1].actions = { toThree };
            engine.setSongMarkers (secSong, ms);
        }
        engine.selectProgram (0, 0);
        engine.selectSong (secSong);
        CHECK (waitReady());
        CHECK (engine.getTrackSectionCount() == 3 && engine.getTrackSectionName (1) == "B" && engine.isTrackSectionVamp (1));

        auto peakFor = [&] (double seconds) { return levels ((int) (seconds * 48000.0 / kBlock))[0]; };
        engine.startTrack();
        peakFor (1.4);                                                   // into B
        CHECK (engine.getTrackSection() == 1 && engine.isTrackVamping());
        const auto inVamp = peakFor (1.5);                               // B's end has come and gone: still B
        std::printf ("     vamping: section %d, level %.2f, position %.2f s\n", engine.getTrackSection(), inVamp, engine.getTrackPosition());
        CHECK (engine.getTrackSection() == 1 && std::abs (inVamp - 0.5f) < 0.05f && engine.getTrackPosition() < 2.0);
        pump (30);
        std::printf ("     entering B put Upper on program %d\n", engine.getSetup().inputs[0].currentProgram);
        CHECK (engine.getSetup().inputs[0].currentProgram == 3);
        engine.selectProgram (0, 0);
        levels (100);                                                    // the vamp goes round again: no second firing
        pump (30);
        CHECK (engine.getSetup().inputs[0].currentProgram == 0);

        engine.advanceTrack();                                           // the pedal: release
        peakFor (1.1);                                                   // to the end of this pass, then C
        CHECK (engine.getTrackSection() == 2);
        std::printf ("     released into C: section %d, level %.2f\n", engine.getTrackSection(), levels (4)[0]);

        engine.queueTrackSection (0);                                    // at C's end, back to A instead of stopping
        peakFor (1.1);
        std::printf ("     queued A: section %d, running %d\n", engine.getTrackSection(), (int) engine.isTrackRunning());
        CHECK (engine.isTrackRunning() && engine.getTrackSection() == 0);
        CHECK (std::abs (levels (4)[0] - 0.2f) < 0.05f);

        engine.stopTrack();
        levels (4); pump (50);
        CHECK (! engine.isTrackRunning());

        // Cued while stopped: Play starts at C.
        engine.queueTrackSection (2);
        engine.startTrack();
        levels (2);
        std::printf ("     cued C: section %d, position %.2f s\n", engine.getTrackSection(), engine.getTrackPosition());
        CHECK (engine.getTrackSection() == 2 && engine.getTrackPosition() >= 2.0);
        engine.stopTrack();
        levels (4); pump (50);

        /* A MIDI file plays into the backing-track input. The first file
           makes that input, and gives the song a program there named after
           it, with the General MIDI player: the file plays as it is. */
        {
            MidiMessageSequence seq;
            seq.addEvent (MidiMessage::noteOn  (1, 60, (uint8) 100), 0);       // ticks; 960 per quarter, 120 bpm
            seq.addEvent (MidiMessage::noteOff (1, 60), 960 * 3);              // held for 1.5 s
            seq.addEvent (MidiMessage::programChange (1, 5), 10);              // for the GM player only
            seq.updateMatchedPairs();
            MidiFile mf;
            mf.setTicksPerQuarterNote (960);
            mf.addTrack (seq);
            const auto f = folder.getChildFile ("part.mid");
            f.deleteFile();
            FileOutputStream os (f);
            CHECK (os.openedOk() && mf.writeTo (os));
        }
        CHECK (engine.getTrackInput() < 0);
        const int midiSong = engine.addSong ("MIDI part");
        StemDef part; part.file = "part.mid";
        engine.addSongStem (midiSong, part);
        const int track = engine.getTrackInput();
        CHECK (track >= 0 && engine.getSetup().inputs[(size_t) track].isTrack() && engine.getSetup().inputs[(size_t) track].channel == 0);
        const int trackProg = engine.getSongs()[(size_t) midiSong].programFor (track);
        CHECK (trackProg >= 0);
        auto trackProgram = [&]() -> const ProgramDef& { return engine.getSetup().inputs[(size_t) track].programs[(size_t) trackProg]; };
        CHECK (trackProgram().name == "MIDI part" && trackProgram().slots.size() == 1 && trackProgram().slots[0].isGm() && trackProgram().slots[0].hearsAll());

        const int programBefore = engine.getSetup().inputs[0].currentProgram;
        engine.selectSong (midiSong);
        CHECK (waitReady() && engine.hasTrack());
        CHECK (engine.getSetup().inputs[(size_t) track].currentProgram == trackProg);
        settle();                                                        // the GM player reads its soundfont
        std::printf ("     MIDI track length %.2f s\n", engine.getTrackLength());
        CHECK (std::abs (engine.getTrackLength() - 1.5) < 0.05);

        auto playAndStop = [&] (int blocks)
        {
            engine.startTrack();
            const float l = levels (blocks)[0];                          // about a quarter of a second in
            engine.stopTrack();
            levels (4); pump (50); levels (60);
            return l;
        };

        const bool gmAvailable = GmSynth::unavailableReason().isEmpty();
        if (! gmAvailable)
            std::printf ("     skip General MIDI check (%s)\n", GmSynth::unavailableReason().toRawUTF8());
        else
        {
            CHECK (engine.isPluginAlive (track, trackProg, 0));
            const float gmLevel = playAndStop (24);
            std::printf ("     General MIDI player (%s): %.3f\n", GmSynth::findSoundfont().getFileName().toRawUTF8(), gmLevel);
            CHECK (gmLevel > 0.01f);
            const float gmAfter = levels (8)[0];
            std::printf ("     after stopping the GM player: %.5f\n", gmAfter);
            CHECK (gmAfter < 1e-3f);
        }
        CHECK (engine.getSetup().inputs[0].currentProgram == programBefore);    // the file's program change is not Upper's business

        /* The part to Upper's sounds instead -- an instrument that hands the
           notes to that keyboard's program -- and the GM player stops
           hearing it: one instrument per part, as the editor assigns them. */
        CHECK (engine.addInputSlot (track, trackProg, 0));
        engine.assignPart (track, trackProg, 1, 1);
        CHECK (! trackProgram().slots[0].hears (1) && trackProgram().slots[1].hears (1) && trackProgram().slots[1].isInput()
               && trackProgram().slots[1].targetInputName == "Upper" && trackProgram().slots[1].displayName() == "Upper's sounds");
        CHECK (engine.partInstrument (track, trackProg, 1) == 1 && engine.partInstrument (track, trackProg, 2) == 0);
        engine.startTrack();
        const float viaKeys = levels (24)[0];
        std::printf ("     channel 1 to Upper's sounds: %.3f\n", viaKeys);
        CHECK (viaKeys > 0.01f);
        CHECK (engine.getSetup().inputs[0].currentProgram == programBefore);    // its program change was dropped on the way
        engine.stopTrack();                                              // with the note still held
        levels (4); pump (50);
        levels (120);                                                    // the synth's release
        const float afterStop = levels (8)[0];
        std::printf ("     after stopping with a note held: %.5f\n", afterStop);
        CHECK (afterStop < 1e-3f);

        /* The part on one of Upper's programs by name -- program 6, "Pad",
           which Upper itself is not on. It is loaded for the part and plays
           beside Upper's own program; Upper's keys still play that one. */
        {
            engine.clearProgram (0, 6);
            engine.setProgramName (0, 6, "Pad");
            CHECK (engine.addSlot (0, 6, synth, err));
            CHECK (! engine.isProgramLoaded (0, 6));
            CHECK (engine.addInputSlot (track, trackProg, 0, 6));
            const int padSlot = (int) trackProgram().slots.size() - 1;
            CHECK (trackProgram().slots[(size_t) padSlot].targetProgram == 6 && trackProgram().slots[(size_t) padSlot].displayName() == "Upper: Pad");
            engine.assignPart (track, trackProg, 1, padSlot);
            CHECK (engine.isProgramLoaded (0, 6));                      // named by a current program: kept loaded
            settle();
            CHECK (engine.isPluginAlive (0, 6, 0));
            CHECK (engine.getSetup().inputs[0].currentProgram == programBefore);
            engine.startTrack();
            const float viaPad = levels (24)[0];
            std::printf ("     channel 1 to Upper: Pad (Upper on another program): %.3f\n", viaPad);
            CHECK (viaPad > 0.01f);
            CHECK (engine.getSetup().inputs[0].currentProgram == programBefore);
            engine.stopTrack();
            levels (4); pump (50); levels (120);
            const float padQuiet = levels (8)[0];
            std::printf ("     after stopping: %.5f\n", padQuiet);
            CHECK (padQuiet < 1e-3f);
            // Renamed, the part follows; housekeeping leaves the program loaded while a part plays it.
            engine.setProgramName (0, 6, "Big pad");
            CHECK (trackProgram().slots[(size_t) padSlot].displayName() == "Upper: Big pad");
            pump (900);
            CHECK (engine.isProgramLoaded (0, 6));
            // Nothing names it any more: it is let go like any idle program.
            engine.removeSlot (track, trackProg, padSlot);
            pump (900);
            CHECK (! engine.isProgramLoaded (0, 6));
        }

        // Nothing plays the part: silence.
        engine.assignPart (track, trackProg, 1, -1);
        CHECK (engine.partInstrument (track, trackProg, 1) < 0);
        const float nobody = playAndStop (24);
        std::printf ("     no instrument on channel 1: %.5f\n", nobody);
        CHECK (nobody < 1e-3f);

        if (gmAvailable)
        {
            // Back on the GM player, with its mixer: volume 0 or mute silences the part; an instrument of our choosing still sounds.
            engine.assignPart (track, trackProg, 1, 0);
            auto quietWith = [&] (SlotDef::GmChannel c) { engine.setGmChannel (track, trackProg, 0, 1, c); return playAndStop (24); };
            SlotDef::GmChannel atZeroC; atZeroC.volume = 0;
            SlotDef::GmChannel mutedC; mutedC.muted = true;
            SlotDef::GmChannel stringsC; stringsC.program = 48;          // String Ensemble 1, over the file's
            const float atZero = quietWith (atZeroC), muted = quietWith (mutedC), strings = quietWith (stringsC), asFile = quietWith ({});
            std::printf ("     GM mixer: volume 0 %.4f, muted %.4f, strings %.3f, as the file has it %.3f\n", atZero, muted, strings, asFile);
            CHECK (atZero < 3e-3f && muted < 3e-3f && strings > 0.005f && asFile > 0.01f);
            engine.setGmChannel (track, trackProg, 0, 1, stringsC);

            // Saved and read back: the track input, the song's program there, the GM mixer and the keyboard instrument.
            TemporaryFile gf (".stagerig.json");
            CHECK (engine.captureSetup().saveToFile (gf.getFile()).wasOk());
            Setup gback;
            CHECK (Setup::loadFromFile (gf.getFile(), gback).wasOk());
            CHECK (gback.trackInput() == track && gback.songs[(size_t) midiSong].programFor (track) == trackProg);
            const auto& tp = gback.inputs[(size_t) track].programs[(size_t) trackProg];
            CHECK (tp.slots.size() == 2 && tp.slots[0].isGm() && tp.slots[0].gmChannels[0].program == 48 && tp.slots[0].hears (1)
                   && tp.slots[1].isInput() && tp.slots[1].targetInput == 0 && ! tp.slots[1].hears (1) && tp.slots[1].listensToText() == "No channel");
            CHECK (gback.songs[(size_t) midiSong].stems[0].isMidi() && ! gback.songs[(size_t) midiSong].stems[0].hasLegacyRoutes);

            // The editor's file round trip: a note added and saved, the first version kept, the tempo and program change intact.
            {
                const auto f = folder.getChildFile ("part.mid");
                MidiDoc doc;
                String err;
                CHECK (doc.load (f, err));
                CHECK (doc.notes.size() == 1 && doc.notes[0].key == 60 && doc.firstProgram (1) == 5);
                const int before = (int) doc.notes.size();
                MidiDoc::Note n; n.channel = 1; n.key = 67; n.velocity = 90; n.start = 960; n.end = 1920; n.track = doc.trackFor (1);
                doc.notes.push_back (n);
                doc.notes[0].velocity = 55;
                CHECK (doc.save (f, err));
                CHECK (f.getSiblingFile ("part.orig.mid").existsAsFile());
                MidiDoc back;
                CHECK (back.load (f, err));
                bool found = false;
                for (auto& x : back.notes) if (x.key == 67 && x.start == 960 && x.end == 1920 && x.velocity == 90) found = true;
                CHECK ((int) back.notes.size() == before + 1 && found && back.firstProgram (1) == 5);
                CHECK (std::abs (back.tickToSeconds (960) - 0.5) < 1e-6);          // 120 bpm: a quarter is half a second
                engine.reloadTrack (midiSong);
                CHECK (waitReady());
                std::printf ("     edited MIDI file: %d notes, length %.2f s\n", (int) back.notes.size(), engine.getTrackLength());
            }
        }

        // --- one keyboard layering the other, both ways: a layer, never an echo -------------------
        {
            engine.selectProgram (0, 0); settle();                      // Upper on its synth
            engine.clearProgram (1, 7);
            engine.selectProgram (1, 7); settle();
            CHECK (engine.addInputSlot (1, 7, 0));                      // Lower's program plays Upper's sounds
            CHECK (engine.addInputSlot (0, 0, 1));                      // and Upper's plays Lower's: a cycle
            const int upperSlots = (int) engine.getSetup().inputs[0].programs[0].slots.size();
            CHECK (engine.isPluginAlive (1, 7, 0) && engine.getSetup().inputs[1].programs[7].slots[0].displayName() == "Upper's sounds");
            engine.injectMidi (1, MidiMessage::noteOn (2, 64, (uint8) 100));   // played on Lower
            const float layered = levels (8)[0];
            std::printf ("     Lower's note through Upper's sounds: %.3f\n", layered);
            CHECK (layered > 0.01f);
            engine.injectMidi (1, MidiMessage::noteOff (2, 64));
            levels (120);
            const float quiet = levels (8)[0];
            std::printf ("     after letting go, with both keyboards layering each other: %.5f\n", quiet);
            CHECK (quiet < 1e-3f);
            engine.removeSlot (0, 0, upperSlots - 1);
            engine.clearProgram (1, 7);
        }

        // Removing an input: every song's programs shift with the inputs that are left.
        {
            const int extra = engine.addInput ("Extra");
            const int shifted = engine.addSong ("Shifted");
            engine.setSongProgram (shifted, 0, 3);
            engine.setSongProgram (shifted, extra, 9);
            CHECK (engine.getSongs()[(size_t) shifted].programFor (extra) == 9);
            engine.removeInput (extra);
            CHECK (engine.getSongs()[(size_t) shifted].programFor (0) == 3 && (int) engine.getSongs()[(size_t) shifted].programs.size() == extra);
            engine.removeSong (shifted);
        }

        // A song with no stems arms nothing.
        const int bare = engine.addSong ("No track");
        engine.selectSong (bare);
        CHECK (! engine.hasTrack());

        // Round trip through the setup file.
        TemporaryFile tf (".stagerig.json");
        CHECK (engine.captureSetup().saveToFile (tf.getFile()).wasOk());
        Setup back;
        CHECK (Setup::loadFromFile (tf.getFile(), back).wasOk());
        CHECK (back.songs[(size_t) song].stems.size() == 1 && back.songs[(size_t) song].stems[0].file == "stem.wav");
        CHECK (back.songs[(size_t) song].countInBeats == 2 && back.songs[(size_t) song].countInOutput == click);
        engine.removeOutput (click);
    }

    // --- routing: per-slot receive channel, and an external instrument on a MIDI port ----------
    {
        struct Catcher : public MidiInputCallback
        {
            std::mutex m;
            std::vector<MidiMessage> got;
            void handleIncomingMidiMessage (MidiInput*, const MidiMessage& msg) override { std::lock_guard<std::mutex> l (m); got.push_back (msg); }
            std::vector<MidiMessage> take() { std::lock_guard<std::mutex> l (m); auto r = got; got.clear(); return r; }
        } catcher;
        /* ALSA's Midi Through port gives back whatever is sent to it: send to
           its output side, listen on its input side. (A virtual port of our
           own would do, but JUCE does not list a process's own ports.) */
        MidiDeviceInfo port;
        std::unique_ptr<MidiInput> virt;
        for (const auto& d : MidiOutput::getAvailableDevices())
            if (d.name.containsIgnoreCase ("Midi Through")) port = d;
        if (port.identifier.isNotEmpty())
            for (const auto& d : MidiInput::getAvailableDevices())
                if (d.name == port.name) { virt = MidiInput::openDevice (d.identifier, &catcher); break; }
        if (virt == nullptr) port = {};
        else { virt->start(); std::printf ("     looping through %s (%s)\n", port.name.toRawUTF8(), port.identifier.toRawUTF8()); }
        if (port.identifier.isEmpty())
        {
            std::printf ("     skip external-instrument check (no Midi Through port here)\n");
        }
        else
        {
            const int oldChannel = engine.getSetup().inputs[0].channel;
            engine.setInputChannel (0, 0);                                   // Omni: the slot sorts the channels
            engine.clearProgram (0, 90);
            engine.selectProgram (0, 90); settle();
            CHECK (engine.addExternalSlot (0, 90, port));
            CHECK (engine.isPluginAlive (0, 90, 0));
            engine.setSlotPatch (0, 90, 0, 0, 3, 17);                        // playing: sent at once
            auto renderAndWait = [&] { AudioBuffer<float> b (2, kBlock); for (int i = 0; i < 6; ++i) { b.clear(); engine.renderBlockForTesting (b.getArrayOfWritePointers(), 2, kBlock); } Thread::sleep (150); return catcher.take(); };
            auto has = [] (const std::vector<MidiMessage>& v, std::function<bool (const MidiMessage&)> f) { for (auto& m : v) if (f (m)) return true; return false; };

            auto patch = renderAndWait();
            CHECK (has (patch, [] (const MidiMessage& m) { return m.isProgramChange() && m.getProgramChangeNumber() == 17 && m.getChannel() == 1; }));
            CHECK (has (patch, [] (const MidiMessage& m) { return m.isController() && m.getControllerNumber() == 32 && m.getControllerValue() == 3; }));

            engine.injectMidi (0, MidiMessage::noteOn (5, 64, (uint8) 90));   // plays on channel 1, as the slot is set
            auto notes = renderAndWait();
            std::printf ("     external instrument received %d messages\n", (int) notes.size());
            CHECK (has (notes, [] (const MidiMessage& m) { return m.isNoteOn() && m.getNoteNumber() == 64 && m.getChannel() == 1; }));
            engine.injectMidi (0, MidiMessage::noteOff (5, 64));
            renderAndWait();

            // Listens to channel 2 only.
            engine.setSlotListensTo (0, 90, 0, 1 << 1);
            engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 90));
            CHECK (! has (renderAndWait(), [] (const MidiMessage& m) { return m.isNoteOn(); }));
            engine.injectMidi (0, MidiMessage::noteOn (2, 62, (uint8) 90));
            CHECK (has (renderAndWait(), [] (const MidiMessage& m) { return m.isNoteOn() && m.getNoteNumber() == 62; }));

            // PANIC reaches the hardware too.
            engine.panic();
            CHECK (has (renderAndWait(), [] (const MidiMessage& m) { return m.isAllNotesOff() || (m.isController() && m.getControllerNumber() == 123); }));

            // Its sound back through an interface input joins the program's mix.
            engine.setSlotReturn (0, 90, 0, 0, -1);
            AudioBuffer<float> tone (1, kBlock), outBuf (2, kBlock);
            for (int i = 0; i < kBlock; ++i) tone.setSample (0, i, 0.4f * (float) std::sin (MathConstants<double>::twoPi * 330.0 * i / 48000.0));
            float back = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                outBuf.clear();
                engine.renderBlockForTesting (tone.getArrayOfReadPointers(), 1, outBuf.getArrayOfWritePointers(), 2, kBlock);
                back = jmax (back, outBuf.getMagnitude (0, 0, kBlock));
            }
            std::printf ("     audio return in the mix: %.3f (input peak 0.4)\n", back);
            CHECK (std::abs (back - 0.4f) < 0.05f);

            // Saved and read back.
            TemporaryFile ef (".stagerig.json");
            CHECK (engine.captureSetup().saveToFile (ef.getFile()).wasOk());
            Setup back2;
            CHECK (Setup::loadFromFile (ef.getFile(), back2).wasOk());
            const auto& sd = back2.inputs[0].programs[90].slots[0];
            CHECK (sd.isExternal() && sd.midiOutIdentifier == port.identifier && sd.patch == 17 && sd.bankLsb == 3
                   && sd.hears (2) && ! sd.hears (1) && sd.listensToText() == "Channel 2" && sd.returnLeft == 0 && sd.returnRight == -1);

            engine.clearProgram (0, 90);

            // --- a slot's MIDI chain: a processor plugin and a send, before the sound ---
            perf::ClapPluginFormat clapFmt;
            OwnedArray<PluginDescription> found;
            clapFmt.findAllTypesForFile (found, STAGERIG_TEST_NOTE_EFFECT);
            CHECK (found.size() == 1);
            if (found.size() == 1)
            {
                const int midiChain = ProgramDef::midiChainOf (0);
                engine.clearProgram (0, 91);
                engine.selectProgram (0, 91); settle();
                CHECK (engine.addExternalSlot (0, 91, port));               // the sound: plays on channel 1
                String fxErr;
                CHECK (engine.addEffect (0, 91, midiChain, *found[0], fxErr));
                settle();
                auto* octave = engine.getPlugin (0, 91, midiChain, 0);
                CHECK (octave != nullptr && octave->isAlive());
                renderAndWait();

                auto noteOn = [] (int n, int ch) { return [n, ch] (const MidiMessage& m) { return m.isNoteOn() && m.getNoteNumber() == n && m.getChannel() == ch; }; };
                auto play = [&] (int note) { engine.injectMidi (0, MidiMessage::noteOn (1, note, (uint8) 90)); auto got = renderAndWait();
                                             engine.injectMidi (0, MidiMessage::noteOff (1, note)); renderAndWait(); return got; };

                // Through the processor: the sound hears the octave, not the key.
                auto got = play (60);
                std::printf ("     MIDI processor -> external: %d messages\n", (int) got.size());
                CHECK (has (got, noteOn (72, 1)));
                CHECK (! has (got, noteOn (60, 1)));

                // Bypassed, the notes go past it.
                engine.setEffectBypassed (0, 91, midiChain, 0, true);
                got = play (60);
                CHECK (has (got, noteOn (60, 1)) && ! has (got, noteOn (72, 1)));
                engine.setEffectBypassed (0, 91, midiChain, 0, false);

                // A send after it gets the octave too, on its own channel; the sound still plays.
                CHECK (engine.addMidiSend (0, 91, 0, port));
                engine.setMidiSendChannel (0, 91, 0, 1, 5);
                CHECK (engine.isPluginAlive (0, 91, midiChain, 1));
                got = play (62);
                CHECK (has (got, noteOn (74, 5)) && has (got, noteOn (74, 1)));

                // Moved ahead of the processor, the send hears the key as played.
                engine.moveEffect (0, 91, midiChain, 1, 0);
                got = play (62);
                CHECK (has (got, noteOn (62, 5)) && has (got, noteOn (74, 1)) && ! has (got, noteOn (74, 5)));

                // The send recalls a patch on its channel.
                engine.setMidiSendPatch (0, 91, 0, 0, -1, -1, 9);
                got = renderAndWait();
                CHECK (has (got, [] (const MidiMessage& m) { return m.isProgramChange() && m.getProgramChangeNumber() == 9 && m.getChannel() == 5; }));

                // PANIC gets past the processor to the sound.
                engine.panic();
                CHECK (has (renderAndWait(), [] (const MidiMessage& m) { return m.getChannel() == 1 && (m.isAllNotesOff() || (m.isController() && m.getControllerNumber() == 123)); }));

                /* The transpose shifts the keys on the way in -- the send hears
                   62, the sound 74 through the octave -- and a note held across
                   a change is let go at the key it started on. */
                engine.setTranspose (2);
                engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 90));
                got = renderAndWait();
                CHECK (has (got, noteOn (62, 5)) && has (got, noteOn (74, 1)) && ! has (got, noteOn (60, 5)));
                engine.setTranspose (0);
                engine.injectMidi (0, MidiMessage::noteOff (1, 60));
                got = renderAndWait();
                CHECK (has (got, [] (const MidiMessage& m) { return m.isNoteOff() && m.getNoteNumber() == 62 && m.getChannel() == 5; }));
                CHECK (has (got, [] (const MidiMessage& m) { return m.isNoteOff() && m.getNoteNumber() == 74 && m.getChannel() == 1; }));
                CHECK (engine.getTranspose() == 0);

                TemporaryFile cf (".stagerig.json");
                CHECK (engine.captureSetup().saveToFile (cf.getFile()).wasOk());
                Setup back3;
                CHECK (Setup::loadFromFile (cf.getFile(), back3).wasOk());
                const auto& chainBack = back3.inputs[0].programs[91].slots[0].midiEffects;
                CHECK (chainBack.size() == 2 && chainBack[0].isSend() && chainBack[0].channel == 5 && chainBack[0].patch == 9
                       && chainBack[0].portIdentifier == port.identifier && ! chainBack[1].isSend()
                       && chainBack[1].plugin.name == found[0]->name);

                // Removing the slot takes its chain with it.
                engine.removeSlot (0, 91, 0);
                CHECK (engine.getPlugin (0, 91, midiChain, 0) == nullptr);
                engine.clearProgram (0, 91);
            }
            // --- a built-in run before the sound, as on a rig: the last note holds while the key is held ---
            {
                engine.clearProgram (0, 92);
                engine.selectProgram (0, 92); settle();
                CHECK (engine.addExternalSlot (0, 92, port));
                CHECK (engine.addBuiltInMidiEffect (0, 92, 0, EffectDef::Kind::run));
                EffectDef rd; rd.kind = EffectDef::Kind::run; rd.scaleRoot = 0; rd.scaleMode = 0; rd.runNotes = 15; rd.runBeats = 1.0f; rd.runHoldLast = true;
                engine.setBuiltInMidiEffect (0, 92, 0, 0, rd);
                const bool linkWasOn = engine.isLinkEnabled();
                engine.setLinkEnabled (false);
                engine.setTempoBpm (90.0);
                renderAndWait();
                catcher.take();
                const double sr = engine.getSampleRate();
                /* Rendered at the pace of real time: the port's sender orders
                   what it is handed by the clock, as a device needs, so blocks
                   rendered in a burst would reach it out of order. */
                auto renderSeconds = [&] (double seconds)
                {
                    AudioBuffer<float> b (2, kBlock);
                    const double t0 = Time::getMillisecondCounterHiRes(), blockMs = 1000.0 * kBlock / sr;
                    for (int i = 0, n = (int) (seconds * sr / kBlock); i < n; ++i)
                    {
                        b.clear();
                        engine.renderBlockForTesting (b.getArrayOfWritePointers(), 2, kBlock);
                        while (Time::getMillisecondCounterHiRes() < t0 + (i + 1) * blockMs) Thread::sleep (1);
                    }
                    Thread::sleep (200);
                    return catcher.take();
                };
                engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 90));
                auto held = renderSeconds (2.0);                              // the key stays down well past the beat
                int ons = 0, lastOn = -1; String order;
                for (auto& m : held) { if (m.isNoteOn()) { ++ons; lastOn = m.getNoteNumber(); order << m.getNoteNumber() << " "; } else if (m.isNoteOff()) order << "-" << m.getNoteNumber() << " "; }
                const bool lastLetGo = has (held, [lastOn] (const MidiMessage& m) { return m.isNoteOff() && m.getNoteNumber() == lastOn; });
                std::printf ("     run to the sound, key held: %s\n", order.toRawUTF8());
                CHECK (ons == 15 && lastOn == 84 && ! lastLetGo);
                engine.injectMidi (0, MidiMessage::noteOff (1, 60));
                auto released = renderAndWait();
                CHECK (has (released, [] (const MidiMessage& m) { return m.isNoteOff() && m.getNoteNumber() == 84; }));

                /* The song's key: a run that follows it plays in it. C pressed in
                   D major starts from B, the scale note below; with the keyboards
                   down a tone the key moves too (C major), and the C arrives as
                   Bb, so the run starts from A. Its own key again when the song
                   has none. */
                auto noteOn = [] (int n, int ch) { return [n, ch] (const MidiMessage& m) { return m.isNoteOn() && m.getNoteNumber() == n && m.getChannel() == ch; }; };
                const int keyed = engine.addSong ("Keyed");
                engine.setSongProgram (keyed, 0, 92);
                engine.setSongKey (keyed, 2, 0);
                const int keySet = engine.addSet ("Keys");
                engine.setSetSongs (keySet, { keyed });
                engine.selectSet (keySet);
                engine.selectSongInSet (0); settle();
                CHECK (engine.currentSongIndex() == keyed);
                int kr = -1, km = -1;
                CHECK (engine.keyInPlay (kr, km) && kr == 2 && km == 0);
                {
                    // A song tapped outside the set list is the song in play, key and all; the set keeps its place.
                    const int outside = engine.addSong ("Outside the set");
                    engine.setSongKey (outside, 9, 1);
                    engine.selectSong (outside);
                    CHECK (engine.currentSongIndex() == outside && engine.getCurrentSongInSet() == 0);
                    CHECK (engine.keyInPlay (kr, km) && kr == 9 && km == 1);
                    engine.selectSongInSet (0);
                    CHECK (engine.currentSongIndex() == keyed && engine.keyInPlay (kr, km) && kr == 2);
                    engine.removeSong (outside);
                    CHECK (engine.currentSongIndex() == keyed);
                }
                {
                    /* Scenes: a song with no track steps through its sections by
                       hand, each firing its actions at once. Added scenes hold the
                       programs loaded now; entering the song enters the first. */
                    const int staged = engine.addSong ("Staged");
                    CHECK (! engine.songHasScenes (staged));
                    engine.selectProgram (0, 92);
                    const int verse = engine.addScene (staged, "Verse");
                    engine.selectProgram (0, 93);
                    const int chorus = engine.addScene (staged, "Chorus");
                    engine.selectProgram (0, 94);
                    const int solo = engine.addScene (staged, {});
                    CHECK (verse == 0 && chorus == 1 && solo == 2 && engine.songHasScenes (staged));
                    const auto& sg = engine.getSongs()[(size_t) staged];
                    CHECK (sg.markers.size() == 3 && sg.markers[2].name == "Scene 3");
                    CHECK (sg.markers[0].actions.size() >= 1 && sg.markers[0].actions[0].kind == ActionDef::Kind::program && sg.markers[0].actions[0].input == 0 && sg.markers[0].actions[0].program == 92);
                    CHECK (sg.markers[1].actions[0].program == 93 && sg.markers[2].actions[0].program == 94);
                    engine.selectSong (staged); settle();
                    CHECK (engine.currentSongIndex() == staged && engine.getScene() == 0 && engine.getSceneCount() == 3);
                    CHECK (engine.getSetup().inputs[0].currentProgram == 92);          // the first scene, on entering the song
                    engine.advanceTrack(); settle();                                   // the Next button, the section pedal
                    CHECK (engine.getScene() == 1 && engine.getSetup().inputs[0].currentProgram == 93);
                    engine.queueTrackSection (2); settle();                            // the tablet's tap: at once, no track to wait for
                    CHECK (engine.getScene() == 2 && engine.getSetup().inputs[0].currentProgram == 94);
                    engine.advanceTrack(); settle();
                    CHECK (engine.getScene() == 2);                                    // the last one stays
                    engine.selectScene (0); settle();
                    CHECK (engine.getScene() == 0 && engine.getSetup().inputs[0].currentProgram == 92);
                    // Capture: the scene takes what is loaded now; other actions stay.
                    {
                        auto m = sg.markers;
                        ActionDef cue; cue.kind = ActionDef::Kind::midi; cue.portName = "Lights"; cue.data1 = 5;
                        m[1].actions.push_back (cue);
                        engine.setSongMarkers (staged, m);
                    }
                    engine.selectProgram (0, 95);
                    engine.captureScene (staged, 1);
                    {
                        // One program action per keyboard (the track input aside), then the cue, kept.
                        int keyboards = 0;
                        for (const auto& in : engine.getSetup().inputs) if (! in.isTrack() && in.currentProgram >= 0) ++keyboards;
                        const auto& acts = sg.markers[1].actions;
                        CHECK ((int) acts.size() == keyboards + 1 && acts[0].kind == ActionDef::Kind::program && acts[0].input == 0 && acts[0].program == 95
                               && acts.back().kind == ActionDef::Kind::midi);
                    }
                    engine.moveScene (staged, 2, 0);
                    CHECK (sg.markers[0].name == "Scene 3" && sg.markers[1].name == "Verse" && engine.getScene() == 1);   // the scene in play moved with it
                    {
                        // A scene removed before the one in play: it stays the one in play.
                        auto m = sg.markers;
                        m.erase (m.begin());
                        engine.setSongMarkers (staged, m);
                        CHECK (engine.getScene() == 0 && sg.markers[0].name == "Verse");
                        // Going straight to a scene of another song fires that scene, not the first one's too.
                        engine.selectSongInSet (0); settle();
                        engine.selectSong (staged, 1); settle();
                        CHECK (engine.getScene() == 1 && engine.getSetup().inputs[0].currentProgram == 95);
                    }
                    // A click makes it a timed song again: sections on the clock, no scenes.
                    engine.setSongClick (staged, true, 4);
                    CHECK (! engine.songHasScenes (staged));
                    engine.setSongClick (staged, false, 4);
                    CHECK (engine.songHasScenes (staged));
                    std::printf ("     scenes: three by hand, entered, stepped, tapped, captured, moved\n");
                    engine.selectSongInSet (0); settle();
                    CHECK (engine.currentSongIndex() == keyed && engine.getSceneCount() == 0);
                    engine.removeSong (staged);
                }
                {
                    /* Where the show is: written, read back, and returned to,
                       as a StageRig restarted after a crash does. */
                    engine.selectSongInSet (0); settle();
                    const auto before = engine.getLivePosition();
                    CHECK (Engine::LivePosition::fromString (before.toString()) == before);
                    CHECK (Engine::LivePosition::fromString ("nonsense").set == -1);
                    auto moved = before;
                    moved.programs[0] = before.programs[0] == 5 ? 6 : 5;
                    moved.tempoBpm = 133.0;
                    engine.selectProgram (0, 7); engine.setTempoBpm (90.0); settle();
                    engine.restoreLivePosition (moved); settle();
                    CHECK (engine.getSetup().inputs[0].currentProgram == moved.programs[0]);
                    CHECK (std::abs (engine.getSetup().tempoBpm - 133.0) < 0.01);
                    CHECK (engine.getCurrentSet() == before.set && engine.getCurrentSongInSet() == before.songInSet);
                    CHECK (engine.currentSongIndex() == before.song);
                    engine.restoreLivePosition (before); settle();
                    CHECK (engine.getLivePosition() == before);

                    /* The pre-show check. Headless, so no audio interface: that
                       is the first thing it says. A song in the set whose
                       track file has gone is named; put back, it is not. */
                    auto has = [&engine] (Engine::CheckItem::Level l, const String& part)
                    {
                        for (auto& i : engine.showCheck()) if (i.level == l && i.text.contains (part)) return true;
                        return false;
                    };
                    const auto first = engine.showCheck();
                    CHECK (! first.empty() && first.front().level == Engine::CheckItem::problem);
                    CHECK (has (Engine::CheckItem::problem, "No audio interface"));
                    CHECK (has (Engine::CheckItem::good, "plugin"));
                    const int song = engine.currentSongIndex();
                    StemDef gone; gone.file = "not-there-at-all.wav";
                    engine.addSongStem (song, gone);
                    CHECK (has (Engine::CheckItem::problem, "not-there-at-all.wav is missing"));
                    engine.removeSongStem (song, (int) engine.getSetup().songs[(size_t) song].stems.size() - 1);
                    CHECK (! has (Engine::CheckItem::problem, "not-there-at-all.wav"));
                    for (auto& i : engine.showCheck())
                        std::printf ("       check: %s %s\n", i.level == Engine::CheckItem::problem ? "[fix]" : i.level == Engine::CheckItem::warning ? "[look]" : "[ok] ", i.text.toRawUTF8());
                }
                {
                    // The Stage view's front panel: the parameters it picks by name, and the ones chosen by hand saved with the instrument.
                    ParamInfoList ps;
                    auto add = [&ps] (const char* n, bool boolean = false, int steps = 0, int cc = -1)
                    { ParamInfo p; p.index = (int) ps.size(); p.id = String (p.index); p.name = n; p.boolean = boolean; p.numSteps = steps; p.midiController = cc; ps.push_back (p); };
                    add ("Master Volume"); add ("Osc 1 Wave", false, 8); add ("Filter Cutoff"); add ("Filter Resonance"); add ("Amp Attack"); add ("Amp Release");
                    add ("Bypass"); add ("Mod Wheel", false, 0, 1); add ("Reverb Mix"); add ("Legato", true); add ("Osc Sync", false, 2);
                    CHECK (perf::pickPanelParams (ps, 4) == (std::vector<int> { 2, 3, 4, 5 }));
                    CHECK (perf::pickPanelParams (ps, 8) == (std::vector<int> { 2, 3, 4, 5, 8, 0, 1 }));     // then by order; no bypass, no CC stand-in, no switch
                    CHECK (perf::pickPanelParams ({}, 4).empty());
                    // An organ: its drawbars first, as many as fit; an effect section's attack is not the envelope's.
                    ParamInfoList organ;
                    auto addO = [&organ] (const char* n) { ParamInfo p; p.index = (int) organ.size(); p.id = String (p.index); p.name = n; organ.push_back (p); };
                    addO ("Compressor Attack"); addO ("Compressor Release"); addO ("Overscream Drive");
                    for (const char* d : { "16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'", "1'" }) addO ((String ("Upper Drawbar ") + d).toRawUTF8());
                    addO ("Leslie Speed"); addO ("Percussion Level"); addO ("Amp Attack");
                    CHECK (perf::pickPanelParams (organ, 8) == (std::vector<int> { 3, 4, 5, 6, 7, 8, 9, 10 }));
                    CHECK (perf::pickPanelParams (organ, 12) == (std::vector<int> { 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 }));   // then the Leslie, percussion, the real attack
                    engine.setSlotPanelParams (0, 92, 0, { "3", "2" });
                    CHECK (engine.getSetup().inputs[0].programs[92].slots[0].panelParams == (std::vector<String> { "3", "2" }));
                    engine.setSlotPanelParams (0, 92, 0, {});
                    CHECK (engine.getSetup().inputs[0].programs[92].slots[0].panelParams.empty());
                    std::printf ("     front panel: cutoff, resonance, attack, release picked first\n");
                }
                CHECK (engine.effectiveEffectDef (rd).scaleRoot == 2 && engine.effectiveEffectDef (rd).scaleMode == 0);
                renderAndWait();
                engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 90));
                auto inD = renderSeconds (0.3);
                engine.injectMidi (0, MidiMessage::noteOff (1, 60));
                renderSeconds (0.2);
                String firstNotes;
                for (auto& m : inD) if (m.isNoteOn()) firstNotes << m.getNoteNumber() << " ";
                std::printf ("     run in the song's key (D major) from C: %s\n", firstNotes.toRawUTF8());
                CHECK (has (inD, noteOn (59, 1)) && has (inD, noteOn (61, 1)) && ! has (inD, noteOn (60, 1)));
                engine.setTranspose (-2);
                CHECK (engine.keyInPlay (kr, km) && kr == 0 && engine.effectiveEffectDef (rd).scaleRoot == 0);
                engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 90));
                auto shifted = renderSeconds (0.3);
                engine.injectMidi (0, MidiMessage::noteOff (1, 60));
                renderSeconds (0.2);
                CHECK (has (shifted, noteOn (57, 1)) && has (shifted, noteOn (59, 1)));
                engine.setTranspose (0);
                EffectDef own = rd; own.followSongKey = false; own.scaleRoot = 7;
                CHECK (engine.effectiveEffectDef (own).scaleRoot == 7);
                engine.setSongKey (keyed, -1, 0);
                CHECK (! engine.keyInPlay (kr, km) && engine.effectiveEffectDef (rd).scaleRoot == rd.scaleRoot);
                engine.removeSet (keySet);
                engine.removeSong (keyed);

                engine.clearProgram (0, 92);
                engine.setLinkEnabled (linkWasOn);
            }

            // --- a chord chart moved to the song's key: the chords, never the words -------------------
            {
                CHECK (transposeChordName ("Am7", 2, false) == "Bm7");
                CHECK (transposeChordName ("F#m/C#", 3, true) == "Am/E");
                CHECK (transposeChordName ("Bb", 2, false) == "C" && transposeChordName ("E", 1, true) == "F");
                CHECK (transposeChordName ("N.C.", 5, false) == "N.C." && transposeChordName ("|", 5, false) == "|");
                CHECK (transposeChordName ("D/F#", 7, true) == "A/C#" && transposeChordName ("Bb7", 1, false) == "B7" && transposeChordName ("Ab", 1, false) == "A");   // sharp stays sharp, flat stays flat
                CHECK (noteNameFor (10, true) == "Bb" && noteNameFor (10, false) == "A#" && pitchClassOf ("Ebmaj7") == 3 && pitchClassOf ("x") == -1);
                const String cho = "{title: X}\n{key: G}\n[G]Hel[D/F#]lo [Em7]there\nno chords here [Bm]\n";
                CHECK (chordProKeyRoot (cho) == 7 && chordProKeyRoot ("{title: Y}\n[C]x") == -1 && chordProKeyRoot ("{key: Am}") == 9);
                const String inBb = transposeChordPro (cho, 10, true);
                std::printf ("     chart in Bb:\n%s", inBb.toRawUTF8());
                CHECK (inBb == "{title: X}\n{key: Bb}\n[Bb]Hel[F/A]lo [Gm7]there\nno chords here [Dm]\n");
                CHECK (transposeChordPro (cho, 7, true) == cho);                                  // already in G
                CHECK (transposeChordPro ("[C]no key named", 2, false) == "[C]no key named");    // nowhere to move from
                CHECK (transposeChordPro ("{key: Am}\n[Am]x [E7]y", 11, false) == "{key: Bm}\n[Bm]x [F#7]y");
                CHECK (MidiFx::keyPrefersFlats (10, 0) && MidiFx::keyPrefersFlats (7, 1) && ! MidiFx::keyPrefersFlats (7, 0) && ! MidiFx::keyPrefersFlats (9, 1));
                CHECK (MidiFx::keyName (2, 1) == "D natural minor" && MidiFx::keyIsMinor (1) && ! MidiFx::keyIsMinor (0));
            }
            // --- a MIDI file part to a MIDI device: an instrument in the track's program listens to the file's channel ---
            {
                const auto folder = engine.getChartsFolder();
                folder.createDirectory();
                MidiMessageSequence seq;
                seq.addEvent (MidiMessage::noteOn  (5, 67, (uint8) 100), 0);      // the file's channel 5
                seq.addEvent (MidiMessage::noteOff (5, 67), 960 * 2);
                seq.addEvent (MidiMessage::programChange (5, 40), 5);             // the file's idea of a sound: not the synth's business
                seq.updateMatchedPairs();
                MidiFile mf;
                mf.setTicksPerQuarterNote (960);
                mf.addTrack (seq);
                const auto f = folder.getChildFile ("strings.mid");
                f.deleteFile();
                { FileOutputStream os (f); CHECK (os.openedOk() && mf.writeTo (os)); }

                const int song = engine.addSong ("Strings to the synth");
                StemDef st; st.file = "strings.mid";
                engine.addSongStem (song, st);
                const int track = engine.getTrackInput();
                const int tp = engine.getSongs()[(size_t) song].programFor (track);
                CHECK (track >= 0 && tp >= 0);
                CHECK (engine.addExternalSlot (track, tp, port));                 // slot 1; slot 0 is the GM player
                engine.setSlotOutChannel (track, tp, 1, 3);                       // the synth listens on 3
                engine.assignPart (track, tp, 5, 1);                              // the file's channel 5 is its part
                engine.selectSong (song);
                for (int i = 0; i < 400 && ! engine.isTrackReady(); ++i) pump (10);
                CHECK (engine.isTrackReady());
                settle();
                CHECK (engine.getSetup().inputs[(size_t) track].currentProgram == tp);
                renderAndWait();
                engine.startTrack();
                auto got = renderAndWait();
                std::printf ("     MIDI file channel 5 to the synth on channel 3: %d messages\n", (int) got.size());
                CHECK (has (got, [] (const MidiMessage& m) { return m.isNoteOn() && m.getNoteNumber() == 67 && m.getChannel() == 3; }));
                CHECK (! has (got, [] (const MidiMessage& m) { return m.isProgramChange(); }));
                engine.stopTrack();
                renderAndWait(); pump (50);

                TemporaryFile sf (".stagerig.json");
                CHECK (engine.captureSetup().saveToFile (sf.getFile()).wasOk());
                Setup back4;
                CHECK (Setup::loadFromFile (sf.getFile(), back4).wasOk());
                const auto& tpd = back4.inputs[(size_t) track].programs[(size_t) tp];
                CHECK (tpd.slots.size() == 2 && tpd.slots[1].isExternal() && tpd.slots[1].hears (5) && ! tpd.slots[1].hears (1)
                       && tpd.slots[0].isGm() && ! tpd.slots[0].hears (5) && tpd.slots[0].hears (1)
                       && back4.songs[(size_t) song].programFor (track) == tp);
                engine.removeSong (song);
            }
            engine.setInputChannel (0, oldChannel);
            virt->stop();
        }
    }


    // ---- keyboards: knobs, buttons, a screen and lights ----------------------
    {
        struct FakeSurface : public ControlSurface
        {
            std::vector<SurfaceView> views;
            std::vector<Touch> queued;
            String getName() const override             { return "Fake"; }
            String getKeysPortName() const override     { return "Fake keys"; }
            String getLanguage() const override         { return "a test's"; }
            bool isConnected() const override           { return true; }
            bool hasScreen() const override             { return true; }
            void show (const SurfaceView& v) override   { views.push_back (v); }
            void takeTouches (std::vector<Touch>& out) override { out.insert (out.end(), queued.begin(), queued.end()); queued.clear(); }
        };
        ControlMapLibrary library (scratchDir.getChildFile ("Controller setups"));
        {
            // A layout file that arrives in the folder from elsewhere (dropped in, edited by hand) is seen without a restart.
            ControlMap dropped;
            dropped.name = "Dropped in"; dropped.pluginName = synth.name; dropped.pluginManufacturer = synth.manufacturerName;
            ControlPage pg; pg.name = "Only"; dropped.pages.push_back (pg);
            CHECK (! library.reloadIfChanged());                                      // nothing changed since it was loaded
            CHECK (dropped.saveToFile (library.getFolder().getChildFile ("Dropped in.stagerig-controls")).wasOk());
            CHECK (library.find (synth, "Dropped in") == nullptr);                   // not until it looks
            CHECK (library.reloadIfChanged() && library.find (synth, "Dropped in") != nullptr);
            CHECK (! library.reloadIfChanged());
            library.getFolder().getChildFile ("Dropped in.stagerig-controls").deleteFile();
            CHECK (library.reloadIfChanged() && library.find (synth, "Dropped in") == nullptr);
        }
        SurfaceHub hub (engine, library, settings, /*lookForKeyboards*/ false);
        auto owned = std::make_unique<FakeSurface>();
        auto* fake = owned.get();
        hub.addSurfaceForTesting (std::move (owned));
        auto button = [] (const String& id, bool down) { ControlSurface::Touch t; t.type = ControlSurface::Touch::button; t.id = id; t.down = down; return t; };

        const int keepProgram = engine.getSetup().inputs[0].currentProgram;
        const int ch = jmax (1, engine.getSetup().inputs[0].channel);
        String err;
        for (int p : { 100, 101, 102, 103, 110 }) engine.clearProgram (0, p);
        engine.selectProgram (0, 103); settle();
        CHECK (engine.addSlot (0, 103, synth, err)); settle();
        CHECK (engine.addSlot (0, 103, synth, err)); settle();
        engine.setSlotKeyRange (0, 103, 0, 0, 59);
        engine.setSlotKeyRange (0, 103, 1, 60, 127);
        engine.selectProgram (0, 100); settle();
        CHECK (engine.addSlot (0, 100, synth, err)); settle();
        engine.setProgramName (0, 100, "Lead");
        for (int i = 0; i < 100 && ! engine.isPluginAlive (0, 100, 0); ++i) pump (20);

        hub.tick();
        CHECK (! fake->views.empty());
        if (! fake->views.empty())
        {
            const auto v = fake->views.back();
            std::printf ("     screen: %s | %s | %s | page %s (%d of %d) | knob 1 %s = %s\n", v.inputName.toRawUTF8(), v.programName.toRawUTF8(),
                         v.instrument.toRawUTF8(), v.pageName.toRawUTF8(), v.page + 1, v.pageCount, v.knobNames[0].toRawUTF8(), v.knobValues[0].toRawUTF8());
            CHECK (v.program == 100 && v.programName == "Lead" && v.instrument == synth.name);
            CHECK (v.pageCount >= 1 && v.pageName == "Front panel" && v.knobNames[0].isNotEmpty() && v.knobValues[0].isNotEmpty());
            // One instrument on every key: the whole keyboard in its colour, dimly.
            CHECK (v.keyColours[0] != 0 && v.keyColours[0] == v.keyColours[127] && v.keyColours[60] == v.keyColours[0]);
        }
        const auto before = fake->views.size();
        hub.tick();
        CHECK (fake->views.size() == before);      // nothing changed: nothing sent

        // A maker's NKS pages, for an instrument with no setup of ours: the keyboard's knobs take them.
        if (auto* live = engine.getPlugin (0, 100, 0); live != nullptr && live->getParameters().size() >= 3)
        {
            using Bytes = std::vector<uint8>;
            auto str = [] (const String& t) { Bytes b; const auto u = t.toStdString(); b.push_back (0xd9); b.push_back ((uint8) u.size()); b.insert (b.end(), u.begin(), u.end()); return b; };
            auto cat = [] (uint8 head, std::vector<Bytes> parts) { Bytes b { head }; for (auto& p : parts) b.insert (b.end(), p.begin(), p.end()); return b; };
            const auto& ps = live->getParameters();
            const auto knobs = cat (0x92, { cat (0x83, { str ("id"), Bytes { 0xce, 0x70, 0, 0, 1 }, str ("name"), str (ps[2].name), str ("section"), str ("Tone") }),
                                            cat (0x82, { str ("id"), Bytes { 0xce, 0x70, 0, 0, 2 }, str ("name"), str (ps[1].name) }) });
            auto chunk = [] (const char* id, const Bytes& data)
            {
                Bytes c { (uint8) id[0], (uint8) id[1], (uint8) id[2], (uint8) id[3] };
                const auto n = data.size() + 4;
                for (int i = 0; i < 4; ++i) c.push_back ((uint8) (n >> (8 * i)));
                c.insert (c.end(), { 1, 0, 0, 0 });
                c.insert (c.end(), data.begin(), data.end());
                if (n & 1) c.push_back (0);
                return c;
            };
            Bytes body { 'N', 'I', 'K', 'S' };
            for (const auto& c : { chunk ("NISI", cat (0x83, { str ("name"), str ("Init"), str ("vendor"), str (synth.manufacturerName), str ("bankchain"), cat (0x91, { str (synth.name) }) })),
                                   chunk ("NICA", cat (0x81, { str ("ni8"), cat (0x91, { knobs }) })) })
                body.insert (body.end(), c.begin(), c.end());
            Bytes file { 'R', 'I', 'F', 'F' };
            for (int i = 0; i < 4; ++i) file.push_back ((uint8) (body.size() >> (8 * i)));
            file.insert (file.end(), body.begin(), body.end());
            const auto nksDir = scratchDir.getChildFile ("NKS presets");
            nksDir.createDirectory();
            CHECK (nksDir.getChildFile ("Init.nksf").replaceWithData (file.data(), file.size()));

            ControlMapLibrary withNks (scratchDir.getChildFile ("Controller setups (NKS)"));
            withNks.startNksSearch ({ nksDir });
            CHECK (withNks.waitForNks (10000));
            SurfaceHub nksHub (engine, withNks, settings, /*lookForKeyboards*/ false);
            const auto layout = nksHub.layoutFor (0, 100, 0);
            CHECK (layout.fromNks && ! layout.map.has_value() && layout.pages.size() == 1 && layout.pages[0].name == "Tone");
            if (! layout.pages.empty())
                CHECK (layout.pages[0].knobs[0].paramIndex == ps[2].index && layout.pages[0].knobs[1].paramIndex == ps[1].index
                       && layout.pages[0].knobs[2].paramIndex < 0);
            CHECK (! hub.layoutFor (0, 100, 0).fromNks);      // a library without them: the front panel, as before
            nksDir.deleteRecursively();
        }

        // The programs around this one, for a keyboard that names them; choosing one, and an instrument, from it.
        {
            const auto v = fake->views.back();
            CHECK (v.bankFirst == 96 && v.bankNames[4] == "Lead" && v.bankNames[7] == "Program 103" && v.bankNames[5].isEmpty());
            CHECK (v.programAfter && v.pluginNames.size() == 1 && v.focusPlugin == 0 && v.knobLevels[0] >= 0.0f);
            ControlSurface::Touch t;
            t.type = ControlSurface::Touch::program; t.index = 7;
            fake->queued.push_back (t);
            hub.tick(); settle();
            CHECK (engine.getSetup().inputs[0].currentProgram == 103);
            t.type = ControlSurface::Touch::instrument; t.index = 1;
            fake->queued.push_back (t);
            hub.tick();
            CHECK (hub.getFocusSlot (0) == 1 && fake->views.back().focusPlugin == 1 && fake->views.back().pluginNames.size() == 2);
            t.type = ControlSurface::Touch::action; t.index = SurfaceControls::programDown;
            fake->queued.push_back (t);
            hub.tick(); settle();
            CHECK (engine.getSetup().inputs[0].currentProgram == 100);     // past the empty 101 and 102

            // Eight at a time: 108 is empty, so the first of 104..111 with something in it; and back among 96..103.
            engine.selectProgram (0, 110); settle();
            CHECK (engine.addSlot (0, 110, synth, err)); settle();
            engine.selectProgram (0, 100); settle();
            hub.tick();
            CHECK (fake->views.back().bankAfter);
            t.type = ControlSurface::Touch::action; t.index = SurfaceControls::programBankUp;
            fake->queued.push_back (t);
            hub.tick(); settle();
            CHECK (engine.getSetup().inputs[0].currentProgram == 110);
            hub.tick();
            CHECK (fake->views.back().bankFirst == 104 && fake->views.back().bankBefore && ! fake->views.back().bankAfter);
            hub.doAction (0, SurfaceControls::programBankUp); settle();
            CHECK (engine.getSetup().inputs[0].currentProgram == 110);     // nothing further on
            hub.doAction (0, SurfaceControls::programBankDown); settle();
            {
                const auto& in = engine.getSetup().inputs[0];
                CHECK (in.currentProgram >= 96 && in.currentProgram < 104 && ! in.programs[(size_t) in.currentProgram].isEmpty());
            }
            engine.selectProgram (0, 100); settle();
            for (int i = 0; i < 100 && ! engine.isPluginAlive (0, 100, 0); ++i) pump (20);
            hub.tick();
        }

        // A knob of the keyboard moves the parameter it shows.
        const auto layout = hub.layoutFor (0, 100, 0);
        CHECK (! layout.pages.empty() && ! layout.map.has_value());
        auto* plug = engine.getPlugin (0, 100, 0);
        const int p0 = layout.pages.empty() ? -1 : layout.pages[0].knobs[0].paramIndex;
        const int p1 = layout.pages.empty() ? -1 : layout.pages[0].knobs[1].paramIndex;
        CHECK (p0 >= 0 && p1 >= 0 && plug != nullptr);
        if (p0 >= 0 && p1 >= 0 && plug != nullptr)
        {
            CHECK (plug->setParameterValue (p0, 0.5f)); render (engine, 2); pump (50);
            ControlSurface::Touch t; t.type = ControlSurface::Touch::knob; t.index = 0; t.amount = 0.1f;
            fake->queued.push_back (t); fake->queued.push_back (t);
            hub.tick(); render (engine, 2); pump (50);
            float got = -1.0f; plug->fetchParameterValue (p0, got);
            std::printf ("     knob 1 turned twice by a tenth from 0.5: %.3f\n", got);
            CHECK (std::abs (got - 0.7f) < 0.02f);

            // A knob taught on the input, sending where it is: CC 74 at the top puts knob 2 at the top.
            SurfaceControls c; c.knobCC[1] = 74;
            engine.setInputControls (0, c);
            engine.injectMidi (0, MidiMessage::controllerEvent (ch, 74, 127)); pump (60);
            hub.tick(); render (engine, 2); pump (50);
            got = -1.0f; plug->fetchParameterValue (p1, got);
            std::printf ("     knob 2 taught as CC 74, sent 127: %.3f\n", got);
            CHECK (got > 0.98f);
            // ... and endless: steps around 64.
            c.knobsRelative = true;
            engine.setInputControls (0, c);
            for (int i = 0; i < 10; ++i) engine.injectMidi (0, MidiMessage::controllerEvent (ch, 74, 63));    // ten steps to the left
            pump (60); hub.tick(); render (engine, 2); pump (50);
            got = -1.0f; plug->fetchParameterValue (p1, got);
            std::printf ("     ten steps to the left: %.3f\n", got);
            CHECK (std::abs (got - 0.9f) < 0.02f);
        }

        // Buttons are taught by pressing them: a controller, and a keyboard's own button.
        String taughtAs;
        hub.learn (0, 100 + SurfaceControls::programUp, [&taughtAs] (const String& w) { taughtAs = w; });
        CHECK (hub.isLearning());
        engine.injectMidi (0, MidiMessage::controllerEvent (ch, 105, 127)); pump (60);
        CHECK (taughtAs == "CC 105" && ! hub.isLearning());
        CHECK (engine.getSetup().inputs[0].controls.buttons[SurfaceControls::programUp] == "cc:105");
        CHECK (engine.getSetup().inputs[0].controls.knobCC[1] == 74);       // the knob is still taught
        hub.learn (0, 100 + SurfaceControls::programDown, [&taughtAs] (const String& w) { taughtAs = w; });
        fake->queued.push_back (button ("fk:7", true));
        hub.tick();
        CHECK (engine.getSetup().inputs[0].controls.buttons[SurfaceControls::programDown] == "fk:7" && taughtAs == "fk:7");

        // Next program passes over the empty ones; the release does nothing.
        engine.injectMidi (0, MidiMessage::controllerEvent (ch, 105, 127)); pump (60);
        engine.injectMidi (0, MidiMessage::controllerEvent (ch, 105, 0)); pump (60);
        settle();
        CHECK (engine.getSetup().inputs[0].currentProgram == 103);
        for (int i = 0; i < 100 && ! (engine.isPluginAlive (0, 103, 0) && engine.isPluginAlive (0, 103, 1)); ++i) pump (20);
        hub.tick();
        CHECK (hub.getFocusSlot (0) == 0);
        {
            const auto v = fake->views.back();
            // A split: each half in its instrument's colour.
            CHECK (v.program == 103 && v.keyColours[30] != 0 && v.keyColours[90] != 0 && v.keyColours[30] != v.keyColours[90]);
        }
        hub.doAction (0, SurfaceControls::instrumentNext);
        CHECK (hub.getFocusSlot (0) == 1);
        hub.doAction (0, SurfaceControls::instrumentNext);
        CHECK (hub.getFocusSlot (0) == 0);
        const int pages = (int) hub.layoutFor (0, 103, 0).pages.size();
        hub.doAction (0, SurfaceControls::pageUp);
        CHECK (hub.getFocusPage (0) == (pages > 1 ? 1 : 0));
        hub.doAction (0, SurfaceControls::pageDown); hub.doAction (0, SurfaceControls::pageDown);
        CHECK (hub.getFocusPage (0) == 0);
        // The keyboard's own button, taught as Previous program: back to 100.
        fake->queued.push_back (button ("fk:7", true));
        fake->queued.push_back (button ("fk:7", false));
        hub.tick(); settle();
        CHECK (engine.getSetup().inputs[0].currentProgram == 100);

        // A controller setup: its page, its names, its keys.
        if (plug != nullptr && p0 >= 0)
        {
            ControlMap m;
            m.pluginName = synth.name; m.pluginManufacturer = synth.manufacturerName; m.name = "Bright lead";
            ControlPage page; page.name = "Mine";
            page.knobs.push_back ({ plug->getParameters()[(size_t) p0].id, plug->getParameters()[(size_t) p0].name, "Brightness" });
            page.knobs.push_back ({ {}, plug->getParameters()[(size_t) p1].name, {} });     // found by its name
            m.pages.push_back (page);
            m.keys.push_back ({ 36, 40, 0xff3c3c, "Keyswitches" });
            CHECK (library.save (m).wasOk());
            hub.libraryChanged();
            hub.tick();
            auto v = fake->views.back();
            std::printf ("     with a setup: %s | page %s | knob 1 %s, knob 2 %s\n", v.instrument.toRawUTF8(), v.pageName.toRawUTF8(), v.knobNames[0].toRawUTF8(), v.knobNames[1].toRawUTF8());
            CHECK (v.instrument == synth.name + ": Bright lead" && v.pageName == "Mine" && v.pageCount == 1);
            CHECK (v.knobNames[0] == "Brightness" && v.knobNames[1] == plug->getParameters()[(size_t) p1].name && v.knobNames[2].isEmpty());
            CHECK (hub.layoutFor (0, 100, 0).pages[0].knobs[1].paramIndex == p1);
            CHECK (v.keyColours[36] == 0xff3c3c && v.keyColours[40] == 0xff3c3c && v.keyColours[41] != 0xff3c3c && v.keyColours[35] != 0xff3c3c);
            // The transpose moves the key that plays a keyswitch: the lights move with it.
            engine.setTranspose (2);
            hub.tick();
            v = fake->views.back();
            CHECK (v.keyColours[34] == 0xff3c3c && v.keyColours[38] == 0xff3c3c && v.keyColours[39] != 0xff3c3c);
            engine.setTranspose (0);
            // "-": no setup for this instrument, though there is one for the plugin.
            engine.setSlotControlMap (0, 100, 0, "-");
            hub.tick();
            CHECK (fake->views.back().pageName == "Front panel");
            engine.setSlotControlMap (0, 100, 0, {});
            hub.tick();
            CHECK (fake->views.back().pageName == "Mine");
        }

        // The Launchkey's messages, from Novation's guide.
        {
            const auto open = LaunchkeyMk4::openingMessages (false);
            CHECK (open.size() >= 4 && open[0].getRawDataSize() == 3 && open[0].getRawData()[0] == 0x9f && open[0].getRawData()[1] == 0x0c && open[0].getRawData()[2] == 0x7f);
            CHECK (LaunchkeyMk4::closingMessages()[0].getRawData()[2] == 0x00);
            SurfaceView v; v.inputName = "Upper"; v.program = 7; v.programName = "Strings"; v.instrument = "Synth";
            v.pageName = "Sound"; v.pageCount = 1; v.knobNames[0] = "Cutoff"; v.knobValues[0] = "50%";
            const auto first = LaunchkeyMk4::screenMessages (v, nullptr, false);
            bool allSysEx = ! first.empty(), titled = false;
            for (const auto& m : first)
            {
                const auto* d = m.getRawData();
                allSysEx = allSysEx && m.isSysEx() && d[1] == 0x00 && d[2] == 0x20 && d[3] == 0x29 && d[4] == 0x02 && d[5] == 0x14 && d[6] == 0x06;
                if (d[7] == 0x20 && d[8] == 0x00) titled = String::fromUTF8 ((const char*) d + 9, m.getRawDataSize() - 10) == "Upper  007";
            }
            CHECK (allSysEx && titled);
            auto moved = v; moved.knobValues[0] = "51%";
            const auto next = LaunchkeyMk4::screenMessages (moved, &v, false);
            CHECK (next.size() == 1 && next[0].getRawData()[7] == 0x15 && next[0].getRawData()[8] == 0x01);    // knob 1's value, and nothing else
            CHECK (LaunchkeyMk4::screenMessages (v, nullptr, true)[0].getRawData()[5] == 0x13);               // a Mini
            ControlSurface::Touch t;
            CHECK (LaunchkeyMk4::parse (MidiMessage::controllerEvent (16, 0x16, 66), t) && t.type == ControlSurface::Touch::knob && t.index == 1 && std::abs (t.amount - 0.02f) < 1e-6f);
            CHECK (LaunchkeyMk4::parse (MidiMessage::controllerEvent (15, 0x55, 127), t) && t.type == ControlSurface::Touch::knobTouch && t.index == 0 && t.down);
            CHECK (LaunchkeyMk4::parse (MidiMessage::controllerEvent (16, 0x66, 127), t) && t.type == ControlSurface::Touch::button && t.id == "lk:102" && t.down);
            CHECK (! LaunchkeyMk4::parse (MidiMessage::controllerEvent (16, 0x06, 50), t));        // a fader
            CHECK (LaunchkeyMk4::parse (MidiMessage::noteOn (1, 40, (uint8) 100), t) && t.id == "lk:n40" && t.down);
        }

        // Native Instruments' DAW protocol (S MK2/MK3, A, M), as ReaKontrol speaks it.
        {
            String keys;
            CHECK (KompleteKontrolDaw::modelFor ("Komplete Kontrol DAW - 1", keys) == "Komplete Kontrol (DAW)" && keys == "KOMPLETE KONTROL");
            CHECK (KompleteKontrolDaw::modelFor ("Native Instruments - KONTROL S61 MK3 - DAW", keys) == "Komplete Kontrol S61 MK3" && keys == "S61 MK3");
            CHECK (KompleteKontrolDaw::modelFor ("MIDIIN2 (KONTROL S49 MK3)", keys) == "Komplete Kontrol S49 MK3");
            CHECK (KompleteKontrolDaw::modelFor ("MIDIOUT2 (KONTROL S49 MK3)", keys) == "Komplete Kontrol S49 MK3");
            CHECK (KompleteKontrolDaw::modelFor ("Komplete Kontrol A DAW", keys) == "Komplete Kontrol A" && KompleteKontrolDaw::modelFor ("Komplete Kontrol M DAW", keys) == "Komplete Kontrol M");
            CHECK (KompleteKontrolDaw::modelFor ("KOMPLETE KONTROL - 1", keys).isEmpty());      // an MK2's keys, beside its DAW port
            CHECK (KompleteKontrolDaw::modelFor ("KONTROL S88 MK3", keys).isEmpty());          // its keys, not its DAW port
            CHECK (KompleteKontrolDaw::modelFor ("KOMPLETE KONTROL S88", keys).isEmpty());     // an MK1
            const auto h = KompleteKontrolDaw::hello();
            CHECK (h.getRawDataSize() == 3 && h.getRawData()[0] == 0xbf && h.getRawData()[1] == 0x01 && h.getRawData()[2] == 4);
            CHECK (KompleteKontrolDaw::goodbye().getRawData()[1] == 0x02);
            // Answered, the keyboard is told the tracks run across; an S MK3 (version 4) is asked for high-resolution knob turns too.
            {
                const uint8 orientation[] = { 0xf0, 0x00, 0x21, 0x09, 0x00, 0x00, 0x44, 0x43, 0x01, 0x00, 0x03, 0x01, 0x00,
                                              't', 'r', 'a', 'c', 'k', '_', 'o', 'r', 'i', 'e', 'n', 't', 'a', 't', 'i', 'o', 'n', 0xf7 };
                const auto mk2 = KompleteKontrolDaw::connectedMessages (2), mk3 = KompleteKontrolDaw::connectedMessages (4);
                CHECK (mk2.size() == 1 && mk2[0].getRawDataSize() == (int) sizeof (orientation) && std::memcmp (mk2[0].getRawData(), orientation, sizeof (orientation)) == 0);
                CHECK (mk3.size() == 2 && std::memcmp (mk3[0].getRawData(), orientation, sizeof (orientation)) == 0
                       && mk3[1].getRawDataSize() == 3 && mk3[1].getRawData()[0] == 0xbf && mk3[1].getRawData()[1] == 0x06 && mk3[1].getRawData()[2] == 1);
            }

            SurfaceView v; v.program = 9; v.programName = "Strings"; v.bankFirst = 8;
            v.bankNames[0] = "Piano"; v.bankNames[1] = "Strings"; v.programBefore = true; v.bankAfter = true;
            v.pluginNames.add ("Synth"); v.focusPlugin = 0; v.pageName = "Sound"; v.pageCount = 2;
            v.knobNames[0] = "Cutoff"; v.knobValues[0] = "50%"; v.knobLevels[0] = 0.5f;
            auto find = [] (const std::vector<MidiMessage>& ms, int command, int index) -> const MidiMessage*
            {
                for (const auto& m : ms)
                    if (m.isSysEx() && m.getSysExDataSize() >= 12 && m.getSysExData()[9] == command && m.getSysExData()[11] == index) return &m;
                return nullptr;
            };
            auto text = [] (const MidiMessage* m) { return m == nullptr ? String ("<none>") : String::fromUTF8 ((const char*) m->getSysExData() + 12, m->getSysExDataSize() - 12); };
            const auto first = KompleteKontrolDaw::screenMessages (v, nullptr, { 2, true });
            const uint8 header[] = { 0xf0, 0x00, 0x21, 0x09, 0x00, 0x00, 0x44, 0x43, 0x01, 0x00 };
            CHECK (find (first, 0x48, 1) != nullptr && std::memcmp (find (first, 0x48, 1)->getRawData(), header, sizeof (header)) == 0);
            CHECK (text (find (first, 0x48, 0)) == "Piano" && text (find (first, 0x48, 1)) == "Strings");       // the programs, by name
            CHECK (find (first, 0x40, 2) != nullptr && find (first, 0x40, 2)->getSysExData()[10] == 0);          // an empty one: no track
            const auto* sel = find (first, 0x42, 1);
            CHECK (sel != nullptr && sel->getSysExData()[10] == 1);
            CHECK (text (find (first, 0x72, 0)) == "Cutoff" && text (find (first, 0x73, 0)) == "50%" && text (find (first, 0x75, 0)) == "Sound");
            CHECK (find (first, 0x72, 1) != nullptr && find (first, 0x72, 1)->getSysExDataSize() == 12 && find (first, 0x75, 1) == nullptr);   // an empty knob: no name, no section
            CHECK (find (first, 0x71, 0) == nullptr && find (first, 0x76, 0) == nullptr);                        // plugin names and the preset: an S MK3's only
            bool presetLit = false;
            for (const auto& m : first) presetLit = presetLit || (m.isController() && m.getControllerNumber() == 0x36);
            CHECK (! presetLit);
            const auto mk3 = KompleteKontrolDaw::screenMessages (v, nullptr, { 4, true });
            CHECK (text (find (mk3, 0x76, 0)) == "Strings" && text (find (mk3, 0x71, 0)) == "Synth" && find (mk3, 0x70, 0) != nullptr);
            for (const auto& m : mk3) presetLit = presetLit || (m.isController() && m.getControllerNumber() == 0x36 && m.getControllerValue() == 1);
            CHECK (presetLit);
            bool navTracks = false;
            for (const auto& m : first)
                if (m.isController() && m.getChannel() == 16 && m.getControllerNumber() == 0x30) navTracks = m.getControllerValue() == 1;   // previous only
            CHECK (navTracks);
            // The bank arrows: lit for the next eight programs in the mixer view, for the next knob page in the plugin view.
            auto banksLit = [] (const std::vector<MidiMessage>& ms)
            {
                int lit = -1;
                for (const auto& m : ms)
                    if (m.isController() && m.getChannel() == 16 && m.getControllerNumber() == 0x31) lit = m.getControllerValue();
                return lit;
            };
            CHECK (banksLit (first) == 2);
            v.bankAfter = false;
            CHECK (banksLit (KompleteKontrolDaw::screenMessages (v, nullptr, { 2, true })) == 0);
            CHECK (banksLit (KompleteKontrolDaw::screenMessages (v, nullptr, { 2, false })) == 2);     // page 1 of 2
            CHECK (find (KompleteKontrolDaw::screenMessages (v, nullptr, { 4, true }), 0x71, 0) != nullptr);
            auto turned = v; turned.knobValues[0] = "51%"; turned.knobLevels[0] = 0.51f;
            const auto next = KompleteKontrolDaw::screenMessages (turned, &v, { 2, true });
            CHECK (next.size() == 4 && find (next, 0x48, 0) == nullptr && text (find (next, 0x73, 0)) == "51%");   // the value, nothing more

            ControlSurface::Touch t;
            KompleteKontrolDaw::Heard heard;
            CHECK (! KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x01, 4), t, heard) && heard.version == 4);
            CHECK (KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x30, 127), t, heard) && t.type == ControlSurface::Touch::action && t.index == SurfaceControls::programDown);
            CHECK (KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x30, 1), t, heard) && t.index == SurfaceControls::programUp);
            CHECK (KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x31, 1), t, heard) && t.index == SurfaceControls::programBankUp);
            CHECK (KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x31, 127), t, heard) && t.index == SurfaceControls::programBankDown);
            CHECK (! KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x05, 1), t, heard) && ! heard.banksForPrograms);   // the plugin view
            CHECK (KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x31, 1), t, heard) && t.index == SurfaceControls::pageUp);
            CHECK (! KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x05, 0), t, heard) && heard.banksForPrograms);     // the mixer view
            CHECK (KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x42, 3), t, heard) && t.type == ControlSurface::Touch::program && t.index == 3);
            CHECK (KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x52, 125), t, heard) && t.type == ControlSurface::Touch::knob && t.index == 2
                   && std::abs (t.amount + 3.0f / 1016.0f) < 1e-6f);
            CHECK (KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x10, 1), t, heard) && t.type == ControlSurface::Touch::button && t.id == "ni:16" && t.down);
            CHECK (KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x14, 0), t, heard) && t.id == "ni:20" && t.down);   // a press whatever the value: no release is sent
            // The mute and solo buttons say which track in the value; the 4D encoder's up, down and turns say a direction.
            CHECK (KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x43, 0), t, heard) && t.type == ControlSurface::Touch::button && t.id == "ni:mute0" && t.down);
            CHECK (KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x44, 3), t, heard) && t.id == "ni:solo3");
            CHECK (! KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x44, 8), t, heard));
            CHECK (KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x32, 1), t, heard) && t.id == "ni:50+");
            CHECK (KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x32, 127), t, heard) && t.id == "ni:50-");
            CHECK (KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x34, 1), t, heard) && t.id == "ni:52+");
            CHECK (KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x64, 127), t, heard) && t.id == "ni:100-");
            CHECK (SurfaceHub::describeControl ("ni:mute0") == "Komplete Kontrol mute button 1 (DAW)" && SurfaceHub::describeControl ("ni:solo3") == "Komplete Kontrol solo button 4 (DAW)"
                   && SurfaceHub::describeControl ("ni:50-") == "Komplete Kontrol wheel 50 back (DAW)" && SurfaceHub::describeControl ("ni:16") == "Komplete Kontrol button 16 (DAW)");
            CHECK (! KompleteKontrolDaw::parse (MidiMessage::controllerEvent (16, 0x58, 1), t, heard));        // shift and a knob: a pan, not the knobs'
            CHECK (! KompleteKontrolDaw::parse (MidiMessage::controllerEvent (1, 0x30, 1), t, heard));         // another channel: not the protocol
            // A knob's turn at high resolution: the value says what it was over (0 a volume, 1 a pan, 2 a plugin's parameter).
            const uint8 hi[] = { 0x00, 0x21, 0x09, 0x00, 0x00, 0x44, 0x43, 0x01, 0x00, 0x7f, 0x02, 0x05, 0x7f, 0x7f };   // knob 6, one step back
            CHECK (KompleteKontrolDaw::parse (MidiMessage::createSysExMessage (hi, (int) sizeof (hi)), t, heard) && t.type == ControlSurface::Touch::knob
                   && t.index == 5 && t.amount < 0.0f && t.amount > -0.001f);
            const uint8 hiVolume[] = { 0x00, 0x21, 0x09, 0x00, 0x00, 0x44, 0x43, 0x01, 0x00, 0x7f, 0x00, 0x03, 0x01, 0x00 };   // knob 4 in the mixer view, one step on
            CHECK (KompleteKontrolDaw::parse (MidiMessage::createSysExMessage (hiVolume, (int) sizeof (hiVolume)), t, heard) && t.type == ControlSurface::Touch::knob
                   && t.index == 3 && std::abs (t.amount - 1.0f / 8191.0f) < 1e-7f);
            const uint8 hiPan[] = { 0x00, 0x21, 0x09, 0x00, 0x00, 0x44, 0x43, 0x01, 0x00, 0x7f, 0x01, 0x03, 0x01, 0x00 };      // its pan: not the knobs'
            CHECK (! KompleteKontrolDaw::parse (MidiMessage::createSysExMessage (hiPan, (int) sizeof (hiPan)), t, heard));
            const uint8 plugin[] = { 0x00, 0x21, 0x09, 0x00, 0x00, 0x44, 0x43, 0x01, 0x00, 0x70, 0x00, 0x02 };                 // the third plugin chosen
            CHECK (KompleteKontrolDaw::parse (MidiMessage::createSysExMessage (plugin, (int) sizeof (plugin)), t, heard) && t.type == ControlSurface::Touch::instrument && t.index == 2);
        }

        // The Komplete Kontrol MK1's light guide and controls, as the keyboard showed them.
        {
            const auto* s88 = KompleteKontrolMk1::modelFor (0x1410);
            CHECK (s88 != nullptr && s88->keys == 88 && s88->lowestNote == 21 && KompleteKontrolMk1::modelFor (0x1630) == nullptr);
            std::array<uint32, 128> colours {};
            colours[21] = 0xff0000; colours[108] = 0x0000ff; colours[60] = 0x808080;
            const auto r = KompleteKontrolMk1::lightReport (colours, *s88);
            CHECK (r.size() == 265 && r[0] == 0x82 && r[1] == 127 && r[2] == 0 && r[3] == 0);
            CHECK (r[1 + 87 * 3 + 2] == 127 && r[1 + 39 * 3] == 64);
            uint8 a[37] = {}, b[37] = {};
            a[6] = (uint8) (990 & 0xff); a[7] = (uint8) (990 >> 8);
            b[6] = 5; b[7] = 0;                          // knob 1 round past 999: fifteen steps on
            b[0] = 0x08;                                 // button 3 pressed
            b[3] = 0x01;                                 // knob 1 touched (bit 24)
            std::vector<ControlSurface::Touch> found;
            KompleteKontrolMk1::compareReports (a, b, 37, found);
            bool press = false, touch = false, turn = false;
            for (const auto& t : found)
            {
                press = press || (t.type == ControlSurface::Touch::button && t.id == "kk:b3" && t.down);
                touch = touch || (t.type == ControlSurface::Touch::knobTouch && t.index == 0 && t.down);
                turn  = turn  || (t.type == ControlSurface::Touch::knob && t.index == 0 && std::abs (t.amount - 0.015f) < 1e-6f);
            }
            CHECK (press && touch && turn && found.size() == 3);

            // Its nine screens, as the S88 showed them: a line across all nine a report.
            CHECK (KompleteKontrolMk1::fitWords ("Filter Cutoff", 8) == "FltrCtff" && KompleteKontrolMk1::fitWords ("Reverb Mix", 8) == "Revrb Mx"
                   && KompleteKontrolMk1::fitWords ("  Bass ", 8) == "Bass" && KompleteKontrolMk1::fitWords ("1234.5 Hz", 8) == "1234.5Hz");
            SurfaceView sv;
            sv.program = 7; sv.programName = "Grand Piano Ballad";
            sv.knobNames[0] = "Filter Cutoff"; sv.knobValues[0] = String (CharPointer_UTF8 ("\xc3\xa9t\xc3\xa9")); sv.knobLevels[0] = 1.0f;
            sv.knobLevels[1] = 0.0f;
            const auto lines = KompleteKontrolMk1::screenReports (sv);
            auto glyphAt = [&] (int line, int screen, int letter)
            {
                const auto* p = lines[(size_t) line].data() + 9 + screen * 16 + letter * 2;
                return p[0] | (p[1] << 8);
            };
            const uint8 head[] = { 0xe0, 0x00, 0x00, 0x02, 0x00, 0x48, 0x00, 0x01, 0x00 };
            CHECK (lines[0].size() == 249 && std::memcmp (lines[2].data(), head, sizeof (head)) == 0 && lines[0][3] == 0 && lines[1][3] == 1);
            CHECK (glyphAt (1, 0, 0) == 0x10fb && glyphAt (2, 0, 0) == 0x523f);       // "Grand Pn" over "Ballad"
            CHECK (glyphAt (1, 1, 0) == 0x18c3 && glyphAt (2, 1, 0) == 0x2860);       // "FltrCtff" over "ete": accents dropped
            CHECK (lines[0][9 + 16] == 0x07 && lines[0][9 + 16 + 15] == 0x03);        // a full knob: every bar
            CHECK (lines[0][9 + 32] == 0x04 && lines[0][9 + 34] == 0x00);             // at the bottom: only the frame
            CHECK (lines[0][9] == 0x00 && glyphAt (1, 3, 0) == 0);                    // one page; no knob 3
            sv.programName = "Program 007"; sv.pageCount = 3; sv.page = 2; sv.pageName = "Effects";
            const auto paged = KompleteKontrolMk1::screenReports (sv);
            CHECK (paged[2][9] == 0xff && paged[2][10] == 0x24 && paged[0][9] == 0x07);   // "007" below "Program"; the last page's bars
            sv.programName = "Organ";
            CHECK (KompleteKontrolMk1::screenReports (sv)[2][9] == 0xf3);        // a short name leaves room for "Effects"
        }

        checkMidiCi();

        engine.setInputControls (0, {});
        engine.selectProgram (0, keepProgram); settle();
        for (int p : { 100, 101, 102, 103, 110 }) engine.clearProgram (0, p);
        settle();
    }

    // CLAP has to be loadable by the synchronous API, because that is the one
    // the out-of-process host calls. A format claiming it needs an unblocked
    // message thread is refused outright by JUCE with "cannot be instantiated
    // synchronously", which silently broke every CLAP plugin once already.
    {
        perf::ClapPluginFormat clap;
        PluginDescription anyClap;
        anyClap.pluginFormatName = perf::ClapPluginFormat::getFormatName();
        CHECK (! clap.requiresUnblockedMessageThreadDuringCreation (anyClap));

        // The format must be registered where the host looks for it, by name.
        AudioPluginFormatManager fm;
        addDefaultFormatsToManager (fm);
        fm.addFormat (new perf::ClapPluginFormat());
        bool foundClap = false;
        for (auto* f : fm.getFormats())
            if (f->getName() == perf::ClapPluginFormat::getFormatName())
                foundClap = true;
        CHECK (foundClap);

        // An identifier carries both path and plugin id, since one bundle can
        // hold several plugins; round-tripping it must not lose either.
        const auto id = perf::ClapPluginFormat::makeIdentifier ("/a/b/Thing.clap", "com.x.thing");
        CHECK (perf::ClapPluginFormat::pathFromIdentifier (id) == "/a/b/Thing.clap");
        CHECK (perf::ClapPluginFormat::pluginIdFromIdentifier (id) == "com.x.thing");
    }

    std::printf (failures == 0 ? "EngineTest: all checks passed\n" : "EngineTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
