#include "GmSynth.h"
#include <dlfcn.h>
#include <mutex>

using namespace juce;

namespace perf
{

namespace
{
    /* The part of FluidSynth's C API used here, declared by hand: the library
       is opened at run time, so no headers are needed to build. */
    struct Fluid
    {
        void* (*newSettings)() = nullptr;
        void  (*deleteSettings) (void*) = nullptr;
        int   (*setNum) (void*, const char*, double) = nullptr;
        int   (*setInt) (void*, const char*, int) = nullptr;
        void* (*newSynth) (void*) = nullptr;
        void  (*deleteSynth) (void*) = nullptr;
        int   (*sfload) (void*, const char*, int) = nullptr;
        int   (*noteOn) (void*, int, int, int) = nullptr;
        int   (*noteOff) (void*, int, int) = nullptr;
        int   (*cc) (void*, int, int, int) = nullptr;
        int   (*programChange) (void*, int, int) = nullptr;
        int   (*pitchBend) (void*, int, int) = nullptr;
        int   (*channelPressure) (void*, int, int) = nullptr;
        int   (*keyPressure) (void*, int, int, int) = nullptr;
        int   (*allSoundsOff) (void*, int) = nullptr;
        int   (*allNotesOff) (void*, int) = nullptr;
        int   (*writeFloat) (void*, int, void*, int, int, void*, int, int) = nullptr;
        void* (*setLogFunction) (int, void*, void*) = nullptr;
        bool ok = false;
    };

    const Fluid& fluid()
    {
        static Fluid f = []
        {
            Fluid r;
            void* lib = nullptr;
            for (auto* name : { "libfluidsynth.so.3", "libfluidsynth.so.2", "libfluidsynth.so" })
                if ((lib = dlopen (name, RTLD_NOW | RTLD_LOCAL)) != nullptr) break;
            if (lib == nullptr) return r;
            auto get = [lib] (auto& fn, const char* sym) { fn = reinterpret_cast<std::remove_reference_t<decltype (fn)>> (dlsym (lib, sym)); return fn != nullptr; };
            r.ok = get (r.newSettings, "new_fluid_settings") && get (r.deleteSettings, "delete_fluid_settings")
                && get (r.setNum, "fluid_settings_setnum") && get (r.setInt, "fluid_settings_setint")
                && get (r.newSynth, "new_fluid_synth") && get (r.deleteSynth, "delete_fluid_synth")
                && get (r.sfload, "fluid_synth_sfload") && get (r.noteOn, "fluid_synth_noteon")
                && get (r.noteOff, "fluid_synth_noteoff") && get (r.cc, "fluid_synth_cc")
                && get (r.programChange, "fluid_synth_program_change") && get (r.pitchBend, "fluid_synth_pitch_bend")
                && get (r.channelPressure, "fluid_synth_channel_pressure") && get (r.keyPressure, "fluid_synth_key_pressure")
                && get (r.allSoundsOff, "fluid_synth_all_sounds_off") && get (r.allNotesOff, "fluid_synth_all_notes_off")
                && get (r.writeFloat, "fluid_synth_write_float");
            // Its own messages go nowhere: a missing preset is not worth a line on stderr per note.
            if (r.ok && get (r.setLogFunction, "fluid_set_log_function"))
                for (int level = 0; level <= 4; ++level) r.setLogFunction (level, nullptr, nullptr);
            return r;
        }();
        return f;
    }

    /* One synth per soundfont kept for the life of the process: FluidSynth
       shares sample data between synths that load the same file, so while
       this one holds it, a new song's synth loads in milliseconds. */
    std::mutex keeperMutex;
    std::unique_ptr<GmSynth> keeper;
    String keeperPath;
}

bool GmSynth::isAvailable() { return fluid().ok; }

File GmSynth::findSoundfont (const File& preferred)
{
    if (preferred.existsAsFile()) return preferred;
    for (auto* path : { "/usr/share/sounds/sf2/FluidR3_GM.sf2",       // Debian, Ubuntu
                        "/usr/share/soundfonts/FluidR3_GM.sf2",       // Fedora, Arch
                        "/usr/share/soundfonts/FluidR3_GM2-2.sf2",
                        "/usr/share/sounds/sf2/default-GM.sf2",
                        "/usr/share/soundfonts/default.sf2",
                        "/usr/share/sounds/sf2/TimGM6mb.sf2" })
        if (File (path).existsAsFile()) return File (path);
    return {};
}

String GmSynth::unavailableReason (const File& preferred)
{
    if (! isAvailable())
        return "the General MIDI player needs FluidSynth (install libfluidsynth3, or fluidsynth-libs on Fedora)";
    if (findSoundfont (preferred) == File())
        return "no General MIDI soundfont found (install fluid-soundfont-gm, or choose one under Options)";
    return {};
}

std::unique_ptr<GmSynth> GmSynth::create (double sampleRate, const File& soundfont, String& error)
{
    const auto& f = fluid();
    if (! f.ok) { error = unavailableReason(); return nullptr; }
    if (! soundfont.existsAsFile()) { error = "soundfont not found: " + soundfont.getFullPathName(); return nullptr; }

    std::unique_ptr<GmSynth> s (new GmSynth());
    s->settings = f.newSettings();
    if (s->settings == nullptr) { error = "FluidSynth could not start"; return nullptr; }
    f.setNum (s->settings, "synth.sample-rate", sampleRate);
    f.setNum (s->settings, "synth.gain", 0.5);
    f.setInt (s->settings, "synth.polyphony", 256);
    f.setInt (s->settings, "synth.cpu-cores", 1);
    f.setInt (s->settings, "synth.threadsafe-api", 0);        // only the audio thread plays it
    f.setInt (s->settings, "synth.dynamic-sample-loading", 0);
    s->synth = f.newSynth (s->settings);
    if (s->synth == nullptr) { error = "FluidSynth could not start"; return nullptr; }

    {
        // Under the keeper's lock, so two songs loading at once share one read of the file.
        const std::lock_guard<std::mutex> l (keeperMutex);
        if (f.sfload (s->synth, soundfont.getFullPathName().toRawUTF8(), 1) < 0)
        {
            error = soundfont.getFileName() + " is not a soundfont FluidSynth can read";
            return nullptr;
        }
        if (keeperPath != soundfont.getFullPathName())
        {
            keeper.reset();
            std::unique_ptr<GmSynth> k (new GmSynth());
            k->settings = f.newSettings();
            k->synth = k->settings != nullptr ? f.newSynth (k->settings) : nullptr;
            if (k->synth != nullptr && f.sfload (k->synth, soundfont.getFullPathName().toRawUTF8(), 1) >= 0)
            {
                keeper = std::move (k);
                keeperPath = soundfont.getFullPathName();
            }
        }
    }
    s->scratch.setSize (2, 8192);
    return s;
}

GmSynth::~GmSynth()
{
    const auto& f = fluid();
    if (synth != nullptr)    f.deleteSynth (synth);
    if (settings != nullptr) f.deleteSettings (settings);
}

void GmSynth::handle (const MidiMessage& m)
{
    const auto& f = fluid();
    const int ch = m.getChannel() - 1;
    if (ch < 0 || ch > 15) return;
    if (m.isNoteOn())                 f.noteOn (synth, ch, m.getNoteNumber(), m.getVelocity());
    else if (m.isNoteOff())           f.noteOff (synth, ch, m.getNoteNumber());
    else if (m.isController())        f.cc (synth, ch, m.getControllerNumber(), m.getControllerValue());
    else if (m.isProgramChange())     f.programChange (synth, ch, m.getProgramChangeNumber());
    else if (m.isPitchWheel())        f.pitchBend (synth, ch, m.getPitchWheelValue());
    else if (m.isChannelPressure())   f.channelPressure (synth, ch, m.getChannelPressureValue());
    else if (m.isAftertouch())        f.keyPressure (synth, ch, m.getNoteNumber(), m.getAfterTouchValue());
}

void GmSynth::silence()
{
    const auto& f = fluid();
    f.allNotesOff (synth, -1);
    f.allSoundsOff (synth, -1);
}

void GmSynth::renderAdding (const MidiBuffer& midi, float* left, float* right, int numSamples, float startGain, float endGain)
{
    const auto& f = fluid();
    auto* l = scratch.getWritePointer (0);
    auto* r = scratch.getWritePointer (1);
    const int cap = scratch.getNumSamples();
    auto it = midi.cbegin();
    int done = 0;
    while (done < numSamples)
    {
        // Every event due by here, then audio up to the next one: notes land on their sample.
        while (it != midi.cend() && (*it).samplePosition <= done) { handle ((*it).getMessage()); ++it; }
        int upTo = numSamples;
        if (it != midi.cend()) upTo = jmin (upTo, (*it).samplePosition);
        const int len = jmin (upTo - done, cap);
        if (len <= 0) continue;
        f.writeFloat (synth, len, l, 0, 1, r, 0, 1);
        // The fade, ramped across the block.
        const float g0 = startGain + (endGain - startGain) * (float) done / (float) numSamples;
        const float step = (endGain - startGain) / (float) numSamples;
        for (int i = 0; i < len; ++i)
        {
            const float g = g0 + step * (float) i;
            left[done + i]  += l[i] * g;
            right[done + i] += r[i] * g;
        }
        done += len;
    }
}

const char* GmSynth::programName (int p)
{
    static const char* const names[128] = {
        "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet",
        "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
        "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
        "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar", "Muted Guitar", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
        "Acoustic Bass", "Fingered Bass", "Picked Bass", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
        "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
        "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
        "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
        "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
        "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
        "Square Lead", "Sawtooth Lead", "Calliope Lead", "Chiff Lead", "Charang Lead", "Voice Lead", "Fifths Lead", "Bass + Lead",
        "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad", "Bowed Pad", "Metallic Pad", "Halo Pad", "Sweep Pad",
        "Rain", "Soundtrack", "Crystal", "Atmosphere", "Brightness", "Goblins", "Echoes", "Sci-fi",
        "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
        "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
        "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause", "Gunshot" };
    return p >= 0 && p < 128 ? names[p] : "";
}

} // namespace perf
