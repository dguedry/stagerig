#include "SoundFiles.h"
#include "PluginHost.h"
#include "SamplerPluginFormat.h"
#include "SfzSynth.h"

#include <map>

namespace perf
{
using namespace juce;

namespace
{
    bool isSampler (const PluginDescription& d) { return d.pluginFormatName == SamplerPluginFormat::getFormatName(); }

    /** An effect as a setup holds it that is a plugin (not a send or a built-in MIDI effect). */
    bool isPluginEffect (const var& e) { return e.hasProperty ("plugin") && e.getProperty ("kind", var()).toString().isEmpty(); }

    struct Drop { int chain, index; };
}

File SoundFiles::mySoundsFolder (const File& settingsFolder)
{
    return settingsFolder.getChildFile ("My sounds");
}

bool SoundFiles::pluginExists (const PluginDescription& d, const File& settingsFolder, PluginHost& host)
{
    if (isSampler (d)) return SfzSynth::find (settingsFolder, d.name, d.fileOrIdentifier) != File();
    for (auto* f : host.getFormatManager().getFormats())
        if (f->getName() == d.pluginFormatName) return f->doesPluginStillExist (d);
    return false;
}

bool SoundFiles::write (const File& file, const ProgramDef& program, const String& kind, const String& about, String& error)
{
    auto p = program;
    const auto here = file.getParentDirectory();
    for (auto& s : p.slots)
        if (s.kind == SlotDef::Kind::plugin && isSampler (s.plugin))
            if (const File f (s.plugin.fileOrIdentifier); File::isAbsolutePath (s.plugin.fileOrIdentifier) && f.isAChildOf (here))
                s.plugin.fileOrIdentifier = f.getRelativePathFrom (here).replaceCharacter ('\\', '/');

    auto* o = new DynamicObject();
    o->setProperty ("stagerigSound", 1);
    o->setProperty ("name", p.name);
    o->setProperty ("kind", kind);
    if (about.isNotEmpty()) o->setProperty ("about", about);
    o->setProperty ("program", p.toVar());
    if (! here.createDirectory())
    {
        error = "Could not make the folder " + here.getFullPathName();
        return false;
    }
    if (! file.replaceWithText (JSON::toString (var (o))))
    {
        error = "Could not write " + file.getFullPathName() + " (is the disk full?)";
        return false;
    }
    return true;
}

bool SoundFiles::read (const File& file, const File& settingsFolder, PluginHost& host,
                       ProgramDef& program, StringArray& left, String& error)
{
    const auto v = JSON::parse (file.loadFileAsString());
    if (! v.isObject() || (int) v.getProperty ("stagerigSound", 0) < 1 || ! v.getProperty ("program", var()).isObject())
    {
        error = file.getFileName() + " is not a StageRig sound";
        return false;
    }
    if ((int) v.getProperty ("stagerigSound", 0) > 1)
    {
        error = file.getFileName() + " was made by a newer StageRig: update StageRig to open it";
        return false;
    }

    /* Each effect whose plugin is missing: its first stand-in that is here,
       else it is left out (and the mappings that pointed at it with it). */
    auto prog = JSON::parse (JSON::toString (v.getProperty ("program", var())));     // a copy to change
    std::vector<Drop> drops;
    auto choose = [&] (var chain, int address)
    {
        auto* arr = chain.getArray();
        if (arr == nullptr) return;
        for (int i = 0; i < arr->size(); ++i)
        {
            auto& e = arr->getReference (i);
            if (! isPluginEffect (e)) continue;
            auto ok = [&] (const var& x) { return pluginExists (EffectDef::fromVar (x).plugin, settingsFolder, host); };
            if (ok (e)) continue;
            const auto wanted = EffectDef::fromVar (e).plugin.name;
            var standIn;            // a copy: the stand-ins live inside `e`, which it replaces
            if (auto* alts = e.getProperty ("orElse", var()).getArray())
                for (const auto& alt : *alts)
                    if (isPluginEffect (alt) && ok (alt)) { standIn = alt; break; }
            if (! standIn.isVoid())
                e = standIn;
            else
            {
                drops.push_back ({ address, i });
                left.add (wanted + " (an effect; left out)");
            }
        }
    };
    choose (prog.getProperty ("effects", var()), -1);
    if (auto* slots = prog.getProperty ("slots", var()).getArray())
        for (int s = 0; s < slots->size(); ++s)
            choose (slots->getReference (s).getProperty ("effects", var()), s);

    program = ProgramDef::fromVar (prog);
    for (auto d = drops.rbegin(); d != drops.rend(); ++d)       // the last first, so earlier indexes hold
    {
        auto* chain = d->chain < 0 ? &program.effects : &program.slots[(size_t) d->chain].effects;
        if (d->index >= (int) chain->size()) continue;
        chain->erase (chain->begin() + d->index);
        auto& maps = program.mappings;
        maps.erase (std::remove_if (maps.begin(), maps.end(), [&] (const MappingDef& m) { return m.slot == d->chain && m.effect == d->index; }), maps.end());
        for (auto& m : maps)
            if (m.slot == d->chain && m.effect > d->index) --m.effect;
    }

    const auto here = file.getParentDirectory();
    for (auto& s : program.slots)
    {
        if (s.kind != SlotDef::Kind::plugin) continue;
        if (isSampler (s.plugin) && ! File::isAbsolutePath (s.plugin.fileOrIdentifier))
            s.plugin.fileOrIdentifier = here.getChildFile (s.plugin.fileOrIdentifier).getFullPathName();
        if (! pluginExists (s.plugin, settingsFolder, host))
            left.add (s.plugin.name + (isSampler (s.plugin) ? " (a sampled instrument that is not on this computer)" : " (not installed)"));
    }
    if (program.name.isEmpty()) program.name = v.getProperty ("name", file.getFileNameWithoutExtension()).toString();
    return true;
}

Array<SoundFiles::Info> SoundFiles::installed (const File& settingsFolder, PluginHost& host)
{
    Array<Info> found;
    Array<File> seen;
    std::map<String, bool> exists;        // each plugin asked about once
    auto here = [&] (const PluginDescription& d)
    {
        const auto key = d.pluginFormatName + "|" + d.fileOrIdentifier + "|" + d.name;
        auto it = exists.find (key);
        if (it == exists.end()) it = exists.emplace (key, pluginExists (d, settingsFolder, host)).first;
        return it->second;
    };

    auto roots = SfzSynth::searchRoots (settingsFolder);
    roots.push_back ({ mySoundsFolder (settingsFolder), true });
    std::function<void (const File&, int, const String&)> look = [&] (const File& d, int depth, const String& library)
    {
        for (const auto& f : d.findChildFiles (File::findFiles, false, String ("*") + extension))
        {
            if (seen.contains (f)) continue;
            seen.add (f);
            const auto v = JSON::parse (f.loadFileAsString());
            if (! v.isObject() || (int) v.getProperty ("stagerigSound", 0) != 1) continue;
            Info i;
            i.file = f;
            i.name = v.getProperty ("name", f.getFileNameWithoutExtension()).toString();
            i.about = v.getProperty ("about", var()).toString();
            i.kind = v.getProperty ("kind", var()).toString();
            if (! SfzSynth::categories().contains (i.kind)) i.kind = SfzSynth::categoryOf (i.name);
            i.library = library.isNotEmpty() ? library : d.getFileName();
            const auto here_ = f.getParentDirectory();
            if (auto* slots = v.getProperty ("program", var()).getProperty ("slots", var()).getArray())
                for (const auto& sv : *slots)
                {
                    auto s = SlotDef::fromVar (sv);
                    if (s.kind != SlotDef::Kind::plugin) continue;
                    if (isSampler (s.plugin) && ! File::isAbsolutePath (s.plugin.fileOrIdentifier))
                        s.plugin.fileOrIdentifier = here_.getChildFile (s.plugin.fileOrIdentifier).getFullPathName();
                    if (! here (s.plugin)) i.needs.addIfNotAlreadyThere (s.plugin.name);
                }
            found.add (i);
        }
        if (depth > 0)
            for (const auto& sub : d.findChildFiles (File::findDirectories, false))
                look (sub, depth - 1, library.isNotEmpty() ? library : sub.getFileName());
    };
    for (const auto& root : roots)
        if (root.dir.isDirectory())
            look (root.dir, 3, root.isLibrary ? root.dir.getFileName() : String());

    const auto kinds = SfzSynth::categories();
    std::sort (found.begin(), found.end(), [&] (const Info& a, const Info& b)
    {
        if (a.kind != b.kind) return kinds.indexOf (a.kind) < kinds.indexOf (b.kind);
        return a.name.compareNatural (b.name, false) < 0;
    });
    return found;
}

} // namespace perf
