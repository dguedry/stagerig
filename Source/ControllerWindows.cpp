#include "ControllerWindows.h"
#include "Theme.h"
#include "ParamPicker.h"
#include "PanelParams.h"
#include "RemotePlugin.h"

namespace perf
{
using namespace juce;

namespace
{
    using namespace perf::theme;

    String noteName (int n) { return MidiMessage::getMidiNoteName (n, true, true, 3); }

    struct NamedColour { const char* name; uint32 rgb; };
    const NamedColour keyColourChoices[] = {
        { "Orange", 0xff8a00 }, { "Red", 0xff3c3c }, { "Yellow", 0xffdc00 }, { "Green", 0x3cdc3c },
        { "Teal", 0x00e6c8 }, { "Blue", 0x00b4ff }, { "Violet", 0x5a64ff }, { "Pink", 0xff3cc8 }, { "White", 0xffffff },
    };

    Label& caption (Label& l, const String& text)
    {
        l.setText (text, dontSendNotification);
        l.setFont (FontOptions (11.0f, Font::bold));
        l.setColour (Label::textColourId, textDim);
        return l;
    }

    Component* launch (Component* content, const String& title, int w, int h)
    {
        content->setSize (w, h);
        return perf::theme::launchDialog (content, title, 0, 0);
    }
}

//==============================================================================
namespace
{
/** What the keyboard's eight knobs turn on one instrument, shown as the
    eight knobs: each box is a knob, named and valued, lit when that knob is
    turned. Click one to change it underneath. The layout is the plugin's, so
    every program with it shares it; it starts automatic (the maker's pages,
    or StageRig's picks) and becomes yours, saved as you go, the moment one
    thing is changed. Nothing here is about keyboards: what a knob turns is
    the same on all of them. */
class ControlMapEditor : public Component, private Timer, private Engine::Listener
{
public:
    ControlMapEditor (Engine& e, SurfaceHub& h, ControlMapLibrary& lib, int in, int prog, int sl, std::function<void()> ch)
        : engine (e), hub (h), library (lib), input (in), program (prog), slot (sl), changed (std::move (ch))
    {
        if (const auto* sd = slotAt()) plugin = sd->plugin;
        engine.addListener (this);

        addAndMakeVisible (title);
        title.setFont (FontOptions (18.0f, Font::bold));
        title.setText ("Knobs for " + plugin.name, dontSendNotification);
        addAndMakeVisible (subtitle);
        subtitle.setColour (Label::textColourId, textDim);
        subtitle.setJustificationType (Justification::topLeft);
        subtitle.setText ("How it works: turning knob 1 to 8 on your keyboard turns the parameter in box 1 to 8 here, the same in every program with " + plugin.name + ". "
                          "It takes a keyboard StageRig can talk to: a Komplete Kontrol (S MK1, MK2, MK3, A or M), a Launchkey MK4, any MIDI 2.0 keyboard, "
                          "or one whose eight knobs you taught in the Controls panel. Those with a screen show these names and values.", dontSendNotification);

        for (auto* b : { &knobsTab, &keysTab })
        {
            addAndMakeVisible (b);
            b->setClickingTogglesState (true);
            b->setRadioGroupId (1);
            b->onClick = [this] { showTab(); };
        }
        knobsTab.setToggleState (true, dontSendNotification);

        addAndMakeVisible (caption (layoutCap, "LAYOUT"));
        addAndMakeVisible (layoutBox);
        layoutBox.onChange = [this] { chooseLayout(); };
        addAndMakeVisible (shareBtn);
        shareBtn.onClick = [this] { shareMenu(); };

        addAndMakeVisible (addPageBtn);
        addPageBtn.setTooltip ("Another page of eight knobs: the keyboard's page buttons step to it");
        addPageBtn.onClick = [this] { makeOwn(); edit.pages.push_back ({ "Page " + String ((int) edit.pages.size() + 1), {} }); page = (int) edit.pages.size() - 1; saveAndShow(); };
        addAndMakeVisible (removePageBtn);
        removePageBtn.onClick = [this]
        {
            makeOwn();
            if (edit.pages.size() <= 1) return;
            edit.pages.erase (edit.pages.begin() + page);
            page = jmax (0, page - 1);
            saveAndShow();
        };
        addChildComponent (pageBox);
        pageBox.onChange = [this] { if (pageBox.getSelectedId() > 0 && pageBox.getSelectedId() - 1 != page) { page = pageBox.getSelectedId() - 1; showAll(); } };
        addAndMakeVisible (pageNameEd);
        pageNameEd.setTextToShowWhenEmpty ("Page name", textDim);
        pageNameEd.setTooltip ("The page's name, shown when you step to it");
        pageNameEd.onReturnKey = pageNameEd.onFocusLost = [this] { renamePage(); };

        for (int k = 0; k < ControlPage::numKnobs; ++k)
        {
            auto& t = tiles[(size_t) k];
            addAndMakeVisible (t);
            t.number = k + 1;
            t.onClick = [this, k] { select (k); };
        }

        addAndMakeVisible (caption (editCap, ""));
        addAndMakeVisible (paramPicker);
        paramPicker.onChange = [this] { pickParam (paramPicker.getSelectedId() - 2); };
        addAndMakeVisible (labelEd);
        labelEd.setTextToShowWhenEmpty ("Name on the screen (the parameter's own when empty)", textDim);
        labelEd.onReturnKey = labelEd.onFocusLost = [this] { renameKnob(); };
        addAndMakeVisible (switchTick);
        switchTick.setTooltip ("This control is on or off, not a range: the Stage view and the tablet show a switch for it, one tap to flip. "
                               "For a button the plugin gives as a range (Kontakt's all are); one the plugin says is a switch is ticked already.");
        switchTick.onClick = [this]
        {
            const bool on = switchTick.getToggleState();
            knobForEdit().isSwitch = on;
            saveAndShow();
        };
        addAndMakeVisible (fromWindowBtn);
        fromWindowBtn.setTooltip ("Opens the plugin's window: move the control you want, and this knob turns it");
        fromWindowBtn.onClick = [this] { pickFromWindow(); };
        addAndMakeVisible (clearBtn);
        clearBtn.onClick = [this] { pickParam (-1); };

        addAndMakeVisible (caption (keysCap, "KEY COLOURS: keyswitches, a kit's pads, the range that plays"));
        addAndMakeVisible (keysView);
        keysView.setViewedComponent (&keysList, false);
        keysView.setScrollBarsShown (true, false);
        addAndMakeVisible (addKeysBtn);
        addKeysBtn.onClick = [this] { makeOwn(); storeKeys(); edit.keys.push_back ({ 36, 47, keyColourChoices[edit.keys.size() % 9].rgb, {} }); saveAndShow(); };

        addAndMakeVisible (where);
        where.setColour (Label::textColourId, textDim);
        addAndMakeVisible (status);
        status.setColour (Label::textColourId, textDim);

        fillParams();
        load();
        showTab();
        startTimerHz (15);
    }

    ~ControlMapEditor() override
    {
        engine.removeListener (this);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        auto top = r.removeFromTop (26);
        keysTab.setBounds (top.removeFromRight (110));
        top.removeFromRight (4);
        knobsTab.setBounds (top.removeFromRight (80));
        title.setBounds (top);
        subtitle.setBounds (r.removeFromTop (50));
        r.removeFromTop (6);

        auto row = r.removeFromTop (26);
        layoutCap.setBounds (row.removeFromLeft (56));
        shareBtn.setBounds (row.removeFromRight (90));
        row.removeFromRight (8);
        layoutBox.setBounds (row);
        r.removeFromTop (12);

        auto bottom = r.removeFromBottom (20);
        status.setBounds (bottom);
        where.setBounds (r.removeFromBottom (20));
        r.removeFromBottom (6);

        // Knobs: the pages, the eight boxes, and the chosen one's editor.
        auto knobsArea = r;
        row = knobsArea.removeFromTop (26);
        removePageBtn.setBounds (row.removeFromRight (110));
        row.removeFromRight (4);
        addPageBtn.setBounds (row.removeFromRight (80));
        row.removeFromRight (8);
        pageNameEd.setBounds (row.removeFromRight (160));
        row.removeFromRight (8);
        pageTabsArea = row;
        layoutPageTabs();
        knobsArea.removeFromTop (10);
        auto tileRow = knobsArea.removeFromTop (118);
        const int gap = 6, w = (tileRow.getWidth() - gap * 7) / 8;
        for (int k = 0; k < ControlPage::numKnobs; ++k)
            tiles[(size_t) k].setBounds (tileRow.getX() + k * (w + gap), tileRow.getY(), w, tileRow.getHeight());
        knobsArea.removeFromTop (12);
        editCap.setBounds (knobsArea.removeFromTop (18));
        row = knobsArea.removeFromTop (28);
        clearBtn.setBounds (row.removeFromRight (70));
        row.removeFromRight (4);
        fromWindowBtn.setBounds (row.removeFromRight (190));
        row.removeFromRight (8);
        paramPicker.setBounds (row);
        knobsArea.removeFromTop (6);
        row = knobsArea.removeFromTop (26);
        switchTick.setBounds (row.removeFromRight (150));
        row.removeFromRight (8);
        labelEd.setBounds (row);

        // Keys.
        auto keysArea = r;
        row = keysArea.removeFromTop (26);
        addKeysBtn.setBounds (row.removeFromRight (110));
        keysCap.setBounds (row);
        keysArea.removeFromTop (4);
        keysView.setBounds (keysArea);
        layoutKeys();
    }

private:
    /** One knob: its name, what it turns, and where it is. */
    struct Tile : public Component
    {
        int number = 1;
        String name, param, value;
        float level = -1.0f;
        bool selected = false, lit = false, empty = true;
        std::function<void()> onClick;

        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (1.0f);
            g.setColour (lit ? Colour (0xff3a4f6e) : bgRow);
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (selected ? Colour (0xff5fa8ff) : Colour (0xff444852));
            g.drawRoundedRectangle (r, 6.0f, selected ? 2.0f : 1.0f);

            auto inner = getLocalBounds().reduced (6);
            g.setColour (textDim);
            g.setFont (FontOptions (10.5f));
            g.drawText ("KNOB " + String (number), inner.removeFromTop (14), Justification::centred);

            // The knob itself: an arc to where it is.
            auto dial = inner.removeFromTop (46).toFloat().withSizeKeepingCentre (40.0f, 40.0f);
            const float range = MathConstants<float>::pi * 1.5f;
            Path track;
            track.addCentredArc (dial.getCentreX(), dial.getCentreY(), 17.0f, 17.0f, 0.0f, -range / 2.0f, range / 2.0f, true);
            g.setColour (Colour (0xff50555f));
            g.strokePath (track, PathStrokeType (4.0f, PathStrokeType::curved, PathStrokeType::rounded));
            if (level >= 0.0f)
            {
                Path on;
                on.addCentredArc (dial.getCentreX(), dial.getCentreY(), 17.0f, 17.0f, 0.0f, -range / 2.0f, -range / 2.0f + range * level, true);
                g.setColour (Colour (0xff5fa8ff));
                g.strokePath (on, PathStrokeType (4.0f, PathStrokeType::curved, PathStrokeType::rounded));
            }

            g.setColour (empty ? textDim : Colours::white);
            g.setFont (FontOptions (13.5f, Font::bold));
            g.drawFittedText (empty ? String ("(nothing)") : name, inner.removeFromTop (20), Justification::centred, 1, 0.8f);
            g.setColour (textDim);
            g.setFont (FontOptions (11.5f));
            g.drawFittedText (value.isNotEmpty() ? value : param, inner.removeFromTop (16), Justification::centred, 1, 0.8f);
            if (value.isNotEmpty() && param.isNotEmpty() && param != name)
                g.drawFittedText (param, inner.removeFromTop (14), Justification::centred, 1, 0.8f);
        }
        void mouseDown (const MouseEvent&) override { if (onClick) onClick(); }
    };

    struct KeyRow : public Component
    {
        TextEditor low, high, label;
        ComboBox colour;
        TextButton remove { "x" };
        KeyRow()
        {
            for (auto* c : std::initializer_list<Component*> { &low, &high, &label, &colour, &remove }) addAndMakeVisible (c);
            low.setTooltip ("The lowest key, as a note (C1, F#2) or a number, as the plugin hears it");
            high.setTooltip ("The highest key");
            label.setTextToShowWhenEmpty ("What these keys are (\"Articulations\")", textDim);
            for (int i = 0; i < (int) std::size (keyColourChoices); ++i) colour.addItem (keyColourChoices[i].name, i + 1);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (0, 2);
            remove.setBounds (r.removeFromRight (26)); r.removeFromRight (6);
            low.setBounds (r.removeFromLeft (64)); r.removeFromLeft (4);
            high.setBounds (r.removeFromLeft (64)); r.removeFromLeft (6);
            colour.setBounds (r.removeFromLeft (100)); r.removeFromLeft (6);
            label.setBounds (r);
        }
    };

    /** Whatever plugin sits in this window's slot now, or null when there is none. */
    const SlotDef* slotAt() const
    {
        const auto& inputs = engine.getSetup().inputs;
        if (input < 0 || input >= (int) inputs.size() || program < 0 || program >= InputDef::numPrograms) return nullptr;
        const auto& prog = inputs[(size_t) input].programs[(size_t) program];
        if (slot < 0 || slot >= (int) prog.slots.size() || ! prog.slots[(size_t) slot].isPlugin()) return nullptr;
        return &prog.slots[(size_t) slot];
    }

    /** The instrument this window is for, or null once it is gone (removed,
        replaced by another plugin, or its input). Only meaningful after the
        constructor has taken the plugin from the slot. */
    const SlotDef* slotDef() const
    {
        const auto* sd = slotAt();
        return sd != nullptr && sd->plugin.name == plugin.name && sd->plugin.fileOrIdentifier == plugin.fileOrIdentifier ? sd : nullptr;
    }
    RemotePlugin* livePlugin() const { return slotDef() != nullptr && engine.isPluginAlive (input, program, slot) ? engine.getPlugin (input, program, slot) : nullptr; }

    /** The instrument went (or another took its place): the window closes rather than edit the wrong thing. */
    bool stillHere()
    {
        if (slotDef() != nullptr) return true;
        if (! closing)
        {
            closing = true;
            MessageManager::callAsync ([sp = SafePointer<ControlMapEditor> (this)]
            {
                if (sp == nullptr) return;
                if (auto* dw = sp->findParentComponentOfClass<DialogWindow>()) delete dw;
            });
        }
        return false;
    }
    void setupChanged() override                        { stillHere(); }
    void programContentChanged (int, int) override      { stillHere(); }

    void fillParams()
    {
        auto* p = livePlugin();
        params = p != nullptr ? p->getParameters() : ParamInfoList();
        paramPicker.clear (dontSendNotification);
        paramPicker.addItem ("(nothing)", 1);
        for (const auto& prm : params)
            if (prm.midiController < 0) paramPicker.addItem (prm, prm.index + 2);
    }

    int paramIndexOf (const ControlKnob& knob) const
    {
        for (const auto& prm : params)
            if ((knob.paramId.isNotEmpty() && prm.id == knob.paramId) || (knob.paramId.isEmpty() && knob.paramName.isNotEmpty() && prm.name.equalsIgnoreCase (knob.paramName)))
                return prm.index;
        return -1;
    }

    /** What is in use now: one of yours, or the automatic pages made into a layout to show (not saved). */
    void load()
    {
        if (! stillHere()) return;
        names.clear();
        for (const auto* m : library.forPlugin (plugin)) names.add (m->name);
        const auto chosen = slotDef()->controlMap;
        own = false;
        edit = {};
        if (chosen != "-")
            if (const auto* m = library.find (plugin, chosen)) { edit = *m; own = true; }
        if (! own) edit = automaticLayout();

        layoutBox.clear (dontSendNotification);
        layoutBox.addItem (automaticName(), 1);
        for (int i = 0; i < names.size(); ++i) layoutBox.addItem ("Yours: " + names[i], 10 + i);
        layoutBox.setSelectedId (own ? 10 + jmax (0, names.indexOf (edit.name, true)) : 1, dontSendNotification);
        page = jlimit (0, jmax (0, (int) edit.pages.size() - 1), page);
        showAll();
    }

    String automaticName() const
    {
        // The sound it was given has pages of its own (a Kontakt instrument's): those, by its name.
        if (const auto* sd = slotDef(); sd != nullptr && library.nksOfFile (sd->soundFile) != nullptr)
            return "Automatic: the maker's pages for " + (sd->soundName.isNotEmpty() ? sd->soundName : String ("this sound"));
        if (const auto* nks = library.findNks (plugin))
            return "Automatic: " + (nks->vendor.isNotEmpty() ? nks->vendor + "'s" : String ("the maker's")) + " own pages";
        return "Automatic: StageRig's picks, then every parameter";
    }

    ControlMap automaticLayout() const
    {
        ControlMap m;
        m.pluginName = plugin.name;
        m.pluginManufacturer = plugin.manufacturerName;
        auto* p = livePlugin();
        if (p == nullptr) return m;
        // As the keyboard has it with no layout of yours: what the hub works out.
        const auto layout = hub.layoutFor (input, program, slot, true);
        for (const auto& pg : layout.pages)
        {
            ControlPage page;
            page.name = pg.name;
            for (const auto& k : pg.knobs)
            {
                ControlKnob knob;
                if (k.paramIndex >= 0 && k.paramIndex < (int) p->getParameters().size())
                {
                    const auto& prm = p->getParameters()[(size_t) k.paramIndex];
                    knob = { prm.id, prm.name, k.name != prm.name ? k.name : String() };
                }
                page.knobs.push_back (knob);
            }
            while (! page.knobs.empty() && page.knobs.back().isEmpty()) page.knobs.pop_back();
            m.pages.push_back (page);
        }
        if (m.pages.empty()) m.pages.push_back ({ "Sound", {} });
        return m;
    }

    /** About to change something: an automatic layout becomes yours first. */
    void makeOwn()
    {
        storeKeys();
        if (own) return;
        auto m = automaticLayout();
        // Named for the instrument inside the plugin when the sound says which (a Kontakt library), else for the plugin.
        String base = plugin.name;
        if (const auto* sd = slotDef(); sd != nullptr)
            if (const auto* own = library.nksOfFile (sd->soundFile); own != nullptr && own->product.isNotEmpty()) base = own->product;
        m.name = base;
        for (int n = 2; library.find (plugin, m.name) != nullptr; ++n) m.name = base + " " + String (n);
        if (const auto* nks = library.findNks (plugin)) m.notes = "Made from " + (nks->vendor.isNotEmpty() ? nks->vendor + "'s" : String ("the maker's")) + " pages.";
        m.keys = edit.keys;
        edit = m;
        own = true;
        madeOwn = true;
    }

    void saveAndShow()
    {
        if (! stillHere()) return;
        if (own)
        {
            if (const auto r = library.save (edit); r.failed()) { status.setText (r.getErrorMessage(), dontSendNotification); return; }
            engine.setSlotControlMap (input, program, slot, edit.name);
            hub.libraryChanged();
            if (changed) changed();
            status.setText (madeOwn ? "Saved as your layout \"" + edit.name + "\": every program with " + plugin.name + " now uses it. "
                                      "Automatic is still there in Layout."
                                    : "Saved.", dontSendNotification);
            madeOwn = false;
        }
        const int keepPage = page, keepSel = selected;
        load();
        page = jlimit (0, jmax (0, (int) edit.pages.size() - 1), keepPage);
        selected = keepSel;
        showAll();
    }

    void chooseLayout()
    {
        if (! stillHere()) return;
        const int id = layoutBox.getSelectedId();
        storeKeys();
        engine.setSlotControlMap (input, program, slot, id >= 10 ? names[id - 10] : String ("-"));
        hub.libraryChanged();
        if (changed) changed();
        page = 0;
        load();
        status.setText (id >= 10 ? String() : String ("Automatic: StageRig works the pages out. Change a knob to make them yours."), dontSendNotification);
    }

    void showAll()
    {
        // Page tabs: made again only when the pages change, never from inside a tab's own click.
        StringArray tabNames;
        for (int i = 0; i < (int) edit.pages.size(); ++i)
            tabNames.add (edit.pages[(size_t) i].name.isNotEmpty() ? edit.pages[(size_t) i].name : "Page " + String (i + 1));
        bool sameTabs = tabNames.size() == (int) pageTabs.size();
        for (int i = 0; sameTabs && i < tabNames.size(); ++i)
            sameTabs = pageTabs[(size_t) i]->getButtonText() == tabNames[i];
        if (! sameTabs)
        {
            pageTabs.clear();
            for (int i = 0; i < tabNames.size(); ++i)
            {
                auto b = std::make_unique<TextButton> (tabNames[i]);
                b->setRadioGroupId (2);
                b->onClick = [this, i]
                {
                    if (i == page) { showTabStates(); return; }
                    page = i;
                    MessageManager::callAsync ([sp = SafePointer<ControlMapEditor> (this)] { if (sp != nullptr) sp->showAll(); });
                    showTabStates();
                };
                addAndMakeVisible (*b);
                pageTabs.push_back (std::move (b));
            }
        }
        showTabStates();
        pageBox.clear (dontSendNotification);
        for (int i = 0; i < (int) edit.pages.size(); ++i)
            pageBox.addItem ("Page " + String (i + 1) + " of " + String ((int) edit.pages.size()) + ": " + edit.pages[(size_t) i].name, i + 1);
        pageBox.setSelectedId (page + 1, dontSendNotification);
        layoutPageTabs();
        pageNameEd.setText (page < (int) edit.pages.size() ? edit.pages[(size_t) page].name : String(), dontSendNotification);
        removePageBtn.setEnabled (edit.pages.size() > 1);
        shareBtn.setEnabled (true);

        showTiles();
        showSelected();
        showKeys();

        StringArray shown, turned, taught, untaught, silent;
        for (const auto& k : hub.getKeyboards())
        {
            if (k.input != input) continue;
            if (k.knobsSilent && ! k.knobsByThemselves) silent.add (k.name);
            else if (k.plain || ! k.knobsByThemselves) (k.knobsByThemselves ? taught : untaught).add (k.name);
            else (k.screen ? shown : turned).add (k.name);
        }
        String text;
        if (! shown.isEmpty()) text << "Shown on " << shown.joinIntoString (", ") << ". ";
        if (! turned.isEmpty()) text << "Turned by " << turned.joinIntoString (", ") << ", which " << (turned.size() == 1 ? "has" : "have") << " no screen for the names. ";
        if (! taught.isEmpty()) text << "Turned by the taught knobs of " << taught.joinIntoString (", ") << ". ";
        if (! untaught.isEmpty()) text << untaught.joinIntoString (", ") << " plays this input as plain MIDI: teach its knobs in Options > Your keyboards. ";
        if (! silent.isEmpty()) text << silent.joinIntoString (", ") << " plays this input as plain MIDI, and its knobs send no MIDI at all: "
                                        "untick Plain MIDI for it in Options > Your keyboards and they turn these. ";
        if (text.isEmpty()) text = "No keyboard plays this input yet: choose its MIDI device in the Inputs list.";
        // The other kind of control, in this program only: a pedal or wheel mapped to this plugin.
        StringArray mapped;
        for (const auto& m : engine.getSetup().inputs[(size_t) input].programs[(size_t) program].mappings)
            if (m.slot == slot && m.effect < 0)
                mapped.add (m.sourceDescription() + " turns " + (m.paramName.isNotEmpty() ? m.paramName : m.paramId));
        if (! mapped.isEmpty()) text << " In this program " << mapped.joinIntoString (", ") << " (MIDI mappings).";
        where.setText (text, dontSendNotification);
        if (livePlugin() == nullptr) status.setText ("The plugin is not running, so its parameters cannot be listed. Load the program first.", dontSendNotification);
    }

    void showTabStates()
    {
        for (int i = 0; i < (int) pageTabs.size(); ++i)
            pageTabs[(size_t) i]->setToggleState (i == page, dontSendNotification);
    }

    /** The pages as tabs while they fit across; a list when there are too many (an automatic layout can have dozens). */
    void layoutPageTabs()
    {
        auto r = pageTabsArea;
        int needed = 0;
        for (auto& b : pageTabs) needed += jmin (140, jmax (60, b->getBestWidthForHeight (r.getHeight()))) + 4;
        const bool asTabs = needed <= r.getWidth();
        pageBox.setVisible (! asTabs && knobsTab.getToggleState());
        pageBox.setBounds (r.withWidth (jmin (260, r.getWidth())));
        for (auto& b : pageTabs)
        {
            b->setVisible (asTabs && knobsTab.getToggleState());
            const int w = jmin (140, jmax (60, b->getBestWidthForHeight (r.getHeight())));
            b->setBounds (r.removeFromLeft (w));
            r.removeFromLeft (4);
        }
    }

    void showTiles()
    {
        auto* p = livePlugin();
        const ControlPage* pg = page < (int) edit.pages.size() ? &edit.pages[(size_t) page] : nullptr;
        for (int k = 0; k < ControlPage::numKnobs; ++k)
        {
            auto& t = tiles[(size_t) k];
            ControlKnob knob;
            if (pg != nullptr && k < (int) pg->knobs.size()) knob = pg->knobs[(size_t) k];
            const int idx = paramIndexOf (knob);
            t.empty = idx < 0;
            t.param = idx >= 0 ? params[(size_t) idx].name : String();
            t.name = knob.label.isNotEmpty() ? knob.label : t.param;
            t.level = -1.0f;
            t.value = {};
            if (p != nullptr && idx >= 0 && idx < (int) p->getParameters().size())
            {
                const auto& prm = p->getParameters()[(size_t) idx];
                t.level = jlimit (0.0f, 1.0f, p->getCachedParameterValue (idx));
                t.value = prm.boolean || prm.numSteps == 2 || knob.isSwitch ? String (t.level >= 0.5f ? "On" : "Off") : String (roundToInt (t.level * 100.0f)) + "%";
            }
            t.selected = k == selected;
            t.repaint();
        }
    }

    void select (int k)
    {
        selected = k;
        waitingForWindow = false;
        showTiles();
        showSelected();
    }

    void showSelected()
    {
        const ControlPage* pg = page < (int) edit.pages.size() ? &edit.pages[(size_t) page] : nullptr;
        ControlKnob knob;
        if (pg != nullptr && selected < (int) pg->knobs.size()) knob = pg->knobs[(size_t) selected];
        editCap.setText ("KNOB " + String (selected + 1) + " TURNS", dontSendNotification);
        const int idx = paramIndexOf (knob);
        paramPicker.setSelectedId (idx >= 0 ? idx + 2 : 1, dontSendNotification);
        labelEd.setText (knob.label, dontSendNotification);
        const bool running = livePlugin() != nullptr;
        paramPicker.setEnabled (running);
        fromWindowBtn.setEnabled (running);
        fromWindowBtn.setButtonText (waitingForWindow ? "Move it in the plugin now..." : "Pick it in the plugin's window");
        clearBtn.setEnabled (idx >= 0 || knob.label.isNotEmpty());
        // On or off: as the plugin says where it does, else as set here.
        const bool byPlugin = idx >= 0 && idx < (int) params.size() && (params[(size_t) idx].boolean || params[(size_t) idx].numSteps == 2);
        switchTick.setToggleState (byPlugin || knob.isSwitch, dontSendNotification);
        switchTick.setEnabled (idx >= 0 && ! byPlugin);
    }

    ControlKnob& knobForEdit()
    {
        makeOwn();
        page = jlimit (0, jmax (0, (int) edit.pages.size() - 1), page);
        if (edit.pages.empty()) edit.pages.push_back ({ "Sound", {} });
        auto& knobs = edit.pages[(size_t) page].knobs;
        while ((int) knobs.size() <= selected) knobs.push_back ({});
        return knobs[(size_t) selected];
    }

    void trimPage()
    {
        if (page >= (int) edit.pages.size()) return;
        auto& knobs = edit.pages[(size_t) page].knobs;
        while (! knobs.empty() && knobs.back().isEmpty() && knobs.back().label.isEmpty()) knobs.pop_back();
    }

    void pickParam (int idx)
    {
        auto& knob = knobForEdit();
        if (idx >= 0 && idx < (int) params.size()) knob = { params[(size_t) idx].id, params[(size_t) idx].name, knob.label };
        else knob = {};
        waitingForWindow = false;
        trimPage();
        saveAndShow();
    }

    void renameKnob()
    {
        const ControlPage* pg = page < (int) edit.pages.size() ? &edit.pages[(size_t) page] : nullptr;
        const auto now = pg != nullptr && selected < (int) pg->knobs.size() ? pg->knobs[(size_t) selected].label : String();
        const auto text = labelEd.getText().trim();
        if (text == now) return;
        knobForEdit().label = text;
        trimPage();
        saveAndShow();
    }

    void renamePage()
    {
        const auto text = pageNameEd.getText().trim();
        if (page >= (int) edit.pages.size() || text.isEmpty() || text == edit.pages[(size_t) page].name) return;
        makeOwn();
        edit.pages[(size_t) page].name = text;
        saveAndShow();
    }

    void pickFromWindow()
    {
        if (! stillHere()) return;
        auto* p = livePlugin();
        if (p == nullptr) return;
        waitingForWindow = ! waitingForWindow;
        if (waitingForWindow)
            if (const auto problem = engine.openPluginWindow (input, program, slot, -1); problem.isNotEmpty())
            { status.setText (problem, dontSendNotification); waitingForWindow = false; }
        showSelected();
    }

    // Engine::Listener: a control moved in the plugin's window, while one is waited for.
    void parameterTouched (int in, int prog, int sl, int effect, int paramIndex) override
    {
        if (! waitingForWindow || in != input || prog != program || sl != slot || effect >= 0) return;
        pickParam (paramIndex);
        status.setText ("Knob " + String (selected + 1) + " turns " + (paramIndex >= 0 && paramIndex < (int) params.size() ? params[(size_t) paramIndex].name : String ("it")) + ".",
                        dontSendNotification);
    }

    void timerCallback() override
    {
        if (! stillHere()) return;
        // A plugin that has just come up: list its parameters, and show them once there are some
        // (reloading every tick for a plugin with none would remake the page tabs under the mouse).
        if (params.empty() && livePlugin() != nullptr) { fillParams(); if (! params.empty()) load(); }
        // The knob just turned on the keyboard lights up; the values follow the plugin.
        const auto turn = hub.getLastTurn (input);
        const bool recent = turn.knob >= 0 && Time::getMillisecondCounter() - turn.at < 700;
        for (int k = 0; k < ControlPage::numKnobs; ++k)
            tiles[(size_t) k].lit = recent && turn.knob == k;
        if (knobsTab.getToggleState()) showTiles();
    }

    void showTab()
    {
        const bool knobs = knobsTab.getToggleState();
        for (auto* c : std::initializer_list<Component*> { &addPageBtn, &removePageBtn, &pageNameEd, &editCap, &paramPicker, &labelEd, &switchTick, &fromWindowBtn, &clearBtn })
            c->setVisible (knobs);
        for (auto& t : tiles) t.setVisible (knobs);
        layoutPageTabs();
        for (auto* c : std::initializer_list<Component*> { &keysCap, &keysView, &addKeysBtn }) c->setVisible (! knobs);
    }

    void showKeys()
    {
        keyRows.clear();
        for (const auto& z : edit.keys)
        {
            auto row = std::make_unique<KeyRow>();
            row->low.setText (noteName (z.low), dontSendNotification);
            row->high.setText (noteName (z.high), dontSendNotification);
            row->label.setText (z.label, dontSendNotification);
            int pick = 1;
            for (int i = 0; i < (int) std::size (keyColourChoices); ++i) if (keyColourChoices[i].rgb == z.colour) pick = i + 1;
            row->colour.setSelectedId (pick, dontSendNotification);
            auto* raw = row.get();
            row->low.onFocusLost = row->high.onFocusLost = row->label.onFocusLost = row->low.onReturnKey = row->high.onReturnKey = row->label.onReturnKey
                = [this] { keysEdited(); };
            row->colour.onChange = [this] { keysEdited(); };
            row->remove.onClick = [this, raw]
            {
                storeKeys();
                for (size_t i = 0; i < keyRows.size(); ++i)
                    if (keyRows[i].get() == raw) { makeOwn(); edit.keys.erase (edit.keys.begin() + (long) i); break; }
                MessageManager::callAsync ([sp = SafePointer<ControlMapEditor> (this)] { if (sp != nullptr) sp->saveAndShow(); });
            };
            keysList.addAndMakeVisible (*row);
            keyRows.push_back (std::move (row));
        }
        layoutKeys();
    }

    void keysEdited()
    {
        const auto before = edit.keys;
        storeKeys();
        bool same = before.size() == edit.keys.size();
        for (size_t i = 0; same && i < before.size(); ++i)
            same = before[i].low == edit.keys[i].low && before[i].high == edit.keys[i].high && before[i].colour == edit.keys[i].colour && before[i].label == edit.keys[i].label;
        if (same) return;
        const auto keys = edit.keys;
        makeOwn();
        edit.keys = keys;
        saveAndShow();
    }

    void storeKeys()
    {
        for (size_t i = 0; i < keyRows.size() && i < edit.keys.size(); ++i)
        {
            auto& z = edit.keys[i];
            const int lo = parseNoteName (keyRows[i]->low.getText()), hi = parseNoteName (keyRows[i]->high.getText());
            if (lo >= 0) z.low = lo;
            if (hi >= 0) z.high = hi;
            if (z.high < z.low) std::swap (z.low, z.high);
            const int c = keyRows[i]->colour.getSelectedId() - 1;
            if (c >= 0 && c < (int) std::size (keyColourChoices)) z.colour = keyColourChoices[c].rgb;
            z.label = keyRows[i]->label.getText().trim();
        }
    }

    void layoutKeys()
    {
        const int w = jmax (200, keysView.getWidth() - keysView.getScrollBarThickness());
        keysList.setSize (w, jmax (30, (int) keyRows.size() * 30));
        for (size_t i = 0; i < keyRows.size(); ++i) keyRows[i]->setBounds (0, (int) i * 30, w, 30);
    }

    void shareMenu()
    {
        PopupMenu m;
        m.addItem (1, "Import a layout someone shared...");
        m.addItem (2, "Export this layout, to share...", own);
        m.addSeparator();
        m.addItem (3, "Rename this layout...", own);
        m.addItem (4, "Delete this layout", own);
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (shareBtn), [sp = SafePointer<ControlMapEditor> (this)] (int r)
        {
            if (sp == nullptr) return;
            if (r == 1) sp->importLayout();
            if (r == 2) sp->exportLayout();
            if (r == 3) sp->renameLayout();
            if (r == 4) sp->deleteLayout();
        });
    }

    void renameLayout()
    {
        auto* w = new AlertWindow ("Rename the layout", "What it is for: the plugin, or the instrument inside it (\"Alicia's Keys\").", MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", edit.name);
        w->addButton ("Rename", 1, KeyPress (KeyPress::returnKey));
        w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
        w->enterModalState (true, ModalCallbackFunction::create ([sp = SafePointer<ControlMapEditor> (this), w] (int r)
        {
            std::unique_ptr<AlertWindow> owned (w);
            if (sp == nullptr || r == 0) return;
            const auto name = w->getTextEditorContents ("name").trim();
            if (name.isEmpty() || name.equalsIgnoreCase (sp->edit.name)) return;
            if (sp->library.find (sp->plugin, name) != nullptr) { sp->status.setText ("There is already a layout called " + name + ".", dontSendNotification); return; }
            const auto old = sp->edit;
            sp->edit.name = name;
            sp->library.remove (old);
            sp->saveAndShow();
        }), false);
    }

    void deleteLayout()
    {
        const auto doomed = edit;
        AlertWindow::showOkCancelBox (AlertWindow::WarningIcon, "Delete the layout", "Delete your layout " + doomed.name + " for " + plugin.name
                                      + "? The knobs go back to automatic. There is no undo.", "Delete", "Keep", nullptr,
                                      ModalCallbackFunction::create ([sp = SafePointer<ControlMapEditor> (this), doomed] (int r)
        {
            if (sp == nullptr || r == 0) return;
            sp->library.remove (doomed);
            sp->engine.setSlotControlMap (sp->input, sp->program, sp->slot, {});
            sp->hub.libraryChanged();
            if (sp->changed) sp->changed();
            sp->load();
        }));
    }

    void importLayout()
    {
        chooser = std::make_unique<FileChooser> ("Import a layout", File::getSpecialLocation (File::userHomeDirectory).getChildFile ("Downloads"),
                                                 String ("*") + ControlMap::fileExtension + ";*.json");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [sp = SafePointer<ControlMapEditor> (this)] (const FileChooser& fc)
        {
            if (sp == nullptr || fc.getResult() == File()) return;
            ControlMap m;
            if (const auto r = sp->library.importFile (fc.getResult(), &m); r.failed()) { sp->status.setText (r.getErrorMessage(), dontSendNotification); return; }
            sp->hub.libraryChanged();
            if (! m.appliesTo (sp->plugin))
            {
                sp->status.setText ("Imported " + m.name + ", a layout for " + m.pluginName + ": it is there for that plugin.", dontSendNotification);
                return;
            }
            sp->engine.setSlotControlMap (sp->input, sp->program, sp->slot, m.name);
            if (sp->changed) sp->changed();
            sp->load();
            sp->status.setText ("Imported " + m.name + (m.author.isNotEmpty() ? ", made by " + m.author : String()) + ".", dontSendNotification);
        });
    }

    void exportLayout()
    {
        if (! own) return;
        chooser = std::make_unique<FileChooser> ("Export the layout", File::getSpecialLocation (File::userDocumentsDirectory).getChildFile (library.fileFor (edit).getFileName()),
                                                 String ("*") + ControlMap::fileExtension);
        chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting, [sp = SafePointer<ControlMapEditor> (this)] (const FileChooser& fc)
        {
            if (sp == nullptr || fc.getResult() == File()) return;
            auto f = fc.getResult();
            if (! f.hasFileExtension (ControlMap::fileExtension)) f = f.withFileExtension (ControlMap::fileExtension);
            const auto r = sp->edit.saveToFile (f);
            sp->status.setText (r.wasOk() ? "Exported to " + f.getFullPathName() + ". Whoever has " + sp->plugin.name + " can import it." : r.getErrorMessage(), dontSendNotification);
        });
    }

    Engine& engine;
    SurfaceHub& hub;
    ControlMapLibrary& library;
    const int input, program, slot;
    std::function<void()> changed;
    PluginDescription plugin;
    ParamInfoList params;
    StringArray names;
    ControlMap edit;
    bool own = false, madeOwn = false, waitingForWindow = false, closing = false;
    int page = 0, selected = 0;

    Label title, subtitle, layoutCap, editCap, keysCap, where, status;
    TextButton knobsTab { "Knobs" }, keysTab { "Key colours" };
    ComboBox layoutBox, pageBox;
    TextButton shareBtn { "Share..." }, addPageBtn { "+ Page" }, removePageBtn { "Remove page" }, fromWindowBtn, clearBtn { "Clear" }, addKeysBtn { "+ Keys" };
    TextEditor pageNameEd, labelEd;
    ToggleButton switchTick { "On/off switch" };
    std::vector<std::unique_ptr<TextButton>> pageTabs;
    Rectangle<int> pageTabsArea;
    std::array<Tile, ControlPage::numKnobs> tiles;
    ParamPicker paramPicker;
    Viewport keysView;
    Component keysList;
    std::vector<std::unique_ptr<KeyRow>> keyRows;
    std::unique_ptr<FileChooser> chooser;
};

//==============================================================================
/** Your keyboards: each keyboard an input plays from, how StageRig talks to
    it, and the two switches that are about the device rather than a program:
    Plain MIDI, and for one that speaks MIDI 2.0, hearing it as MIDI 1.0. What
    its knobs and buttons do is elsewhere, on purpose: the rig's and the
    keyboard's own jobs in the Controls panel, what the knobs turn in each
    instrument's Knobs window. */
class ControllersView : public Component, private Timer
{
public:
    ControllersView (Engine& e, SurfaceHub& h, std::function<void()> ch) : engine (e), hub (h), changed (std::move (ch))
    {
        addAndMakeVisible (caption (foundCap, "YOUR KEYBOARDS"));
        addAndMakeVisible (keyboardsView);
        keyboardsView.setViewedComponent (&keyboardsList, false);
        keyboardsView.setScrollBarsShown (true, false);
        addAndMakeVisible (none);
        none.setColour (Label::textColourId, textDim);
        none.setJustificationType (Justification::topLeft);
        addAndMakeVisible (reportBtn);
        reportBtn.setTooltip ("Copies what StageRig knows about your keyboards and MIDI ports, and what they have said to it lately, "
                              "to paste into a message when something does not work.");
        reportBtn.onClick = [this]
        {
            SystemClipboard::copyTextToClipboard (hub.getReport());
            reportBtn.setButtonText ("Copied");
            Timer::callAfterDelay (1500, [sp = SafePointer<ControllersView> (this)] { if (sp != nullptr) sp->reportBtn.setButtonText ("Copy report"); });
        };
        addAndMakeVisible (explain);
        explain.setColour (Label::textColourId, textDim);
        explain.setJustificationType (Justification::topLeft);
        explain.setText ("StageRig talks to a keyboard the most standard way it knows -- MIDI-CI, else its maker's own protocol -- and plays any other "
                         "as plain MIDI. What a keyboard's pedals, buttons and knobs do is set in the Controls panel of the main window, where its own jobs "
                         "are listed under its name; what its knobs turn on an instrument is that instrument's Knobs window.", dontSendNotification);
        showKeyboards();
        startTimer (500);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        auto head = r.removeFromTop (22);
        reportBtn.setBounds (head.removeFromRight (110));
        foundCap.setBounds (head);
        r.removeFromTop (4);
        explain.setBounds (r.removeFromBottom (52));
        r.removeFromBottom (8);
        keyboardsView.setBounds (r);
        none.setBounds (r);
        layoutKeyboards();
    }

private:
    static constexpr int rowHeight = 46;

    /** One keyboard: what it is, what it plays, how StageRig talks to it and what it does. */
    struct KeyboardRow : public Component
    {
        Label name, detail;
        ToggleButton plain { "Plain MIDI" }, midi1 { "Hear as MIDI 1.0" };
        KeyboardRow()
        {
            addAndMakeVisible (name);
            name.setFont (FontOptions (14.0f, Font::bold));
            addAndMakeVisible (detail);
            detail.setColour (Label::textColourId, textDim);
            addAndMakeVisible (plain);
            plain.setTooltip ("Only a plain MIDI keyboard: its keys, pedals and taught knobs, and StageRig sends it nothing. "
                              "For a keyboard that misbehaves when StageRig talks to it.");
            addChildComponent (midi1);
            midi1.setTooltip ("This keyboard speaks MIDI 2.0, so StageRig hears its knobs, pedals and pressure at full resolution "
                              "instead of in 128 steps: a slow sweep of a mapped knob moves the plugin smoothly. Tick this to hear it "
                              "as MIDI 1.0 instead, for a keyboard that misbehaves. The plugins get MIDI 1.0 either way.");
        }
        void paint (Graphics& g) override { g.setColour (bgRow); g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0, 2), 4.0f); }
        void resized() override
        {
            auto r = getLocalBounds().reduced (8, 4);
            plain.setBounds (r.removeFromRight (110));
            if (midi1.isVisible()) midi1.setBounds (r.removeFromRight (150));
            name.setBounds (r.removeFromTop (r.getHeight() / 2));
            detail.setBounds (r);
        }
    };

    void showKeyboards()
    {
        const auto list = hub.getKeyboards();
        const auto& inputs = engine.getSetup().inputs;
        // A keyboard that speaks MIDI 2.0 is heard as one device, whatever inputs play from it (Engine hears it per device).
        auto midi2 = [&] (const SurfaceHub::KeyboardInfo& k, bool& heardAsMidi1)
        {
            bool is = false;
            heardAsMidi1 = false;
            for (int i : k.inputs)
                if (i >= 0 && i < (int) inputs.size())
                {
                    if (inputs[(size_t) i].midi1Only) heardAsMidi1 = true;
                    if (inputs[(size_t) i].midi1Only || Engine::isMidi2Device (inputs[(size_t) i].midiDeviceIdentifier)) is = true;
                }
            return is;
        };
        String signature;
        for (const auto& k : list)
        {
            bool asMidi1 = false;
            const bool is2 = midi2 (k, asMidi1);
            signature << k.name << k.port << k.input << k.language << (int) k.screen << (int) k.lights << (int) k.knobsByThemselves << (int) k.plain << (int) is2 << (int) asMidi1 << "|";
        }
        for (const auto& p : hub.getProblems()) signature << p;
        if (signature == shownKeyboards) return;
        shownKeyboards = signature;

        keyboardRows.clear();
        for (const auto& k : list)
        {
            auto row = std::make_unique<KeyboardRow>();
            StringArray playing;
            for (int i : k.inputs) if (i >= 0 && i < (int) inputs.size()) playing.add (inputs[(size_t) i].name);
            if (playing.isEmpty() && k.input >= 0 && k.input < (int) inputs.size()) playing.add (inputs[(size_t) k.input].name);
            const auto plays = playing.isEmpty() ? String ("plays no input") : "plays " + playing.joinIntoString (" and ");
            row->name.setText (k.name + "   " + plays, dontSendNotification);
            StringArray what;
            if (k.plain) what.add (k.more.isNotEmpty() ? "plain MIDI" : "plain MIDI, as set here");
            else         what.add (k.language);
            if (k.screen) what.add ("names on its screen");
            if (k.lights) what.add ("its keys lit");
            what.add (k.knobsByThemselves ? "its knobs work by themselves"
                      : k.knobsSilent    ? "its knobs send no MIDI, so they cannot be taught"
                                         : "teach its knobs in the Controls panel");
            if (k.plain && k.more.isNotEmpty()) what.add ("untick Plain MIDI for " + k.more);
            row->detail.setText (what.joinIntoString ("  -  "), dontSendNotification);
            row->plain.setToggleState (k.plain, dontSendNotification);
            row->plain.setEnabled (k.port.isNotEmpty());
            row->plain.onClick = [this, port = k.port, raw = row.get()]
            {
                hub.setPlainMidi (port, raw->plain.getToggleState());
                shownKeyboards.clear();
            };
            bool asMidi1 = false;
            row->midi1.setVisible (midi2 (k, asMidi1));
            row->midi1.setToggleState (asMidi1, dontSendNotification);
            row->midi1.onClick = [this, ins = k.inputs, raw = row.get()]
            {
                for (int i : ins) engine.setInputMidi1Only (i, raw->midi1.getToggleState());
                if (changed) changed();
                shownKeyboards.clear();
            };
            keyboardsList.addAndMakeVisible (*row);
            keyboardRows.push_back (std::move (row));
        }
        String text;
        for (const auto& p : hub.getProblems()) text << p << "\n";
        if (keyboardRows.empty())
            text = "No keyboard an input plays from is plugged in. Choose each input's MIDI device in the Inputs list; any keyboard plays, "
                   "and StageRig talks to one that can show names or light its keys by itself.\n" + text;
        none.setText (text.trimEnd(), dontSendNotification);
        none.setVisible (keyboardRows.empty());
        layoutKeyboards();
    }

    void layoutKeyboards()
    {
        const int w = jmax (200, keyboardsView.getWidth() - keyboardsView.getScrollBarThickness());
        keyboardsList.setSize (w, jmax (rowHeight, (int) keyboardRows.size() * rowHeight));
        for (size_t i = 0; i < keyboardRows.size(); ++i) keyboardRows[i]->setBounds (0, (int) i * rowHeight, w, rowHeight);
    }

    void timerCallback() override { showKeyboards(); }

    Engine& engine;
    SurfaceHub& hub;
    std::function<void()> changed;
    Label foundCap, none, explain;
    TextButton reportBtn { "Copy report" };
    Viewport keyboardsView;
    Component keyboardsList;
    std::vector<std::unique_ptr<KeyboardRow>> keyboardRows;
    String shownKeyboards { "(nothing shown yet)" };     // not empty: no keyboard at all is something to show too
};

//==============================================================================
/** Every pedal and button of the rig in one list, so there is one place to
    look. Rig-wide jobs (the set list, the backing track, the tempo, the loop,
    an output's volume) work from a pedal on any keyboard; a keyboard's own
    jobs (programs, pages, instruments) are per keyboard, and can be one of
    its own buttons on a keyboard StageRig talks to. MIDI mappings are the
    other kind -- one program only -- and are not here. */
class PedalsView : public Component, private Timer
{
public:
    PedalsView (Engine& e, SurfaceHub& h, std::function<void()> ch) : engine (e), hub (h), changed (std::move (ch))
    {
        addAndMakeVisible (explain);
        explain.setColour (Label::textColourId, textDim);
        explain.setJustificationType (Justification::topLeft);
        explain.setText ("Press Learn, then press the pedal or button. The jobs at the top work from any keyboard, in every program; "
                         "a keyboard's own jobs are under its name, and on a keyboard StageRig talks to can be one of its buttons.", dontSendNotification);
        addAndMakeVisible (view);
        view.setViewedComponent (&list, false);
        view.setScrollBarsShown (true, false);
        build();
        startTimer (250);
    }

    ~PedalsView() override { cancelAll(); }

    /** Nothing waits once the window is closed, or when another row starts: the engine's pedal learns and the hub's. */
    void cancelAll()
    {
        engine.cancelPedalLearns();
        hub.cancelLearn();
        for (auto& o : rows) o->learning = false;
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        explain.setBounds (r.removeFromTop (34));
        r.removeFromTop (4);
        view.setBounds (r);
        layoutRows();
    }

private:
    struct Row : public Component
    {
        enum Kind { job, header, note, toggle } kind = job;   // a Learn/Forget row; a section heading; a line of text; a tick
        String where;
        std::function<int()> get;                     // the controller taught, 0 for none (rig-wide jobs)
        std::function<String()> describe;             // a keyboard's own job: what it is, in words, or empty
        std::function<void()> arm, forget;
        std::function<bool()> armed;
        std::function<bool()> getTick;                // a toggle row: its state
        std::function<void (bool)> setTick;
        bool learning = false;
        Label name, what;
        TextButton learn { "Learn" }, forgetBtn { "Forget" };
        ToggleButton tick;

        Row()
        {
            addAndMakeVisible (name);
            addAndMakeVisible (what);
            what.setColour (Label::textColourId, textDim);
            addAndMakeVisible (learn);
            addAndMakeVisible (forgetBtn);
            addChildComponent (tick);
            tick.onClick = [this] { if (setTick) setTick (tick.getToggleState()); };
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (6, 2);
            if (kind == header || kind == note) { name.setBounds (r); return; }
            if (kind == toggle) { tick.setBounds (r); return; }
            forgetBtn.setBounds (r.removeFromRight (64)); r.removeFromRight (4);
            learn.setBounds (r.removeFromRight (70)); r.removeFromRight (8);
            name.setBounds (r.removeFromLeft (jmin (300, r.getWidth() / 2)));
            what.setBounds (r);
        }
        void paint (Graphics& g) override
        {
            if (kind != job) return;
            g.setColour (bgRow);
            g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0, 1), 3.0f);
        }
        String taught() const
        {
            if (describe) return describe();
            const int cc = get ? get() : 0;
            return cc > 0 ? "CC " + String (cc) + where : String();
        }
        void show()
        {
            if (kind == toggle) { tick.setToggleState (getTick ? getTick() : false, dontSendNotification); return; }
            if (kind != job) return;
            const auto t = taught();
            what.setText (learning ? String ("Waiting: press it now...") : t.isNotEmpty() ? t : String ("not taught"), dontSendNotification);
            what.setColour (Label::textColourId, learning ? Colours::orange : textDim);
            learn.setButtonText (learning ? "Cancel" : "Learn");
            forgetBtn.setEnabled (t.isNotEmpty());
        }
    };

    Row& add (const String& job)
    {
        auto r = std::make_unique<Row>();
        r->name.setText (job, dontSendNotification);
        list.addAndMakeVisible (*r);
        rows.push_back (std::move (r));
        return *rows.back();
    }
    void addHeader (const String& text)
    {
        auto& r = add (text);
        r.kind = Row::header;
        caption (r.name, text);
    }
    void addNote (const String& text)
    {
        auto& r = add (text);
        r.kind = Row::note;
        r.name.setColour (Label::textColourId, textDim);
    }

    void build()
    {
        rows.clear();
        auto rig = [this] (const String& job, std::function<int()> get, std::function<void()> arm, std::function<void()> forget, std::function<bool()> armed)
        {
            auto& r = add (job);
            r.where = ", on any keyboard";
            r.get = std::move (get); r.arm = std::move (arm); r.forget = std::move (forget); r.armed = std::move (armed);
            wire (r);
        };
        auto& e = engine;
        addHeader ("SET LIST");
        rig ("Next song",     [&e] { return e.getNextSongCC(); }, [&e] { e.armSongLearn (true); },  [&e] { e.setNextSongCC (0); }, [&e] { return e.isSongLearnArmed (true); });
        rig ("Previous song", [&e] { return e.getPrevSongCC(); }, [&e] { e.armSongLearn (false); }, [&e] { e.setPrevSongCC (0); }, [&e] { return e.isSongLearnArmed (false); });
        addHeader ("BACKING TRACK");
        rig ("Start / stop the track", [&e] { return e.getTransportCC(); }, [&e] { e.armTransportLearn(); }, [&e] { e.setTransportCC (0); }, [&e] { return e.isTransportLearnArmed(); });
        rig ("Next section, next scene, or release a vamp", [&e] { return e.getSectionCC(); }, [&e] { e.armSectionLearn(); }, [&e] { e.setSectionCC (0); }, [&e] { return e.isSectionLearnArmed(); });
        addHeader ("TEMPO");
        rig ("Tap tempo", [&e] { return e.getTapTempoCC(); }, [&e] { e.armTapTempoLearn (true); }, [&e] { e.setTapTempoCC (0); }, [&e] { return e.isTapTempoLearnArmed(); });
        addHeader ("LOOP A PASSAGE  (on the keyboard the pedal is pressed on)");
        rig ("Record, play, stop", [&e] { return e.getLooperCC(); }, [&e] { e.armLooperLearn (false); }, [&e] { e.setLooperCC (0); }, [&e] { return e.isLooperLearnArmed (false); });
        rig ("Clear the loop",     [&e] { return e.getLooperClearCC(); }, [&e] { e.armLooperLearn (true); }, [&e] { e.setLooperClearCC (0); }, [&e] { return e.isLooperLearnArmed (true); });
        const auto& outputs = engine.getSetup().outputs;
        if (! outputs.empty())
        {
            addHeader ("OUTPUT VOLUME  (a pedal or knob)");
            for (int o = 0; o < (int) outputs.size(); ++o)
                rig (outputs[(size_t) o].name + " volume",
                     [&e, o] { const auto& outs = e.getSetup().outputs; return o < (int) outs.size() ? outs[(size_t) o].volumeCC : 0; },
                     [&e, o] { e.armOutputVolumeLearn (o); }, [&e, o] { e.setOutputVolumeCC (o, 0); }, [&e] { return e.isOutputVolumeLearnArmed(); });
        }
        // Each keyboard's own jobs.
        const auto& inputs = engine.getSetup().inputs;
        for (int i = 0; i < (int) inputs.size(); ++i)
        {
            if (inputs[(size_t) i].kind != InputDef::Kind::midi) continue;
            addHeader ("ON " + inputs[(size_t) i].name.toUpperCase() + "  (" + (inputs[(size_t) i].midiDeviceName.isNotEmpty() ? inputs[(size_t) i].midiDeviceName : String ("no MIDI device yet")) + ")");

            /* Its eight knobs. Taught here only for a plain MIDI keyboard whose knobs send controllers: a keyboard
               StageRig talks to needs no teaching, and a Komplete Kontrol MK1 left as plain MIDI sends nothing from
               them at all. Either way the rows go, and one line says why. */
            String silent, byItself;
            for (const auto& k : hub.getKeyboards())
                if (std::find (k.inputs.begin(), k.inputs.end(), i) != k.inputs.end())
                {
                    if (k.knobsSilent && k.plain) silent = k.name;
                    else if (! k.plain && k.knobsByThemselves) byItself = k.name;
                }
            bool anyTaught = false;
            for (int cc : inputs[(size_t) i].controls.knobCC) if (cc > 0) anyTaught = true;
            if (byItself.isNotEmpty())    addNote ("Its knobs work by themselves: what they turn is each instrument's Knobs window.");
            else if (silent.isNotEmpty()) addNote ("Its knobs send no MIDI while it is plain MIDI: untick Plain MIDI in Options > Your keyboards and they work by themselves.");
            if ((silent.isEmpty() && byItself.isEmpty()) || anyTaught)
            {
                for (int k = 0; k < SurfaceControls::numKnobs; ++k)
                {
                    auto& r = add ("Knob " + String (k + 1));
                    r.describe = [&e, i, k]
                    {
                        const auto& ins = e.getSetup().inputs;
                        const int cc = i < (int) ins.size() ? ins[(size_t) i].controls.knobCC[(size_t) k] : 0;
                        return cc > 0 ? "CC " + String (cc) : String();
                    };
                    r.arm = [this, i, k, raw = &r]
                    {
                        hub.learn (i, k, [sp = SafePointer<PedalsView> (this), raw] (const String&)
                        {
                            if (sp == nullptr) return;
                            raw->learning = false;
                            sp->refresh();
                            if (sp->changed) sp->changed();
                        });
                    };
                    r.armed = [this] { return hub.isLearning(); };
                    r.forget = [&e, i, k]
                    {
                        auto c = e.getSetup().inputs[(size_t) i].controls;
                        c.knobCC[(size_t) k] = 0;
                        e.setInputControls (i, c);
                    };
                    wire (r);
                }
                auto& t = add ("The knobs are endless: they send steps around 64, not where they are");
                t.kind = Row::toggle;
                t.tick.setButtonText (t.name.getText());
                t.getTick = [&e, i] { const auto& ins = e.getSetup().inputs; return i < (int) ins.size() && ins[(size_t) i].controls.knobsRelative; };
                t.setTick = [this, &e, i] (bool on)
                {
                    auto c = e.getSetup().inputs[(size_t) i].controls;
                    c.knobsRelative = on;
                    e.setInputControls (i, c);
                    if (changed) changed();
                };
            }
            for (int a = 0; a < SurfaceControls::numActions; ++a)
            {
                auto& r = add (SurfaceControls::actionName (a));
                r.describe = [&e, i, a]
                {
                    const auto& ins = e.getSetup().inputs;
                    if (i >= (int) ins.size()) return String();
                    const auto& id = ins[(size_t) i].controls.buttons[(size_t) a];
                    return id.isNotEmpty() ? SurfaceHub::describeControl (id) : String();
                };
                r.arm = [this, i, a, raw = &r]
                {
                    hub.learn (i, 100 + a, [sp = SafePointer<PedalsView> (this), raw] (const String&)
                    {
                        if (sp == nullptr) return;
                        raw->learning = false;
                        sp->refresh();
                        if (sp->changed) sp->changed();
                    });
                };
                r.armed = [this] { return hub.isLearning(); };
                r.forget = [&e, i, a]
                {
                    auto c = e.getSetup().inputs[(size_t) i].controls;
                    c.buttons[(size_t) a].clear();
                    e.setInputControls (i, c);
                };
                wire (r);
            }
        }
        refresh();
        layoutRows();
    }

    void wire (Row& r)
    {
        r.learn.onClick = [this, raw = &r]
        {
            // Click again to stop waiting. One learn at a time: another row waiting is let go.
            const bool stop = raw->learning;
            cancelAll();
            if (! stop) { raw->learning = true; raw->arm(); }
            refresh();
        };
        r.forgetBtn.onClick = [this, raw = &r]
        {
            raw->forget();
            raw->learning = false;
            refresh();
            if (changed) changed();
        };
    }

    void refresh()
    {
        for (auto& r : rows) r->show();
    }

    void layoutRows()
    {
        const int w = jmax (300, view.getWidth() - view.getScrollBarThickness());
        int y = 0;
        for (auto& r : rows)
        {
            const int h = r->kind == Row::header ? 26 : r->kind == Row::note ? 22 : 30;
            r->setBounds (0, y, w, h);
            y += h;
        }
        list.setSize (w, jmax (30, y));
    }

    void timerCallback() override
    {
        // A learn is over when the engine is no longer waiting (its controller then shows), or the hub's finished.
        bool any = false;
        for (auto& r : rows)
            if (r->learning && r->armed && ! r->armed()) { r->learning = false; any = true; }
        if (any && changed) changed();
        // Outputs and inputs come and go: the list follows.
        String sig;
        for (const auto& o : engine.getSetup().outputs) sig << o.name << "|";
        for (const auto& in : engine.getSetup().inputs) sig << in.name << "/" << in.midiDeviceName << "|";
        for (const auto& k : hub.getKeyboards()) sig << k.name << (int) k.plain << (int) k.knobsByThemselves << (int) k.knobsSilent << "|";
        if (sig != shape) { shape = sig; build(); return; }
        refresh();
    }

    Engine& engine;
    SurfaceHub& hub;
    std::function<void()> changed;
    Label explain;
    Viewport view;
    Component list;
    std::vector<std::unique_ptr<Row>> rows;
    String shape;
};
}

Component* showControlMapEditor (Engine& engine, SurfaceHub& hub, ControlMapLibrary& library, int input, int program, int slot, std::function<void()> changed)
{
    const auto& inputs = engine.getSetup().inputs;
    if (input < 0 || input >= (int) inputs.size() || program < 0 || program >= InputDef::numPrograms) return nullptr;
    const auto& prog = inputs[(size_t) input].programs[(size_t) program];
    if (slot < 0 || slot >= (int) prog.slots.size() || ! prog.slots[(size_t) slot].isPlugin()) return nullptr;
    return launch (new ControlMapEditor (engine, hub, library, input, program, slot, std::move (changed)),
            "Knobs: " + prog.slots[(size_t) slot].plugin.name, 900, 502);
}

Component* showControllers (Engine& engine, SurfaceHub& hub, std::function<void()> changed)
{
    return launch (new ControllersView (engine, hub, std::move (changed)), "Your keyboards", 860, 340);
}

Component* createPedalsView (Engine& engine, SurfaceHub& hub, std::function<void()> changed)
{
    return new PedalsView (engine, hub, std::move (changed));
}

} // namespace perf
