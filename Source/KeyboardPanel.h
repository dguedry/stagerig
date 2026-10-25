#pragma once

#include "Engine.h"
#include <juce_audio_utils/juce_audio_utils.h>

namespace perf
{

/**
    Draws a pitch or mod wheel as the thing it is: a cylinder turning in a
    slot, seen edge-on.

    A plain vertical slider is readable but it is not what anyone looks for on
    a keyboard. The parts that sell it are the curvature -- a light gradient
    across the width, darkest at the edges where the cylinder turns away -- and
    the ridges, which are what you actually see moving when a real wheel moves.

    The ridges are spaced by the cosine of their position around the cylinder,
    so they crowd together towards the top and bottom edges the way they do on
    a real one, rather than being evenly spaced like a ladder.
*/
class WheelLookAndFeel : public juce::LookAndFeel_V4
{
public:
    /** `springsToCentre` draws the detent mark a pitch wheel rests against. */
    explicit WheelLookAndFeel (bool springsToCentreIn) : springsToCentre (springsToCentreIn) {}

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float, float,
                           juce::Slider::SliderStyle, juce::Slider& slider) override
    {
        const juce::Rectangle<float> full ((float) x, (float) y, (float) width, (float) height);

        /* The wheel itself is narrower than the space it is given: a real one
           sits down in a slot with the panel visible either side. */
        const float wheelWidth = juce::jmin (full.getWidth() - 6.0f, 18.0f);
        auto wheel = full.withSizeKeepingCentre (wheelWidth, full.getHeight() - 4.0f);
        const float radius = wheelWidth * 0.5f;
        /* A near-square cap, not a semicircle: the end of a cylinder seen
           edge-on is almost flat, and a full round makes it read as a pill. */
        const float corner = juce::jmin (radius, 6.0f);

        // The slot the wheel turns in, cut into the panel.
        auto slot = wheel.expanded (3.0f, 2.0f);
        const float slotCorner = juce::jmin (radius + 2.0f, 8.0f);
        g.setColour (juce::Colour (0xff0b0c10));
        g.fillRoundedRectangle (slot, slotCorner);
        g.setColour (juce::Colour (0xff2a2d36));
        g.drawRoundedRectangle (slot.reduced (0.5f), slotCorner, 1.0f);

        /* Across the width: light down one side, dark at both edges. This is
           the whole illusion -- without it the wheel reads as a flat strip. */
        juce::ColourGradient across (juce::Colour (0xff202329), wheel.getX(), 0.0f,
                                     juce::Colour (0xff17191f), wheel.getRight(), 0.0f, false);
        across.addColour (0.30, juce::Colour (0xff6b707c));
        across.addColour (0.46, juce::Colour (0xff585d68));
        across.addColour (0.72, juce::Colour (0xff33373f));
        g.setGradientFill (across);
        g.fillRoundedRectangle (wheel, corner);

        /* Ridges. Spacing is the giveaway: evenly spaced lines look like a
           ladder, so these are placed by their angle around the cylinder and
           crowd together towards the top and bottom where the surface turns
           away. They roll with the value, which is what sells the movement. */
        const float travel    = juce::jmax (1.0f, wheel.getHeight());
        const float thumbPos  = juce::jlimit (wheel.getY(), wheel.getBottom(), (float) sliderPos);
        const float rollPhase = (thumbPos - wheel.getY()) / travel;

        const int ridgeCount = 22;
        for (int i = 0; i < ridgeCount; ++i)
        {
            const float turn  = std::fmod ((float) i / (float) ridgeCount + rollPhase, 1.0f);
            const float angle = turn * juce::MathConstants<float>::pi;

            // Crowded at the ends, spread across the middle.
            const float ny = (1.0f - std::cos (angle)) * 0.5f;
            const float ry = wheel.getY() + ny * wheel.getHeight();

            // Ridges facing us catch the light; those at the edge are dim.
            const float facing = std::sin (angle);
            if (facing < 0.05f) continue;

            const float inset = 2.0f + (1.0f - facing) * radius * 0.5f;
            const float x1 = wheel.getX() + inset, x2 = wheel.getRight() - inset;
            if (x2 <= x1) continue;

            g.setColour (juce::Colours::black.withAlpha (0.55f * facing));
            g.drawLine (x1, ry, x2, ry, 1.0f);
            g.setColour (juce::Colours::white.withAlpha (0.16f * facing));
            g.drawLine (x1, ry + 1.0f, x2, ry + 1.0f, 1.0f);
        }

        /* The ends fall away into shadow, so the wheel reads as round rather
           than as a flat-topped cylinder. Drawn over the ridges so they fade
           out at the top and bottom as they turn away. */
        const float fade = juce::jmin (wheel.getHeight() * 0.3f, 26.0f);
        juce::ColourGradient top (juce::Colours::black.withAlpha (0.8f), 0.0f, wheel.getY(),
                                  juce::Colours::transparentBlack, 0.0f, wheel.getY() + fade, false);
        g.setGradientFill (top);
        g.fillRoundedRectangle (wheel, corner);
        juce::ColourGradient bottom (juce::Colours::black.withAlpha (0.8f), 0.0f, wheel.getBottom(),
                                     juce::Colours::transparentBlack, 0.0f, wheel.getBottom() - fade, false);
        g.setGradientFill (bottom);
        g.fillRoundedRectangle (wheel, corner);

        /* A pitch wheel rests at centre, so mark the detent it springs back
           to. A mod wheel has no such thing and gets no mark. */
        if (springsToCentre)
        {
            g.setColour (juce::Colours::white.withAlpha (0.16f));
            g.drawLine (wheel.getX() + 3.0f, wheel.getCentreY(), wheel.getRight() - 3.0f, wheel.getCentreY(), 1.0f);
        }

        /* The grip: the moulded thumb rest, standing proud of the cylinder.
           It is what the eye tracks, so it is shaded like a small cylinder of
           its own rather than filled flat. */
        const float gripHeight = 13.0f;
        juce::Rectangle<float> grip (wheel.getX() - 0.5f,
                                     juce::jlimit (wheel.getY(), wheel.getBottom() - gripHeight,
                                                   thumbPos - gripHeight * 0.5f),
                                     wheel.getWidth() + 1.0f, gripHeight);

        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (grip.translated (0.0f, 1.5f), 3.5f);

        juce::ColourGradient gripFill (juce::Colour (0xff2e323a), grip.getX(), 0.0f,
                                       juce::Colour (0xff23262c), grip.getRight(), 0.0f, false);
        gripFill.addColour (0.30, juce::Colour (0xffb8bec9));
        gripFill.addColour (0.46, juce::Colour (0xff868c98));
        gripFill.addColour (0.70, juce::Colour (0xff4a4f59));
        g.setGradientFill (gripFill);
        g.fillRoundedRectangle (grip, 3.5f);

        // Lip top and bottom, so it reads as a moulding and not a painted band.
        g.setColour (juce::Colours::white.withAlpha (slider.isMouseOverOrDragging() ? 0.55f : 0.34f));
        g.drawLine (grip.getX() + 3.0f, grip.getY() + 1.0f, grip.getRight() - 3.0f, grip.getY() + 1.0f, 1.2f);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawLine (grip.getX() + 3.0f, grip.getBottom() - 1.0f, grip.getRight() - 3.0f, grip.getBottom() - 1.0f, 1.2f);

        // A glint along the lit side, so the cylinder looks polished.
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.drawLine (wheel.getX() + radius * 0.62f, wheel.getY() + 3.0f,
                    wheel.getX() + radius * 0.62f, wheel.getBottom() - 3.0f, 1.0f);
    }

private:
    const bool springsToCentre;
};

/**
    An on-screen keyboard and a few controllers for testing sounds and mappings
    without a MIDI controller. Everything it sends goes through Engine::injectMidi
    into one input, on that input's channel, so program changes, key zones,
    mappings and learn all behave exactly as they would with real hardware.

    On an input that takes every channel (Omni -- the backing track, or a
    keyboard whose program sorts its parts by channel) the notes need a
    channel of their own, and that is chosen for you: clicking an instrument
    in the program plays it, and a small button names the channel and the
    instrument in case you want another. An input with a channel of its own
    plays on it and shows no choice.
*/
class KeyboardPanel : public juce::Component,
                      private juce::MidiKeyboardState::Listener
{
public:
    explicit KeyboardPanel (Engine& e)
        : engine (e), keyboard (state, juce::MidiKeyboardComponent::horizontalKeyboard)
    {
        state.addListener (this);

        addAndMakeVisible (keyboard);
        keyboard.setAvailableRange (24, 108);
        keyboard.setLowestVisibleKey (36);
        keyboard.setKeyWidth (22.0f);
        keyboard.setOctaveForMiddleC (4);
        keyboard.setVelocity (0.8f, false);
        keyboard.setWantsKeyboardFocus (true);

        addAndMakeVisible (targetLabel);
        targetLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        targetLabel.setColour (juce::Label::textColourId, juce::Colour (0xff4f9dff));

        addChildComponent (chBtn);
        chBtn.setTooltip ("Which of the input's channels these keys play on, and so which instrument. Clicking an instrument above chooses it too.");
        chBtn.onClick = [this] { showChannelMenu(); };

        addAndMakeVisible (hint);
        hint.setFont (juce::FontOptions (11.0f));
        hint.setColour (juce::Label::textColourId, juce::Colour (0xff9aa0ab));
        hint.setText ("Click keys, or focus the keyboard and play A S D F G H J K (W E T Y U for sharps), Z / X to change octave.", juce::dontSendNotification);

        /* A LinearBar with a value near its minimum draws as an almost empty
           box, which is why these read as dead text fields rather than
           controls. Vertical wheels with a visible thumb and a number beside
           them look like what they are, and pitch and mod are vertical on
           every keyboard ever made. */
        auto setupWheel = [this] (juce::Slider& s, double min, double max, double step,
                                  const juce::String& tip)
        {
            s.setRange (min, max, step);
            s.setSliderStyle (juce::Slider::LinearVertical);
            s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 34, 16);
            s.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffc7ccd6));
            s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            s.setTooltip (tip);
        };

        auto setupBar = [] (juce::Slider& s, double min, double max, double step,
                            const juce::String& tip)
        {
            s.setRange (min, max, step);
            s.setSliderStyle (juce::Slider::LinearHorizontal);
            s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
            s.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff15161c));
            s.setColour (juce::Slider::trackColourId,      juce::Colour (0xff4f9dff));
            s.setColour (juce::Slider::thumbColourId,      juce::Colour (0xffd7dbe2));
            s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            s.setTooltip (tip);
        };

        addAndMakeVisible (velocityLabel);
        addAndMakeVisible (velocity);
        setupBar (velocity, 1, 127, 1, "How hard the on-screen keys are struck");
        velocity.setValue (100, juce::dontSendNotification);
        velocity.onValueChange = [this] { keyboard.setVelocity ((float) (velocity.getValue() / 127.0), false); };

        addAndMakeVisible (sustain);
        sustain.setClickingTogglesState (true);
        sustain.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2b7a3f));
        sustain.setTooltip ("Sustain pedal (CC 64)");
        sustain.onClick = [this] { send (juce::MidiMessage::controllerEvent (channel(), 64, sustain.getToggleState() ? 127 : 0)); };

        addAndMakeVisible (modLabel);
        addAndMakeVisible (modWheel);
        setupWheel (modWheel, 0, 127, 1, "Mod wheel (CC 1). Stays where you leave it, like the real thing.");
        modWheel.onValueChange = [this] { send (juce::MidiMessage::controllerEvent (channel(), 1, (int) modWheel.getValue())); };

        addAndMakeVisible (bendLabel);
        addAndMakeVisible (pitchBend);
        setupWheel (pitchBend, -8192, 8191, 1, "Pitch bend (springs back to centre when released)");
        /* Centred, because that is where a bend wheel rests. It is bipolar, so
           the thumb belongs in the middle of its travel, not at the bottom. */
        pitchBend.setValue (0, juce::dontSendNotification);
        pitchBend.setDoubleClickReturnValue (true, 0);
        pitchBend.onValueChange = [this] { send (juce::MidiMessage::pitchWheel (channel(), (int) pitchBend.getValue() + 8192)); };
        /* Back to centre when let go. onDragEnd covers a normal drag; the mouse
           being released off the slider, or the value being nudged with the
           keyboard, would otherwise leave the pitch hanging. */
        pitchBend.onDragEnd = [this] { pitchBend.setValue (0); };

        /* Drawn as real wheels. Only the bend wheel gets a centre detent,
           because only it springs back to one. */
        pitchBend.setLookAndFeel (&bendLook);
        modWheel.setLookAndFeel (&modLook);

        addAndMakeVisible (ccLabel);
        addAndMakeVisible (ccNumber);
        ccNumber.setRange (0, 127, 1);
        ccNumber.setSliderStyle (juce::Slider::IncDecButtons);
        /* Say how wide the number is, or JUCE gives the text box its default
           width and squeezes the +/- buttons into whatever is left -- which is
           how the + ended up half drawn. */
        ccNumber.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 52, 22);
        ccNumber.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
        ccNumber.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff3a3d47));
        ccNumber.setValue (74, juce::dontSendNotification);
        ccNumber.setTooltip ("Which controller to send. Set it, then use Learn MIDI on a plugin parameter.");
        addAndMakeVisible (ccValue);
        setupBar (ccValue, 0, 127, 1, "Value to send for that controller");
        ccValue.onValueChange = [this] { send (juce::MidiMessage::controllerEvent (channel(), (int) ccNumber.getValue(), (int) ccValue.getValue())); };

        for (auto* l : { &velocityLabel, &ccLabel })
        {
            l->setFont (juce::FontOptions (12.0f));
            l->setColour (juce::Label::textColourId, juce::Colour (0xff9aa0ab));
            l->setJustificationType (juce::Justification::centredRight);
        }
        for (auto* l : { &bendLabel, &modLabel })
        {
            l->setFont (juce::FontOptions (11.0f));
            l->setColour (juce::Label::textColourId, juce::Colour (0xff9aa0ab));
            l->setJustificationType (juce::Justification::centred);
        }
    }

    ~KeyboardPanel() override
    {
        releaseAll();
        state.removeListener (this);

        // The sliders must let go before the look and feel they point at dies.
        pitchBend.setLookAndFeel (nullptr);
        modWheel.setLookAndFeel (nullptr);
    }

    /** Which input receives the notes. Held notes are released when this changes. */
    void setTargetInput (int inputIndex)
    {
        if (inputIndex == target) { refreshLabel(); return; }
        releaseAll();
        target = inputIndex;
        omniChannel = firstHeardChannel();
        refreshLabel();
    }

    /** Play this instrument of the current program: on an Omni input, the
        keys move to a channel it hears. Clicking an instrument row calls this. */
    void playInstrument (int slot)
    {
        const auto* def = currentProgram();
        if (def == nullptr || slot < 0 || slot >= (int) def->slots.size() || ! isOmni()) return;
        const auto& sd = def->slots[(size_t) slot];
        if (sd.hears (channel())) { refreshLabel(); return; }
        for (int c = 1; c <= 16; ++c)
            if (sd.hears (c)) { releaseAll(); omniChannel = c; break; }
        refreshLabel();
    }

    void refreshLabel()
    {
        const auto& inputs = engine.getSetup().inputs;
        const bool have = target >= 0 && target < (int) inputs.size();
        const bool choose = have && isOmni() && channelsMatter();
        if (! have)
            targetLabel.setText ("No input selected", juce::dontSendNotification);
        else if (choose)
            targetLabel.setText ("Playing into: " + inputs[(size_t) target].name, juce::dontSendNotification);
        else
            targetLabel.setText ("Playing into: " + inputs[(size_t) target].name + "  (ch " + juce::String (channel()) + ")", juce::dontSendNotification);
        chBtn.setVisible (choose);
        if (choose)
        {
            const auto who = instrumentOn (channel());
            chBtn.setButtonText ("ch " + juce::String (channel()) + (who.isNotEmpty() ? "  \xc2\xb7  " + who : juce::String ("  \xc2\xb7  nothing")));
        }
        resized();
    }

    /** All notes off + reset controllers on the target. */
    void releaseAll()
    {
        state.allNotesOff (0);
        if (sustain.getToggleState()) { sustain.setToggleState (false, juce::dontSendNotification); send (juce::MidiMessage::controllerEvent (channel(), 64, 0)); }
    }

    /* Tall enough for upright wheels with their readouts underneath. */
    static constexpr int preferredHeight = 150;

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 4);
        auto top = r.removeFromTop (22);
        targetLabel.setBounds (top.removeFromLeft (chBtn.isVisible() ? 200 : 260));
        if (chBtn.isVisible()) { chBtn.setBounds (top.removeFromLeft (230).reduced (0, 1)); top.removeFromLeft (10); }
        hint.setBounds (top);
        r.removeFromTop (4);

        /* The two wheels sit together on the left, upright and full height,
           the way they do on a keyboard -- bend nearest the keys. Velocity,
           sustain and the CC sender stack beside them, because those are set
           once and left alone rather than played. */
        /* Narrow: a wheel is a cylinder seen edge-on, and real ones are
           barely wider than a thumb. A wide column reads as a strip. */
        auto wheels = r.removeFromLeft (74);
        r.removeFromLeft (10);

        auto labels = wheels.removeFromBottom (14);
        bendLabel.setBounds (labels.removeFromLeft (34));
        labels.removeFromLeft (6);
        modLabel.setBounds (labels);

        pitchBend.setBounds (wheels.removeFromLeft (34));
        wheels.removeFromLeft (6);
        modWheel.setBounds (wheels);

        auto controls = r.removeFromLeft (300);
        r.removeFromLeft (10);
        keyboard.setBounds (r);

        auto row = controls.removeFromTop (24);
        velocityLabel.setBounds (row.removeFromLeft (52)); row.removeFromLeft (4);
        velocity.setBounds (row);
        controls.removeFromTop (6);

        row = controls.removeFromTop (24);
        sustain.setBounds (row.removeFromLeft (120));
        controls.removeFromTop (6);

        row = controls.removeFromTop (24);
        ccLabel.setBounds (row.removeFromLeft (70)); row.removeFromLeft (4);
        ccNumber.setBounds (row.removeFromLeft (104)); row.removeFromLeft (8);
        ccValue.setBounds (row);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff26282f));
    }

private:
    int channel() const
    {
        const auto& inputs = engine.getSetup().inputs;
        if (target >= 0 && target < (int) inputs.size() && inputs[(size_t) target].channel > 0)
            return inputs[(size_t) target].channel;
        return omniChannel;
    }
    bool isOmni() const
    {
        const auto& inputs = engine.getSetup().inputs;
        return target >= 0 && target < (int) inputs.size() && inputs[(size_t) target].channel <= 0 && ! inputs[(size_t) target].isAudio();
    }
    const ProgramDef* currentProgram() const
    {
        const auto& inputs = engine.getSetup().inputs;
        if (target < 0 || target >= (int) inputs.size()) return nullptr;
        const auto& in = inputs[(size_t) target];
        return in.currentProgram >= 0 && in.currentProgram < (int) in.programs.size() ? &in.programs[(size_t) in.currentProgram] : nullptr;
    }
    /** Does any instrument of the current program take some channels only? Then the choice is real. */
    bool channelsMatter() const
    {
        if (const auto* def = currentProgram())
            for (const auto& sd : def->slots) if (! sd.hearsAll()) return true;
        return false;
    }
    /** The lowest channel any instrument hears -- or 1. */
    int firstHeardChannel() const
    {
        if (const auto* def = currentProgram())
            for (int c = 1; c <= 16; ++c)
                for (const auto& sd : def->slots) if (sd.hears (c)) return c;
        return 1;
    }
    /** The instrument that plays channel c, by name; empty for none. */
    juce::String instrumentOn (int c) const
    {
        if (const auto* def = currentProgram())
            for (const auto& sd : def->slots) if (sd.hears (c)) return sd.displayName();
        return {};
    }
    void showChannelMenu()
    {
        juce::PopupMenu m;
        for (int c = 1; c <= 16; ++c)
        {
            const auto who = instrumentOn (c);
            if (who.isEmpty()) continue;
            m.addItem (c, "Ch " + juce::String (c) + "   \xc2\xb7   " + who, true, c == channel());
        }
        juce::Component::SafePointer<KeyboardPanel> self (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&chBtn), [self] (int r)
        {
            if (self == nullptr || r < 1) return;
            if (r != self->channel()) { self->releaseAll(); self->omniChannel = r; }
            self->refreshLabel();
        });
    }

    void send (const juce::MidiMessage& m)
    {
        if (target >= 0) engine.injectMidi (target, m);
    }

    // MidiKeyboardState::Listener (message thread)
    void handleNoteOn (juce::MidiKeyboardState*, int, int note, float vel) override
    {
        send (juce::MidiMessage::noteOn (channel(), note, vel));
    }
    void handleNoteOff (juce::MidiKeyboardState*, int, int note, float) override
    {
        send (juce::MidiMessage::noteOff (channel(), note));
    }

    Engine& engine;
    int target = -1;
    int omniChannel = 1;           // the channel the keys play on when the input takes every channel
    juce::TextButton chBtn;
    juce::MidiKeyboardState state;
    juce::MidiKeyboardComponent keyboard;
    juce::Label targetLabel, hint;
    juce::Label velocityLabel { {}, "Velocity" }, modLabel { {}, "Mod" }, bendLabel { {}, "Bend" }, ccLabel { {}, "Controller" };
    juce::Slider velocity, modWheel, pitchBend, ccNumber, ccValue;

    WheelLookAndFeel bendLook { true }, modLook { false };
    juce::TextButton sustain { "Sustain" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyboardPanel)
};

} // namespace perf
