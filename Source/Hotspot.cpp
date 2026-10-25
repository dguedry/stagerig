#include "Hotspot.h"

using namespace juce;

namespace perf
{

//==============================================================================
/** Runs a command and returns its output. ChildProcess rather than a shell, so
    nothing a user typed (a network name with a quote in it) can be misread as
    shell syntax. */
static String run (const StringArray& args, int timeoutMs = 15000, int* exitCode = nullptr)
{
    ChildProcess p;
    if (! p.start (args, ChildProcess::wantStdOut | ChildProcess::wantStdErr))
    {
        if (exitCode != nullptr) *exitCode = -1;
        return {};
    }
    const auto out = p.readAllProcessOutput();
    if (! p.waitForProcessToFinish (timeoutMs))
    {
        p.kill();
        if (exitCode != nullptr) *exitCode = -1;
        return out;
    }
    if (exitCode != nullptr) *exitCode = p.getExitCode();
    return out;
}

/** nmcli --terse returns "KEY:value" lines, in whatever order the fields were
    asked for, so always look a value up by name. Values can themselves contain
    colons, hence splitting only on the first one. */
static String field (const String& out, const String& key)
{
    for (auto& line : StringArray::fromLines (out))
        if (line.startsWith (key + ":"))
            return line.fromFirstOccurrenceOf (":", false, false).trim();
    return {};
}

static bool looksVirtual (const String& iface)
{
    return iface.startsWith ("docker") || iface.startsWith ("br-") || iface.startsWith ("virbr")
        || iface.startsWith ("lxdbr")  || iface.startsWith ("veth") || iface.startsWith ("vnet")
        || iface.startsWith ("tun")    || iface.startsWith ("tap")  || iface.startsWith ("p2p-");
}

bool Hotspot::available()
{
    return File ("/usr/bin/nmcli").existsAsFile() || File ("/bin/nmcli").existsAsFile();
}

//==============================================================================
Array<Hotspot::Adapter> Hotspot::adapters()
{
    Array<Adapter> found;
    if (! available()) return found;

    const auto status = run ({ "nmcli", "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device", "status" });

    for (auto& line : StringArray::fromLines (status))
    {
        const auto device = line.upToFirstOccurrenceOf (":", false, false);
        auto rest        = line.fromFirstOccurrenceOf (":", false, false);
        const auto type  = rest.upToFirstOccurrenceOf (":", false, false);
        rest             = rest.fromFirstOccurrenceOf (":", false, false);
        const auto state = rest.upToFirstOccurrenceOf (":", false, false);

        if (type != "wifi" || device.isEmpty() || looksVirtual (device)) continue;

        Adapter a;
        a.interfaceName = device;

        const auto info = run ({ "nmcli", "-t", "-f", "GENERAL.PRODUCT,WIFI-PROPERTIES.AP",
                                 "device", "show", device });
        a.description = field (info, "GENERAL.PRODUCT");
        a.supportsAccessPoint = field (info, "WIFI-PROPERTIES.AP").equalsIgnoreCase ("yes");

        // "connected" means this adapter is carrying a connection right now.
        // Using it for a hotspot would most likely drop that connection, since
        // one radio rarely does both at once.
        a.inUseAsClient = state.startsWith ("connected");

        if (a.description.isEmpty()) a.description = device;
        found.add (a);
    }

    // An idle adapter costs nothing to commandeer, so offer it first. Adapters
    // that cannot do access point mode sink to the bottom but stay listed, so a
    // user can see why their adapter is not being offered.
    std::stable_sort (found.begin(), found.end(), [] (const Adapter& a, const Adapter& b)
    {
        if (a.supportsAccessPoint != b.supportsAccessPoint) return a.supportsAccessPoint;
        if (a.inUseAsClient != b.inUseAsClient)             return ! a.inUseAsClient;
        return false;
    });
    return found;
}

//==============================================================================
Hotspot::State Hotspot::state()
{
    State s;
    if (! available()) return s;

    // Any active connection running in access point mode counts, whether we
    // created it or the user set one up themselves.
    const auto active = run ({ "nmcli", "-t", "-f", "NAME,DEVICE,TYPE", "connection", "show", "--active" });

    for (auto& line : StringArray::fromLines (active))
    {
        const auto name   = line.upToFirstOccurrenceOf (":", false, false);
        auto rest         = line.fromFirstOccurrenceOf (":", false, false);
        const auto device = rest.upToFirstOccurrenceOf (":", false, false);
        const auto type   = rest.fromFirstOccurrenceOf (":", false, false);

        if (! type.contains ("wireless") && type != "wifi") continue;

        const auto details = run ({ "nmcli", "-t", "-f",
                                    "802-11-wireless.mode,802-11-wireless.ssid",
                                    "connection", "show", name });
        if (field (details, "802-11-wireless.mode") != "ap") continue;

        s.active = true;
        s.interfaceName = device;
        s.networkName = field (details, "802-11-wireless.ssid");

        // nmcli indexes repeated fields with brackets -- "IP4.ADDRESS[1]:10.42.0.1/24"
        // -- so match the stem and strip the prefix, rather than guessing an index.
        const auto ip = run ({ "nmcli", "-t", "-f", "IP4.ADDRESS", "device", "show", device });
        for (auto& l : StringArray::fromLines (ip))
            if (l.startsWith ("IP4.ADDRESS"))
            {
                s.address = l.fromFirstOccurrenceOf (":", false, false).trim()
                             .upToFirstOccurrenceOf ("/", false, false);
                break;
            }
        return s;
    }
    return s;
}

//==============================================================================
static const char* kProfileName = "stagerig-stage";

String Hotspot::start (const Config& c)
{
    if (! available())
        return "This needs NetworkManager, which this system does not appear to have.";
    if (c.interfaceName.isEmpty())
        return "No wifi adapter was chosen.";
    if (c.password.length() < 8)
        return "The wifi password must be at least 8 characters.";
    if (c.networkName.isEmpty())
        return "The network needs a name.";

    // Replace any previous profile outright: a half-edited one left over from an
    // earlier attempt is harder to reason about than a fresh one.
    int code = 0;
    run ({ "nmcli", "connection", "delete", kProfileName }, 15000, &code);

    const auto add = run ({ "nmcli", "connection", "add", "type", "wifi",
                            "ifname", c.interfaceName, "con-name", kProfileName,
                            "autoconnect", "no", "ssid", c.networkName }, 20000, &code);
    if (code != 0)
        return "Could not create the wifi network: " + add.trim();

    // band bg = 2.4GHz: shorter range than 5GHz in theory, but it goes through
    // people and walls, every tablet has it, and no country locks channel 6
    // behind radar detection the way it does parts of the 5GHz band.
    const auto mod = run ({ "nmcli", "connection", "modify", kProfileName,
                            "802-11-wireless.mode", "ap",
                            "802-11-wireless.band", "bg",
                            "ipv4.method", "shared",
                            "ipv6.method", "ignore",
                            "wifi-sec.key-mgmt", "wpa-psk",
                            "wifi-sec.proto", "rsn",
                            "wifi-sec.pairwise", "ccmp",
                            "wifi-sec.group", "ccmp",
                            "wifi-sec.psk", c.password }, 20000, &code);
    if (code != 0)
        return "Could not configure the wifi network: " + mod.trim();

    const auto up = run ({ "nmcli", "connection", "up", kProfileName }, 45000, &code);
    if (code != 0)
    {
        run ({ "nmcli", "connection", "delete", kProfileName });
        auto why = up.trim();
        if (why.containsIgnoreCase ("not authorized") || why.containsIgnoreCase ("not permitted"))
            why = "this system requires an administrator to create a network.";
        return "The wifi network did not start: " + why;
    }
    return {};
}

String Hotspot::stop()
{
    if (! available()) return {};
    int code = 0;
    const auto out = run ({ "nmcli", "connection", "down", kProfileName }, 30000, &code);
    if (code != 0 && ! out.containsIgnoreCase ("not an active"))
        return "Could not stop the wifi network: " + out.trim();
    return {};
}

//==============================================================================
Hotspot::Config Hotspot::load (PropertiesFile& settings)
{
    Config c;
    c.interfaceName = settings.getValue ("hotspotInterface");
    c.networkName   = settings.getValue ("hotspotNetwork", "StageRig");
    c.password      = settings.getValue ("hotspotPassword");
    if (c.password.isEmpty()) c.password = suggestPassword();
    return c;
}

void Hotspot::save (PropertiesFile& settings, const Config& c)
{
    settings.setValue ("hotspotInterface", c.interfaceName);
    settings.setValue ("hotspotNetwork", c.networkName);
    settings.setValue ("hotspotPassword", c.password);
    settings.saveIfNeeded();
}

String Hotspot::suggestPassword()
{
    // Typed once per tablet, off a screen, in bad light: no characters that
    // misread and no punctuation to hunt for on a tablet keyboard.
    static const char* words[] = { "stage", "encore", "chorus", "bridge", "tempo", "reverb", "octave", "cadence" };
    auto& rng = Random::getSystemRandom();
    String s (words[rng.nextInt (8)]);
    s << (rng.nextInt (90) + 10);
    s << words[rng.nextInt (8)];
    return s;
}

//==============================================================================
String Hotspot::firewallWarning (int port)
{
    // ufw is the common case on Debian and Ubuntu. Reading its status needs
    // root, but the numeric rule list does not distinguish "no rule" from
    // "cannot read", so treat an unreadable firewall as "nothing to say"
    // rather than crying wolf.
    const File ufw ("/usr/sbin/ufw");
    if (! ufw.existsAsFile()) return {};

    int code = 0;
    const auto status = run ({ "ufw", "status" }, 8000, &code);
    if (code != 0 || ! status.containsIgnoreCase ("Status: active")) return {};

    // An explicit rule for our port, however scoped, means someone has already
    // thought about this.
    if (status.contains (String (port))) return {};

    return "Your firewall (ufw) is on and does not list port " + String (port) + ", so a tablet will "
           "not be able to connect. To allow it from your own networks only:\n\n"
           "    sudo ufw allow from 192.168.0.0/16 to any port " + String (port) + " proto tcp\n"
           "    sudo ufw allow from 10.0.0.0/8 to any port " + String (port) + " proto tcp";
}

} // namespace perf
