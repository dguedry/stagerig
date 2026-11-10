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
};

/** A plugin's sounds, to try while playing: a click, or the arrow keys, gives
    the plugin that sound at once. The list is every sound StageRig can find
    for it -- the ones you kept, the plugin's own programs, VST3 presets, the
    maker's NKS sounds -- and the first line puts back the sound it had when
    the window opened. Typing narrows the list. Keep... adds the sound the
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
        search.setTextToShowWhenEmpty ("Search: name, bank or kind (\"pad warm\")", theme::textDim);
        search.onTextChange = [this] { filter(); };
        search.onReturnKey = [this] { if (shown.size() > 1) pick (1); };
        search.addKeyListener (this);

        addAndMakeVisible (list);
        list.setModel (this);
        list.setRowHeight (34);
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
            pick (juce::jlimit (0, (int) shown.size() - 1, list.getSelectedRow() + (k.isKeyCode (juce::KeyPress::downKey) ? 1 : -1)));
            return true;
        }
        return false;
    }

    int getNumRows() override { return (int) shown.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= (int) shown.size()) return;
        g.fillAll (selected ? theme::accentDim : (row % 2 == 0 ? theme::bgDark : theme::bgDark.brighter (0.03f)));
        const auto& s = sounds[(size_t) shown[(size_t) row]];
        const bool before = s.from == SoundChoice::From::before;
        g.setColour (! before && s.name == playing ? theme::accent.brighter (0.4f) : juce::Colours::white);
        g.setFont (juce::FontOptions (14.0f, before ? juce::Font::italic : juce::Font::bold));
        g.drawText (s.name, 10, 2, w - 20, 17, juce::Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (s.detail, 10, 18, w - 20, 14, juce::Justification::centredLeft);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { offerToForget (row); return; }
        pick (row);
    }
    void returnKeyPressed (int row) override { pick (row); }
    void selectedRowsChanged (int row) override { if (! quiet && row >= 0) pick (row); }

    void pick (int row)
    {
        if (row < 0 || row >= (int) shown.size()) return;
        {
            const juce::ScopedValueSetter<bool> q (quiet, true);
            list.selectRow (row);
        }
        const int index = shown[(size_t) row];
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
                sp->chosen = -1;
                sp->filter();
            }
        }), true);
    }

    void offerToForget (int row)
    {
        if (row < 0 || row >= (int) shown.size()) return;
        const int index = shown[(size_t) row];
        if (sounds[(size_t) index].from != SoundChoice::From::kept) return;
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

    void filter()
    {
        const auto words = juce::StringArray::fromTokens (search.getText().toLowerCase(), " ", "\"");
        shown.clear();
        for (int i = 0; i < (int) sounds.size(); ++i)
        {
            const auto& s = sounds[(size_t) i];
            const auto text = (s.name + " " + s.detail).toLowerCase();
            bool all = true;
            if (s.from != SoundChoice::From::before)
                for (const auto& w : words) if (w.isNotEmpty() && ! text.contains (w)) { all = false; break; }
            if (all) shown.push_back (i);
        }
        const juce::ScopedValueSetter<bool> q (quiet, true);
        list.updateContent();
        list.deselectAllRows();
        list.repaint();
        const int total = (int) sounds.size() - 1;
        hint.setText (total == 0 ? "No sounds for " + plugin + " were found to list. Choose one in its own window, then Keep it here."
                                 : juce::String ((int) shown.size() - 1) + " of " + juce::String (total) + " sounds. Click one, or use the arrow keys, "
                                   "to hear it while you play: the program keeps the last one chosen.",
                      juce::dontSendNotification);
    }

    std::vector<SoundChoice> sounds;         // [0]: the sound before
    std::vector<int> shown;
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
