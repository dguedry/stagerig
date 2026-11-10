#pragma once

#include "ControlMaps.h"
#include "Theme.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace perf
{

/** A plugin's sounds from its maker (NKS presets), to try while playing:
    a click, or the arrow keys, gives the plugin that sound at once. The
    first line puts back the sound it had when the window opened. Typing
    narrows the list by name, bank and the maker's tags ("pad warm"). */
class SoundBrowser : public juce::Component,
                     private juce::ListBoxModel,
                     private juce::KeyListener
{
public:
    /** `choose` gets the sound, or nullptr for the one before. */
    SoundBrowser (std::vector<NksSound> all, const juce::String& current,
                  std::function<void (const NksSound*)> chooseFn)
        : sounds (std::move (all)), choose (std::move (chooseFn))
    {
        addAndMakeVisible (search);
        search.setTextToShowWhenEmpty ("Search: name, bank or kind (\"pad warm\")", theme::textDim);
        search.onTextChange = [this] { filter(); };
        search.onReturnKey = [this] { if (shown.size() > 1) pick (1); };
        search.setWantsKeyboardFocus (true);
        search.addKeyListener (this);

        addAndMakeVisible (list);
        list.setModel (this);
        list.setRowHeight (34);
        list.setColour (juce::ListBox::backgroundColourId, theme::bgDark);

        addAndMakeVisible (hint);
        hint.setFont (juce::FontOptions (12.0f));
        hint.setColour (juce::Label::textColourId, theme::textDim);

        playing = current;
        filter();
        setSize (460, 560);
    }

    void visibilityChanged() override { if (isShowing()) search.grabKeyboardFocus(); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        search.setBounds (r.removeFromTop (28));
        r.removeFromTop (6);
        hint.setBounds (r.removeFromBottom (36));
        list.setBounds (r);
    }

    ~SoundBrowser() override { search.removeKeyListener (this); }

    /** Up and down in the search box walk the list, playing each sound. */
    bool keyPressed (const juce::KeyPress& k, juce::Component*) override
    {
        if (k.isKeyCode (juce::KeyPress::downKey) || k.isKeyCode (juce::KeyPress::upKey))
        {
            const int row = juce::jlimit (0, (int) shown.size() - 1, list.getSelectedRow() + (k.isKeyCode (juce::KeyPress::downKey) ? 1 : -1));
            pick (row);
            return true;
        }
        return false;
    }

private:
    int getNumRows() override { return (int) shown.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= (int) shown.size()) return;
        g.fillAll (selected ? theme::accentDim : (row % 2 == 0 ? theme::bgDark : theme::bgDark.brighter (0.03f)));
        const auto* s = shown[(size_t) row];
        if (s == nullptr)
        {
            g.setColour (juce::Colours::white);
            g.setFont (juce::FontOptions (14.0f, juce::Font::italic));
            g.drawText ("The sound before", 10, 0, w - 20, h, juce::Justification::centredLeft);
            return;
        }
        g.setColour (s->name == playing ? theme::accent.brighter (0.4f) : juce::Colours::white);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText (s->name, 10, 2, w - 20, 17, juce::Justification::centredLeft);
        juce::StringArray under;
        if (s->bank.isNotEmpty()) under.add (s->bank);
        if (! s->tags.isEmpty()) under.add (s->tags.joinIntoString (", "));
        g.setColour (theme::textDim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (under.joinIntoString ("  -  "), 10, 18, w - 20, 14, juce::Justification::centredLeft);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override { pick (row); }
    void returnKeyPressed (int row) override { pick (row); }

    void selectedRowsChanged (int row) override
    {
        if (! quiet && row >= 0) pick (row);      // arrow keys in the list
    }

    void pick (int row)
    {
        if (row < 0 || row >= (int) shown.size()) return;
        {
            const juce::ScopedValueSetter<bool> q (quiet, true);
            list.selectRow (row);
        }
        const auto* s = shown[(size_t) row];
        if (s == chosen && row != 0) return;
        chosen = s;
        playing = s != nullptr ? s->name : juce::String();
        choose (s);
        list.repaint();
    }

    void filter()
    {
        const auto words = juce::StringArray::fromTokens (search.getText().toLowerCase(), " ", "\"");
        shown.clear();
        shown.push_back (nullptr);
        for (const auto& s : sounds)
        {
            const auto text = (s.name + " " + s.bank + " " + s.tags.joinIntoString (" ")).toLowerCase();
            bool all = true;
            for (const auto& w : words) if (w.isNotEmpty() && ! text.contains (w)) { all = false; break; }
            if (all) shown.push_back (&s);
        }
        const juce::ScopedValueSetter<bool> q (quiet, true);
        list.updateContent();
        list.deselectAllRows();
        list.repaint();
        hint.setText (juce::String ((int) shown.size() - 1) + " of " + juce::String ((int) sounds.size())
                      + " sounds. Click one, or use the arrow keys, to hear it while you play: "
                        "the program keeps the last one chosen.", juce::dontSendNotification);
    }

    std::vector<NksSound> sounds;
    std::vector<const NksSound*> shown;      // nullptr: the sound before
    const NksSound* chosen = nullptr;
    juce::String playing;
    std::function<void (const NksSound*)> choose;
    bool quiet = false;

    juce::TextEditor search;
    juce::ListBox list;
    juce::Label hint;
};

} // namespace perf
