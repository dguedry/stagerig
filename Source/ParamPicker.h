#pragma once

#include "ParamInfo.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace perf
{

/** Drop-in replacement for a ComboBox that lists a plugin's parameters.

    Plugins such as Kontakt expose thousands of parameters; a PopupMenu that
    long builds a component per item and never finishes. This shows a field
    that looks like a combo box and opens a searchable list instead: type to
    filter, arrows to move, Return or click to pick, Escape to cancel.

    VST3 plugins take no MIDI controllers directly: they publish one parameter
    per (MIDI channel, controller) and the host converts, so "Channel Volume"
    appears once per channel. When the plugin told us which copy is which
    (ParamInfo::midiChannel), those are labelled "(ch N)", listed first for the
    channel the input plays on, and the other channels are one toggle away.
    Without that information, long runs of identical names are hidden behind a
    toggle instead. */
class ParamPicker : public juce::Component
{
public:
    std::function<void()> onChange;

    void addItem (const juce::String& text, int id, int midiChannel = 0, int midiController = -1)
    {
        items.add ({ text, id, midiChannel, midiController });
        repaint();
    }
    void addItem (const ParamInfo& p, int id)                { addItem (p.name.isEmpty() ? "Param " + juce::String (p.index) : p.name, id, p.midiChannel, p.midiController); }

    int getNumItems() const                                  { return items.size(); }
    /** Display label, e.g. "Channel Volume(MSB) (ch 3)"; used for the mapping's name. */
    juce::String getItemText (int index) const               { return index >= 0 && index < items.size() ? labelFor (items.getReference (index)) : juce::String(); }
    int getSelectedId() const                                { return selected; }

    /** MIDI channel the target plugin is played on (1..16), or 0 when unknown / omni.
        Controller parameters for this channel are listed first. */
    void setPreferredChannel (int ch)                        { preferredChannel = juce::jlimit (0, 16, ch); }

    void clear (juce::NotificationType n = juce::sendNotificationAsync)
    {
        items.clear();
        const bool had = selected != 0;
        selected = 0;
        repaint();
        if (had) notify (n);
    }

    void setSelectedId (int id, juce::NotificationType n = juce::sendNotificationAsync)
    {
        const int newId = indexOfId (id) >= 0 ? id : 0;
        if (newId == selected) return;
        selected = newId;
        repaint();
        notify (n);
    }

    void setTextWhenNothingSelected (const juce::String& t)   { placeholder = t; repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (findColour (juce::ComboBox::outlineColourId).withAlpha (isEnabled() ? 1.0f : 0.5f));
        g.drawRoundedRectangle (r, 4.0f, 1.0f);

        const int idx = indexOfId (selected);
        const bool has = idx >= 0;
        g.setColour (findColour (juce::ComboBox::textColourId).withAlpha (has && isEnabled() ? 1.0f : 0.5f));
        g.setFont (juce::FontOptions ((float) getHeight() * 0.6f));
        g.drawText (has ? labelFor (items.getReference (idx)) : placeholder, getLocalBounds().withTrimmedLeft (8).withTrimmedRight (24),
                    juce::Justification::centredLeft, true);

        juce::Path p;
        const float ax = (float) getWidth() - 14.0f, ay = (float) getHeight() * 0.5f;
        p.addTriangle (ax - 4.0f, ay - 2.0f, ax + 4.0f, ay - 2.0f, ax, ay + 3.0f);
        g.setColour (findColour (juce::ComboBox::arrowColourId).withAlpha (isEnabled() ? 1.0f : 0.4f));
        g.fillPath (p);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (isEnabled() && items.size() > 0)
            showPopup();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::returnKey || k == juce::KeyPress::spaceKey || k == juce::KeyPress::downKey)
        {
            if (isEnabled() && items.size() > 0) showPopup();
            return true;
        }
        return false;
    }

    /** Human name for a VST3 controller number (0..127 CC, 128 aftertouch, 129 pitch bend, 130 program change). */
    static juce::String controllerName (int controller)
    {
        if (controller < 0) return {};
        if (controller < 128) return "CC " + juce::String (controller);
        if (controller == 128) return "Aftertouch";
        if (controller == 129) return "Pitch bend";
        if (controller == 130) return "Program change";
        return {};
    }

private:
    struct Item { juce::String name; int id = 0; int channel = 0; int controller = -1; };

    static juce::String labelFor (const Item& it)
    {
        return it.channel > 0 ? it.name + " (ch " + juce::String (it.channel) + ")" : it.name;
    }

    int indexOfId (int id) const
    {
        for (int i = 0; i < items.size(); ++i) if (items.getReference (i).id == id) return i;
        return -1;
    }

    void notify (juce::NotificationType n)
    {
        if (n == juce::dontSendNotification || ! onChange) return;
        if (n == juce::sendNotificationSync) onChange();
        else juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<ParamPicker> (this)] { if (sp != nullptr && sp->onChange) sp->onChange(); });
    }

    /** Search box whose arrow/page keys drive the list instead of the caret. */
    struct SearchBox : public juce::TextEditor
    {
        std::function<bool (const juce::KeyPress&)> onNavigate;
        bool keyPressed (const juce::KeyPress& k) override
        {
            if (onNavigate && (k == juce::KeyPress::upKey || k == juce::KeyPress::downKey
                               || k == juce::KeyPress::pageUpKey || k == juce::KeyPress::pageDownKey))
                return onNavigate (k);
            return juce::TextEditor::keyPressed (k);
        }
    };

    struct Popup : public juce::Component, private juce::ListBoxModel, private juce::TextEditor::Listener
    {
        explicit Popup (ParamPicker& o) : owner (o)
        {
            addAndMakeVisible (search);
            search.setTextToShowWhenEmpty ("Search " + juce::String (owner.items.size()) + " parameters...", juce::Colours::grey);
            search.setEscapeAndReturnKeysConsumed (true);
            search.setSelectAllWhenFocused (true);
            search.addListener (this);
            search.onNavigate = [this] (const juce::KeyPress& k) { return navigate (k); };

            addAndMakeVisible (list);
            list.setModel (this);
            list.setRowHeight (22);
            list.setColour (juce::ListBox::backgroundColourId, owner.findColour (juce::ComboBox::backgroundColourId));
            addAndMakeVisible (count);
            count.setFont (juce::FontOptions (11.0f));
            count.setColour (juce::Label::textColourId, juce::Colours::grey);
            count.setJustificationType (juce::Justification::centredRight);

            buildOrder();
            filter();
        }

        /** Decide the display order and which items are behind the toggle. */
        void buildOrder()
        {
            const int n = owner.items.size();
            order.clearQuick();
            secondary.clear();
            hasChannels = false;
            for (int i = 0; i < n; ++i) if (owner.items.getReference (i).channel > 0) { hasChannels = true; break; }

            if (hasChannels)
            {
                // Controller parameters for the input's channel first (all channels when omni),
                // then the plugin's own parameters, then the other channels' copies (toggle).
                const int pref = owner.preferredChannel;
                for (int i = 0; i < n; ++i)
                {
                    const auto& it = owner.items.getReference (i);
                    if (it.channel > 0 && (pref == 0 || it.channel == pref)) order.add (i);
                }
                for (int i = 0; i < n; ++i) if (owner.items.getReference (i).channel == 0) order.add (i);
                for (int i = 0; i < n; ++i)
                {
                    const auto& it = owner.items.getReference (i);
                    if (it.channel > 0 && pref != 0 && it.channel != pref) { order.add (i); secondary.setBit (i); }
                }
                numSecondary = secondary.countNumberOfSetBits();
                if (numSecondary > 0)
                {
                    addAndMakeVisible (showAll);
                    showAll.setButtonText ("Show the other MIDI channels too (" + juce::String (numSecondary) + " parameters; this input plays on ch " + juce::String (pref) + ")");
                    showAll.setTooltip ("Each MIDI controller exists once per channel. Only the channel this input sends on affects what you hear, unless the plugin is set up multi-timbrally.");
                    showAll.setToggleState (showAllChannels, juce::dontSendNotification);
                    showAll.onClick = [this] { showAllChannels = showAll.getToggleState(); filter(); };
                }
                return;
            }

            // No channel information: hide long runs of identical names (VST3 controller
            // proxies from a plugin without IMidiMapping) behind a toggle.
            for (int i = 0; i < n; ++i) order.add (i);
            for (int i = 0; i < n;)
            {
                int j = i + 1;
                while (j < n && owner.items.getReference (j).name == owner.items.getReference (i).name) ++j;
                if (j - i >= kRunLength)
                    for (int k = i; k < j; ++k) secondary.setBit (k);
                i = j;
            }
            numSecondary = secondary.countNumberOfSetBits();
            if (numSecondary > 0)
            {
                addAndMakeVisible (showAll);
                showAll.setButtonText ("Show repeated parameters (" + juce::String (numSecondary) + " copies of " + juce::String (countRuns()) + " names)");
                showAll.setTooltip ("Plugins usually list each MIDI controller once per channel. This plugin did not say which copy is which channel, so the repeats are folded away here.");
                showAll.setToggleState (showRepeated, juce::dontSendNotification);
                showAll.onClick = [this] { showRepeated = showAll.getToggleState(); filter(); };
            }
        }

        int countRuns() const
        {
            int runs = 0;
            for (int i = 0; i < owner.items.size(); ++i)
                if (secondary[i] && (i == 0 || ! secondary[i - 1] || owner.items.getReference (i).name != owner.items.getReference (i - 1).name))
                    ++runs;
            return runs;
        }

        bool secondaryShown() const { return hasChannels ? showAllChannels : showRepeated; }

        void parentHierarchyChanged() override
        {
            if (isShowing())
                juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<Popup> (this)] { if (sp != nullptr) sp->search.grabKeyboardFocus(); });
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (6);
            auto top = r.removeFromTop (26);
            count.setBounds (top.removeFromRight (130));
            search.setBounds (top);
            r.removeFromTop (4);
            if (numSecondary > 0)
                showAll.setBounds (r.removeFromBottom (22));
            list.setBounds (r);
        }

        void filter()
        {
            rows.clearQuick();
            juce::StringArray words;
            words.addTokens (search.getText().trim().toLowerCase(), true);
            const int selectedIndex = owner.indexOfId (owner.selected);
            int hiddenMatches = 0;
            for (int i : order)
            {
                const auto& it = owner.items.getReference (i);
                const auto hay = owner.labelFor (it).toLowerCase() + " #" + juce::String (i) + " " + controllerName (it.controller).toLowerCase();
                bool ok = true;
                for (auto& w : words) if (! hay.contains (w)) { ok = false; break; }
                if (! ok) continue;
                if (secondary[i] && ! secondaryShown() && i != selectedIndex)
                    ++hiddenMatches;    // behind the toggle, unless it is the current selection
                else
                    rows.add (i);
            }
            list.updateContent();
            count.setText (juce::String (rows.size()) + " / " + juce::String (owner.items.size())
                           + (hiddenMatches > 0 ? " (+" + juce::String (hiddenMatches) + " hidden)" : juce::String()), juce::dontSendNotification);
            const int cur = rows.indexOf (selectedIndex);
            list.selectRow (cur >= 0 ? cur : (rows.isEmpty() ? -1 : 0));
            if (cur >= 0) list.scrollToEnsureRowIsOnscreen (cur);
        }

        bool navigate (const juce::KeyPress& k)
        {
            if (rows.isEmpty()) return true;
            const int visible = juce::jmax (1, list.getHeight() / list.getRowHeight());
            int row = juce::jmax (0, list.getSelectedRow());
            if (k == juce::KeyPress::upKey)            row = juce::jmax (0, row - 1);
            else if (k == juce::KeyPress::downKey)     row = juce::jmin (rows.size() - 1, row + 1);
            else if (k == juce::KeyPress::pageUpKey)   row = juce::jmax (0, row - visible);
            else if (k == juce::KeyPress::pageDownKey) row = juce::jmin (rows.size() - 1, row + visible);
            list.selectRow (row);
            return true;
        }

        void choose (int row)
        {
            if (row < 0 || row >= rows.size()) return;
            owner.setSelectedId (owner.items.getReference (rows[row]).id, juce::sendNotificationSync);
            dismiss();
        }

        void dismiss()
        {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                box->dismiss();
        }

        // ListBoxModel
        int getNumRows() override { return rows.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override
        {
            if (row < 0 || row >= rows.size()) return;
            if (sel) { g.setColour (owner.findColour (juce::TextEditor::highlightColourId).withAlpha (0.6f)); g.fillRect (0, 0, w, h); }
            const int i = rows[row];
            const auto& it = owner.items.getReference (i);
            g.setFont (juce::FontOptions ((float) h * 0.6f));
            g.setColour (owner.findColour (juce::ComboBox::textColourId));
            g.drawText (it.name, 8, 0, w - 190, h, juce::Justification::centredLeft, true);
            if (it.channel > 0)
            {
                g.setColour (owner.findColour (juce::ComboBox::textColourId).withAlpha (0.75f));
                g.drawText (controllerName (it.controller) + "  ch " + juce::String (it.channel), w - 182, 0, 118, h, juce::Justification::centredRight, false);
            }
            g.setColour (juce::Colours::grey);
            g.drawText ("#" + juce::String (i), w - 62, 0, 56, h, juce::Justification::centredRight, false);
        }
        void listBoxItemClicked (int row, const juce::MouseEvent&) override { choose (row); }
        void returnKeyPressed (int row) override                             { choose (row); }

        // TextEditor::Listener
        void textEditorTextChanged (juce::TextEditor&) override      { filter(); }
        void textEditorReturnKeyPressed (juce::TextEditor&) override { choose (list.getSelectedRow() >= 0 ? list.getSelectedRow() : 0); }
        void textEditorEscapeKeyPressed (juce::TextEditor&) override { dismiss(); }

        static constexpr int kRunLength = 8;

        ParamPicker& owner;
        SearchBox search;
        juce::ListBox list;
        juce::Label count;
        juce::ToggleButton showAll;
        bool hasChannels = false;
        juce::Array<int> order;         // display order (indices into owner.items)
        juce::BigInteger secondary;     // items behind the toggle
        int numSecondary = 0;
        juce::Array<int> rows;          // indices into owner.items that pass the filter
    };

    /** Remembered for the session, so the choice sticks between pickers. */
    static inline bool showAllChannels = false;
    static inline bool showRepeated = false;

    void showPopup()
    {
        auto content = std::make_unique<Popup> (*this);
        content->setSize (juce::jmax (460, getWidth()), 380);
        juce::CallOutBox::launchAsynchronously (std::move (content), getScreenBounds(), nullptr);
    }

    juce::Array<Item> items;
    int selected = 0;
    int preferredChannel = 0;
    juce::String placeholder;
};

} // namespace perf
