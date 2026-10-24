#include "GmMixer.h"
#include "GmSynth.h"

using namespace juce;

namespace perf
{

namespace
{
    const Colour bgPanel { 0xff1b1d22 };
    const Colour bgRow   { 0xff23262d };
    const Colour textDim { 0xff9aa0ab };

    const char* const familyNames[16] = { "Piano", "Chromatic percussion", "Organ", "Guitar", "Bass", "Strings", "Ensemble", "Brass",
                                          "Reed", "Pipe", "Synth lead", "Synth pad", "Synth effects", "Ethnic", "Percussive", "Sound effects" };
    struct Kit { int program; const char* name; };
    const Kit kits[] = { { 0, "Standard kit" }, { 8, "Room kit" }, { 16, "Power kit" }, { 24, "Electronic kit" }, { 25, "TR-808 kit" },
                         { 32, "Jazz kit" }, { 40, "Brush kit" }, { 48, "Orchestra kit" }, { 56, "Sound FX kit" } };
}

String gmKitName (int p)
{
    for (auto& k : kits) if (k.program == p) return k.name;
    return "Kit " + String (p + 1);
}

void addGmInstrumentsToMenu (PopupMenu& menu, int idBase, int selected)
{
    for (int f = 0; f < 16; ++f)
    {
        PopupMenu fam;
        for (int p = f * 8; p < f * 8 + 8; ++p)
            fam.addItem (idBase + p, String (p + 1) + "  " + GmSynth::programName (p), true, p == selected);
        menu.addSubMenu (familyNames[f], fam, true, nullptr, selected >= f * 8 && selected < f * 8 + 8);
    }
}

void addGmKitsToMenu (PopupMenu& menu, int idBase, int selected)
{
    for (auto& k : kits) menu.addItem (idBase + k.program, k.name, true, k.program == selected);
}

String partInstrumentName (const Engine& engine, int track, int program, int channel, int fileProgram)
{
    const auto& inputs = engine.getSetup().inputs;
    if (track < 0 || track >= (int) inputs.size() || program < 0) return "Nothing";
    const auto& prog = inputs[(size_t) track].programs[(size_t) program];
    for (const auto& sd : prog.slots)
    {
        if (! sd.hears (channel)) continue;
        if (sd.isGm())
        {
            const int p = sd.gmChannels[(size_t) channel - 1].program;
            const int shown = p >= 0 ? p : fileProgram;
            return (channel == 10 ? gmKitName (shown) : String (GmSynth::programName (shown))) + "   \xc2\xb7  General MIDI";
        }
        return sd.displayName();
    }
    return "Nothing";
}

} // namespace perf
