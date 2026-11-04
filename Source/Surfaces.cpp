#include "Surfaces.h"
#include "PropertyExchange.h"
#include "PanelParams.h"
#include "RemotePlugin.h"
#include <cstring>
#include <chrono>
#include <limits>

#if JUCE_LINUX || JUCE_BSD
 #include <cerrno>
 #include <cstring>
 #include <fcntl.h>
 #include <poll.h>
 #include <unistd.h>
#endif

namespace perf
{
using namespace juce;

//==============================================================================
SurfaceHub::SurfaceHub (Engine& e, ControlMapLibrary& lib, PropertiesFile& s, bool lookForKeyboards)
    : engine (e), library (lib), settings (s), searching (lookForKeyboards)
{
    engine.addListener (this);
    if (searching) startTimerHz (30);
}

SurfaceHub::~SurfaceHub()
{
    stopTimer();
    engine.armSurfaceLearn (-1);
    engine.removeListener (this);
    surfaces.clear();     // each puts its keyboard back as it found it
}

uint32 SurfaceHub::instrumentColour (int slot)
{
    static const uint32 colours[] = { 0xff8a00, 0x00b4ff, 0x3cdc3c, 0xff3cc8, 0xffdc00, 0x5a64ff, 0xff3c3c, 0x00e6c8 };
    return colours[(size_t) (jmax (0, slot) % 8)];
}

uint32 SurfaceHub::dim (uint32 c, float amount) const
{
    auto ch = [amount] (uint32 v) { return (uint32) roundToInt ((float) (v & 0xff) * amount); };
    return (ch (c >> 16) << 16) | (ch (c >> 8) << 8) | ch (c);
}

//==============================================================================
SurfaceHub::Layout SurfaceHub::layoutFor (int input, int program, int slot, bool automatic) const
{
    Layout layout;
    const auto& inputs = engine.getSetup().inputs;
    if (input < 0 || input >= (int) inputs.size() || program < 0 || program >= InputDef::numPrograms) return layout;
    const auto& prog = inputs[(size_t) input].programs[(size_t) program];
    if (slot < 0 || slot >= (int) prog.slots.size()) return layout;
    const auto& sd = prog.slots[(size_t) slot];
    if (! sd.isPlugin() || ! engine.isPluginAlive (input, program, slot)) return layout;
    auto* plugin = engine.getPlugin (input, program, slot);
    if (plugin == nullptr) return layout;
    const auto& params = plugin->getParameters();

    auto fromSetup = [&] (const ControlMap& map)
    {
        for (const auto& mp : map.pages)
        {
            Page page;
            page.name = mp.name;
            for (int k = 0; k < SurfaceView::numKnobs && k < (int) mp.knobs.size(); ++k)
            {
                const auto& mk = mp.knobs[(size_t) k];
                int idx = mk.paramId.isNotEmpty() ? plugin->findParameterIndex (mk.paramId) : -1;
                if (idx < 0 && mk.paramName.isNotEmpty())
                    for (const auto& p : params)
                        if (p.name.equalsIgnoreCase (mk.paramName)) { idx = p.index; break; }
                page.knobs[(size_t) k].paramIndex = idx;
                page.knobs[(size_t) k].name = mk.label.isNotEmpty() ? mk.label : idx >= 0 ? params[(size_t) idx].name : mk.paramName;
            }
            layout.pages.push_back (std::move (page));
        }
    };
    if (! automatic)
        if (const auto* m = library.find (sd.plugin, sd.controlMap)) layout.map = *m;
    if (layout.map.has_value() && ! layout.map->pages.empty())
    {
        fromSetup (*layout.map);
        return layout;
    }
    // No setup of ours: the maker's pages, if its NKS presets have them.
    if (const auto* nks = library.findNks (sd.plugin))
    {
        const auto made = controlMapFromNks (*nks, sd.plugin, params);
        if (! made.pages.empty())
        {
            fromSetup (made);
            layout.map.reset();
            layout.fromNks = true;
            return layout;
        }
    }

    /* No setup: the front panel's knobs first, as the Stage view has them,
       then the rest of what a player could turn, eight at a time. */
    std::vector<int> first;
    if (sd.panelParams.empty()) first = pickPanelParams (params, SurfaceView::numKnobs);
    else for (const auto& id : sd.panelParams)
        if (const int idx = plugin->findParameterIndex (id); idx >= 0 && (int) first.size() < SurfaceView::numKnobs) first.push_back (idx);

    auto addPage = [&] (const String& name, const std::vector<int>& indices)
    {
        Page page;
        page.name = name;
        for (int k = 0; k < (int) indices.size() && k < SurfaceView::numKnobs; ++k)
            page.knobs[(size_t) k] = { indices[(size_t) k], params[(size_t) indices[(size_t) k]].name };
        layout.pages.push_back (std::move (page));
    };
    if (! first.empty()) addPage ("Front panel", first);

    std::vector<int> rest;
    for (const auto& p : params)
    {
        if (! p.automatable || p.midiController >= 0 || p.name.containsIgnoreCase ("bypass")) continue;
        if (std::find (first.begin(), first.end(), p.index) != first.end()) continue;
        rest.push_back (p.index);
        if ((int) rest.size() == SurfaceView::numKnobs)
        {
            addPage ("Parameters " + String ((int) layout.pages.size() + 1), rest);
            rest.clear();
            if (layout.pages.size() >= 16) break;
        }
    }
    if (! rest.empty() && layout.pages.size() < 16) addPage ("Parameters " + String ((int) layout.pages.size() + 1), rest);
    return layout;
}

int SurfaceHub::getFocusSlot (int input)
{
    const auto& inputs = engine.getSetup().inputs;
    if (input < 0 || input >= (int) inputs.size()) return -1;
    const auto& in = inputs[(size_t) input];
    const int program = in.currentProgram;
    const auto& prog = in.programs[(size_t) program];
    auto& f = focus[input];
    const bool stillThere = f.program == program && f.slot >= 0 && f.slot < (int) prog.slots.size() && prog.slots[(size_t) f.slot].isPlugin();
    if (! stillThere)
    {
        // A new program: the instrument that has a setup, else one its maker gave pages, else its first plugin.
        int pick = -1;
        for (int s = 0; s < (int) prog.slots.size() && pick < 0; ++s)
            if (prog.slots[(size_t) s].isPlugin() && library.find (prog.slots[(size_t) s].plugin, prog.slots[(size_t) s].controlMap) != nullptr) pick = s;
        for (int s = 0; s < (int) prog.slots.size() && pick < 0; ++s)
            if (prog.slots[(size_t) s].isPlugin() && library.findNks (prog.slots[(size_t) s].plugin) != nullptr) pick = s;
        for (int s = 0; s < (int) prog.slots.size() && pick < 0; ++s)
            if (prog.slots[(size_t) s].isPlugin()) pick = s;
        if (f.program != program || f.slot != pick) layouts.erase (input);
        f = { program, pick, 0 };
    }
    return f.slot;
}

int SurfaceHub::getFocusPage (int input)
{
    getFocusSlot (input);
    return focus[input].page;
}

const SurfaceHub::Layout& SurfaceHub::currentLayout (int input)
{
    static const Layout none;
    if (input < 0 || input >= (int) engine.getSetup().inputs.size()) return none;
    const int slot = getFocusSlot (input);
    auto it = layouts.find (input);
    if (it == layouts.end())
    {
        const auto& inputs = engine.getSetup().inputs;
        it = layouts.emplace (input, layoutFor (input, inputs[(size_t) input].currentProgram, slot)).first;
        auto& f = focus[input];
        f.page = jlimit (0, jmax (0, (int) it->second.pages.size() - 1), f.page);
    }
    return it->second;
}

SurfaceView SurfaceHub::viewFor (int input)
{
    SurfaceView v;
    const auto& inputs = engine.getSetup().inputs;
    if (input < 0 || input >= (int) inputs.size()) return v;
    const auto& in = inputs[(size_t) input];
    const int program = in.currentProgram;
    const auto& prog = in.programs[(size_t) program];
    v.inputName = in.name;
    v.channel = in.channel;
    v.program = program;
    v.programName = prog.name.isNotEmpty() ? prog.name : "Program " + String (program).paddedLeft ('0', 3);
    for (int p = 0; p < InputDef::numPrograms; ++p)
        if (p == program || ! in.programs[(size_t) p].isEmpty())
            v.programs.emplace_back (p, in.programs[(size_t) p].name.isNotEmpty() ? in.programs[(size_t) p].name : "Program " + String (p).paddedLeft ('0', 3));
    for (int k = 0; k < SurfaceView::numKnobs; ++k)
        v.knobControllers[(size_t) k] = in.controls.knobCC[(size_t) k] > 0 ? in.controls.knobCC[(size_t) k] : SurfaceControls::autoKnobCC + k;
    // A spare controller a knob was taught goes to the taught knob: the untaught one is told the next spare instead.
    for (int k = 0; k < SurfaceView::numKnobs; ++k)
    {
        if (in.controls.knobCC[(size_t) k] > 0) continue;
        auto taken = [&] (int cc) { for (int j = 0; j < SurfaceView::numKnobs; ++j) if (j != k && v.knobControllers[(size_t) j] == cc) return true; return false; };
        for (int cc = SurfaceControls::autoKnobCC + k; taken (v.knobControllers[(size_t) k]) && cc < 120; ++cc)
            if (! taken (cc)) v.knobControllers[(size_t) k] = cc;
    }
    v.bankFirst = program - program % SurfaceView::numKnobs;
    for (int i = 0; i < SurfaceView::numKnobs; ++i)
    {
        const int p = v.bankFirst + i;
        if (p < InputDef::numPrograms && ! in.programs[(size_t) p].isEmpty())
            v.bankNames[(size_t) i] = in.programs[(size_t) p].name.isNotEmpty() ? in.programs[(size_t) p].name : "Program " + String (p).paddedLeft ('0', 3);
    }
    for (int p = 0; p < InputDef::numPrograms; ++p)
        if (p != program && ! in.programs[(size_t) p].isEmpty())
        {
            (p < program ? v.programBefore : v.programAfter) = true;
            if (p < v.bankFirst) v.bankBefore = true;
            if (p >= v.bankFirst + SurfaceView::numKnobs) v.bankAfter = true;
        }

    const int slot = getFocusSlot (input);
    v.page = getFocusPage (input);
    const auto& layout = currentLayout (input);
    for (int s = 0; s < (int) prog.slots.size(); ++s)
        if (prog.slots[(size_t) s].isPlugin())
        {
            if (s == slot) v.focusPlugin = v.pluginNames.size();
            v.pluginNames.add (prog.slots[(size_t) s].plugin.name);
        }
    if (slot >= 0)
    {
        const auto& sd = prog.slots[(size_t) slot];
        v.instrument = sd.plugin.name;
        if (layout.map.has_value() && ! layout.map->name.equalsIgnoreCase (sd.plugin.name)) v.instrument << ": " << layout.map->name;
    }
    v.pageCount = (int) layout.pages.size();
    v.page = jlimit (0, jmax (0, v.pageCount - 1), v.page);
    if (v.page < v.pageCount)
    {
        const auto& page = layout.pages[(size_t) v.page];
        v.pageName = page.name;
        auto* plugin = slot >= 0 && engine.isPluginAlive (input, program, slot) ? engine.getPlugin (input, program, slot) : nullptr;
        for (int k = 0; k < SurfaceView::numKnobs; ++k)
        {
            const auto& knob = page.knobs[(size_t) k];
            v.knobNames[(size_t) k] = knob.name;
            if (plugin != nullptr && knob.paramIndex >= 0 && knob.paramIndex < (int) plugin->getParameters().size())
            {
                const float value = plugin->getCachedParameterValue (knob.paramIndex);
                const auto& p = plugin->getParameters()[(size_t) knob.paramIndex];
                v.knobLevels[(size_t) k] = jlimit (0.0f, 1.0f, value);
                v.knobValues[(size_t) k] = p.boolean || p.numSteps == 2 ? String (value >= 0.5f ? "On" : "Off")
                                                                        : String (roundToInt (value * 100.0f)) + "%";
            }
        }
    }

    /* The keys: each instrument's range in its colour, dimly, and on it a
       setup's keyswitches and ranges at full strength. The transpose moves a
       key before the range is checked, so the lights move with it and always
       mark the keys to press. */
    const int t = engine.getTranspose();
    for (int s = 0; s < (int) prog.slots.size(); ++s)
    {
        const auto& sd = prog.slots[(size_t) s];
        if (! sd.enabled) continue;
        const uint32 c = dim (instrumentColour (s), 0.22f);
        for (int key = jmax (0, sd.lowKey - t); key <= jmin (127, sd.highKey - t); ++key)
            if (v.keyColours[(size_t) key] == 0) v.keyColours[(size_t) key] = c;
    }
    for (int s = 0; s < (int) prog.slots.size(); ++s)
    {
        const auto& sd = prog.slots[(size_t) s];
        if (! sd.enabled || ! sd.isPlugin()) continue;
        if (const auto* map = library.find (sd.plugin, sd.controlMap))
            for (const auto& z : map->keys)
                for (int note = z.low; note <= z.high; ++note)
                {
                    const int key = note - sd.transpose - t;
                    if (key >= 0 && key <= 127 && key + t >= sd.lowKey && key + t <= sd.highKey)
                        v.keyColours[(size_t) key] = z.colour & 0xffffff;
                }
    }
    return v;
}

//==============================================================================
void SurfaceHub::turnKnob (int input, int knob, float delta)
{
    if (input < 0 || input >= (int) engine.getSetup().inputs.size()) return;
    if (knob >= 0 && knob < SurfaceView::numKnobs) lastTurn[input] = { knob, Time::getMillisecondCounter() };
    // The instrument and page first: settling them can drop the layout the reference below points into.
    const int slot = getFocusSlot (input), page = getFocusPage (input);
    const auto& layout = currentLayout (input);
    if (page >= (int) layout.pages.size() || knob < 0 || knob >= SurfaceView::numKnobs || slot < 0) return;
    const int idx = layout.pages[(size_t) page].knobs[(size_t) knob].paramIndex;
    const int program = engine.getSetup().inputs[(size_t) input].currentProgram;
    if (idx < 0 || ! engine.isPluginAlive (input, program, slot)) return;
    auto* plugin = engine.getPlugin (input, program, slot);
    if (plugin == nullptr) return;

    /* From where the knob last put it, while that is recent: the plugin's
       own report of the value comes back a moment later, and turning from
       it would lose the steps in between. */
    auto& last = lastSet[{ input, knob }];
    const auto now = Time::getMillisecondCounter();
    const float from = last.paramIndex == idx && now - last.at < 1500 ? last.value : plugin->getCachedParameterValue (idx);
    const float value = jlimit (0.0f, 1.0f, from + delta);
    plugin->setParameterValue (idx, value, 50);
    last = { idx, value, now };
    editedSinceTick = true;
}

void SurfaceHub::setKnob (int input, int knob, float value)
{
    if (input < 0 || input >= (int) engine.getSetup().inputs.size()) return;
    if (knob >= 0 && knob < SurfaceView::numKnobs) lastTurn[input] = { knob, Time::getMillisecondCounter() };
    const int program = engine.getSetup().inputs[(size_t) input].currentProgram;
    const int slot = getFocusSlot (input), page = getFocusPage (input);
    const auto& layout = currentLayout (input);
    if (page >= (int) layout.pages.size() || knob < 0 || knob >= SurfaceView::numKnobs || slot < 0) return;
    const int idx = layout.pages[(size_t) page].knobs[(size_t) knob].paramIndex;
    if (idx < 0 || ! engine.isPluginAlive (input, program, slot)) return;
    if (auto* plugin = engine.getPlugin (input, program, slot))
    {
        const float v = jlimit (0.0f, 1.0f, value);
        plugin->setParameterValue (idx, v, 50);
        lastSet[{ input, knob }] = { idx, v, Time::getMillisecondCounter() };
        editedSinceTick = true;
    }
}

void SurfaceHub::doAction (int input, int action)
{
    const auto& inputs = engine.getSetup().inputs;
    if (input < 0 || input >= (int) inputs.size()) return;
    const auto& in = inputs[(size_t) input];
    switch (action)
    {
        case SurfaceControls::programUp:
        case SurfaceControls::programDown:
        {
            // The next program that has something in it: the empty ones between are passed over.
            const int step = action == SurfaceControls::programUp ? 1 : -1;
            for (int p = in.currentProgram + step; p >= 0 && p < InputDef::numPrograms; p += step)
                if (! in.programs[(size_t) p].isEmpty()) { engine.selectProgram (input, p); break; }
            break;
        }
        case SurfaceControls::programBankUp:
        case SurfaceControls::programBankDown:
        {
            // The next eight with something in them, at the same place among them if that one has something, else the first that does.
            constexpr int n = SurfaceView::numKnobs;
            const int step = action == SurfaceControls::programBankUp ? n : -n;
            const int place = in.currentProgram % n;
            for (int first = in.currentProgram - place + step; first >= 0 && first < InputDef::numPrograms; first += step)
            {
                int p = -1;
                if (first + place < InputDef::numPrograms && ! in.programs[(size_t) (first + place)].isEmpty()) p = first + place;
                for (int i = 0; i < n && p < 0 && first + i < InputDef::numPrograms; ++i)
                    if (! in.programs[(size_t) (first + i)].isEmpty()) p = first + i;
                if (p >= 0) { engine.selectProgram (input, p); break; }
            }
            break;
        }
        case SurfaceControls::pageUp:
        case SurfaceControls::pageDown:
        {
            const int count = (int) currentLayout (input).pages.size();
            auto& f = focus[input];
            f.page = jlimit (0, jmax (0, count - 1), f.page + (action == SurfaceControls::pageUp ? 1 : -1));
            break;
        }
        case SurfaceControls::instrumentNext:
        {
            const auto& prog = in.programs[(size_t) in.currentProgram];
            const int now = getFocusSlot (input), n = (int) prog.slots.size();
            for (int i = 1; i <= n; ++i)
            {
                const int s = (now + i) % n;
                if (prog.slots[(size_t) s].isPlugin())
                {
                    if (s != now) { focus[input].slot = s; focus[input].page = 0; layouts.erase (input); }
                    break;
                }
            }
            break;
        }
        default: break;
    }
}

//==============================================================================
String SurfaceHub::describeControl (const String& id)
{
    if (id.startsWith ("cc:"))   return "Controller " + id.substring (3);
    if (id.startsWith ("kk:b"))  return "Komplete Kontrol button " + id.substring (4);
    if (id.startsWith ("lk:n"))  return "Launchkey pad " + id.substring (4);
    if (id.startsWith ("lk:"))   return "Launchkey button " + id.substring (3);
    if (id.startsWith ("ni:"))   return "Komplete Kontrol button " + id.substring (3) + " (DAW)";
    return id;
}

void SurfaceHub::learn (int input, int what, std::function<void (const String&)> done)
{
    learning = { input, what, std::move (done) };
    engine.armSurfaceLearn (input);
}

void SurfaceHub::cancelLearn()
{
    learning = {};
    engine.armSurfaceLearn (-1);
}

void SurfaceHub::surfaceLearned (int input, int cc, int)
{
    if (learning.input != input) return;
    taught ("cc:" + String (cc), cc);
}

void SurfaceHub::taught (const String& id, int cc)
{
    const int input = learning.input, what = learning.what;
    const auto& inputs = engine.getSetup().inputs;
    if (input < 0 || input >= (int) inputs.size()) { cancelLearn(); return; }
    auto c = inputs[(size_t) input].controls;
    const bool knob = what >= 0 && what < SurfaceControls::numKnobs;
    if (knob && cc <= 0) { engine.armSurfaceLearn (input); return; }     // a knob is a controller: a keyboard's button cannot be one, so keep waiting

    // One control, one job: whatever else it did, it stops doing.
    for (auto& k : c.knobCC) if (cc > 0 && k == cc) k = 0;
    for (auto& b : c.buttons) if (b == id) b.clear();
    if (knob) c.knobCC[(size_t) what] = cc;
    else if (what >= 100 && what < 100 + SurfaceControls::numActions) c.buttons[(size_t) (what - 100)] = id;

    engine.setInputControls (input, c);
    auto done = std::move (learning.done);
    cancelLearn();
    if (onEdited) onEdited();
    if (done) done (describeControl (id));
}

void SurfaceHub::surfaceKnob (int input, int knob, int value, bool relative, float exact)
{
    auto& p = pending[{ input, knob }];
    if (relative) p.delta += (float) value / 100.0f;
    else        { p.absolute = exact >= 0.0f ? exact : (float) value / 127.0f; p.delta = 0.0f; }
}

void SurfaceHub::handleTouch (ControlSurface& surface, const ControlSurface::Touch& t)
{
    const int input = inputFor (surface);
    if (input < 0) return;
    switch (t.type)
    {
        case ControlSurface::Touch::knob:
            pending[{ input, t.index }].delta += t.amount;
            break;
        case ControlSurface::Touch::button:
        {
            if (! t.down) break;
            if (learning.input == input && learning.what >= 100) { taught (t.id, 0); break; }
            const auto& buttons = engine.getSetup().inputs[(size_t) input].controls.buttons;
            for (int a = 0; a < SurfaceControls::numActions; ++a)
                if (buttons[(size_t) a] == t.id) { doAction (input, a); break; }
            break;
        }
        case ControlSurface::Touch::action:
            doAction (input, t.index);
            break;
        case ControlSurface::Touch::program:
        {
            const auto& in = engine.getSetup().inputs[(size_t) input];
            const int p = in.currentProgram - in.currentProgram % SurfaceView::numKnobs + t.index;
            if (t.index >= 0 && t.index < SurfaceView::numKnobs && p < InputDef::numPrograms && ! in.programs[(size_t) p].isEmpty() && p != in.currentProgram)
                engine.selectProgram (input, p);
            break;
        }
        case ControlSurface::Touch::instrument:
        {
            const auto& in = engine.getSetup().inputs[(size_t) input];
            const auto& prog = in.programs[(size_t) in.currentProgram];
            for (int s = 0, n = 0; s < (int) prog.slots.size(); ++s)
                if (prog.slots[(size_t) s].isPlugin() && n++ == t.index)
                {
                    if (s != getFocusSlot (input)) { focus[input].slot = s; focus[input].page = 0; layouts.erase (input); }
                    break;
                }
            break;
        }
        case ControlSurface::Touch::knobTouch:
            break;
    }
}

int SurfaceHub::inputFor (const ControlSurface& s) const
{
    const auto& inputs = engine.getSetup().inputs;
    const auto keys = s.getKeysPortName();
    // Its own port first, then one that starts with its name ("KOMPLETE KONTROL S88" for "... MIDI 1"), so a shorter name never takes another keyboard.
    for (int i = 0; i < (int) inputs.size(); ++i)
        if (inputs[(size_t) i].kind == InputDef::Kind::midi && inputs[(size_t) i].midiDeviceName.equalsIgnoreCase (keys)) return i;
    for (int i = 0; i < (int) inputs.size(); ++i)
        if (inputs[(size_t) i].kind == InputDef::Kind::midi && inputs[(size_t) i].midiDeviceName.startsWithIgnoreCase (keys)) return i;
    for (int i = 0; i < (int) inputs.size(); ++i)
        if (inputs[(size_t) i].kind == InputDef::Kind::midi) return i;
    return -1;
}

//==============================================================================
bool SurfaceHub::isKompleteKontrolEnabled() const   { return settings.getBoolValue ("surfaceKompleteKontrol", false); }
void SurfaceHub::setKompleteKontrolEnabled (bool on) { settings.setValue ("surfaceKompleteKontrol", on); lastSearch = 0; }

StringArray SurfaceHub::plainPorts() const
{
    StringArray ports;
    ports.addLines (settings.getValue ("plainMidiKeyboards"));
    ports.removeEmptyStrings();
    return ports;
}

bool SurfaceHub::isPlainKeys (const String& keys) const
{
    // The list holds port names; a driver's keys name is the port's, or the start of it ("KOMPLETE KONTROL S88" of "... MIDI 1").
    for (const auto& p : plainPorts())
        if (p.equalsIgnoreCase (keys) || p.startsWithIgnoreCase (keys)) return true;
    return false;
}

bool SurfaceHub::isPlain (const ControlSurface& s) const   { return isPlainKeys (s.getKeysPortName()); }

void SurfaceHub::setPlainMidi (const String& port, bool plain)
{
    // The MK1's translator is StageRig's own guesswork, so it is off unless asked for: plain is its default.
    if (port.containsIgnoreCase ("KOMPLETE KONTROL S") && ! port.containsIgnoreCase ("MK"))
    {
        setKompleteKontrolEnabled (! plain);
        return;
    }
    auto ports = plainPorts();
    ports.removeString (port, true);
    if (plain) ports.add (port);
    settings.setValue ("plainMidiKeyboards", ports.joinIntoString ("\n"));
    lastSearch = 0;
}

StringArray SurfaceHub::getProblems() const
{
    auto all = problems;
    for (const auto& s : surfaces)
        if (const auto p = s->getProblem(); p.isNotEmpty()) all.add (p);
    return all;
}

std::vector<SurfaceHub::SurfaceInfo> SurfaceHub::getSurfaces() const
{
    std::vector<SurfaceInfo> list;
    for (const auto& s : surfaces)
        if (s->isFound())
            list.push_back ({ s->getName(), inputFor (*s), s->hasScreen(), s->hasLights() });
    return list;
}

std::vector<SurfaceHub::KeyboardInfo> SurfaceHub::getKeyboards() const
{
    std::vector<KeyboardInfo> list;
    const auto ins = MidiInput::getAvailableDevices();
    const auto& inputs = engine.getSetup().inputs;
    auto portFor = [&ins] (const String& keys)
    {
        for (const auto& d : ins) if (d.name.containsIgnoreCase (keys)) return d.name;
        return keys;
    };
    auto inputsOn = [&inputs] (const String& port)
    {
        std::vector<int> on;
        for (int i = 0; i < (int) inputs.size(); ++i)
            if (inputs[(size_t) i].kind == InputDef::Kind::midi && inputs[(size_t) i].midiDeviceName == port) on.push_back (i);
        return on;
    };
    for (const auto& s : surfaces)
    {
        if (! s->isFound()) continue;
        KeyboardInfo k;
        k.name = s->getName().upToLastOccurrenceOf (" (MIDI-CI)", false, false);
        k.port = portFor (s->getKeysPortName());
        k.input = inputFor (*s);
        k.inputs = inputsOn (k.port);
        k.language = s->getLanguage();
        k.screen = s->hasScreen();
        k.lights = s->hasLights();
        k.knobsByThemselves = true;
        k.experimental = dynamic_cast<const KompleteKontrolMk1*> (s.get()) != nullptr;
        list.push_back (k);
    }
    // The rest of the keyboards the inputs play from: plain MIDI.
    const auto mk1s = KompleteKontrolMk1::find();
    for (int i = 0; i < (int) inputs.size(); ++i)
    {
        const auto& in = inputs[(size_t) i];
        if (in.kind != InputDef::Kind::midi || in.midiDeviceName.isEmpty()) continue;
        bool connected = false, listed = false;
        for (const auto& d : ins) if (d.name == in.midiDeviceName) connected = true;
        for (const auto& k : list) if (k.port == in.midiDeviceName) listed = true;
        if (! connected || listed) continue;
        KeyboardInfo k;
        k.name = in.midiDeviceName;
        k.port = in.midiDeviceName;
        k.input = i;
        k.inputs = inputsOn (k.port);
        k.language = "plain MIDI";
        k.plain = plainPorts().contains (in.midiDeviceName);
        k.knobsByThemselves = false;
        for (int cc : in.controls.knobCC) if (cc > 0) k.knobsByThemselves = true;    // taught: they work, in their way
        for (const auto& d : mk1s)
            if (in.midiDeviceName.containsIgnoreCase (String ("KOMPLETE KONTROL ") + d.model->name) && ! in.midiDeviceName.containsIgnoreCase ("MK"))
            {
                k.name = String ("Komplete Kontrol ") + d.model->name + " MK1";
                k.plain = ! isKompleteKontrolEnabled();
                k.more = "its screens, light guide and knobs (experimental: worked out from the keyboard itself)";
            }
        list.push_back (k);
    }
    return list;
}

String SurfaceHub::getReport() const
{
    String r;
    r << "StageRig " << (JUCEApplicationBase::getInstance() != nullptr ? JUCEApplicationBase::getInstance()->getApplicationVersion() : String ("(test)"))
      << " on " << SystemStats::getOperatingSystemName() << "\n\n";
    r << "MIDI inputs:\n";
    for (const auto& d : MidiInput::getAvailableDevices())
        r << "  " << d.name << (Midi2Inputs::isMidi2 (d.identifier) ? "  [MIDI 2.0]" : "") << "\n";
    r << "MIDI outputs:\n";
    for (const auto& d : MidiOutput::getAvailableDevices()) r << "  " << d.name << "\n";
    r << "\nKeyboards:\n";
    const auto& inputs = engine.getSetup().inputs;
    for (const auto& k : getKeyboards())
        r << "  " << k.name << ": " << (k.plain ? String ("plain MIDI (set so)") : k.language)
          << ", plays " << (k.input >= 0 && k.input < (int) inputs.size() ? inputs[(size_t) k.input].name : String ("no input"))
          << (k.screen ? ", screen" : "") << (k.lights ? ", lights" : "") << "\n";
    for (const auto& s : surfaces)
    {
        r << "\n" << s->getName() << " -- " << s->getLanguage() << (s->isFound() ? "" : " (asked; no answer)") << "\n";
        if (const auto more = s->getReport(); more.isNotEmpty()) r << more << "\n";
    }
    for (const auto& p : getProblems()) r << "\nProblem: " << p << "\n";
    return r;
}

void SurfaceHub::addSurfaceForTesting (std::unique_ptr<ControlSurface> s)
{
    surfaces.push_back (std::move (s));
}

void SurfaceHub::lookForKeyboards()
{
    bool changed = false;
    problems.clear();
    for (auto it = surfaces.begin(); it != surfaces.end();)
    {
        auto* kk = dynamic_cast<KompleteKontrolMk1*> (it->get());
        auto* lk = dynamic_cast<LaunchkeyMk4*> (it->get());
        auto* ni = dynamic_cast<KompleteKontrolDaw*> (it->get());
        ignoreUnused (lk, ni);
        /* The standard first: a maker's translator gives way to MIDI-CI on a
           keyboard that answers it. */
        bool standardThere = false;
        if (dynamic_cast<MidiCiKeyboard*> (it->get()) == nullptr)
            for (const auto& s : surfaces)
                if (auto* ci = dynamic_cast<MidiCiKeyboard*> (s.get()); ci != nullptr && ci->isFound()
                        && ci->getKeysPortName().containsIgnoreCase ((*it)->getKeysPortName()))
                    standardThere = true;
        const bool off = (kk != nullptr && ! isKompleteKontrolEnabled()) || isPlain (**it) || standardThere;
        if (off || ! (*it)->isConnected())
        {
            shown.erase (it->get());
            reportedProblems.erase (it->get());
            it = surfaces.erase (it);
            changed = true;
        }
        else ++it;
    }

    if (isKompleteKontrolEnabled())
        for (const auto& d : KompleteKontrolMk1::find())
        {
            bool have = false;
            for (const auto& s : surfaces)
                if (auto* kk = dynamic_cast<KompleteKontrolMk1*> (s.get()); kk != nullptr && kk->getPath() == d.path) have = true;
            if (have) continue;
            String error;
            auto kk = std::make_unique<KompleteKontrolMk1> (d, error);
            if (kk->opened()) { surfaces.push_back (std::move (kk)); changed = true; }
            else problems.add (error);
        }

    auto answersMidiCi = [this] (const String& keys)
    {
        for (const auto& s : surfaces)
            if (auto* ci = dynamic_cast<MidiCiKeyboard*> (s.get()); ci != nullptr && ci->isFound() && ci->getKeysPortName().containsIgnoreCase (keys))
                return true;
        return false;
    };

    for (const auto& p : LaunchkeyMk4::find())
    {
        bool have = false;
        for (const auto& s : surfaces)
            if (auto* lk = dynamic_cast<LaunchkeyMk4*> (s.get()); lk != nullptr && lk->getOutputIdentifier() == p.out.identifier) have = true;
        if (have || isPlainKeys (p.keysName) || answersMidiCi (p.keysName)) continue;
        auto lk = std::make_unique<LaunchkeyMk4> (p);
        if (lk->opened()) { surfaces.push_back (std::move (lk)); changed = true; }
        else problems.add ("Could not open the Launchkey's DAW port, " + p.out.name + ".");
    }

    for (const auto& p : KompleteKontrolDaw::find())
    {
        bool have = false;
        for (const auto& s : surfaces)
            if (auto* ni = dynamic_cast<KompleteKontrolDaw*> (s.get()); ni != nullptr && ni->getOutputIdentifier() == p.out.identifier) have = true;
        if (have || isPlainKeys (p.keysName) || answersMidiCi (p.keysName)) continue;
        auto ni = std::make_unique<KompleteKontrolDaw> (p);
        if (ni->opened()) { surfaces.push_back (std::move (ni)); changed = true; }
        else problems.add ("Could not open the " + p.name + "'s DAW port, " + p.out.name + ".");
    }

    /* Every keyboard an input plays from is asked, once, whether it speaks
       MIDI-CI, the standard: one that answers is talked to that way, and a
       maker's translator above gives way to it (at the next look). */
    {
        StringArray names;
        for (const auto& in : engine.getSetup().inputs)
            if (in.kind == InputDef::Kind::midi && in.midiDeviceName.isNotEmpty()) names.addIfNotAlreadyThere (in.midiDeviceName);
        const auto plain = plainPorts();
        for (const auto& p : MidiCiKeyboard::find (names))
        {
            if (plain.contains (p.in.name)) continue;
            bool have = false;
            for (const auto& s : surfaces)
                if (auto* ci = dynamic_cast<MidiCiKeyboard*> (s.get()); ci != nullptr && ci->getInputIdentifier() == p.in.identifier) have = true;
            if (have) continue;
            auto ci = std::make_unique<MidiCiKeyboard> (p);
            if (ci->opened()) { surfaces.push_back (std::move (ci)); changed = true; }
        }
    }

    // An input whose keyboard is told which controllers its knobs send hears the spare ones as knobs.
    const auto& inputs = engine.getSetup().inputs;
    for (int i = 0; i < (int) inputs.size(); ++i)
    {
        bool told = false;
        for (const auto& s : surfaces)
            if (dynamic_cast<MidiCiKeyboard*> (s.get()) != nullptr && s->isFound()
                && (inputFor (*s) == i || inputs[(size_t) i].midiDeviceName.equalsIgnoreCase (s->getKeysPortName()))) told = true;
        engine.setInputAutoKnobs (i, told);
    }

    if (changed && onSurfacesChanged) onSurfacesChanged();
}

void SurfaceHub::tick()
{
    // The search for NKS presets finished: the knobs may have their makers' pages now.
    if (library.getNksGeneration() != nksGeneration) { nksGeneration = library.getNksGeneration(); layouts.clear(); }

    if (searching && Time::getMillisecondCounter() - lastSearch > 2000)
    {
        lastSearch = Time::getMillisecondCounter();
        lookForKeyboards();
    }

    std::vector<ControlSurface::Touch> touches;
    for (size_t i = 0; i < surfaces.size(); ++i)
    {
        touches.clear();
        surfaces[i]->takeTouches (touches);
        for (const auto& t : touches) handleTouch (*surfaces[i], t);
    }

    auto turns = std::move (pending);
    pending.clear();
    for (const auto& [key, p] : turns)
    {
        if (key.first < 0 || key.first >= (int) engine.getSetup().inputs.size()) continue;
        if (p.absolute >= 0.0f) setKnob (key.first, key.second, p.absolute);
        else if (p.delta != 0.0f) turnKnob (key.first, key.second, p.delta);
    }

    for (auto& s : surfaces)
    {
        const int input = inputFor (*s);
        if (input < 0) continue;
        const auto view = viewFor (input);
        auto it = shown.find (s.get());
        if (it == shown.end() || it->second != view)
        {
            s->show (view);
            shown[s.get()] = view;
        }
    }

    for (auto& s : surfaces)
    {
        const auto p = s->getProblem();
        auto& said = reportedProblems[s.get()];
        if (p != said)
        {
            said = p;
            if (p.isNotEmpty() && onSurfaceProblem) onSurfaceProblem (p);
        }
    }

    if (editedSinceTick)
    {
        editedSinceTick = false;
        if (onEdited) onEdited();
    }
}

//==============================================================================
// Launchkey MK4
//==============================================================================
namespace
{
    bool isLaunchkeyMk4 (const String& name)  { return name.containsIgnoreCase ("Launchkey") && name.containsIgnoreCase ("MK4"); }
    /** The second of its two USB MIDI interfaces: "... DAW In", "... MIDI 2", "MIDIIN2 (...)". */
    bool isDawPort (const String& name)
    {
        return name.containsIgnoreCase ("DAW") || name.trimEnd().endsWith (" 2") || name.containsIgnoreCase ("MIDIIN2") || name.containsIgnoreCase ("MIDIOUT2");
    }

    MidiMessage launchkeySysEx (bool mini, std::initializer_list<uint8> body, const String& text = {})
    {
        std::vector<uint8> d { 0x00, 0x20, 0x29, 0x02, (uint8) (mini ? 0x13 : 0x14) };
        d.insert (d.end(), body);
        for (auto c : text.toStdString())
        {
            const auto u = (uint8) c;
            d.push_back (u >= 0x20 && u <= 0x7e ? u : (uint8) '?');
        }
        return MidiMessage::createSysExMessage (d.data(), (int) d.size());
    }

    /** The screen fits about twenty characters to a line. */
    String fit (const String& s, int chars = 20)   { return s.length() <= chars ? s : s.substring (0, chars - 1).trimEnd() + "."; }

    constexpr uint8 lkEncoder0 = 0x15;              // the eight encoders' controllers, and their displays
    constexpr uint8 lkStationary = 0x20, lkPluginModeName = 0x25;
}

/** What is left of a Launchkey port's name without the part that says which of
    its two ports it is: "Launchkey MK4 49 MIDI 2" and "MIDIIN2 (Launchkey MK4 49)"
    both give "Launchkey MK4 49", so a keyboard's two ports go together, and two
    Launchkeys never get mixed. */
static String launchkeyStem (const String& name)
{
    String s = name;
    if (s.containsIgnoreCase ("MIDIIN2 (") || s.containsIgnoreCase ("MIDIOUT2 ("))
        s = s.fromFirstOccurrenceOf ("(", false, false).upToLastOccurrenceOf (")", false, false);
    for (const char* cut : { " MIDI ", " DAW", " In", " Out" })
        if (s.containsIgnoreCase (cut)) s = s.upToFirstOccurrenceOf (cut, false, true);
    return s.trim();
}

std::vector<LaunchkeyMk4::Ports> LaunchkeyMk4::find()
{
    std::vector<Ports> found;
    const auto ins = MidiInput::getAvailableDevices(), outs = MidiOutput::getAvailableDevices();
    for (const auto& dawIn : ins)
    {
        if (! isLaunchkeyMk4 (dawIn.name) || ! isDawPort (dawIn.name)) continue;
        const auto stem = launchkeyStem (dawIn.name);
        Ports p;
        p.in = dawIn;
        for (const auto& d : outs)
            if (isLaunchkeyMk4 (d.name) && isDawPort (d.name) && launchkeyStem (d.name) == stem) { p.out = d; break; }
        if (p.out.identifier.isEmpty()) continue;
        p.mini = dawIn.name.containsIgnoreCase ("Mini");
        p.keysName = p.mini ? "Launchkey Mini" : "Launchkey";
        for (const auto& d : ins)
            if (isLaunchkeyMk4 (d.name) && ! isDawPort (d.name) && launchkeyStem (d.name) == stem) { p.keysName = d.name; break; }
        found.push_back (p);
    }
    return found;
}

LaunchkeyMk4::LaunchkeyMk4 (const Ports& p) : ports (p)
{
    output = MidiOutput::openDevice (ports.out.identifier);
    input = MidiInput::openDevice (ports.in.identifier, this);
    if (! opened()) { input.reset(); output.reset(); return; }
    input->start();
    send (openingMessages (ports.mini));
}

LaunchkeyMk4::~LaunchkeyMk4()
{
    if (input != nullptr) input->stop();
    if (output != nullptr) send (closingMessages());
}

bool LaunchkeyMk4::isConnected() const
{
    if (! opened()) return false;
    for (const auto& d : MidiOutput::getAvailableDevices())
        if (d.identifier == ports.out.identifier) return true;
    return false;
}

void LaunchkeyMk4::send (const std::vector<MidiMessage>& messages)
{
    if (output == nullptr) return;
    for (const auto& m : messages) output->sendMessageNow (m);
}

std::vector<MidiMessage> LaunchkeyMk4::openingMessages (bool mini)
{
    std::vector<MidiMessage> m;
    m.push_back (MidiMessage (0x9f, 0x0c, 0x7f));       // DAW mode on
    m.push_back (MidiMessage (0xb6, 0x45, 0x7f));       // encoders relative
    m.push_back (MidiMessage (0xb6, 0x47, 0x7f));       // touch events
    m.push_back (MidiMessage (0xb6, 0x1e, 0x02));       // encoders on Plugin
    m.push_back (launchkeySysEx (mini, { 0x04, lkStationary, 0x02 }));          // three lines: title, name, value
    m.push_back (launchkeySysEx (mini, { 0x04, lkPluginModeName, 0x01 }));
    for (uint8 k = 0; k < SurfaceView::numKnobs; ++k)
        m.push_back (launchkeySysEx (mini, { 0x04, (uint8) (lkEncoder0 + k), 0x61 }));   // name and value, shown on a turn and a touch
    return m;
}

std::vector<MidiMessage> LaunchkeyMk4::closingMessages()
{
    return { MidiMessage (0x9f, 0x0c, 0x00) };         // back to standalone
}

std::vector<MidiMessage> LaunchkeyMk4::screenMessages (const SurfaceView& now, const SurfaceView* before, bool mini)
{
    std::vector<MidiMessage> m;
    auto text = [&] (uint8 target, uint8 field, const String& t) { m.push_back (launchkeySysEx (mini, { 0x06, target, field }, t)); };
    const String title = now.inputName + "  " + String (now.program).paddedLeft ('0', 3);
    const String value = now.pageCount > 1 ? now.pageName + "  " + String (now.page + 1) + "/" + String (now.pageCount) : now.instrument;
    const bool fresh = before == nullptr;
    auto oldTitle = fresh ? String() : before->inputName + "  " + String (before->program).paddedLeft ('0', 3);
    auto oldValue = fresh ? String() : before->pageCount > 1 ? before->pageName + "  " + String (before->page + 1) + "/" + String (before->pageCount) : before->instrument;
    if (fresh || title != oldTitle)                        text (lkStationary, 0, fit (title));
    if (fresh || now.programName != before->programName)   text (lkStationary, 1, fit (now.programName));
    if (fresh || value != oldValue)                        text (lkStationary, 2, fit (value));
    if (fresh || now.pageName != before->pageName)         text (lkPluginModeName, 0, fit (now.pageName.isNotEmpty() ? now.pageName : String ("Plugin"), 12));
    for (int k = 0; k < SurfaceView::numKnobs; ++k)
    {
        const auto target = (uint8) (lkEncoder0 + k);
        if (fresh || now.knobNames[(size_t) k] != before->knobNames[(size_t) k])
            text (target, 0, fit (now.knobNames[(size_t) k].isNotEmpty() ? now.knobNames[(size_t) k] : String ("-")));
        if (fresh || now.knobValues[(size_t) k] != before->knobValues[(size_t) k])
            text (target, 1, fit (now.knobValues[(size_t) k]));
    }
    return m;
}

bool LaunchkeyMk4::parse (const MidiMessage& m, Touch& t)
{
    if (m.isController())
    {
        const int ch = m.getChannel(), cc = m.getControllerNumber(), v = m.getControllerValue();
        const bool encoder = cc >= lkEncoder0 && cc < lkEncoder0 + SurfaceView::numKnobs;
        if (ch == 16 && encoder)
        {
            t = { Touch::knob, cc - lkEncoder0, (float) (v - 64) / 100.0f, false, {} };
            return true;
        }
        if (ch == 15 && (encoder || (cc >= 0x55 && cc < 0x55 + SurfaceView::numKnobs)))
        {
            t = { Touch::knobTouch, (encoder ? cc - lkEncoder0 : cc - 0x55), 0.0f, v >= 64, {} };
            return true;
        }
        if (ch == 16 && cc >= 0x05 && cc <= 0x0d) return false;       // the faders (49/61)
        if (ch == 16)
        {
            t = { Touch::button, 0, 0.0f, v >= 64, "lk:" + String (cc) };
            return true;
        }
        return false;
    }
    if (m.getChannel() == 1 && (m.isNoteOn() || m.isNoteOff()))       // the pads, in DAW layout
    {
        t = { Touch::button, 0, 0.0f, m.isNoteOn(), "lk:n" + String (m.getNoteNumber()) };
        return true;
    }
    return false;
}

void LaunchkeyMk4::handleIncomingMidiMessage (MidiInput*, const MidiMessage& m)
{
    Touch t;
    if (! parse (m, t)) return;
    const std::lock_guard<std::mutex> l (touchMutex);
    if (touches.size() < 1024) touches.push_back (t);
}

void LaunchkeyMk4::takeTouches (std::vector<Touch>& out)
{
    const std::lock_guard<std::mutex> l (touchMutex);
    out.insert (out.end(), touches.begin(), touches.end());
    touches.clear();
}

void LaunchkeyMk4::show (const SurfaceView& v)
{
    if (shownOnce && v.sameText (last)) return;
    send (screenMessages (v, shownOnce ? &last : nullptr, ports.mini));
    last = v;
    shownOnce = true;
}

//==============================================================================
// Komplete Kontrol DAW protocol (S MK2/MK3, A, M)
//==============================================================================
namespace
{
    // From ReaKontrol (github.com/jcsteh/reaKontrol, src/niMidi.cpp).
    constexpr uint8 niHeader[] = { 0x00, 0x21, 0x09, 0x00, 0x00, 0x44, 0x43, 0x01, 0x00 };    // after F0
    enum : uint8
    {
        niHello = 0x01, niGoodbye = 0x02, niBankMapping = 0x05,
        niNavTracks = 0x30, niNavBanks = 0x31, niNavPreset = 0x36,
        niTrackAvail = 0x40, niSelTrackParamsChanged = 0x41, niTrackSelected = 0x42, niTrackMuted = 0x43,
        niTrackSoloed = 0x44, niTrackArmed = 0x45, niTrackVolumeText = 0x46, niTrackPanText = 0x47, niTrackName = 0x48,
        niKnobVolume0 = 0x50, niKnobPan0 = 0x58, niChangeVolume = 0x64, niChangePan = 0x65,
        niKnobParam0 = 0x70, niSelectPlugin = 0x70, niPluginNames = 0x71, niParamName = 0x72, niParamValueText = 0x73,
        niParamPage = 0x74, niParamSection = 0x75, niPresetName = 0x76, niParamHighRes = 0x7f
    };

    /** The keyboard's screen takes plain ASCII. */
    void addText (std::vector<uint8>& d, const String& text)
    {
        for (auto c : text.toStdString())
        {
            const auto u = (uint8) c;
            d.push_back (u >= 0x20 && u <= 0x7e ? u : (uint8) '?');
        }
    }

    MidiMessage niSysEx (uint8 command, int value, int index, const std::vector<uint8>& info = {})
    {
        std::vector<uint8> d (std::begin (niHeader), std::end (niHeader));
        d.push_back (command);
        d.push_back ((uint8) jlimit (0, 127, value));
        d.push_back ((uint8) jlimit (0, 127, index));
        d.insert (d.end(), info.begin(), info.end());
        return MidiMessage::createSysExMessage (d.data(), (int) d.size());
    }

    MidiMessage niSysEx (uint8 command, int value, int index, const String& text)
    {
        std::vector<uint8> info;
        addText (info, text);
        return niSysEx (command, value, index, info);
    }

    MidiMessage niCc (uint8 command, int value)    { return MidiMessage::controllerEvent (16, command, jlimit (0, 127, value)); }
    int signedValue (int v)                         { return v <= 63 ? v : v - 128; }
    int lights (bool before, bool after)            { return (before ? 1 : 0) | (after ? 2 : 0); }
}

String KompleteKontrolDaw::modelFor (const String& portName, String& keysName)
{
    // The ports NI's Host Integration makes for an MK2, an A and an M.
    if (portName.containsIgnoreCase ("Komplete Kontrol DAW"))   { keysName = "KOMPLETE KONTROL";   return "Komplete Kontrol (DAW)"; }
    if (portName.containsIgnoreCase ("Komplete Kontrol A DAW")) { keysName = "KOMPLETE KONTROL A"; return "Komplete Kontrol A"; }
    if (portName.containsIgnoreCase ("Komplete Kontrol M DAW")) { keysName = "KOMPLETE KONTROL M"; return "Komplete Kontrol M"; }
    // An MK3's own second port: "KONTROL S61 MK3 - DAW", "MIDIIN2 (KONTROL S61 MK3)", "KONTROL S61 MK3 MIDI 2".
    if (portName.containsIgnoreCase ("KONTROL") && portName.containsIgnoreCase ("MK3") && isDawPort (portName))
    {
        for (auto* size : { "S49", "S61", "S88" })
            if (portName.containsIgnoreCase (size))
            {
                keysName = String (size) + " MK3";
                return "Komplete Kontrol " + keysName;
            }
        keysName = "MK3";
        return "Komplete Kontrol MK3";
    }
    return {};
}

std::vector<KompleteKontrolDaw::Ports> KompleteKontrolDaw::find()
{
    std::vector<Ports> found;
    const auto outs = MidiOutput::getAvailableDevices();
    for (const auto& in : MidiInput::getAvailableDevices())
    {
        String keys;
        const auto name = modelFor (in.name, keys);
        if (name.isEmpty()) continue;
        /* Its output: the one named as it is (Windows says MIDIOUT2 where the
           input says MIDIIN2), or else the first of the same keyboard not taken. */
        const MidiDeviceInfo* pick = nullptr;
        for (int pass = 0; pass < 2 && pick == nullptr; ++pass)
            for (const auto& out : outs)
            {
                String outKeys;
                bool taken = false;
                for (const auto& f : found) taken = taken || f.out.identifier == out.identifier;
                if (taken || modelFor (out.name, outKeys) != name) continue;
                if (pass == 1 || out.name == in.name || out.name.replace ("MIDIOUT", "MIDIIN") == in.name) { pick = &out; break; }
            }
        if (pick != nullptr) found.push_back ({ in, *pick, name, keys });
    }
    return found;
}

KompleteKontrolDaw::KompleteKontrolDaw (const Ports& p) : ports (p)
{
    output = MidiOutput::openDevice (ports.out.identifier);
    input = MidiInput::openDevice (ports.in.identifier, this);
    if (! opened()) { input.reset(); output.reset(); return; }
    input->start();
    send ({ hello() });
}

KompleteKontrolDaw::~KompleteKontrolDaw()
{
    if (input != nullptr) input->stop();
    if (output != nullptr) send ({ goodbye() });
}

bool KompleteKontrolDaw::isConnected() const
{
    if (! opened()) return false;
    for (const auto& d : MidiOutput::getAvailableDevices())
        if (d.identifier == ports.out.identifier) return true;
    return false;
}

void KompleteKontrolDaw::send (const std::vector<MidiMessage>& messages)
{
    if (output == nullptr) return;
    for (const auto& m : messages) output->sendMessageNow (m);
}

MidiMessage KompleteKontrolDaw::hello()     { return niCc (niHello, 4); }
MidiMessage KompleteKontrolDaw::goodbye()   { return niCc (niGoodbye, 0); }

std::vector<MidiMessage> KompleteKontrolDaw::screenMessages (const SurfaceView& now, const SurfaceView* before, const Heard& heard)
{
    const int protocolVersion = heard.version;
    std::vector<MidiMessage> m;
    const bool fresh = before == nullptr;
    const int selected = now.program - now.bankFirst;

    // The programs, as tracks: their names across the screen, the one in play selected.
    const bool programs = fresh || now.program != before->program || now.bankFirst != before->bankFirst || now.bankNames != before->bankNames
                       || now.programName != before->programName;
    if (programs)
    {
        for (int i = 0; i < SurfaceView::numKnobs; ++i)
        {
            const auto& name = now.bankNames[(size_t) i];
            if (name.isEmpty() && i != selected) { m.push_back (niSysEx (niTrackAvail, 0, i)); continue; }
            m.push_back (niSysEx (niTrackAvail, 1, i));
            m.push_back (niSysEx (niTrackName, 0, i, name.isNotEmpty() ? name : now.programName));
            m.push_back (niSysEx (niTrackSelected, i == selected ? 1 : 0, i));
            m.push_back (niSysEx (niTrackMuted, 0, i));
            m.push_back (niSysEx (niTrackSoloed, 0, i));
            m.push_back (niSysEx (niTrackArmed, 0, i));
        }
        m.push_back (niSysEx (niTrackSelected, 1, selected));
        m.push_back (niSysEx (niSelTrackParamsChanged, 0, 0));
        m.push_back (niCc (niNavTracks, lights (now.programBefore, now.programAfter)));
        m.push_back (niCc (niNavPreset, lights (now.programBefore, now.programAfter)));
        m.push_back (niSysEx (niPresetName, 0, 0, now.programName));
    }

    // The instrument and its page of knobs.
    const bool knobs = programs || now.instrument != before->instrument || now.page != before->page || now.pageCount != before->pageCount
                    || now.pageName != before->pageName || now.knobNames != before->knobNames || now.pluginNames != before->pluginNames
                    || now.focusPlugin != before->focusPlugin;
    if (knobs)
    {
        if (protocolVersion >= 4 && ! now.pluginNames.isEmpty())
        {
            std::vector<uint8> names;
            for (int i = 0; i < now.pluginNames.size(); ++i)
            {
                if (i > 0) names.push_back (0);
                addText (names, now.pluginNames[i]);
            }
            m.push_back (niSysEx (niPluginNames, 0, 0, names));
            if (now.focusPlugin >= 0) m.push_back (niSysEx (niSelectPlugin, 0, now.focusPlugin));
        }
        m.push_back (niSysEx (niParamPage, jmax (1, now.pageCount), now.page));
        for (int k = 0; k < SurfaceView::numKnobs; ++k)
        {
            m.push_back (niSysEx (niParamName, 0, k, now.knobNames[(size_t) k]));
            m.push_back (niSysEx (niParamSection, 0, k, now.knobNames[(size_t) k].isNotEmpty() ? now.pageName : String()));
        }
    }
    // The bank arrows: lit for where they go, whichever they are for.
    if (programs || knobs)
        m.push_back (niCc (niNavBanks, heard.banksForPrograms ? lights (now.bankBefore, now.bankAfter)
                                                              : lights (now.page > 0, now.page + 1 < now.pageCount)));

    // The knobs' values: in the plugin view, and beneath each track in the mixer, where the knobs turn them too.
    for (int k = 0; k < SurfaceView::numKnobs; ++k)
    {
        const auto i = (size_t) k;
        if (! knobs && now.knobValues[i] == before->knobValues[i] && now.knobLevels[i] == before->knobLevels[i]) continue;
        const int level = now.knobLevels[i] >= 0.0f ? roundToInt (now.knobLevels[i] * 127.0f) : 0;
        m.push_back (niCc ((uint8) (niKnobParam0 + k), level));
        m.push_back (niSysEx (niParamValueText, 0, k, now.knobValues[i]));
        m.push_back (niCc ((uint8) (niKnobVolume0 + k), level));
        m.push_back (niSysEx (niTrackVolumeText, 0, k, now.knobNames[i].isNotEmpty() ? now.knobNames[i] + " " + now.knobValues[i] : String()));
    }
    return m;
}

bool KompleteKontrolDaw::parse (const MidiMessage& m, Touch& t, Heard& heard)
{
    if (m.isSysEx())
    {
        const auto* d = m.getSysExData();
        const int n = m.getSysExDataSize();
        constexpr int h = (int) sizeof (niHeader);
        if (n < h + 3 || std::memcmp (d, niHeader, (size_t) h) != 0) return false;
        const uint8 command = d[h], value = d[h + 1], index = d[h + 2];
        if (command == niParamHighRes && n >= h + 5 && index < SurfaceView::numKnobs)
        {
            // A turn as fourteen bits, signed (an S MK3): a fraction of the range, as ReaKontrol takes it.
            int change = d[h + 3] | (d[h + 4] << 7);
            if (change > 8192) change -= 16384;
            juce::ignoreUnused (value);
            t = { Touch::knob, index, (float) change / 8191.0f, false, {} };
            return true;
        }
        if (command == niSelectPlugin)
        {
            t = { Touch::instrument, index, 0.0f, true, {} };
            return true;
        }
        return false;
    }
    if (! m.isController() || m.getChannel() != 16) return false;
    const int command = m.getControllerNumber(), value = m.getControllerValue();
    switch (command)
    {
        case niHello:       heard.version = value; return false;
        case niBankMapping: heard.banksForPrograms = value == 0; return false;     // 0: the mixer view
        case niGoodbye:     return false;
        case niNavTracks:
        case niNavPreset:   t = { Touch::action, signedValue (value) > 0 ? SurfaceControls::programUp : SurfaceControls::programDown, 0.0f, true, {} }; return true;
        case niNavBanks:
        {
            const bool up = signedValue (value) > 0;
            const int action = heard.banksForPrograms ? (up ? SurfaceControls::programBankUp : SurfaceControls::programBankDown)
                                                      : (up ? SurfaceControls::pageUp : SurfaceControls::pageDown);
            t = { Touch::action, action, 0.0f, true, {} };
            return true;
        }
        case niTrackSelected:
            if (value >= SurfaceView::numKnobs) return false;
            t = { Touch::program, value, 0.0f, true, {} };
            return true;
        case niChangeVolume:
        case niChangePan:   return false;
        default: break;
    }
    const bool volume = command >= niKnobVolume0 && command < niKnobVolume0 + SurfaceView::numKnobs;
    const bool param = command >= niKnobParam0 && command < niKnobParam0 + SurfaceView::numKnobs;
    if (volume || param)
    {
        t = { Touch::knob, command - (volume ? niKnobVolume0 : niKnobParam0), (float) signedValue (value) / (127.0f * 8.0f), false, {} };
        return true;
    }
    if (command >= niKnobPan0 && command < niKnobPan0 + SurfaceView::numKnobs) return false;
    if (value == 0) return false;      // a button's release, where it sends one
    t = { Touch::button, 0, 0.0f, true, "ni:" + String (command) };
    return true;
}

void KompleteKontrolDaw::handleIncomingMidiMessage (MidiInput*, const MidiMessage& m)
{
    Touch t;
    Heard h { version.load(), banksForPrograms.load() };
    const bool touched = parse (m, t, h);
    version = h.version;
    banksForPrograms = h.banksForPrograms;
    if (! touched) return;
    const std::lock_guard<std::mutex> l (touchMutex);
    if (touches.size() < 1024) touches.push_back (t);
}

void KompleteKontrolDaw::takeTouches (std::vector<Touch>& out)
{
    {
        const std::lock_guard<std::mutex> l (touchMutex);
        out.insert (out.end(), touches.begin(), touches.end());
        touches.clear();
    }
    // The keyboard answered the hello (or said it again, after Host Integration restarted), or changed view: all of it, afresh.
    const Heard h { version.load(), banksForPrograms.load() };
    if ((h.version != shownFor.version || h.banksForPrograms != shownFor.banksForPrograms) && haveView && h.version != 0)
    {
        shownFor = h;
        send (screenMessages (last, nullptr, h));
        shownOnce = true;
    }
}

void KompleteKontrolDaw::show (const SurfaceView& v)
{
    const bool had = haveView;
    const auto before = last;
    last = v;
    haveView = true;
    const Heard h { version.load(), banksForPrograms.load() };
    if (h.version == 0) return;      // not yet answered: all of it goes when it does
    if (shownOnce && had && v.sameText (before)) return;
    send (screenMessages (v, shownOnce && had ? &before : nullptr, h));
    shownFor = h;
    shownOnce = true;
}

//==============================================================================
// Komplete Kontrol MK1
//==============================================================================
const KompleteKontrolMk1::Model* KompleteKontrolMk1::modelFor (int productId)
{
    static const Model models[] = {
        { 0x1340, "S25", 25, 48 },
        { 0x1350, "S49", 49, 36 },
        { 0x1360, "S61", 61, 36 },
        { 0x1410, "S88", 88, 21 },
    };
    for (const auto& m : models)
        if (m.productId == productId) return &m;
    return nullptr;
}

std::vector<uint8> KompleteKontrolMk1::lightReport (const std::array<uint32, 128>& keyColours, const Model& model)
{
    // Report 0x82: three bytes a key, red, green, blue, 0..127, lowest key first.
    std::vector<uint8> r;
    r.reserve (1 + (size_t) model.keys * 3);
    r.push_back (0x82);
    for (int k = 0; k < model.keys; ++k)
    {
        const int note = model.lowestNote + k;
        const uint32 c = note >= 0 && note < 128 ? keyColours[(size_t) note] : 0;
        r.push_back ((uint8) ((c >> 17) & 0x7f));
        r.push_back ((uint8) ((c >> 9) & 0x7f));
        r.push_back ((uint8) ((c >> 1) & 0x7f));
    }
    return r;
}

void KompleteKontrolMk1::compareReports (const uint8* before, const uint8* now, int size, std::vector<Touch>& out)
{
    /* Report 1, after its id: 40 buttons, one bit each (24-31 are touches
       on the eight knobs); a byte of two small values; then the eight knobs,
       16 bits each, counting 0..999 round and round. */
    if (size < 22) return;
    for (int b = 0; b < 40; ++b)
    {
        const bool was = (before[b / 8] >> (b % 8)) & 1, is = (now[b / 8] >> (b % 8)) & 1;
        if (was == is) continue;
        Touch t;
        if (b >= 24 && b < 32) { t.type = Touch::knobTouch; t.index = b - 24; }
        else                   { t.type = Touch::button; t.id = "kk:b" + String (b); }
        t.down = is;
        out.push_back (t);
    }
    for (int k = 0; k < SurfaceView::numKnobs; ++k)
    {
        const int a = before[6 + k * 2] | (before[7 + k * 2] << 8), b = now[6 + k * 2] | (now[7 + k * 2] << 8);
        int d = b - a;
        if (d == 0) continue;
        if (d > 500) d -= 1000;
        if (d < -500) d += 1000;
        Touch t;
        t.type = Touch::knob; t.index = k; t.amount = (float) d / 1000.0f;
        out.push_back (t);
    }
}

namespace
{
    /* The screens' letters: sixteen segments each, one bit a segment, for
       ASCII 0..127. From shaduzlabs' cabl library (FONT_16-seg.h, MIT
       licence, Copyright (C) 2015 Vincenzo Pacella). */
    const uint16 mk1Font[128] = {
        0x0000, 0x0578, 0x0578, 0x830f, 0x0503, 0x5a87, 0x0c7c, 0x1878, 0x1887, 0x4860, 0x0a81, 0x4c66,
        0x5d03, 0x4a62, 0x5a7e, 0xff00, 0x21c0, 0x840c, 0x2466, 0x420c, 0x189f, 0xc333, 0xf878, 0x2466,
        0xe000, 0x0700, 0x2900, 0x9400, 0x0060, 0x2466, 0xe030, 0x0703, 0x0000, 0x0004, 0x0204, 0x5ac0,
        0x5abb, 0x7e99, 0x8b79, 0x0004, 0x8400, 0x2100, 0xff00, 0x5a00, 0x2000, 0x1800, 0x0000, 0x2400,
        0x24ff, 0x040c, 0x1877, 0x183f, 0x188c, 0x18bb, 0x18fb, 0x000f, 0x18ff, 0x18bf, 0x0021, 0x2001,
        0x8103, 0x1830, 0x2403, 0x5007, 0x487f, 0x18cf, 0x523f, 0x00f3, 0x423f, 0x18f3, 0x18c3, 0x10fb,
        0x18cc, 0x4200, 0x203c, 0x8cc0, 0x00f0, 0x05cc, 0x81cc, 0x00ff, 0x18c7, 0x80ff, 0x98c7, 0x18bb,
        0x4203, 0x00fc, 0x24c0, 0xa0cc, 0xa500, 0x5884, 0x2433, 0x00e1, 0x8100, 0x001e, 0x0180, 0x0030,
        0x0100, 0xc860, 0x48e0, 0x0860, 0x4a60, 0x2860, 0x5a02, 0x4aa1, 0x48c0, 0x4000, 0x4220, 0xc600,
        0x4200, 0x5848, 0x4840, 0x4860, 0x0ac1, 0x4a81, 0x0840, 0x9010, 0x08e0, 0x4060, 0x2040, 0xa048,
        0x6600, 0x2500, 0x2820, 0x4a12, 0x4200, 0x5221, 0x0580, 0x540c,
    };

    uint16 mk1Glyph (juce_wchar c)
    {
        if (c >= 32 && c < 128) return mk1Font[c];
        // Accented letters as their plain ones; anything else as a question mark.
        static const char32_t accented[] = U"\u00c0\u00c1\u00c2\u00c3\u00c4\u00c5\u00e0\u00e1\u00e2\u00e3\u00e4\u00e5\u00c7\u00e7\u00c8\u00c9\u00ca\u00cb"
                                             U"\u00e8\u00e9\u00ea\u00eb\u00cc\u00cd\u00ce\u00cf\u00ec\u00ed\u00ee\u00ef\u00d1\u00f1\u00d2\u00d3\u00d4\u00d5"
                                             U"\u00d6\u00d8\u00f2\u00f3\u00f4\u00f5\u00f6\u00f8\u00d9\u00da\u00db\u00dc\u00f9\u00fa\u00fb\u00fc\u00dd\u00fd\u00ff\u00df";
        static const char* plain = "AAAAAAaaaaaaCcEEEEeeeeIIIIiiiiNnOOOOOOooooooUUUUuuuuYyys";
        for (int i = 0; accented[i] != 0; ++i)
            if ((char32_t) c == accented[i]) return mk1Font[(uint8) plain[i]];
        return mk1Font[(uint8) '?'];
    }

    void putMk1Text (uint8* screen, const String& text)
    {
        for (int i = 0; i < jmin (KompleteKontrolMk1::screenLetters, text.length()); ++i)
        {
            const auto g = mk1Glyph (text[i]);
            screen[i * 2] = (uint8) (g & 0xff);
            screen[i * 2 + 1] = (uint8) (g >> 8);
        }
    }

    /** Nine bars for 0..1: the first sits in a frame that is always shown. */
    void putMk1Bars (uint8* screen, float level)
    {
        const int bars = roundToInt (jlimit (0.0f, 1.0f, level) * 9.0f);
        screen[0] = (uint8) (0x04 | (bars > 0 ? 0x03 : 0x00));
        for (int k = 1; k < 8; ++k)
            screen[k * 2] = bars > k ? 0x03 : 0x00;
        screen[15] = bars > 8 ? 0x03 : 0x00;
    }

    /** A name over two lines of eight: whole words to each where they fit,
        broken between the words where least has to go. */
    void wrapMk1Name (const String& name, String& first, String& second)
    {
        const int n = KompleteKontrolMk1::screenLetters;
        StringArray words; words.addTokens (name.trim(), " ", {});
        words.removeEmptyStrings();
        const auto joined = words.joinIntoString (" ");
        if (joined.length() <= n) { first = joined; second = {}; return; }
        if (words.size() < 2) { first = joined.substring (0, n); second = KompleteKontrolMk1::fitWords (joined.substring (n), n); return; }
        int best = 1, bestLost = std::numeric_limits<int>::max();
        for (int k = 1; k < words.size(); ++k)
        {
            const int a = words.joinIntoString (" ", 0, k).length(), b = words.joinIntoString (" ", k).length();
            const int lost = jmax (0, a - n) + jmax (0, b - n);
            if (lost < bestLost) { best = k; bestLost = lost; }
        }
        first = KompleteKontrolMk1::fitWords (words.joinIntoString (" ", 0, best), n);
        second = KompleteKontrolMk1::fitWords (words.joinIntoString (" ", best), n);
    }
}

String KompleteKontrolMk1::fitWords (const String& text, int letters)
{
    StringArray words; words.addTokens (text.trim(), " ", {});
    words.removeEmptyStrings();
    String s = words.joinIntoString (" ");
    auto isVowel = [] (juce_wchar c) { return String ("aeiou").containsChar (c); };
    // "Filter Cutoff" -> "Fltr Ctff" -> "FltrCtff": lower-case vowels inside words, from the end.
    for (int i = s.length() - 1; i > 0 && s.length() > letters; --i)
        if (isVowel (s[i]) && s[i - 1] != ' ')
            s = s.substring (0, i) + s.substring (i + 1);
    for (int i = s.length() - 1; i > 0 && s.length() > letters; --i)
        if (s[i] == ' ')
            s = s.substring (0, i) + s.substring (i + 1);
    return s.substring (0, letters);
}

std::array<std::vector<uint8>, KompleteKontrolMk1::numScreenLines> KompleteKontrolMk1::screenReports (const SurfaceView& v)
{
    // Report 0xe0: where (x, y, width, height, 16 bits each), then 240 bytes:
    // one line across all nine screens, 16 bytes a screen.
    std::array<std::vector<uint8>, numScreenLines> lines;
    for (int y = 0; y < numScreenLines; ++y)
    {
        auto& r = lines[(size_t) y];
        r.assign (1 + 8 + 240, 0);
        r[0] = 0xe0;
        r[3] = (uint8) y;
        r[5] = numScreens * screenLetters;
        r[7] = 1;
    }
    auto screen = [&] (int line, int index) { return lines[(size_t) line].data() + 9 + index * 16; };

    // The first screen: the program's name, and the page when there is room.
    if (v.program >= 0)
    {
        String first, second;
        wrapMk1Name (v.programName, first, second);
        if (second.isEmpty() && v.pageCount > 1)
            second = fitWords (v.pageName.isNotEmpty() ? v.pageName : "Page " + String (v.page + 1), screenLetters);
        putMk1Text (screen (1, 0), first);
        putMk1Text (screen (2, 0), second);
        if (v.pageCount > 1)
            putMk1Bars (screen (0, 0), (float) (v.page + 1) / (float) v.pageCount);
    }
    // The eight over the knobs.
    for (int k = 0; k < SurfaceView::numKnobs; ++k)
    {
        if (v.knobNames[(size_t) k].isEmpty() && v.knobLevels[(size_t) k] < 0.0f) continue;
        putMk1Text (screen (1, k + 1), fitWords (v.knobNames[(size_t) k], screenLetters));
        putMk1Text (screen (2, k + 1), fitWords (v.knobValues[(size_t) k], screenLetters));
        if (v.knobLevels[(size_t) k] >= 0.0f)
            putMk1Bars (screen (0, k + 1), v.knobLevels[(size_t) k]);
    }
    return lines;
}

#if JUCE_LINUX || JUCE_BSD
std::vector<KompleteKontrolMk1::Device> KompleteKontrolMk1::find()
{
    std::vector<Device> found;
    for (const auto& dir : File ("/sys/class/hidraw").findChildFiles (File::findDirectories | File::findFiles, false))
    {
        const auto uevent = dir.getChildFile ("device/uevent").loadFileAsString();
        // HID_ID=0003:000017CC:00001410
        const auto id = uevent.fromFirstOccurrenceOf ("HID_ID=", false, false).upToFirstOccurrenceOf ("\n", false, false).trim();
        StringArray parts; parts.addTokens (id, ":", {});
        if (parts.size() != 3 || parts[1].getHexValue32() != 0x17cc) continue;
        if (const auto* model = modelFor (parts[2].getHexValue32()))
            found.push_back ({ "/dev/" + dir.getFileName(), model });
    }
    return found;
}

KompleteKontrolMk1::KompleteKontrolMk1 (const Device& d, String& error) : device (d)
{
    fd = ::open (device.path.toRawUTF8(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        error = "Could not open the Komplete Kontrol's lights and knobs (" + device.path + "): " + String (std::strerror (errno))
              + (errno == EACCES ? ". This user is not allowed to use the device: StageRig's packages install a rule for it, "
                                   "and a keyboard plugged in before StageRig was installed needs plugging in again." : ".");
        return;
    }
    reader = std::thread ([this] { read(); });
    writer = std::thread ([this] { write(); });
}

KompleteKontrolMk1::~KompleteKontrolMk1()
{
    {
        const std::lock_guard<std::mutex> l (writeMutex);
        stopping = true;
        // The keyboard's lights and screens go dark as StageRig lets go, if it is still answering.
        nextLights = lightReport ({}, *device.model);
        lightsWaiting = litOnce;
        const auto blank = screenReports ({});
        for (size_t y = 0; y < blank.size(); ++y)
        {
            lineWaiting[y] = litOnce && blank[y] != shownLines[y];
            nextLines[y] = blank[y];
        }
    }
    writeWake.notify_all();
    if (writer.joinable()) writer.join();
    if (reader.joinable()) reader.join();
    if (fd >= 0) ::close (fd);
}

String KompleteKontrolMk1::getProblem() const
{
    if (! stalled.load()) return {};
    return getName() + " stopped answering (its lights and knobs; the keys still play). Unplug it for ten seconds and plug it "
           "back in -- into a port on the computer itself, not a hub, if it keeps happening.";
}

void KompleteKontrolMk1::write()
{
    // Hand the light guide and screens to the computer, then send each newest report.
    const uint8 takeOver[] = { 0xa0, 0x00, 0x00 };
    auto put = [this] (const uint8* data, size_t size)
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            if (::write (fd, data, size) >= 0) return true;
            if (errno == ENODEV) { gone = true; return false; }
            if (errno != EAGAIN && errno != EINTR) break;    // ETIMEDOUT, EPROTO, EPIPE: the keyboard did not take it
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
        stalled = true;
        return false;
    };
    if (! put (takeOver, sizeof (takeOver))) return;

    for (;;)
    {
        std::vector<uint8> report;
        bool last = false;
        {
            std::unique_lock<std::mutex> l (writeMutex);
            auto anyLine = [this] { return std::find (lineWaiting.begin(), lineWaiting.end(), true) != lineWaiting.end(); };
            writeWake.wait (l, [&] { return lightsWaiting || anyLine() || stopping.load(); });
            if (lightsWaiting)
            {
                report.swap (nextLights);
                lightsWaiting = false;
            }
            else if (anyLine())
            {
                const auto y = (size_t) std::distance (lineWaiting.begin(), std::find (lineWaiting.begin(), lineWaiting.end(), true));
                report.swap (nextLines[y]);
                lineWaiting[y] = false;
            }
            else return;
            last = stopping.load() && ! lightsWaiting && ! anyLine();
        }
        if (! put (report.data(), report.size()) || last) return;
    }
}

void KompleteKontrolMk1::read()
{
    uint8 buf[512], previous[64] = {};
    bool havePrevious = false;
    while (! stopping.load())
    {
        pollfd p { fd, POLLIN, 0 };
        const int ready = ::poll (&p, 1, 200);
        if (ready < 0 && errno != EINTR) { gone = true; return; }
        if (ready <= 0) continue;
        if ((p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) { gone = true; return; }
        const auto n = ::read (fd, buf, sizeof (buf));
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EINTR) continue;
            gone = true;
            return;
        }
        if (n < 38 || buf[0] != 0x01) continue;
        if (havePrevious)
        {
            std::vector<Touch> found;
            compareReports (previous, buf + 1, 37, found);
            if (! found.empty())
            {
                const std::lock_guard<std::mutex> l (touchMutex);
                if (touches.size() < 1024) touches.insert (touches.end(), found.begin(), found.end());
            }
        }
        std::memcpy (previous, buf + 1, 37);
        havePrevious = true;
    }
}

void KompleteKontrolMk1::show (const SurfaceView& v)
{
    if (fd < 0 || stalled.load()) return;
    auto lines = screenReports (v);
    const bool newLights = ! litOnce || v.keyColours != lit;
    bool any = newLights;
    {
        const std::lock_guard<std::mutex> l (writeMutex);
        if (newLights)
        {
            nextLights = lightReport (v.keyColours, *device.model);
            lightsWaiting = true;
        }
        for (size_t y = 0; y < lines.size(); ++y)
        {
            if (lines[y] == shownLines[y]) continue;
            shownLines[y] = lines[y];
            nextLines[y] = std::move (lines[y]);
            lineWaiting[y] = true;
            any = true;
        }
    }
    if (any) writeWake.notify_one();
    lit = v.keyColours;
    litOnce = true;
}
#else
std::vector<KompleteKontrolMk1::Device> KompleteKontrolMk1::find()                     { return {}; }
KompleteKontrolMk1::KompleteKontrolMk1 (const Device& d, String& error) : device (d)  { error = "The Komplete Kontrol's lights and knobs work on Linux only."; }
KompleteKontrolMk1::~KompleteKontrolMk1()                                             {}
String KompleteKontrolMk1::getProblem() const                                         { return {}; }
void KompleteKontrolMk1::write()                                                      {}
void KompleteKontrolMk1::read()                                                       {}
void KompleteKontrolMk1::show (const SurfaceView&)                                    {}
#endif

void KompleteKontrolMk1::takeTouches (std::vector<Touch>& out)
{
    const std::lock_guard<std::mutex> l (touchMutex);
    out.insert (out.end(), touches.begin(), touches.end());
    touches.clear();
}

} // namespace perf
