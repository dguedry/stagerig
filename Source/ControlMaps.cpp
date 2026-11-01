#include "ControlMaps.h"

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

} // namespace perf
