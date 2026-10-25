#include "PatchDialog.h"

using namespace juce;

namespace perf
{

/** Patch recall for something on a MIDI port: bank select and program
    change, sent when the program is selected. -1 = not sent. */
void showPatchRecallDialog (const String& portName, int msb, int lsb, int patch, std::function<void (int, int, int)> done)
{
    auto* w = new AlertWindow ("Patch recall",
                               "Sent to " + portName + " when this StageRig program is selected, on the channel it "
                               "plays on. Leave a box empty to send nothing. Use the numbers your synth's display shows (sounds 1-128).",
                               MessageBoxIconType::NoIcon);
    auto txt = [] (int v, int add) { return v >= 0 ? String (v + add) : String(); };
    w->addTextEditor ("msb", txt (msb, 0), "Bank, coarse (MSB, CC 0): 0-127");
    w->addTextEditor ("lsb", txt (lsb, 0), "Bank, fine (LSB, CC 32): 0-127");
    w->addTextEditor ("prog", txt (patch, 1), "Sound number on the synth (program change): 1-128");
    w->addButton ("OK", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([w, done] (int r)
    {
        if (r != 1) return;
        auto num = [w] (const char* id, int lo, int hi, int sub) {
            const auto t = w->getTextEditorContents (id).trim();
            return t.isEmpty() ? -1 : jlimit (lo, hi, t.getIntValue()) - sub; };
        done (num ("msb", 0, 127, 0), num ("lsb", 0, 127, 0), num ("prog", 1, 128, 1));
    }), true);
}

} // namespace perf
