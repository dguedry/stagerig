#pragma once

#include "Engine.h"
#include "LevelMeter.h"
#include "Surfaces.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace perf
{

/** The performance view: what is playing, and a front panel for it.

    Meant to be read and reached at playing distance, from a laptop on a
    stand. For each input, the program number as sent from the keyboard and
    the program's name, as large as the space allows -- when you pick programs
    by number on the keyboard you get no confirmation of what loaded; this is
    that confirmation. Under it, the instrument's level and pan, then the
    page of eight knobs the keyboard's own knobs are on -- the same names and
    values the keyboard's screens show, from the one layout (SurfaceHub), so
    the laptop, the keyboard and the tablet agree. The instrument buttons and
    the page arrows move the keyboard's knobs too. A program still loading is
    marked, so a silent keyboard is explained. */
class StagePanel : public juce::Component,
                   private juce::Timer
{
public:
    static constexpr int readoutHeight = 132, panelHeight = 96;

    explicit StagePanel (Engine& e) : engine (e) { startTimerHz (10); }

    /** The hub the knobs are read from and turned through; none while it is not there. */
    void setHub (SurfaceHub* h)                     { hub = h; for (auto& p : panels) p->key.clear(); }

    /** The readout, plus the front panel when any input has one to show. */
    int preferredHeight() const                     { return readoutHeight + (anyPanel ? panelHeight : 0); }
    std::function<void()> onEdited;                 // a knob moved: the setup has changed
    std::function<void()> onHeightChanged;          // the panel appeared or went: lay the window out again
    std::function<void (int input, int program, int slot)> onOpenKnobs;   // right-click on a knob: the instrument's Knobs window
    std::function<void (int input)> onSelectInput;  // a click on an input's readout: the on-screen keys play it (and the editing panes show it)
    std::function<int()> selectedInput;             // which input that is now, for the readout to mark

    /** The front panel grows with the window: in Perform, with the window to
        itself, the knobs are made to be read and reached from a stand. */
    int panelHeightNow() const                      { return getHeight() >= 560 ? 300 : panelHeight; }
    bool isBig() const                              { return panelHeightNow() > panelHeight; }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || e.getPosition().getY() >= readoutArea()) return;
        for (const auto& [i, column] : columns())
            if (column.contains (e.getPosition()) && onSelectInput) { onSelectInput ((int) i); repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        const auto& inputs = engine.getSetup().inputs;
        g.setColour (juce::Colour (0xff101116));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
        for (const auto& [i, column] : columns())
        {
            // The readout takes whatever height the knobs leave: in Perform mode, most of the window.
            auto cell = column.withHeight (readoutArea()).reduced (6, 8);
            const auto& def = inputs[i];
            const int program = def.currentProgram;
            const auto& prog = def.programs[(size_t) program];
            const bool loading = isLoading ((int) i, program);
            // An audio input's program with no effects is a pass-through, not nothing.
            const bool empty = def.isAudio() ? prog.name.isEmpty() && prog.effects.empty() : prog.isEmpty();

            // input name, small and dim: you know which manual is which, it is
            // the program that needs reading. The one the on-screen keys play is marked, and says so.
            const bool chosen = selectedInput && selectedInput() == (int) i;
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.setColour (chosen ? juce::Colour (0xff5aa9ff) : juce::Colour (0xff9aa0ab));
            auto nameRow = cell.removeFromTop (16);
            g.drawText (def.name.toUpperCase() + (chosen ? "   -  THE ON-SCREEN KEYS PLAY THIS ONE" : ""), nameRow, juce::Justification::centredLeft, true);
            if (chosen) g.fillRect (column.withHeight (3).reduced (6, 0));

            // The number's size: as tall as the cell allows, and no wider than two fifths of it, three digits and all.
            const int big = juce::jmin (cell.getHeight() - 4, 190, (int) (cell.getWidth() * 0.22f));
            auto numberArea = cell.removeFromLeft (juce::jmin (cell.getWidth() / 2, (int) (big * 1.95f)));
            g.setFont (juce::FontOptions ((float) big, juce::Font::bold));
            g.setColour (empty ? juce::Colour (0xff4a4d57) : juce::Colour (0xff5aa9ff));
            g.drawText (juce::String (program).paddedLeft ('0', 3), numberArea, juce::Justification::centredLeft, false);

            // An audio input shows what is arriving, under its program name.
            if (def.isAudio())
                perf::LevelMeterPainter::paint (g, cell.removeFromBottom (12).withTrimmedTop (2), engine.getInputLevel ((int) i));

            juce::String name = prog.name.isNotEmpty() ? prog.name : empty ? (def.isAudio() ? "straight through" : "empty program") : "(unnamed)";
            if (loading) name += "  ...loading";
            g.setFont (juce::FontOptions ((float) juce::jmin (big * 2 / 3, cell.getHeight() - 4), juce::Font::bold));
            g.setColour (empty ? juce::Colour (0xff6b7079) : (loading ? juce::Colour (0xffd8a657) : juce::Colours::white));
            g.drawFittedText (name, cell, juce::Justification::centredLeft, big > 60 ? 3 : 2, 0.6f);
        }
    }

    void resized() override { layoutPanels(); repaint(); }

private:
    //==========================================================================
    /** A rotary knob with its name over it and its value under it. */
    struct Knob : public juce::Component
    {
        struct KnobSlider : public juce::Slider
        {
            std::function<void()> onRightClick;
            void mouseDown (const juce::MouseEvent& e) override
            {
                if (e.mods.isPopupMenu()) { if (onRightClick) onRightClick(); return; }
                juce::Slider::mouseDown (e);
            }
        };
        Knob()
        {
            slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            slider.setRange (0.0, 1.0, 0.0);
            slider.setPopupMenuEnabled (false);
            slider.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xff5aa9ff));
            slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff2c3038));
            slider.setColour (juce::Slider::thumbColourId, juce::Colours::white);
            slider.onValueChange = [this] { if (! silent && onMove) onMove (slider.getValue()); showValue(); };
            slider.onDragStart = [this] { dragging = true; };
            slider.onDragEnd = [this] { dragging = false; };
            addAndMakeVisible (slider);
            addChildComponent (sw);
            sw.setClickingTogglesState (false);
            sw.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2f5f9a));
            sw.onClick = [this] { if (onMove) onMove (sw.getToggleState() ? 0.0 : 1.0); };
            for (auto* l : { &name, &value })
            {
                addAndMakeVisible (*l);
                l->setJustificationType (juce::Justification::centred);
                l->setMinimumHorizontalScale (0.7f);
                l->setInterceptsMouseClicks (false, false);
            }
            name.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            name.setColour (juce::Label::textColourId, juce::Colour (0xff9aa0ab));
            value.setFont (juce::FontOptions (11.0f));
            value.setColour (juce::Label::textColourId, juce::Colours::white);
        }
        void set (double v)                         // from the model or the plugin: never while the hand is on it
        {
            if (dragging) return;
            silent = true;
            slider.setValue (v, juce::dontSendNotification);
            silent = false;
            showValue();
        }
        /** A knob of the keyboard's page: its name and value as the keyboard shows them, and where it is.
            A two-state parameter (a Leslie's slow/fast) is a switch: one click flips it. */
        void show (const juce::String& n, const juce::String& v, float level, bool isSwitch = false)
        {
            name.setText (n.isNotEmpty() ? n : juce::String ("-"), juce::dontSendNotification);
            text = v;
            sw.setVisible (isSwitch);
            slider.setVisible (! isSwitch);
            value.setVisible (! isSwitch);
            if (isSwitch)
            {
                sw.setToggleState (level >= 0.5f, juce::dontSendNotification);
                sw.setButtonText (v.isNotEmpty() ? v : juce::String (level >= 0.5f ? "On" : "Off"));
                return;
            }
            slider.setEnabled (level >= 0.0f);
            slider.setAlpha (level >= 0.0f ? 1.0f : 0.35f);
            set (level >= 0.0f ? level : 0.0);
        }
        void showValue()                            { value.setText (format ? format (slider.getValue()) : text, juce::dontSendNotification); }
        void setBig (bool big)
        {
            name.setFont (juce::FontOptions (big ? 16.0f : 11.0f, juce::Font::bold));
            value.setFont (juce::FontOptions (big ? 15.0f : 11.0f));
        }
        void resized() override
        {
            auto r = getLocalBounds();
            const bool big = r.getHeight() >= 96;
            name.setBounds (r.removeFromTop (big ? 22 : 14));
            value.setBounds (r.removeFromBottom (big ? 20 : 14));
            slider.setBounds (r);
            sw.setBounds (r.withSizeKeepingCentre (juce::jmin (r.getWidth() - 8, big ? 120 : 64), big ? 44 : 24));
        }
        KnobSlider slider;
        juce::TextButton sw;
        juce::Label name, value;
        juce::String text;                          // the value in the keyboard's words, when there is no format
        std::function<void (double)> onMove;
        std::function<juce::String (double)> format;
        bool dragging = false, silent = false;
    };

    /** One input's front panel: which instrument, which page, and the knobs. */
    struct Panel : public juce::Component
    {
        Panel (StagePanel& o, int in) : owner (o), input (in)
        {
            for (auto* b : { &pagePrev, &pageNext })
            {
                addChildComponent (*b);
                b->setTooltip ("The keyboard's knobs go to the previous or next page of this instrument's layout");
            }
            addChildComponent (pageLabel);
            pageLabel.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            pageLabel.setColour (juce::Label::textColourId, juce::Colour (0xff6b7079));
            pageLabel.setJustificationType (juce::Justification::centred);
            pageLabel.setMinimumHorizontalScale (0.6f);
            addChildComponent (slotLabel);
            slotLabel.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            slotLabel.setColour (juce::Label::textColourId, juce::Colour (0xff6b7079));
        }
        void resized() override
        {
            const bool big = getHeight() > 120;
            pageLabel.setFont (juce::FontOptions (big ? 15.0f : 11.0f, juce::Font::bold));
            slotLabel.setFont (juce::FontOptions (big ? 15.0f : 11.0f, juce::Font::bold));
            for (auto& k : knobs) k->setBig (big);
            auto r = getLocalBounds().reduced (6, 2);
            auto top = r.removeFromTop (big ? 30 : 18);
            if (pageLabel.isVisible())
            {
                auto pages = top.removeFromRight (juce::jmin (big ? 340 : 220, top.getWidth() / 2));
                pagePrev.setBounds (pages.removeFromLeft (big ? 36 : 22));
                pageNext.setBounds (pages.removeFromRight (big ? 36 : 22));
                pageLabel.setBounds (pages);
                top.removeFromRight (8);
            }
            const int bw = juce::jmin (big ? 200 : 120, top.getWidth() / juce::jmax (1, (int) slotButtons.size()) - 4);    // each the same width
            for (auto& b : slotButtons) { b->setBounds (top.removeFromLeft (bw)); top.removeFromLeft (4); }
            slotLabel.setBounds (top);
            r.removeFromTop (big ? 6 : 2);
            if (big && knobs.size() > 2)
            {
                /* Made to be read from a stand, with the whole width: level, pan and the
                   page's eight knobs in one row when it holds them, else the eight in two
                   rows of four with level and pan stepping aside. */
                const int minEach = 110;
                if (r.getWidth() >= (int) knobs.size() * minEach)
                {
                    const int each = juce::jmin (220, r.getWidth() / (int) knobs.size());
                    const int x0 = r.getX() + (r.getWidth() - each * (int) knobs.size()) / 2;
                    for (size_t k = 0; k < knobs.size(); ++k)
                    {
                        knobs[k]->setVisible (true);
                        knobs[k]->setBounds (x0 + (int) k * each, r.getY(), each, r.getHeight());
                    }
                    return;
                }
                knobs[0]->setVisible (false);
                knobs[1]->setVisible (false);
                const int rows = 2, perRow = 4;
                const int rowH = r.getHeight() / rows, each = juce::jmin (220, r.getWidth() / perRow);
                const int x0 = r.getX() + (r.getWidth() - each * perRow) / 2;
                for (size_t k = 2; k < knobs.size(); ++k)
                {
                    const int n = (int) k - 2;
                    knobs[k]->setVisible (true);
                    knobs[k]->setBounds (x0 + (n % perRow) * each, r.getY() + (n / perRow) * rowH, each, rowH);
                }
                return;
            }
            // The page's knobs come first; level and pan give way when the width will not hold them all.
            int shown = (int) knobs.size();
            while (shown > 2 && r.getWidth() / shown < 52) --shown;
            const int each = shown == 0 ? 0 : juce::jmin (120, r.getWidth() / shown);
            for (size_t k = 0; k < knobs.size(); ++k)
            {
                const bool fits = (int) k >= (int) knobs.size() - shown;
                knobs[k]->setVisible (fits);
                if (fits) knobs[k]->setBounds (r.removeFromLeft (each));
            }
        }
        StagePanel& owner;
        int input, program = -1, slot = -1;
        juce::String key;
        std::vector<std::unique_ptr<Knob>> knobs;       // level, pan, then the page's eight
        std::vector<std::unique_ptr<juce::TextButton>> slotButtons;
        juce::TextButton pagePrev { "<" }, pageNext { ">" };
        juce::Label pageLabel, slotLabel;
    };

    int readoutArea() const                         { return juce::jmax (readoutHeight, getHeight() - (anyPanel ? panelHeightNow() : 0)); }

    bool isLoading (int input, int program) const
    {
        const auto& prog = engine.getSetup().inputs[(size_t) input].programs[(size_t) program];
        for (int slot = 0; slot < (int) prog.slots.size(); ++slot) if (engine.isPluginLoading (input, program, slot)) return true;
        for (int e = 0; e < (int) prog.effects.size(); ++e) if (engine.isPluginLoading (input, program, -1, e)) return true;
        return false;
    }

    std::vector<std::pair<size_t, juce::Rectangle<int>>> columns() const
    {
        const auto& inputs = engine.getSetup().inputs;
        std::vector<std::pair<size_t, juce::Rectangle<int>>> out;
        std::vector<size_t> shown;
        for (size_t i = 0; i < inputs.size(); ++i) if (! inputs[i].isTrack()) shown.push_back (i);   // the track's program follows the song
        if (shown.empty()) return out;
        auto r = getLocalBounds().reduced (10, 0);
        const int each = r.getWidth() / (int) shown.size();
        for (size_t i : shown) out.push_back ({ i, r.removeFromLeft (each) });
        return out;
    }

    static juce::String dbText (double v)
    {
        const double db = -60.0 + v * 72.0;
        return db <= -59.9 ? juce::String ("off") : (db >= 0.0 ? "+" : "") + juce::String (db, 1) + " dB";
    }
    static juce::String panText (double v)
    {
        const int p = juce::roundToInt ((v * 2.0 - 1.0) * 50.0);
        return p == 0 ? juce::String ("C") : p < 0 ? "L " + juce::String (-p) : "R " + juce::String (p);
    }

    /** The input whose panel is shown: the one chosen (the on-screen keys play it), else the first with a panel. */
    int shownInput() const
    {
        const auto& inputs = engine.getSetup().inputs;
        auto hasPanel = [&] (int i) { return ! inputs[(size_t) i].programs[(size_t) inputs[(size_t) i].currentProgram].slots.empty(); };
        const int chosen = selectedInput ? selectedInput() : -1;
        for (const auto& [i, column] : columns()) if ((int) i == chosen && hasPanel ((int) i)) return chosen;
        for (const auto& [i, column] : columns()) if (hasPanel ((int) i)) return (int) i;
        return -1;
    }

    void layoutPanels()
    {
        // One front panel, the chosen input's, across the whole width: click another input's readout and it is that one's.
        const int shown = shownInput();
        auto area = getLocalBounds().reduced (10, 0).withTop (readoutArea()).withHeight (panelHeightNow());
        for (auto& p : panels)
        {
            const bool placed = p->input == shown && anyPanel;
            if (placed) p->setBounds (area);
            p->setVisible (placed);
        }
        for (auto& p : panels) p->resized();
    }

    /** Builds or refreshes the panel of one input: the instrument the keyboard's knobs are on, and their page. */
    void refreshPanel (Panel& p)
    {
        const auto& inputs = engine.getSetup().inputs;
        if (p.input < 0 || p.input >= (int) inputs.size()) return;
        const auto& def = inputs[(size_t) p.input];
        const int program = def.currentProgram;
        const auto& prog = def.programs[(size_t) program];
        if (prog.slots.empty() || hub == nullptr)
        {
            p.key = {}; p.knobs.clear(); p.slotButtons.clear(); p.program = program; p.slot = -1;
            p.slotLabel.setVisible (false); p.pageLabel.setVisible (false); p.pagePrev.setVisible (false); p.pageNext.setVisible (false);
            return;
        }
        // The instrument the knobs are on, as the hub has it; a program with no plugin shows its first slot's level and pan.
        int slot = hub->getFocusSlot (p.input);
        if (slot < 0) slot = 0;
        slot = juce::jlimit (0, (int) prog.slots.size() - 1, slot);
        const auto& sd = prog.slots[(size_t) slot];
        const auto view = hub->viewFor (p.input);
        const bool hasPage = sd.isPlugin() && view.knobLevels != SurfaceView().knobLevels;

        juce::String key;
        key << program << "|" << slot << "|" << (int) prog.slots.size() << "|" << (int) hasPage << "|" << view.pluginNames.joinIntoString ("/") << "|" << view.pageCount;
        if (key != p.key)
        {
            p.key = key;
            p.program = program;
            p.slot = slot;
            p.knobs.clear(); p.slotButtons.clear();
            p.slotLabel.setVisible (false);
            // Which instrument, when the program has more than one: the keyboard's knobs follow the choice.
            if (prog.slots.size() > 1)
                for (int s = 0; s < (int) prog.slots.size(); ++s)
                {
                    auto b = std::make_unique<juce::TextButton> (slotName (prog.slots[(size_t) s]));
                    b->setClickingTogglesState (false);
                    b->setToggleState (s == slot, juce::dontSendNotification);
                    b->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2f5f9a));
                    b->setTooltip (prog.slots[(size_t) s].isPlugin() ? "Give the knobs -- here and on the keyboard -- to this instrument" : "This instrument has no knobs to give");
                    b->setEnabled (prog.slots[(size_t) s].isPlugin());
                    /* Later, not now: rebuilding the panel deletes this button,
                       and with it the very function that is running. */
                    b->onClick = [this, in = p.input, s]
                    {
                        juce::MessageManager::callAsync ([self = juce::Component::SafePointer<StagePanel> (this), in, s]
                        {
                            if (self == nullptr || self->hub == nullptr) return;
                            self->hub->setFocusSlot (in, s);
                            for (auto& q : self->panels) if (q->input == in) { q->key.clear(); self->refreshPanel (*q); }
                            self->layoutPanels();
                        });
                    };
                    p.addAndMakeVisible (*b);
                    p.slotButtons.push_back (std::move (b));
                }
            else
            {
                p.addAndMakeVisible (p.slotLabel);
                p.slotLabel.setText (slotName (sd).toUpperCase(), juce::dontSendNotification);
            }
            const int in = p.input, sl = slot;
            auto level = std::make_unique<Knob>();
            level->name.setText ("Level", juce::dontSendNotification);
            level->format = dbText;
            level->slider.setDoubleClickReturnValue (true, 60.0 / 72.0);
            level->onMove = [this, in, program, sl] (double v) { engine.setSlotGainDb (in, program, sl, (float) (-60.0 + v * 72.0)); if (onEdited) onEdited(); };
            p.knobs.push_back (std::move (level));
            auto pan = std::make_unique<Knob>();
            pan->name.setText ("Pan", juce::dontSendNotification);
            pan->format = panText;
            pan->slider.setDoubleClickReturnValue (true, 0.5);
            pan->onMove = [this, in, program, sl] (double v) { engine.setSlotPan (in, program, sl, (float) (v * 2.0 - 1.0)); if (onEdited) onEdited(); };
            p.knobs.push_back (std::move (pan));
            if (hasPage)
                for (int k = 0; k < SurfaceView::numKnobs; ++k)
                {
                    auto knob = std::make_unique<Knob>();
                    knob->onMove = [this, in, k] (double v) { if (hub != nullptr) hub->setKnob (in, k, (float) v); if (onEdited) onEdited(); };
                    knob->slider.onRightClick = [this, in, program, sl] { if (onOpenKnobs) onOpenKnobs (in, program, sl); };
                    knob->slider.setTooltip ("Knob " + juce::String (k + 1) + " on the keyboard. Right-click to change what the knobs turn (the Knobs window)");
                    p.knobs.push_back (std::move (knob));
                }
            for (auto& k : p.knobs) p.addAndMakeVisible (*k);
            for (auto* b : { &p.pagePrev, &p.pageNext }) b->setVisible (hasPage && view.pageCount > 1);
            p.pageLabel.setVisible (hasPage);
            p.pagePrev.onClick = [this, in] { if (hub != nullptr) hub->doAction (in, SurfaceControls::pageDown); };
            p.pageNext.onClick = [this, in] { if (hub != nullptr) hub->doAction (in, SurfaceControls::pageUp); };
            p.resized();
        }
        // The values, from the model and the hub's view: never a round trip here.
        if (p.knobs.size() >= 2)
        {
            p.knobs[0]->set ((sd.gainDb + 60.0) / 72.0);
            p.knobs[1]->set ((sd.pan + 1.0) / 2.0);
        }
        if (hasPage)
        {
            for (int k = 0; k < SurfaceView::numKnobs && (size_t) k + 2 < p.knobs.size(); ++k)
                p.knobs[(size_t) k + 2]->show (view.knobNames[(size_t) k], view.knobValues[(size_t) k], view.knobLevels[(size_t) k], view.knobSwitches[(size_t) k]);
            p.pageLabel.setText (view.pageCount > 1 ? "Page " + juce::String (view.page + 1) + "/" + juce::String (view.pageCount) + ": " + view.pageName : view.pageName,
                                 juce::dontSendNotification);
            for (auto* b : { &p.pagePrev, &p.pageNext }) b->setVisible (view.pageCount > 1);
        }
    }

    static juce::String slotName (const SlotDef& sd)
    {
        switch (sd.kind)
        {
            case SlotDef::Kind::plugin:   return sd.plugin.name;
            case SlotDef::Kind::gm:       return "General MIDI";
            case SlotDef::Kind::external: return "MIDI device";
            case SlotDef::Kind::input:    return sd.targetInputName;
        }
        return {};
    }

    void timerCallback() override
    {
        const auto& inputs = engine.getSetup().inputs;
        // Repaint the readout only when something visible changed: this runs all night.
        juce::String now;
        now << (selectedInput ? selectedInput() : -1) << ";";    // the mark on the input the on-screen keys play
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            const int p = inputs[i].currentProgram;
            now << inputs[i].name << "|" << p << "|" << inputs[i].programs[(size_t) p].name << "|" << (isLoading ((int) i, p) ? "L" : "-");
            if (inputs[i].isAudio())
            {
                const auto l = engine.getInputLevel ((int) i);
                now << "|" << (int) (LevelMeterPainter::position (l.left) * 100) << "," << (int) (LevelMeterPainter::position (l.right) * 100) << (l.clipped ? "!" : "");
            }
            now << ";";
        }
        if (now != shown) { shown = now; repaint(); layoutPanels(); }

        // The front panels: one per input shown, made as inputs come and go.
        const auto cols = columns();
        bool changed = false;
        for (const auto& [i, column] : cols)
        {
            bool have = false;
            for (auto& p : panels) if (p->input == (int) i) have = true;
            if (! have) { panels.push_back (std::make_unique<Panel> (*this, (int) i)); addChildComponent (*panels.back()); changed = true; }
        }
        for (size_t k = 0; k < panels.size();)
        {
            bool still = false;
            for (const auto& [i, column] : cols) if ((int) i == panels[k]->input) still = true;
            if (still) ++k; else { panels.erase (panels.begin() + (long) k); changed = true; }
        }
        bool any = false;
        for (const auto& [i, column] : cols) if (! inputs[i].programs[(size_t) inputs[i].currentProgram].slots.empty()) any = true;
        if (any != anyPanel) { anyPanel = any; changed = true; if (onHeightChanged) onHeightChanged(); }
        if (changed) layoutPanels();
        if (anyPanel) for (auto& p : panels) refreshPanel (*p);
    }

    Engine& engine;
    SurfaceHub* hub = nullptr;
    juce::String shown;
    std::vector<std::unique_ptr<Panel>> panels;
    bool anyPanel = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StagePanel)
};

} // namespace perf
