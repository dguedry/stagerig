#include "MappingSuggestions.h"
#include <regex>
#include <set>
#include <limits>

using namespace juce;

namespace perf
{

//==============================================================================
// Templates
//==============================================================================
MappingTemplates::MappingTemplates (const File& storage) : file (storage)
{
    load();
}

bool MappingTemplates::has (const PluginDescription& d) const
{
    return entries.find (keyFor (d)) != entries.end();
}

std::vector<MappingDef> MappingTemplates::get (const PluginDescription& d) const
{
    auto it = entries.find (keyFor (d));
    return it == entries.end() ? std::vector<MappingDef>() : it->second.mappings;
}

void MappingTemplates::set (const PluginDescription& d, const std::vector<MappingDef>& mappings)
{
    Entry e;
    e.pluginName = d.name;
    for (auto m : mappings)
    {
        m.slot = 0; m.effect = -1;     // targets are meaningless in a template
        e.mappings.push_back (m);
    }
    entries[keyFor (d)] = std::move (e);
    save();
}

void MappingTemplates::remove (const PluginDescription& d)
{
    entries.erase (keyFor (d));
    save();
}

Result MappingTemplates::save() const
{
    auto* root = new DynamicObject();
    root->setProperty ("format", "stagerig-mapping-templates");
    root->setProperty ("version", 1);
    auto* plugins = new DynamicObject();
    for (auto& [key, e] : entries)
    {
        auto* o = new DynamicObject();
        o->setProperty ("pluginName", e.pluginName);
        Array<var> arr;
        for (auto& m : e.mappings) arr.add (m.toVar());
        o->setProperty ("mappings", arr);
        plugins->setProperty (key, var (o));
    }
    root->setProperty ("plugins", var (plugins));

    if (! file.getParentDirectory().createDirectory())
        return Result::fail ("Could not create " + file.getParentDirectory().getFullPathName());
    TemporaryFile temp (file);
    {
        FileOutputStream out (temp.getFile());
        if (! out.openedOk()) return Result::fail ("Could not write " + file.getFullPathName());
        JSON::writeToStream (out, var (root));
    }
    return temp.overwriteTargetFileWithTemporary() ? Result::ok() : Result::fail ("Could not replace " + file.getFullPathName());
}

Result MappingTemplates::load()
{
    entries.clear();
    if (! file.existsAsFile()) return Result::ok();
    var parsed;
    auto r = JSON::parse (file.loadFileAsString(), parsed);
    if (r.failed()) return r;
    auto* root = parsed.getDynamicObject();
    const auto format = root != nullptr ? root->getProperty ("format").toString() : String();
    if (root == nullptr || (format != "stagerig-mapping-templates" && format != "performer-mapping-templates"))
        return Result::fail ("Not a mapping template file");
    if (auto* plugins = root->getProperty ("plugins").getDynamicObject())
        for (auto& prop : plugins->getProperties())
        {
            Entry e;
            if (auto* o = prop.value.getDynamicObject())
            {
                e.pluginName = o->getProperty ("pluginName").toString();
                if (auto* arr = o->getProperty ("mappings").getArray())
                    for (auto& mv : *arr) e.mappings.push_back (MappingDef::fromVar (mv));
            }
            entries[prop.name.toString()] = std::move (e);
        }
    return Result::ok();
}

//==============================================================================
int findParamIndex (const ParamInfoList& params, const String& paramId)
{
    for (auto& p : params)
        if (p.id == paramId) return p.index;
    if (paramId.containsOnly ("0123456789"))
    {
        const int idx = paramId.getIntValue();
        for (auto& p : params)
            if (p.index == idx) return idx;
    }
    return -1;
}

//==============================================================================
// Name heuristics
//==============================================================================
namespace
{
    struct Rule
    {
        int cc;
        const char* label;
        std::vector<const char*> patterns;   // in priority order; matched against lowercase names
    };

    // General MIDI Level 2 sound controllers first, then the two generic effect controls.
    const std::vector<Rule>& rules()
    {
        static const std::vector<Rule> r {
            { 74, "Cutoff",       { R"(\bcutoff\b|\bcut off\b)", R"(\bbrightness\b)", R"(filter.*(freq|frequency|cut))", R"(\b(lpf|vcf)\b.*(freq|cut))", R"(\bcutoff)" } },
            { 71, "Resonance",    { R"(\bresonance\b)", R"(\breso\b)", R"(filter.*\bq\b)", R"(\bemphasis\b)", R"(\bres\b)" } },
            { 73, "Attack",       { R"(\b(amp|vca|env|eg1?|adsr|envelope)\b.*attack)", R"(\battack\b)", R"(\batt\b)" } },
            { 75, "Decay",        { R"(\b(amp|vca|env|eg1?|adsr|envelope)\b.*decay)", R"(\bdecay\b)", R"(\bdec\b)" } },
            { 70, "Sustain level",{ R"(\b(amp|vca|env|eg1?|adsr|envelope)\b.*sustain)", R"(\bsustain (level|lvl)\b)", R"(^sustain$)" } },
            { 72, "Release",      { R"(\b(amp|vca|env|eg1?|adsr|envelope)\b.*release)", R"(\brelease\b)", R"(\brel\b)" } },
            { 76, "Vibrato/LFO rate",  { R"(vib(rato)?.*(rate|speed|freq))", R"(\blfo ?1?\b.*(rate|speed|freq))", R"(\blfo\b.*(rate|speed))" } },
            { 77, "Vibrato/LFO depth", { R"(vib(rato)?.*(depth|amount|amt))", R"(\blfo ?1?\b.*(depth|amount|amt|level))", R"(\blfo\b.*(depth|amount))" } },
            {  7, "Volume",       { R"(^(master |main |output )?(volume|vol|level|gain|output)( (level|gain|volume))?$)", R"(\bmaster\b.*(vol|level|gain))", R"(\bvolume\b)", R"(\boutput\b.*(level|gain))", R"(^(level|gain)$)" } },
            { 10, "Pan",          { R"(\bpan(ning|orama)?\b)" } },
            { 91, "Reverb",       { R"(reverb.*(mix|amount|amt|level|send|wet|depth))", R"(\breverb\b)", R"(\bverb\b)" } },
            { 93, "Chorus",       { R"(chorus.*(mix|amount|amt|level|depth|wet))", R"(\bchorus\b)" } },
            { 94, "Detune",       { R"(\bdetune\b)" } },
            {  5, "Portamento",   { R"(portamento|\bglide\b|\bporta\b)" } },
            { 12, "Drive",        { R"(\bdrive\b|distort|saturat|overdrive|\bdist\b)" } },
            { 13, "Delay mix",    { R"(delay.*(mix|amount|amt|level|wet|send))" } },
        };
        return r;
    }

    bool isMappable (const ParamInfo& p)
    {
        return p.automatable && ! p.discrete && ! p.boolean;
    }
}

std::vector<MappingSuggestion> suggestMappingsFromNames (const ParamInfoList& plugin)
{
    struct Candidate { const ParamInfo* param; String name, lower; };
    std::vector<Candidate> params;
    for (auto& p : plugin)
        if (isMappable (p))
        {
            auto name = p.name.trim();
            if (name.isEmpty()) continue;
            params.push_back ({ &p, name, name.toLowerCase() });
        }

    std::vector<MappingSuggestion> out;
    std::set<const ParamInfo*> used;

    for (auto& rule : rules())
    {
        Candidate* best = nullptr;
        int bestScore = std::numeric_limits<int>::max();
        for (size_t pi = 0; pi < rule.patterns.size(); ++pi)
        {
            std::regex re (rule.patterns[pi], std::regex::icase);
            for (auto& c : params)
            {
                if (used.count (c.param) != 0) continue;
                if (! std::regex_search (c.lower.toStdString(), re)) continue;
                // Earlier patterns win; among equals prefer the shorter (more specific) name.
                const int score = (int) pi * 1000 + c.name.length();
                if (score < bestScore) { bestScore = score; best = &c; }
            }
            if (best != nullptr) break;
        }
        if (best == nullptr) continue;

        used.insert (best->param);
        MappingSuggestion s;
        s.mapping.source = MappingDef::Source::CC;
        s.mapping.number = rule.cc;
        s.mapping.paramId = best->param->id;
        s.mapping.paramName = best->name;
        s.reason = "name looks like " + String (rule.label) + " (standard controller " + String (rule.cc) + ")";
        out.push_back (std::move (s));
    }
    return out;
}

//==============================================================================
std::vector<MappingSuggestion> suggestMappingsFromTemplate (const ParamInfoList& params, const std::vector<MappingDef>& tmpl)
{
    std::vector<MappingSuggestion> out;
    for (auto& m : tmpl)
    {
        const int idx = findParamIndex (params, m.paramId);
        if (idx < 0) continue;
        const ParamInfo* p = nullptr;
        for (auto& c : params) if (c.index == idx) p = &c;
        if (p == nullptr) continue;
        MappingSuggestion s;
        s.mapping = m;
        s.mapping.paramName = p->name;
        s.reason = "From the saved template";
        out.push_back (std::move (s));
    }
    return out;
}

std::vector<MappingSuggestion> suggestMappings (const ParamInfoList& params, const PluginDescription& desc,
                                                const MappingTemplates& templates, int slot, int effect,
                                                const std::vector<MappingDef>& existing)
{
    auto suggestions = templates.has (desc) ? suggestMappingsFromTemplate (params, templates.get (desc))
                                            : suggestMappingsFromNames (params);

    std::vector<MappingSuggestion> out;
    for (auto& s : suggestions)
    {
        s.mapping.slot = slot;
        s.mapping.effect = effect;
        bool clash = false;
        for (auto& e : existing)
            if (e.slot == slot && e.effect == effect
                && ((e.source == s.mapping.source && e.number == s.mapping.number) || e.paramId == s.mapping.paramId))
                clash = true;
        if (! clash) out.push_back (s);
    }
    return out;
}

} // namespace perf
