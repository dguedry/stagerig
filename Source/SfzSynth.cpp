#include "SfzSynth.h"
#include "DecentSampler.h"

#include <filesystem>
#include <sfizz.hpp>
#include <sfizz_message.h>

namespace perf
{
using namespace juce;

SfzSynth::SfzSynth() = default;
SfzSynth::~SfzSynth() = default;

namespace
{
    /* The instrument words, in the order they are tried: the first that is
       in a name says what it is. A word written " tom" must start a word,
       " bass " must be one; others may sit anywhere ("SalamanderGrandPiano").
       Order settles the overlaps: a bass drum is a drum before it is a bass,
       a bass recorder a woodwind, a steel drum a mallet instrument. */
    struct Rule { const char* category; std::initializer_list<const char*> words; };
    const Rule rules[] = {
        { "Drums & Percussion", { "cowbell", "agogo" } },
        { "Pianos & Keys",      { "piano", "harpsichord", "clavi", "celest", "rhodes", "wurli", " ep ", "e piano", "spinet", "harmonium",
                                  " keys", "virginal", "mellotron", "toy piano" } },
        { "Organs",             { "organ", "hammond", " b3 ", "leslie", "farfisa" } },
        { "Bass",               { "bass guitar", "electric bass", "upright bass", "double bass", "acoustic bass", "fretless", "synth bass",
                                  "bass synth", " contrabass ", "sub bass", "slap bass", "finger bass", "picked bass", " basses " } },
        { "Guitars",            { "guitar", "ukulele", " uke", "banjo", "mandolin", " lute", "sitar", "dobro", "strumstick", "bouzouki", " oud ",
                                  "charango", "balalaika" } },
        { "Strings",            { "violin", "viola", "cello", "string", " harp", "fiddle", "zither", "psaltery", "dulcimer", "koto", "dan tranh",
                                  "erhu", "hurdy", "kantele", "guzheng", "nyckelharpa" } },
        { "Woodwinds",          { "flute", "piccolo", "recorder", "clarinet", "oboe", "bassoon", " sax", "whistle", "ocarina", "pan pipe",
                                  "panpipe", " fife", "harmonica", "accordion", "concertina", "melodica", "kazoo", "shawm", "bagpipe", "didgeridoo",
                                  "duduk", "shakuhachi", "woodwind", "english horn", "cor anglais" } },
        { "Brass",              { "trumpet", "trombone", " horn", "alphorn", "tuba", "cornet", "flugel", "bugle", "euphonium", "sousaphone",
                                  " brass", "mellophone" } },
        { "Mallets & Bells",    { "marimba", "xylophone", "vibraphone", " vibes", "glockenspiel", "bell", "chime", "balafon", "kalimba", "mbira",
                                  "nyunga", "gamelan", "crotale", "steel drum", "steelpan", "steel pan", "music box", "gong", "tingsha", "handpan",
                                  "hang drum", "mark tree", "glass", "tubular", "singing bowl", "metallophone", "carillon" } },
        { "Drums & Percussion", { "drum", "snare", "kick", " tom", "cymbal", " hat", "hihat", "conga", "bongo", "cajon", "djembe", "tabla",
                                  "darbuka", "timpani", "tambourine", "shaker", "cabasa", "maraca", "clave", "guiro", "woodblock", "block",
                                  "triangle", "anvil", "castanet", " frame", "rattle", "clap", "perc", "bodhran", " udu", "washboard", "spoons",
                                  "ratchet", " whip", "slapstick", "vibraslap", "rainstick", "thunder", "flexatone", "siren", " kit " } },
        { "Synths",             { "synth", "tx81z", " fm ", " pad", " lead", "moog", " arp ", "analog", "juno", "jupiter", "prophet", " dx7" } },
        { "Voices",             { "choir", "vocal", "voice", " aah", " ooh", " vox ", "singer" } },
        { "Bass",               { " bass " } },
    };

    /* Words that say how an instrument is played, not which it is. */
    const char* const styleWords[] = {
        "staccato", "stac", "stacc", "spiccato", "sustain", "sustained", "sus", "susvib", "vib", "vibrato", "nonvib", "legato", "keyswitch", "ks",
        "normal", "soft", "hard", "medium", "loud", "accented", "accent", "handvib", "tremolo", "trem", "gliss", "glissando", "fx", "legacy",
        "long", "short", "pizz", "pizzicato", "arco", "mute", "muted", "open", "closed", "mallet", "mallets", "traditional", "individual",
        "stroke", "roll", "rolls", "hit", "hits", "plucked", "bowed", "damped", "sticks", "brushes", "rods", "hands", "finger", "fingers",
        "slap", "pedal", "release", "dry", "wet", "close", "far", "room", "stereo", "mono", "flutter", "growl", "harmonics", "and", "with",
        "no", "main", "all", "full", "lite", "light", "single", "double", "velocity", "vel", "quiet", "pluck", "plucks", "bow", "bows",
        "longbow", "lute", "buff", "coupled", "stop", "stops", "principal", "ft" };

    // " grand piano steinway b ": lower case, one space between words and at either end.
    String wordsOf (const String& name)
    {
        String out;
        for (auto c : name.toLowerCase())
            out << (CharacterFunctions::isLetterOrDigit (c) ? String::charToString (c) : String (" "));
        return " " + out.trim().replace ("  ", " ").replace ("  ", " ").replace ("  ", " ") + " ";
    }

    File canonical (const File& f)
    {
        std::error_code ec;
        const auto c = std::filesystem::canonical (f.getFullPathName().toStdString(), ec);
        return ec ? f : File (String (c.string()));
    }
}

StringArray SfzSynth::categories()
{
    return { "Pianos & Keys", "Organs", "Guitars", "Bass", "Strings", "Woodwinds", "Brass", "Mallets & Bells",
             "Drums & Percussion", "Synths", "Voices", "Other" };
}

String SfzSynth::categoryOf (const String& name)
{
    const auto words = wordsOf (name);
    for (const auto& r : rules)
        for (const auto* w : r.words)
            if (words.contains (w)) return r.category;
    return "Other";
}

void SfzSynth::splitStyle (const String& name, String& family, String& style)
{
    family = name.trim();
    style.clear();
    const int dash = family.lastIndexOf (" - ");
    if (dash <= 0) return;
    const auto tail = family.substring (dash + 3).trim();
    const auto words = StringArray::fromTokens (wordsOf (tail), " ", "");
    bool allStyle = false;
    for (const auto& w : words)
    {
        if (w.isEmpty()) continue;
        bool known = w.containsOnly ("0123456789");      // an organ's or a harpsichord's stop: 4', 8'
        for (const auto* s : styleWords) known = known || w == s;
        if (! known) return;               // "FM Piano": a sound of its own
        allStyle = true;
    }
    if (! allStyle) return;
    style = tail;
    family = family.substring (0, dash).trim();
}

int SfzSynth::styleRank (const String& style)
{
    const auto s = style.toLowerCase();
    if (s.isEmpty()) return 0;
    if (s == "normal" || s == "sustain" || s == "sus" || s == "main") return 1;
    if (s.contains ("keyswitch") || s == "ks") return 3;     // for those who know what a keyswitch is
    return 2;
}

Array<File> SfzSynth::libraries (const File& settingsFolder)
{
    Array<File> out;
    if (settingsFolder == File()) return out;
    StringArray lines;
    lines.addLines (settingsFolder.getChildFile ("Sample libraries.txt").loadFileAsString());
    for (auto l : lines)
        if (l.trim().isNotEmpty() && File::isAbsolutePath (l.trim())) out.addIfNotAlreadyThere (File (l.trim()));
    return out;
}

bool SfzSynth::addLibrary (const File& settingsFolder, const File& folder)
{
    auto all = libraries (settingsFolder);
    if (all.contains (folder)) return true;
    all.add (folder);
    StringArray lines;
    for (const auto& f : all) lines.add (f.getFullPathName());
    return settingsFolder.createDirectory() && settingsFolder.getChildFile ("Sample libraries.txt").replaceWithText (lines.joinIntoString ("\n") + "\n");
}

void SfzSynth::removeLibrary (const File& settingsFolder, const File& folder)
{
    auto all = libraries (settingsFolder);
    all.removeAllInstancesOf (folder);
    StringArray lines;
    for (const auto& f : all) lines.add (f.getFullPathName());
    settingsFolder.getChildFile ("Sample libraries.txt").replaceWithText (lines.isEmpty() ? String() : lines.joinIntoString ("\n") + "\n");
}

Array<SfzSynth::Instrument> SfzSynth::installedInstruments (const File& settingsFolder)
{
    Array<Instrument> found;
    Array<File> seen;
    const auto home = File::getSpecialLocation (File::userHomeDirectory);
    /* Shared folders hold several libraries, one a folder; a chosen library is one itself. */
    struct Root { File dir; bool isLibrary; };
    std::vector<Root> roots;
    for (const auto& d : { File ("/usr/share/stagerig/instruments"), File ("/usr/local/share/stagerig/instruments"),
                           File ("/usr/share/sounds/sfz"), home.getChildFile (".local/share/sounds/sfz"),
                           settingsFolder != File() ? settingsFolder.getChildFile ("Instruments") : File() })
        roots.push_back ({ d, false });
    for (const auto& d : libraries (settingsFolder)) roots.push_back ({ d, true });

    for (const auto& root : roots)
    {
        if (! root.dir.isDirectory()) continue;
        // A few folders deep, not the whole tree: an instrument's samples sit in folders below its .sfz.
        std::function<void (const File&, int, const String&)> look = [&] (const File& d, int depth, const String& library)
        {
            /* SFZ files and Decent Sampler presets. A library that comes in
               both lists each instrument once, as its SFZ file. */
            auto files = d.findChildFiles (File::findFiles, false, "*.sfz;*.dspreset");
            std::stable_sort (files.begin(), files.end(), [] (const File& a, const File& b)
                              { return a.hasFileExtension ("sfz") && ! b.hasFileExtension ("sfz"); });
            StringArray here;
            for (const auto& f : files)
            {
                if (here.contains (f.getFileNameWithoutExtension(), true)) continue;
                // A preset that is all synthesizer or drawing (no samples) is not one to offer.
                if (f.hasFileExtension ("dspreset") && ! f.loadFileAsString().contains ("<sample")) continue;
                here.add (f.getFileNameWithoutExtension());
                const auto real = canonical (f);
                if (seen.contains (real)) continue;
                seen.add (real);
                Instrument i;
                i.name = f.getFileNameWithoutExtension();
                i.file = f;
                splitStyle (i.name, i.family, i.style);
                i.category = categoryOf (i.family);
                i.library = library.isNotEmpty() ? library : d.getFileName();
                found.add (i);
            }
            if (depth > 0)
                for (const auto& sub : d.findChildFiles (File::findDirectories, false))
                    look (sub, depth - 1, library.isNotEmpty() ? library : sub.getFileName());
        };
        look (root.dir, 4, root.isLibrary ? root.dir.getFileName() : String());
    }
    std::sort (found.begin(), found.end(), [] (const Instrument& a, const Instrument& b)
    {
        if (const int c = a.family.compareNatural (b.family, false)) return c < 0;
        if (const int c = a.library.compareNatural (b.library, false)) return c < 0;
        if (styleRank (a.style) != styleRank (b.style)) return styleRank (a.style) < styleRank (b.style);
        return a.style.compareNatural (b.style, false) < 0;
    });
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
    if (instrument.hasFileExtension ("dspreset"))
    {
        /* A Decent Sampler preset, as the SFZ that plays the same; its
           samples are found from the preset's own folder. */
        const auto sfz = DecentSampler::toSfz (instrument, error);
        if (sfz.isEmpty()) return nullptr;
        s->synth->loadSfzString (instrument.getFullPathName().toStdString(), sfz.toStdString());
    }
    else if (! s->synth->loadSfzFile (instrument.getFullPathName().toStdString()))
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

std::vector<SfzSynth::Control> SfzSynth::controls() const
{
    std::vector<Control> found;
    if (synth == nullptr) return found;
    /* The values are asked for the way sfizz's own window asks: a message to
       the engine, answered at once into `heard`. */
    struct Heard { float value = 0.0f; };
    Heard heard;
    auto client = sfz::Sfizz::createClient (&heard);
    sfz::Sfizz::setReceiveCallback (*client, [] (void* data, int, const char*, const char* sig, const sfizz_arg_t* args)
    {
        if (sig != nullptr && sig[0] == 'f' && args != nullptr) static_cast<Heard*> (data)->value = args[0].f;
    });
    for (const auto& [cc, label] : synth->getCCLabels())
    {
        if (label.empty()) continue;
        heard.value = 0.0f;
        synth->sendMessage (*client, 0, ("/cc" + std::to_string ((int) cc) + "/value").c_str(), "", nullptr);
        found.push_back ({ (int) cc, String::fromUTF8 (label.c_str()), jlimit (0.0f, 1.0f, heard.value) });
    }
    std::sort (found.begin(), found.end(), [] (const Control& a, const Control& b) { return a.cc < b.cc; });
    return found;
}

void SfzSynth::setControl (int cc, float value)
{
    if (synth != nullptr) synth->hdcc (0, cc, jlimit (0.0f, 1.0f, value));
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
