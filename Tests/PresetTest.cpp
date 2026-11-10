/*  The makers' sounds (NKS presets) on real plugins: found for the right
    plugin, and loaded into it -- the plugin's state afterwards holds that
    sound. Needs the plugins installed (yabridge VST3s); a plugin that is not
    is skipped. Run by hand: it starts Wine. */
#include "ControlMaps.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <iostream>

using namespace juce;
using namespace perf;

static int failures = 0;
static void check (bool ok, const String& what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
    if (! ok) ++failures;
}

/** One part of a JUCE VST3 state. */
static MemoryBlock part (const MemoryBlock& state, const char* name)
{
    MemoryBlock m;
    if (auto xml = AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize()))
        if (auto* c = xml->getChildByName (name)) m.fromBase64Encoding (c->getAllSubText());
    return m;
}

static void tryPlugin (ControlMapLibrary& lib, const File& bundle)
{
    std::cout << "\n" << bundle.getFileNameWithoutExtension() << std::endl;
    if (! bundle.exists()) { std::cout << "  (not installed: skipped)" << std::endl; return; }
    VST3PluginFormat fmt;
    OwnedArray<PluginDescription> types;
    fmt.findAllTypesForFile (types, bundle.getFullPathName());
    if (types.isEmpty()) { check (false, "the plugin is found in its bundle"); return; }
    const auto& desc = *types[0];

    const auto sounds = lib.soundsFor (desc);
    check (sounds.size() >= 2, String ((int) sounds.size()) + " sounds found for " + desc.name);
    if (sounds.size() < 2) return;
    for (const auto& s : sounds)
        if (s.matches (desc) == 0) { check (false, s.file.getFullPathName() + " is not this plugin's"); return; }
    std::cout << "  e.g. " << sounds.front().name << " (" << sounds.front().bank << ") "
              << sounds.front().tags.joinIntoString (", ") << std::endl;

    String err;
    auto inst = fmt.createInstanceFromDescription (desc, 48000, 256, err);
    if (inst == nullptr) { check (false, "the plugin loads: " + err); return; }

    MemoryBlock before;
    inst->getStateInformation (before);

    auto load = [&] (const NksSound& s, MemoryBlock& after) -> bool
    {
        MemoryBlock state;
        const auto r = nksStateFor (s.file, before, state);
        check (r.wasOk(), "a state is made for " + s.name + (r.failed() ? ": " + r.getErrorMessage() : String()));
        if (r.failed()) return false;
        inst->setStateInformation (state.getData(), (int) state.getSize());
        inst->getStateInformation (after);
        return true;
    };

    /* A plugin keeps a sound in a newer form of its own than the file's
       (Super 8 and SWAM do), and some let a setting drift a hair between
       reads (SWAM's room): loaded twice, the sound reads back all but the
       same, and far nearer itself than another sound. */
    MemoryBlock first, second, again;
    const auto& a = sounds.front();
    const auto& b = sounds.back();
    if (! load (a, first) || ! load (b, second) || ! load (a, again)) return;
    check (first != before || second != before, "the plugin's sound changed");
    check (first != second, a.name + " and " + b.name + " are different sounds in the plugin");
    auto apart = [] (const MemoryBlock& x, const MemoryBlock& y)    // the share of bytes that differ
    {
        const auto n = jmax (x.getSize(), y.getSize());
        if (n == 0) return 0.0;
        size_t diff = n - jmin (x.getSize(), y.getSize());
        for (size_t i = 0; i < jmin (x.getSize(), y.getSize()); ++i) diff += (size_t) (x[i] != y[i]);
        return (double) diff / (double) n;
    };
    const auto same = apart (part (first, "IComponent"), part (again, "IComponent"));
    const auto other = apart (part (first, "IComponent"), part (second, "IComponent"));
    std::cout << "    " << a.name << " twice: " << String (same * 100.0, 2) << "% apart; from " << b.name << ": " << String (other * 100.0, 2) << "%" << std::endl;
    check (same < 0.01 && same * 10.0 < other, a.name + " loaded twice is the same sound");
    /* Not closed: a Wine plugin closed in this process can hang (Super 8 did,
       now and then). StageRig runs each in a process of its own, ended when
       it will not close; the test ends its process at the finish instead. */
    (void) inst.release();
}

static String only;

static int run()
{
    const auto dir = File::getSpecialLocation (File::tempDirectory).getChildFile ("stagerig-presettest");
    dir.createDirectory();
    ControlMapLibrary lib (dir);
    lib.startNksSearch (ControlMapLibrary::defaultNksFolders());
    check (lib.waitForNks (300000), "the NKS search finishes");

    const auto vst3 = File::getSpecialLocation (File::userHomeDirectory).getChildFile (".vst3/yabridge");
    for (const char* b : { "Super 8.vst3", "SWAM/Trumpets/SWAM Trumpet.vst3", "BBC Symphony Orchestra (64 Bit).vst3" })
        if (only.isEmpty() || String (b).containsIgnoreCase (only))
        {
            tryPlugin (lib, vst3.getChildFile (b));
        }
    dir.deleteRecursively();
    std::cout << "\nPresetTest: " << (failures == 0 ? String ("all checks passed") : String (failures) + " check(s) failed") << std::endl;
    std::_Exit (failures == 0 ? 0 : 1);     // the plugins left open, and Wine with them
}

int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI gui;
    if (argc > 1) only = argv[1];     // one plugin, by part of its name
    int result = 1;
    // Plugins in Wine answer through the message loop: the checks run inside it.
    MessageManager::callAsync ([&] { result = run(); MessageManager::getInstance()->stopDispatchLoop(); });
    MessageManager::getInstance()->runDispatchLoop();
    return result;
}
