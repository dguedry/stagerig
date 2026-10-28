#include "SectionActions.h"
#include <memory>
#include <vector>

namespace perf
{

using namespace juce;

namespace
{
    const Colour bgPanel   { 0xff1b1d22 };
    const Colour bgRow     { 0xff23262d };
    const Colour accent    { 0xff4f9dff };
    const Colour textDim   { 0xff9aa0ab };

    class ActionsComponent : public Component
    {
    public:
        ActionsComponent (Engine& e, int song, int marker, std::function<void()> ch)
            : engine (e), songIndex (song), markerIndex (marker), changed (std::move (ch))
        {
            addAndMakeVisible (intro);
            intro.setColour (Label::textColourId, textDim);
            intro.setFont (FontOptions (12.5f));
            intro.setJustificationType (Justification::topLeft);
            intro.setText ("When the track enters this section -- at Play, at a jump, or by playing into it -- these happen, "
                           "in this order. A vamp going round does not fire them again. Try does one now.", dontSendNotification);
            addAndMakeVisible (addBtn);
            addBtn.onClick = [this] { showAddMenu(); };
            addAndMakeVisible (empty);
            empty.setColour (Label::textColourId, textDim);
            empty.setJustificationType (Justification::centred);
            empty.setText ("Nothing yet. + Add... puts a keyboard on a program, mutes a part, bypasses an effect, or cues a lighting desk.", dontSendNotification);
            rebuild();
        }

        void rebuild()
        {
            rows.clear();
            const auto* m = marker();
            if (m == nullptr) return;
            for (int i = 0; i < (int) m->actions.size(); ++i)
            {
                auto row = std::make_unique<Row> (*this, i, m->actions[(size_t) i]);
                addAndMakeVisible (row.get());
                rows.push_back (std::move (row));
            }
            empty.setVisible (rows.empty());
            resized();
            repaint();
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (12);
            intro.setBounds (r.removeFromTop (44));
            r.removeFromTop (6);
            for (auto& row : rows) { row->setBounds (r.removeFromTop (30)); r.removeFromTop (4); }
            if (rows.empty()) empty.setBounds (r.removeFromTop (60));
            r.removeFromTop (6);
            addBtn.setBounds (r.removeFromTop (28).removeFromLeft (120));
        }

        int preferredHeight() const { return 12 + 44 + 6 + (rows.empty() ? 60 : (int) rows.size() * 34) + 6 + 28 + 12; }

    private:
        struct Row : public Component
        {
            Row (ActionsComponent& o, int i, const ActionDef& a) : owner (o), index (i), def (a)
            {
                addAndMakeVisible (num);
                num.setText (String (i + 1) + ".", dontSendNotification);
                num.setColour (Label::textColourId, textDim);
                addAndMakeVisible (text);
                text.setText (owner.engine.describeAction (a), dontSendNotification);
                text.setMinimumHorizontalScale (0.8f);
                addAndMakeVisible (tryBtn);
                tryBtn.setTooltip ("Do this now, to hear or see it");
                tryBtn.onClick = [this] { owner.engine.performAction (owner.songIndex, def); };
                addAndMakeVisible (remove);
                remove.setTooltip ("Take this off the section");
                remove.onClick = [this] { owner.removeAction (index); };
            }
            void paint (Graphics& g) override { g.setColour (bgRow); g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f); }
            void resized() override
            {
                auto r = getLocalBounds().reduced (6, 2);
                num.setBounds (r.removeFromLeft (26));
                remove.setBounds (r.removeFromRight (28));  r.removeFromRight (6);
                tryBtn.setBounds (r.removeFromRight (50));  r.removeFromRight (8);
                text.setBounds (r);
            }
            ActionsComponent& owner;
            int index;
            ActionDef def;
            Label num, text;
            TextButton tryBtn { "Try" }, remove { "X" };
        };

        const MarkerDef* marker() const
        {
            const auto& songs = engine.getSongs();
            if (songIndex < 0 || songIndex >= (int) songs.size()) return nullptr;
            const auto& m = songs[(size_t) songIndex].markers;
            return markerIndex >= 0 && markerIndex < (int) m.size() ? &m[(size_t) markerIndex] : nullptr;
        }

        void store (std::vector<ActionDef> actions)
        {
            const auto& songs = engine.getSongs();
            if (songIndex < 0 || songIndex >= (int) songs.size()) return;
            auto markers = songs[(size_t) songIndex].markers;
            if (markerIndex < 0 || markerIndex >= (int) markers.size()) return;
            markers[(size_t) markerIndex].actions = std::move (actions);
            engine.setSongMarkers (songIndex, markers);
            if (changed) changed();
            rebuild();
        }

        void addAction (const ActionDef& a)
        {
            const auto* m = marker();
            if (m == nullptr) return;
            auto actions = m->actions;
            actions.push_back (a);
            store (std::move (actions));
        }

        void removeAction (int index)
        {
            const auto* m = marker();
            if (m == nullptr || index < 0 || index >= (int) m->actions.size()) return;
            auto actions = m->actions;
            actions.erase (actions.begin() + index);
            SafePointer<ActionsComponent> self (this);
            MessageManager::callAsync ([self, actions] { if (self != nullptr) self->store (actions); });
        }

        void showAddMenu()
        {
            const auto& setup = engine.getSetup();
            const auto& songs = engine.getSongs();
            if (songIndex < 0 || songIndex >= (int) songs.size()) return;
            const auto& song = songs[(size_t) songIndex];
            PopupMenu menu;
            std::vector<ActionDef> choices;        // id = 1 + index
            auto add = [&choices] (PopupMenu& m, const String& text, const ActionDef& a, bool ticked = false)
            {
                choices.push_back (a);
                m.addItem ((int) choices.size(), text, true, ticked);
            };

            // Programs: each keyboard, its named programs.
            PopupMenu programs;
            for (int i = 0; i < (int) setup.inputs.size(); ++i)
            {
                const auto& in = setup.inputs[(size_t) i];
                if (in.isTrack()) continue;
                PopupMenu progs;
                for (int p = 0; p < InputDef::numPrograms; ++p)
                {
                    const auto& pd = in.programs[(size_t) p];
                    if (pd.name.isEmpty() && pd.slots.empty() && pd.effects.empty()) continue;
                    ActionDef a; a.kind = ActionDef::Kind::program; a.input = i; a.program = p;
                    add (progs, String (p).paddedLeft ('0', 3) + "  " + (pd.name.isNotEmpty() ? pd.name : String ("(unnamed)")), a, p == in.currentProgram);
                }
                if (progs.getNumItems() == 0) progs.addItem (-1, "No programs yet", false);
                programs.addSubMenu (in.name, progs);
            }
            menu.addSubMenu ("Put a keyboard on a program", programs);

            // Parts of the MIDI files, by channel.
            {
                const int track = engine.getTrackInput();
                const int prog = track >= 0 ? song.programFor (track) : -1;
                PopupMenu mute, unmute;
                for (int ch = 1; ch <= 16; ++ch)
                {
                    String label = "Channel " + String (ch);
                    if (track >= 0 && prog >= 0)
                        if (const int sl = engine.partInstrument (track, prog, ch); sl >= 0)
                            label += "   " + setup.inputs[(size_t) track].programs[(size_t) prog].slots[(size_t) sl].displayName();
                    ActionDef a; a.kind = ActionDef::Kind::part; a.channel = ch;
                    a.on = false; add (mute, label, a);
                    a.on = true;  add (unmute, label, a);
                }
                menu.addSubMenu ("Mute a part of the MIDI files", mute, song.stems.size() > 0);
                menu.addSubMenu ("Unmute a part of the MIDI files", unmute, song.stems.size() > 0);
            }

            // Effects of each keyboard's current program.
            {
                PopupMenu bypass, enable;
                for (int i = 0; i < (int) setup.inputs.size(); ++i)
                {
                    const auto& in = setup.inputs[(size_t) i];
                    const auto& prog = in.programs[(size_t) in.currentProgram];
                    PopupMenu offs, ons;
                    auto chainItems = [&] (int chain, const String& where)
                    {
                        if (auto* c = prog.chainFor (chain))
                            for (int e = 0; e < (int) c->size(); ++e)
                            {
                                ActionDef a; a.kind = ActionDef::Kind::effect; a.input = i; a.program = in.currentProgram; a.chain = chain; a.effect = e;
                                const String label = (*c)[(size_t) e].displayName() + "   (" + where + ")";
                                a.on = false; add (offs, label, a);
                                a.on = true;  add (ons, label, a);
                            }
                    };
                    chainItems (-1, "whole program");
                    for (int sl = 0; sl < (int) prog.slots.size(); ++sl)
                    {
                        chainItems (sl, prog.slots[(size_t) sl].displayName());
                        chainItems (ProgramDef::midiChainOf (sl), "MIDI chain of " + prog.slots[(size_t) sl].displayName());
                    }
                    if (offs.getNumItems() == 0) continue;
                    const String title = in.name + "  (its program now: " + String (in.currentProgram).paddedLeft ('0', 3) + " " + prog.name + ")";
                    bypass.addSubMenu (title, offs);
                    enable.addSubMenu (title, ons);
                }
                menu.addSubMenu ("Bypass an effect", bypass, bypass.getNumItems() > 0);
                menu.addSubMenu ("Turn an effect on", enable, enable.getNumItems() > 0);
            }

            menu.addSeparator();
            menu.addItem (10000, "Send MIDI to a device (lights, a hardware synth)...");

            SafePointer<ActionsComponent> self (this);
            menu.showMenuAsync (PopupMenu::Options().withTargetComponent (addBtn), [self, choices] (int r)
            {
                if (self == nullptr || r <= 0) return;
                if (r == 10000) { self->askMidiAction(); return; }
                if (r <= (int) choices.size()) self->addAction (choices[(size_t) r - 1]);
            });
        }

        void askMidiAction()
        {
            const auto devices = engine.midiOutputDevices();
            auto* w = new AlertWindow ("Send MIDI to a device",
                                       "A program change recalls a scene on a lighting desk or a patch on a synth; "
                                       "a controller or a note is a cue button on most desks.", MessageBoxIconType::NoIcon);
            StringArray names;
            for (const auto& d : devices) names.add (d.name);
            if (names.isEmpty()) names.add ("(no MIDI devices found)");
            w->addComboBox ("device", names, "Device");
            w->addComboBox ("type", { "Program change", "Controller", "Note" }, "What to send");
            w->addTextEditor ("channel", "1", "Channel (1-16)");
            w->addTextEditor ("data1", "1", "Program (1-128), or controller / note number (0-127)");
            w->addTextEditor ("data2", "127", "Value or velocity (0-127)");
            w->addButton ("Add", 1, KeyPress (KeyPress::returnKey));
            w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
            SafePointer<ActionsComponent> self (this);
            w->enterModalState (true, ModalCallbackFunction::create ([self, w, devices] (int r)
            {
                std::unique_ptr<AlertWindow> owned (w);
                if (r == 0 || self == nullptr || devices.isEmpty()) return;
                ActionDef a;
                a.kind = ActionDef::Kind::midi;
                const int d = jlimit (0, devices.size() - 1, w->getComboBoxComponent ("device")->getSelectedItemIndex());
                a.portIdentifier = devices[d].identifier;
                a.portName = devices[d].name;
                const int t = w->getComboBoxComponent ("type")->getSelectedItemIndex();
                a.midiType = t == 1 ? ActionDef::MidiType::controller : t == 2 ? ActionDef::MidiType::noteOn : ActionDef::MidiType::programChange;
                a.channel = jlimit (1, 16, w->getTextEditorContents ("channel").getIntValue());
                const int d1 = w->getTextEditorContents ("data1").getIntValue();
                a.data1 = a.midiType == ActionDef::MidiType::programChange ? jlimit (0, 127, d1 - 1) : jlimit (0, 127, d1);
                a.data2 = jlimit (0, 127, w->getTextEditorContents ("data2").getIntValue());
                self->addAction (a);
            }));
        }

        Engine& engine;
        const int songIndex, markerIndex;
        std::function<void()> changed;
        Label intro, empty;
        TextButton addBtn { "+ Add..." };
        std::vector<std::unique_ptr<Row>> rows;
    };
}

void showSectionActions (Engine& engine, int songIndex, int markerIndex, std::function<void()> changed)
{
    const auto& songs = engine.getSongs();
    if (songIndex < 0 || songIndex >= (int) songs.size()) return;
    const auto& markers = songs[(size_t) songIndex].markers;
    if (markerIndex < 0 || markerIndex >= (int) markers.size()) return;

    auto* content = new ActionsComponent (engine, songIndex, markerIndex, std::move (changed));
    content->setSize (640, jmax (260, content->preferredHeight()));
    DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = "When the track enters " + (markers[(size_t) markerIndex].name.isNotEmpty() ? markers[(size_t) markerIndex].name : "this section");
    o.dialogBackgroundColour = bgPanel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

} // namespace perf
