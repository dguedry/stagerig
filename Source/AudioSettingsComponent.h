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

        addAndMakeVisible (applyBtn);
        applyBtn.setTooltip ("Reopen the audio device at this buffer size");
        applyBtn.onClick = [this] { apply(); };

        addAndMakeVisible (status);
        status.setFont (juce::FontOptions (12.0f));
        status.setColour (juce::Label::textColourId, juce::Colour (0xff9aa0ab));
        status.setJustificationType (juce::Justification::topLeft);

        addAndMakeVisible (selector);
        engine.getDeviceManager().addChangeListener (this);
        refreshStatus();
        startTimer (1000);
    }

    ~AudioSettingsComponent() override { engine.getDeviceManager().removeChangeListener (this); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        title.setBounds (r.removeFromTop (22));
        auto row = r.removeFromTop (26);
        quantumLabel.setBounds (row.removeFromLeft (120));
        quantumBox.setBounds (row.removeFromLeft (260)); row.removeFromLeft (10);
        applyBtn.setBounds (row.removeFromLeft (90));
        r.removeFromTop (6);
        takeover.setBounds (r.removeFromTop (24));
        r.removeFromTop (4);
        status.setBounds (r.removeFromTop (118));
        r.removeFromTop (8);
        selector.setBounds (r);
    }

private:
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
        if (onStatus) onStatus ("Audio reopened at " + juce::String (q) + " samples");
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

    void timerCallback() override { refreshStatus(); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refreshStatus(); }

    Engine& engine;
    juce::PropertiesFile& settings;
    std::function<void (const juce::String&)> onStatus;
    juce::Label title { {}, "LATENCY" }, quantumLabel { {}, "Buffer size" }, status;
    juce::ComboBox quantumBox;
    juce::ToggleButton takeover { "Make the whole system use this buffer size while StageRig runs" };
    juce::TextButton applyBtn { "Apply" };
    juce::AudioDeviceSelectorComponent selector;
};

} // namespace perf
