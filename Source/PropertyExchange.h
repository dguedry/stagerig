#pragma once

#include "Surfaces.h"
#include <juce_midi_ci/juce_midi_ci.h>
#include <map>
#include <set>

namespace perf
{

/** StageRig's side of MIDI-CI Property Exchange: the MIDI 2.0 standard by
    which a keyboard asks what it is playing and is told when that changes.
    A keyboard that asks is answered, whoever made it, with:

      - ChannelList: the input, and the program in play (its name and number);
      - ProgramList: every program with something in it, to choose from;
      - X-ParameterList and X-ProgramEdit (Korg's, as its Keystage asks for
        them): the eight knobs' names, the controllers they send and where
        they are.

    It answers from the same SurfaceView every keyboard is shown, so it
    follows the knob pages, the controller setups and the NKS pages. Only the
    messages are here; MidiCiKeyboard (below) carries them over a MIDI port,
    and the tests carry them to a keyboard made in software. Message thread. */
class PropertyResponder : private juce::midi_ci::PropertyDelegate,
                          private juce::midi_ci::DeviceMessageHandler,
                          private juce::midi_ci::DeviceListener
{
public:
    /** Where outgoing SysEx goes, without its F0 and F7. */
    using Send = std::function<void (juce::Span<const std::byte>)>;
    explicit PropertyResponder (Send);
    ~PropertyResponder() override;

    /** A MIDI-CI SysEx message from the keyboard, without its F0 and F7. */
    void receive (juce::Span<const std::byte>);
    /** Tells whoever listens that StageRig is here: a keyboard that speaks MIDI-CI answers, and starts asking. */
    void announce();

    void show (const SurfaceView&);
    /** A keyboard's knob was turned just now, so its own screen already shows it: its value is not sent back (Korg's Keystage is known to stall on that echo). */
    void knobTurnedOnKeyboard (int knob);

    /** Something has asked this StageRig for a property: there is a keyboard listening. */
    bool isAskedFor() const                         { return asked; }
    /** The keyboard's name for itself, from its DeviceInfo when it gave one. */
    juce::String getRemoteName() const;
    /** What was asked and told lately, oldest first, for a bug report. */
    juce::StringArray getLog() const                { return log; }

    /** The properties' contents, as sent: for the tests. */
    static juce::var channelList (const SurfaceView&);
    static juce::var programList (const SurfaceView&);
    static juce::var parameterList (const SurfaceView&);
    static juce::var programEdit (const SurfaceView&);

private:
    juce::midi_ci::PropertyReplyData propertyGetDataRequested (juce::midi_ci::MUID, const juce::midi_ci::PropertyRequestHeader&) override;
    juce::midi_ci::PropertyReplyHeader propertySetDataRequested (juce::midi_ci::MUID, const juce::midi_ci::PropertyRequestData&) override;
    bool subscriptionStartRequested (juce::midi_ci::MUID, const juce::midi_ci::PropertySubscriptionHeader&) override;
    void subscriptionDidStart (juce::midi_ci::MUID, const juce::String& subId, const juce::midi_ci::PropertySubscriptionHeader&) override;
    void subscriptionWillEnd (juce::midi_ci::MUID, const juce::midi_ci::Subscription&) override;
    void processMessage (juce::universal_midi_packets::BytesOnGroup) override;
    void deviceRemoved (juce::midi_ci::MUID) override;

    juce::var contentOf (const juce::String& resource, const juce::String& resId) const;
    void tell (const juce::String& resource);

    Send send;
    std::unique_ptr<juce::midi_ci::Device> device;
    SurfaceView view;
    bool haveView = false, asked = false;
    std::map<juce::String, juce::String> sent;      // resource -> the JSON last sent to its subscribers
    struct Subscriber { juce::midi_ci::MUID muid; juce::String id; bool operator< (const Subscriber& o) const { return std::tie (muid, id) < std::tie (o.muid, o.id); } };
    std::map<juce::String, std::set<Subscriber>> subscribers;
    std::array<juce::uint32, SurfaceView::numKnobs> turnedAt {};
    juce::StringArray log;
    void note (const juce::String&);
};

/** A keyboard on a MIDI port pair that answers MIDI-CI: StageRig's
    PropertyResponder over that port. Every keyboard used by an input is
    asked once, and one that never answers is never shown as found. */
class MidiCiKeyboard : public ControlSurface, private juce::MidiInputCallback, private juce::AsyncUpdater
{
public:
    struct Ports { juce::MidiDeviceInfo in, out; };
    /** MIDI inputs, by name, that have an output of the same name: one keyboard, both ways. */
    static std::vector<Ports> find (const juce::StringArray& inputNames);

    explicit MidiCiKeyboard (const Ports&);
    ~MidiCiKeyboard() override;
    bool opened() const                             { return input != nullptr && output != nullptr; }
    const juce::String& getInputIdentifier() const  { return ports.in.identifier; }

    juce::String getName() const override;
    juce::String getLanguage() const override       { return "standard MIDI-CI"; }
    juce::String getReport() const override;
    juce::String getKeysPortName() const override   { return ports.in.name; }
    bool isConnected() const override;
    bool isFound() const override                   { return responder.isAskedFor(); }
    bool hasScreen() const override                 { return responder.isAskedFor(); }
    void show (const SurfaceView&) override;
    void takeTouches (std::vector<Touch>&) override {}

private:
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    void handleAsyncUpdate() override;

    Ports ports;
    std::unique_ptr<juce::MidiOutput> output;
    std::unique_ptr<juce::MidiInput> input;
    PropertyResponder responder;
    std::array<std::atomic<int>, SurfaceView::numKnobs> knobControllers {};   // read on the MIDI thread
    std::array<std::atomic<bool>, SurfaceView::numKnobs> turned {};
    std::mutex queueMutex;
    std::vector<std::vector<std::byte>> queue;      // MIDI-CI messages in, for the message thread
};

} // namespace perf
