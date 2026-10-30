#pragma once

#include "Engine.h"
#include "LevelMeter.h"
#include "PanelParams.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace perf
{

/** The performance view: what is playing, and a front panel for it.

    Meant to be read and reached at playing distance, from a laptop on a
    stand. For each input, the program number as sent from the keyboard and
    the program's name, as large as the space allows -- when you pick programs
    by number on the keyboard you get no confirmation of what loaded; this is
    that confirmation. Under it, a row of knobs for the program's instrument:
    its level and pan, then the plugin parameters a player reaches for
    without opening a window -- cutoff, resonance, attack, release, reverb --
    picked by name, or chosen by right-clicking a knob. A program still
    loading is marked, so a silent keyboard is explained. */
class StagePanel : public juce::Component,
                   private juce::Timer
{
public:
    static constexpr int readoutHeight = 132, panelHeight = 96;

    explicit StagePanel (Engine& e) : engine (e) { startTimerHz (10); }

    /** The readout, plus the front panel when any input has one to show. */
    int preferredHeight() const                     { return readoutHeight + (anyPanel ? panelHeight : 0); }
    std::function<void()> onEdited;                 // a knob moved: the setup has changed
    std::function<void()> onHeightChanged;          // the panel appeared or went: lay the window out again

    /** The parameters shown when nobody chose: see PanelParams.h. */
    static std::vector<int> pickPanelParams (const ParamInfoList& params, int max) { return perf::pickPanelParams (params, max); }

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
            bool loading = false;
            for (int slot = 0; slot < (int) prog.slots.size() && ! loading; ++slot)
                loading = engine.isPluginLoading ((int) i, program, slot);
            for (int e = 0; e < (int) prog.effects.size() && ! loading; ++e)
                loading = engine.isPluginLoading ((int) i, program, -1, e);
            // An audio input's program with no effects is a pass-through, not nothing.
            const bool empty = def.isAudio() ? prog.name.isEmpty() && prog.effects.empty() : prog.isEmpty();

            // input name, small and dim: you know which manual is which, it is
            // the program that needs reading
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.setColour (juce::Colour (0xff9aa0ab));
            g.drawText (def.name.toUpperCase(), cell.removeFromTop (16), juce::Justification::centredLeft, true);

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
        void showValue()                            { value.setText (format ? format (slider.getValue()) : juce::String (slider.getValue(), 2), juce::dontSendNotification); }
        void resized() override
        {
            auto r = getLocalBounds();
            name.setBounds (r.removeFromTop (14));
            value.setBounds (r.removeFromBottom (14));
            slider.setBounds (r);
        }
        KnobSlider slider;
        juce::Label name, value;
        std::function<void (double)> onMove;
        std::function<juce::String (double)> format;
        bool dragging = false, silent = false;
    };

    /** One input's front panel: which instrument, and its knobs. */
    struct Panel : public juce::Component
    {
        Panel (StagePanel& o, int in) : owner (o), input (in) {}
        void resized() override
        {
            auto r = getLocalBounds().reduced (6, 2);
            auto top = r.removeFromTop (18);
            for (auto& b : slotButtons) { b->setBounds (top.removeFromLeft (juce::jmin (120, top.getWidth() / juce::jmax (1, (int) slotButtons.size())))); top.removeFromLeft (4); }
            if (slotButtons.empty()) slotLabel.setBounds (top);
            r.removeFromTop (2);
            const int each = knobs.empty() ? 0 : juce::jmin (120, r.getWidth() / (int) knobs.size());
            for (auto& k : knobs) k->setBounds (r.removeFromLeft (each));
        }
        StagePanel& owner;
        int input, program = -1, slot = 0;
        RemotePlugin* plugin = nullptr;
        juce::String key;
        std::vector<int> params;                   // the plugin parameter each knob after level and pan shows
        std::vector<std::unique_ptr<Knob>> knobs;
        std::vector<std::unique_ptr<juce::TextButton>> slotButtons;
        juce::Label slotLabel;
    };

    int readoutArea() const                         { return juce::jmax (readoutHeight, getHeight() - (anyPanel ? panelHeight : 0)); }

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

    void layoutPanels()
    {
        const auto cols = columns();
        for (auto& p : panels)
        {
            bool placed = false;
            for (const auto& [i, column] : cols)
                if ((int) i == p->input) { p->setBounds (column.withTop (readoutArea()).withHeight (panelHeight)); placed = true; }
            p->setVisible (placed && anyPanel);
        }
        for (auto& p : panels) p->resized();
    }

    /** Builds or refreshes the panel of one input: its instrument's knobs, for what is loaded now. */
    void refreshPanel (Panel& p)
    {
        const auto& inputs = engine.getSetup().inputs;
        if (p.input < 0 || p.input >= (int) inputs.size()) return;
        const auto& def = inputs[(size_t) p.input];
        const int program = def.currentProgram;
        const auto& prog = def.programs[(size_t) program];
        if (prog.slots.empty()) { p.key = {}; p.knobs.clear(); p.slotButtons.clear(); p.params.clear(); p.plugin = nullptr; p.program = program; return; }
        if (p.program != program) p.slot = 0;
        p.program = program;
        p.slot = juce::jlimit (0, (int) prog.slots.size() - 1, p.slot);
        const auto& sd = prog.slots[(size_t) p.slot];
        RemotePlugin* plugin = sd.kind == SlotDef::Kind::plugin && engine.isPluginAlive (p.input, program, p.slot) ? engine.getPlugin (p.input, program, p.slot) : nullptr;
        const int room = juce::jmax (0, (p.getWidth() - 12) / 72 - 2);
        juce::String key;
        key << program << "|" << p.slot << "|" << (int) prog.slots.size() << "|" << juce::String::toHexString ((juce::pointer_sized_int) plugin) << "|" << room << "|";
        for (const auto& id : sd.panelParams) key << id << ",";
        if (key != p.key)
        {
            p.key = key;
            p.plugin = plugin;
            p.knobs.clear(); p.slotButtons.clear(); p.params.clear();
            // Which instrument, when the program has more than one.
            if (prog.slots.size() > 1)
                for (int s = 0; s < (int) prog.slots.size(); ++s)
                {
                    auto b = std::make_unique<juce::TextButton> (slotName (prog.slots[(size_t) s]));
                    b->setClickingTogglesState (false);
                    b->setToggleState (s == p.slot, juce::dontSendNotification);
                    b->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2f5f9a));
                    b->setTooltip ("Show this instrument's knobs");
                    b->onClick = [this, &p, s] { p.slot = s; p.key = {}; refreshPanel (p); layoutPanels(); };
                    p.addAndMakeVisible (*b);
                    p.slotButtons.push_back (std::move (b));
                }
            else
            {
                p.addAndMakeVisible (p.slotLabel);
                p.slotLabel.setFont (juce::FontOptions (11.0f, juce::Font::bold));
                p.slotLabel.setColour (juce::Label::textColourId, juce::Colour (0xff6b7079));
                p.slotLabel.setText (slotName (sd).toUpperCase(), juce::dontSendNotification);
            }
            const int in = p.input, sl = p.slot;
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
            if (plugin != nullptr && room > 0)
            {
                const auto& params = plugin->getParameters();
                if (sd.panelParams.empty()) p.params = pickPanelParams (params, room);
                else for (const auto& id : sd.panelParams) if (const int idx = plugin->findParameterIndex (id); idx >= 0 && (int) p.params.size() < room) p.params.push_back (idx);
                for (size_t k = 0; k < p.params.size(); ++k)
                {
                    const int index = p.params[k];
                    auto knob = std::make_unique<Knob>();
                    knob->name.setText (index < (int) params.size() ? params[(size_t) index].name : juce::String (index), juce::dontSendNotification);
                    knob->format = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
                    knob->onMove = [plugin, index] (double v) { plugin->setParameterValue (index, (float) v, 100); };
                    knob->slider.onRightClick = [this, &p, k] { chooseParam (p, (int) k); };
                    knob->slider.setTooltip ("Right-click to choose which parameter this knob is");
                    p.knobs.push_back (std::move (knob));
                }
            }
            for (auto& k : p.knobs) p.addAndMakeVisible (*k);
            p.resized();
        }
        // The values, from the model and the plugin's own cache: never a round trip here.
        if (p.knobs.size() >= 2)
        {
            p.knobs[0]->set ((sd.gainDb + 60.0) / 72.0);
            p.knobs[1]->set ((sd.pan + 1.0) / 2.0);
        }
        if (p.plugin != nullptr)
            for (size_t k = 0; k < p.params.size() && k + 2 < p.knobs.size(); ++k)
                p.knobs[k + 2]->set (p.plugin->getCachedParameterValue (p.params[k]));
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

    /** Right-click on a knob: which of the plugin's parameters it is. */
    void chooseParam (Panel& p, int knob)
    {
        if (p.plugin == nullptr) return;
        const auto& params = p.plugin->getParameters();
        juce::PopupMenu m;
        m.addItem (1, "Picked by name  (cutoff, resonance, attack, release...)", true, engine.getSetup().inputs[(size_t) p.input].programs[(size_t) p.program].slots[(size_t) p.slot].panelParams.empty());
        m.addSeparator();
        juce::PopupMenu group;
        int inGroup = 0, first = 0;
        for (const auto& prm : params)
        {
            if (prm.midiController >= 0) continue;
            if (inGroup == 0) first = prm.index;
            group.addItem (1000 + prm.index, prm.name, true, knob < (int) p.params.size() && p.params[(size_t) knob] == prm.index);
            if (++inGroup == 24) { m.addSubMenu (params[(size_t) first].name + " ...", group); group = {}; inGroup = 0; }
        }
        if (inGroup > 0) m.addSubMenu (params[(size_t) first].name + " ...", group);
        const int in = p.input, program = p.program, sl = p.slot;
        juce::Component::SafePointer<StagePanel> self (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (p.knobs[(size_t) knob + 2].get()), [self, in, program, sl, knob] (int r)
        {
            if (self == nullptr || r == 0) return;
            Panel* panel = nullptr;
            for (auto& q : self->panels) if (q->input == in) panel = q.get();
            if (panel == nullptr || panel->plugin == nullptr || panel->program != program || panel->slot != sl) return;
            std::vector<juce::String> ids;
            if (r >= 1000)
            {
                const auto& params = panel->plugin->getParameters();
                auto chosen = panel->params;                                   // the automatic pick, made explicit
                if (knob < (int) chosen.size()) chosen[(size_t) knob] = r - 1000;
                for (int idx : chosen) if (idx >= 0 && idx < (int) params.size()) ids.push_back (params[(size_t) idx].id);
            }
            self->engine.setSlotPanelParams (in, program, sl, ids);
            if (self->onEdited) self->onEdited();
        });
    }

    void timerCallback() override
    {
        const auto& inputs = engine.getSetup().inputs;
        // Repaint the readout only when something visible changed: this runs all night.
        juce::String now;
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            const int p = inputs[i].currentProgram;
            bool loading = false;
            const auto& prog = inputs[i].programs[(size_t) p];
            for (int slot = 0; slot < (int) prog.slots.size() && ! loading; ++slot)
                loading = engine.isPluginLoading ((int) i, p, slot);
            now << inputs[i].name << "|" << p << "|" << prog.name << "|" << (loading ? "L" : "-");
            if (inputs[i].isAudio())
            {
                const auto l = engine.getInputLevel ((int) i);
                now << "|" << (int) (LevelMeterPainter::position (l.left) * 100) << "," << (int) (LevelMeterPainter::position (l.right) * 100) << (l.clipped ? "!" : "");
            }
            now << ";";
        }
        if (now != shown) { shown = now; repaint(); }

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
    juce::String shown;
    std::vector<std::unique_ptr<Panel>> panels;
    bool anyPanel = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StagePanel)
};

} // namespace perf
