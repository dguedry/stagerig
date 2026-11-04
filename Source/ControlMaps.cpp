#include "ControlMaps.h"
#include <cstring>
#include <map>

namespace perf
{
using namespace juce;

namespace
{
    String str (const var& v, const char* name)
    {
        if (auto* o = v.getDynamicObject()) return o->getProperty (name).toString();
        return {};
    }
}

bool ControlMap::appliesTo (const PluginDescription& d) const
{
    if (! pluginName.equalsIgnoreCase (d.name)) return false;
    return pluginManufacturer.isEmpty() || d.manufacturerName.isEmpty()
        || pluginManufacturer.equalsIgnoreCase (d.manufacturerName);
}

bool ControlMap::isSameAs (const ControlMap& o) const
{
    return pluginName.equalsIgnoreCase (o.pluginName) && name.equalsIgnoreCase (o.name)
        && (pluginManufacturer.isEmpty() || o.pluginManufacturer.isEmpty() || pluginManufacturer.equalsIgnoreCase (o.pluginManufacturer));
}

String ControlMap::colourToText (uint32 c)
{
    return "#" + String::toHexString ((int) (c & 0xffffff)).paddedLeft ('0', 6);
}

uint32 ControlMap::colourFromText (const String& t, uint32 fallback)
{
    const auto hex = t.trim().trimCharactersAtStart ("#");
    if (hex.length() != 6 || ! hex.containsOnly ("0123456789abcdefABCDEF")) return fallback;
    return (uint32) hex.getHexValue32() & 0xffffff;
}

var ControlMap::toVar() const
{
    DynamicObject::Ptr o (new DynamicObject());
    o->setProperty ("format", formatTag);
    o->setProperty ("version", formatVersion);
    o->setProperty ("name", name);
    o->setProperty ("plugin", pluginName);
    if (pluginManufacturer.isNotEmpty()) o->setProperty ("manufacturer", pluginManufacturer);
    if (author.isNotEmpty())             o->setProperty ("author", author);
    if (notes.isNotEmpty())              o->setProperty ("notes", notes);

    Array<var> pageList;
    for (const auto& p : pages)
    {
        DynamicObject::Ptr po (new DynamicObject());
        po->setProperty ("name", p.name);
        Array<var> knobList;
        for (const auto& k : p.knobs)
        {
            DynamicObject::Ptr ko (new DynamicObject());
            if (k.paramId.isNotEmpty())   ko->setProperty ("id", k.paramId);
            if (k.paramName.isNotEmpty()) ko->setProperty ("param", k.paramName);
            if (k.label.isNotEmpty())     ko->setProperty ("label", k.label);
            knobList.add (var (ko.get()));
        }
        po->setProperty ("knobs", var (knobList));
        pageList.add (var (po.get()));
    }
    o->setProperty ("pages", var (pageList));

    Array<var> keyList;
    for (const auto& z : keys)
    {
        DynamicObject::Ptr zo (new DynamicObject());
        zo->setProperty ("low", z.low);
        zo->setProperty ("high", z.high);
        zo->setProperty ("colour", colourToText (z.colour));
        if (z.label.isNotEmpty()) zo->setProperty ("label", z.label);
        keyList.add (var (zo.get()));
    }
    if (! keyList.isEmpty()) o->setProperty ("keys", var (keyList));
    return var (o.get());
}

bool ControlMap::fromVar (const var& v, ControlMap& out, String& error)
{
    auto* o = v.getDynamicObject();
    if (o == nullptr || o->getProperty ("format").toString() != formatTag)
    {
        error = "This is not a StageRig controller setup.";
        return false;
    }
    if ((int) o->getProperty ("version") > formatVersion)
    {
        error = "This setup was made by a newer StageRig. Update StageRig to use it.";
        return false;
    }
    ControlMap m;
    m.name               = str (v, "name").trim();
    m.pluginName         = str (v, "plugin").trim();
    m.pluginManufacturer = str (v, "manufacturer").trim();
    m.author             = str (v, "author");
    m.notes              = str (v, "notes");
    if (m.pluginName.isEmpty())
    {
        error = "The setup does not say which plugin it is for.";
        return false;
    }
    if (m.name.isEmpty()) m.name = m.pluginName;

    if (auto* pageList = o->getProperty ("pages").getArray())
        for (const auto& pv : *pageList)
        {
            ControlPage p;
            p.name = str (pv, "name");
            if (auto* po = pv.getDynamicObject())
                if (auto* knobList = po->getProperty ("knobs").getArray())
                    for (const auto& kv : *knobList)
                    {
                        if ((int) p.knobs.size() >= ControlPage::numKnobs) break;
                        p.knobs.push_back ({ str (kv, "id"), str (kv, "param"), str (kv, "label") });
                    }
            if (p.name.isEmpty()) p.name = "Page " + String ((int) m.pages.size() + 1);
            m.pages.push_back (std::move (p));
            if (m.pages.size() >= 64) break;
        }

    if (auto* keyList = o->getProperty ("keys").getArray())
        for (const auto& kv : *keyList)
        {
            auto* zo = kv.getDynamicObject();
            if (zo == nullptr) continue;
            KeyZone z;
            z.low    = jlimit (0, 127, (int) zo->getProperty ("low"));
            z.high   = jlimit (0, 127, zo->hasProperty ("high") ? (int) zo->getProperty ("high") : z.low);
            if (z.high < z.low) std::swap (z.low, z.high);
            z.colour = colourFromText (zo->getProperty ("colour").toString(), 0xff8000);
            z.label  = zo->getProperty ("label").toString();
            m.keys.push_back (z);
            if (m.keys.size() >= 128) break;
        }

    out = std::move (m);
    return true;
}

Result ControlMap::saveToFile (const File& f) const
{
    if (! f.getParentDirectory().createDirectory()) return Result::fail ("Could not make the folder " + f.getParentDirectory().getFullPathName());
    const auto tmp = f.getSiblingFile (f.getFileName() + ".tmp");
    if (! tmp.replaceWithText (JSON::toString (toVar(), false)))
        return Result::fail ("Could not write " + tmp.getFullPathName());
    if (! tmp.moveFileTo (f))
    {
        tmp.deleteFile();
        return Result::fail ("Could not write " + f.getFullPathName());
    }
    return Result::ok();
}

Result ControlMap::loadFromFile (const File& f, ControlMap& out)
{
    if (! f.existsAsFile()) return Result::fail ("There is no file " + f.getFullPathName());
    if (f.getSize() > 4 * 1024 * 1024) return Result::fail (f.getFileName() + " is too big to be a controller setup.");
    var v;
    if (const auto r = JSON::parse (f.loadFileAsString(), v); r.failed())
        return Result::fail (f.getFileName() + " is not a controller setup: " + r.getErrorMessage());
    String error;
    if (! fromVar (v, out, error)) return Result::fail (f.getFileName() + ": " + error);
    return Result::ok();
}

int parseNoteName (const String& text)
{
    const auto t = text.trim();
    if (t.isEmpty()) return -1;
    if (t.containsOnly ("0123456789")) { const int n = t.getIntValue(); return n >= 0 && n <= 127 ? n : -1; }
    static const int semis[] = { 9, 11, 0, 2, 4, 5, 7 };     // A B C D E F G
    const auto letter = CharacterFunctions::toUpperCase (t[0]);
    if (letter < 'A' || letter > 'G') return -1;
    int pc = semis[letter - 'A'];
    int i = 1;
    if (t[i] == '#') { ++pc; ++i; }
    else if (t[i] == 'b' && t.length() > 2) { --pc; ++i; }
    const auto octaveText = t.substring (i).trim();
    if (octaveText.isEmpty() || ! octaveText.trimCharactersAtStart ("-").containsOnly ("0123456789")) return -1;
    const int n = (octaveText.getIntValue() + 2) * 12 + pc;
    return n >= 0 && n <= 127 ? n : -1;
}

//==============================================================================
ControlMapLibrary::ControlMapLibrary (const File& f) : folder (f)
{
    reload();
}

void ControlMapLibrary::reload()
{
    maps.clear();
    for (const auto& f : folder.findChildFiles (File::findFiles, false, String ("*") + ControlMap::fileExtension))
    {
        ControlMap m;
        if (ControlMap::loadFromFile (f, m).wasOk())
            maps.push_back ({ f, std::move (m) });
    }
    std::sort (maps.begin(), maps.end(), [] (const Entry& a, const Entry& b)
    {
        const int byPlugin = a.map.pluginName.compareNatural (b.map.pluginName);
        return byPlugin != 0 ? byPlugin < 0 : a.map.name.compareNatural (b.map.name) < 0;
    });
}

std::vector<const ControlMap*> ControlMapLibrary::forPlugin (const PluginDescription& d) const
{
    std::vector<const ControlMap*> found;
    for (const auto& e : maps)
        if (e.map.appliesTo (d)) found.push_back (&e.map);
    return found;
}

const ControlMap* ControlMapLibrary::find (const PluginDescription& d, const String& name) const
{
    for (const auto& e : maps)
        if (e.map.appliesTo (d) && (name.isEmpty() || e.map.name.equalsIgnoreCase (name)))
            return &e.map;
    return nullptr;
}

File ControlMapLibrary::fileFor (const ControlMap& m) const
{
    auto base = m.name.equalsIgnoreCase (m.pluginName) ? m.pluginName : m.pluginName + " - " + m.name;
    base = File::createLegalFileName (base).trim();
    if (base.isEmpty()) base = "Setup";
    return folder.getChildFile (base + ControlMap::fileExtension);
}

Result ControlMapLibrary::save (const ControlMap& m)
{
    if (m.pluginName.isEmpty()) return Result::fail ("A setup needs a plugin.");
    // The same setup kept under another file name (imported as "copy of...") goes: one file per setup.
    for (const auto& e : maps)
        if (e.map.isSameAs (m) && e.file != fileFor (m)) e.file.deleteFile();
    const auto r = m.saveToFile (fileFor (m));
    reload();
    return r;
}

Result ControlMapLibrary::remove (const ControlMap& m)
{
    bool gone = false;
    for (const auto& e : maps)
        if (e.map.isSameAs (m)) gone = e.file.deleteFile() || gone;
    reload();
    return gone ? Result::ok() : Result::fail ("That setup was not found.");
}

Result ControlMapLibrary::importFile (const File& f, ControlMap* imported)
{
    ControlMap m;
    if (const auto r = ControlMap::loadFromFile (f, m); r.failed()) return r;
    if (const auto r = save (m); r.failed()) return r;
    if (imported != nullptr) *imported = m;
    return Result::ok();
}

//==============================================================================
// NKS presets
//==============================================================================
namespace
{
    /* MessagePack, as the chunks of an .nksf are written: just what they use,
       read into vars. Anything malformed stops the reading. */
    struct MsgPack
    {
        const uint8* p;
        const uint8* end;
        bool ok = true;

        bool need (size_t n)                    { if ((size_t) (end - p) < n) ok = false; return ok; }
        uint64 be (int n)                       { uint64 v = 0; for (int i = 0; i < n; ++i) v = (v << 8) | *p++; return v; }

        var read (int depth = 0)
        {
            if (depth > 32 || ! need (1)) { ok = false; return {}; }
            const uint8 t = *p++;
            if (t <= 0x7f)               return (int) t;
            if (t >= 0xe0)               return (int) (int8) t;
            if ((t & 0xf0) == 0x80)      return map (t & 0x0f, depth);
            if ((t & 0xf0) == 0x90)      return array (t & 0x0f, depth);
            if ((t & 0xe0) == 0xa0)      return text (t & 0x1f);
            switch (t)
            {
                case 0xc0: return {};
                case 0xc2: return false;
                case 0xc3: return true;
                case 0xc4: case 0xc5: case 0xc6:            // bytes: passed over
                {
                    const int w = t == 0xc4 ? 1 : t == 0xc5 ? 2 : 4;
                    if (! need ((size_t) w)) return {};
                    const auto n = (size_t) be (w);
                    if (need (n)) p += n;
                    return {};
                }
                case 0xca: { if (! need (4)) return {}; const auto b = (uint32) be (4); float f; std::memcpy (&f, &b, 4); return (double) f; }
                case 0xcb: { if (! need (8)) return {}; const auto b = be (8); double d; std::memcpy (&d, &b, 8); return d; }
                case 0xcc: return need (1) ? var ((int) be (1)) : var();
                case 0xcd: return need (2) ? var ((int) be (2)) : var();
                case 0xce: return need (4) ? var ((int64) be (4)) : var();
                case 0xcf: return need (8) ? var ((int64) be (8)) : var();
                case 0xd0: return need (1) ? var ((int) (int8) be (1)) : var();
                case 0xd1: return need (2) ? var ((int) (int16) be (2)) : var();
                case 0xd2: return need (4) ? var ((int) (int32) be (4)) : var();
                case 0xd3: return need (8) ? var ((int64) be (8)) : var();
                case 0xd9: return need (1) ? text ((size_t) be (1)) : var();
                case 0xda: return need (2) ? text ((size_t) be (2)) : var();
                case 0xdb: return need (4) ? text ((size_t) be (4)) : var();
                case 0xdc: return need (2) ? array ((size_t) be (2), depth) : var();
                case 0xdd: return need (4) ? array ((size_t) be (4), depth) : var();
                case 0xde: return need (2) ? map ((size_t) be (2), depth) : var();
                case 0xdf: return need (4) ? map ((size_t) be (4), depth) : var();
                default:   ok = false; return {};
            }
        }
        var text (size_t n)
        {
            if (! need (n)) return {};
            const auto s = String::fromUTF8 ((const char*) p, (int) n);
            p += n;
            return s;
        }
        /* A preset's pages are a few dozen entries; a count beyond this is a
           corrupt or hostile file, not something to allocate for. */
        static constexpr size_t maxEntries = 4096;
        var array (size_t n, int depth)
        {
            if (n > maxEntries) { ok = false; return {}; }
            Array<var> a;
            for (size_t i = 0; i < n && ok; ++i) a.add (read (depth + 1));
            return a;
        }
        var map (size_t n, int depth)
        {
            if (n > maxEntries) { ok = false; return {}; }
            DynamicObject::Ptr o (new DynamicObject());
            for (size_t i = 0; i < n && ok; ++i)
            {
                const auto key = read (depth + 1).toString();
                const auto value = read (depth + 1);
                if (key.isNotEmpty()) o->setProperty (key, value);
            }
            return var (o.get());
        }
    };

    /* "Env Amt" and "Envelope Amount", "Cutoff" and "Filter Cutoff": the
       maker's short names for a knob against the plugin's own. */
    String plainWords (const String& s)
    {
        String out;
        for (auto c : s.toLowerCase()) out << (CharacterFunctions::isLetterOrDigit (c) ? c : (juce_wchar) ' ');
        return StringArray::fromTokens (out, " ", "").joinIntoString (" ");
    }
    /* "amt" for "amount", "pres" for "pressure": the same first letter, and
       the rest in order. Numbers are themselves. */
    bool wordAbbreviates (const String& w, const String& full)
    {
        if (full.startsWith (w)) return true;
        if (w.isEmpty() || full.isEmpty() || w[0] != full[0] || w.containsOnly ("0123456789")) return false;
        int at = 1;
        for (int i = 1; i < w.length(); ++i)
        {
            while (at < full.length() && full[at] != w[i]) ++at;
            if (at++ >= full.length()) return false;
        }
        return true;
    }
    bool wordsAbbreviate (const StringArray& shortWords, const StringArray& longWords)
    {
        int at = 0;
        for (const auto& w : shortWords)
        {
            while (at < longWords.size() && ! wordAbbreviates (w, longWords[at])) ++at;
            if (at == longWords.size()) return false;
            ++at;
        }
        return ! shortWords.isEmpty();
    }
    bool sameKnobName (const String& a, const String& b)
    {
        const auto pa = plainWords (a), pb = plainWords (b);
        if (pa.isEmpty() || pb.isEmpty()) return false;
        if (pa == pb) return true;
        const auto wa = StringArray::fromTokens (pa, " ", ""), wb = StringArray::fromTokens (pb, " ", "");
        return wordsAbbreviate (wa, wb) || wordsAbbreviate (wb, wa);
    }
}

Result NksPreset::read (const File& f, NksPreset& out)
{
    if (f.getSize() > 64 * 1024 * 1024) return Result::fail (f.getFileName() + " is too big to be an NKS preset.");
    MemoryBlock data;
    if (! f.loadFileAsData (data)) return Result::fail ("Could not read " + f.getFullPathName());
    return parse (data.getData(), data.getSize(), out);
}

Result NksPreset::parse (const void* data, size_t size, NksPreset& out)
{
    const auto* d = static_cast<const uint8*> (data);
    if (size < 12 || std::memcmp (d, "RIFF", 4) != 0 || std::memcmp (d + 8, "NIKS", 4) != 0)
        return Result::fail ("This is not an NKS preset.");
    NksPreset n;
    var info, controls, plugin;
    for (size_t at = 12; at + 8 <= size;)
    {
        const uint32 len = (uint32) d[at + 4] | ((uint32) d[at + 5] << 8) | ((uint32) d[at + 6] << 16) | ((uint32) d[at + 7] << 24);
        const auto* body = d + at + 8;
        if (len > size - at - 8) break;
        // Each of these is a version (four bytes) and then MessagePack.
        auto unpack = [&] (var& into) { if (len > 4) { MsgPack m { body + 4, body + len }; into = m.read(); if (! m.ok) into = var(); } };
        if      (std::memcmp (d + at, "NISI", 4) == 0) unpack (info);
        else if (std::memcmp (d + at, "NICA", 4) == 0) unpack (controls);
        else if (std::memcmp (d + at, "PLID", 4) == 0) unpack (plugin);
        at += 8 + (size_t) len + (len & 1);
    }

    n.name   = info["name"].toString();
    n.vendor = info["vendor"].toString();
    if (auto* chain = info["bankchain"].getArray(); chain != nullptr && ! chain->isEmpty()) n.product = chain->getReference (0).toString();
    n.pluginName   = plugin["pluginName"].toString();
    n.pluginVendor = plugin["pluginVendor"].toString();
    if (auto* uid = plugin["VST3.uid"].getArray())
        for (const auto& part : *uid)
            if ((int64) part != 0) n.vst3Ids = true;

    if (auto* pageList = controls["ni8"].getArray())
        for (const auto& pv : *pageList)
        {
            auto* knobs = pv.getArray();
            if (knobs == nullptr) continue;
            ControlPage page;
            StringArray sections;
            bool any = false;
            String section;     // a knob's section goes on to the knobs after it, until the next
            for (const auto& kv : *knobs)
            {
                if ((int) page.knobs.size() >= ControlPage::numKnobs) break;
                ControlKnob k;
                if (auto* o = kv.getDynamicObject())
                    if (const auto sec = o->getProperty ("section").toString(); sec.isNotEmpty()) { section = sec; sections.addIfNotAlreadyThere (sec); }
                if (auto* o = kv.getDynamicObject(); o != nullptr && o->hasProperty ("id"))
                {
                    k.paramId = String ((int64) o->getProperty ("id"));
                    k.label = o->getProperty ("name").toString();
                    k.paramName = section;
                    any = true;
                }
                page.knobs.push_back (k);
            }
            if (! any) continue;
            page.name = sections.isEmpty() ? "Page " + String ((int) n.pages.size() + 1) : sections.joinIntoString (", ");
            n.pages.push_back (std::move (page));
            if (n.pages.size() >= 64) break;
        }
    if (n.pages.empty()) return Result::fail ("This NKS preset has no knob pages.");
    out = std::move (n);
    return Result::ok();
}

int NksPreset::matches (const PluginDescription& d) const
{
    const auto wanted = plainWords (d.name);
    if (wanted.isEmpty()) return 0;
    const auto maker = plainWords (pluginVendor.isNotEmpty() ? pluginVendor : vendor), theirs = plainWords (d.manufacturerName);
    if (maker.isNotEmpty() && theirs.isNotEmpty() && ! maker.contains (theirs) && ! theirs.contains (maker)) return 0;
    int best = 0;
    for (const auto& c : { plainWords (pluginName), plainWords (product) })
    {
        if (c.isEmpty()) continue;
        if (c == wanted)                           best = jmax (best, 3);
        else if (c.endsWith (" " + wanted))        best = jmax (best, 2);    // "spitfire audio bbc symphony orchestra"
        else if (c.startsWith (wanted + " "))      best = jmax (best, 1);    // "super 8 r2"
    }
    return best;
}

String NksPreset::pagesKey() const
{
    String key;
    for (const auto& p : pages)
    {
        key << p.name << '|';
        for (const auto& k : p.knobs) key << k.paramId << '=' << k.label << ';';
        key << '\n';
    }
    return key;
}

ControlMap controlMapFromNks (const NksPreset& nks, const PluginDescription& d, const ParamInfoList& params)
{
    ControlMap m;
    m.name = d.name;
    m.pluginName = d.name;
    m.pluginManufacturer = d.manufacturerName;
    m.author = nks.vendor;
    m.notes = "From " + (nks.vendor.isNotEmpty() ? nks.vendor + "'s" : String ("the maker's")) + " NKS presets.";
    const bool sameKind = d.pluginFormatName == (nks.vst3Ids ? "VST3" : "VST");

    // The maker's name for a knob, with its section and without: "Filter Env: Attack", "Attack".
    auto fits = [] (const ParamInfo& p, const ControlKnob& k)
    {
        return sameKnobName (p.name, k.label) || (k.paramName.isNotEmpty() && sameKnobName (p.name, k.paramName + " " + k.label));
    };
    auto atNumber = [&params] (const ControlKnob& k) -> const ParamInfo*
    {
        const auto n = k.paramId.getLargeIntValue();
        return n >= 0 && n < (int64) params.size() && k.paramId.containsOnly ("0123456789") ? &params[(size_t) n] : nullptr;
    };

    /* VST2 numbers on another kind of plugin: some plugins keep them in their
       VST3 version (the parameter there is the one the maker meant, whatever
       it is called now), some do not. Kept if most of them land on a
       parameter with a name like the maker's. */
    bool numbersHold = false;
    if (! sameKind)
    {
        int asked = 0, landed = 0;
        for (const auto& np : nks.pages)
            for (const auto& k : np.knobs)
                if (k.paramId.isNotEmpty())
                {
                    ++asked;
                    if (const auto* p = atNumber (k); p != nullptr && fits (*p, k)) ++landed;
                }
        numbersHold = landed >= 2 && landed * 3 >= asked * 2;
    }

    /* By name: the same name, or the section and name, first; then a name
       like it, but only one -- "Attack" could be any envelope's. */
    auto byName = [&] (const ControlKnob& k) -> const ParamInfo*
    {
        const auto withSection = k.paramName.isNotEmpty() ? k.paramName + ": " + k.label : String();
        for (const auto& p : params)
            if (p.name.equalsIgnoreCase (k.label) || (withSection.isNotEmpty() && p.name.equalsIgnoreCase (withSection))) return &p;
        for (int pass = 0; pass < 2; ++pass)
        {
            if (pass == 0 && k.paramName.isEmpty()) continue;
            const auto wanted = pass == 0 ? k.paramName + " " + k.label : k.label;
            const ParamInfo* only = nullptr;
            int count = 0;
            for (const auto& p : params)
                if (p.midiController < 0 && sameKnobName (p.name, wanted)) { only = &p; ++count; }
            if (count == 1) return only;
        }
        return nullptr;
    };

    for (const auto& np : nks.pages)
    {
        ControlPage page;
        page.name = np.name;
        bool any = false;
        for (const auto& nk : np.knobs)
        {
            const ParamInfo* found = nullptr;
            if (nk.paramId.isNotEmpty())
            {
                if (sameKind)
                {
                    for (const auto& p : params) if (p.id == nk.paramId) { found = &p; break; }
                }
                else if (numbersHold) found = atNumber (nk);
                if (found == nullptr) found = byName (nk);
            }
            ControlKnob k;
            if (found != nullptr)
            {
                k = { found->id, found->name, nk.label.equalsIgnoreCase (found->name) ? String() : nk.label };
                any = true;
            }
            page.knobs.push_back (k);
        }
        while (! page.knobs.empty() && page.knobs.back().isEmpty()) page.knobs.pop_back();
        if (any) m.pages.push_back (std::move (page));
    }
    return m;
}

Array<File> ControlMapLibrary::defaultNksFolders()
{
    Array<File> folders;
    auto addWindowsTree = [&folders] (const File& drive)
    {
        for (const char* sub : { "Program Files", "Program Files (x86)", "ProgramData", "users", "Users" })
            if (const auto f = drive.getChildFile (sub); f.isDirectory()) folders.addIfNotAlreadyThere (f);
    };
   #if JUCE_LINUX || JUCE_BSD
    const auto home = File::getSpecialLocation (File::userHomeDirectory);
    Array<File> prefixes;
    if (const char* p = std::getenv ("WINEPREFIX"); p != nullptr && *p != 0) prefixes.add (File (p));
    prefixes.add (home.getChildFile (".wine"));
    for (const auto& f : home.findChildFiles (File::findDirectories | File::ignoreHiddenFiles, false, ".wine-*")) prefixes.add (f);
    for (const auto& f : home.findChildFiles (File::findDirectories, false, ".wine-*")) prefixes.addIfNotAlreadyThere (f);
    prefixes.add (home.getChildFile (".local/share/vstenv/prefix"));
    for (const auto& p : prefixes)
        if (p.getChildFile ("drive_c").isDirectory()) addWindowsTree (p.getChildFile ("drive_c"));
   #elif JUCE_WINDOWS
    addWindowsTree (File ("C:\\"));
   #elif JUCE_MAC
    for (const auto& f : { File ("/Library/Application Support"), File ("/Users/Shared"),
                           File::getSpecialLocation (File::userHomeDirectory).getChildFile ("Library/Application Support") })
        if (f.isDirectory()) folders.add (f);
   #endif
    return folders;
}

void ControlMapLibrary::startNksSearch (const Array<File>& folders)
{
    if (nksThread.joinable()) return;
    nksThread = std::thread ([this, folders]
    {
        // The presets by folder: a plugin keeps its presets together, and they mostly share their pages.
        std::map<String, Array<File>> byFolder;
        int seen = 0;
        /* One folder at a time, so a stop (StageRig quitting) is seen within
           one folder's listing however big the tree; symlinks are not followed. */
        std::vector<File> toWalk (folders.begin(), folders.end());
        while (! toWalk.empty() && ! nksStop.load() && seen <= 200000)
        {
            const File dir = toWalk.back();
            toWalk.pop_back();
            if (dir.isSymbolicLink()) continue;
            for (const auto& child : dir.findChildFiles (File::findFilesAndDirectories | File::ignoreHiddenFiles, false))
            {
                if (child.isSymbolicLink()) continue;
                if (child.isDirectory()) { if (toWalk.size() < 100000) toWalk.push_back (child); continue; }
                if (! child.hasFileExtension (".nksf") || ++seen > 200000) continue;
                auto& list = byFolder[dir.getFullPathName()];
                if (list.size() < 16) list.add (child);
            }
        }
        std::vector<NksPreset> found;
        for (const auto& [dir, files] : byFolder)
        {
            if (nksStop.load()) break;
            std::map<String, std::pair<int, NksPreset>> kinds;
            for (const auto& f : files)
            {
                NksPreset n;
                if (NksPreset::read (f, n).failed()) continue;
                auto& k = kinds[n.pagesKey()];
                if (k.first++ == 0) k.second = std::move (n);
            }
            const std::pair<int, NksPreset>* most = nullptr;
            for (const auto& [key, k] : kinds) if (most == nullptr || k.first > most->first) most = &k;
            if (most != nullptr) found.push_back (most->second);
        }
        nks = std::move (found);
        ++nksGeneration;
    });
}

const NksPreset* ControlMapLibrary::findNks (const PluginDescription& d) const
{
    if (nksGeneration.load() == 0) return nullptr;
    const NksPreset* best = nullptr;
    int bestScore = 0;
    for (const auto& n : nks)
        if (const int s = n.matches (d); s > bestScore) { best = &n; bestScore = s; }
    return best;
}

bool ControlMapLibrary::waitForNks (int ms) const
{
    const auto until = Time::getMillisecondCounter() + (uint32) ms;
    while (nksGeneration.load() == 0 && nksThread.joinable() && Time::getMillisecondCounter() < until) Thread::sleep (20);
    return nksGeneration.load() > 0;
}

ControlMapLibrary::~ControlMapLibrary()
{
    nksStop = true;
    if (nksThread.joinable()) nksThread.join();
}

} // namespace perf
