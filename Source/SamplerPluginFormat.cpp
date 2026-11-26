#include "SamplerPluginFormat.h"
#include "SfzSynth.h"

#include <atomic>

namespace perf
{
using namespace juce;

namespace
{
/** One of the instrument's named controls, as a parameter: 0..1, the MIDI
    controller's whole travel. Moved by the host (a knob, the tablet) or by
    the controller itself arriving in the notes; either way the synth is told
    on the audio thread, in the next block. */
class SamplerParameter final : public HostedAudioProcessorParameter
{
public:
    SamplerParameter (int controller, String n, float start) : cc (controller), paramName (std::move (n)), initial (start), value (start) {}

    String getParameterID() const override              { return "cc" + String (cc); }
    String getName (int maximumLength) const override   { return paramName.substring (0, maximumLength); }
    String getLabel() const override                    { return {}; }
    float getValue() const override                     { return value.load(); }
    float getDefaultValue() const override              { return initial; }
    void setValue (float v) override                    { value.store (jlimit (0.0f, 1.0f, v)); toSynth.store (true); }
    String getText (float v, int) const override        { return String (roundToInt (v * 100.0f)) + "%"; }
    float getValueForText (const String& t) const override { return jlimit (0.0f, 1.0f, t.getFloatValue() / 100.0f); }

    const int cc;
    const String paramName;
    const float initial;
    std::atomic<float> value;
    std::atomic<bool> toSynth { true };      // the synth has not been told this value yet
    std::atomic<bool> toHost { false };      // it came in the notes: the host has not been told
};

class SamplerInstance final : public AudioPluginInstance,
                              private Timer
{
public:
    SamplerInstance (PluginDescription d, File f, std::unique_ptr<SfzSynth> s, double rate, int block)
        : AudioPluginInstance (BusesProperties().withOutput ("Output", AudioChannelSet::stereo(), true)),
          description (std::move (d)), file (std::move (f)), synth (std::move (s)), madeAtRate (rate), madeAtBlock (block)
    {
        for (const auto& c : synth->controls())
        {
            auto* p = new SamplerParameter (c.cc, c.name, c.value);
            byController[c.cc] = p;
            addHostedParameter (std::unique_ptr<HostedAudioProcessorParameter> (p));
        }
        startTimerHz (20);
    }

    const String getName() const override                   { return description.name; }
    double getTailLengthSeconds() const override            { return 0.0; }
    bool acceptsMidi() const override                       { return true; }
    bool producesMidi() const override                      { return false; }
    /* Its window is its named controls, as sliders; an instrument with none has no window. */
    AudioProcessorEditor* createEditor() override           { return byController.empty() ? nullptr : new GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override                         { return ! byController.empty(); }
    int getNumPrograms() override                           { return 1; }
    int getCurrentProgram() override                        { return 0; }
    void setCurrentProgram (int) override                   {}
    const String getProgramName (int) override              { return {}; }
    void changeProgramName (int, const String&) override    {}
    void fillInPluginDescription (PluginDescription& d) const override { d = description; }

    void prepareToPlay (double sampleRate, int blockSize) override
    {
        // A synth is made for one rate and block: another, and the instrument is read again.
        if (std::abs (sampleRate - madeAtRate) > 0.5 || blockSize > madeAtBlock)
        {
            String error;
            if (auto again = SfzSynth::create (sampleRate, blockSize, file, error))
            {
                const SpinLock::ScopedLockType l (swap);
                synth = std::move (again);
                madeAtRate = sampleRate;
                madeAtBlock = blockSize;
            }
        }
        for (auto& [cc, p] : byController) p->toSynth.store (true);
    }

    void releaseResources() override {}

    void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi) override
    {
        const ScopedNoDenormals noDenormals;
        buffer.clear();
        const SpinLock::ScopedTryLockType l (swap);
        if (! l.isLocked() || synth == nullptr || buffer.getNumChannels() < 2) { midi.clear(); return; }

        for (auto& [cc, p] : byController)
            if (p->toSynth.exchange (false)) synth->setControl (cc, p->value.load());
        // A named control moved from the keyboard: the parameter follows, so the knob and the tablet show it.
        for (const auto meta : midi)
            if (const auto m = meta.getMessage(); m.isController())
                if (auto it = byController.find (m.getControllerNumber()); it != byController.end())
                {
                    it->second->value.store ((float) m.getControllerValue() / 127.0f);
                    it->second->toHost.store (true);
                }

        synth->renderAdding (midi, buffer.getWritePointer (0), buffer.getWritePointer (1), buffer.getNumSamples());
        midi.clear();
    }

    /* The state: the named controls where they stand. The instrument itself is the plugin. */
    void getStateInformation (MemoryBlock& dest) override
    {
        XmlElement xml ("SAMPLER");
        for (auto& [cc, p] : byController)
        {
            auto* c = xml.createNewChildElement ("CONTROL");
            c->setAttribute ("cc", cc);
            c->setAttribute ("value", (double) p->value.load());
        }
        copyXmlToBinary (xml, dest);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size); xml != nullptr && xml->hasTagName ("SAMPLER"))
            for (auto* c : xml->getChildWithTagNameIterator ("CONTROL"))
                if (auto it = byController.find (c->getIntAttribute ("cc", -1)); it != byController.end())
                {
                    it->second->setValue ((float) c->getDoubleAttribute ("value", it->second->initial));
                    it->second->toHost.store (true);
                }
    }

private:
    void timerCallback() override
    {
        for (auto& [cc, p] : byController)
            if (p->toHost.exchange (false)) p->sendValueChangedMessageToListeners (p->value.load());
    }

    PluginDescription description;
    File file;
    std::unique_ptr<SfzSynth> synth;
    SpinLock swap;
    double madeAtRate;
    int madeAtBlock;
    std::map<int, SamplerParameter*> byController;      // owned by the processor's parameter list
};
} // namespace

//==============================================================================
File SamplerPluginFormat::settingsFolder()
{
    if (const char* dir = std::getenv ("STAGERIG_SETTINGS_DIR"); dir != nullptr && *dir != 0)
        return File::getCurrentWorkingDirectory().getChildFile (String::fromUTF8 (dir));
    return File::getSpecialLocation (File::userHomeDirectory).getChildFile (".config/StageRig");
}

PluginDescription SamplerPluginFormat::describe (const File& sfzFile)
{
    PluginDescription d;
    d.name = sfzFile.getFileNameWithoutExtension();
    d.descriptiveName = d.name;
    d.pluginFormatName = getFormatName();
    d.category = "Instrument|Sampler";
    d.manufacturerName = "Sampled instrument";
    d.version = "1";
    d.fileOrIdentifier = sfzFile.getFullPathName();
    d.uniqueId = d.deprecatedUid = d.name.toLowerCase().hashCode();      // the name, not the place: the same instrument on any computer
    d.isInstrument = true;
    d.numInputChannels = 0;
    d.numOutputChannels = 2;
    return d;
}

void SamplerPluginFormat::findAllTypesForFile (OwnedArray<PluginDescription>& results, const String& fileOrIdentifier)
{
    if (const File f (fileOrIdentifier); File::isAbsolutePath (fileOrIdentifier) && f.hasFileExtension ("sfz;dspreset") && f.existsAsFile())
        results.add (new PluginDescription (describe (f)));
}

bool SamplerPluginFormat::doesPluginStillExist (const PluginDescription& d)
{
    return SfzSynth::find (settingsFolder(), d.name, d.fileOrIdentifier) != File();
}

StringArray SamplerPluginFormat::searchPathsForPlugins (const FileSearchPath&, bool, bool)
{
    StringArray found;
    for (const auto& i : SfzSynth::installedInstruments (settingsFolder())) found.add (i.file.getFullPathName());
    return found;
}

std::unique_ptr<AudioPluginInstance> SamplerPluginFormat::createInstance (const PluginDescription& d, double sampleRate, int blockSize, String& error)
{
    const auto file = SfzSynth::find (settingsFolder(), d.name, d.fileOrIdentifier);
    if (file == File())
    {
        error = d.name + " is not on this computer: put its folder in the Instruments folder beside StageRig's settings";
        return nullptr;
    }
    auto synth = SfzSynth::create (sampleRate, blockSize, file, error);
    if (synth == nullptr) return nullptr;
    return std::make_unique<SamplerInstance> (d, file, std::move (synth), sampleRate, jmax (16, blockSize));
}

void SamplerPluginFormat::createPluginInstance (const PluginDescription& d, double sampleRate, int blockSize, PluginCreationCallback callback)
{
    String error;
    auto instance = createInstance (d, sampleRate, blockSize, error);
    callback (std::move (instance), error);
}

} // namespace perf
