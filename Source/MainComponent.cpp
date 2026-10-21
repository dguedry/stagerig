#include "MainComponent.h"
#include "LevelMeter.h"
#include "PluginView.h"
#include "QrCode.h"
#include "MappingSuggestions.h"
#include "PluginManagerComponent.h"
#include "AudioSettingsComponent.h"
#include "ProgramMap.h"
#include "ParamPicker.h"
#include <optional>

using namespace juce;

namespace perf
{

namespace
{
    const Colour bgDark    { 0xff1e1f24 };
    const Colour bgPanel   { 0xff26282f };
    const Colour bgRow     { 0xff2e313a };
    const Colour accent    { 0xff4f9dff };
    const Colour accentDim { 0xff2b4f80 };
    const Colour textDim   { 0xff9aa0ab };

    String programTitle (int index, const ProgramDef& p)
    {
        String s = String (index).paddedLeft ('0', 3);
        if (p.name.isNotEmpty()) s << "  " << p.name;
        return s;
    }

    String noteName (int n) { return MidiMessage::getMidiNoteName (n, true, true, 3); }

    String araWarning (const String& name)
    {
        return name + " looks like an ARA-only plugin. ARA plugins need an ARA host (Reaper, Logic...); here the editor may be empty and can freeze StageRig. Prefer the non-ARA version.";
    }

    void styleHeader (Label& l)
    {
        l.setFont (FontOptions (14.0f, Font::bold));
        l.setColour (Label::textColourId, Colours::white);
    }
}

//==============================================================================
//  InputsPanel
//==============================================================================
class InputsPanel : public Component,
                    private ListBoxModel,
                    private Timer
{
public:
    InputsPanel (Engine& e, MainComponent& o) : engine (e), owner (o)
    {
        startTimerHz (30);      // audio inputs show their level in the list
        styleHeader (header);
        addAndMakeVisible (header);
        addAndMakeVisible (list);
        list.setModel (this);
        list.setRowHeight (44);
        list.setColour (ListBox::backgroundColourId, bgPanel);

        addAndMakeVisible (addBtn);
        addAndMakeVisible (removeBtn);
        addBtn.onClick = [this]
        {
            PopupMenu m;
            m.addItem (1, "Keyboard (MIDI)");
            m.addItem (2, "Audio input (microphone, guitar, hardware synth)");
            m.showMenuAsync (PopupMenu::Options().withTargetComponent (&addBtn), [this] (int r)
            {
                if (r == 0) return;
                const bool audio = r == 2;
                const auto n = String (engine.getSetup().inputs.size() + 1);
                auto idx = engine.addInput (audio ? "Audio " + n : "Input " + n,
                                            audio ? InputDef::Kind::audio : InputDef::Kind::midi);
                lastNotified = -1; setSelected (idx);
                if (audio && engine.getAudioInputChannelNames().isEmpty())
                    owner.showStatus ("No audio input is open. Open Audio... and choose an input device and channels.");
            });
        };
        removeBtn.onClick = [this]
        {
            int sel = list.getSelectedRow();
            if (sel >= 0) { engine.removeInput (sel); lastNotified = -1; setSelected (jmax (0, sel - 1)); }
        };

        addAndMakeVisible (nameEditor);
        nameEditor.setTextToShowWhenEmpty ("Name", textDim);
        nameEditor.onTextChange = [this]
        {
            int sel = list.getSelectedRow();
            if (sel >= 0) { engine.setInputName (sel, nameEditor.getText()); list.repaintRow (sel); }
        };

        addAndMakeVisible (deviceBox);
        deviceBox.onChange = [this]
        {
            int sel = list.getSelectedRow();
            if (sel < 0) return;
            int id = deviceBox.getSelectedId();
            MidiDeviceInfo info;
            if (id >= 2 && id - 2 < devices.size()) info = devices[id - 2];
            engine.setInputMidiDevice (sel, info);
            list.repaintRow (sel);
        };

        addAndMakeVisible (pcChannelBox);
        // Ids: 1 = same as notes, 2 = any, 3.. = channel 1..16
        pcChannelBox.addItem ("Same as notes", 1);
        pcChannelBox.addItem ("Any channel", 2);
        for (int c = 1; c <= 16; ++c) pcChannelBox.addItem ("Ch " + String (c), c + 2);
        pcChannelBox.setTooltip ("Which channel this input's Program Change messages arrive on. Most keyboards use the channel they play on (Same as notes). "
                                 "Some workstations send them on a fixed channel instead: a Roland Jupiter-50 sends registrations on channel 16 while playing on 1, 3 and 4. "
                                 "Any channel accepts them from anywhere on this MIDI port, which is the setting to try if program changes are not getting through.");
        pcChannelBox.onChange = [this]
        {
            int sel = list.getSelectedRow();
            if (sel < 0) return;
            const int id = pcChannelBox.getSelectedId();
            engine.setInputProgramChangeChannel (sel, id == 1 ? InputDef::pcChannelSameAsNotes
                                                             : (id == 2 ? InputDef::pcChannelAny : id - 2));
            owner.markDirty();
        };

        addAndMakeVisible (channelBox);
        channelBox.addItem ("Omni", 1);
        for (int c = 1; c <= 16; ++c) channelBox.addItem ("Ch " + String (c), c + 1);
        channelBox.onChange = [this]
        {
            int sel = list.getSelectedRow();
            if (sel >= 0) { engine.setInputChannel (sel, channelBox.getSelectedId() - 1); list.repaintRow (sel); }
        };

        addAndMakeVisible (pcToggle);
        pcToggle.onClick = [this]
        {
            int sel = list.getSelectedRow();
            if (sel >= 0) engine.setInputRespondToProgramChange (sel, pcToggle.getToggleState());
            pcChannelBox.setEnabled (pcToggle.getToggleState());
        };

        addAndMakeVisible (outputBox);
        outputBox.setTooltip ("Which output this input lands on. Outputs are device channel pairs with a master chain, set up under Options.");
        outputBox.onChange = [this]
        {
            int sel = list.getSelectedRow();
            if (sel >= 0 && outputBox.getSelectedId() > 0) { engine.setInputOutput (sel, outputBox.getSelectedId() - 1); owner.markDirty(); }
        };

        addAndMakeVisible (audioBox);
        audioBox.setTooltip ("Which of the open device inputs this input listens to: one channel (mono, fed to both sides) "
                             "or a pair. The channels on offer are those chosen in Audio...");
        audioBox.onChange = [this]
        {
            int sel = list.getSelectedRow();
            const int id = audioBox.getSelectedId();
            if (sel < 0 || id <= 0) return;
            if (id >= 2000) engine.setInputAudioChannels (sel, id - 2000, id - 2000 + 1);
            else            engine.setInputAudioChannels (sel, id - 1000, -1);
            owner.markDirty();
            list.repaintRow (sel);
        };

        for (auto* l : { &deviceLabel, &channelLabel, &pcChannelLabel, &audioLabel, &outputLabel })
        {
            addAndMakeVisible (l);
            l->setColour (Label::textColourId, textDim);
            l->setFont (FontOptions (12.0f));
        }
    }

    void setSelected (int idx)
    {
        list.selectRow (idx, false, true);
        refreshEditor();
    }

    void refresh()
    {
        devices = MidiInput::getAvailableDevices();
        list.updateContent();
        list.repaint();
        refreshEditor();
    }

    void refreshEditor()
    {
        int sel = list.getSelectedRow();
        auto& inputs = engine.getSetup().inputs;
        const bool valid = sel >= 0 && sel < (int) inputs.size();
        for (auto* c : std::initializer_list<Component*> { &nameEditor, &deviceBox, &channelBox, &pcToggle, &removeBtn })
            c->setEnabled (valid);
        if (! valid) return;

        auto& def = inputs[(size_t) sel];
        nameEditor.setText (def.name, dontSendNotification);

        outputBox.clear (dontSendNotification);
        for (int o = 0; o < (int) engine.getSetup().outputs.size(); ++o)
        {
            const auto& od = engine.getSetup().outputs[(size_t) o];
            outputBox.addItem (od.name + "  (" + String (od.left + 1) + (od.right >= 0 ? "+" + String (od.right + 1) : String()) + ")", o + 1);
        }
        outputBox.setSelectedId (def.output + 1, dontSendNotification);

        // The audio channel row only means something for an audio input.
        audioLabel.setVisible (def.isAudio());
        audioBox.setVisible (def.isAudio());
        deviceLabel.setText (def.isAudio() ? "Control MIDI device (program changes, pedals)" : "MIDI device", dontSendNotification);
        if (def.isAudio())
        {
            const auto names = engine.getAudioInputChannelNames();
            audioBox.clear (dontSendNotification);
            if (names.isEmpty())
                audioBox.addItem ("(no audio input open: see Audio...)", 1);
            for (int c = 0; c < names.size(); ++c)
                audioBox.addItem (String (c + 1) + ": " + names[c], 1000 + c);
            for (int c = 0; c + 1 < names.size(); ++c)
                audioBox.addItem (String (c + 1) + "+" + String (c + 2) + ": " + names[c] + " / " + names[c + 1], 2000 + c);
            const int want = def.audioRight >= 0 ? 2000 + def.audioLeft : 1000 + def.audioLeft;
            if (audioBox.indexOfItemId (want) >= 0) audioBox.setSelectedId (want, dontSendNotification);
            else audioBox.setText (def.audioRight >= 0 ? String (def.audioLeft + 1) + "+" + String (def.audioRight + 1) + " (not open)"
                                                      : String (def.audioLeft + 1) + " (not open)", dontSendNotification);
        }

        deviceBox.clear (dontSendNotification);
        deviceBox.addItem ("(no MIDI device)", 1);
        int selectedId = 1;
        for (int i = 0; i < devices.size(); ++i)
        {
            deviceBox.addItem (devices[i].name, i + 2);
            if (devices[i].identifier == def.midiDeviceIdentifier) selectedId = i + 2;
        }
        if (selectedId == 1 && def.midiDeviceName.isNotEmpty())
        {
            deviceBox.addItem (def.midiDeviceName + " (disconnected)", 1000);
            selectedId = 1000;
        }
        deviceBox.setSelectedId (selectedId, dontSendNotification);
        channelBox.setSelectedId (def.channel + 1, dontSendNotification);
        pcToggle.setToggleState (def.respondToProgramChange, dontSendNotification);
        pcChannelBox.setSelectedId (def.programChangeChannel == InputDef::pcChannelSameAsNotes ? 1
                                    : (def.programChangeChannel == InputDef::pcChannelAny ? 2 : def.programChangeChannel + 2),
                                    dontSendNotification);
        pcChannelBox.setEnabled (def.respondToProgramChange);
        resized();      // the editor is taller for an audio input
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        header.setBounds (r.removeFromTop (22));
        /* Tall enough for what is actually in it: name, device, channel, the
           Program Change toggle and its channel -- 164px of rows and padding.
           It was 150, so the last row was being clipped, which is why the PC
           channel selector looked squeezed. */
        const bool audio = audioBox.isVisible();
        auto editor = r.removeFromBottom (audio ? 262 : 216);
        auto buttons = r.removeFromBottom (26);
        r.removeFromBottom (4);
        list.setBounds (r);

        addBtn.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (2, 0));
        removeBtn.setBounds (buttons.reduced (2, 0));

        editor.removeFromTop (6);
        nameEditor.setBounds (editor.removeFromTop (24));
        editor.removeFromTop (6);
        if (audio)
        {
            audioLabel.setBounds (editor.removeFromTop (16));
            audioBox.setBounds (editor.removeFromTop (24));
            editor.removeFromTop (6);
        }
        deviceLabel.setBounds (editor.removeFromTop (16));
        deviceBox.setBounds (editor.removeFromTop (24));
        editor.removeFromTop (6);
        auto chRow = editor.removeFromTop (24);
        channelLabel.setBounds (chRow.removeFromLeft (56));
        channelBox.setBounds (chRow.removeFromLeft (90));
        editor.removeFromTop (6);
        pcToggle.setBounds (editor.removeFromTop (22));
        editor.removeFromTop (6);
        /* The same height and label column as the Channel row above: this is
           the same kind of control, and it was the only one a size smaller. */
        auto pcRow = editor.removeFromTop (24);
        pcChannelLabel.setBounds (pcRow.removeFromLeft (56));
        pcChannelBox.setBounds (pcRow.removeFromLeft (130));
        editor.removeFromTop (6);
        auto outRow = editor.removeFromTop (24);
        outputLabel.setBounds (outRow.removeFromLeft (56));
        outputBox.setBounds (outRow);
    }

    // ListBoxModel
    int getNumRows() override { return (int) engine.getSetup().inputs.size(); }

    void paintListBoxItem (int row, Graphics& g, int w, int, bool selected) override
    {
        auto& inputs = engine.getSetup().inputs;
        if (row < 0 || row >= (int) inputs.size()) return;
        auto& def = inputs[(size_t) row];

        g.fillAll (selected ? accentDim : bgRow);
        g.setColour (Colours::white);
        g.setFont (FontOptions (15.0f, Font::bold));
        g.drawText (def.name, 8, 4, w - 16, 18, Justification::centredLeft, true);

        g.setFont (FontOptions (12.0f));
        g.setColour (textDim);
        String info;
        if (def.isAudio())
        {
            info << "audio in " << (def.audioLeft + 1);
            if (def.audioRight >= 0) info << "+" << (def.audioRight + 1);
            if (def.midiDeviceName.isNotEmpty()) info << "  \xc2\xb7  " << def.midiDeviceName;
        }
        else
            info = def.midiDeviceName.isNotEmpty() ? def.midiDeviceName : "no device";
        if (def.channel > 0) info << "  ch " << def.channel;
        g.drawText (info, 8, 22, w - 80, 16, Justification::centredLeft, true);

        g.setColour (accent);
        g.setFont (FontOptions (13.0f, Font::bold));
        g.drawText ("PC " + String (def.currentProgram).paddedLeft ('0', 3), w - 72, 22, 64, 16, Justification::centredRight, true);

        if (def.isAudio())
            LevelMeterPainter::paint (g, { w - 104, 6, 96, 12 }, engine.getInputLevel (row));
    }

    void selectedRowsChanged (int row) override { owner_select (row); }

private:
    void timerCallback() override
    {
        // Repaint only the audio rows, and only when their meter would look different.
        const auto& inputs = engine.getSetup().inputs;
        for (int i = 0; i < (int) inputs.size(); ++i)
        {
            if (! inputs[(size_t) i].isAudio()) continue;
            const auto l = engine.getInputLevel (i);
            const int key = (int) (LevelMeterPainter::position (l.left) * 100) * 1000
                          + (int) (LevelMeterPainter::position (l.right) * 100) * 2 + (l.clipped ? 1 : 0);
            if (meterKeys.size() <= (size_t) i) meterKeys.resize ((size_t) i + 1, -1);
            if (meterKeys[(size_t) i] != key) { meterKeys[(size_t) i] = key; list.repaintRow (i); }
        }
    }
    std::vector<int> meterKeys;

    void owner_select (int row)
    {
        if (row < 0 || row == lastNotified) return;
        lastNotified = row;
        owner.selectInput (row);
        refreshEditor();
    }

    Engine& engine;
    MainComponent& owner;
    Label header { {}, "INPUTS" };
    ListBox list;
    TextButton addBtn { "+ Add" }, removeBtn { "- Remove" };
    TextEditor nameEditor;
    Label deviceLabel { {}, "MIDI device" }, channelLabel { {}, "Channel" };
    ComboBox deviceBox, channelBox;
    ToggleButton pcToggle { "Respond to Program Change" };
    Label pcChannelLabel { {}, "PC on" };
    ComboBox pcChannelBox;
    Label audioLabel { {}, "Audio channels" };
    ComboBox audioBox;
    Label outputLabel { {}, "Output" };
    ComboBox outputBox;
    Array<MidiDeviceInfo> devices;
    int lastNotified = -1;
};

//==============================================================================
//  ProgramsPanel
//==============================================================================
class ProgramsPanel : public Component,
                      private ListBoxModel
{
public:
    ProgramsPanel (Engine& e, MainComponent& o) : engine (e), owner (o)
    {
        styleHeader (header);
        addAndMakeVisible (header);
        addAndMakeVisible (list);
        list.setModel (this);
        list.setRowHeight (24);
        list.setColour (ListBox::backgroundColourId, bgPanel);

        addAndMakeVisible (nameEditor);
        nameEditor.setTextToShowWhenEmpty ("Program name", textDim);
        nameEditor.onTextChange = [this]
        {
            engine.setProgramName (owner.getSelectedInput(), owner.getEditedProgram(), nameEditor.getText());
            list.repaintRow (owner.getEditedProgram());
        };

        /* Groups are a label, never a container: a program's number is fixed by
           MIDI Program Change, so a group cannot move or renumber anything. It
           exists to make 128 rows findable. */
        addAndMakeVisible (groupEditor);
        groupEditor.setTextToShowWhenEmpty ("Group", textDim);
        groupEditor.setTooltip ("Optional category -- Organs, Strings, Brass. Used to group this list "
                                "and the tablet app; it never changes a program's number.");
        groupEditor.onTextChange = [this]
        {
            engine.setProgramGroup (owner.getSelectedInput(), owner.getEditedProgram(), groupEditor.getText());
            list.repaint();
        };
        // Offer the groups already in use, so "Organ", "organs" and "Organ " do
        // not become three different groups through typing alone.
        groupEditor.onFocusLost = [this] { groupEditor.setText (groupEditor.getText().trim(), false); };
        addAndMakeVisible (groupBtn);
        groupBtn.setTooltip ("Pick a group already used on this input");
        groupBtn.onClick = [this]
        {
            PopupMenu m;
            const auto used = engine.getProgramGroups (owner.getSelectedInput());
            const auto current = groupEditor.getText().trim();
            m.addItem (1, "(no group)", true, current.isEmpty());
            int id = 2;
            for (const auto& gname : used) m.addItem (id++, gname, true, gname == current);
            if (used.isEmpty()) m.addItem (-1, "No groups yet -- type one", false);

            m.showMenuAsync (PopupMenu::Options().withTargetComponent (groupBtn),
                             [this, used] (int choice)
            {
                if (choice <= 0) return;
                const auto text = choice == 1 ? String() : used[choice - 2];
                groupEditor.setText (text, true);
            });
        };

        addAndMakeVisible (copyBtn);
        addAndMakeVisible (pasteBtn);
        addAndMakeVisible (clearBtn);
        copyBtn.onClick  = [this] { clipboard = owner.getEditedProgram(); pasteBtn.setEnabled (true); owner.showStatus ("Copied program " + String (clipboard)); };
        pasteBtn.onClick = [this] { if (clipboard >= 0) engine.copyProgram (owner.getSelectedInput(), clipboard, owner.getEditedProgram()); };
        clearBtn.onClick = [this] { engine.clearProgram (owner.getSelectedInput(), owner.getEditedProgram()); };
        pasteBtn.setEnabled (false);
    }

    void refresh()
    {
        const auto& inputs = engine.getSetup().inputs;
        const int in = owner.getSelectedInput();
        const bool valid = in >= 0 && in < (int) inputs.size();
        list.setEnabled (valid);
        nameEditor.setEnabled (valid);
        if (valid)
        {
            const int p = inputs[(size_t) in].currentProgram;
            list.selectRow (p, false, true);
            nameEditor.setText (inputs[(size_t) in].programs[(size_t) p].name, dontSendNotification);
            groupEditor.setText (inputs[(size_t) in].programs[(size_t) p].group, dontSendNotification);
        }
        groupEditor.setEnabled (valid);
        groupBtn.setEnabled (valid);
        list.updateContent();
        list.repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        header.setBounds (r.removeFromTop (22));
        auto bottom = r.removeFromBottom (26);
        r.removeFromBottom (4);
        auto groupRow = r.removeFromBottom (24);
        groupBtn.setBounds (groupRow.removeFromRight (28));
        groupRow.removeFromRight (4);
        groupEditor.setBounds (groupRow);
        r.removeFromBottom (4);
        nameEditor.setBounds (r.removeFromBottom (24));
        r.removeFromBottom (4);
        list.setBounds (r);
        const int w = bottom.getWidth() / 3;
        copyBtn.setBounds (bottom.removeFromLeft (w).reduced (2, 0));
        pasteBtn.setBounds (bottom.removeFromLeft (w).reduced (2, 0));
        clearBtn.setBounds (bottom.reduced (2, 0));
    }

    int getNumRows() override { return InputDef::numPrograms; }

    void paintListBoxItem (int row, Graphics& g, int w, int h, bool selected) override
    {
        const auto& inputs = engine.getSetup().inputs;
        const int in = owner.getSelectedInput();
        if (in < 0 || in >= (int) inputs.size()) return;
        const auto& prog = inputs[(size_t) in].programs[(size_t) row];
        /* An audio input's program is its effects; one with none but a name is
           a real program all the same -- the signal passing straight through. */
        const bool audio = inputs[(size_t) in].isAudio();
        const bool blank = audio ? prog.effects.empty() && prog.name.isEmpty() : prog.isEmpty();

        g.fillAll (selected ? accentDim : (row % 2 ? bgRow : bgPanel));
        g.setColour (blank ? textDim : Colours::white);
        g.setFont (FontOptions (13.0f, blank ? Font::plain : Font::bold));
        g.drawText (programTitle (row, prog), 8, 0, w - 60, h, Justification::centredLeft, true);

        if (! blank)
        {
            g.setColour (engine.isProgramLoaded (in, row) ? Colours::limegreen : textDim);
            if (engine.isProgramLoaded (in, row))
            {
                for (int sIdx = 0; sIdx < (int) prog.slots.size(); ++sIdx)
                    if (engine.isPluginLoading (in, row, sIdx)) { g.setColour (Colours::orange); break; }
                for (int e = 0; e < (int) prog.effects.size(); ++e)
                    if (engine.isPluginLoading (in, row, -1, e)) { g.setColour (Colours::orange); break; }
            }
            g.setFont (FontOptions (11.0f));
            const auto count = audio ? (prog.effects.empty() ? String ("straight through")
                                                             : String (prog.effects.size()) + (prog.effects.size() == 1 ? " effect" : " effects"))
                                     : String (prog.slots.size()) + (prog.slots.size() == 1 ? " plugin" : " plugins");
            g.drawText (count, w - 100, 0, 94, h, Justification::centredRight, true);
        }

        // The group, dimmed and to the right of the name: visible when scanning
        // the list, never competing with the program's own name.
        // Dropped when the pane is squeezed: the name is what the row is for.
        if (prog.group.isNotEmpty() && w >= 240)
        {
            g.setColour (Colour (0xff7f8896));
            g.setFont (FontOptions (10.5f));
            g.drawText (prog.group.toUpperCase(), 8, 0, w - 80, h, Justification::centredRight, true);
        }
    }

    void listBoxItemClicked (int row, const MouseEvent&) override
    {
        engine.selectProgram (owner.getSelectedInput(), row);
    }

    void returnKeyPressed (int row) override { engine.selectProgram (owner.getSelectedInput(), row); }

private:
    Engine& engine;
    MainComponent& owner;
    Label header { {}, "PROGRAMS" };
    ListBox list;
    TextEditor nameEditor, groupEditor;
    TextButton groupBtn { "v" };
    TextButton copyBtn { "Copy" }, pasteBtn { "Paste" }, clearBtn { "Clear" };
    int clipboard = -1;
};

//==============================================================================
//  EffectChainComponent -- an ordered list of insert effects (per slot or per program)
//==============================================================================
/** "Add instrument/effect" menu: grouped by manufacturer like KnownPluginList::addToMenu,
    but every entry carries the plugin's icon. Item ids are index + 1 into `types`. */
static void addPluginsToMenu (PopupMenu& menu, const Array<PluginDescription>& types, PluginIcons& icons)
{
    StringArray makers;
    for (auto& d : types) makers.addIfNotAlreadyThere (d.manufacturerName.isNotEmpty() ? d.manufacturerName : "Other");
    makers.sortNatural();
    auto addItems = [&] (PopupMenu& target, const String& maker)
    {
        Array<int> order;
        for (int i = 0; i < types.size(); ++i)
            if ((types[i].manufacturerName.isNotEmpty() ? types[i].manufacturerName : "Other") == maker) order.add (i);
        std::sort (order.begin(), order.end(), [&] (int a, int b) { return types[a].name.compareNatural (types[b].name) < 0; });
        for (int i : order)
            target.addItem (i + 1, types[i].name + "  [" + types[i].pluginFormatName + "]", true, false, icons.get (types[i], 32));
    };
    if (makers.size() == 1) { addItems (menu, makers[0]); return; }
    for (auto& m : makers)
    {
        PopupMenu sub;
        addItems (sub, m);
        menu.addSubMenu (m, sub);
    }
}

class EffectChainComponent : public Component
{
public:
    static constexpr int rowHeight = 26;

    EffectChainComponent (Engine& e, MainComponent& o, int slotIndex) : engine (e), owner (o), slot (slotIndex)
    {
        address = [this] { return std::make_pair (owner.getSelectedInput(), owner.getEditedProgram()); };
        addAndMakeVisible (addBtn);
        addBtn.onClick = [this] { showAddMenu(); };
        addBtn.setTooltip (slot < 0 ? "Add an effect after the mix of all slots" : "Add an insert effect after this instrument");
    }

    /** Whose chain this is: (input, program), or an output's master chain
        through Engine::outputAddress(). The program chains follow the
        selection; the Outputs window sets its own. */
    std::function<std::pair<int, int>()> address;

    void setSlot (int s) { slot = s; }

    void rebuild (const std::vector<EffectDef>& effects, int inputIndex, int program)
    {
        rows.clear();
        for (int i = 0; i < (int) effects.size(); ++i)
        {
            auto row = std::make_unique<Row> (*this, i);
            row->update (effects[(size_t) i],
                         engine.isPluginAlive (inputIndex, program, slot, i),
                         engine.isPluginLoading (inputIndex, program, slot, i),
                         engine.getPluginLoadError (inputIndex, program, slot, i),
                         i > 0, i + 1 < (int) effects.size());
            addAndMakeVisible (row.get());
            rows.push_back (std::move (row));
        }
        resized();
    }

    int preferredHeight() const { return (int) rows.size() * rowHeight + rowHeight; }

    void resized() override
    {
        auto r = getLocalBounds();
        for (auto& row : rows)
            row->setBounds (r.removeFromTop (rowHeight));
        auto last = r.removeFromTop (rowHeight).reduced (0, 2);
        addBtn.setBounds (last.removeFromRight (110));
    }

    void paint (Graphics& g) override
    {
        if (rows.empty()) return;
        g.setColour (textDim.withAlpha (0.35f));
        g.drawVerticalLine (10, 0.0f, (float) rows.size() * rowHeight);
    }

private:
    struct Row : public Component
    {
        Row (EffectChainComponent& c, int idx) : chain (c), index (idx)
        {
            addAndMakeVisible (on);
            addAndMakeVisible (icon);
            addAndMakeVisible (name);
            addAndMakeVisible (guiBtn);
            addAndMakeVisible (upBtn);
            addAndMakeVisible (downBtn);
            addAndMakeVisible (removeBtn);
            name.setFont (FontOptions (13.0f));
            on.setTooltip ("Effect active (off = bypassed)");
            upBtn.setTooltip ("Move earlier in the chain");
            downBtn.setTooltip ("Move later in the chain");

            auto& e = chain.engine;
            auto& o = chain.owner;
            on.onClick        = [this, &e, &o] { const auto a = chain.address(); e.setEffectBypassed (a.first, a.second, chain.slot, index, ! on.getToggleState()); o.markDirty(); };
            guiBtn.onClick    = [this, &e, &o] { const auto a = chain.address(); if (alive) o.openPluginEditor (a.first, a.second, chain.slot, index);
                                                                                  else       e.reloadPlugin (a.first, a.second, chain.slot, index); };
            upBtn.onClick     = [this, &e]     { const auto a = chain.address(); e.moveEffect (a.first, a.second, chain.slot, index, index - 1); };
            downBtn.onClick   = [this, &e]     { const auto a = chain.address(); e.moveEffect (a.first, a.second, chain.slot, index, index + 1); };
            removeBtn.onClick = [this, &e]     { const auto a = chain.address(); e.removeEffect (a.first, a.second, chain.slot, index); };
        }

        void update (const EffectDef& def, bool loaded, bool loading, const String& error, bool canUp, bool canDown)
        {
            alive = loaded;
            on.setToggleState (! def.bypassed, dontSendNotification);
            icon.setImage (chain.engine.getPluginHost().getIcons().get (def.plugin, 40), RectanglePlacement::centred);
            icon.setAlpha (def.bypassed ? 0.45f : 1.0f);
            name.setText (def.plugin.name + "  [" + def.plugin.pluginFormatName + "]" + (loading ? "   loading..." : String()), dontSendNotification);
            name.setColour (Label::textColourId, error.isNotEmpty() ? Colours::orangered : (def.bypassed || loading ? textDim : Colours::white));
            name.setTooltip (error.isNotEmpty() ? error : def.plugin.fileOrIdentifier);
            guiBtn.setButtonText (loading ? "Loading" : loaded ? "Edit GUI" : "Reload");
            guiBtn.setEnabled (! loading);
            guiBtn.setColour (TextButton::buttonColourId, loaded || loading ? getLookAndFeel().findColour (TextButton::buttonColourId) : Colours::darkred);
            upBtn.setEnabled (canUp);
            downBtn.setEnabled (canDown);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (0, 2);
            r.removeFromLeft (18);   // indent under the chain line
            on.setBounds (r.removeFromLeft (24));
            icon.setBounds (r.removeFromLeft (18).reduced (0, 1)); r.removeFromLeft (6);
            removeBtn.setBounds (r.removeFromRight (28)); r.removeFromRight (4);
            downBtn.setBounds (r.removeFromRight (26));
            upBtn.setBounds (r.removeFromRight (26));     r.removeFromRight (4);
            guiBtn.setBounds (r.removeFromRight (64));    r.removeFromRight (4);
            name.setBounds (r);
        }

        EffectChainComponent& chain;
        int index;
        bool alive = false;
        ToggleButton on;
        ImageComponent icon;
        Label name;
        TextButton guiBtn { "Edit GUI" }, upBtn { "^" }, downBtn { "v" }, removeBtn { "X" };
    };

    void showAddMenu()
    {
        Array<PluginDescription> types;
        for (auto& d : engine.getPluginHost().getKnownPlugins().getTypes())
            if (! d.isInstrument) types.add (d);
        if (types.isEmpty())
        {
            owner.showStatus ("No effect plugins known yet. Use Plugins... to scan for VST3/LV2 plugins.");
            return;
        }
        PopupMenu menu;
        addPluginsToMenu (menu, types, engine.getPluginHost().getIcons());

        /* A hardware synth or module, played through a MIDI port: everything
           a plugin slot does -- splits, layers, curves -- with the notes going
           out to the instrument instead. */
        const auto ports = MidiOutput::getAvailableDevices();
        PopupMenu external;
        for (int i = 0; i < ports.size(); ++i) external.addItem (100000 + i, ports[i].name);
        if (ports.isEmpty()) external.addItem (99999, "(no MIDI outputs found)", false);
        menu.addSeparator();
        menu.addSubMenu ("External MIDI instrument", external);

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&addBtn), [this, types, ports] (int result)
        {
            if (result >= 100000 && result < 100000 + ports.size())
            {
                engine.addExternalSlot (owner.getSelectedInput(), owner.getEditedProgram(), ports[result - 100000]);
                owner.showStatus ("Added " + ports[result - 100000].name + ": set its channel, patch and audio return under Setup...");
                return;
            }
            const int idx = result - 1;
            if (idx < 0 || idx >= types.size()) return;
            String error;
            const auto a = address();
            if (! engine.addEffect (a.first, a.second, slot, types[idx], error))
                owner.showStatus ("Could not load " + types[idx].name + ": " + error);
            else if (MainComponent::looksLikeAraPlugin (types[idx]))
                owner.showStatus (araWarning (types[idx].name));
            else
                owner.showStatus ("Added effect " + types[idx].name);
        });
    }

    Engine& engine;
    MainComponent& owner;
    int slot;
    TextButton addBtn { "+ Add effect..." };
    std::vector<std::unique_ptr<Row>> rows;

    friend struct Row;
};

//==============================================================================
//  SlotsPanel
//==============================================================================
/** Options > Outputs: the device channel pairs the rig lands on, each with a
    master chain and a volume. A window of its own because it is set up once
    per venue, not touched mid-set. */
class OutputsComponent : public Component,
                         private ListBoxModel,
                         private Engine::Listener
{
public:
    OutputsComponent (Engine& e, MainComponent& o) : engine (e), owner (o), chain (e, o, -1)
    {
        engine.addListener (this);
        addAndMakeVisible (list);
        list.setModel (this);
        list.setRowHeight (40);
        list.setColour (ListBox::backgroundColourId, bgPanel);

        addAndMakeVisible (addBtn);
        addAndMakeVisible (removeBtn);
        addBtn.onClick = [this]
        {
            const auto names = engine.getAudioOutputChannelNames();
            const int n = (int) engine.getSetup().outputs.size();
            const int idx = engine.addOutput ("Output " + String (n + 1), jmin (2 * n, 62), jmin (2 * n + 1, 63));
            owner.markDirty();
            list.updateContent();
            list.selectRow (idx);
            if (engine.getAudioOutputChannelNames().size() < 2 * n + 2)
                owner.showStatus ("The device has not opened those channels. Open Audio... and tick more output channels, then pick them here.");
            ignoreUnused (names);
        };
        removeBtn.onClick = [this]
        {
            const int sel = list.getSelectedRow();
            if (sel > 0 || (sel == 0 && engine.getSetup().outputs.size() > 1)) { engine.removeOutput (sel); owner.markDirty(); list.updateContent(); list.selectRow (0); }
        };

        addAndMakeVisible (nameEditor);
        nameEditor.onTextChange = [this] { if (int s = list.getSelectedRow(); s >= 0) { engine.setOutputName (s, nameEditor.getText()); owner.markDirty(); list.repaintRow (s); } };

        addAndMakeVisible (channelsBox);
        channelsBox.setTooltip ("Which device output channels this output goes to. Only channels opened in Audio... are offered.");
        channelsBox.onChange = [this]
        {
            const int s = list.getSelectedRow(); const int id = channelsBox.getSelectedId();
            if (s < 0 || id <= 0) return;
            if (id >= 2000) engine.setOutputChannels (s, id - 2000, id - 2000 + 1); else engine.setOutputChannels (s, id - 1000, -1);
            owner.markDirty(); list.repaintRow (s);
        };

        addAndMakeVisible (volume);
        volume.setRange (-60.0, 12.0, 0.5);
        volume.setSliderStyle (Slider::LinearBar);
        volume.setTextValueSuffix (" dB");
        volume.setDoubleClickReturnValue (true, 0.0);
        volume.setTooltip ("Master volume of this output, after its effects. Double-click for 0 dB.");
        volume.onValueChange = [this] { if (int s = list.getSelectedRow(); s >= 0) { engine.setOutputGainDb (s, (float) volume.getValue()); owner.markDirty(); } };

        addAndMakeVisible (learnBtn);
        learnBtn.setTooltip ("Press, then move the pedal or knob that should control this output's volume -- on any keyboard, any channel.");
        learnBtn.onClick = [this]
        {
            const int s = list.getSelectedRow(); if (s < 0) return;
            if (engine.isOutputVolumeLearnArmed()) { engine.armOutputVolumeLearn (-1); learnBtn.setButtonText ("Learn volume CC"); }
            else { engine.armOutputVolumeLearn (s); learnBtn.setButtonText ("Move it now..."); }
        };
        addAndMakeVisible (clearCcBtn);
        clearCcBtn.onClick = [this] { if (int s = list.getSelectedRow(); s >= 0) { engine.setOutputVolumeCC (s, 0); owner.markDirty(); } };

        for (auto* l : { &nameLabel, &channelsLabel, &volumeLabel, &ccLabel, &chainLabel })
        {
            addAndMakeVisible (l);
            l->setColour (Label::textColourId, textDim);
            l->setFont (FontOptions (12.0f));
        }
        chainLabel.setFont (FontOptions (13.0f, Font::bold));
        chainLabel.setColour (Label::textColourId, accent);

        addAndMakeVisible (chain);
        chain.address = [this] { return std::make_pair (Engine::outputAddress (list.getSelectedRow()), 0); };

        list.selectRow (0);
        refresh();
    }
    ~OutputsComponent() override { engine.armOutputVolumeLearn (-1); engine.removeListener (this); }

    void refresh()
    {
        list.updateContent();
        list.repaint();
        const auto& outs = engine.getSetup().outputs;
        const int s = list.getSelectedRow();
        const bool valid = s >= 0 && s < (int) outs.size();
        for (auto* c : std::initializer_list<Component*> { &nameEditor, &channelsBox, &volume, &learnBtn, &clearCcBtn, &chain })
            c->setEnabled (valid);
        removeBtn.setEnabled (valid && outs.size() > 1);
        if (! valid) return;
        const auto& def = outs[(size_t) s];
        if (nameEditor.getText() != def.name) nameEditor.setText (def.name, dontSendNotification);
        volume.setValue (def.gainDb, dontSendNotification);
        ccLabel.setText (def.volumeCC > 0 ? "Volume follows CC " + String (def.volumeCC) : "No volume controller", dontSendNotification);
        learnBtn.setButtonText (engine.isOutputVolumeLearnArmed() ? "Move it now..." : "Learn volume CC");

        const auto names = engine.getAudioOutputChannelNames();
        channelsBox.clear (dontSendNotification);
        for (int c = 0; c < names.size(); ++c)
            channelsBox.addItem (String (c + 1) + ": " + names[c], 1000 + c);
        for (int c = 0; c + 1 < names.size(); ++c)
            channelsBox.addItem (String (c + 1) + "+" + String (c + 2) + ": " + names[c] + " / " + names[c + 1], 2000 + c);
        const int want = def.right >= 0 ? 2000 + def.left : 1000 + def.left;
        if (channelsBox.indexOfItemId (want) >= 0) channelsBox.setSelectedId (want, dontSendNotification);
        else channelsBox.setText (String (def.left + 1) + (def.right >= 0 ? "+" + String (def.right + 1) : String()) + " (not open: see Audio...)", dontSendNotification);

        chain.rebuild (def.chain.effects, Engine::outputAddress (s), 0);
        resized();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto left = r.removeFromLeft (200);
        auto buttons = left.removeFromBottom (26);
        addBtn.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (2, 0));
        removeBtn.setBounds (buttons.reduced (2, 0));
        left.removeFromBottom (6);
        list.setBounds (left);
        r.removeFromLeft (12);

        nameLabel.setBounds (r.removeFromTop (16));
        nameEditor.setBounds (r.removeFromTop (24)); r.removeFromTop (8);
        channelsLabel.setBounds (r.removeFromTop (16));
        channelsBox.setBounds (r.removeFromTop (24)); r.removeFromTop (8);
        volumeLabel.setBounds (r.removeFromTop (16));
        auto vr = r.removeFromTop (24);
        volume.setBounds (vr.removeFromLeft (jmax (120, vr.getWidth() - 300))); vr.removeFromLeft (8);
        learnBtn.setBounds (vr.removeFromLeft (130)); vr.removeFromLeft (4);
        clearCcBtn.setBounds (vr.removeFromLeft (60)); vr.removeFromLeft (8);
        ccLabel.setBounds (vr);
        r.removeFromTop (12);
        chainLabel.setBounds (r.removeFromTop (20));
        chain.setBounds (r.removeFromTop (chain.preferredHeight()));
    }

    // ListBoxModel
    int getNumRows() override { return (int) engine.getSetup().outputs.size(); }
    void paintListBoxItem (int row, Graphics& g, int w, int, bool selected) override
    {
        const auto& outs = engine.getSetup().outputs;
        if (row < 0 || row >= (int) outs.size()) return;
        const auto& d = outs[(size_t) row];
        g.fillAll (selected ? accentDim : bgRow);
        g.setColour (Colours::white);
        g.setFont (FontOptions (14.0f, Font::bold));
        g.drawText (d.name, 8, 3, w - 16, 18, Justification::centredLeft, true);
        g.setFont (FontOptions (12.0f));
        g.setColour (textDim);
        int users = 0;
        for (auto& in : engine.getSetup().inputs) if (in.output == row) ++users;
        g.drawText ("channels " + String (d.left + 1) + (d.right >= 0 ? "+" + String (d.right + 1) : String ("")) + "   \xc2\xb7   "
                    + String (users) + (users == 1 ? " input" : " inputs") + "   \xc2\xb7   " + String (d.chain.effects.size()) + " fx",
                    8, 21, w - 16, 16, Justification::centredLeft, true);
    }
    void selectedRowsChanged (int) override { refresh(); }

private:
    // Engine::Listener
    void outputsChanged() override { refresh(); }
    void setupChanged() override { refresh(); }
    void outputVolumeLearned (int output, int cc) override
    {
        owner.showStatus ("CC " + String (cc) + " now controls the volume of output " + engine.getSetup().outputs[(size_t) jlimit (0, (int) engine.getSetup().outputs.size() - 1, output)].name + ".");
        owner.markDirty();
        refresh();
    }

    Engine& engine;
    MainComponent& owner;
    ListBox list;
    TextButton addBtn { "+ Add" }, removeBtn { "- Remove" };
    Label nameLabel { {}, "Name" }, channelsLabel { {}, "Device channels" }, volumeLabel { {}, "Master volume" },
          ccLabel, chainLabel { {}, "MASTER EFFECTS  (on everything that lands on this output)" };
    TextEditor nameEditor;
    ComboBox channelsBox;
    Slider volume;
    TextButton learnBtn { "Learn volume CC" }, clearCcBtn { "Clear" };
    EffectChainComponent chain;
};

//==============================================================================
class SlotsPanel : public Component
{
public:
    static constexpr int slotHeaderHeight = 118;

    struct SlotRow : public Component
    {
        SlotRow (SlotsPanel& p, int idx) : panel (p), index (idx), chain (p.engine, p.owner, idx)
        {
            addAndMakeVisible (enabled);
            addAndMakeVisible (icon);
            addAndMakeVisible (name);
            addAndMakeVisible (gain);
            addAndMakeVisible (transpose);
            addAndMakeVisible (keyRange);
            addAndMakeVisible (outCh);
            addAndMakeVisible (inCh);
            addAndMakeVisible (pan);
            addAndMakeVisible (velRange);
            addAndMakeVisible (velCurve);
            for (auto* c : { &gainCap, &transposeCap, &keysCap, &chCap, &inChCap, &panCap, &velCap, &curveCap })
            {
                addAndMakeVisible (c);
                c->setFont (FontOptions (10.5f, Font::bold));
                c->setColour (Label::textColourId, textDim);
                c->setJustificationType (Justification::bottomLeft);
                c->setBorderSize ({ 0, 3, 0, 0 });
            }
            gainCap.setText ("GAIN", dontSendNotification);
            transposeCap.setText ("TRANSPOSE", dontSendNotification);
            chCap.setText ("PLAYS ON", dontSendNotification);
            inChCap.setText ("LISTENS TO", dontSendNotification);
            panCap.setText ("PAN", dontSendNotification);
            curveCap.setText ("VELOCITY CURVE", dontSendNotification);

            pan.setRange (-1.0, 1.0, 0.01);
            pan.setSliderStyle (Slider::LinearBar);
            pan.textFromValueFunction = [] (double v) { const int n = (int) std::lround (std::abs (v) * 100.0); return n == 0 ? String ("centre") : (v < 0 ? "L " : "R ") + String (n); };
            pan.valueFromTextFunction = [] (const String& t) { const double n = t.retainCharacters ("0123456789").getDoubleValue() / 100.0; return t.containsIgnoreCase ("L") ? -n : (t.containsIgnoreCase ("R") ? n : 0.0); };
            pan.setDoubleClickReturnValue (true, 0.0);
            pan.updateText();     // the bar's text box was filled before the formatter existed
            pan.setTooltip ("Stereo position of this plugin in the mix. Double-click for centre.");

            velRange.setRange (1, 127, 1);
            velRange.setMinAndMaxValues (1, 127, dontSendNotification);
            velRange.setPopupDisplayEnabled (true, false, this);
            velRange.setTooltip ("Velocity layer: only notes played within this velocity range reach the plugin. Two slots with complementary ranges switch sounds by touch.");

            velCurve.setRange (-1.0, 1.0, 0.05);
            velCurve.setSliderStyle (Slider::LinearBar);
            velCurve.textFromValueFunction = [] (double v) { const int n = (int) std::lround (std::abs (v) * 100.0); return n == 0 ? String ("linear") : (v > 0 ? "+" + String (n) + " % louder" : String (n) + " % softer"); };
            velCurve.valueFromTextFunction = [] (const String& t) { const double n = t.retainCharacters ("0123456789").getDoubleValue() / 100.0; return t.containsIgnoreCase ("soft") || t.startsWith ("-") ? -n : n; };
            velCurve.setDoubleClickReturnValue (true, 0.0);
            velCurve.updateText();     // the bar's text box was filled before the formatter existed
            velCurve.setTooltip ("Reshapes incoming velocities: louder makes soft playing come out stronger (for a stiff keyboard or a quiet library), softer does the opposite. Double-click for linear.");
            addAndMakeVisible (guiBtn);
            addAndMakeVisible (removeBtn);
            addAndMakeVisible (chain);

            name.setFont (FontOptions (14.0f, Font::bold));
            name.setColour (Label::textColourId, Colours::white);

            gain.setRange (-60.0, 12.0, 0.1);
            gain.setSliderStyle (Slider::LinearBar);
            gain.setTextValueSuffix (" dB");
            gain.setDoubleClickReturnValue (true, 0.0);
            gain.setTooltip ("Output level of this plugin, applied after its effects. Drag, or double-click for 0 dB.");

            transpose.setRange (-36, 36, 1);
            transpose.setSliderStyle (Slider::LinearBar);
            transpose.textFromValueFunction = [] (double v) { const int st = (int) v; return (st > 0 ? "+" : "") + String (st) + " st"; };
            transpose.valueFromTextFunction = [] (const String& t) { return (double) t.retainCharacters ("-0123456789").getIntValue(); };
            transpose.setDoubleClickReturnValue (true, 0.0);
            transpose.updateText();     // the bar's text box was filled before the formatter existed
            transpose.setTooltip ("Shift incoming notes by this many semitones (+12 = one octave up). Double-click for none.");

            keyRange.setRange (0, 127, 1);
            keyRange.setMinAndMaxValues (0, 127, dontSendNotification);
            keyRange.textFromValueFunction = [] (double v) { return noteName ((int) v); };
            keyRange.setPopupDisplayEnabled (true, false, this);
            keyRange.setTooltip ("Which keys reach this plugin. Drag the two handles to make a split: notes outside the range are ignored by this slot.");

            outCh.addItem ("Same channel", 1);
            for (int c = 1; c <= 16; ++c) outCh.addItem ("Channel " + String (c), c + 1);
            inCh.addItem ("All channels", 1);
            for (int c = 1; c <= 16; ++c) inCh.addItem ("Channel " + String (c), c + 1);
            inCh.setTooltip ("Which incoming MIDI channel this slot plays. With the input on Omni, one program can give each "
                             "channel its own sound: a bass on channel 2 and a pad on channel 1, from one keyboard or one MIDI file.");
            outCh.setTooltip ("MIDI channel the plugin receives on. Keep the input's channel, or force one (multi-timbral plugins like Kontakt play the instrument on that channel).");

            auto& e = panel.engine;
            auto& o = panel.owner;
            enabled.onClick   = [this, &e, &o] { e.setSlotEnabled   (o.getSelectedInput(), o.getEditedProgram(), index, enabled.getToggleState()); o.markDirty(); };
            gain.onValueChange      = [this, &e, &o] { e.setSlotGainDb    (o.getSelectedInput(), o.getEditedProgram(), index, (float) gain.getValue()); o.markDirty(); };
            transpose.onValueChange = [this, &e, &o] { e.setSlotTranspose (o.getSelectedInput(), o.getEditedProgram(), index, (int) transpose.getValue()); o.markDirty(); };
            keyRange.onValueChange  = [this, &e, &o] { e.setSlotKeyRange (o.getSelectedInput(), o.getEditedProgram(), index, (int) keyRange.getMinValue(), (int) keyRange.getMaxValue());
                                                       updateKeyCaption(); o.markDirty(); };
            pan.onValueChange       = [this, &e, &o] { e.setSlotPan (o.getSelectedInput(), o.getEditedProgram(), index, (float) pan.getValue()); o.markDirty(); };
            velCurve.onValueChange  = [this, &e, &o] { e.setSlotVelocityCurve (o.getSelectedInput(), o.getEditedProgram(), index, (float) velCurve.getValue()); o.markDirty(); };
            velRange.onValueChange  = [this, &e, &o] { e.setSlotVelocityRange (o.getSelectedInput(), o.getEditedProgram(), index, (int) velRange.getMinValue(), (int) velRange.getMaxValue());
                                                       updateVelCaption(); o.markDirty(); };
            outCh.onChange    = [this, &e, &o] { e.setSlotOutChannel (o.getSelectedInput(), o.getEditedProgram(), index, outCh.getSelectedId() - 1); o.markDirty(); };
            inCh.onChange     = [this, &e, &o] { e.setSlotReceiveChannel (o.getSelectedInput(), o.getEditedProgram(), index, inCh.getSelectedId() - 1); o.markDirty(); };
            guiBtn.onClick    = [this, &e, &o] { if (external) showExternalMenu();
                                                 else if (alive) o.openPluginEditor (o.getSelectedInput(), o.getEditedProgram(), index, -1);
                                                 else       e.reloadPlugin (o.getSelectedInput(), o.getEditedProgram(), index, -1); };
            removeBtn.onClick = [this, &e, &o] { e.removeSlot (o.getSelectedInput(), o.getEditedProgram(), index); };
        }

        void update (const SlotDef& def, bool loaded, bool loading, const String& error, int inputIndex, int program)
        {
            alive = loaded;
            enabled.setToggleState (def.enabled, dontSendNotification);
            external = def.isExternal();
            PluginDescription forIcon = def.plugin;
            if (external) { forIcon = {}; forIcon.name = def.midiOutName; forIcon.pluginFormatName = "MIDI"; }   // a badge with the port's initials
            icon.setImage (panel.engine.getPluginHost().getIcons().get (forIcon, 48), RectanglePlacement::centred);
            icon.setAlpha (def.enabled ? 1.0f : 0.45f);
            if (external)
            {
                String what = def.midiOutName + "  [MIDI out";
                if (def.patch >= 0) what << ", patch " << (def.patch + 1);
                what << (def.returnLeft >= 0 ? ", audio back on in " + String (def.returnLeft + 1) + (def.returnRight >= 0 ? "+" + String (def.returnRight + 1) : String()) : String()) << "]";
                name.setText (what, dontSendNotification);
                name.setColour (Label::textColourId, loaded ? Colours::white : Colours::orangered);
                name.setTooltip (loaded ? "An external instrument, played through this MIDI port" : error);
            }
            else
            {
                name.setText (def.plugin.name + "  [" + def.plugin.pluginFormatName + "]" + (loading ? "   loading..." : String()), dontSendNotification);
                name.setColour (Label::textColourId, error.isNotEmpty() ? Colours::orangered : (loading ? textDim : Colours::white));
                name.setTooltip (error.isNotEmpty() ? error : def.plugin.fileOrIdentifier);
            }
            gain.setValue (def.gainDb, dontSendNotification);
            transpose.setValue (def.transpose, dontSendNotification);
            keyRange.setMinAndMaxValues (def.lowKey, def.highKey, dontSendNotification);
            updateKeyCaption();
            pan.setValue (def.pan, dontSendNotification);
            velCurve.setValue (def.velocityCurve, dontSendNotification);
            velRange.setMinAndMaxValues (def.lowVelocity, def.highVelocity, dontSendNotification);
            updateVelCaption();
            outCh.setSelectedId (def.outChannel + 1, dontSendNotification);
            inCh.setSelectedId (def.receiveChannel + 1, dontSendNotification);
            slotDef = def;
            if (external)
            {
                guiBtn.setButtonText ("Setup...");
                guiBtn.setEnabled (true);
                guiBtn.setColour (TextButton::buttonColourId, getLookAndFeel().findColour (TextButton::buttonColourId));
            }
            else
            {
                guiBtn.setButtonText (loading ? "Loading" : loaded ? "Edit GUI" : "Reload");
                guiBtn.setEnabled (! loading);
                guiBtn.setColour (TextButton::buttonColourId, loaded || loading ? getLookAndFeel().findColour (TextButton::buttonColourId) : Colours::darkred);
            }
            chain.rebuild (def.effects, inputIndex, program);
        }

        int preferredHeight() const { return slotHeaderHeight + chain.preferredHeight() + 4; }

        void updateVelCaption()
        {
            const int lo = (int) velRange.getMinValue(), hi = (int) velRange.getMaxValue();
            velCap.setText ("VELOCITY   " + (lo <= 1 && hi >= 127 ? String ("all") : String (lo) + " to " + String (hi)), dontSendNotification);
        }

        void updateKeyCaption()
        {
            const int lo = (int) keyRange.getMinValue(), hi = (int) keyRange.getMaxValue();
            keysCap.setText ("KEY RANGE   " + (lo == 0 && hi == 127 ? String ("all keys") : noteName (lo) + " to " + noteName (hi)), dontSendNotification);
        }

        void paint (Graphics& g) override
        {
            g.setColour (bgRow);
            g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0, 1), 4.0f);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (6, 4);
            auto top = r.removeFromTop (24);
            enabled.setBounds (top.removeFromLeft (24));
            icon.setBounds (top.removeFromLeft (24).reduced (0, 1)); top.removeFromLeft (8);
            removeBtn.setBounds (top.removeFromRight (28));
            top.removeFromRight (4);
            guiBtn.setBounds (top.removeFromRight (64));
            name.setBounds (top);

            r.removeFromTop (2);
            auto caps = r.removeFromTop (14);
            auto bottom = r.removeFromTop (22);
            const int w = bottom.getWidth();
            auto column = [&] (int percent, Component& cap, Component& control)
            {
                const int cw = w * percent / 100;
                cap.setBounds (caps.removeFromLeft (cw).reduced (2, 0));
                control.setBounds (bottom.removeFromLeft (cw).reduced (2, 0));
            };
            column (20, gainCap, gain);
            column (12, panCap, pan);
            column (14, transposeCap, transpose);
            column (27, inChCap, inCh);
            chCap.setBounds (caps.reduced (2, 0));
            outCh.setBounds (bottom.reduced (2, 0));

            r.removeFromTop (4);
            caps = r.removeFromTop (14);
            bottom = r.removeFromTop (22);
            column (38, keysCap, keyRange);
            column (36, velCap, velRange);
            curveCap.setBounds (caps.reduced (2, 0));
            velCurve.setBounds (bottom.reduced (2, 0));

            r.removeFromTop (6);
            chain.setBounds (r.withTrimmedLeft (24));
        }

        /** An external instrument's port, patch recall and audio return. */
        void showExternalMenu()
        {
            auto& e = panel.engine;
            auto& o = panel.owner;
            const int in = o.getSelectedInput(), prog = o.getEditedProgram();
            const auto ports = MidiOutput::getAvailableDevices();
            const auto inputs = e.getAudioInputChannelNames();

            PopupMenu portMenu;
            for (int i = 0; i < ports.size(); ++i)
                portMenu.addItem (100 + i, ports[i].name, true, ports[i].identifier == slotDef.midiOutIdentifier);
            PopupMenu returnMenu;
            returnMenu.addItem (300, "None: it goes to the desk on its own", true, slotDef.returnLeft < 0);
            for (int c = 0; c < inputs.size(); ++c)
                returnMenu.addItem (400 + c, String (c + 1) + ": " + inputs[c], true, slotDef.returnLeft == c && slotDef.returnRight < 0);
            for (int c = 0; c + 1 < inputs.size(); ++c)
                returnMenu.addItem (500 + c, String (c + 1) + "+" + String (c + 2) + ": " + inputs[c] + " / " + inputs[c + 1], true,
                                    slotDef.returnLeft == c && slotDef.returnRight == c + 1);
            if (inputs.isEmpty()) returnMenu.addItem (301, "(no interface inputs open: see Audio...)", false);

            PopupMenu m;
            m.addSubMenu ("MIDI port", portMenu);
            m.addItem (200, "Patch recall...");
            m.addSubMenu ("Audio comes back on", returnMenu);
            m.showMenuAsync (PopupMenu::Options().withTargetComponent (guiBtn), [this, &e, &o, in, prog, ports] (int r)
            {
                if (r >= 100 && r < 100 + ports.size()) e.setSlotMidiOut (in, prog, index, ports[r - 100]);
                else if (r == 200) showPatchDialog();
                else if (r == 300) e.setSlotReturn (in, prog, index, -1, -1);
                else if (r >= 400 && r < 500) e.setSlotReturn (in, prog, index, r - 400, -1);
                else if (r >= 500) e.setSlotReturn (in, prog, index, r - 500, r - 500 + 1);
                else return;
                o.markDirty();
                panel.refresh();
            });
        }

        void showPatchDialog()
        {
            auto* w = new AlertWindow ("Patch recall",
                                       "Sent to " + slotDef.midiOutName + " when this program is selected, on the channel the slot "
                                       "plays on. Leave a box empty to send nothing. Program numbers as the synth shows them, 1-128.",
                                       MessageBoxIconType::NoIcon);
            auto txt = [] (int v, int add) { return v >= 0 ? String (v + add) : String(); };
            w->addTextEditor ("msb", txt (slotDef.bankMsb, 0), "Bank select MSB (CC 0), 0-127");
            w->addTextEditor ("lsb", txt (slotDef.bankLsb, 0), "Bank select LSB (CC 32), 0-127");
            w->addTextEditor ("prog", txt (slotDef.patch, 1), "Program, 1-128");
            w->addButton ("OK", 1, KeyPress (KeyPress::returnKey));
            w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
            auto& e = panel.engine;
            auto& o = panel.owner;
            const int in = o.getSelectedInput(), prog = o.getEditedProgram();
            w->enterModalState (true, ModalCallbackFunction::create ([this, w, &e, &o, in, prog] (int r)
            {
                if (r == 1)
                {
                    auto num = [w] (const char* id, int lo, int hi, int sub) {
                        const auto t = w->getTextEditorContents (id).trim();
                        return t.isEmpty() ? -1 : jlimit (lo, hi, t.getIntValue()) - sub; };
                    e.setSlotPatch (in, prog, index, num ("msb", 0, 127, 0), num ("lsb", 0, 127, 0), num ("prog", 1, 128, 1));
                    o.markDirty();
                    panel.refresh();
                }
            }), true);
        }

        SlotsPanel& panel;
        int index;
        bool alive = false, external = false;
        SlotDef slotDef;
        ImageComponent icon;
        ToggleButton enabled;
        Label name;
        Slider gain, transpose, pan, velCurve, keyRange { Slider::TwoValueHorizontal, Slider::NoTextBox }, velRange { Slider::TwoValueHorizontal, Slider::NoTextBox };
        Label gainCap, transposeCap, keysCap, chCap, inChCap, panCap, velCap, curveCap;
        ComboBox outCh, inCh;
        TextButton guiBtn { "Edit GUI" }, removeBtn { "X" };
        EffectChainComponent chain;
    };

    /** The program-level chain, shown after the slots. */
    struct ProgramChainRow : public Component
    {
        ProgramChainRow (SlotsPanel& p) : chain (p.engine, p.owner, -1)
        {
            addAndMakeVisible (header);
            addAndMakeVisible (chain);
            header.setFont (FontOptions (13.0f, Font::bold));
            header.setColour (Label::textColourId, accent);
        }
        int preferredHeight() const { return 26 + chain.preferredHeight() + 6; }
        void paint (Graphics& g) override
        {
            g.setColour (bgRow.darker (0.15f));
            g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0, 1), 4.0f);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (6, 4);
            header.setBounds (r.removeFromTop (22));
            chain.setBounds (r.withTrimmedLeft (24));
        }
        Label header { {}, "PROGRAM EFFECTS  (after the mix of all slots)" };
        EffectChainComponent chain;
    };

    SlotsPanel (Engine& e, MainComponent& o) : engine (e), owner (o)
    {
        styleHeader (header);
        addAndMakeVisible (header);
        addAndMakeVisible (addBtn);
        addBtn.onClick = [this] { showAddMenu(); };
        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&container, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (emptyLabel);
        emptyLabel.setColour (Label::textColourId, textDim);
        emptyLabel.setJustificationType (Justification::centred);
        /* The hint lies across the chain row, and a Label takes the clicks that
           land on it: with no instrument yet, "+ Add effect" did nothing. For
           an audio input that is the normal state, not a passing one. */
        emptyLabel.setInterceptsMouseClicks (false, false);

        // Audio inputs only: the level arriving, and the trim on the signal before the chain.
        addChildComponent (meter);
        meter.setTooltip ("What is arriving at this input, before the trim: set the interface's gain so it stays out of the red.");
        addChildComponent (trimCap);
        trimCap.setFont (FontOptions (10.5f, Font::bold));
        trimCap.setColour (Label::textColourId, textDim);
        trimCap.setJustificationType (Justification::centredRight);
        addChildComponent (trim);
        trim.setRange (-60.0, 24.0, 0.5);
        trim.setSliderStyle (Slider::LinearBar);
        trim.setTextValueSuffix (" dB");
        trim.setDoubleClickReturnValue (true, 0.0);
        trim.setTooltip ("Level of the incoming signal before this program's effects. Double-click for 0 dB.");
        trim.onValueChange = [this]
        {
            engine.setProgramInputGainDb (owner.getSelectedInput(), owner.getEditedProgram(), (float) trim.getValue());
            owner.markDirty();
        };
    }

    void refresh()
    {
        const auto& inputs = engine.getSetup().inputs;
        const int in = owner.getSelectedInput();
        const int prog = owner.getEditedProgram();
        rows.clear();
        programChain.reset();
        audioInput = in >= 0 && in < (int) inputs.size() && inputs[(size_t) in].isAudio();
        if (in >= 0 && in < (int) inputs.size())
        {
            const auto& def = inputs[(size_t) in].programs[(size_t) prog];
            if (! audioInput)
                for (int s = 0; s < (int) def.slots.size(); ++s)
                {
                    auto row = std::make_unique<SlotRow> (*this, s);
                    row->update (def.slots[(size_t) s], engine.isPluginAlive (in, prog, s), engine.isPluginLoading (in, prog, s), engine.getPluginLoadError (in, prog, s), in, prog);
                    container.addAndMakeVisible (row.get());
                    rows.push_back (std::move (row));
                }
            programChain = std::make_unique<ProgramChainRow> (*this);
            if (audioInput)
                programChain->header.setText ("EFFECTS  (on the incoming signal)", dontSendNotification);
            programChain->chain.rebuild (def.effects, in, prog);
            container.addAndMakeVisible (programChain.get());
            trim.setValue (def.inputGainDb, dontSendNotification);
            meter.setInput (in);
        }
        /* An audio input has no instruments to add: the signal is the
           instrument. Its chain's own "+ Add effect" is the way in. */
        header.setText (audioInput ? "EFFECTS" : "PLUGINS", dontSendNotification);
        addBtn.setVisible (! audioInput);
        meter.setVisible (audioInput);
        trimCap.setVisible (audioInput);
        trim.setVisible (audioInput);
        emptyLabel.setText (audioInput ? "No effects in this program. The signal passes straight through; add an effect below."
                                       : "No plugins in this program. Click \"Add instrument...\"", dontSendNotification);
        emptyLabel.setVisible (rows.empty() && (programChain == nullptr || def_hasNoEffects (in, prog)));
        resized();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        auto top = r.removeFromTop (24);
        if (audioInput)
        {
            trim.setBounds (top.removeFromRight (100));
            top.removeFromRight (4);
            trimCap.setBounds (top.removeFromRight (80));
            top.removeFromRight (12);
            meter.setBounds (top.removeFromRight (160).reduced (0, 5));
        }
        else
            addBtn.setBounds (top.removeFromRight (140));
        header.setBounds (top);
        r.removeFromTop (4);
        viewport.setBounds (r);
        emptyLabel.setBounds (r.withTrimmedBottom (r.getHeight() / 3));
        layoutRows();
    }

private:
    bool def_hasNoEffects (int in, int prog) const
    {
        const auto& inputs = engine.getSetup().inputs;
        if (in < 0 || in >= (int) inputs.size()) return true;
        return inputs[(size_t) in].programs[(size_t) prog].effects.empty();
    }

    void layoutRows()
    {
        int total = 0;
        for (auto& r : rows) total += r->preferredHeight() + 4;
        if (programChain != nullptr) total += programChain->preferredHeight() + 4;

        const int w = viewport.getWidth() - (total > viewport.getHeight() ? viewport.getScrollBarThickness() : 0);
        container.setSize (jmax (1, w), jmax (1, total));
        int y = 0;
        for (auto& r : rows) { r->setBounds (0, y, w, r->preferredHeight()); y += r->preferredHeight() + 4; }
        if (programChain != nullptr) programChain->setBounds (0, y, w, programChain->preferredHeight());
    }

    void showAddMenu()
    {
        auto types = engine.getPluginHost().getKnownPlugins().getTypes();
        if (types.isEmpty())
        {
            owner.showStatus ("No plugins known yet. Use Plugins... to scan for VST3/LV2 plugins.");
            return;
        }
        PopupMenu menu;
        addPluginsToMenu (menu, types, engine.getPluginHost().getIcons());
        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&addBtn), [this, types] (int result)
        {
            const int idx = result - 1;
            if (idx < 0 || idx >= types.size()) return;
            String error;
            if (! engine.addSlot (owner.getSelectedInput(), owner.getEditedProgram(), types[idx], error))
                owner.showStatus ("Could not load " + types[idx].name + ": " + error);
            else if (MainComponent::looksLikeAraPlugin (types[idx]))
                owner.showStatus (araWarning (types[idx].name));
            else
                owner.showStatus ("Added " + types[idx].name);
        });
    }

    Engine& engine;
    MainComponent& owner;
    Label header { {}, "PLUGINS" };
    TextButton addBtn { "+ Add instrument..." };
    Viewport viewport;
    Component container;
    std::vector<std::unique_ptr<SlotRow>> rows;
    std::unique_ptr<ProgramChainRow> programChain;
    Label emptyLabel { {}, "No plugins in this program. Click \"Add instrument...\"" };
    bool audioInput = false;
    LevelMeter meter { engine };
    Label trimCap { {}, "INPUT TRIM" };
    Slider trim;

    friend struct SlotRow;
    friend struct ProgramChainRow;
};

//==============================================================================
//  SuggestionsComponent -- confirm dialog for proposed mappings
//==============================================================================
class SuggestionsComponent : public Component,
                             private ListBoxModel
{
public:
    SuggestionsComponent (Engine& e, int input, int program, const String& targetName, std::vector<MappingSuggestion> s)
        : engine (e), inputIndex (input), programIndex (program), suggestions (std::move (s))
    {
        selected.insertMultiple (0, true, (int) suggestions.size());

        addAndMakeVisible (info);
        info.setColour (Label::textColourId, textDim);
        info.setText (suggestions.empty()
                        ? "No suggestions for " + targetName + ". Its parameter names don't match any standard controller, and no template is saved. Use Learn MIDI, then Save template."
                        : String ((int) suggestions.size()) + " proposed mappings for " + targetName + ". Untick any you don't want.",
                      dontSendNotification);
        info.setJustificationType (Justification::topLeft);

        addAndMakeVisible (list);
        list.setModel (this);
        list.setRowHeight (24);
        list.setColour (ListBox::backgroundColourId, bgPanel);
        list.setClickingTogglesRowSelection (false);

        addAndMakeVisible (addBtn);
        addAndMakeVisible (cancelBtn);
        addBtn.setEnabled (! suggestions.empty());
        addBtn.onClick = [this]
        {
            int n = 0;
            for (int i = 0; i < (int) suggestions.size(); ++i)
                if (selected[i]) { engine.addMapping (inputIndex, programIndex, suggestions[(size_t) i].mapping); ++n; }
            close();
        };
        cancelBtn.onClick = [this] { close(); };
        setSize (620, jlimit (180, 520, 110 + 24 * (int) suggestions.size()));
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        info.setBounds (r.removeFromTop (40));
        auto buttons = r.removeFromBottom (28);
        cancelBtn.setBounds (buttons.removeFromRight (100));
        buttons.removeFromRight (6);
        addBtn.setBounds (buttons.removeFromRight (130));
        r.removeFromBottom (8);
        list.setBounds (r);
    }

    int getNumRows() override { return (int) suggestions.size(); }

    void paintListBoxItem (int row, Graphics& g, int w, int h, bool) override
    {
        if (row < 0 || row >= (int) suggestions.size()) return;
        const auto& s = suggestions[(size_t) row];
        g.fillAll (row % 2 ? bgRow : bgPanel);
        getLookAndFeel().drawTickBox (g, *this, 6.0f, 4.0f, 16.0f, 16.0f, selected[row], true, false, false);
        g.setColour (Colours::white);
        g.setFont (FontOptions (13.0f, Font::bold));
        g.drawText (s.mapping.sourceDescription(), 30, 0, 90, h, Justification::centredLeft, true);
        g.setFont (FontOptions (13.0f));
        g.drawText (s.mapping.paramName, 125, 0, w * 45 / 100, h, Justification::centredLeft, true);
        g.setColour (textDim);
        g.setFont (FontOptions (12.0f));
        g.drawText (s.reason, 125 + w * 45 / 100, 0, w - (125 + w * 45 / 100) - 6, h, Justification::centredLeft, true);
    }

    void listBoxItemClicked (int row, const MouseEvent&) override
    {
        if (row >= 0 && row < selected.size()) { selected.set (row, ! selected[row]); list.repaintRow (row); }
    }

private:
    void close()
    {
        if (auto* dw = findParentComponentOfClass<DialogWindow>())
            dw->exitModalState (0);
    }

    Engine& engine;
    int inputIndex, programIndex;
    std::vector<MappingSuggestion> suggestions;
    Array<bool> selected;
    Label info;
    ListBox list;
    TextButton addBtn { "Add selected" }, cancelBtn { "Cancel" };
};

//==============================================================================
//  MappingsPanel
//==============================================================================
class MappingsPanel : public Component,
                      private TableListBoxModel
{
public:
    enum Columns { colSource = 1, colTarget, colParam, colMin, colMax, colPass };

    struct Target { int slot = 0, effect = -1; String label; };

    MappingsPanel (Engine& e, MainComponent& o) : engine (e), owner (o)
    {
        styleHeader (header);
        addAndMakeVisible (header);
        addAndMakeVisible (table);
        table.setModel (this);
        table.setColour (ListBox::backgroundColourId, bgPanel);
        table.setHeaderHeight (22);
        table.setRowHeight (22);
        auto& th = table.getHeader();
        th.addColumn ("Source", colSource, 90);
        th.addColumn ("Target", colTarget, 150);
        th.addColumn ("Parameter", colParam, 220);
        th.addColumn ("Min", colMin, 50);
        th.addColumn ("Max", colMax, 50);
        th.addColumn ("Pass", colPass, 44);
        th.setStretchToFitActive (true);

        // Editor row 1: target
        addAndMakeVisible (targetBox);
        addAndMakeVisible (paramBox);
        addAndMakeVisible (touchedBtn);
        targetBox.onChange = [this] { fillParams(); };
        paramBox.onChange  = [this] { updateButtons(); };
        touchedBtn.onClick = [this] { useTouched(); };
        touchedBtn.setTooltip ("Select the parameter you last moved in a plugin GUI");

        // Editor row 2: source + range
        addAndMakeVisible (learnBtn);
        addAndMakeVisible (sourceLabel);
        addAndMakeVisible (minSlider);
        addAndMakeVisible (maxSlider);
        addAndMakeVisible (passToggle);
        learnBtn.setClickingTogglesState (true);
        learnBtn.setColour (TextButton::buttonOnColourId, Colours::orangered);
        learnBtn.onClick = [this] { engine.setLearnArmed (learnBtn.getToggleState()); if (learnBtn.getToggleState()) owner.showStatus ("Move a MIDI controller..."); };
        sourceLabel.setColour (Label::textColourId, accent);
        sourceLabel.setFont (FontOptions (13.0f, Font::bold));
        for (auto* s : { &minSlider, &maxSlider })
        {
            s->setRange (0.0, 1.0, 0.01);
            s->setSliderStyle (Slider::LinearBar);
        }
        minSlider.setValue (0.0, dontSendNotification);
        maxSlider.setValue (1.0, dontSendNotification);
        minSlider.setTooltip ("Parameter value at controller minimum");
        maxSlider.setTooltip ("Parameter value at controller maximum");
        passToggle.setTooltip ("Also send the raw MIDI message to the plugins");

        // Editor row 3: actions
        addAndMakeVisible (addBtn);
        addAndMakeVisible (updateBtn);
        addAndMakeVisible (removeBtn);
        addAndMakeVisible (suggestBtn);
        addAndMakeVisible (templateBtn);
        suggestBtn.setTooltip ("Propose mappings for the selected target: from its saved template, or from parameter names matched to standard MIDI controller numbers");
        templateBtn.setTooltip ("Save the selected target's current mappings as the default template for this plugin");
        suggestBtn.onClick  = [this] { showSuggestions(); };
        templateBtn.onClick = [this] { saveTemplate(); };
        addBtn.onClick    = [this] { if (auto m = makeMapping()) engine.addMapping (owner.getSelectedInput(), owner.getEditedProgram(), *m); };
        updateBtn.onClick = [this] { int r = table.getSelectedRow(); if (r >= 0) if (auto m = makeMapping()) engine.updateMapping (owner.getSelectedInput(), owner.getEditedProgram(), r, *m); };
        removeBtn.onClick = [this] { int r = table.getSelectedRow(); if (r >= 0) engine.removeMapping (owner.getSelectedInput(), owner.getEditedProgram(), r); };

        /* Min, max and pass-through apply to the selected mapping as they are
           changed. They used to take effect only on Update, and nothing said
           so: a pass-through ticked and never pressed home left the CC consumed
           by its mapping and never reaching the plugin, which looked like the
           plugin ignoring the controller. Source and target still go through
           Update, since changing those is making a different mapping. */
        auto applyEdit = [this]
        {
            const int r = table.getSelectedRow();
            const auto* def = currentProgram();
            if (r < 0 || def == nullptr || r >= (int) def->mappings.size()) return;
            auto m = def->mappings[(size_t) r];
            m.minValue    = (float) minSlider.getValue();
            m.maxValue    = (float) maxSlider.getValue();
            m.passThrough = passToggle.getToggleState();
            engine.updateMapping (owner.getSelectedInput(), owner.getEditedProgram(), r, m);
            owner.markDirty();
        };
        minSlider.onValueChange = applyEdit;
        maxSlider.onValueChange = applyEdit;
        passToggle.onClick      = applyEdit;
    }

    void refresh()
    {
        Target prev;
        bool hadPrev = false;
        if (auto* t = selectedTarget()) { prev = *t; hadPrev = true; }

        targets.clear();
        targetBox.clear (dontSendNotification);
        if (const auto* def = currentProgram())
        {
            for (int s = 0; s < (int) def->slots.size(); ++s)
            {
                if (! def->slots[(size_t) s].isExternal())     // no parameters to map on a hardware synth
                    targets.push_back ({ s, -1, String (s + 1) + ": " + def->slots[(size_t) s].plugin.name });
                for (int e = 0; e < (int) def->slots[(size_t) s].effects.size(); ++e)
                    targets.push_back ({ s, e, String (s + 1) + " > FX" + String (e + 1) + ": " + def->slots[(size_t) s].effects[(size_t) e].plugin.name });
            }
            for (int e = 0; e < (int) def->effects.size(); ++e)
                targets.push_back ({ -1, e, "Program FX" + String (e + 1) + ": " + def->effects[(size_t) e].plugin.name });
        }
        for (int i = 0; i < (int) targets.size(); ++i)
            targetBox.addItem (targets[(size_t) i].label, i + 1);

        int reselect = targets.empty() ? 0 : 1;
        if (hadPrev)
            for (int i = 0; i < (int) targets.size(); ++i)
                if (targets[(size_t) i].slot == prev.slot && targets[(size_t) i].effect == prev.effect) reselect = i + 1;
        if (reselect > 0) targetBox.setSelectedId (reselect, dontSendNotification);

        fillParams();
        table.updateContent();
        table.repaint();
        updateButtons();
    }

    void onLearn (MappingDef::Source s, int number)
    {
        learnedSource = s;
        learnedNumber = number;
        haveSource = true;
        learnBtn.setToggleState (false, dontSendNotification);
        MappingDef tmp; tmp.source = s; tmp.number = number;
        sourceLabel.setText (tmp.sourceDescription(), dontSendNotification);
        owner.showStatus ("Learned " + tmp.sourceDescription() + ". Pick a parameter and click Add.");
        updateButtons();
    }

    void onTouched (int slot, int effect, int paramIndex)
    {
        touchedSlot = slot; touchedEffect = effect; touchedParam = paramIndex;
        touchedBtn.setEnabled (true);
        if (auto* plugin = engine.getPlugin (owner.getSelectedInput(), owner.getEditedProgram(), slot, effect))
            for (auto& p : plugin->getParameters())
                if (p.index == paramIndex)
                    touchedBtn.setButtonText ("Use touched: " + p.name.substring (0, 24));
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        header.setBounds (r.removeFromTop (22));
        auto editor = r.removeFromBottom (3 * 26 + 8);
        r.removeFromBottom (4);
        table.setBounds (r);

        auto row1 = editor.removeFromTop (24);
        targetBox.setBounds (row1.removeFromLeft (220)); row1.removeFromLeft (4);
        touchedBtn.setBounds (row1.removeFromRight (200)); row1.removeFromRight (4);
        paramBox.setBounds (row1);
        editor.removeFromTop (4);

        auto row2 = editor.removeFromTop (24);
        learnBtn.setBounds (row2.removeFromLeft (100)); row2.removeFromLeft (4);
        sourceLabel.setBounds (row2.removeFromLeft (100)); row2.removeFromLeft (4);
        passToggle.setBounds (row2.removeFromRight (140));
        auto half = row2.getWidth() / 2;
        minSlider.setBounds (row2.removeFromLeft (half).reduced (2, 0));
        maxSlider.setBounds (row2.reduced (2, 0));
        editor.removeFromTop (4);

        auto row3 = editor.removeFromTop (24);
        addBtn.setBounds (row3.removeFromLeft (100)); row3.removeFromLeft (4);
        updateBtn.setBounds (row3.removeFromLeft (100)); row3.removeFromLeft (4);
        removeBtn.setBounds (row3.removeFromLeft (100));
        templateBtn.setBounds (row3.removeFromRight (120)); row3.removeFromRight (4);
        suggestBtn.setBounds (row3.removeFromRight (110));
    }

    // TableListBoxModel
    int getNumRows() override { auto* d = currentProgram(); return d ? (int) d->mappings.size() : 0; }

    void paintRowBackground (Graphics& g, int row, int, int, bool selected) override
    {
        g.fillAll (selected ? accentDim : (row % 2 ? bgRow : bgPanel));
    }

    void paintCell (Graphics& g, int row, int col, int w, int h, bool) override
    {
        auto* def = currentProgram();
        if (def == nullptr || row < 0 || row >= (int) def->mappings.size()) return;
        const auto& m = def->mappings[(size_t) row];
        String text;
        switch (col)
        {
            case colSource: text = m.sourceDescription(); break;
            case colTarget: text = targetLabel (m.slot, m.effect); break;
            case colParam:  text = m.paramName.isNotEmpty() ? m.paramName : m.paramId; break;
            case colMin:    text = String (m.minValue, 2); break;
            case colMax:    text = String (m.maxValue, 2); break;
            case colPass:   text = m.passThrough ? "yes" : "-"; break;
            default: break;
        }
        g.setColour (Colours::white);
        g.setFont (FontOptions (13.0f));
        g.drawText (text, 4, 0, w - 8, h, Justification::centredLeft, true);
    }

    void selectedRowsChanged (int row) override
    {
        auto* def = currentProgram();
        if (def == nullptr || row < 0 || row >= (int) def->mappings.size()) { updateButtons(); return; }
        const auto& m = def->mappings[(size_t) row];
        learnedSource = m.source; learnedNumber = m.number; haveSource = true;
        sourceLabel.setText (m.sourceDescription(), dontSendNotification);
        for (int i = 0; i < (int) targets.size(); ++i)
            if (targets[(size_t) i].slot == m.slot && targets[(size_t) i].effect == m.effect)
                targetBox.setSelectedId (i + 1, dontSendNotification);
        fillParams();
        for (int i = 0; i < paramIds.size(); ++i)
            if (paramIds[i] == m.paramId) { paramBox.setSelectedId (i + 1, dontSendNotification); break; }
        minSlider.setValue (m.minValue, dontSendNotification);
        maxSlider.setValue (m.maxValue, dontSendNotification);
        passToggle.setToggleState (m.passThrough, dontSendNotification);
        updateButtons();
    }

private:
    /** MIDI channel the target plugin receives on: the slot's forced channel if set,
        else the input's channel (0 = omni / unknown). */
    int channelInto (const Target& t) const
    {
        const auto& inputs = engine.getSetup().inputs;
        const int in = owner.getSelectedInput();
        int ch = (in >= 0 && in < (int) inputs.size()) ? inputs[(size_t) in].channel : 0;
        if (auto* def = currentProgram(); def != nullptr && t.slot >= 0 && t.slot < (int) def->slots.size()
                                           && def->slots[(size_t) t.slot].outChannel > 0)
            ch = def->slots[(size_t) t.slot].outChannel;
        return ch;
    }

    const ProgramDef* currentProgram() const
    {
        const auto& inputs = engine.getSetup().inputs;
        const int in = owner.getSelectedInput();
        if (in < 0 || in >= (int) inputs.size()) return nullptr;
        return &inputs[(size_t) in].programs[(size_t) owner.getEditedProgram()];
    }

    String targetLabel (int slot, int effect) const
    {
        for (auto& t : targets)
            if (t.slot == slot && t.effect == effect) return t.label;
        if (slot < 0) return "Program FX" + String (effect + 1);
        return String (slot + 1) + (effect >= 0 ? " > FX" + String (effect + 1) : String());
    }

    const Target* selectedTarget() const
    {
        const int i = targetBox.getSelectedId() - 1;
        return (i >= 0 && i < (int) targets.size()) ? &targets[(size_t) i] : nullptr;
    }

    void fillParams()
    {
        const String prev = paramBox.getSelectedId() > 0 && paramBox.getSelectedId() <= paramIds.size()
                                ? paramIds[paramBox.getSelectedId() - 1] : String();
        paramBox.clear (dontSendNotification);
        paramIds.clear();
        auto* t = selectedTarget();
        auto* plugin = t != nullptr ? engine.getPlugin (owner.getSelectedInput(), owner.getEditedProgram(), t->slot, t->effect) : nullptr;
        if (plugin == nullptr)
        {
            paramBox.setTextWhenNothingSelected (t != nullptr ? "(plugin not loaded)" : "(no plugin)");
            updateButtons();
            return;
        }
        paramBox.setTextWhenNothingSelected ("Choose parameter...");
        paramBox.setPreferredChannel (channelInto (*t));
        int id = 1, reselect = 0;
        paramIndices.clear();
        for (auto& p : plugin->getParameters())
        {
            paramBox.addItem (p, id);
            paramIds.add (p.id);
            paramIndices.add (p.index);
            if (paramIds[paramIds.size() - 1] == prev) reselect = id;
            ++id;
        }
        if (reselect > 0) paramBox.setSelectedId (reselect, dontSendNotification);
        updateButtons();
    }

    void useTouched()
    {
        if (touchedParam < 0) return;
        for (int i = 0; i < (int) targets.size(); ++i)
            if (targets[(size_t) i].slot == touchedSlot && targets[(size_t) i].effect == touchedEffect)
                targetBox.setSelectedId (i + 1, dontSendNotification);
        fillParams();
        const int pos = paramIndices.indexOf (touchedParam);
        if (pos >= 0)
            paramBox.setSelectedId (pos + 1, dontSendNotification);
        updateButtons();
    }

    std::optional<MappingDef> makeMapping()
    {
        auto* t = selectedTarget();
        const int pi = paramBox.getSelectedId() - 1;
        if (! haveSource) { owner.showStatus ("Click Learn MIDI and move a controller first."); return {}; }
        if (t == nullptr || pi < 0 || pi >= paramIds.size()) { owner.showStatus ("Choose a target plugin and a parameter."); return {}; }
        MappingDef m;
        m.source = learnedSource; m.number = learnedNumber;
        m.slot = t->slot; m.effect = t->effect; m.paramId = paramIds[pi];
        m.paramName = paramBox.getItemText (pi);
        m.minValue = (float) minSlider.getValue();
        m.maxValue = (float) maxSlider.getValue();
        m.passThrough = passToggle.getToggleState();
        return m;
    }

    void updateButtons()
    {
        const bool canMake = haveSource && targetBox.getSelectedId() > 0 && paramBox.getSelectedId() > 0;
        addBtn.setEnabled (canMake);
        updateBtn.setEnabled (canMake && table.getSelectedRow() >= 0);
        removeBtn.setEnabled (table.getSelectedRow() >= 0);
        touchedBtn.setEnabled (touchedParam >= 0);

        auto* t = selectedTarget();
        const bool loaded = t != nullptr && engine.getPlugin (owner.getSelectedInput(), owner.getEditedProgram(), t->slot, t->effect) != nullptr;
        suggestBtn.setEnabled (loaded);
        int mappedHere = 0;
        if (auto* def = currentProgram(); def != nullptr && t != nullptr)
            for (auto& m : def->mappings) if (m.slot == t->slot && m.effect == t->effect) ++mappedHere;
        templateBtn.setEnabled (loaded && mappedHere > 0);
    }

    const PluginDescription* targetDescription (const Target& t) const
    {
        auto* def = currentProgram();
        if (def == nullptr) return nullptr;
        if (t.effect < 0) return (t.slot >= 0 && t.slot < (int) def->slots.size()) ? &def->slots[(size_t) t.slot].plugin : nullptr;
        auto* chain = def->chainFor (t.slot);
        return (chain != nullptr && t.effect < (int) chain->size()) ? &(*chain)[(size_t) t.effect].plugin : nullptr;
    }

    void showSuggestions()
    {
        auto* t = selectedTarget();
        auto* def = currentProgram();
        if (t == nullptr || def == nullptr) return;
        auto* plugin = engine.getPlugin (owner.getSelectedInput(), owner.getEditedProgram(), t->slot, t->effect);
        auto* desc = targetDescription (*t);
        if (plugin == nullptr || desc == nullptr) { owner.showStatus ("Plugin is not loaded"); return; }

        auto suggestions = suggestMappings (plugin->getParameters(), *desc, engine.getPluginHost().getMappingTemplates(), t->slot, t->effect, def->mappings);
        auto* content = new SuggestionsComponent (engine, owner.getSelectedInput(), owner.getEditedProgram(), t->label, std::move (suggestions));
        DialogWindow::LaunchOptions o;
        o.content.setOwned (content);
        o.dialogTitle = "Suggested mappings";
        o.dialogBackgroundColour = bgPanel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    void saveTemplate()
    {
        auto* t = selectedTarget();
        auto* def = currentProgram();
        if (t == nullptr || def == nullptr) return;
        auto* desc = targetDescription (*t);
        if (desc == nullptr) return;
        std::vector<MappingDef> mine;
        for (auto& m : def->mappings)
            if (m.slot == t->slot && m.effect == t->effect) mine.push_back (m);
        if (mine.empty()) { owner.showStatus ("No mappings on this target to save"); return; }
        engine.getPluginHost().getMappingTemplates().set (*desc, mine);
        owner.showStatus ("Saved " + String ((int) mine.size()) + " mappings as the template for " + desc->name + ". Suggest... will offer them for any " + desc->name + ".");
    }

    Engine& engine;
    MainComponent& owner;
    Label header { {}, "MIDI MAPPINGS" };
    TableListBox table;
    ComboBox targetBox;
    ParamPicker paramBox;
    std::vector<Target> targets;
    StringArray paramIds;
    Array<int> paramIndices;
    TextButton touchedBtn { "Use touched parameter" }, learnBtn { "Learn MIDI" }, addBtn { "Add" }, updateBtn { "Update" }, removeBtn { "Remove" },
               suggestBtn { "Suggest..." }, templateBtn { "Save template" };
    Label sourceLabel { {}, "-" };
    Slider minSlider, maxSlider;
    ToggleButton passToggle { "Pass through" };
    MappingDef::Source learnedSource = MappingDef::Source::CC;
    int learnedNumber = 1;
    bool haveSource = false;
    int touchedSlot = -1, touchedEffect = -1, touchedParam = -1;
};

//==============================================================================
//  MainComponent
//==============================================================================
bool MainComponent::looksLikeAraPlugin (const PluginDescription& d)
{
    // JUCE doesn't expose the OnlyARA sub-category, so fall back to the name.
    return d.name.containsWholeWordIgnoreCase ("ARA") || d.category.containsIgnoreCase ("ARA");
}

MainComponent::MainComponent (Engine& e, PropertiesFile& s, const File& initialSetup) : engine (e), settings (s)
{
    inputsPanel   = std::make_unique<InputsPanel> (engine, *this);
    programsPanel = std::make_unique<ProgramsPanel> (engine, *this);
    slotsPanel    = std::make_unique<SlotsPanel> (engine, *this);
    mappingsPanel = std::make_unique<MappingsPanel> (engine, *this);
    for (auto* c : std::initializer_list<Component*> { inputsPanel.get(), programsPanel.get(), slotsPanel.get(), mappingsPanel.get() })
        addAndMakeVisible (c);

    for (auto* b : { &newBtn, &openBtn, &saveBtn, &saveAsBtn, &audioBtn, &pluginsBtn, &panicBtn })
        addAndMakeVisible (b);
    newBtn.onClick         = [this] { newSetup(); };
    openBtn.onClick        = [this] { openSetup(); };
    saveBtn.onClick        = [this] { saveSetup (false); };
    saveAsBtn.onClick      = [this] { saveSetup (true); };
    audioBtn.onClick       = [this] { showAudioSettings(); };
    pluginsBtn.onClick     = [this] { showPluginManager(); };
    panicBtn.onClick       = [this] { engine.panic(); showStatus ("Panic: all notes off"); };
    panicBtn.setColour (TextButton::buttonColourId, Colours::darkred);

    /* What is set up once and then left alone -- the two switches and the two
       occasional actions -- lives in a menu. As toolbar buttons they did not
       fit a 1280-wide window: the tempo readout was squashed to nothing and
       the preload checkbox hidden, on exactly the laptop this runs on. */
    addAndMakeVisible (optionsBtn);
    optionsBtn.setTooltip ("Preload all programs, hold notes across program changes, rescan MIDI, print the program map");
    optionsBtn.onClick = [this]
    {
        const auto& setup = engine.getSetup();
        PopupMenu m;
        m.addItem (1, "Preload all programs", true, setup.preloadAllPrograms);
        m.addItem (2, "Hold notes across program changes", true, setup.holdAcrossChange);
        m.addItem (6, "Show songs and set lists", true, songsPanel != nullptr && songsPanel->isVisible());
        m.addSeparator();
        m.addItem (5, "Outputs & master effects...");
        m.addItem (3, "Rescan MIDI devices");
        m.addItem (4, "Print program map...");
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (optionsBtn), [this] (int r)
        {
            const auto& s = engine.getSetup();
            switch (r)
            {
                case 1: engine.setPreloadAllPrograms (! s.preloadAllPrograms); break;
                case 2: engine.setHoldAcrossChange (! s.holdAcrossChange); break;
                case 3: engine.refreshMidiDevices(); showStatus ("MIDI devices rescanned"); break;
                case 4: printProgramMap(); break;
                case 5: showOutputs(); break;
                case 6: setSongsPanelShown (songsPanel == nullptr || ! songsPanel->isVisible()); break;
                default: break;
            }
        });
    };

    keyboardPanel = std::make_unique<KeyboardPanel> (engine);
    addChildComponent (keyboardPanel.get());
    addAndMakeVisible (keyboardBtn);
    keyboardBtn.setClickingTogglesState (true);
    keyboardBtn.setColour (TextButton::buttonOnColourId, accentDim);
    keyboardBtn.setTooltip ("Show an on-screen keyboard and controllers that play into the selected input");
    keyboardBtn.onClick = [this]
    {
        const bool show = keyboardBtn.getToggleState();
        if (! show) keyboardPanel->releaseAll();
        keyboardPanel->setVisible (show);
        settings.setValue ("showKeyboard", show);
        resized();
    };
    keyboardBtn.setToggleState (settings.getBoolValue ("showKeyboard", false), dontSendNotification);
    keyboardPanel->setVisible (keyboardBtn.getToggleState());

    // Stage display: the active program per input, big enough to read from a stand.
    stagePanel = std::make_unique<StagePanel> (engine);
    addChildComponent (stagePanel.get());
    addAndMakeVisible (stageBtn);
    stageBtn.setClickingTogglesState (true);
    stageBtn.setColour (TextButton::buttonOnColourId, accentDim);
    stageBtn.setTooltip ("Show the current program of each input in large type, for reading while you play");
    stageBtn.onClick = [this]
    {
        const bool show = stageBtn.getToggleState();
        stagePanel->setVisible (show);
        settings.setValue ("showStage", show);
        resized();
    };
    stageBtn.setToggleState (settings.getBoolValue ("showStage", false), dontSendNotification);
    stagePanel->setVisible (stageBtn.getToggleState());

    /* Songs and sets. Hidden by default and remembered, like Stage: it is the
       panel you want while building a set and while playing one, and in the way
       the rest of the time. */
    songsPanel = std::make_unique<SongsPanel> (engine);

    // Pane dividers: drag to give a pane more room; remembered across runs.
    songsWidth    = settings.getIntValue ("paneSongsWidth",    SongsPanel::preferredWidth);
    inputsWidth   = settings.getIntValue ("paneInputsWidth",   260);
    programsWidth = settings.getIntValue ("paneProgramsWidth", 260);
    for (auto* d : { &songsDivider, &inputsDivider, &programsDivider })
    {
        addAndMakeVisible (d);
        /* A drag works on what is on screen. After the window shrank, the
           stored widths may be wider than the panes could be shown; dragging
           from there changed a number nothing reflected, and the dividers
           seemed stuck until the stored width had come back under the
           limit. Start from the shown widths instead. */
        d->onBegin = [this]
        {
            if (songsPanel != nullptr && songsPanel->isVisible()) songsWidth = songsPanel->getWidth();
            inputsWidth   = inputsPanel->getWidth();
            programsWidth = programsPanel->getWidth();
        };
        d->onRelease = [this] { savePaneWidths(); };
    }
    // Clamped as dragged, so a width that cannot be shown is never stored.
    songsDivider.onDrag    = [this] (int dx)
    {
        /* Dragged well past its minimum, the set list folds away -- the one
           gesture for "I do not use this" -- and comes back from Options at
           a usable width. */
        if (songsWidth + dx < SongsPanel::minimumWidth - 50) { songsWidth = SongsPanel::preferredWidth; setSongsPanelShown (false, true); return; }
        songsWidth = jlimit (SongsPanel::minimumWidth, 900, songsWidth + dx);
        resized();
    };
    inputsDivider.onDrag   = [this] (int dx) { inputsWidth   = jlimit (200, 900, inputsWidth + dx);   resized(); };
    programsDivider.onDrag = [this] (int dx) { programsWidth = jlimit (200, 900, programsWidth + dx); resized(); };
    addChildComponent (songsPanel.get());
    songsPanel->setVisible (settings.getBoolValue ("showSongs", false));


    addAndMakeVisible (helpBtn);
    helpBtn.setTooltip ("The user manual, without leaving the app");
    helpBtn.onClick = [] { HelpWindow::show(); };

    addAndMakeVisible (remoteBtn);
    remoteBtn.setClickingTogglesState (true);
    remoteBtn.setColour (TextButton::buttonOnColourId, accentDim);
    remoteBtn.setTooltip ("Select programs from a tablet on the same network");
    remoteBtn.onClick = [this] { showRemote(); };

    /* Tempo. Tapping is the point -- between songs you set it by ear -- but a
       tempo you already know is faster typed than tapped, so the reading is
       also the text box: click it and type. Right-click still has the tap
       controller.

       One click, not two: on stage, a number you have to double-click is a
       number you cannot change. */
    addAndMakeVisible (tempoLabel);
    tempoLabel.setColour (Label::textColourId, Colours::white);
    tempoLabel.setJustificationType (Justification::centredRight);
    tempoLabel.setTooltip ("The tempo every plugin is told. Click to type one; right-click to assign a footswitch.");
    tempoLabel.setEditable (true, true, false);

    /* Editing shows a box round the number, so it is obvious it can be typed
       in. Until then it reads as a plain label, which is what it was. */
    tempoLabel.setColour (Label::outlineWhenEditingColourId, accent);

    tempoLabel.onEditorShow = [this]
    {
        /* Just the number while editing: "120.0 bpm" would have to be retyped
           in full, and the unit is not in question. */
        if (auto* ed = tempoLabel.getCurrentTextEditor())
        {
            ed->setText (String (engine.getTempoBpm(), 1), false);
            ed->setJustification (Justification::centredRight);
            ed->setInputRestrictions (6, "0123456789.");
            ed->selectAll();
        }
    };

    tempoLabel.onTextChange = [this]
    {
        const auto typed = tempoLabel.getText().retainCharacters ("0123456789.").getDoubleValue();
        if (typed <= 0.0)
        {
            // Not a tempo: put the old one back rather than leaving a blank.
            showStatus ("That is not a tempo.");
            updateTempoLabel();
            return;
        }

        engine.setTempoBpm (typed);
        engine.resetTapTempo();          // a typed tempo ends the tapping
        updateTempoLabel();              // shows what the engine clamped it to
        showStatus ("Tempo " + String (engine.getTempoBpm(), 1) + " bpm");
        markDirty();
    };

    /* The tempo lives in the setup, and someone who types one at a music stand
       has no way to reach Ctrl+S -- same reason a program picked from the
       tablet is saved promptly. */

    addAndMakeVisible (tapButton);
    tapButton.setTooltip ("Tap four times in time. Click the number to type a tempo; right-click for a footswitch.");
    tapButton.onClick = [this]
    {
        // A right-click arrives here too; treat it as "configure", not a tap,
        // so someone reaching for the menu does not nudge the tempo.
        if (ModifierKeys::getCurrentModifiers().isPopupMenu()) { showTempoMenu(); return; }
        if (const double bpm = engine.tapTempo(); bpm > 0.0)
            showStatus ("Tempo " + String (bpm, 1) + " bpm");
        updateTempoLabel();
    };

    addAndMakeVisible (tailLabel);
    tailLabel.setColour (Label::textColourId, textDim);
    addAndMakeVisible (tailSlider);
    tailSlider.setRange (0.0, 20.0, 0.5);
    tailSlider.setSliderStyle (Slider::LinearBar);
    tailSlider.setTextValueSuffix (" s");
    tailSlider.setTooltip ("How long the previous program keeps sounding after a program change");
    tailSlider.onValueChange = [this] { engine.setReleaseTailSeconds (tailSlider.getValue()); };

    for (auto* l : { &statusLabel, &cpuLabel, &fileLabel })
    {
        addAndMakeVisible (l);
        l->setFont (FontOptions (12.0f));
        l->setColour (Label::textColourId, textDim);
    }
    cpuLabel.setJustificationType (Justification::centredRight);
    fileLabel.setJustificationType (Justification::centredRight);

    engine.addListener (this);

    // Open the requested setup, else restore the last one.
    File last (settings.getValue ("lastSetup"));
    if (initialSetup.existsAsFile())            loadSetupFile (initialSetup);
    else if (last.existsAsFile())               loadSetupFile (last);
    else if (getAutosaveFile().existsAsFile())  loadSetupFile (getAutosaveFile());
    else                                        refreshAll();

    /* Restore tablet control if it was on when we last quit. Someone who set up a
       tablet on a stand expects it to still work after a restart, and discovering
       otherwise mid-set is exactly when they can least afford to go and fix it.
       Started after the setup is loaded so the page has programs to show. */
    if (settings.getBoolValue ("remoteOn", false))
    {
        remoteBtn.setToggleState (true, dontSendNotification);
        if (startRemote())
            showStatus ("Tablet control on: " + remote->getUrl() + "  code " + remote->getToken());
    }

    updateTempoLabel();
    startTimerHz (4);
    setSize (1280, 800);
}

MainComponent::~MainComponent()
{
    engine.removeListener (this);
}

int MainComponent::getEditedProgram() const
{
    const auto& inputs = engine.getSetup().inputs;
    if (selectedInput < 0 || selectedInput >= (int) inputs.size()) return 0;
    return inputs[(size_t) selectedInput].currentProgram;
}

void MainComponent::paint (Graphics& g)
{
    g.fillAll (bgDark);
    auto r = getLocalBounds();
    r.removeFromTop (44);
    r.removeFromBottom (24);
    g.setColour (bgPanel);
    g.fillRect (inputsPanel->getBounds());
    g.fillRect (programsPanel->getBounds());
    g.fillRect (slotsPanel->getBounds());
    g.fillRect (mappingsPanel->getBounds());
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    auto toolbar = r.removeFromTop (44).reduced (8, 8);
    for (auto* b : { &newBtn, &openBtn, &saveBtn, &saveAsBtn })
    {
        b->setBounds (toolbar.removeFromLeft (72));
        toolbar.removeFromLeft (3);
    }
    toolbar.removeFromLeft (10);
    remoteBtn.setBounds (toolbar.removeFromLeft (66));      toolbar.removeFromLeft (8);
    helpBtn.setBounds (toolbar.removeFromLeft (52));        toolbar.removeFromLeft (8);
    audioBtn.setBounds (toolbar.removeFromLeft (74));       toolbar.removeFromLeft (4);
    pluginsBtn.setBounds (toolbar.removeFromLeft (74));     toolbar.removeFromLeft (4);
    optionsBtn.setBounds (toolbar.removeFromLeft (70));     toolbar.removeFromLeft (8);
    panicBtn.setBounds (toolbar.removeFromRight (80));      toolbar.removeFromRight (6);
    keyboardBtn.setBounds (toolbar.removeFromRight (78));   toolbar.removeFromRight (4);
    stageBtn.setBounds (toolbar.removeFromRight (72));      toolbar.removeFromRight (4);
    tailSlider.setBounds (toolbar.removeFromRight (60));    toolbar.removeFromRight (2);
    tailLabel.setBounds (toolbar.removeFromRight (26));     toolbar.removeFromRight (8);
    tapButton.setBounds (toolbar.removeFromRight (42));     toolbar.removeFromRight (2);
    tempoLabel.setBounds (toolbar.removeFromRight (66));    toolbar.removeFromRight (8);

    auto status = r.removeFromBottom (24).reduced (8, 2);
    cpuLabel.setBounds (status.removeFromRight (470));
    fileLabel.setBounds (status.removeFromRight (320));
    statusLabel.setBounds (status);

    if (keyboardPanel != nullptr && keyboardPanel->isVisible())
    {
        keyboardPanel->setBounds (r.removeFromBottom (KeyboardPanel::preferredHeight).reduced (8, 0));
        r.removeFromBottom (8);
    }
    if (stagePanel != nullptr && stagePanel->isVisible())
    {
        stagePanel->setBounds (r.removeFromTop (StagePanel::preferredHeight).reduced (8, 0));
        r.removeFromTop (8);
    }

    r.reduce (8, 0);
    /* The dividers live in the 8 px gaps between the panes. Each pane has a
       width the player chose by dragging, within limits: the right side keeps
       enough for the mappings table, whose columns collapse into "0..." and
       "1..." when squeezed, and when the window cannot hold it all the songs
       panel yields first -- a set list is readable much narrower than that
       table is usable, and below its minimum it hides entirely until the
       toggle turns it back on. */
    /* The dragged widths are preferences, kept as dragged: when the window is
       too narrow for them the panes are squeezed for this layout only --
       programs first, then inputs, then the set list, each down to its
       minimum -- and come back as the window grows. The right side keeps room
       for the mappings table. At the window's minimum width everything fits
       at its minimum, so nothing ever has to be hidden; a pane that vanished
       when the window shrank, with nothing to bring it back, was worse than
       a squeezed one. */
    const int gap = 8, rightMin = 420, paneMin = 200, paneMax = 900;
    const bool songsShown = songsPanel != nullptr && songsPanel->isVisible();
    int songsW    = songsShown ? jlimit (SongsPanel::minimumWidth, paneMax, songsWidth) : 0;
    int inputsW   = jlimit (paneMin, paneMax, inputsWidth);
    int programsW = jlimit (paneMin, paneMax, programsWidth);

    int over = (songsW + (songsShown ? gap : 0) + inputsW + gap + programsW + gap + rightMin) - r.getWidth();
    if (over > 0) { const int cut = jmin (over, programsW - paneMin); programsW -= cut; over -= cut; }
    if (over > 0) { const int cut = jmin (over, inputsW - paneMin);   inputsW   -= cut; over -= cut; }
    if (over > 0 && songsShown) { const int cut = jmin (over, songsW - SongsPanel::minimumWidth); songsW -= cut; }

    songsDivider.setVisible (songsShown);
    if (songsShown)
    {
        songsPanel->setBounds (r.removeFromLeft (songsW));
        songsDivider.setBounds (r.removeFromLeft (gap));
    }

    inputsPanel->setBounds (r.removeFromLeft (inputsW));
    inputsDivider.setBounds (r.removeFromLeft (gap));
    programsPanel->setBounds (r.removeFromLeft (programsW));
    programsDivider.setBounds (r.removeFromLeft (gap));
    mappingsPanel->setBounds (r.removeFromBottom (jmax (200, r.getHeight() * 40 / 100)));
    r.removeFromBottom (8);
    slotsPanel->setBounds (r);
}

void MainComponent::PaneDivider::paint (Graphics& g)
{
    // A quiet grip: three dots in the gap, brighter under the mouse.
    const auto c = (isMouseOver() || isMouseButtonDown()) ? accent : textDim.withAlpha (0.45f);
    g.setColour (c);
    const float x = getWidth() * 0.5f;
    const float y = getHeight() * 0.5f;
    for (int i = -1; i <= 1; ++i)
        g.fillEllipse (x - 1.5f, y + 7.0f * (float) i - 1.5f, 3.0f, 3.0f);
}

void MainComponent::setSongsPanelShown (bool show, bool byDrag)
{
    if (songsPanel == nullptr) return;
    songsPanel->setVisible (show);
    settings.setValue ("showSongs", show);
    settings.saveIfNeeded();
    resized();
    if (byDrag)
        showStatus ("Songs and set lists hidden. Options > Show songs and set lists brings the panel back.");
}

void MainComponent::savePaneWidths()
{
    settings.setValue ("paneSongsWidth",    songsWidth);
    settings.setValue ("paneInputsWidth",   inputsWidth);
    settings.setValue ("paneProgramsWidth", programsWidth);
    settings.saveIfNeeded();
}

void MainComponent::timerCallback()
{
    PluginView::dropDeadSessions();      // a mirrored window that was closed ends its session

    const auto now = Time::getMillisecondCounterHiRes();
    if (keyboardPanel != nullptr && keyboardPanel->isVisible()) keyboardPanel->refreshLabel();   // input name/channel may have been edited
    {
        String dev;
        if (auto* d = engine.getDeviceManager().getCurrentAudioDevice())
        {
            const int smp = engine.getLastBlockSize() > 0 ? engine.getLastBlockSize() : d->getCurrentBufferSizeSamples();
            dev = String (d->getCurrentSampleRate() / 1000.0, 1) + " kHz / " + String (smp) + " smp ("
                  + String (1000.0 * smp / d->getCurrentSampleRate(), 1) + " ms)";
        }
        else dev = "no audio device";
        const int wanted = settings.getBoolValue ("pipewireTakeover", true) ? settings.getIntValue ("pipewireQuantum", 128) : 0;
        const bool clockHeld = wanted > 0 && engine.getLastBlockSize() > wanted
                               && engine.getDeviceManager().getCurrentAudioDevice() != nullptr
                               && engine.getDeviceManager().getCurrentAudioDevice()->getTypeName() == "JACK";
        cpuLabel.setText ("CPU " + String (engine.getCpuUsage() * 100.0, 1) + "%  |  " + dev
                          + (clockHeld ? "  |  clock held by another app" : String())
                          + (engine.getLateBlockCount() > 0 ? "  |  late " + String (engine.getLateBlockCount()) : String()), dontSendNotification);
        cpuLabel.setTooltip (clockHeld ? "StageRig asked PipeWire for " + String (wanted) + " samples but the graph runs bigger blocks: another application (e.g. Bitwig) is forcing the PipeWire clock. Close it, or set its block size to " + String (wanted) + "." : String());
        cpuLabel.setColour (Label::textColourId, (engine.getLateBlockCount() > 0 || clockHeld) ? Colours::orange : textDim);
    }
    if (statusText.isNotEmpty() && now - statusTime > 8000.0)
    {
        statusText.clear();
        statusLabel.setText ({}, dontSendNotification);
    }

    /* Periodic autosave, so a crash or a frozen plugin doesn't cost the session.

       This also writes the open setup file, not just the recovery copy. Picking
       a program is a real change to the setup, and someone who picks one from a
       tablet mid-set has no way to reach Ctrl+S -- if the machine is then killed
       rather than quit, saveOnQuit() never runs and the choice is silently gone.
       Writing the file the user actually opened is what they expect "it saved"
       to mean. */
    if (dirty && now - lastAutosaveTime > autosaveIntervalMs)
    {
        lastAutosaveTime = now;
        dirty = false;

        /* Only the plugins that may have changed, and within a budget: this
           runs mid-set, and a plugin that stalls its audio to serialise, or a
           host that has stopped answering, must not take the music with it. */
        const auto captured = engine.captureSetup (5000, true);
        const bool ok = captured.saveToFile (getAutosaveFile()).wasOk();

        if (currentFile != File() && currentFile.existsAsFile())
            captured.saveToFile (currentFile);

        if (ok)
            fileLabel.setText ((currentFile == File() ? String ("(unsaved setup)") : currentFile.getFullPathName())
                                   + "   autosaved " + Time::getCurrentTime().toString (false, true, false), dontSendNotification);
    }
}

void MainComponent::showStatus (const String& s)
{
    statusText = s;
    statusTime = Time::getMillisecondCounterHiRes();
    statusLabel.setText (s, dontSendNotification);
}

//==============================================================================
void MainComponent::selectInput (int idx)
{
    const auto& inputs = engine.getSetup().inputs;
    selectedInput = jlimit (0, jmax (0, (int) inputs.size() - 1), idx);
    if (keyboardPanel != nullptr) keyboardPanel->setTargetInput (inputs.empty() ? -1 : selectedInput);
    refreshProgramView();
}

void MainComponent::refreshAll()
{
    const auto& setup = engine.getSetup();
    selectedInput = jlimit (0, jmax (0, (int) setup.inputs.size() - 1), selectedInput);
    tailSlider.setValue (setup.releaseTailSeconds, dontSendNotification);
    inputsPanel->refresh();
    inputsPanel->setSelected (selectedInput);
    if (keyboardPanel != nullptr) keyboardPanel->setTargetInput (setup.inputs.empty() ? -1 : selectedInput);
    refreshProgramView();
}

void MainComponent::refreshProgramView()
{
    programsPanel->refresh();
    slotsPanel->refresh();
    mappingsPanel->refresh();
}

// Engine::Listener ------------------------------------------------------------
void MainComponent::setupChanged()
{
    markDirty();
    updateTempoLabel();
    if (songsPanel != nullptr) songsPanel->refresh();
    refreshAll();
}

void MainComponent::tapTempoLearned (int cc)
{
    showStatus ("CC " + String (cc) + " now taps the tempo.");
    markDirty();
}

void MainComponent::programChanged (int inputIndex, int)
{
    markDirty();
    /* Picking a program is the one change someone makes constantly on stage and
       never thinks to save -- and from a tablet, the laptop may be across the
       room with no chance to. Bring the next autosave forward instead of making
       them wait out the full interval. */
    lastAutosaveTime = jmin (lastAutosaveTime,
                             Time::getMillisecondCounterHiRes() - autosaveIntervalMs + 2000.0);
    if (keyboardPanel != nullptr) keyboardPanel->refreshLabel();
    inputsPanel->repaint();
    if (inputIndex == selectedInput) refreshProgramView();
}

void MainComponent::programContentChanged (int inputIndex, int program)
{
    markDirty();
    if (inputIndex != selectedInput) return;
    programsPanel->repaint();
    if (program == getEditedProgram())
    {
        slotsPanel->refresh();
        mappingsPanel->refresh();
    }
}

void MainComponent::learnReceived (int, MappingDef::Source source, int number)
{
    mappingsPanel->onLearn (source, number);
}

void MainComponent::parameterTouched (int inputIndex, int program, int slot, int effect, int paramIndex)
{
    if (inputIndex == selectedInput && program == getEditedProgram())
        mappingsPanel->onTouched (slot, effect, paramIndex);
}

void MainComponent::statusMessage (const String& s) { showStatus (s); }

//==============================================================================
void MainComponent::openPluginEditor (int inputIndex, int program, int slot, int effect)
{
    auto* plugin = engine.getPlugin (inputIndex, program, slot, effect);
    if (plugin == nullptr || ! plugin->isAlive()) { showStatus ("Plugin is not running. Use Reload to restart it."); return; }

    if (looksLikeAraPlugin (plugin->getDescription()))
        showStatus (araWarning (plugin->getName()));

    const auto& setup = engine.getSetup();
    String title = plugin->getName();
    if (Engine::isOutputAddress (inputIndex))
    {
        const int o = Engine::outputFromAddress (inputIndex);
        if (o < (int) setup.outputs.size())
            title = "Output " + setup.outputs[(size_t) o].name + " / FX" + String (effect + 1) + " / " + plugin->getName();
    }
    else if (inputIndex >= 0 && inputIndex < (int) setup.inputs.size())
        title = setup.inputs[(size_t) inputIndex].name + " / " + programTitle (program, setup.inputs[(size_t) inputIndex].programs[(size_t) program]) + " / " + plugin->getName();

    if (! plugin->showEditor (title))
        showStatus (plugin->getName() + ": could not open its editor (" + plugin->getLastError() + ")");
}

//==============================================================================
File MainComponent::getAutosaveFile() const
{
    return settings.getFile().getSiblingFile ("autosave.stagerig.json");
}

void MainComponent::setCurrentFile (const File& f)
{
    currentFile = f;
    /* An unsaved setup keeps its charts beside the recovery copy, so attaching
       one before the first Save still has somewhere to go; Save As brings the
       folder along. */
    engine.setSetupFile (f == File() ? getAutosaveFile() : f);
    settings.setValue ("lastSetup", f.getFullPathName());
    settings.saveIfNeeded();
    fileLabel.setText (f == File() ? "(unsaved setup)" : f.getFullPathName(), dontSendNotification);
}

void MainComponent::newSetup()
{
    engine.loadSetup (Setup::makeDefault());
    setCurrentFile ({});
    showStatus ("New setup");
}

void MainComponent::loadSetupFile (const File& f)
{
    Setup s;
    auto r = Setup::loadFromFile (f, s);
    if (r.failed())
    {
        showStatus ("Load failed: " + r.getErrorMessage());
        refreshAll();
        return;
    }
    engine.loadSetup (std::move (s));
    setCurrentFile (f == getAutosaveFile() ? File() : f);
    dirty = false;
    lastAutosaveTime = Time::getMillisecondCounterHiRes();

    // Warn if an autosave is newer than the file we just opened (a previous session died).
    if (f != getAutosaveFile() && getAutosaveFile().existsAsFile()
        && getAutosaveFile().getLastModificationTime() > f.getLastModificationTime() + RelativeTime::seconds (5))
        showStatus ("Loaded " + f.getFileName() + ".  Note: " + getAutosaveFile().getFullPathName() + " is newer (autosaved "
                    + getAutosaveFile().getLastModificationTime().toString (true, true, false) + "); use Open... to recover it.");
    else
        showStatus ("Loaded " + f.getFileName());
}

bool MainComponent::writeSetupFile (const File& f, int captureBudgetMs)
{
    auto r = engine.captureSetup (captureBudgetMs).saveToFile (f);
    if (r.failed()) { showStatus ("Save failed: " + r.getErrorMessage()); return false; }
    dirty = false;
    lastAutosaveTime = Time::getMillisecondCounterHiRes();
    return true;
}

void MainComponent::openSetup()
{
    fileChooser = std::make_unique<FileChooser> ("Open StageRig setup",
                                                 currentFile.exists() ? currentFile.getParentDirectory() : File::getSpecialLocation (File::userHomeDirectory),
                                                 "*.stagerig.json;*.performer.json;*.json");
    fileChooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f != File()) loadSetupFile (f);
    });
}

void MainComponent::saveSetup (bool forceAskForFile)
{
    if (! forceAskForFile && currentFile != File())
    {
        if (writeSetupFile (currentFile)) showStatus ("Saved " + currentFile.getFileName());
        return;
    }
    fileChooser = std::make_unique<FileChooser> ("Save StageRig setup",
                                                 currentFile != File() ? currentFile : File::getSpecialLocation (File::userHomeDirectory).getChildFile ("MySetup.stagerig.json"),
                                                 "*.stagerig.json");
    fileChooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting, [this] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == File()) return;
        if (! f.hasFileExtension ("json")) f = f.withFileExtension ("stagerig.json");
        if (writeSetupFile (f))
        {
            // The charts travel with the setup: copy the folder to the new name.
            const auto from = engine.getChartsFolder(), to = chartsFolderFor (f);
            if (from.isDirectory() && from != to) from.copyDirectoryTo (to);
            setCurrentFile (f);
            showStatus ("Saved " + f.getFileName());
        }
    });
}

void MainComponent::saveOnQuit()
{
    /* Bounded: a host that has stopped answering must not hold the exit.
       Eight seconds covers every healthy host on a big rig; a plugin that
       cannot answer in that time keeps the state the model last had for it,
       which the autosave and the program changes have been keeping current. */
    if (currentFile != File()) writeSetupFile (currentFile, 8000);
    else                       writeSetupFile (getAutosaveFile(), 8000);
}

//==============================================================================
void MainComponent::showOutputs()
{
    auto* content = new OutputsComponent (engine, *this);
    content->setSize (760, 420);
    DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = "Outputs & master effects";
    o.dialogBackgroundColour = bgPanel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::outputsChanged()
{
    markDirty();
    inputsPanel->refresh();
}

//==============================================================================
void MainComponent::showAudioSettings()
{
    auto* content = new AudioSettingsComponent (engine, settings, [this] (const String& s) { showStatus (s); });
    content->setSize (640, 640);
    DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = "Audio Settings";
    o.dialogBackgroundColour = bgPanel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

/** Ask which wifi adapter should serve the hotspot, and remember the answer.

    Asked once per machine, because the answer is a property of the hardware and
    never changes: a laptop with one radio has one honest option, a laptop with a
    spare USB adapter has an obviously better one. We only ask when there is a
    real choice to make; with a single candidate there is nothing to ask about.

    `done (true)` means we have a usable configuration saved. */
void MainComponent::chooseHotspotAdapter (std::function<void (bool)> done)
{
    auto cfg = Hotspot::load (settings);

    auto list = Hotspot::adapters();
    Array<Hotspot::Adapter> usable;
    for (auto& a : list) if (a.supportsAccessPoint) usable.add (a);

    if (usable.isEmpty())
    {
        auto why = list.isEmpty()
            ? String ("No wifi adapter was found, so this computer cannot serve its own network.")
            : String ("This computer's wifi adapter cannot act as an access point. A USB wifi "
                      "adapter is the usual fix; most cost very little and work without drivers.");
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, "Cannot create a hotspot", why, "OK");
        done (false);
        return;
    }

    // Already answered, and the adapter is still present: nothing to ask.
    for (auto& a : usable)
        if (a.interfaceName == cfg.interfaceName)
            { done (true); return; }

    if (usable.size() == 1)
    {
        cfg.interfaceName = usable[0].interfaceName;
        Hotspot::save (settings, cfg);
        done (true);
        return;
    }

    // A real choice. Spell out the consequence rather than the hardware, since
    // "this one will drop your internet" is the part that matters.
    auto* w = new AlertWindow ("Which wifi adapter should serve the network?",
                              "Tablets will join a wifi network created by this computer.",
                              MessageBoxIconType::NoIcon);
    StringArray choices;
    for (auto& a : usable)
        choices.add (a.description + (a.inUseAsClient ? "  (in use: this computer would leave its current network)"
                                                     : "  (free)"));
    w->addComboBox ("adapter", choices, "Adapter");
    if (auto* box = w->getComboBoxComponent ("adapter")) box->setSelectedItemIndex (0);

    w->addTextEditor ("network", cfg.networkName, "Network name");
    w->addTextEditor ("password", cfg.password, "Password");
    w->addButton ("Use this", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));

    w->enterModalState (true, ModalCallbackFunction::create (
        [this, w, usable, cfg, done] (int result) mutable
        {
            std::unique_ptr<AlertWindow> owned (w);
            if (result == 0) { done (false); return; }

            const auto idx = w->getComboBoxComponent ("adapter")->getSelectedItemIndex();
            cfg.interfaceName = usable[jlimit (0, usable.size() - 1, idx)].interfaceName;
            cfg.networkName   = w->getTextEditorContents ("network").trim();
            cfg.password      = w->getTextEditorContents ("password").trim();

            if (cfg.networkName.isEmpty()) cfg.networkName = "StageRig";
            if (cfg.password.length() < 8)
            {
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Password too short",
                                                  "Wifi passwords must be at least 8 characters.", "OK");
                done (false);
                return;
            }
            Hotspot::save (settings, cfg);
            done (true);
        }), false);
}

/** Bring up our own network, then show the tablet dialog against it. */
void MainComponent::startHotspot()
{
    chooseHotspotAdapter ([this] (bool ok)
    {
        if (! ok) return;

        const auto cfg = Hotspot::load (settings);
        showStatus ("Starting the wifi network...");

        if (const auto err = Hotspot::start (cfg); err.isNotEmpty())
        {
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Hotspot", err, "OK");
            showStatus ("The wifi network did not start.");
            return;
        }
        showStatus ("Wifi network \"" + cfg.networkName + "\" is on.");
        if (remoteBtn.getToggleState()) showRemote();
    });
}

/* Just the server, no dialog. At startup we want the previous session's choice
   honoured quietly; a dialog appearing on its own every launch would be an
   interruption, not a help. */
bool MainComponent::startRemote()
{
    if (remote == nullptr) remote = std::make_unique<RemoteServer> (engine, settings);
    const int port = settings.getIntValue ("remotePort", 7777);

    if (! remote->start (port))
    {
        remoteBtn.setToggleState (false, dontSendNotification);
        settings.setValue ("remoteOn", false);
        showStatus ("Could not listen on port " + String (port) + " — is another copy of StageRig running?");
        return false;
    }

    settings.setValue ("remotePort", port);
    settings.setValue ("remoteOn", true);
    settings.saveIfNeeded();
    return true;
}

void MainComponent::updateTempoLabel()
{
    tempoLabel.setText (String (engine.getTempoBpm(), 1) + " bpm", dontSendNotification);
}

/** Choosing what taps the tempo. Behind a right-click because it is set up
    once and then left alone: on stage you tap, or type into the readout. */
void MainComponent::showTempoMenu()
{
    PopupMenu m;
    /* No "Type a tempo..." here any more: the readout itself is the text box,
       which is one click instead of three. */
    const int tapCC = engine.getTapTempoCC();
    m.addSectionHeader ("Tap from a MIDI controller");
    // Pressing the pedal beats looking up what it sends, so that comes first.
    m.addItem (4, "Learn: press the pedal or button...");
    m.addItem (2, tapCC > 0 ? "Type a controller number (now CC " + String (tapCC) + ")..."
                            : "Type a controller number...");
    if (tapCC > 0)
        m.addItem (3, "Stop using CC " + String (tapCC));

    m.showMenuAsync (PopupMenu::Options().withTargetComponent (tapButton), [this] (int choice)
    {
        if (choice == 2)
        {
            auto* w = new AlertWindow ("Tap tempo",
                                       "Which controller taps the tempo? A footswitch usually sends "
                                       "CC 64 (sustain) or CC 80.\n\nIt works on any input and in any "
                                       "program, and only the press counts.",
                                       MessageBoxIconType::NoIcon);
            w->addTextEditor ("cc", String (jmax (1, engine.getTapTempoCC())));
            w->addButton ("Use it", 1, KeyPress (KeyPress::returnKey));
            w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
            w->enterModalState (true, ModalCallbackFunction::create ([this, w] (int r)
            {
                std::unique_ptr<AlertWindow> owned (w);
                if (r == 0) return;
                const int cc = w->getTextEditorContents ("cc").getIntValue();
                if (cc < 1 || cc > 127) { showStatus ("A controller number is 1 to 127."); return; }
                engine.setTapTempoCC (cc);
                showStatus ("CC " + String (cc) + " now taps the tempo.");
                markDirty();
            }), false);
        }
        else if (choice == 3)
        {
            engine.setTapTempoCC (0);
            showStatus ("No controller taps the tempo now.");
            markDirty();
        }
        else if (choice == 4)
        {
            engine.armTapTempoLearn (true);
            showStatus ("Press the pedal or button that should tap the tempo...");
        }
    });
}

void MainComponent::showRemote()
{
    if (! remoteBtn.getToggleState())
    {
        if (remote != nullptr) remote->stop();
        settings.setValue ("remoteOn", false);
        settings.saveIfNeeded();
        showStatus ("Tablet control off.");
        return;
    }

    if (! startRemote()) return;
    const auto url = remote->getUrl();

    // Two steps, big type: read off a screen at arm's length while standing up.
    auto* content = new Component();
    content->setSize (520, 300);
    struct RemotePanel : public Component
    {
        RemotePanel (String u, String c, String net)
            : url (std::move (u)), code (std::move (c)), network (std::move (net)),
              qr (QrCode::encode (url)) {}
        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().reduced (18);
            g.fillAll (Colour (0xff1e1f24));

            // Point a camera at this and skip the typing entirely.
            if (qr.isValid())
            {
                auto box = r.removeFromRight (128);
                qr.draw (g, box.removeFromTop (128), Colours::black, Colours::white);
                g.setColour (Colour (0xff9aa0ab));
                g.setFont (FontOptions (11.5f));
                g.drawFittedText ("or scan this", box.removeFromTop (18), Justification::centred, 1);
                r.removeFromRight (14);
            }

            g.setColour (Colour (0xff9aa0ab));
            g.setFont (FontOptions (13.0f, Font::bold));
            g.drawFittedText ("1.  OPEN THIS ON YOUR TABLET", r.removeFromTop (20), Justification::centredLeft, 1);
            g.setColour (Colours::white);
            g.setFont (FontOptions (20.0f, Font::bold));
            g.drawFittedText (url, r.removeFromTop (34), Justification::centredLeft, 1);

            r.removeFromTop (18);
            g.setColour (Colour (0xff9aa0ab));
            g.setFont (FontOptions (13.0f, Font::bold));
            g.drawFittedText ("2.  ENTER THIS CODE", r.removeFromTop (20), Justification::centredLeft, 1);
            auto codeBox = r.removeFromTop (74);
            g.setColour (Colour (0xff15161c));
            g.fillRoundedRectangle (codeBox.toFloat(), 8.0f);
            g.setColour (Colour (0xff5aa9ff));
            g.setFont (FontOptions (48.0f, Font::bold));
            g.drawFittedText (code, codeBox, Justification::centred, 1);

            r.removeFromTop (14);
            g.setColour (Colour (0xff9aa0ab));
            g.setFont (FontOptions (12.5f));

            const auto note = network.isNotEmpty()
                ? "The tablet must join this computer's wifi network, \"" + network + "\". It remembers "
                  "the code, and the code does not change when StageRig restarts, so \"Add to Home "
                  "Screen\" gives you a one-tap program selector.\n\n"
                  "Anyone on that network who has the code can change your sounds."
                : "The tablet must be on the same wifi as this computer. It remembers the code, "
                  "and the code does not change when StageRig restarts, so \"Add to Home Screen\" "
                  "gives you a one-tap program selector.\n\n"
                  "No wifi at the venue? Use \"Create a wifi network\" below and the tablet joins this "
                  "computer directly.";
            g.drawFittedText (note, r.removeFromBottom (r.getHeight() - 4), Justification::topLeft, 7);
        }
        String url, code, network;
        QrCode qr;
    };

    const auto hs = Hotspot::state();
    auto* panel = new RemotePanel (url, remote->getToken(), hs.active ? hs.networkName : String());
    panel->setBounds (0, 0, 520, 300);
    content->addAndMakeVisible (panel);

    // Offered here because this is where someone stands when the venue wifi has
    // just let them down.
    if (Hotspot::available())
    {
        auto* hotspotBtn = new TextButton (hs.active ? "Stop the wifi network" : "Create a wifi network");
        hotspotBtn->setBounds (18, 300, 484, 30);

        /* Close the dialog by searching up from the BUTTON, not from `this`:
           MainComponent lives in the main window, so searching from there walks a
           different hierarchy entirely and never finds this dialog -- which left
           the window sitting open after the network had already been torn down.

           Closing before the nmcli call also keeps things honest: stopping takes
           a moment, and a visible window that cannot repaint reads as a freeze. */
        hotspotBtn->onClick = [this, wasActive = hs.active, btn = Component::SafePointer<TextButton> (hotspotBtn)]
        {
            if (btn != nullptr)
                if (auto* dw = btn->findParentComponentOfClass<DialogWindow>())
                    dw->exitModalState (0);

            if (wasActive)
            {
                if (const auto err = Hotspot::stop(); err.isNotEmpty())
                    showStatus (err);
                else
                    showStatus ("The wifi network is off.");
                if (remoteBtn.getToggleState()) showRemote();
            }
            else
            {
                startHotspot();
            }
        };
        content->addAndMakeVisible (hotspotBtn);
        content->setSize (520, 344);
    }

    DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = "Tablet control";
    o.dialogBackgroundColour = bgPanel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.launchAsync();
    showStatus ("Tablet control on: " + url + "  code " + remote->getToken());

    /* A blocked port looks exactly like a broken app from the tablet's side: the
       address is right, the server is listening, and the desktop can load the
       page because local traffic never passes the firewall. Say so here rather
       than leaving someone to discover it at a gig. */
    if (const auto warn = Hotspot::firewallWarning (remote->getPort()); warn.isNotEmpty())
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Tablet control", warn, "OK");
}

void MainComponent::printProgramMap()
{
    const auto setup = engine.captureSetup();
    const auto title = currentFile.existsAsFile() ? currentFile.getFileNameWithoutExtension() : String ("StageRig");
    // JUCE cannot print on Linux; write HTML styled for paper and let the browser print it.
    auto out = File::getSpecialLocation (File::tempDirectory)
                   .getChildFile ("stagerig-program-map-" + String (Time::currentTimeMillis()) + ".html");
    // Include the tablet address only while the server is actually running: a
    // printed code pointing at a closed port would be worse than none.
    const auto tabletUrl = (remote != nullptr && remote->isRunning()) ? remote->getUrl() : String();
    if (! out.replaceWithText (ProgramMap::toHtml (setup, title, tabletUrl)))
    {
        showStatus ("Could not write the program map to " + out.getFullPathName());
        return;
    }
    out.startAsProcess();
    showStatus ("Program map opened in your browser — print it from there (Ctrl+P).");
}

void MainComponent::showPluginManager()
{
    auto* content = new PluginManagerComponent (engine.getPluginHost(), [this] (const String& s) { showStatus (s); });
    content->setSize (960, 600);
    DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = "Plugins";
    o.dialogBackgroundColour = bgPanel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

} // namespace perf
