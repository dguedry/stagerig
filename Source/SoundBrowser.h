#pragma once

#include "Theme.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <optional>

namespace perf
{

/** One sound a plugin can be given, wherever it comes from. */
struct SoundChoice
{
    enum class From { before, kept, program, vst3Preset, nks };
    From from = From::nks;
    juce::String name;
    juce::String detail;         // the second line: where it comes from, its bank and kind
    juce::File file;             // kept, vst3Preset, nks
    int program = -1;            // program: the plugin's own number for it
    /** Where it sits, outermost first, as its maker groups it: "Scarbee Mark I",
        "Factory", "Leads". The list is browsed a group at a time. */
    juce::StringArray group;
};

/** A plugin's sounds, to try while playing: a click, or the arrow keys, gives
    the plugin that sound at once. The list is every sound StageRig can find
    for it -- the ones you kept, the plugin's own programs, VST3 presets, the
    maker's NKS sounds -- and the first line puts back the sound it had when
    the window opened. A plugin with many sounds shows them a group at a
    time, as its maker groups them (a library, a bank, a kind of sound): a
    click on a group goes into it, the first line comes back out. Typing
    searches every group at once. Keep... adds the sound the
    plugin is playing now (one chosen in its own window: a Kontakt
    instrument), to choose from here in any program. */
class SoundBrowser : public juce::Component,
                     private juce::ListBoxModel,
                     private juce::KeyListener
{
public:
    struct Callbacks
    {
        std::function<void (const SoundChoice&)> choose;
        /** Keeps the sound playing now under this name; the new entry, or nothing if it could not. */
        std::function<std::optional<SoundChoice> (const juce::String&)> keep;
        std::function<bool (const SoundChoice&)> forget;
    };

    SoundBrowser (std::vector<SoundChoice> all, const juce::String& current, const juce::String& pluginName, Callbacks cb)
        : sounds (std::move (all)), plugin (pluginName), callbacks (std::move (cb))
    {
        SoundChoice before;
        before.from = SoundChoice::From::before;
        before.name = "The sound before";
        before.detail = "What " + plugin + " had when this list was opened";
        sounds.insert (sounds.begin(), before);

        addAndMakeVisible (search);
        search.setTextToShowWhenEmpty ("Search every group: name, bank or kind (\"pad warm\")", theme::textDim);
        search.onTextChange = [this] { filter(); };
        search.onReturnKey = [this] { if (rows.size() > 1) open (1); };
        search.addKeyListener (this);

        addAndMakeVisible (list);
        list.setModel (this);
        list.setRowHeight (34);
        // Typing goes on searching after a click in the list.
        list.setWantsKeyboardFocus (false);
        list.setMouseClickGrabsKeyboardFocus (false);
        list.setColour (juce::ListBox::backgroundColourId, theme::bgDark);

        addAndMakeVisible (keepBtn);
        keepBtn.setTooltip ("Adds the sound " + plugin + " is playing now to this list, to choose in any program: "
                            "pick a sound in the plugin's own window first (a Kontakt instrument, say)");
        keepBtn.onClick = [this] { askToKeep(); };

        addAndMakeVisible (hint);
        hint.setFont (juce::FontOptions (12.0f));
        hint.setColour (juce::Label::textColourId, theme::textDim);
        hint.setJustificationType (juce::Justification::topLeft);

        playing = current;
        // Opens where the sound playing now is.
        for (const auto& s : sounds)
            if (s.from != SoundChoice::From::before && s.name == playing) { at = s.group; break; }
        filter();
        setSize (480, 600);
    }

    ~SoundBrowser() override { search.removeKeyListener (this); }

    void visibilityChanged() override { if (isShowing()) search.grabKeyboardFocus(); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        search.setBounds (r.removeFromTop (28));
        r.removeFromTop (6);
        auto bottom = r.removeFromBottom (30);
        keepBtn.setBounds (bottom.removeFromRight (230));
        r.removeFromBottom (4);
        hint.setBounds (r.removeFromBottom (34));
        list.setBounds (r);
    }

private:
    bool keyPressed (const juce::KeyPress& k, juce::Component*) override
    {
        if (k.isKeyCode (juce::KeyPress::downKey) || k.isKeyCode (juce::KeyPress::upKey))
        {
            pick (juce::jlimit (0, (int) rows.size() - 1, list.getSelectedRow() + (k.isKeyCode (juce::KeyPress::downKey) ? 1 : -1)));
            return true;
        }
        return false;
    }

    int getNumRows() override { return (int) rows.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= (int) rows.size()) return;
        g.fillAll (selected ? theme::selected : (row % 2 == 0 ? theme::bgDark : theme::bgDark.brighter (0.03f)));
        const auto& r = rows[(size_t) row];
        if (r.sound < 0)
        {
            // A group to go into, or the way back out.
            g.setColour (r.out ? theme::textDim : juce::Colours::white);
            g.setFont (juce::FontOptions (14.0f, r.out ? juce::Font::plain : juce::Font::bold));
            const auto left = r.out ? "<   " + r.name : r.name;
            const int leftW = juce::jmin (w - 90, juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), left) + 24);
            g.drawText (left, 10, 0, leftW, h, juce::Justification::centredLeft);
            // Beside the way out, where this is; beside a group, how many it holds.
            g.setColour (r.out ? juce::Colours::white : theme::textDim);
            g.setFont (juce::FontOptions (12.0f, r.out ? juce::Font::bold : juce::Font::plain));
            g.drawText (r.out ? r.detail : juce::String (r.count) + "   >", 10 + leftW, 0, w - 20 - leftW, h, juce::Justification::centredRight);
            return;
        }
        const auto& s = sounds[(size_t) r.sound];
        const bool before = s.from == SoundChoice::From::before;
        g.setColour (! before && s.name == playing ? theme::amber : juce::Colours::white);
        g.setFont (juce::FontOptions (14.0f, before ? juce::Font::italic : juce::Font::bold));
        if (r.detail.isEmpty()) { g.drawText (s.name, 10, 0, w - 20, h, juce::Justification::centredLeft); return; }
        g.drawText (s.name, 10, 2, w - 20, 17, juce::Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (r.detail, 10, 18, w - 20, 14, juce::Justification::centredLeft);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { offerToForget (row); return; }
        open (row);
    }
    void returnKeyPressed (int row) override { open (row); }
    void selectedRowsChanged (int row) override { if (! quiet && row >= 0) pick (row); }

    /** A click or Return: into a group, back out of one, or the sound. */
    void open (int row)
    {
        if (row < 0 || row >= (int) rows.size()) return;
        const auto r = rows[(size_t) row];
        if (r.sound >= 0) { pick (row); return; }
        if (r.out)
        {
            // Out past the groups that hold nothing but one other group.
            do at.remove (at.size() - 1); while (at.size() > floor.size() && passesThrough (at));
        }
        else
        {
            at.add (r.name);
            search.setText ({}, false);
        }
        chosen = -1;
        filter();
        list.scrollToEnsureRowIsOnscreen (0);
    }

    /** The arrow keys: plays a sound, only rests on a group. */
    void pick (int row)
    {
        if (row < 0 || row >= (int) rows.size()) return;
        {
            const juce::ScopedValueSetter<bool> q (quiet, true);
            list.selectRow (row);
        }
        const int index = rows[(size_t) row].sound;
        if (index < 0) return;
        if (index == chosen && index != 0) return;
        chosen = index;
        const auto& s = sounds[(size_t) index];
        playing = s.from == SoundChoice::From::before ? juce::String() : s.name;
        if (callbacks.choose) callbacks.choose (s);
        list.repaint();
    }

    void askToKeep()
    {
        auto* w = new juce::AlertWindow ("Keep this sound",
                                         "A name for the sound " + plugin + " is playing now. It is added to this list, "
                                         "to choose in any program with " + plugin + ".", juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("name", playing, "Name");
        w->addButton ("Keep", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([sp = juce::Component::SafePointer<SoundBrowser> (this), w] (int r)
        {
            const auto name = w->getTextEditorContents ("name").trim();
            if (sp == nullptr || r != 1 || name.isEmpty() || ! sp->callbacks.keep) return;
            if (auto added = sp->callbacks.keep (name))
            {
                auto& all = sp->sounds;
                // A sound kept under a name already there replaces it.
                all.erase (std::remove_if (all.begin(), all.end(), [&] (const SoundChoice& c)
                                           { return c.from == SoundChoice::From::kept && c.name == added->name; }), all.end());
                all.insert (all.begin() + 1, *added);
                sp->playing = added->name;
                sp->at = added->group;
                sp->search.setText ({}, false);
                sp->chosen = -1;
                sp->filter();
            }
        }), true);
    }

    void offerToForget (int row)
    {
        if (row < 0 || row >= (int) rows.size()) return;
        const int index = rows[(size_t) row].sound;
        if (index < 0 || sounds[(size_t) index].from != SoundChoice::From::kept) return;
        juce::PopupMenu m;
        m.addItem (1, "Remove \"" + sounds[(size_t) index].name + "\" from the list");
        m.showMenuAsync (juce::PopupMenu::Options(), [sp = juce::Component::SafePointer<SoundBrowser> (this), index] (int r)
        {
            if (sp == nullptr || r != 1 || ! sp->callbacks.forget) return;
            if (sp->callbacks.forget (sp->sounds[(size_t) index]))
            {
                sp->sounds.erase (sp->sounds.begin() + index);
                sp->chosen = -1;
                sp->filter();
            }
        });
    }

    static bool startsWith (const juce::StringArray& group, const juce::StringArray& path)
    {
        if (group.size() < path.size()) return false;
        for (int i = 0; i < path.size(); ++i) if (group[i] != path[i]) return false;
        return true;
    }

    /** The groups straight under a path, in the order they first come, and whether sounds sit there too. */
    juce::StringArray groupsUnder (const juce::StringArray& path, bool* soundsHere = nullptr) const
    {
        juce::StringArray under;
        if (soundsHere != nullptr) *soundsHere = false;
        for (size_t i = 1; i < sounds.size(); ++i)
        {
            const auto& g = sounds[i].group;
            if (! startsWith (g, path)) continue;
            if (g.size() > path.size()) under.addIfNotAlreadyThere (g[path.size()]);
            else if (soundsHere != nullptr) *soundsHere = true;
        }
        return under;
    }

    /** A group with nothing in it but one other group: not worth a stop. */
    bool passesThrough (const juce::StringArray& path) const
    {
        bool soundsHere = false;
        return groupsUnder (path, &soundsHere).size() == 1 && ! soundsHere;
    }

    void filter()
    {
        const auto words = juce::StringArray::fromTokens (search.getText().toLowerCase(), " ", "\"");
        const bool searching = words.joinIntoString ({}).isNotEmpty();
        const int total = (int) sounds.size() - 1;

        // A short list is shown whole; a long one a group at a time.
        const bool grouped = total > shortList && ! groupsUnder ({}).isEmpty();
        floor.clear();
        if (grouped) while (passesThrough (floor)) floor.add (groupsUnder (floor)[0]);
        if (! grouped) at.clear();
        else
        {
            // Somewhere that is still there (a kept sound's group goes with its last sound).
            bool soundsHere = false;
            while (! at.isEmpty() && groupsUnder (at, &soundsHere).isEmpty() && ! soundsHere) at.remove (at.size() - 1);
            if (! startsWith (at, floor)) at = floor;
            while (passesThrough (at)) at.add (groupsUnder (at)[0]);
        }

        rows.clear();
        rows.push_back ({ 0, false, {}, 0, sounds[0].detail });
        int listed = 0;
        if (searching || ! grouped)
        {
            for (int i = 1; i < (int) sounds.size(); ++i)
            {
                const auto& s = sounds[(size_t) i];
                const auto where = s.group.joinIntoString ("  >  ");
                const auto detail = where.isEmpty() ? s.detail : s.detail.isEmpty() ? where : where + "  -  " + s.detail;
                const auto text = (s.name + " " + detail).toLowerCase();
                bool all = true;
                for (const auto& w : words) if (w.isNotEmpty() && ! text.contains (w)) { all = false; break; }
                if (all) { rows.push_back ({ i, false, {}, 0, detail }); ++listed; }
            }
        }
        else
        {
            if (at.size() > floor.size())
            {
                auto above = at; above.remove (above.size() - 1);
                while (above.size() > floor.size() && passesThrough (above)) above.remove (above.size() - 1);
                rows.push_back ({ -1, true, above.size() > floor.size() ? above[above.size() - 1] : juce::String ("All sounds"), 0, at.joinIntoString ("  >  ") });
            }
            for (const auto& name : groupsUnder (at))
            {
                auto path = at; path.add (name);
                int count = 0;
                for (size_t i = 1; i < sounds.size(); ++i) if (startsWith (sounds[i].group, path)) ++count;
                rows.push_back ({ -1, false, name, count, {} });
            }
            for (int i = 1; i < (int) sounds.size(); ++i)
                if (sounds[(size_t) i].group == at) { rows.push_back ({ i, false, {}, 0, sounds[(size_t) i].detail }); ++listed; }
        }

        const juce::ScopedValueSetter<bool> q (quiet, true);
        list.updateContent();
        list.deselectAllRows();
        list.repaint();
        const juce::String how = " Click a sound, or use the arrow keys, to hear it while you play: the program keeps the last one chosen.";
        hint.setText (total == 0 ? "No sounds for " + plugin + " were found to list. Choose one in its own window, then Keep it here."
                      : searching || ! grouped ? juce::String (listed) + " of " + juce::String (total) + " sounds." + how
                      : juce::String (total) + " sounds, in groups: click a group to go into it, or type to search them all." + how,
                      juce::dontSendNotification);
    }

    /** One line of the list: a sound, a group to go into, or the way back out. */
    struct Row
    {
        int sound = -1;          // its place in sounds; -1 for a group
        bool out = false;        // the way back out
        juce::String name;       // a group's
        int count = 0;           // a group's sounds, the ones in its own groups too
        juce::String detail;     // a sound's second line
    };
    static constexpr int shortList = 24;

    std::vector<SoundChoice> sounds;         // [0]: the sound before
    std::vector<Row> rows;
    juce::StringArray at, floor;             // the group shown, and the outermost one worth showing
    juce::String plugin, playing;
    int chosen = -1;
    Callbacks callbacks;
    bool quiet = false;

    juce::TextEditor search;
    juce::ListBox list;
    juce::Label hint;
    juce::TextButton keepBtn { "Keep the sound playing now..." };
};

} // namespace perf
