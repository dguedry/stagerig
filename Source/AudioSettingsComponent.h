#pragma once

#include "Engine.h"
#include "LowLatency.h"
#include <juce_audio_utils/juce_audio_utils.h>

namespace perf
{

/** Audio settings: a live-latency section (PipeWire quantum, clock takeover,
    realtime status) above JUCE's device selector. */
class AudioSettingsComponent : public juce::Component,
                               private juce::Timer,
                               private juce::ChangeListener
{
public:
    AudioSettingsComponent (Engine& e, juce::PropertiesFile& s, std::function<void (const juce::String&)> statusCb)
        : engine (e), settings (s), onStatus (std::move (statusCb)),
          selector (e.getDeviceManager(), 0, 64, 1, 64, false, false, true, false)
    {
        addAndMakeVisible (title);
        title.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, juce::Colours::white);

        addAndMakeVisible (quantumLabel);
        addAndMakeVisible (quantumBox);
        for (int q : { 32, 64, 128, 256, 512, 1024 })
            quantumBox.addItem (juce::String (q) + " samples  (" + juce::String (1000.0 * q / 48000.0, 2) + " ms @ 48 kHz)", q);
        quantumBox.setSelectedId (settings.getIntValue ("pipewireQuantum", 128), juce::dontSendNotification);
        quantumBox.setTooltip ("Buffer size StageRig asks PipeWire for. Smaller = lower latency, more CPU headroom needed. 128 is a good live default; 64 if the machine is quiet.");

        addAndMakeVisible (takeover);
        takeover.setToggleState (settings.getBoolValue ("pipewireTakeover", true), juce::dontSendNotification);
        takeover.setTooltip ("Every audio app on this computer is held at this buffer size and 48 kHz while StageRig is open, and put back when it quits. Turn this on when another app has forced a big buffer and StageRig cannot get a small one.");

        addAndMakeVisible (performance);
        performance.setToggleState (settings.getBoolValue ("performanceMode", true), juce::dontSendNotification);
        performance.setTooltip ("Holds the computer in its performance power mode while StageRig runs, so the processor "
                                "does not slow down between notes and answer late (a cause of crackle). Put back when StageRig quits.");
        performance.onClick = [this]
        {
            const bool on = performance.getToggleState();
            settings.setValue ("performanceMode", on);
            settings.saveIfNeeded();
            const bool ok = LowLatency::holdPerformanceMode (on);
            if (on && ! ok && onStatus) onStatus ("This computer has no performance power mode to hold (power-profiles-daemon).");
            refreshStatus();
        };

        addAndMakeVisible (volumeLabel);
        addAndMakeVisible (volumeSlider);
        volumeSlider.setRange (0.0, 100.0, 1.0);
        volumeSlider.setTextValueSuffix ("%");
        volumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        volumeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);
        volumeSlider.setTooltip ("The computer's own volume for this output, applied after StageRig's levels. "
                                 "100% passes StageRig through untouched: set loudness with StageRig and your amplifier.");
        volumeSlider.onValueChange = [this] { volumeWanted (juce::roundToInt (volumeSlider.getValue())); };
        addAndMakeVisible (fullBtn);
        fullBtn.setTooltip ("Pass StageRig's sound through at full level. Turn your speakers or headphones down first: this can be a big jump.");
        fullBtn.onClick = [this] { volumeSlider.setValue (100.0, juce::sendNotificationSync); };
        addAndMakeVisible (volumeNote);
        volumeNote.setFont (juce::FontOptions (12.0f));
        volumeNote.setColour (juce::Label::textColourId, juce::Colour (0xff9aa0ab));

        addAndMakeVisible (applyBtn);
        applyBtn.setTooltip ("Close and reopen the audio device. Use it when the sound has stopped: after the computer's "
                             "sound system restarted, or an audio device came back.");
        quantumBox.onChange = [this] { apply(); };
        takeover.onClick = [this] { holdSystem (takeover.getToggleState()); apply(); };
        addAndMakeVisible (keptNote);
        keptNote.setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (typeNote);
        typeNote.setFont (juce::FontOptions (12.5f));
        typeNote.setJustificationType (juce::Justification::topLeft);
        typeNote.setColour (juce::Label::textColourId, juce::Colour (0xffc8ccd4));
        keptNote.setColour (juce::Label::textColourId, juce::Colour (0xff9aa0ab));
        applyBtn.onClick = [this] { apply(); };

        addAndMakeVisible (status);
        status.setFont (juce::FontOptions (12.0f));
        status.setColour (juce::Label::textColourId, juce::Colour (0xff9aa0ab));
        status.setJustificationType (juce::Justification::topLeft);

        addAndMakeVisible (selector);
        engine.getDeviceManager().addChangeListener (this);
        lastType = engine.getDeviceManager().getCurrentAudioDeviceType();
        engine.setAudioWindowOpen (true);
        refreshStatus();
        refreshVolume();
        startTimer (1000);
    }

    ~AudioSettingsComponent() override
    {
        engine.getDeviceManager().removeChangeListener (this);
        engine.setAudioWindowOpen (false);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        title.setBounds (r.removeFromTop (22));
        auto row = r.removeFromTop (26);
        quantumLabel.setBounds (row.removeFromLeft (120));
        quantumBox.setBounds (row.removeFromLeft (260));
        r.removeFromTop (6);
        takeover.setBounds (r.removeFromTop (24));
        performance.setBounds (r.removeFromTop (24));
        r.removeFromTop (4);
        auto vol = r.removeFromTop (26);
        volumeLabel.setBounds (vol.removeFromLeft (120));
        fullBtn.setBounds (vol.removeFromRight (70)); vol.removeFromRight (8);
        volumeSlider.setBounds (vol);
        volumeNote.setBounds (r.removeFromTop (18).withTrimmedLeft (120));
        r.removeFromTop (4);
        status.setBounds (r.removeFromTop (118));
        r.removeFromTop (8);
        typeNote.setBounds (r.removeFromTop (64));
        r.removeFromTop (4);
        auto bottom = r.removeFromBottom (28);
        applyBtn.setBounds (bottom.removeFromRight (150));
        bottom.removeFromRight (10);
        keptNote.setBounds (bottom);
        r.removeFromBottom (6);
        selector.setBounds (r);
    }

private:
    /** Ticked: the whole computer is held at StageRig's buffer size, the clock as it was
        kept until StageRig quits (Main puts it back). Unticked: put back now. */
    void holdSystem (bool hold)
    {
        if (hold)
        {
            if (! settings.getBoolValue ("clockSaved", false))
            {
                const auto was = LowLatency::readClock();
                settings.setValue ("clockForceQuantum", was.forceQuantum);
                settings.setValue ("clockForceRate", was.forceRate);
                settings.setValue ("clockQuantum", was.quantum);
                settings.setValue ("clockRate", was.rate);
                settings.setValue ("clockSaved", true);
            }
        }
        else if (settings.getBoolValue ("clockSaved", false))
        {
            LowLatency::restoreClock ({ settings.getValue ("clockForceQuantum"), settings.getValue ("clockForceRate"),
                                        settings.getValue ("clockQuantum"), settings.getValue ("clockRate") });
            settings.removeValue ("clockSaved");
        }
        settings.saveIfNeeded();
    }

    void apply()
    {
        const int q = quantumBox.getSelectedId();
        settings.setValue ("pipewireQuantum", q);
        settings.setValue ("pipewireTakeover", takeover.getToggleState());
        settings.saveIfNeeded();
        LowLatency::setRequestedQuantum (q, 48000);
        if (takeover.getToggleState()) LowLatency::forceClock (q, 48000);
        engine.restartAudioDevice();
        refreshStatus();
        if (onStatus) onStatus ("Audio reopened, at " + juce::String (q) + " samples.");
    }

    void refreshStatus()
    {
        juce::String txt;
        if (auto* d = engine.getDeviceManager().getCurrentAudioDevice())
        {
            const double sr = d->getCurrentSampleRate();
            const int smp = engine.getLastBlockSize() > 0 ? engine.getLastBlockSize() : d->getCurrentBufferSizeSamples();
            txt << d->getTypeName() << " / " << d->getName() << ":  " << juce::String (sr / 1000.0, 1) << " kHz, "
                << smp << " samples per block (" << juce::String (1000.0 * smp / sr, 1) << " ms), output latency "
                << juce::String (1000.0 * d->getOutputLatencyInSamples() / sr, 1) << " ms";
            if (d->getTypeName() == "JACK")
                txt << (LowLatency::pipeWireJackLoaded() ? "  (PipeWire graph)" : "  (JACK server)");
            else
                txt << "  -- choose JACK as the audio device type below: that is PipeWire's direct path, with no resampling";
        }
        else txt << "no audio device open";
        for (const auto& w : engine.deviceWarnings()) txt << "\nWARNING: " << w;
        txt << "\n" << LowLatency::realtimeSchedulingHint();
        if (settings.getBoolValue ("performanceMode", true))
            txt << (LowLatency::performanceModeHeld() ? "\nProcessor held at full speed (performance power mode)"
                                                      : "\nWARNING: the processor could not be held at full speed: no performance power mode here");
        const auto clock = LowLatency::readClock();
        if (clock.forceQuantum.isNotEmpty() && clock.forceQuantum != "0")
            txt << "\nSystem buffer forced to " << clock.forceQuantum << " samples" << (clock.forceRate.isNotEmpty() && clock.forceRate != "0" ? " at " + clock.forceRate + " Hz" : juce::String());
        const int wanted = settings.getBoolValue ("pipewireTakeover", true) ? settings.getIntValue ("pipewireQuantum", 128) : 0;
        if (wanted > 0 && engine.getLastBlockSize() > wanted)
            txt << "\nWARNING: the system is running a " << engine.getLastBlockSize() << "-sample buffer, not the " << wanted
                << " you asked for. Another application (Bitwig, for example) has forced it: close it, or set its buffer to " << wanted << ".";
        else if (clock.quantum.isNotEmpty())
            txt << "\nSystem buffer " << clock.quantum << " samples at " << clock.rate << " Hz (not forced)";
        status.setText (txt, juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, LowLatency::realtimeSchedulingAvailable() ? juce::Colour (0xff9aa0ab) : juce::Colours::orange);
    }

    //==============================================================================
    /* The computer's volume for the chosen output (PipeWire's, through pactl): the
       JACK output's name is the output's description there, and ALSA's default
       device is the system's default output. Anything else has no such volume. */
    static juce::String runPactl (const juce::StringArray& args)
    {
        juce::ChildProcess p;
        juce::StringArray cmd { "pactl" };
        cmd.addArray (args);
        if (! p.start (cmd)) return {};
        auto out = p.readAllProcessOutput();
        return p.getExitCode() == 0 ? out : juce::String();
    }

    /** The system output StageRig plays into, as { name, description }, or empty. */
    std::pair<juce::String, juce::String> systemOutput() const
    {
        auto& dm = engine.getDeviceManager();
        const auto type = dm.getCurrentAudioDeviceType();
        const auto out = dm.getAudioDeviceSetup().outputDeviceName;
        juce::String wantName, wantDesc;
        if (type == "JACK") wantDesc = out;
        else if (type == "ALSA" && out.contains ("PipeWire")) wantName = runPactl ({ "get-default-sink" }).trim();
        else return {};
        if (wantDesc.isEmpty() && wantName.isEmpty()) return {};
        for (const auto& sink : sinks)
            if ((wantDesc.isNotEmpty() && sink.second == wantDesc) || (wantName.isNotEmpty() && sink.first == wantName))
                return sink;
        return {};
    }

    void refreshVolume()
    {
        sinks.clear();
        volumes.clear();
        const auto parsed = juce::JSON::parse (runPactl ({ "-f", "json", "list", "sinks" }));
        if (auto* arr = parsed.getArray())
            for (const auto& s : *arr)
            {
                const juce::String name = s["name"].toString(), desc = s["description"].toString();
                int percent = -1;
                if (auto* chans = s["volume"].getDynamicObject())
                    for (const auto& ch : chans->getProperties())
                    {
                        percent = ch.value["value_percent"].toString().upToFirstOccurrenceOf ("%", false, false).getIntValue();
                        break;
                    }
                sinks.push_back ({ name, desc });
                volumes.push_back (percent);
            }

        const auto out = systemOutput();
        sinkName = out.first;
        int percent = -1;
        for (size_t i = 0; i < sinks.size(); ++i)
            if (sinks[i].first == sinkName) percent = volumes[i];

        const bool known = sinkName.isNotEmpty() && percent >= 0;
        volumeSlider.setEnabled (known);
        fullBtn.setEnabled (known && percent != 100);
        if (! known)
        {
            volumeNote.setText ("This output has no computer volume of its own.", juce::dontSendNotification);
            return;
        }
        if (! volumeSlider.isMouseButtonDown() && pendingVolume < 0)
            volumeSlider.setValue (juce::jmin (100, percent), juce::dontSendNotification);
        volumeNote.setText (percent >= 100 ? out.second + ": full level, StageRig's sound untouched."
                                           : out.second + ": turned down by the computer. 100% is best for a stage rig.",
                            juce::dontSendNotification);
    }

    void volumeWanted (int percent)
    {
        if (sinkName.isEmpty()) return;
        const bool waiting = pendingVolume >= 0;
        pendingVolume = percent;
        if (waiting) return;        // one pactl at a time while the slider moves
        juce::Timer::callAfterDelay (60, [sp = juce::Component::SafePointer<AudioSettingsComponent> (this)]
        {
            if (sp == nullptr) return;
            const int v = sp->pendingVolume;
            runPactl ({ "set-sink-volume", sp->sinkName, juce::String (v) + "%" });
            sp->pendingVolume = -1;
            if (juce::roundToInt (sp->volumeSlider.getValue()) != v) sp->volumeWanted (juce::roundToInt (sp->volumeSlider.getValue()));
            else sp->refreshVolume();
        });
    }

    void timerCallback() override { refreshStatus(); refreshVolume(); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        /* Just switched to ALSA: no input to start with. An input on another card
           than the output is two clocks drifting apart (crackle), and it takes
           that card from the desktop too; choose one if you use a mic or line in. */
        const auto type = engine.getDeviceManager().getCurrentAudioDeviceType();
        if (type == "ALSA" && lastType.isNotEmpty() && lastType != "ALSA")
        {
            auto& dm = engine.getDeviceManager();
            auto setup = dm.getAudioDeviceSetup();
            if (setup.inputDeviceName.isNotEmpty())
            {
                setup.inputDeviceName = {};
                setup.useDefaultInputChannels = false;
                setup.inputChannels.clear();
                lastType = type;
                dm.setAudioDeviceSetup (setup, true);
            }
        }
        lastType = type;
        refreshStatus();
        refreshVolume();
    }
    juce::String lastType;

    Engine& engine;
    juce::PropertiesFile& settings;
    std::function<void (const juce::String&)> onStatus;
    juce::Label title { {}, "LEVEL AND LATENCY" }, quantumLabel { {}, "Buffer size" }, status;
    juce::ComboBox quantumBox;
    juce::ToggleButton performance { "Keep the processor at full speed while StageRig runs" };
    juce::ToggleButton takeover { "Make the whole system use this buffer size while StageRig runs" };
    juce::TextButton applyBtn { "Reopen audio" }, fullBtn { "100%" };
    juce::Label volumeLabel { {}, "Computer volume" }, volumeNote,
                keptNote { {}, "Every change here takes effect at once and is kept." },
                typeNote { {}, "Device type: on stage, choose ALSA and your interface. StageRig then has it to itself, "
                               "with nothing in between: the least delay and the fewest things that can go wrong (other apps "
                               "can't play through it meanwhile). JACK shares the interface with other apps through the "
                               "computer's sound system. Either way StageRig reconnects a device switched off and on." };
    juce::Slider volumeSlider;
    std::vector<std::pair<juce::String, juce::String>> sinks;   // { name, description }
    std::vector<int> volumes;
    juce::String sinkName;
    int pendingVolume = -1;
    juce::AudioDeviceSelectorComponent selector;
};

} // namespace perf
