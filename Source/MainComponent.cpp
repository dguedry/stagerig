#include "MainComponent.h"
#include "GmSynth.h"
#include "PatchDialog.h"
#include "GmMixer.h"
#include "Confirm.h"
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
        return name + " looks like an ARA-only plugin. ARA plugins need a DAW that supports ARA (Reaper, Logic...); here the window may be empty and can freeze StageRig. Prefer the non-ARA version.";
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
        addBtn.setTooltip ("Add a keyboard, an audio input or the backing track");
        removeBtn.setTooltip ("Remove this input and all its programs");
        addBtn.onClick = [this]
        {
            PopupMenu m;
            m.addItem (1, "Keyboard (MIDI)");
            m.addItem (2, "Audio input (microphone, guitar, hardware synth)");
            m.addItem (3, "Backing track (the songs' MIDI files)", engine.getTrackInput() < 0);
            m.showMenuAsync (PopupMenu::Options().withTargetComponent (&addBtn), [this] (int r)
            {
                if (r == 0) return;
                if (r == 3)
                {
                    lastNotified = -1; setSelected (engine.ensureTrackInput());
                    owner.showStatus ("The backing track is an input: each song's program here decides what its MIDI file's parts play");
                    return;
                }
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
            const int sel = list.getSelectedRow();
            if (sel < 0 || sel >= (int) engine.getSetup().inputs.size()) return;
            const auto& in = engine.getSetup().inputs[(size_t) sel];
            confirm ("Remove input", "Remove \"" + in.name + "\"" + (in.isAudio() || in.isTrack() ? String (" and its programs?") : String (" and all of its programs?"))
                       + "\n\nThe songs that used it forget it. There is no undo.", "Remove", [this, sel]
            {
                engine.removeInput (sel); lastNotified = -1; setSelected (jmax (0, sel - 1));
            });
        };

        addAndMakeVisible (nameEditor);
        nameEditor.setTextToShowWhenEmpty ("Name", textDim);
        nameEditor.onTextChange = [this]
        {
            int sel = list.getSelectedRow();
            if (sel >= 0) { engine.setInputName (sel, nameEditor.getText()); list.repaintRow (sel); }
        };

        addAndMakeVisible (deviceBox);
        deviceBox.setTooltip ("The MIDI device this keyboard is plugged in as. Missing? Plug it in, then Options > Rescan MIDI devices.");
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
        channelBox.setTooltip ("Only notes on this channel reach this input. All channels takes everything the device sends; use that for a keyboard whose zones send on several channels.");
        channelBox.addItem ("All channels", 1);
        for (int c = 1; c <= 16; ++c) channelBox.addItem ("Ch " + String (c), c + 1);
        channelBox.onChange = [this]
        {
            int sel = list.getSelectedRow();
            if (sel >= 0) { engine.setInputChannel (sel, channelBox.getSelectedId() - 1); list.repaintRow (sel); }
        };

        addAndMakeVisible (pcToggle);
        pcToggle.setTooltip ("When your keyboard sends a program change, switch this input to that program");
        pcToggle.onClick = [this]
        {
            int sel = list.getSelectedRow();
            if (sel >= 0) engine.setInputRespondToProgramChange (sel, pcToggle.getToggleState());
            pcChannelBox.setEnabled (pcToggle.getToggleState());
        };

        addAndMakeVisible (outputBox);
        outputBox.setTooltip ("Which output this input's sound goes to. An output is a pair of interface channels with its own master effects and volume. Set them up under Options > Outputs & master effects.");
        outputBox.onChange = [this]
        {
            int sel = list.getSelectedRow();
            if (sel >= 0 && outputBox.getSelectedId() > 0) { engine.setInputOutput (sel, outputBox.getSelectedId() - 1); owner.markDirty(); }
        };

        addAndMakeVisible (audioBox);
        audioBox.setTooltip ("Which interface channels this audio input takes its sound from: one channel (mono, heard left and right) "
                             "or a pair. Only channels ticked in Audio... are offered.");
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

        // The audio channel row only means something for an audio input; the
        // backing track has no device, channel or program changes at all.
        audioLabel.setVisible (def.isAudio());
        audioBox.setVisible (def.isAudio());
        for (auto* c : std::initializer_list<Component*> { &deviceLabel, &deviceBox, &channelLabel, &channelBox, &pcToggle, &pcChannelLabel, &pcChannelBox })
            c->setVisible (! def.isTrack());
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
        if (devices.isEmpty())
        {
            deviceBox.addItem ("No MIDI device connected: plug one in, then Options > Rescan MIDI devices", 999);
            deviceBox.setItemEnabled (999, false);
        }
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
        if (def.isTrack())
            info = "plays the songs' MIDI files";
        else if (def.isAudio())
        {
            info << "audio in " << (def.audioLeft + 1);
            if (def.audioRight >= 0) info << "+" << (def.audioRight + 1);
            if (def.midiDeviceName.isNotEmpty()) info << "  \xc2\xb7  " << def.midiDeviceName;
        }
        else
            info = def.midiDeviceName.isNotEmpty() ? def.midiDeviceName : "no MIDI device chosen";
        if (def.channel > 0 && ! def.isTrack()) info << "  ch " << def.channel;
        g.drawText (info, 8, 22, w - 80, 16, Justification::centredLeft, true);

        if (! def.isTrack())      // its program follows the song, not a number anyone sends
        {
            g.setColour (accent);
            g.setFont (FontOptions (13.0f, Font::bold));
            g.drawText (String (def.currentProgram).paddedLeft ('0', 3), w - 72, 22, 64, 16, Justification::centredRight, true);
        }

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
    Label pcChannelLabel { {}, "Arrive on" };
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
        copyBtn.setTooltip ("Copy this program (instruments, effects, mappings)");
        pasteBtn.setTooltip ("Paste the copied program here, replacing this one");
        clearBtn.setTooltip ("Empty this program");
        copyBtn.onClick  = [this] { clipboard = owner.getEditedProgram(); pasteBtn.setEnabled (true); owner.showStatus ("Copied program " + String (clipboard)); };
        pasteBtn.onClick = [this] { if (clipboard >= 0) engine.copyProgram (owner.getSelectedInput(), clipboard, owner.getEditedProgram()); };
        clearBtn.onClick = [this]
        {
            const int in = owner.getSelectedInput(), p = owner.getEditedProgram();
            const auto& inputs = engine.getSetup().inputs;
            if (in < 0 || in >= (int) inputs.size()) return;
            const auto& pd = inputs[(size_t) in].programs[(size_t) p];
            if (pd.isEmpty() && pd.name.isEmpty()) return;
            confirm ("Empty program", "Empty program " + String (p).paddedLeft ('0', 3) + (pd.name.isNotEmpty() ? " " + pd.name : String())
                       + "?\n\nIts instruments, effects and mappings go. There is no undo.", "Empty", [this, in, p] { engine.clearProgram (in, p); });
        };
        pasteBtn.setEnabled (false);
    }

    void refresh()
    {
        const auto& inputs = engine.getSetup().inputs;
        const int in = owner.getSelectedInput();
        const bool valid = in >= 0 && in < (int) inputs.size();
        list.setEnabled (valid);
        nameEditor.setEnabled (valid);
        header.setText (valid && inputs[(size_t) in].isTrack() ? "PROGRAMS  (the backing track gets one per song; the song chooses it)" : "PROGRAMS  (click one to switch)", dontSendNotification);
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
                                     : String (prog.slots.size()) + (prog.slots.size() == 1 ? " instrument" : " instruments");
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
    but every entry carries the plugin's icon. Item ids are idBase + index into `types`. */
static void addPluginsToMenu (PopupMenu& menu, const Array<PluginDescription>& types, PluginIcons& icons, int idBase = 1)
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
            target.addItem (idBase + i, types[i].name + "  [" + types[i].pluginFormatName + "]", true, false, icons.get (types[i], 32));
    };
    if (makers.size() == 1) { addItems (menu, makers[0]); return; }
    for (auto& m : makers)
    {
        PopupMenu sub;
        addItems (sub, m);
        menu.addSubMenu (m, sub);
    }
}

/** The MIDI outputs worth offering: the system's own "Midi Through" loopback
    is left out -- it is always there, goes nowhere, and looks like a choice. */
static Array<MidiDeviceInfo> midiOutputPorts()
{
    Array<MidiDeviceInfo> ports;
    for (const auto& d : MidiOutput::getAvailableDevices())
        if (! d.name.containsIgnoreCase ("Midi Through")) ports.add (d);
    return ports;
}

/** Fills a menu with ports as items idBase + index. The current port, if
    unplugged, is shown ticked and greyed; with none at all the menu says why. */
static void addPortItems (PopupMenu& m, const Array<MidiDeviceInfo>& ports, int idBase, const String& currentId = {}, const String& currentName = {})
{
    bool currentListed = currentId.isEmpty();
    for (int i = 0; i < ports.size(); ++i)
    {
        m.addItem (idBase + i, ports[i].name, true, ports[i].identifier == currentId);
        currentListed = currentListed || ports[i].identifier == currentId;
    }
    if (! currentListed) m.addItem (idBase - 1, currentName + "  (not connected)", false, true);
    if (ports.isEmpty())
    {
        m.addItem (idBase - 2, "No MIDI interface connected", false);
        m.addItem (idBase - 3, "Plug one in, then Options > Rescan MIDI devices", false);
    }
}

/** A chain of blocks, in order: the insert effects after a slot's sound, a
    program's or an output's chain -- or a slot's MIDI chain, the MIDI
    processors and sends its notes go through before the sound.

    A slot's effects chain carries the slot's "+": it offers what can go
    there, which is an audio effect after the sound or a MIDI block before
    it. The MIDI chain above it then has no button of its own. */
class EffectChainComponent : public Component
{
public:
    static constexpr int rowHeight = 26;

    EffectChainComponent (Engine& e, MainComponent& o, int slotIndex) : engine (e), owner (o), slot (slotIndex)
    {
        address = [this] { return std::make_pair (owner.getSelectedInput(), owner.getEditedProgram()); };
        addChildComponent (addBtn);
        addBtn.setVisible (! isMidi());
        addBtn.onClick = [this] { showAddMenu(); };
        if (slot >= 0) addBtn.setButtonText ("+ Add...");
        addBtn.setTooltip (slot < 0 ? "Add an effect on everything in this program, after the instruments"
                                    : "Add to this instrument: an effect on its sound, or a MIDI effect or a send to a MIDI device for its notes before the sound");
    }

    /** Whose chain this is: (input, program), or an output's master chain
        through Engine::outputAddress(). The program chains follow the
        selection; the Outputs window sets its own. */
    std::function<std::pair<int, int>()> address;

    void setSlot (int s) { slot = s; }
    bool isMidi() const  { return ProgramDef::isMidiChain (slot); }

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

    int preferredHeight() const { return (int) rows.size() * rowHeight + (addBtn.isVisible() ? rowHeight : 0); }

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
        g.setColour (isMidi() ? accent.withAlpha (0.55f) : textDim.withAlpha (0.35f));
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
            on.setTooltip (chain.isMidi() ? "Active (off = bypassed: the notes go past it)" : "Effect active (off = bypassed)");
            upBtn.setTooltip ("Move up: the signal reaches it sooner");
            downBtn.setTooltip ("Move down: the signal reaches it later");
            removeBtn.setTooltip ("Remove this effect");

            auto& e = chain.engine;
            auto& o = chain.owner;
            on.onClick        = [this, &e, &o] { const auto a = chain.address(); e.setEffectBypassed (a.first, a.second, chain.slot, index, ! on.getToggleState()); o.markDirty(); };
            guiBtn.onClick    = [this, &e, &o] { const auto a = chain.address();
                                                 if (def.isSend()) chain.showSendMenu (index, def, guiBtn);
                                                 else if (def.isBuiltIn()) chain.showBuiltInMenu (index, def, guiBtn);
                                                 else if (alive)   o.openPluginEditor (a.first, a.second, chain.slot, index);
                                                 else              e.reloadPlugin (a.first, a.second, chain.slot, index); };
            upBtn.onClick     = [this, &e, &o] { const auto a = chain.address(); e.moveEffect (a.first, a.second, chain.slot, index, index - 1); o.markDirty(); };
            downBtn.onClick   = [this, &e, &o] { const auto a = chain.address(); e.moveEffect (a.first, a.second, chain.slot, index, index + 1); o.markDirty(); };
            removeBtn.onClick = [this, &e, &o] { const auto a = chain.address(); e.removeEffect (a.first, a.second, chain.slot, index); o.markDirty(); };
        }

        void update (const EffectDef& d, bool loaded, bool loading, const String& error, bool canUp, bool canDown)
        {
            def = d;
            alive = loaded;
            on.setToggleState (! def.bypassed, dontSendNotification);
            PluginDescription forIcon = def.plugin;
            if (def.isSend()) { forIcon = {}; forIcon.name = def.portName; forIcon.pluginFormatName = "MIDI"; }   // the port's initials
            if (def.isBuiltIn()) { forIcon = {}; forIcon.name = def.displayName(); forIcon.pluginFormatName = "MIDI"; }
            icon.setImage (chain.engine.getPluginHost().getIcons().get (forIcon, 40), RectanglePlacement::centred);
            icon.setAlpha (def.bypassed ? 0.45f : 1.0f);
            if (def.isBuiltIn())
            {
                int kr, km;
                const bool inSongKey = def.followSongKey && (def.kind == EffectDef::Kind::scale || def.kind == EffectDef::Kind::run || def.kind == EffectDef::Kind::bigBand) && chain.engine.keyInPlay (kr, km);
                name.setText (def.displayName() + "  [" + MidiFx::describe (chain.engine.effectiveEffectDef (def)) + (inSongKey ? ", the song's key" : "") + "]", dontSendNotification);
                name.setTooltip (def.kind == EffectDef::Kind::arpeggio ? "Plays the notes you hold one at a time, in time with the rig's tempo"
                               : def.kind == EffectDef::Kind::chord    ? "Every note you play becomes this chord"
                               : def.kind == EffectDef::Kind::run      ? "Each key you press starts a run up (or down) the scale from it, in time; the last note holds while the key does. A chord is a run in each voice."
                               : def.kind == EffectDef::Kind::bigBand  ? "Each key plays the chord on that step of the key, as a big band section voices it: C6 on the tonic, Dm7 on the 2nd, G7 on the 5th. Chords... sets the notes of each."
                               : def.kind == EffectDef::Kind::humanize ? "Each note lands a little late and a little louder or softer, by a different amount every time: a player's unevenness on keyed or quantised notes"
                               : def.kind == EffectDef::Kind::phrase   ? "Each key plays a short MIDI file -- a lick, a fill, a riff -- moved to begin on the key pressed, at the rig's tempo and the key's velocity"
                                                                        : "Notes off the scale are moved down to the nearest note on it");
                guiBtn.setButtonText ("Settings...");
                guiBtn.setTooltip ("Its settings");
                guiBtn.setEnabled (true);
                guiBtn.setColour (TextButton::buttonColourId, getLookAndFeel().findColour (TextButton::buttonColourId));
            }
            else if (def.isSend())
            {
                String what = "Also plays " + def.portName + "  [" + (def.channel > 0 ? "channel " + String (def.channel) : String ("same channel"));
                if (def.patch >= 0) what << ", patch " << (def.patch + 1);
                name.setText (what + "]", dontSendNotification);
                name.setTooltip (loaded ? "A copy of the notes at this point also goes to this MIDI device; the notes carry on to what follows, unchanged"
                                        : error);
                guiBtn.setButtonText ("Device...");
                guiBtn.setTooltip ("MIDI port, channel and patch recall");
                guiBtn.setEnabled (true);
                guiBtn.setColour (TextButton::buttonColourId, getLookAndFeel().findColour (TextButton::buttonColourId));
            }
            else
            {
                name.setText (def.plugin.name + "  [" + def.plugin.pluginFormatName + (chain.isMidi() ? ", MIDI" : "") + "]"
                                + (loading ? "   loading..." : String()), dontSendNotification);
                name.setTooltip (error.isNotEmpty() ? error : "File: " + def.plugin.fileOrIdentifier);
                guiBtn.setButtonText (loading ? "Loading" : loaded ? "Window" : "Reload");
                guiBtn.setTooltip (loaded ? "Open the plugin's own window" : "The plugin stopped: load it again");
                guiBtn.setEnabled (! loading);
                guiBtn.setColour (TextButton::buttonColourId, loaded || loading ? getLookAndFeel().findColour (TextButton::buttonColourId) : Colours::darkred);
            }
            name.setColour (Label::textColourId, error.isNotEmpty() ? Colours::orangered : (def.bypassed || loading ? textDim : Colours::white));
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
        EffectDef def;
        ToggleButton on;
        ImageComponent icon;
        Label name;
        TextButton guiBtn { "Window" }, upBtn { "^" }, downBtn { "v" }, removeBtn { "X" };
    };

    /** A send's port, channel and patch recall. */
    void showSendMenu (int index, const EffectDef& def, Component& target)
    {
        const auto ports = midiOutputPorts();
        PopupMenu portMenu;
        addPortItems (portMenu, ports, 100, def.portIdentifier, def.portName);
        PopupMenu chMenu;
        chMenu.addItem (300, "Same channel as it arrives", true, def.channel == 0);
        for (int c = 1; c <= 16; ++c) chMenu.addItem (300 + c, "Channel " + String (c), true, def.channel == c);

        PopupMenu m;
        m.addSubMenu ("MIDI port", portMenu);
        m.addSubMenu ("On channel", chMenu);
        m.addItem (200, "Patch recall...");
        const auto a = address();
        const int s = ProgramDef::slotOfChain (slot);
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (target), [this, a, s, index, ports, def] (int r)
        {
            if (r >= 100 && r < 100 + ports.size()) engine.setMidiSendPort (a.first, a.second, s, index, ports[r - 100]);
            else if (r >= 300 && r <= 316)        engine.setMidiSendChannel (a.first, a.second, s, index, r - 300);
            else if (r == 200)
            {
                showPatchRecallDialog (def.portName, def.bankMsb, def.bankLsb, def.patch, [this, a, s, index] (int msb, int lsb, int patch)
                {
                    engine.setMidiSendPatch (a.first, a.second, s, index, msb, lsb, patch);
                    owner.markDirty();
                });
                return;
            }
            else return;
            owner.markDirty();
        });
    }

    void showBuiltInMenu (int index, const EffectDef& def, Component& target)
    {
        /* Each setting a submenu with the choice ticked: few enough to read
           at a glance, and nothing to type. */
        PopupMenu m;
        const auto a = address();
        const int s = ProgramDef::slotOfChain (slot);
        if (def.kind == EffectDef::Kind::arpeggio)
        {
            PopupMenu rate, mode, oct, gate;
            for (int i = 0; i < MidiFx::numArpRates(); ++i) rate.addItem (100 + i, MidiFx::arpRates[i], true, def.arpRate == i);
            for (int i = 0; i < MidiFx::numArpModes(); ++i) mode.addItem (200 + i, MidiFx::arpModes[i], true, def.arpMode == i);
            for (int i = 1; i <= 4; ++i) oct.addItem (300 + i, String (i) + (i == 1 ? " octave" : " octaves"), true, def.arpOctaves == i);
            for (int g : { 25, 50, 80, 100 }) gate.addItem (400 + g, String (g) + "%" + (g == 100 ? "  (legato)" : g == 25 ? "  (staccato)" : ""), true, def.arpGate == g);
            m.addSubMenu ("Rate", rate);
            m.addSubMenu ("Pattern", mode);
            m.addSubMenu ("Octaves", oct);
            m.addSubMenu ("Note length", gate);
            m.addItem (500, "Latch  (keeps going after you let go)", true, def.arpLatch);
        }
        else if (def.kind == EffectDef::Kind::chord)
        {
            for (int i = 0; i < MidiFx::numChordTypes(); ++i) m.addItem (600 + i, MidiFx::chordTypes[i], true, def.chordType == i);
        }
        else if (def.kind == EffectDef::Kind::humanize)
        {
            PopupMenu timing, vel;
            const int times[] = { 0, 5, 12, 25, 50 };
            const char* timeNames[] = { "On time", "Tight  (up to 5 ms late)", "Relaxed  (up to 12 ms)", "Lazy  (up to 25 ms)", "Sloppy  (up to 50 ms)" };
            for (int i = 0; i < 5; ++i) timing.addItem (1600 + times[i], timeNames[i], true, def.humanTimingMs == times[i]);
            const int vels[] = { 0, 5, 10, 20, 40 };
            for (int i = 0; i < 5; ++i) vel.addItem (1700 + vels[i], vels[i] == 0 ? String ("As played") : String (CharPointer_UTF8 ("\xc2\xb1")) + String (vels[i]), true, def.humanVelocity == vels[i]);
            m.addSubMenu ("Timing", timing);
            m.addSubMenu ("Velocity", vel);
        }
        else if (def.kind == EffectDef::Kind::phrase)
        {
            PopupMenu root, library;
            m.addItem (1800, def.phraseNotes.empty() ? "Load a MIDI file..." : "Load another MIDI file...  (now " + def.phraseName + ")");
            /* The built-in ones, a submenu per folder: licks and comping written
               in C, drums with the pitch fixed. The list is read when the menu
               opens, so a file dropped into the folder is there next time. */
            libraryFiles.clear();
            if (const auto folder = MidiFx::phraseLibraryFolder(); folder.isDirectory())
            {
                auto groups = folder.findChildFiles (File::findDirectories, false);
                groups.sort();
                for (const auto& g : groups)
                {
                    PopupMenu one;
                    auto files = g.findChildFiles (File::findFiles, false, "*.mid");
                    files.sort();
                    for (const auto& f : files)
                    {
                        one.addItem (2100 + (int) libraryFiles.size(), f.getFileNameWithoutExtension(), true, def.phraseName == f.getFileNameWithoutExtension());
                        libraryFiles.push_back (f);
                    }
                    library.addSubMenu (g.getFileName(), one);
                }
            }
            m.addSubMenu ("Built-in phrases", library, ! libraryFiles.empty());
            root.addItem (1900, "Begins on the key pressed  (the file's first note moves to it)", true, def.phraseRoot == -1);
            root.addItem (1901, "As written  (the key C4 plays it as it is; others move it)", true, def.phraseRoot == 60);
            root.addItem (1902, "As written from C3", true, def.phraseRoot == 48);
            root.addItem (1903, "As written from C5", true, def.phraseRoot == 72);
            root.addItem (1904, "Fixed  (drums: every key plays it as written)", true, def.phraseRoot == -2);
            m.addSubMenu ("Pitch", root, ! def.phraseNotes.empty());
            m.addItem (2000, "Stop when the key is let go  (else a tap plays the whole phrase)", ! def.phraseNotes.empty(), def.phraseStop);
            m.addItem (2001, "Repeat while the key is held", ! def.phraseNotes.empty(), def.phraseLoop);
        }
        else
        {
            PopupMenu root, mode;
            int kr, km;
            const bool songHasKey = engine.keyInPlay (kr, km);
            for (int i = 0; i < 12; ++i) root.addItem (700 + i, MidiFx::noteNames[i], true, ! def.followSongKey && def.scaleRoot == i);
            for (int i = 0; i < MidiFx::numScaleModes(); ++i) mode.addItem (800 + i, MidiFx::scaleModes[i], true, ! def.followSongKey && def.scaleMode == i);
            m.addItem (1400, "Follow the song's key" + (songHasKey ? "  (" + MidiFx::keyName (kr, km) + " now)" : String ("  (this song has none, so its own key is used)")), true, def.followSongKey);
            m.addSubMenu ("Key  (its own; picking one stops it following the song)", root);
            m.addSubMenu ("Scale  (its own)", mode);
            if (def.kind == EffectDef::Kind::run)
            {
                PopupMenu notes, takes, starts;
                notes.addItem (900 + 8, "An octave  (8 notes)", true, def.runNotes == 8);
                notes.addItem (900 + 15, "Two octaves  (15 notes)", true, def.runNotes == 15);
                notes.addItem (900 + 5, "A fifth  (5 notes)", true, def.runNotes == 5);
                notes.addSeparator();
                for (int n = 3; n <= 16; ++n) if (n != 5 && n != 8 && n != 15) notes.addItem (900 + n, String (n) + " notes", true, def.runNotes == n);
                const float options[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
                const char* names[] = { "A quarter of a beat", "Half a beat", "A beat", "Two beats", "A bar  (four beats)" };
                for (int i = 0; i < 5; ++i) takes.addItem (1000 + i, names[i], true, std::abs (def.runBeats - options[i]) < 0.01f);
                static const char* ordinals[] = { "The key pressed", "The 2nd above it", "The 3rd above it", "The 4th above it", "The 5th above it", "The 6th above it", "The 7th above it" };
                for (int i = 0; i < 7; ++i) starts.addItem (1300 + i, ordinals[i], true, def.runDegree == i);
                m.addSubMenu ("Length", notes);
                m.addSubMenu ("Takes  (the last note lands at the end)", takes);
                m.addSubMenu ("Starts on", starts, def.runToKey == 0);
                m.addItem (1100, "Up from where it starts", true, def.runDown == 0 && def.runToKey == 0);
                m.addItem (1101, "Down from where it starts", true, def.runDown == 1 && def.runToKey == 0);
                m.addItem (1102, "Up to the key  (begins below it, lands on it)", true, def.runDown == 0 && def.runToKey == 1);
                m.addItem (1103, "Down to the key  (begins above it, lands on it)", true, def.runDown == 1 && def.runToKey == 1);
                m.addItem (1200, "Hold the last note while the key is held", true, def.runHoldLast);
            }
            if (def.kind == EffectDef::Kind::bigBand)
                m.addItem (1500, "Chords...  (the chord each key plays, step by step)");
        }
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (target), [this, a, s, index, def] (int r)
        {
            if (r == 0) return;
            EffectDef d = def;
            if (r >= 100 && r < 200)      d.arpRate = r - 100;
            else if (r >= 200 && r < 300) d.arpMode = r - 200;
            else if (r >= 300 && r < 400) d.arpOctaves = r - 300;
            else if (r >= 400 && r < 500) d.arpGate = r - 400;
            else if (r == 500)            d.arpLatch = ! d.arpLatch;
            else if (r >= 600 && r < 700) d.chordType = r - 600;
            else if (r >= 700 && r < 800) { d.scaleRoot = r - 700; d.followSongKey = false; }
            else if (r >= 800 && r < 900) { d.scaleMode = r - 800; d.followSongKey = false; }
            else if (r == 1400)           d.followSongKey = ! d.followSongKey;
            else if (r >= 900 && r < 1000) d.runNotes = r - 900;
            else if (r >= 1000 && r < 1100) { const float options[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f }; d.runBeats = options[jlimit (0, 4, r - 1000)]; }
            else if (r >= 1300 && r < 1400) d.runDegree = r - 1300;
            else if (r >= 1100 && r <= 1103) { d.runDown = (r - 1100) & 1; d.runToKey = (r - 1100) >> 1; }
            else if (r == 1200) d.runHoldLast = ! d.runHoldLast;
            else if (r == 1500) { showBandChordsDialog (index, def); return; }
            else if (r >= 1600 && r < 1700) d.humanTimingMs = r - 1600;
            else if (r >= 1700 && r < 1800) d.humanVelocity = r - 1700;
            else if (r == 1800) { loadPhraseFile (index, def); return; }
            else if (r >= 1900 && r <= 1904) { const int roots[] = { -1, 60, 48, 72, -2 }; d.phraseRoot = roots[r - 1900]; }
            else if (r >= 2100 && r < 2100 + (int) libraryFiles.size())
            {
                const auto f = libraryFiles[(size_t) (r - 2100)];
                String error;
                if (! MidiFx::importPhrase (f, d, error)) { owner.showStatus (error); return; }
                const bool drums = f.getParentDirectory().getFileName() == "Drums";
                d.phraseRoot = drums ? -2 : 60;                 // written in C: C4 plays it as it is; drums stay put
                if (drums) d.phraseLoop = true;                 // a groove goes round while the key is held
            }
            else if (r == 2000) d.phraseStop = ! d.phraseStop;
            else if (r == 2001) d.phraseLoop = ! d.phraseLoop;
            engine.setBuiltInMidiEffect (a.first, a.second, s, index, d);
            owner.markDirty();
        });
    }

    void showBandChordsDialog (int index, const EffectDef& def)
    {
        /* Twelve lines, one per step of the key, each the chord that key
           plays: written as notes or as a name, whichever the player thinks
           in. Shown in the key in play, so the words match the chart. */
        const auto a = address();
        const int s = ProgramDef::slotOfChain (slot);
        const auto eff = engine.effectiveEffectDef (def);
        const int major = MidiFx::bandMajorRoot (eff.scaleRoot, eff.scaleMode);
        const bool flats = MidiFx::keyPrefersFlats (eff.scaleRoot, eff.scaleMode);
        const String key = MidiFx::keyName (eff.scaleRoot, eff.scaleMode);
        const bool relative = major != eff.scaleRoot;
        auto* w = new AlertWindow ("Big band chords",
                                   "The chord each key plays, in " + key + (relative ? " (its chords are " + String (MidiFx::noteNames[major]) + " major's)" : String())
                                     + ". Write each chord as notes with their octaves, the key pressed being octave 4 -- G3 C4 E4 A4 puts the G under the key, "
                                       "E4 G4 A4 C5 puts the key on top -- or as bare notes that climb from the key (C E G A), or as a name (C6, Dm7, G7, Bm7b5, C6/E). "
                                       "A blank step plays the key alone. The same chords serve every key: change the song's key and they move with it.",
                                   MessageBoxIconType::NoIcon);
        for (int r = 0; r < 12; ++r)
        {
            const int rowRoot = (major + r) % 12;
            const auto& row = (size_t) r < def.bandChords.size() ? def.bandChords[(size_t) r] : std::vector<int>();
            const String name = MidiFx::bandChordName (rowRoot, row, flats);
            const String notes = MidiFx::bandChordNotes (rowRoot, row, flats);
            w->addTextEditor ("c" + String (r), notes, String (MidiFx::bandDegrees[r]) + "  (" + MidiFx::bandChordNotes (rowRoot, { 0 }, flats, false) + ")"
                                                            + (name.isNotEmpty() && name != notes ? "   " + name : String()));
            w->getTextEditor ("c" + String (r))->setTextToShowWhenEmpty ("the key alone", textDim);
            w->getTextEditor ("c" + String (r))->getProperties().set ("shown", notes);
        }
        w->addButton ("Save", 1, KeyPress (KeyPress::returnKey));
        w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
        /* The engine and the window, not this row: a program change while the
           dialog is open rebuilds the rows and deletes this one. */
        w->enterModalState (true, ModalCallbackFunction::create ([&engine = engine, &owner = owner, w, a, s, index, def, major] (int r)
        {
            std::unique_ptr<AlertWindow> owned (w);
            if (r == 0) return;
            EffectDef d = def;
            d.bandChords.resize (12);
            StringArray bad;
            for (int i = 0; i < 12; ++i)
            {
                std::vector<int> offsets;
                const auto text = w->getTextEditorContents ("c" + String (i)).trim();
                // A row left as shown keeps exactly what it had: reading "C5" back could make a lone C a power chord.
                if (text == w->getTextEditor ("c" + String (i))->getProperties()["shown"].toString()) continue;
                if (MidiFx::parseBandChord (text, (major + i) % 12, offsets)) d.bandChords[(size_t) i] = offsets;
                else bad.add (String (MidiFx::bandDegrees[i]) + " (\"" + text + "\")");
            }
            engine.setBuiltInMidiEffect (a.first, a.second, s, index, d);
            owner.markDirty();
            if (! bad.isEmpty()) owner.showStatus ("Could not read the chord on " + bad.joinIntoString (", ") + ": kept as it was. Write notes (D F A C) or a name (Dm7).");
        }), false);
    }

    void loadPhraseFile (int index, const EffectDef& def)
    {
        const auto a = address();
        const int s = ProgramDef::slotOfChain (slot);
        chooser = std::make_unique<FileChooser> ("A MIDI file for the phrase", File::getSpecialLocation (File::userHomeDirectory), "*.mid;*.midi;*.MID");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [&engine = engine, &owner = owner, a, s, index, def] (const FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f == File()) return;
            EffectDef d = def;
            String error;
            if (! MidiFx::importPhrase (f, d, error)) { owner.showStatus (error); return; }
            engine.setBuiltInMidiEffect (a.first, a.second, s, index, d);
            owner.markDirty();
            owner.showStatus ("Phrase: " + MidiFx::describe (d) + ". Each key plays it, moved to the key pressed.");
        });
    }

    void showAddMenu()
    {
        Array<PluginDescription> effects, all;
        for (auto& d : engine.getPluginHost().getKnownPlugins().getTypes())
        {
            all.add (d);
            if (! d.isInstrument) effects.add (d);
        }
        const bool slotChain = slot >= 0;      // a slot's "+": its MIDI chain's blocks too
        if (effects.isEmpty() && ! slotChain)
        {
            owner.showStatus ("No effect plugins known yet. Use Plugins... to scan for VST3, LV2 or CLAP plugins.");
            return;
        }
        constexpr int midiBase = 100000, sendBase = 200000;
        PopupMenu menu;
        const auto ports = midiOutputPorts();
        if (! slotChain)
            addPluginsToMenu (menu, effects, engine.getPluginHost().getIcons());
        else
        {
            /* Three things, said plainly: what they do, not what they are. */
            PopupMenu fxMenu, procMenu, sendMenu;
            addPluginsToMenu (fxMenu, effects, engine.getPluginHost().getIcons());
            if (effects.isEmpty()) fxMenu.addItem (99999, "No effect plugins known yet: scan under Plugins...", false);
            /* Any plugin can be a MIDI effect: arpeggiators and chord
               generators are filed as instruments as often as effects, and a
               plugin that plays no MIDI simply hands nothing on. */
            procMenu.addSectionHeader ("Built in");
            procMenu.addItem (300001, "Arpeggiator  (held notes played one at a time, in time)");
            procMenu.addItem (300002, "Chord  (every note becomes a chord)");
            procMenu.addItem (300003, "Scale  (wrong notes bent to a scale)");
            procMenu.addItem (300004, "Run  (a scale run from each key, as a string section plays one)");
            procMenu.addItem (300005, "Big band  (each key plays the chord on that step of the key: C6, Dm7, G7)");
            procMenu.addItem (300006, "Humanize  (notes land a little late, a little louder or softer, as a player's do)");
            procMenu.addItem (300007, "Phrase  (each key plays a short MIDI file, moved to the key pressed)");
            procMenu.addSectionHeader ("Plugins");
            addPluginsToMenu (procMenu, all, engine.getPluginHost().getIcons(), midiBase);
            if (all.isEmpty()) procMenu.addItem (99998, "No plugins known yet: scan under Plugins...", false);
            addPortItems (sendMenu, ports, sendBase);
            menu.addSubMenu ("Effect on the sound (reverb, EQ, amp...)", fxMenu);
            menu.addSubMenu ("MIDI effect on the notes (arpeggiator, chords...)", procMenu);
            menu.addSubMenu ("Send to a MIDI device too (hardware synth, lights)", sendMenu);
        }

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&addBtn), [this, effects, all, ports] (int result)
        {
            const auto a = address();
            if (result >= 300001 && result <= 300007)
            {
                const auto kind = result == 300001 ? EffectDef::Kind::arpeggio : result == 300002 ? EffectDef::Kind::chord
                                : result == 300003 ? EffectDef::Kind::scale : result == 300005 ? EffectDef::Kind::bigBand
                                : result == 300006 ? EffectDef::Kind::humanize : result == 300007 ? EffectDef::Kind::phrase : EffectDef::Kind::run;
                engine.addBuiltInMidiEffect (a.first, a.second, slot, kind);
                owner.showStatus (kind == EffectDef::Kind::phrase ? "Added. Settings... on its row loads the MIDI file it plays."
                                                                  : "Added. Settings... on its row sets it up; it works at the rig's tempo.");
                owner.markDirty();
                return;
            }
            if (result >= sendBase && result < sendBase + ports.size())
            {
                engine.addMidiSend (a.first, a.second, slot, ports[result - sendBase]);
                owner.showStatus ("This instrument's notes now also go to " + ports[result - sendBase].name + ". Click Device... on that row to set its channel and patch.");
                owner.markDirty();
                return;
            }
            const bool midi = result >= midiBase && result < midiBase + all.size();
            const auto& types = midi ? all : effects;
            const int idx = midi ? result - midiBase : result - 1;
            if (idx < 0 || idx >= types.size()) return;
            String error;
            const int target = midi ? ProgramDef::midiChainOf (slot) : slot;
            if (! engine.addEffect (a.first, a.second, target, types[idx], error))
                owner.showStatus ("Could not load " + types[idx].name + ": " + error);
            else if (MainComponent::looksLikeAraPlugin (types[idx]))
                owner.showStatus (araWarning (types[idx].name));
            else
                owner.showStatus (midi ? "Added MIDI effect " + types[idx].name + ": this instrument's notes go through it before the sound"
                                       : "Added effect " + types[idx].name);
            owner.markDirty();
        });
    }

    Engine& engine;
    MainComponent& owner;
    int slot;
    TextButton addBtn { "+ Add effect..." };
    std::vector<std::unique_ptr<Row>> rows;
    std::unique_ptr<FileChooser> chooser;
    std::vector<File> libraryFiles;      // the built-in phrases, as the Settings menu last listed them

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
        addBtn.setTooltip ("Add an output (a pair of interface channels)");
        removeBtn.setTooltip ("Remove this output; its inputs move to the first one");
        addBtn.onClick = [this]
        {
            const auto names = engine.getAudioOutputChannelNames();
            const int n = (int) engine.getSetup().outputs.size();
            const int idx = engine.addOutput ("Output " + String (n + 1), jmin (2 * n, 62), jmin (2 * n + 1, 63));
            owner.markDirty();
            list.updateContent();
            list.selectRow (idx);
            if (engine.getAudioOutputChannelNames().size() < 2 * n + 2)
                owner.showStatus ("Your interface has not opened those channels. Open Audio..., tick more output channels, then pick them here.");
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
        channelsBox.setTooltip ("Which interface channels this output plays through. Only channels ticked in Audio... are offered.");
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
            if (engine.isOutputVolumeLearnArmed()) { engine.armOutputVolumeLearn (-1); learnBtn.setButtonText ("Learn volume pedal"); }
            else { engine.armOutputVolumeLearn (s); learnBtn.setButtonText ("Move it now..."); }
        };
        addAndMakeVisible (clearCcBtn);
        clearCcBtn.setTooltip ("Stop a controller moving this volume");
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
        ccLabel.setText (def.volumeCC > 0 ? "Volume follows controller " + String (def.volumeCC) : "No volume controller", dontSendNotification);
        learnBtn.setButtonText (engine.isOutputVolumeLearnArmed() ? "Move it now..." : "Learn volume pedal");

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
                    + String (users) + (users == 1 ? " input" : " inputs") + "   \xc2\xb7   " + String (d.chain.effects.size()) + (d.chain.effects.size() == 1 ? " effect" : " effects"),
                    8, 21, w - 16, 16, Justification::centredLeft, true);
    }
    void selectedRowsChanged (int) override { refresh(); }

private:
    // Engine::Listener
    // Deferred, as MainComponent defers: a chain row's button may be the one asking.
    void outputsChanged() override { MessageManager::callAsync ([self = SafePointer<OutputsComponent> (this)] { if (self != nullptr) self->refresh(); }); }
    void setupChanged() override { refresh(); }
    void outputVolumeLearned (int output, int cc) override
    {
        owner.showStatus ("Controller " + String (cc) + " now controls the volume of output " + engine.getSetup().outputs[(size_t) jlimit (0, (int) engine.getSetup().outputs.size() - 1, output)].name + ".");
        owner.markDirty();
        refresh();
    }

    Engine& engine;
    MainComponent& owner;
    ListBox list;
    TextButton addBtn { "+ Add" }, removeBtn { "- Remove" };
    Label nameLabel { {}, "Name" }, channelsLabel { {}, "Interface channels" }, volumeLabel { {}, "Master volume" },
          ccLabel, chainLabel { {}, "MASTER EFFECTS  (on everything that lands on this output)" };
    TextEditor nameEditor;
    ComboBox channelsBox;
    Slider volume;
    TextButton learnBtn { "Learn volume pedal" }, clearCcBtn { "Forget pedal" };
    EffectChainComponent chain;
};

//==============================================================================
class SlotsPanel : public Component
{
public:
    static constexpr int slotHeaderHeight = 118;

    struct SlotRow : public Component
    {
        SlotRow (SlotsPanel& p, int idx) : panel (p), index (idx), midiChain (p.engine, p.owner, ProgramDef::midiChainOf (idx)), chain (p.engine, p.owner, idx)
        {
            addAndMakeVisible (enabled);
            enabled.setTooltip ("On. Off silences this instrument but keeps it loaded");
            addAndMakeVisible (icon);
            addAndMakeVisible (name);
            // A click on the row itself (its name, its icon, the space between) plays this instrument on the on-screen keyboard.
            name.setInterceptsMouseClicks (false, false);
            icon.setInterceptsMouseClicks (false, false);
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
            chCap.setText ("TO CHANNEL", dontSendNotification);
            inChCap.setText ("FROM CHANNEL", dontSendNotification);
            panCap.setText ("PAN", dontSendNotification);
            curveCap.setText ("VELOCITY CURVE", dontSendNotification);

            pan.setRange (-1.0, 1.0, 0.01);
            pan.setSliderStyle (Slider::LinearBar);
            pan.textFromValueFunction = [] (double v) { const int n = (int) std::lround (std::abs (v) * 100.0); return n == 0 ? String ("centre") : (v < 0 ? "L " : "R ") + String (n); };
            pan.valueFromTextFunction = [] (const String& t) { const double n = t.retainCharacters ("0123456789").getDoubleValue() / 100.0; return t.containsIgnoreCase ("L") ? -n : (t.containsIgnoreCase ("R") ? n : 0.0); };
            pan.setDoubleClickReturnValue (true, 0.0);
            pan.updateText();     // the bar's text box was filled before the formatter existed
            pan.setTooltip ("Where this instrument sits left to right. Double-click for centre.");

            velRange.setRange (1, 127, 1);
            velRange.setMinAndMaxValues (1, 127, dontSendNotification);
            velRange.setPopupDisplayEnabled (true, false, this);
            velRange.setTooltip ("Only notes played within this velocity range reach this instrument. Two instruments whose ranges meet switch sounds by how hard you play.");

            velCurve.setRange (-1.0, 1.0, 0.05);
            velCurve.setSliderStyle (Slider::LinearBar);
            velCurve.textFromValueFunction = [] (double v) { const int n = (int) std::lround (std::abs (v) * 100.0); return n == 0 ? String ("linear") : (v > 0 ? "+" + String (n) + " % louder" : String (n) + " % softer"); };
            velCurve.valueFromTextFunction = [] (const String& t) { const double n = t.retainCharacters ("0123456789").getDoubleValue() / 100.0; return t.containsIgnoreCase ("soft") || t.startsWith ("-") ? -n : n; };
            velCurve.setDoubleClickReturnValue (true, 0.0);
            velCurve.updateText();     // the bar's text box was filled before the formatter existed
            velCurve.setTooltip ("Reshapes incoming velocities: louder makes soft playing come out stronger (for a stiff keyboard or a quiet library), softer does the opposite. Double-click for linear.");
            addAndMakeVisible (guiBtn);
            addAndMakeVisible (removeBtn);
            removeBtn.setTooltip ("Remove this instrument from the program");
            addAndMakeVisible (midiChain);
            addAndMakeVisible (chain);

            name.setFont (FontOptions (14.0f, Font::bold));
            name.setColour (Label::textColourId, Colours::white);

            gain.setRange (-60.0, 12.0, 0.1);
            gain.setSliderStyle (Slider::LinearBar);
            gain.setTextValueSuffix (" dB");
            gain.setDoubleClickReturnValue (true, 0.0);
            gain.setTooltip ("Level of this instrument, after its effects. Drag, or double-click for 0 dB.");

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
            keyRange.setTooltip ("Which keys reach this instrument. Drag the two handles to make a split: notes outside the range do not play it.");

            outCh.addItem ("Same channel", 1);
            for (int c = 1; c <= 16; ++c) outCh.addItem ("Channel " + String (c), c + 1);
            inCh.setTooltip ("Which MIDI channels this instrument takes notes from. Normally all of them. Pick channels when one keyboard "
                             "sends its zones on different channels, or to give each part of a MIDI file its own sound.");
            outCh.setTooltip ("Which MIDI channel the instrument itself is on. Normally the same channel the notes came in on. "
                              "Set one for a hardware synth, or a multitimbral plugin like Kontakt with a sound per channel.");

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
            inCh.onClick      = [this] { showListensToMenu(); };
            guiBtn.onClick    = [this, &e, &o] { if (external)    showExternalMenu();
                                                 else if (gm && alive) showGmSoundMenu();
                                                 else if (alive) o.openPluginEditor (o.getSelectedInput(), o.getEditedProgram(), index, -1);
                                                 else       e.reloadPlugin (o.getSelectedInput(), o.getEditedProgram(), index, -1); };
            removeBtn.onClick = [this, &e, &o]
            {
                const int in = o.getSelectedInput(), p = o.getEditedProgram(), s = index;
                confirm ("Remove instrument", "Remove " + slotDef.displayName() + " from this program?\n\nIts settings and effects go with it. There is no undo.", "Remove",
                         [&e, in, p, s] { e.removeSlot (in, p, s); });
            };
        }

        void update (const SlotDef& def, bool loaded, bool loading, const String& error, int inputIndex, int program)
        {
            alive = loaded;
            guiBtn.setVisible (true);
            enabled.setToggleState (def.enabled, dontSendNotification);
            external = def.isExternal();
            gm = def.isGm();
            inputSlot = def.isInput();
            PluginDescription forIcon = def.plugin;
            if (external)  { forIcon = {}; forIcon.name = def.midiOutName; forIcon.pluginFormatName = "MIDI"; }   // a badge with the port's initials
            if (gm)        { forIcon = {}; forIcon.name = "General MIDI"; forIcon.pluginFormatName = "GM"; }
            if (inputSlot) { forIcon = {}; forIcon.name = def.targetInputName; forIcon.pluginFormatName = "Keys"; }
            icon.setImage (panel.engine.getPluginHost().getIcons().get (forIcon, 48), RectanglePlacement::centred);
            icon.setAlpha (def.enabled ? 1.0f : 0.45f);
            if (external)
            {
                String what = def.midiOutName + "  [MIDI device";
                if (def.patch >= 0) what << ", patch " << (def.patch + 1);
                what << (def.returnLeft >= 0 ? ", audio in " + String (def.returnLeft + 1) + (def.returnRight >= 0 ? "+" + String (def.returnRight + 1) : String()) : String()) << "]";
                name.setText (what, dontSendNotification);
                name.setColour (Label::textColourId, loaded ? Colours::white : Colours::orangered);
                name.setTooltip (loaded ? "A MIDI device (hardware synth, module, lights) played through this MIDI port" : error);
            }
            else if (gm)
            {
                name.setText ("General MIDI player: " + gmSoundName (def) + (loading ? "   loading..." : String()), dontSendNotification);
                name.setColour (Label::textColourId, error.isNotEmpty() ? Colours::orangered : (loading ? textDim : Colours::white));
                name.setTooltip (error.isNotEmpty() ? error : "The built-in General MIDI player: the standard 128 sounds and drum kits. Sound... picks the one this plays; "
                                                             "\"As the notes say\" lets the program changes in the notes choose, which is how a MIDI file's parts work (each part's sound is then set in the song's Backing track window).");
            }
            else if (inputSlot)
            {
                name.setText (def.targetProgram >= 0 ? def.targetInputName + ": " + def.targetProgramName + "  [that keyboard's program " + String (def.targetProgram).paddedLeft ('0', 3) + "]"
                                                     : def.targetInputName + "'s sounds  [whatever that keyboard is playing]", dontSendNotification);
                name.setColour (Label::textColourId, error.isNotEmpty() ? Colours::orangered : Colours::white);
                name.setTooltip (error.isNotEmpty() ? error : def.targetProgram >= 0 ? "The notes this gets are played by that program of that keyboard, whatever the keyboard itself is on: a layer from another keyboard, or a MIDI file part."
                                                                                      : "The notes this gets are played by whatever that keyboard is playing right now: a layer from another keyboard, or a MIDI file part.");
            }
            else
            {
                name.setText (def.plugin.name + "  [" + def.plugin.pluginFormatName + "]" + (loading ? "   loading..." : String()), dontSendNotification);
                name.setColour (Label::textColourId, error.isNotEmpty() ? Colours::orangered : (loading ? textDim : Colours::white));
                name.setTooltip (error.isNotEmpty() ? error : "File: " + def.plugin.fileOrIdentifier);
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
            inCh.setButtonText (def.listensToText());
            // A keyboard's instruments rarely need the channel controls; they stay for the multi-channel cases.
            inCh.setColour (TextButton::textColourOffId, def.hearsAll() ? textDim : Colours::white);
            slotDef = def;
            if (external)
            {
                guiBtn.setButtonText ("Device...");
                guiBtn.setTooltip ("MIDI port, patch recall and audio return");
                guiBtn.setEnabled (true);
                guiBtn.setColour (TextButton::buttonColourId, getLookAndFeel().findColour (TextButton::buttonColourId));
            }
            else if (gm)
            {
                // Running, its button picks the sound; not running, it retries the soundfont.
                guiBtn.setButtonText (loading ? "Loading" : loaded ? "Sound..." : "Retry");
                guiBtn.setTooltip (loaded ? "Pick the General MIDI sound this plays" : "The player stopped: load it again");
                guiBtn.setEnabled (! loading);
                guiBtn.setVisible (true);
                guiBtn.setColour (TextButton::buttonColourId, loaded || loading ? getLookAndFeel().findColour (TextButton::buttonColourId) : Colours::darkred);
            }
            else if (inputSlot)
                guiBtn.setVisible (false);
            else
            {
                guiBtn.setButtonText (loading ? "Loading" : loaded ? "Window" : "Reload");
                guiBtn.setTooltip (loaded ? "Open the plugin's own window" : "The plugin stopped: load it again");
                guiBtn.setEnabled (! loading);
                guiBtn.setColour (TextButton::buttonColourId, loaded || loading ? getLookAndFeel().findColour (TextButton::buttonColourId) : Colours::darkred);
            }
            midiChain.rebuild (def.midiEffects, inputIndex, program);
            chain.rebuild (def.effects, inputIndex, program);
        }

        void mouseDown (const MouseEvent&) override { panel.owner.playInstrumentFromKeyboard (index); }

        int midiChainHeight() const { const int h = midiChain.preferredHeight(); return h > 0 ? h + 2 : 0; }
        int preferredHeight() const { return slotHeaderHeight + midiChainHeight() + chain.preferredHeight() + 4; }

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

            // In signal order: the notes' MIDI chain, then (the sound, above) its effects.
            r.removeFromTop (6);
            if (const int mh = midiChainHeight(); mh > 0)
                midiChain.setBounds (r.removeFromTop (mh).withTrimmedLeft (24).withTrimmedBottom (2));
            chain.setBounds (r.withTrimmedLeft (24));
        }

        /** Which channels this instrument plays: all, or any set of them. */
        void showListensToMenu()
        {
            PopupMenu m;
            m.addItem (1, "All channels", true, slotDef.hearsAll());
            m.addSeparator();
            for (int c = 1; c <= 16; ++c)
                m.addItem (1 + c, "Channel " + String (c), true, ! slotDef.hearsAll() && slotDef.hears (c));
            m.showMenuAsync (PopupMenu::Options().withTargetComponent (inCh), [this] (int r)
            {
                if (r < 1) return;
                auto& e = panel.engine;
                auto& o = panel.owner;
                int mask = slotDef.listensTo;
                if (r == 1) mask = SlotDef::kAllChannels;
                else
                {
                    const int bit = 1 << (r - 2);
                    // From "all", picking one channel means that one only; otherwise it toggles.
                    mask = slotDef.hearsAll() ? bit : (mask ^ bit);
                    if (mask == 0) mask = SlotDef::kAllChannels;
                }
                e.setSlotListensTo (o.getSelectedInput(), o.getEditedProgram(), index, mask);
                o.markDirty();
                panel.refresh();
            });
        }

        /** An external instrument's port, patch recall and audio return. */
        /** The General MIDI player's sound on the channels this slot takes
            notes from: one name when they agree, as a keyboard's do. */
        static String gmSoundName (const SlotDef& def)
        {
            int program = -2;
            bool drums = true;
            for (int c = 1; c <= 16; ++c)
            {
                if (! def.hears (c)) continue;
                const int p = def.gmChannels[(size_t) c - 1].program;
                if (c != 10) drums = false;
                if (program == -2) program = p;
                else if (program != p) return "mixed sounds";
            }
            if (program == -2) return "no channel";
            if (program < 0)   return "as the notes say";
            return drums ? gmKitName (program) : String (GmSynth::programName (program));
        }

        /** Sound...: the General MIDI instrument for the channels this slot hears
            (a kit for channel 10), or back to the program changes in the notes. */
        void showGmSoundMenu()
        {
            const auto def = slotDef;
            int current = -2;
            for (int c = 1; c <= 16; ++c)
                if (def.hears (c) && c != 10) { current = def.gmChannels[(size_t) c - 1].program; break; }
            PopupMenu m;
            m.addItem (1, "As the notes say  (program changes choose, as a MIDI file's do)", true, current == -1);
            m.addSeparator();
            addGmInstrumentsToMenu (m, 1000, current);
            if (def.hears (10))
            {
                PopupMenu kits;
                addGmKitsToMenu (kits, 2000, def.gmChannels[9].program);
                m.addSubMenu ("Drum kit (channel 10)", kits);
            }
            auto& e = panel.engine;
            auto& o = panel.owner;
            auto& p = panel;
            const int in = o.getSelectedInput(), prog = o.getEditedProgram(), slot = index;
            m.showMenuAsync (PopupMenu::Options().withTargetComponent (&guiBtn), [&e, &o, &p, in, prog, slot, def] (int r)
            {
                if (r < 1) return;
                for (int c = 1; c <= 16; ++c)
                {
                    if (! def.hears (c)) continue;
                    if (r >= 2000 && c != 10) continue;            // a kit is channel 10's
                    if (r >= 1000 && r < 2000 && c == 10) continue; // an instrument is the others'
                    auto gc = def.gmChannels[(size_t) c - 1];
                    gc.program = r == 1 ? -1 : r >= 2000 ? r - 2000 : r - 1000;
                    e.setGmChannel (in, prog, slot, c, gc);
                }
                o.markDirty();
                p.refresh();          // last: it rebuilds the rows, this one included
            });
        }

        void showExternalMenu()
        {
            auto& e = panel.engine;
            auto& o = panel.owner;
            const int in = o.getSelectedInput(), prog = o.getEditedProgram();
            const auto ports = midiOutputPorts();
            const auto inputs = e.getAudioInputChannelNames();

            PopupMenu portMenu;
            addPortItems (portMenu, ports, 100, slotDef.midiOutIdentifier, slotDef.midiOutName);
            PopupMenu returnMenu;
            returnMenu.addItem (300, "None: its sound goes to the mixer on its own, not through StageRig", true, slotDef.returnLeft < 0);
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
            auto& e = panel.engine;
            auto& o = panel.owner;
            const int in = o.getSelectedInput(), prog = o.getEditedProgram();
            showPatchRecallDialog (slotDef.midiOutName, slotDef.bankMsb, slotDef.bankLsb, slotDef.patch, [this, &e, &o, in, prog] (int msb, int lsb, int patch)
            {
                e.setSlotPatch (in, prog, index, msb, lsb, patch);
                o.markDirty();
                panel.refresh();
            });
        }

        SlotsPanel& panel;
        int index;
        bool alive = false, external = false, gm = false, inputSlot = false;
        SlotDef slotDef;
        ImageComponent icon;
        ToggleButton enabled;
        Label name;
        Slider gain, transpose, pan, velCurve, keyRange { Slider::TwoValueHorizontal, Slider::NoTextBox }, velRange { Slider::TwoValueHorizontal, Slider::NoTextBox };
        Label gainCap, transposeCap, keysCap, chCap, inChCap, panCap, velCap, curveCap;
        ComboBox outCh;
        TextButton inCh { "All channels" };
        TextButton guiBtn { "Window" }, removeBtn { "X" };
        EffectChainComponent midiChain, chain;
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
        Label header { {}, "PROGRAM EFFECTS  (on everything in this program)" };
        EffectChainComponent chain;
    };

    SlotsPanel (Engine& e, MainComponent& o) : engine (e), owner (o)
    {
        styleHeader (header);
        addAndMakeVisible (header);
        addAndMakeVisible (addBtn);
        addBtn.setTooltip ("A plugin, a MIDI device, the built-in General MIDI player, or another keyboard's program");
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
           instrument. Its chain's own "+ Add effect" is the way in. The
           backing track's instruments are the song's parts' -- say where
           they are chosen, so nobody builds them here by hand. */
        const bool trackInput = in >= 0 && in < (int) inputs.size() && inputs[(size_t) in].isTrack();
        header.setText (audioInput ? "EFFECTS" : trackInput ? "INSTRUMENTS  (set per part in the song's Backing track window)"
                                                            : "INSTRUMENTS", dontSendNotification);
        addBtn.setVisible (! audioInput);
        meter.setVisible (audioInput);
        trimCap.setVisible (audioInput);
        trim.setVisible (audioInput);
        emptyLabel.setText (audioInput ? "No effects in this program. The signal passes straight through; add an effect below."
                            : trackInput ? "No parts yet. Add a MIDI file to the song (Backing track... under the songs) and pick what plays each part there."
                                         : "No instruments in this program. Click \"Add instrument...\"", dontSendNotification);
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
        /* The kinds of instrument: a plugin; a MIDI device -- a hardware
           synth or module, a lighting controller -- played through a MIDI
           port, with everything a plugin slot does (splits, layers, curves,
           MIDI effects); the built-in General MIDI player; or another
           keyboard's sounds, for a layer from one keyboard to the other or a
           MIDI file part played by what that keyboard is on. */
        Array<PluginDescription> types;
        for (auto& d : engine.getPluginHost().getKnownPlugins().getTypes())
            if (d.isInstrument) types.add (d);
        if (types.isEmpty()) types = engine.getPluginHost().getKnownPlugins().getTypes();   // nothing filed as one: offer what there is
        PopupMenu plugins;
        addPluginsToMenu (plugins, types, engine.getPluginHost().getIcons());
        if (types.isEmpty()) plugins.addItem (99998, "No plugins known yet: scan under Plugins...", false);

        const auto ports = midiOutputPorts();
        PopupMenu external;
        addPortItems (external, ports, 100000);

        const auto& inputs = engine.getSetup().inputs;
        const int self = owner.getSelectedInput();
        PopupMenu others;
        for (int i = 0; i < (int) inputs.size(); ++i)
        {
            const auto& in = inputs[(size_t) i];
            if (i == self || in.isAudio() || in.isTrack()) continue;
            // Its programs by name, as its pages have them, then whatever it is playing.
            PopupMenu programs;
            String group;
            for (int k = 0; k < (int) in.programs.size(); ++k)
            {
                const auto& pd = in.programs[(size_t) k];
                if (pd.isEmpty() && pd.name.isEmpty()) continue;
                if (pd.group.isNotEmpty() && pd.group != group) { programs.addSectionHeader (pd.group); group = pd.group; }
                programs.addItem (210000 + i * 128 + k, String (k).paddedLeft ('0', 3) + "  " + (pd.name.isNotEmpty() ? pd.name : String ("(unnamed)")));
            }
            if (programs.getNumItems() > 0) programs.addSeparator();
            programs.addItem (200000 + i, "Whatever " + in.name + " is playing");
            others.addSubMenu (in.name, programs);
        }
        if (others.getNumItems() == 0) others.addItem (199999, "No other keyboard", false);

        PopupMenu menu;
        menu.addSubMenu ("Plugin instrument", plugins);
        menu.addSubMenu ("MIDI device (hardware synth, module, lights)", external);
        menu.addItem (300000, GmSynth::isAvailable() ? String ("General MIDI player (built in)")
                                                     : "General MIDI player (built in)  -- " + GmSynth::unavailableReason (engine.getGmSoundfont()), GmSynth::isAvailable());
        menu.addSubMenu ("Another keyboard's program", others);

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&addBtn), [this, types, ports] (int result)
        {
            const int in = owner.getSelectedInput(), prog = owner.getEditedProgram();
            if (result >= 100000 && result < 100000 + ports.size())
            {
                engine.addExternalSlot (in, prog, ports[result - 100000]);
                owner.showStatus ("Added " + ports[result - 100000].name + ". Click Device... on its row to set its channel, patch and audio return.");
                owner.markDirty();
                return;
            }
            if (result == 300000)
            {
                engine.addGmSlot (in, prog);
                owner.showStatus ("Added the General MIDI player: the standard 128 sounds, chosen by the program changes in the notes. Sound... picks one instead.");
                owner.markDirty();
                return;
            }
            if (result >= 200000 && result < 200000 + (int) engine.getSetup().inputs.size())
            {
                engine.addInputSlot (in, prog, result - 200000);
                owner.showStatus ("Added " + engine.getSetup().inputs[(size_t) (result - 200000)].name + "'s sounds: what this gets is played by whatever that keyboard is on");
                owner.markDirty();
                return;
            }
            if (result >= 210000 && result < 210000 + 128 * (int) engine.getSetup().inputs.size())
            {
                const int target = (result - 210000) / 128, program = (result - 210000) % 128;
                if (engine.addInputSlot (in, prog, target, program))
                    owner.showStatus ("Added " + engine.getSetup().inputs[(size_t) target].name + ": " + engine.getSetup().inputs[(size_t) target].programs[(size_t) program].name
                                      + " -- that program plays what this gets, whatever the keyboard itself is on");
                owner.markDirty();
                return;
            }
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
    Label header { {}, "INSTRUMENTS" };
    TextButton addBtn { "+ Add instrument..." };
    Viewport viewport;
    Component container;
    std::vector<std::unique_ptr<SlotRow>> rows;
    std::unique_ptr<ProgramChainRow> programChain;
    Label emptyLabel { {}, "No instruments in this program. Click \"Add instrument...\"" };
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
                        ? "No suggestions for " + targetName + ": its parameter names do not look like any standard controller, and no template has been saved for it. Map it once with Learn MIDI, then press Save template so Suggest... can offer those mappings next time."
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
        th.addColumn ("Controller", colSource, 90);
        th.addColumn ("Instrument / effect", colTarget, 150);
        th.addColumn ("Parameter", colParam, 220);
        th.addColumn ("Min", colMin, 50);
        th.addColumn ("Max", colMax, 50);
        th.addColumn ("Pass on", colPass, 44);
        th.setStretchToFitActive (true);

        // Editor row 1: target
        addAndMakeVisible (targetBox);
        addAndMakeVisible (paramBox);
        addAndMakeVisible (touchedBtn);
        targetBox.setTooltip ("Which instrument or effect the controller will move");
        targetBox.onChange = [this] { fillParams(); };
        paramBox.onChange  = [this] { updateButtons(); };
        touchedBtn.onClick = [this] { useTouched(); };
        touchedBtn.setTooltip ("Pick the parameter you last moved in a plugin's window");

        // Editor row 2: source + range
        addAndMakeVisible (learnBtn);
        addAndMakeVisible (sourceLabel);
        addAndMakeVisible (minSlider);
        addAndMakeVisible (maxSlider);
        addAndMakeVisible (passToggle);
        learnBtn.setClickingTogglesState (true);
        learnBtn.setTooltip ("Press, then move the knob, pedal or wheel on your keyboard");
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
        passToggle.setTooltip ("Also let the controller message itself reach the plugin. Normally a mapped controller is used up by the mapping.");

        // Editor row 3: actions
        addAndMakeVisible (addBtn);
        addAndMakeVisible (updateBtn);
        addAndMakeVisible (removeBtn);
        addAndMakeVisible (suggestBtn);
        addAndMakeVisible (templateBtn);
        addBtn.setTooltip ("Add this mapping to the program");
        updateBtn.setTooltip ("Change the selected mapping to what is set above");
        removeBtn.setTooltip ("Delete the selected mapping");
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
            // Every instrument and effect: a plugin's parameters, and the thing itself (on, off, its level) for any of them.
            for (int s = 0; s < (int) def->slots.size(); ++s)
            {
                targets.push_back ({ s, -1, String (s + 1) + ": " + def->slots[(size_t) s].displayName() });
                const auto& midiChain = def->slots[(size_t) s].midiEffects;
                for (int e = 0; e < (int) midiChain.size(); ++e)
                    targets.push_back ({ ProgramDef::midiChainOf (s), e, String (s + 1) + " > MIDI effect " + String (e + 1) + ": " + midiChain[(size_t) e].displayName() });
                for (int e = 0; e < (int) def->slots[(size_t) s].effects.size(); ++e)
                    targets.push_back ({ s, e, String (s + 1) + " > effect " + String (e + 1) + ": " + def->slots[(size_t) s].effects[(size_t) e].displayName() });
            }
            for (int e = 0; e < (int) def->effects.size(); ++e)
                targets.push_back ({ -1, e, "Program effect " + String (e + 1) + ": " + def->effects[(size_t) e].displayName() });
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
                    touchedBtn.setButtonText ("Use last moved: " + p.name.substring (0, 24));
    }

    void paintOverChildren (Graphics& g) override
    {
        if (getNumRows() > 0) return;
        g.setColour (textDim);
        g.setFont (FontOptions (13.0f));
        g.drawFittedText ("No mappings in this program.\n\nTo make one: press Learn MIDI, move a knob or pedal on your keyboard, "
                          "pick the instrument or effect and the parameter it should move, then Add.",
                          table.getBounds().withTrimmedTop (22).reduced (24), Justification::centred, 4);
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
        if (slot < 0) return "Program effect " + String (effect + 1);
        return String (slot + 1) + (effect >= 0 ? " > effect " + String (effect + 1) : String());
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
        paramIndices.clear();
        int id = 1, reselect = 0;
        if (t != nullptr)
        {
            /* The thing itself comes first: a pedal that switches a layer or
               an effect, a knob that is its level. No plugin needed, so a
               hardware synth or the GM player can be switched the same way. */
            const bool instrument = t->effect < 0;
            auto builtIn = [&] (const String& pid, const String& name)
            {
                paramBox.addItem (name, id);
                paramIds.add (pid);
                paramIndices.add (-1);
                if (pid == prev) reselect = id;
                ++id;
            };
            builtIn ("#on",     instrument ? "On / off  (pedal down = on)" : "On / off  (pedal down = on, up = bypassed)");
            builtIn ("#toggle", "On / off, toggled by each press");
            if (instrument) builtIn ("#level", "Level  (the fader: -60 dB to +12)");
        }
        if (plugin == nullptr)
        {
            paramBox.setTextWhenNothingSelected (t != nullptr ? "(plugin not loaded)" : "(no plugin)");
            if (reselect > 0) paramBox.setSelectedId (reselect, dontSendNotification);
            updateButtons();
            return;
        }
        paramBox.setTextWhenNothingSelected ("Choose parameter...");
        paramBox.setPreferredChannel (channelInto (*t));
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
    Label header { {}, "MIDI MAPPINGS  (a knob or pedal on your keyboard moves a plugin parameter, in this program)" };
    TableListBox table;
    ComboBox targetBox;
    ParamPicker paramBox;
    std::vector<Target> targets;
    StringArray paramIds;
    Array<int> paramIndices;
    TextButton touchedBtn { "Use last moved" }, learnBtn { "Learn MIDI" }, addBtn { "Add" }, updateBtn { "Update" }, removeBtn { "Remove" },
               suggestBtn { "Suggest..." }, templateBtn { "Save template" };
    Label sourceLabel { {}, "(nothing learned)" };
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
    panicBtn.onClick       = [this] { engine.panic(); engine.stopTrack(); showStatus ("Panic: all notes off, backing track stopped"); };
    panicBtn.setColour (TextButton::buttonColourId, Colours::darkred);
    newBtn.setTooltip ("Start a new, empty setup (inputs, programs, songs: everything)");
    openBtn.setTooltip ("Open a setup file (*.stagerig.json)");
    saveBtn.setTooltip ("Save this setup");
    saveAsBtn.setTooltip ("Save this setup under a new name");
    audioBtn.setTooltip ("Audio interface, sample rate and buffer size");
    pluginsBtn.setTooltip ("Scan for plugins and manage the list");
    panicBtn.setTooltip ("All notes off, on every instrument and MIDI device");

    /* What is set up once and then left alone -- the two switches and the two
       occasional actions -- lives in a menu. As toolbar buttons they did not
       fit a 1280-wide window: the tempo readout was squashed to nothing and
       the preload checkbox hidden, on exactly the laptop this runs on. */
    addAndMakeVisible (optionsBtn);
    optionsBtn.setTooltip ("Outputs and master effects, the songs panel, preloading, note hold, MIDI rescan, a printable program list, General MIDI sounds, MIDI clock to hardware");
    optionsBtn.onClick = [this]
    {
        const auto& setup = engine.getSetup();
        PopupMenu m;
        m.addItem (11, "Pre-show check...  (everything that can go wrong, before the first song)");
        m.addSeparator();
        m.addItem (1, "Preload all programs (instant changes; uses more memory)", true, setup.preloadAllPrograms);
        m.addItem (2, "Let held notes ring through a program change", true, setup.holdAcrossChange);
        m.addItem (6, "Show songs and set lists", true, songsPanel != nullptr && songsPanel->isVisible());
        m.addSeparator();
        m.addItem (5, "Outputs & master effects...");
        m.addItem (3, "Rescan MIDI devices");
        m.addItem (4, "Print the program list (for the stand)...");
        {
            const auto sf = GmSynth::findSoundfont (engine.getGmSoundfont());
            m.addItem (7, "General MIDI sounds (soundfont file)..." + (sf != File() ? "  (" + sf.getFileName() + ")" : String ("  (none found)")));
        }
        {
            /* MIDI clock to hardware: tick a device and it keeps time with the
               rig -- a drum machine, a synth's arpeggiator, a delay pedal. */
            PopupMenu clock;
            const auto devices = engine.midiOutputDevices();
            for (int i = 0; i < devices.size(); ++i)
                clock.addItem (100 + i, devices[i].name, true, engine.sendsClockTo (devices[i].identifier));
            if (devices.isEmpty()) clock.addItem (-1, "No MIDI devices found", false);
            const int sending = (int) setup.clockOut.size();
            m.addSubMenu ("Send MIDI clock to" + (sending > 0 ? "  (" + String (sending) + (sending == 1 ? " device)" : " devices)") : String()), clock);
        }
        {
            const int peers = engine.getLinkPeers();
            m.addItem (9, "Ableton Link: share the tempo and the beat over the network"
                           + (engine.isLinkEnabled() ? (peers == 0 ? String ("  (no one else yet)") : "  (" + String (peers) + (peers == 1 ? " other)" : " others)")) : String()),
                       true, engine.isLinkEnabled());
            m.addItem (10, "Link also starts and stops the others", engine.isLinkEnabled(), engine.isLinkStartStopSync());
        }
        m.addSeparator();
        m.addItem (8, engine.isRecording() ? "Stop recording the gig  (" + formatElapsed (engine.getRecordingSeconds()) + ")"
                                            : "Record the gig  (every output to WAV, the keyboards to MIDI)");
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (optionsBtn), [this] (int r)
        {
            const auto& s = engine.getSetup();
            if (r == 8) { toggleRecording(); return; }
            if (r == 9)
            {
                engine.setLinkEnabled (! engine.isLinkEnabled());
                showStatus (engine.isLinkEnabled() ? "Link on: the tempo and the beat are shared with everything on the network that has Link. Play waits for the next shared bar."
                                                   : "Link off");
                updateTempoLabel();
                return;
            }
            if (r == 10) { engine.setLinkStartStopSync (! engine.isLinkStartStopSync()); return; }
            if (r >= 100)
            {
                const auto devices = engine.midiOutputDevices();
                if (r - 100 < devices.size())
                {
                    const auto& d = devices[r - 100];
                    engine.setClockTo (d, ! engine.sendsClockTo (d.identifier));
                    showStatus (engine.sendsClockTo (d.identifier) ? "Sending MIDI clock to " + d.name + " at the rig's tempo; Start and Stop with the backing track"
                                                                    : "No longer sending MIDI clock to " + d.name);
                }
                return;
            }
            switch (r)
            {
                case 1: engine.setPreloadAllPrograms (! s.preloadAllPrograms); break;
                case 2: engine.setHoldAcrossChange (! s.holdAcrossChange); break;
                case 3: engine.refreshMidiDevices(); showStatus ("MIDI devices rescanned"); break;
                case 4: printProgramMap(); break;
                case 5: showOutputs(); break;
                case 6: setSongsPanelShown (songsPanel == nullptr || ! songsPanel->isVisible()); break;
                case 7: chooseGmSoundfont(); break;
                case 11: showShowCheck(); break;
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
    stagePanel->onEdited = [this] { markDirty(); };
    stagePanel->onHeightChanged = [this] { resized(); };
    addChildComponent (stagePanel.get());
    addAndMakeVisible (stageBtn);
    stageBtn.setClickingTogglesState (true);
    stageBtn.setColour (TextButton::buttonOnColourId, accentDim);
    stageBtn.setTooltip ("The performance view: each input's program in large type, with a front panel of knobs for its sound -- level, pan, and the parameters a player reaches for");
    stageBtn.onClick = [this]
    {
        const bool show = stageBtn.getToggleState();
        stagePanel->setVisible (show);
        settings.setValue ("showStage", show);
        resized();
    };
    stageBtn.setToggleState (settings.getBoolValue ("showStage", false), dontSendNotification);
    stagePanel->setVisible (stageBtn.getToggleState());

    /* Perform: the editing panes go, and the performance view takes the
       window -- the program in letters a foot high, the knobs under it, the
       set list beside it. For the laptop on a stand behind the player. */
    addAndMakeVisible (performBtn);
    performBtn.setClickingTogglesState (true);
    performBtn.setColour (TextButton::buttonOnColourId, accentDim);
    performBtn.setTooltip ("Perform: hide the editing panes and give the window to the performance view and the set list, big enough to read from the stand");
    performBtn.onClick = [this]
    {
        settings.setValue ("perform", performBtn.getToggleState());
        resized();
    };
    performBtn.setToggleState (settings.getBoolValue ("perform", false), dontSendNotification);

    /* Songs and sets. Hidden by default and remembered, like Stage: it is the
       panel you want while building a set and while playing one, and in the way
       the rest of the time. */
    songsPanel = std::make_unique<SongsPanel> (engine);
    songsPanel->selectedInputProvider = [this] { return getSelectedInput(); };

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
    songsPanel->setVisible (settings.getBoolValue ("showSongs", true));

    // The songs and set lists panel has a button like Stage and Keyboard: it is where a gig is built and played.
    addAndMakeVisible (songsBtn);
    songsBtn.setClickingTogglesState (true);
    songsBtn.setColour (TextButton::buttonOnColourId, accentDim);
    songsBtn.setTooltip ("Show the songs and set lists: what every keyboard plays for each number, in order");
    songsBtn.setToggleState (songsPanel->isVisible(), dontSendNotification);
    songsBtn.onClick = [this] { setSongsPanelShown (songsBtn.getToggleState(), false); };


    addAndMakeVisible (helpBtn);
    helpBtn.setTooltip ("The user manual, without leaving the app");
    helpBtn.onClick = [] { HelpWindow::show(); };

    addAndMakeVisible (remoteBtn);
    remoteBtn.setClickingTogglesState (true);
    remoteBtn.setColour (TextButton::buttonOnColourId, accentDim);
    remoteBtn.setTooltip ("Control StageRig from a tablet on the same wifi: programs, set list, charts");
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
    tailLabel.setTooltip ("How long the previous program keeps sounding after a program change");
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

    /* Started again by the watchman after a crash (see Main.cpp): back to the
       song, scene and programs the show was on, which the timer below keeps
       in the settings as they change. The track is armed, not started --
       the band decides when it comes back in. */
    if (const char* rec = std::getenv ("STAGERIG_RECOVERED"); rec != nullptr && *rec != 0)
    {
        const auto pos = Engine::LivePosition::fromString (settings.getValue ("livePosition"));
        engine.restoreLivePosition (pos);
        const auto& songs = engine.getSetup().songs;
        showStatus ("StageRig stopped and restarted itself" + (pos.song >= 0 && pos.song < (int) songs.size()
                        ? ". Back on " + songs[(size_t) pos.song].name + "; the plugins are loading."
                        : String (". The plugins are loading.")));
        refreshAll();
    }
    lastLivePosition = engine.getLivePosition().toString();

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

    /* The first time, with nothing plugged in and nothing scanned, the window
       says where to start. Once, when it has settled; never again after that. */
    Timer::callAfterDelay (1500, [self = SafePointer<MainComponent> (this)]
    {
        if (self == nullptr) return;
        const auto& inputs = self->engine.getSetup().inputs;
        bool anyDevice = false;
        for (const auto& in : inputs) if (in.midiDeviceIdentifier.isNotEmpty() || in.isAudio()) anyDevice = true;
        if (! anyDevice && self->engine.getPluginHost().getKnownPlugins().getTypes().isEmpty())
            self->showStatus ("Start here: Audio... picks your interface, Plugins... scans for sounds, then choose each keyboard's MIDI device under INPUTS.");
    });
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
    audioBtn.setBounds (toolbar.removeFromLeft (68));       toolbar.removeFromLeft (4);
    pluginsBtn.setBounds (toolbar.removeFromLeft (70));     toolbar.removeFromLeft (4);
    optionsBtn.setBounds (toolbar.removeFromLeft (66));     toolbar.removeFromLeft (8);
    panicBtn.setBounds (toolbar.removeFromRight (76));      toolbar.removeFromRight (6);
    keyboardBtn.setBounds (toolbar.removeFromRight (74));   toolbar.removeFromRight (4);
    stageBtn.setBounds (toolbar.removeFromRight (60));      toolbar.removeFromRight (4);
    performBtn.setBounds (toolbar.removeFromRight (70));    toolbar.removeFromRight (4);
    songsBtn.setBounds (toolbar.removeFromRight (60));      toolbar.removeFromRight (4);
    tailSlider.setBounds (toolbar.removeFromRight (60));    toolbar.removeFromRight (2);
    tailLabel.setBounds (toolbar.removeFromRight (26));     toolbar.removeFromRight (8);
    tapButton.setBounds (toolbar.removeFromRight (42));     toolbar.removeFromRight (2);
    tempoLabel.setBounds (toolbar.removeFromRight (engine.getTranspose() != 0 ? 118 : 66));    toolbar.removeFromRight (8);

    auto status = r.removeFromBottom (24).reduced (8, 2);
    cpuLabel.setBounds (status.removeFromRight (470));
    fileLabel.setBounds (status.removeFromRight (320));
    statusLabel.setBounds (status);

    if (keyboardPanel != nullptr && keyboardPanel->isVisible())
    {
        keyboardPanel->setBounds (r.removeFromBottom (KeyboardPanel::preferredHeight).reduced (8, 0));
        r.removeFromBottom (8);
    }
    const bool perform = performBtn.getToggleState();
    for (auto* c : std::initializer_list<Component*> { inputsPanel.get(), programsPanel.get(), slotsPanel.get(), mappingsPanel.get(), &inputsDivider, &programsDivider })
        if (c != nullptr) c->setVisible (! perform);
    if (perform)
    {
        // The set list on the left if it is shown, the performance view on everything else.
        if (stagePanel != nullptr) stagePanel->setVisible (true);
        r.reduce (8, 0);
        const bool songsOn = songsPanel != nullptr && songsPanel->isVisible();
        songsDivider.setVisible (songsOn);
        if (songsOn)
        {
            songsPanel->setBounds (r.removeFromLeft (jlimit (SongsPanel::minimumWidth, 900, songsWidth)));
            songsDivider.setBounds (r.removeFromLeft (8));
        }
        if (stagePanel != nullptr) stagePanel->setBounds (r);
        return;
    }
    if (stagePanel != nullptr) stagePanel->setVisible (stageBtn.getToggleState());
    if (stagePanel != nullptr && stagePanel->isVisible())
    {
        stagePanel->setBounds (r.removeFromTop (stagePanel->preferredHeight()).reduced (8, 0));
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
    songsBtn.setToggleState (show, dontSendNotification);
    settings.setValue ("showSongs", show);
    settings.saveIfNeeded();
    resized();
    if (byDrag)
        showStatus ("Songs and set lists hidden. The Songs button in the toolbar brings the panel back.");
}

void MainComponent::savePaneWidths()
{
    settings.setValue ("paneSongsWidth",    songsWidth);
    settings.setValue ("paneInputsWidth",   inputsWidth);
    settings.setValue ("paneProgramsWidth", programsWidth);
    settings.saveIfNeeded();
}

String MainComponent::formatElapsed (double seconds)
{
    const int s = (int) seconds;
    return String (s / 60) + ":" + String (s % 60).paddedLeft ('0', 2);
}

void MainComponent::toggleRecording()
{
    if (engine.isRecording())
    {
        const auto folder = engine.getRecordingFolder();
        engine.stopRecording();
        showStatus ("Recording saved in " + folder.getFullPathName());
        return;
    }
    String error;
    if (engine.startRecording (error))
        showStatus ("Recording every output to " + engine.getRecordingFolder().getFullPathName() + "  (Options to stop)");
    else
        showStatus ("Could not record: " + error);
}

void MainComponent::timerCallback()
{
    PluginView::dropDeadSessions();      // a mirrored window that was closed ends its session

    /* Where the show is, kept as it changes: a StageRig restarted after a
       crash comes back exactly here. A few numbers, written only when one
       moves -- a song, a scene, a program, the tempo. */
    if (const auto live = engine.getLivePosition().toString(); live != lastLivePosition)
    {
        lastLivePosition = live;
        settings.setValue ("livePosition", live);
        settings.saveIfNeeded();
    }

    const auto now = Time::getMillisecondCounterHiRes();
    if (keyboardPanel != nullptr && keyboardPanel->isVisible()) keyboardPanel->refreshLabel();   // input name/channel may have been edited
    {
        String dev;
        if (auto* d = engine.getDeviceManager().getCurrentAudioDevice())
        {
            const int smp = engine.getLastBlockSize() > 0 ? engine.getLastBlockSize() : d->getCurrentBufferSizeSamples();
            dev = String (d->getCurrentSampleRate() / 1000.0, 1) + " kHz / " + String (smp) + " samples ("
                  + String (1000.0 * smp / d->getCurrentSampleRate(), 1) + " ms)";
        }
        else dev = "no audio device: open Audio...";
        const int wanted = settings.getBoolValue ("pipewireTakeover", true) ? settings.getIntValue ("pipewireQuantum", 128) : 0;
        const bool clockHeld = wanted > 0 && engine.getLastBlockSize() > wanted
                               && engine.getDeviceManager().getCurrentAudioDevice() != nullptr
                               && engine.getDeviceManager().getCurrentAudioDevice()->getTypeName() == "JACK";
        cpuLabel.setText ((engine.isRecording() ? "REC " + formatElapsed (engine.getRecordingSeconds()) + "  |  " : String())
                          + "CPU " + String (engine.getCpuUsage() * 100.0, 1) + "%  |  " + dev
                          + (clockHeld ? "  |  clock held by another app" : String())
                          + (engine.getLateBlockCount() > 0 ? "  |  " + String (engine.getLateBlockCount()) + " dropouts" : String()), dontSendNotification);
        cpuLabel.setTooltip (clockHeld ? "StageRig asked for a " + String (wanted) + "-sample buffer but the system is running a bigger one: another application (Bitwig, for example) has forced it. Close that app, or set its buffer to " + String (wanted) + "." : String());
        cpuLabel.setColour (Label::textColourId, (engine.getLateBlockCount() > 0 || clockHeld) ? Colours::orange
                                                 : engine.isRecording() ? Colour (0xffe05a5a) : textDim);
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
    /* Deferred: this arrives from inside the engine call that a row's button
       made (remove this effect, move it), and the refresh rebuilds the rows
       -- deleting that button under its own click handler. The handler
       finishes first; the rows follow a moment later. */
    MessageManager::callAsync ([self = SafePointer<MainComponent> (this), inputIndex, program]
    {
        if (self == nullptr || inputIndex != self->selectedInput) return;
        self->programsPanel->repaint();
        if (program == self->getEditedProgram())
        {
            self->slotsPanel->refresh();
            self->mappingsPanel->refresh();
        }
    });
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
            title = "Output " + setup.outputs[(size_t) o].name + " / effect " + String (effect + 1) + " / " + plugin->getName();
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
    auto start = [this]
    {
        engine.loadSetup (Setup::makeDefault());
        setCurrentFile ({});
        showStatus ("New setup");
    };
    if (! dirty) { start(); return; }
    auto* w = new AlertWindow ("New setup", "Start an empty setup? The current one has unsaved changes.", MessageBoxIconType::QuestionIcon);
    w->addButton ("Save first", 2, KeyPress (KeyPress::returnKey));
    w->addButton ("Discard", 1);
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([this, w, start] (int r)
    {
        std::unique_ptr<AlertWindow> owned (w);
        if (r == 0) return;
        if (r == 2) { saveSetup (false); if (dirty) return; }      // a cancelled save keeps the setup
        start();
    }), false);
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
    const int madeBy = s.loadedFormatVersion;
    engine.loadSetup (std::move (s));
    setCurrentFile (f == getAutosaveFile() ? File() : f);
    if (madeBy > Setup::formatVersion)
    {
        /* Made by a newer StageRig: whatever it has that this one does not
           know is not loaded, and saving over the file would lose it. Say so
           before anyone presses Save. */
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Made by a newer StageRig",
            f.getFileName() + " was saved by a newer version of StageRig. Anything in it this version does not know "
            "-- a newer MIDI effect, a newer setting -- was not loaded, and saving over the file would lose it.\n\n"
            "Update StageRig, or use Save As... to keep this version's copy separate.");
        overwriteWarned = false;
        newerFile = true;
    }
    else newerFile = false;
    dirty = false;
    lastAutosaveTime = Time::getMillisecondCounterHiRes();

    // Warn if an autosave is newer than the file we just opened (a previous session died).
    if (f != getAutosaveFile() && getAutosaveFile().existsAsFile()
        && getAutosaveFile().getLastModificationTime() > f.getLastModificationTime() + RelativeTime::seconds (5))
        showStatus ("Loaded " + f.getFileName() + ".  A newer autosave exists ("
                    + getAutosaveFile().getLastModificationTime().toString (false, true, false, true) + "): Open... > " + getAutosaveFile().getFileName());
    else
        showStatus ("Loaded " + f.getFileName());
}

bool MainComponent::writeSetupFile (const File& f, int captureBudgetMs)
{
    if (newerFile && f == currentFile && ! overwriteWarned && f != getAutosaveFile())
    {
        /* The first save over a newer version's file asks first: it would lose
           what this version could not load. Yes saves; the question is not asked again. */
        SafePointer<MainComponent> self (this);
        AlertWindow::showOkCancelBox (MessageBoxIconType::WarningIcon, "Save over a newer StageRig's file?",
            f.getFileName() + " was saved by a newer version of StageRig. Saving over it with this version loses anything "
            "in it this version did not load.\n\nSave As... keeps both.", "Save over it", "Cancel", nullptr,
            ModalCallbackFunction::create ([self, f, captureBudgetMs] (int r)
            {
                if (self == nullptr || r == 0) return;
                self->overwriteWarned = true;
                if (self->writeSetupFile (f, captureBudgetMs)) self->showStatus ("Saved " + f.getFileName());
            }));
        return false;
    }
    auto r = engine.captureSetup (captureBudgetMs).saveToFile (f);
    if (r.failed()) { showStatus ("Save failed: " + r.getErrorMessage()); return false; }
    if (f != getAutosaveFile()) newerFile = false;     // written by this version now: Save no longer asks
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
    // A newer version's file is never written over unasked: quitting keeps the work in the autosave instead.
    if (currentFile != File() && ! (newerFile && ! overwriteWarned)) writeSetupFile (currentFile, 8000);
    else                       writeSetupFile (getAutosaveFile(), 8000);
}

//==============================================================================
namespace
{
    /** The pre-show check: a line per finding, worst first, with a dot that
        says how much it matters, and a button to look again after fixing. */
    class ShowCheckView : public Component
    {
    public:
        explicit ShowCheckView (std::function<std::vector<Engine::CheckItem>()> run) : runCheck (std::move (run))
        {
            addAndMakeVisible (viewport);
            viewport.setViewedComponent (&list, false);
            viewport.setScrollBarsShown (true, false);
            addAndMakeVisible (againBtn);
            againBtn.onClick = [this] { refresh(); };
            addAndMakeVisible (summary);
            summary.setFont (FontOptions (17.0f, Font::bold));
            refresh();
        }

        void refresh()
        {
            items = runCheck();
            int problems = 0, warnings = 0;
            for (auto& i : items) { problems += i.level == Engine::CheckItem::problem; warnings += i.level == Engine::CheckItem::warning; }
            summary.setText (problems > 0 ? String (problems) + (problems == 1 ? " thing to fix" : " things to fix") + " before the show"
                             : warnings > 0 ? "Ready, with " + String (warnings) + (warnings == 1 ? " thing" : " things") + " to look at"
                             : "Ready to play", dontSendNotification);
            summary.setColour (Label::textColourId, problems > 0 ? Colour (0xffe05a5a) : warnings > 0 ? Colours::orange : Colour (0xff5ac77a));
            list.items = items;
            resized();
            list.repaint();
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (12);
            auto top = r.removeFromTop (32);
            againBtn.setBounds (top.removeFromRight (120).reduced (0, 2));
            summary.setBounds (top);
            r.removeFromTop (8);
            viewport.setBounds (r);
            list.setSize (jmax (100, r.getWidth() - viewport.getScrollBarThickness()), list.heightFor (jmax (100, r.getWidth() - viewport.getScrollBarThickness())));
        }

    private:
        struct List : public Component
        {
            std::vector<Engine::CheckItem> items;
            static constexpr float fontSize = 15.0f;

            static float rowHeight (const String& text, int width)
            {
                AttributedString a;
                a.append (text, FontOptions (fontSize), Colours::white);
                TextLayout t;
                t.createLayout (a, (float) (width - 34));
                return jmax (22.0f, t.getHeight() + 10.0f);
            }
            int heightFor (int width) const
            {
                float h = 0;
                for (auto& i : items) h += rowHeight (i.text, width);
                return (int) h + 4;
            }
            void paint (Graphics& g) override
            {
                float y = 0;
                for (auto& i : items)
                {
                    const float h = rowHeight (i.text, getWidth());
                    g.setColour (i.level == Engine::CheckItem::problem ? Colour (0xffe05a5a)
                                 : i.level == Engine::CheckItem::warning ? Colours::orange : Colour (0xff5ac77a));
                    g.fillEllipse (6.0f, y + 9.0f, 10.0f, 10.0f);
                    AttributedString a;
                    a.append (i.text, FontOptions (fontSize), i.level == Engine::CheckItem::good ? textDim : Colours::white);
                    TextLayout t;
                    t.createLayout (a, (float) (getWidth() - 34));
                    t.draw (g, { 28.0f, y + 5.0f, (float) getWidth() - 34.0f, h });
                    y += h;
                }
            }
        };

        std::function<std::vector<Engine::CheckItem>()> runCheck;
        std::vector<Engine::CheckItem> items;
        Label summary;
        TextButton againBtn { "Check again" };
        Viewport viewport;
        List list;
    };
}

void MainComponent::showShowCheck()
{
    /* The engine knows the rig; the window adds what only it knows: the
       file, and the tablet. Captured by value or weakly, so a check left
       open never reaches into a window that has gone. */
    auto run = [self = SafePointer<MainComponent> (this), &eng = engine]
    {
        auto items = eng.showCheck();
        if (self != nullptr)
        {
            using L = Engine::CheckItem;
            if (self->currentFile == File())
                items.push_back ({ L::warning, "This setup has never been saved to a file of its own. Use Save As... so it can be brought to another computer." });
            else if (self->dirty)
                items.push_back ({ L::warning, "There are unsaved changes. Save, so a restart comes back to this." });
            if (self->remote != nullptr && self->remote->isRunning())
                items.push_back ({ L::good, "Tablet control is on" });
            std::stable_sort (items.begin(), items.end(), [] (const L& a, const L& b) { return a.level > b.level; });
        }
        return items;
    };
    auto* content = new ShowCheckView (run);
    content->setSize (720, 520);
    DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = "Pre-show check";
    o.dialogBackgroundColour = bgPanel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

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

void MainComponent::playInstrumentFromKeyboard (int slot)
{
    if (keyboardPanel != nullptr && keyboardPanel->isVisible()) keyboardPanel->playInstrument (slot);
}

void MainComponent::outputsChanged()
{
    markDirty();
    MessageManager::callAsync ([self = SafePointer<MainComponent> (this)] { if (self != nullptr) self->inputsPanel->refresh(); });
}

//==============================================================================
void MainComponent::showAudioSettings()
{
    auto* content = new AudioSettingsComponent (engine, settings, [this] (const String& s) { showStatus (s); });
    content->setSize (640, 640);
    DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = "Audio settings";
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
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, "Cannot create a wifi network", why, "OK");
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
    w->addButton ("Use this adapter", 1, KeyPress (KeyPress::returnKey));
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
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Wifi network", err, "OK");
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
    if (remote == nullptr)
    {
        remote = std::make_unique<RemoteServer> (engine, settings);
        remote->onEdited = [this] { markDirty(); };       // a knob or a scene changed on the stand: unsaved, and the autosave takes it
    }
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
    const int st = engine.getTranspose();
    const String linkText = engine.isLinkEnabled() ? "   Link" + (engine.getLinkPeers() > 0 ? " \u00b7 " + String (engine.getLinkPeers()) : String()) : String();
    tempoLabel.setText (String (engine.getTempoBpm(), 1) + " bpm" + (st != 0 ? "   " + String (st > 0 ? "+" : "") + String (st) + " st" : String()) + linkText, dontSendNotification);
    tempoLabel.setTooltip (st != 0 ? "The tempo, and the song's transpose: the keyboards play " + String (std::abs (st)) + (std::abs (st) == 1 ? " semitone " : " semitones ") + (st > 0 ? "up" : "down")
                                   : "The tempo. Tap it, or set it on the song.");
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
            w->addTextEditor ("cc", String (jmax (1, engine.getTapTempoCC())), "Controller number (CC)");
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
    {
        /* What the tablet may change: the show, always; the set lists and
           scenes only when allowed, so a gig cannot be rearranged by an elbow
           on a tablet left unlocked on a stand. */
        auto* editBtn = new ToggleButton ("Allow editing from the tablet  (set lists, scenes)");
        editBtn->setBounds (18, content->getHeight() + 6, 484, 24);
        editBtn->setToggleState (settings.getBoolValue ("remoteEditing", true), dontSendNotification);
        editBtn->setColour (ToggleButton::textColourId, Colours::white);
        editBtn->setColour (ToggleButton::tickColourId, Colour (0xff5aa9ff));
        editBtn->setTooltip ("Off: the tablet still picks songs, programs, keys and scenes, but cannot build or change set lists or scenes");
        editBtn->onClick = [this, b = Component::SafePointer<ToggleButton> (editBtn)]
        {
            if (b == nullptr) return;
            settings.setValue ("remoteEditing", b->getToggleState());
            settings.saveIfNeeded();
            if (remote != nullptr) remote->touch();       // every tablet redraws with Edit shown or gone
        };
        content->addAndMakeVisible (editBtn);
        content->setSize (520, content->getHeight() + 36);
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

/** The soundfont backing-track MIDI files play through. FluidR3_GM is found
    by itself where distributions put it; this is for a better one. */
void MainComponent::chooseGmSoundfont()
{
    const auto current = GmSynth::findSoundfont (engine.getGmSoundfont());
    gmChooser = std::make_unique<FileChooser> ("General MIDI soundfont for backing-track MIDI files",
                                               current != File() ? current : File ("/usr/share/sounds/sf2"), "*.sf2;*.sf3");
    gmChooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (f == File()) return;
        engine.setGmSoundfont (f);
        showStatus ("MIDI files now play through " + f.getFileName()
                    + (GmSynth::isAvailable() ? String() : String (" -- once FluidSynth is installed (libfluidsynth3)")));
    });
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
