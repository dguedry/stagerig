#include "SfzSynth.h"

#include <sfizz.hpp>

namespace perf
{
using namespace juce;

SfzSynth::SfzSynth() = default;
SfzSynth::~SfzSynth() = default;

Array<SfzSynth::Instrument> SfzSynth::installedInstruments (const File& settingsFolder)
{
    Array<Instrument> found;
    const auto home = File::getSpecialLocation (File::userHomeDirectory);
    for (const auto& dir : { File ("/usr/share/stagerig/instruments"), File ("/usr/local/share/stagerig/instruments"),
                             File ("/usr/share/sounds/sfz"), home.getChildFile (".local/share/sounds/sfz"),
                             settingsFolder != File() ? settingsFolder.getChildFile ("Instruments") : File() })
    {
        if (! dir.isDirectory()) continue;
        // A few folders deep, not the whole tree: an instrument's samples sit in folders below its .sfz.
        std::function<void (const File&, int)> look = [&] (const File& d, int depth)
        {
            for (const auto& f : d.findChildFiles (File::findFiles, false, "*.sfz"))
            {
                bool seen = false;
                for (const auto& i : found) seen = seen || i.file == f || i.name.equalsIgnoreCase (f.getFileNameWithoutExtension());
                if (! seen) found.add ({ f.getFileNameWithoutExtension(), f });
            }
            if (depth > 0)
                for (const auto& sub : d.findChildFiles (File::findDirectories, false))
                    look (sub, depth - 1);
        };
        look (dir, 3);
    }
    std::sort (found.begin(), found.end(), [] (const Instrument& a, const Instrument& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return found;
}

File SfzSynth::find (const File& settingsFolder, const String& name, const String& path)
{
    if (path.isNotEmpty() && File::isAbsolutePath (path) && File (path).existsAsFile()) return File (path);
    if (name.isNotEmpty())
        for (const auto& i : installedInstruments (settingsFolder))
            if (i.name.equalsIgnoreCase (name)) return i.file;
    return {};
}

std::unique_ptr<SfzSynth> SfzSynth::create (double sampleRate, int blockSize, const File& instrument, String& error)
{
    if (! instrument.existsAsFile()) { error = "instrument not found: " + instrument.getFullPathName(); return nullptr; }

    std::unique_ptr<SfzSynth> s (new SfzSynth());
    s->synth = std::make_unique<sfz::Sfizz>();
    s->maxBlock = jmax (16, blockSize);
    s->synth->setSampleRate ((float) sampleRate);
    s->synth->setSamplesPerBlock (s->maxBlock);
    s->synth->setNumVoices (128);          // a piano with the pedal down wants more than the 64 it starts with
    if (! s->synth->loadSfzFile (instrument.getFullPathName().toStdString()))
    {
        error = instrument.getFileName() + " could not be read as an SFZ instrument";
        return nullptr;
    }
    if (s->synth->getNumRegions() == 0)
    {
        error = instrument.getFileName() + " has no sounds in it (or its samples are missing)";
        return nullptr;
    }
    s->scratch.setSize (2, s->maxBlock);
    return s;
}

void SfzSynth::silence()
{
    if (synth != nullptr) synth->allSoundOff();
}

void SfzSynth::renderAdding (const MidiBuffer& midi, float* left, float* right, int numSamples)
{
    if (synth == nullptr || numSamples <= 0) return;

    // In pieces no longer than the synth was told a block is.
    auto event = midi.begin();
    for (int done = 0; done < numSamples;)
    {
        const int n = jmin (maxBlock, numSamples - done);
        for (; event != midi.end() && (*event).samplePosition < done + n; ++event)
        {
            const auto m = (*event).getMessage();
            const int at = jlimit (0, n - 1, (*event).samplePosition - done);
            if (m.isNoteOn())                 synth->noteOn (at, m.getNoteNumber(), m.getVelocity());
            else if (m.isNoteOff())           synth->noteOff (at, m.getNoteNumber(), m.getVelocity());
            else if (m.isAllNotesOff() || m.isAllSoundOff()) synth->allSoundOff();
            else if (m.isController())        synth->cc (at, m.getControllerNumber(), m.getControllerValue());
            else if (m.isPitchWheel())        synth->pitchWheel (at, m.getPitchWheelValue() - 8192);
            else if (m.isChannelPressure())   synth->channelAftertouch (at, m.getChannelPressureValue());
            else if (m.isAftertouch())        synth->polyAftertouch (at, m.getNoteNumber(), m.getAfterTouchValue());
        }
        float* out[2] = { scratch.getWritePointer (0), scratch.getWritePointer (1) };
        synth->renderBlock (out, (size_t) n);
        FloatVectorOperations::add (left + done,  out[0], n);
        FloatVectorOperations::add (right + done, out[1], n);
        done += n;
    }
}

} // namespace perf
