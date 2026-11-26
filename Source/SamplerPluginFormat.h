#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace perf
{

/**
    Sampled instruments, hosted as plugins.

    An SFZ instrument -- a small text file and a folder of recordings -- is
    played by sfizz, compiled in (SfzSynth). Here each one is given the shape
    of a plugin: a description that names it, and an instance whose
    parameters are the controls the instrument's maker named (reverb,
    release, a microphone's level). So everything StageRig does with a
    plugin it does with a sampled instrument and knows no difference: it
    runs in its own process, its controls go on the knob pages, a pedal can
    be mapped to one, the tablet shows them as sliders, and a sound of it
    can be kept.

    A description's fileOrIdentifier is the .sfz file's path (or a Decent
    Sampler preset's, DecentSampler.h); its name is the file's name without
    its extension. On another computer the path is not there,
    and the name finds the instrument where it is installed.

    Only the plugin host process has this format in its list. StageRig itself
    makes the descriptions (describe()), from the instruments it finds.
*/
class SamplerPluginFormat : public juce::AudioPluginFormat
{
public:
    static juce::String getFormatName()                 { return "SFZ"; }
    juce::String getName() const override               { return getFormatName(); }

    /** The description of the instrument in this .sfz file. */
    static juce::PluginDescription describe (const juce::File& sfzFile);
    /** Where the player's own instruments are: the "Instruments" folder beside the settings. */
    static juce::File settingsFolder();

    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String& fileOrIdentifier) override;
    bool fileMightContainThisPluginType (const juce::String& f) override    { return f.endsWithIgnoreCase (".sfz") || f.endsWithIgnoreCase (".dspreset"); }
    juce::String getNameOfPluginFromIdentifier (const juce::String& f) override { return juce::File (f).getFileNameWithoutExtension(); }
    bool pluginNeedsRescanning (const juce::PluginDescription&) override     { return false; }
    bool doesPluginStillExist (const juce::PluginDescription&) override;
    bool canScanForPlugins() const override             { return true; }
    bool isTrivialToScan() const override               { return true; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override;
    juce::FileSearchPath getDefaultLocationsToSearch() override              { return {}; }
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }

    static std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription&, double sampleRate, int blockSize, juce::String& error);

private:
    void createPluginInstance (const juce::PluginDescription&, double initialSampleRate, int initialBufferSize, PluginCreationCallback) override;
};

} // namespace perf
