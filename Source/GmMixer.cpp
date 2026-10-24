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

namespace
{
    class GmMixerComponent : public Component,
                             private Engine::Listener
    {
    public:
        GmMixerComponent (Engine& e, int in, int prog, int sl) : engine (e), input (in), program (prog), slot (sl)
        {
            engine.addListener (this);
            addAndMakeVisible (intro);
            intro.setColour (Label::textColourId, textDim);
            intro.setJustificationType (Justification::topLeft);
            intro.setText ("A MIDI file chooses the instruments with its own program changes; a channel set here keeps "
                           "what you choose instead. The player only hears the channels its row under INSTRUMENTS listens to.",
                           dontSendNotification);
            for (int ch = 1; ch <= 16; ++ch)
            {
                auto row = std::make_unique<Row>();
                row->channel = ch;
                row->label.setText ("Ch " + String (ch), dontSendNotification);
                row->label.setFont (FontOptions (13.0f, Font::bold));
                row->instrument.onClick = [this, r = row.get()] { chooseInstrument (*r); };
                auto slider = [] (Slider& s, double def, std::function<String (double)> text)
                {
                    s.setSliderStyle (Slider::LinearBar);
                    s.setRange (-1.0, 127.0, 1.0);
                    s.setDoubleClickReturnValue (true, def);
                    s.textFromValueFunction = std::move (text);
                    s.updateText();
                };
                slider (row->volume, -1.0, [] (double v) { return v < 0 ? String ("vol: file") : "vol " + String ((int) v); });
                slider (row->pan, -1.0, [] (double v) { if (v < 0) return String ("pan: file"); const int p = (int) v - 64; return p == 0 ? String ("centre") : (p < 0 ? "L " : "R ") + String (std::abs (p)); });
                row->volume.setTooltip ("Volume (CC 7) for this channel. Double-click: as the file has it");
                row->pan.setTooltip ("Pan (CC 10) for this channel. Double-click: as the file has it");
                row->volume.onValueChange = [this, r = row.get()] { auto c = current (r->channel); c.volume = (int) r->volume.getValue(); engine.setGmChannel (input, program, slot, r->channel, c); };
                row->pan.onValueChange    = [this, r = row.get()] { auto c = current (r->channel); c.pan = (int) r->pan.getValue(); engine.setGmChannel (input, program, slot, r->channel, c); };
                row->mute.setClickingTogglesState (true);
                row->mute.setColour (TextButton::buttonOnColourId, Colours::orangered);
                row->mute.onClick = [this, r = row.get()] { auto c = current (r->channel); c.muted = r->mute.getToggleState(); engine.setGmChannel (input, program, slot, r->channel, c); };
                for (auto* c : std::initializer_list<Component*> { &row->label, &row->instrument, &row->volume, &row->pan, &row->mute })
                    addAndMakeVisible (c);
                rows.push_back (std::move (row));
            }
            refresh();
            setSize (640, 70 + 16 * 30 + 12);
        }

        ~GmMixerComponent() override { engine.removeListener (this); }

        void paint (Graphics& g) override
        {
            g.fillAll (bgPanel);
            for (size_t i = 0; i < rows.size(); ++i)
                if (i % 2 == 0) g.setColour (bgRow), g.fillRect (rows[i]->label.getBounds().withX (8).withWidth (getWidth() - 16).expanded (0, 2));
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (12);
            intro.setBounds (r.removeFromTop (52));
            r.removeFromTop (6);
            for (auto& row : rows)
            {
                auto line = r.removeFromTop (30).reduced (0, 3);
                row->label.setBounds (line.removeFromLeft (48));
                row->mute.setBounds (line.removeFromRight (30)); line.removeFromRight (6);
                row->pan.setBounds (line.removeFromRight (90));  line.removeFromRight (6);
                row->volume.setBounds (line.removeFromRight (90)); line.removeFromRight (8);
                row->instrument.setBounds (line);
            }
        }

    private:
        struct Row
        {
            int channel = 1;
            Label label;
            TextButton instrument, mute { "M" };
            Slider volume, pan;
        };

        const SlotDef* def() const
        {
            const auto& inputs = engine.getSetup().inputs;
            if (input < 0 || input >= (int) inputs.size()) return nullptr;
            const auto& slots = inputs[(size_t) input].programs[(size_t) program].slots;
            return slot >= 0 && slot < (int) slots.size() && slots[(size_t) slot].isGm() ? &slots[(size_t) slot] : nullptr;
        }

        SlotDef::GmChannel current (int ch) const
        {
            if (auto* d = def()) return d->gmChannels[(size_t) ch - 1];
            return {};
        }

        void refresh()
        {
            auto* d = def();
            for (auto& row : rows)
            {
                const auto c = d != nullptr ? d->gmChannels[(size_t) row->channel - 1] : SlotDef::GmChannel();
                const bool heard = d == nullptr || d->hears (row->channel);
                row->instrument.setButtonText (c.program < 0 ? "As the file has it"
                                               : row->channel == 10 ? gmKitName (c.program) : String (GmSynth::programName (c.program)));
                row->instrument.setEnabled (heard);
                row->label.setColour (Label::textColourId, heard ? Colours::white : textDim);
                row->volume.setValue (c.volume, dontSendNotification);
                row->pan.setValue (c.pan, dontSendNotification);
                row->mute.setToggleState (c.muted, dontSendNotification);
                row->instrument.setTooltip (heard ? "Which instrument plays this channel" : "Another instrument plays this channel (see LISTENS TO)");
            }
        }

        void chooseInstrument (Row& row)
        {
            const auto c = current (row.channel);
            PopupMenu m;
            m.addItem (1, "As the file has it", true, c.program < 0);
            m.addSeparator();
            if (row.channel == 10)
            {
                addGmKitsToMenu (m, 1000, c.program);
                PopupMenu instruments;
                addGmInstrumentsToMenu (instruments, 1000, -1);
                m.addSubMenu ("Instruments (not drums)", instruments);
            }
            else
                addGmInstrumentsToMenu (m, 1000, c.program);
            m.showMenuAsync (PopupMenu::Options().withTargetComponent (row.instrument), [this, ch = row.channel] (int r)
            {
                if (r < 1) return;
                auto c = current (ch);
                c.program = r == 1 ? -1 : r - 1000;
                engine.setGmChannel (input, program, slot, ch, c);
                refresh();
            });
        }

        void programContentChanged (int in, int prog) override { if (in == input && prog == program) refresh(); }
        void setupChanged() override { refresh(); }

        Engine& engine;
        const int input, program, slot;
        Label intro;
        std::vector<std::unique_ptr<Row>> rows;
    };
}

void showGmMixer (Engine& engine, int inputIndex, int program, int slot)
{
    const auto& inputs = engine.getSetup().inputs;
    if (inputIndex < 0 || inputIndex >= (int) inputs.size()) return;
    const auto& prog = inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slot < 0 || slot >= (int) prog.slots.size() || ! prog.slots[(size_t) slot].isGm()) return;

    auto* content = new GmMixerComponent (engine, inputIndex, program, slot);
    DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = "General MIDI player: " + inputs[(size_t) inputIndex].name + " / " + String (program).paddedLeft ('0', 3)
                    + (prog.name.isNotEmpty() ? " " + prog.name : String());
    o.dialogBackgroundColour = bgPanel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

} // namespace perf
