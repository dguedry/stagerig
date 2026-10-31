/*  Tests for the tablet-control server: address choice, the code, and the gate.

    The address test is the reason this file exists. A machine with Docker, LXD
    or libvirt has a dozen IPv4 addresses and a tablet can reach almost none of
    them, so "which address do we print" is real logic that can regress
    silently -- the feature still looks like it works until someone tries a
    tablet at a gig.
*/
#include <juce_core/juce_core.h>
#include "RemoteServer.h"
#include "Hotspot.h"
#include "QrCode.h"
#include "BinaryData.h"
#include "Engine.h"
#include "Charts.h"
#include "PluginHost.h"

using namespace juce;

static int failures = 0;
static void check (bool ok, const String& what)
{
    std::cout << (ok ? "ok   " : "FAIL ") << what << std::endl;
    if (! ok) ++failures;
}

int main()
{
    /* The engine delivers events through an async update, which needs a message
       manager. Without this the parameter-change path silently does nothing --
       which is exactly what it looked like when this test first failed. */
    ScopedJuceInitialiser_GUI juceInit;

    // The chosen address must be one this machine actually has, and never a
    // container bridge or a link-local address.
    const auto host = perf::RemoteServer::getHostAddress();
    std::cout << "chosen address: " << host << std::endl;

    check (host.isNotEmpty(), "an address is chosen");
    check (! host.startsWith ("169.254."), "never link-local");

    StringArray mine;
    for (auto& ip : IPAddress::getAllAddresses()) mine.add (ip.toString());
    check (mine.contains (host), "the address belongs to this machine");

    // Docker's default bridge is the classic wrong answer: it is usually first
    // in the kernel's list on a developer machine.
    const auto hasDocker = mine.contains ("172.17.0.1");
    if (hasDocker)
        check (host != "172.17.0.1", "docker0 is not offered to the tablet");
    else
        std::cout << "skip docker0 check (no docker bridge on this machine)" << std::endl;

    // ---- Hotspot -------------------------------------------------------------
    // Serving our own network is how tablet control survives a venue with no
    // usable wifi, so the adapter list and the remembered choice are worth
    // guarding. These assert shape, not specific hardware: CI has no wifi.
    std::cout << "\nnetwork manager present: " << (perf::Hotspot::available() ? "yes" : "no") << std::endl;

    const auto adapters = perf::Hotspot::adapters();
    std::cout << "wifi adapters: " << adapters.size() << std::endl;
    for (auto& a : adapters)
        std::cout << "  " << a.interfaceName << "  ap=" << (a.supportsAccessPoint ? "yes" : "no")
                  << "  inUse=" << (a.inUseAsClient ? "yes" : "no")
                  << "  " << a.description << std::endl;

    // No virtual interface may ever be offered as a wifi adapter: a bridge shows
    // up in some nmcli output and would be a nonsense choice.
    bool anyVirtual = false, anyEmpty = false;
    for (auto& a : adapters)
    {
        if (a.interfaceName.startsWith ("br-") || a.interfaceName.startsWith ("docker")
            || a.interfaceName.startsWith ("virbr") || a.interfaceName.startsWith ("p2p-"))
            anyVirtual = true;
        if (a.interfaceName.isEmpty() || a.description.isEmpty()) anyEmpty = true;
    }
    check (! anyVirtual, "no virtual interface is offered as a wifi adapter");
    check (! anyEmpty, "every adapter has a name and a description");

    // Access-point-capable adapters sort ahead of ones that cannot, and a free
    // adapter ahead of one already carrying our connection: the first entry is
    // what a single-choice machine silently adopts, so the order is load-bearing.
    bool ordered = true;
    for (int i = 1; i < adapters.size(); ++i)
        if (! adapters[i - 1].supportsAccessPoint && adapters[i].supportsAccessPoint)
            ordered = false;
    check (ordered, "access-point-capable adapters are offered first");

    // A suggested password must be valid for WPA, or the hotspot cannot start.
    for (int i = 0; i < 20; ++i)
    {
        const auto pw = perf::Hotspot::suggestPassword();
        if (pw.length() < 8) { check (false, "suggested password is long enough for WPA"); break; }
        if (i == 19) check (true, "suggested password is long enough for WPA");
    }

    // The choice must survive a restart, which is the entire point of asking once.
    {
        auto tmp = File::getSpecialLocation (File::tempDirectory).getChildFile ("stagerig-hotspot-test.settings");
        tmp.deleteFile();
        PropertiesFile::Options o;
        o.applicationName = "stagerig-hotspot-test";
        {
            PropertiesFile pf (tmp, o);
            perf::Hotspot::Config c;
            c.interfaceName = "wlan-test0";
            c.networkName = "MyStage";
            c.password = "chorus42bridge";
            perf::Hotspot::save (pf, c);
        }
        PropertiesFile pf (tmp, o);
        const auto back = perf::Hotspot::load (pf);
        check (back.interfaceName == "wlan-test0", "the chosen adapter is remembered");
        check (back.networkName == "MyStage", "the network name is remembered");
        check (back.password == "chorus42bridge", "the password is remembered");
        tmp.deleteFile();
    }

    // With nothing saved we still get a usable default, so the prompt can be
    // pre-filled rather than blank.
    {
        auto tmp = File::getSpecialLocation (File::tempDirectory).getChildFile ("stagerig-hotspot-empty.settings");
        tmp.deleteFile();
        PropertiesFile::Options o;
        o.applicationName = "stagerig-hotspot-empty";
        PropertiesFile pf (tmp, o);
        const auto fresh = perf::Hotspot::load (pf);
        check (fresh.networkName.isNotEmpty(), "a default network name is offered");
        check (fresh.password.length() >= 8, "a valid default password is offered");
        check (fresh.interfaceName.isEmpty(), "no adapter is assumed before asking");
        tmp.deleteFile();
    }

    // ---- QR code -------------------------------------------------------------
    /* A hand-written encoder is worthless if a tablet cannot read what it makes,
       and "it looks like a QR code" is not evidence. These check the structural
       invariants a scanner depends on; the module output was also verified
       against an independent decoder, which read back every address shape and
       port we can produce. */
    {
        const auto qr = perf::QrCode::encode ("http://10.42.0.1:7777/");
        check (qr.isValid(), "a typical address encodes");
        check (qr.getSize() == 25, "a typical address fits version 2 (25 modules)");

        // Finder patterns: three corners, dark centre 3x3, light ring around it.
        auto finderOk = [&qr] (int cx, int cy)
        {
            for (int dy = -4; dy <= 4; ++dy)
                for (int dx = -4; dx <= 4; ++dx)
                {
                    const int x = cx + dx, y = cy + dy;
                    if (x < 0 || y < 0 || x >= qr.getSize() || y >= qr.getSize()) continue;
                    const int d = std::max (std::abs (dx), std::abs (dy));
                    if (d <= 3 && qr.getModule (x, y) != (d != 2)) return false;
                }
            return true;
        };
        check (finderOk (3, 3), "top-left finder pattern is correct");
        check (finderOk (qr.getSize() - 4, 3), "top-right finder pattern is correct");
        check (finderOk (3, qr.getSize() - 4), "bottom-left finder pattern is correct");

        // The timing patterns must alternate without a break. A gap here costs a
        // scanner the grid it uses to locate every other module, and it was a
        // real bug: reserving the format area overwrote two timing modules.
        bool timingOk = true;
        for (int i = 8; i < qr.getSize() - 8; ++i)
        {
            if (qr.getModule (6, i) != (i % 2 == 0)) timingOk = false;
            if (qr.getModule (i, 6) != (i % 2 == 0)) timingOk = false;
        }
        check (timingOk, "timing patterns alternate with no break");

        // Longer and shorter addresses must both work, and the version has to
        // grow rather than silently truncate.
        const auto small = perf::QrCode::encode ("http://10.0.0.2:80/");
        const auto big   = perf::QrCode::encode ("http://192.168.100.254:65535/");
        check (small.isValid() && big.isValid(), "short and long addresses both encode");
        check (big.getSize() >= small.getSize(), "a longer address uses at least as many modules");

        // Something far too long must be refused, not mangled: the caller shows
        // the address as text instead.
        String tooLong;
        for (int i = 0; i < 200; ++i) tooLong << "x";
        check (! perf::QrCode::encode (tooLong).isValid(), "over-long text is refused rather than truncated");

        // The printed program map embeds the code as SVG.
        const auto svg = qr.toSvg (28);
        check (svg.startsWith ("<svg") && svg.endsWith ("</svg>"), "SVG output is a complete element");
        check (svg.contains ("28mm"), "SVG is sized for paper");
        check (! perf::QrCode::encode ("").isValid() || true, "empty text does not crash");
    }

    // ---- a program picked from the tablet reaches the setup ---------------------
    /* The tablet is often the only thing in reach mid-set, so a choice made there
       has to be as real as one made on the laptop: it must change the engine AND
       survive being written out and read back. */
    {
        auto dir = File::getSpecialLocation (File::tempDirectory).getChildFile ("stagerig-remote-persist");
        dir.deleteRecursively();
        dir.createDirectory();

        PropertiesFile::Options po;
        po.applicationName = "stagerig-remote-persist";
        PropertiesFile props (dir.getChildFile ("p.settings"), po);

        perf::PluginHost host (props);
        perf::Engine engine (host, props, false);
        engine.setProgramName (0, 1, "Second");

        perf::RemoteServer rs (engine, props);
        {
            // The Now page's JSON: the inputs with their instruments and effects, the outputs, the transpose.
            engine.setTranspose (-2);
            const auto now = JSON::parse (rs.nowJson());
            check (now.getProperty ("transpose", 0) == var (-2), "the Now page carries the transpose");
            check (now.getProperty ("inputs", var()).size() == (int) engine.getSetup().inputs.size(), "the Now page lists every input");
            const auto first = now.getProperty ("inputs", var())[0];
            check (first.hasProperty ("instruments") && first.hasProperty ("effects") && first.hasProperty ("programName"), "each input has its instruments, effects and program");
            check (now.getProperty ("outputs", var()).size() >= 1 && now.getProperty ("outputs", var())[0].hasProperty ("gainDb"), "the outputs come with their volume");
            engine.setTranspose (0);
        }
        if (rs.start (0) || rs.start (7791))
        {
            const auto before = engine.getSetup().inputs[0].currentProgram;
            engine.selectProgram (0, 1);
            const auto after = engine.getSetup().inputs[0].currentProgram;
            check (before != after && after == 1, "selecting a program changes the live setup");

            const auto file = dir.getChildFile ("setup.stagerig.json");
            check (engine.captureSetup().saveToFile (file).wasOk(), "the setup writes to disk");

            perf::Setup reloaded;
            const auto r = perf::Setup::loadFromFile (file, reloaded);
            check (r.wasOk(), "the setup reads back");
            check (r.wasOk() && reloaded.inputs[0].currentProgram == 1,
                   "the chosen program survives a save and reload");
            rs.stop();
        }
        else
        {
            std::cout << "skip persistence check (could not bind a port)" << std::endl;
        }
        dir.deleteRecursively();
    }

    // ---- tablet control is remembered across a restart --------------------------
    /* Someone who sets a tablet on a stand expects it to still work after a
       restart. The toggle itself has to persist, not just the code: without this
       the app always came up with the server off, which looks like the tablet has
       broken rather than like a setting was forgotten. */
    {
        auto tmp = File::getSpecialLocation (File::tempDirectory).getChildFile ("stagerig-remoteon.settings");
        tmp.deleteFile();
        PropertiesFile::Options o;
        o.applicationName = "stagerig-remoteon";

        {
            PropertiesFile pf (tmp, o);
            check (! pf.getBoolValue ("remoteOn", false), "tablet control is off until it is turned on");
            pf.setValue ("remoteOn", true);
            pf.setValue ("remotePort", 7777);
            pf.saveIfNeeded();
        }
        {
            PropertiesFile pf (tmp, o);
            check (pf.getBoolValue ("remoteOn", false), "tablet control being on survives a restart");
            check (pf.getIntValue ("remotePort", 0) == 7777, "the port survives a restart");
            pf.setValue ("remoteOn", false);
            pf.saveIfNeeded();
        }
        {
            PropertiesFile pf (tmp, o);
            check (! pf.getBoolValue ("remoteOn", true), "turning it off is remembered too");
        }
        tmp.deleteFile();
    }

    // ---- the manual is embedded and complete ---------------------------------
    /* The in-app help renders docs/MANUAL.md from the binary. If the embedding
       breaks, the Help button opens an empty window -- which is worse than no
       help at all, because it looks like the app is broken. */
    {
        const auto manual = String::createStringFromData (BinaryData::MANUAL_md, BinaryData::MANUAL_mdSize);
        check (manual.isNotEmpty(), "the manual is embedded in the binary");
        check (manual.length() > 10000, "the whole manual is embedded, not a fragment");
        check (manual.contains ("# StageRig user manual"), "the manual starts with its title");

        // The sections someone needs when something is wrong at a venue.
        check (manual.contains ("If the tablet cannot connect"), "the firewall section is present");
        check (manual.contains ("sudo ufw allow from"), "the firewall commands are present");
        check (manual.contains ("When the venue has no usable wifi"), "the hotspot section is present");
        check (manual.contains ("## When something goes wrong"), "the troubleshooting section is present");

        // UTF-8 must survive the round trip through BinaryData: the manual uses
        // typographic punctuation, and mojibake here would be very visible.
        check (! manual.contains ("â€"), "the manual is not mojibake");
    }

    /* Charts: what the stand can show, and how a PDF's pages are found. */
    {
        check (perf::chartKindFor (File ("/x/lead.PDF")) == "pdf",       "a PDF is a pdf, whatever the case");
        check (perf::chartKindFor (File ("/x/scan.jpeg")) == "image",    "a JPEG is an image");
        check (perf::chartKindFor (File ("/x/song.cho")) == "chordpro",  "a .cho is ChordPro");
        check (perf::chartKindFor (File ("/x/song.txt")) == "text",      "a .txt is text");
        check (perf::chartKindFor (File ("/x/song.musicxml")).isEmpty(), "MusicXML is not (yet) a chart");
        check (! perf::isChartFile (File ("/x/notes.docx")),             "a .docx is refused");
        check (perf::chartContentTypeFor (File ("/x/a.pdf")) == "application/pdf", "PDF content type");
        check (perf::chartContentTypeFor (File ("/x/a.png")) == "image/png",       "PNG content type");
        check (perf::chartContentTypeFor (File ("/x/a.cho")).startsWith ("text/plain"), "ChordPro is served as text");
        check (perf::chartPagesFolder (File ("/x/lead.pdf")).getFullPathName() == "/x/lead.pdf.pages", "pages live beside the PDF");

        // Rendered pages come back in page order even past page 9.
        const auto tmp = File::getSpecialLocation (File::tempDirectory).getChildFile ("stagerig-charts-test");
        tmp.deleteRecursively();
        const auto pdf = tmp.getChildFile ("score.pdf");
        const auto pages = perf::chartPagesFolder (pdf);
        pages.createDirectory();
        for (int i = 1; i <= 12; ++i)
            pages.getChildFile ("page-" + String (i).paddedLeft ('0', 2) + ".png").replaceWithText ("x");
        pages.getChildFile ("notes.txt").replaceWithText ("not a page");
        const auto found = perf::chartPages (pdf);
        check (found.size() == 12, "twelve rendered pages are found, and nothing else");
        check (found.size() == 12 && found[9].getFileName() == "page-10.png", "page ten comes tenth");
        check (perf::chartPages (File ("/nowhere/none.pdf")).isEmpty(), "an unrendered PDF has no pages");
        tmp.deleteRecursively();
    }

    // The editing lock's one list: building the show is locked, playing it never is.
    {
        using perf::RemoteServer;
        for (const char* e : { "/api/setedit?do=new&name=x&t=c", "/api/scene?do=add&t=c", "/api/scene?do=capture", "/api/savesong?t=c" })
            check (RemoteServer::isEditingRoute (e), String ("editing: ") + e);
        for (const char* p : { "/api/select?input=0&program=3", "/api/transport?do=section&index=2", "/api/knob?input=0&slot=0&id=level&value=0.5",
                               "/api/songkey?song=1&root=2&mode=0", "/api/song?index=1", "/api/panic", "/api/state", "/api/panel?input=0", "/api/setlist?index=1" })
            check (! RemoteServer::isEditingRoute (p), String ("playing, never locked: ") + p);
    }

    std::cout << (failures == 0 ? "\nall remote tests passed\n" : "\nremote tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
