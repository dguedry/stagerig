#include "DecentSampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include <map>
#include <vector>

namespace perf
{
using namespace juce;

namespace
{
    /* The levels a sample's settings come from, nearest first: the sample, its group, the groups. */
    using Levels = std::vector<const XmlElement*>;

    String inherited (const Levels& levels, const char* name)
    {
        for (const auto* e : levels)
            if (e != nullptr && e->hasAttribute (name)) return e->getStringAttribute (name).trim();
        return {};
    }

    /* "0.5" is a gain, "-3dB" decibels; either way a gain. */
    double gainOf (const String& v)
    {
        if (v.isEmpty()) return 1.0;
        if (v.endsWithIgnoreCase ("db")) return Decibels::decibelsToGain (v.dropLastCharacters (2).trim().getDoubleValue(), -200.0);
        return jmax (0.0, v.getDoubleValue());
    }

    String number (double v)
    {
        auto s = String (v, 4);
        while (s.containsChar ('.') && (s.endsWithChar ('0') || s.endsWithChar ('.'))) s = s.dropLastCharacters (1);
        return s == "-0" ? String ("0") : s;
    }

    StringArray tagsOf (const XmlElement* e)
    {
        StringArray t;
        if (e != nullptr) t.addTokens (e->getStringAttribute ("tags"), ",", "");
        t.trim();
        t.removeEmptyStrings();
        return t;
    }

    /* What one of the preset's knobs does to the envelope or a volume. */
    struct Binding
    {
        String parameter;               // AMP_VOLUME, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE
        String level;                   // instrument, group, tag
        int group = -1;                 // level group: which
        StringArray tags;               // level tag: which
        double value = 0.0;             // where the knob starts, in the parameter's units
        bool live = false;              // moved by a controller (a straight line: base + cc * range)
        double base = 0.0, range = 0.0;
        int cc = -1;

        bool reaches (int groupIndex, const StringArray& regionTags) const
        {
            if (level == "instrument") return true;
            if (level == "group") return group == groupIndex;
            if (level == "tag")
                for (const auto& t : tags) if (regionTags.contains (t)) return true;
            return false;
        }
    };

    struct Knob { String label; int cc = -1; double start = 0.0; };

    double fromTable (const String& table, double x)
    {
        std::vector<std::pair<double, double>> points;
        for (auto pair : StringArray::fromTokens (table, ";", ""))
            if (const auto xy = StringArray::fromTokens (pair, ",", ""); xy.size() == 2)
                points.push_back ({ xy[0].getDoubleValue(), xy[1].getDoubleValue() });
        if (points.empty()) return x;
        if (x <= points.front().first) return points.front().second;
        for (size_t i = 1; i < points.size(); ++i)
            if (x <= points[i].first)
            {
                const auto [x0, y0] = points[i - 1];
                const auto [x1, y1] = points[i];
                return x1 > x0 ? y0 + (x - x0) / (x1 - x0) * (y1 - y0) : y1;
            }
        return points.back().second;
    }

    /* The knobs on the preset's screen that move the envelope or a volume. */
    void readKnobs (const XmlElement* ui, std::vector<Binding>& bindings, std::vector<Knob>& knobs, int& nextCc, const std::set<int>& used)
    {
        if (ui == nullptr) return;
        for (auto* e : ui->getChildIterator())
        {
            const auto tag = e->getTagName().toLowerCase();
            if (tag == "tab" || tag == "group" || tag == "panel") { readKnobs (e, bindings, knobs, nextCc, used); continue; }
            if (! e->hasAttribute ("value") || ! (tag.contains ("knob") || tag.contains ("slider") || tag == "control")) continue;

            const double lo = e->getDoubleAttribute ("minValue", 0.0), hi = e->getDoubleAttribute ("maxValue", 1.0);
            const double v = e->getDoubleAttribute ("value", lo);
            const double norm = hi > lo ? jlimit (0.0, 1.0, (v - lo) / (hi - lo)) : 0.0;
            int knobCc = -1;
            for (auto* b : e->getChildWithTagNameIterator ("binding"))
            {
                if (b->getStringAttribute ("type") != "amp" || ! b->getBoolAttribute ("enabled", true)) continue;
                Binding x;
                x.parameter = b->getStringAttribute ("parameter").toUpperCase();
                if (! StringArray { "AMP_VOLUME", "ENV_ATTACK", "ENV_DECAY", "ENV_SUSTAIN", "ENV_RELEASE" }.contains (x.parameter)) continue;
                x.level = b->getStringAttribute ("level", "instrument").toLowerCase();
                x.group = b->getIntAttribute ("position", b->getIntAttribute ("groupIndex", -1));
                x.tags.addTokens (b->getStringAttribute ("tags", b->getStringAttribute ("identifier")), ",", "");
                x.tags.trim();
                x.tags.removeEmptyStrings();

                const auto translation = b->getStringAttribute ("translation", "linear").toLowerCase();
                if (translation == "table")
                    x.value = fromTable (b->getStringAttribute ("translationTable"), v);
                else if (translation == "linear")
                {
                    double outLo = lo, outHi = hi;
                    if (b->hasAttribute ("translationOutputMin") || b->hasAttribute ("translationOutputMax"))
                    {
                        outLo = b->getDoubleAttribute ("translationOutputMin", 0.0);
                        outHi = b->getDoubleAttribute ("translationOutputMax", 1.0);
                    }
                    if (b->getBoolAttribute ("translationReversed", false)) std::swap (outLo, outHi);
                    x.value = outLo + norm * (outHi - outLo);
                    x.live = hi > lo;
                    x.base = outLo;
                    x.range = outHi - outLo;
                }
                else continue;      // fixed values belong to buttons and menus

                if (x.live)
                {
                    while (knobCc < 0 && nextCc <= 119 && used.count (nextCc) != 0) ++nextCc;
                    if (knobCc < 0 && nextCc <= 119)
                    {
                        knobCc = nextCc++;
                        knobs.push_back ({ e->getStringAttribute ("label", "Control " + String (knobs.size() + 1)), knobCc, norm });
                    }
                    x.cc = knobCc;
                    x.live = knobCc >= 0;
                }
                bindings.push_back (x);
            }
        }
    }

    /* Controllers the preset already listens to, so a knob's controller is not one of them. */
    void usedControllers (const XmlElement& e, std::set<int>& used)
    {
        for (int i = 0; i < e.getNumAttributes(); ++i)
        {
            const auto n = e.getAttributeName (i);
            for (auto prefix : { "loCC", "hiCC", "onLoCC", "onHiCC" })
                if (n.startsWith (prefix) && n.substring ((int) std::strlen (prefix)).containsOnly ("0123456789"))
                    used.insert (n.substring ((int) std::strlen (prefix)).getIntValue());
        }
        if (e.hasTagName ("cc")) used.insert (e.getIntAttribute ("number", -1));
        for (auto* c : e.getChildIterator()) usedControllers (*c, used);
    }
}

String DecentSampler::toSfz (const File& preset, String& error)
{
    const auto xml = XmlDocument::parse (preset);
    if (xml == nullptr || ! xml->hasTagName ("DecentSampler"))
    {
        error = preset.getFileName() + " could not be read as a Decent Sampler instrument";
        return {};
    }
    const auto* groups = xml->getChildByName ("groups");
    if (groups == nullptr)
    {
        error = preset.getFileName() + " has no samples in it";
        return {};
    }

    std::set<int> used;
    usedControllers (*xml, used);
    int nextCc = 102;       // 102-119 are the undefined controllers; not the ones the preset listens to
    std::vector<Binding> bindings;
    std::vector<Knob> knobs;
    readKnobs (xml->getChildByName ("ui"), bindings, knobs, nextCc, used);

    std::map<String, double> tagGain;
    if (const auto* tags = xml->getChildByName ("tags"))
        for (auto* t : tags->getChildWithTagNameIterator ("tag"))
            tagGain[t->getStringAttribute ("name")] = gainOf (t->getStringAttribute ("volume"));

    String out;
    out << "// Made by StageRig from " << preset.getFileName() << ", a Decent Sampler instrument\n";
    if (! knobs.empty())
    {
        out << "<control>\n";
        for (const auto& k : knobs)
            if (k.cc >= 0)
                out << "label_cc" << k.cc << "=" << k.label.replaceCharacters ("\n\r=", "   ") << "\n"
                    << "set_hdcc" << k.cc << "=" << number (k.start) << "\n";
    }
    // Decent Sampler's velocity is a straight line: half the velocity, half the gain.
    out << "<global>\namp_velcurve_0=0\namp_velcurve_127=1\n";

    int regions = 0, groupIndex = -1;
    for (auto* group : groups->getChildWithTagNameIterator ("group"))
    {
        ++groupIndex;
        if (! group->getBoolAttribute ("enabled", true)) continue;

        // A round robin with no length given is as long as its last position.
        int seqCount = 0;
        for (auto* s : group->getChildWithTagNameIterator ("sample"))
            seqCount = jmax (seqCount, inherited ({ s, group }, "seqPosition").getIntValue());

        for (auto* s : group->getChildWithTagNameIterator ("sample"))
        {
            const auto path = s->getStringAttribute ("path").replaceCharacter ('\\', '/').trim();
            if (path.isEmpty()) continue;
            const Levels levels { s, group, groups };
            // A background sound (tape hiss, room noise) that plays with no key down: not an instrument's, left out.
            if (inherited (levels, "trigger").equalsIgnoreCase ("continuous")) continue;
            StringArray tags = tagsOf (group);
            tags.mergeArray (tagsOf (s));

            // Volume: the three levels multiply, as do the tags'.
            double gains[3] = { gainOf (s->getStringAttribute ("volume")), gainOf (group->getStringAttribute ("volume")),
                                gainOf (groups->getStringAttribute ("volume")) };
            double tagged = 1.0;
            for (const auto& t : tags) if (auto it = tagGain.find (t); it != tagGain.end()) tagged *= it->second;

            String region;
            region << "<region>";
            const Binding* liveVolume = nullptr;
            String envelope[4] = { inherited (levels, "attack"), inherited (levels, "decay"), inherited (levels, "sustain"), inherited (levels, "release") };
            String envelopeCc;
            static const char* envNames[4] = { "attack", "decay", "sustain", "release" };
            for (const auto& b : bindings)
            {
                if (! b.reaches (groupIndex, tags)) continue;
                if (b.parameter == "AMP_VOLUME")
                {
                    double& slot = b.level == "instrument" ? gains[2] : b.level == "group" ? gains[1] : tagged;
                    if (b.live && liveVolume == nullptr) { liveVolume = &b; slot = 1.0; }
                    else slot = jmax (0.0, b.value);
                    continue;
                }
                const int which = b.parameter == "ENV_ATTACK" ? 0 : b.parameter == "ENV_DECAY" ? 1 : b.parameter == "ENV_SUSTAIN" ? 2 : 3;
                const double scale = which == 2 ? 100.0 : 1.0;
                if (b.live)
                {
                    envelope[which] = number (b.base);
                    envelopeCc << " ampeg_" << envNames[which] << "_oncc" << b.cc << "=" << number (b.range * scale);
                }
                else envelope[which] = number (b.value);
            }

            double gain = gains[0] * gains[1] * gains[2] * tagged;
            if (liveVolume != nullptr)
            {
                // The knob's straight line, kept within sfizz's 0-100%: its top is the gain, its bottom a part of it.
                const double top = jmax (liveVolume->base, liveVolume->base + liveVolume->range);
                if (top > 0.0)
                {
                    gain *= top;
                    region << " amplitude=" << number (liveVolume->base / top * 100.0)
                           << " amplitude_oncc" << liveVolume->cc << "=" << number (liveVolume->range / top * 100.0);
                }
                else gain = 0.0;
            }
            region << " volume=" << number (gain > 0.0 ? Decibels::gainToDecibels (gain, -144.0) : -144.0);

            // Where it plays.
            const auto root = s->getStringAttribute ("rootNote").trim();
            const auto loNote = s->getStringAttribute ("loNote", "0").trim(), hiNote = s->getStringAttribute ("hiNote", "127").trim();
            region << " lokey=" << loNote << " hikey=" << hiNote << " pitch_keycenter=" << (root.isNotEmpty() ? root : loNote);
            region << " lovel=" << s->getStringAttribute ("loVel", "0").trim() << " hivel=" << s->getStringAttribute ("hiVel", "127").trim();

            const double tuning = groups->getDoubleAttribute ("globalTuning", 0.0) + group->getDoubleAttribute ("groupTuning", 0.0)
                                + s->getDoubleAttribute ("tuning", 0.0);
            if (std::abs (tuning) > 1.0e-6)
            {
                const int semitones = roundToInt (tuning);
                region << " transpose=" << semitones << " tune=" << number ((tuning - semitones) * 100.0);
            }
            if (const auto pan = inherited (levels, "pan"); pan.isNotEmpty()) region << " pan=" << number (pan.getDoubleValue());
            if (const auto t = inherited (levels, "pitchKeyTrack"); t.isNotEmpty()) region << " pitch_keytrack=" << number (t.getDoubleValue() * 100.0);
            if (const auto t = inherited (levels, "ampVelTrack"); t.isNotEmpty()) region << " amp_veltrack=" << number (t.getDoubleValue() * 100.0);

            if (const auto trigger = inherited (levels, "trigger").toLowerCase(); StringArray { "release", "first", "legato" }.contains (trigger))
                region << " trigger=" << trigger;

            // Only when a controller is in range, or played by one (pedal-down noises).
            static const std::pair<const char*, const char*> conditions[] = { { "loCC", "locc" }, { "hiCC", "hicc" }, { "onLoCC", "on_locc" }, { "onHiCC", "on_hicc" } };
            for (const auto* e : { (const XmlElement*) groups, (const XmlElement*) group, (const XmlElement*) s })
                for (int i = 0; i < e->getNumAttributes(); ++i)
                {
                    const auto n = e->getAttributeName (i);
                    const auto v = e->getAttributeValue (i).trim();
                    for (const auto& [from, to] : conditions)
                        if (n.startsWith (from) && n.substring ((int) std::strlen (from)).containsOnly ("0123456789") && n.length() > (int) std::strlen (from))
                            region << " " << to << n.substring ((int) std::strlen (from)) << "=" << v;
                }

            // Round robins.
            const auto mode = inherited (levels, "seqMode").toLowerCase();
            const int position = jmax (1, inherited (levels, "seqPosition").getIntValue());
            const int length = inherited (levels, "seqLength").getIntValue() > 0 ? inherited (levels, "seqLength").getIntValue() : jmax (1, seqCount);
            if (mode == "round_robin" && length > 1)
                region << " seq_length=" << length << " seq_position=" << jmin (position, length);
            else if ((mode == "random" || mode == "true_random") && length > 1)
                region << " lorand=" << number ((position - 1) / (double) length) << " hirand=" << number (position / (double) length);

            // The part of the file, and its loop.
            if (s->hasAttribute ("start")) region << " offset=" << s->getStringAttribute ("start").trim();
            if (s->hasAttribute ("end")) region << " end=" << s->getStringAttribute ("end").trim();
            const auto loop = inherited ({ s, group }, "loopEnabled").toLowerCase();
            if (loop == "true")
            {
                region << " loop_mode=loop_continuous";
                if (s->hasAttribute ("loopStart")) region << " loop_start=" << s->getStringAttribute ("loopStart").trim();
                if (s->hasAttribute ("loopEnd")) region << " loop_end=" << s->getStringAttribute ("loopEnd").trim();
                // In frames there, seconds here; at the usual 44.1 kHz.
                if (const auto x = inherited ({ s, group }, "loopCrossfade").getDoubleValue(); x > 0.0)
                    region << " loop_crossfade=" << number (x / 44100.0);
            }
            else if (loop == "false") region << " loop_mode=no_loop";

            // The envelope, or none: the sample plays to its end.
            if (inherited (levels, "ampEnvEnabled").toLowerCase() == "false")
            {
                if (loop != "true") region << " loop_mode=one_shot";
            }
            else
            {
                for (int i = 0; i < 4; ++i)
                    if (envelope[i].isNotEmpty())
                        region << " ampeg_" << envNames[i] << "=" << number (envelope[i].getDoubleValue() * (i == 2 ? 100.0 : 1.0));
                region << envelopeCc;
            }

            // Last, alone on its line: a path may have spaces in it.
            out << region << "\nsample=" << path << "\n";
            ++regions;
        }
    }

    if (regions == 0)
    {
        error = preset.getFileName() + " has no samples StageRig can play";
        return {};
    }
    return out;
}

Array<File> DecentSampler::unpackLibraries (const File& folder)
{
    Array<File> made;
    std::function<void (const File&, int)> look = [&] (const File& d, int depth)
    {
        for (const auto& f : d.findChildFiles (File::findFiles, false, "*.dslibrary"))
        {
            const auto into = f.getSiblingFile (f.getFileNameWithoutExtension());
            if (into.exists()) continue;
            ZipFile zip (f);
            if (zip.getNumEntries() == 0) continue;
            if (zip.uncompressTo (into, false).wasOk()) made.add (into);
            else into.deleteRecursively();
        }
        if (depth > 0)
            for (const auto& sub : d.findChildFiles (File::findDirectories, false)) look (sub, depth - 1);
    };
    look (folder, 3);
    return made;
}

} // namespace perf
