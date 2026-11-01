#include "Surfaces.h"
#include "PanelParams.h"
#include "RemotePlugin.h"

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
SurfaceHub::Layout SurfaceHub::layoutFor (int input, int program, int slot) const
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

    layout.map = library.find (sd.plugin, sd.controlMap);
    if (layout.map != nullptr && ! layout.map->pages.empty())
    {
        for (const auto& mp : layout.map->pages)
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
        return layout;
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
        // A new program: the instrument that has a setup, else its first plugin.
        int pick = -1;
        for (int s = 0; s < (int) prog.slots.size() && pick < 0; ++s)
            if (prog.slots[(size_t) s].isPlugin() && library.find (prog.slots[(size_t) s].plugin, prog.slots[(size_t) s].controlMap) != nullptr) pick = s;
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
    v.program = program;
    v.programName = prog.name.isNotEmpty() ? prog.name : "Program " + String (program).paddedLeft ('0', 3);

    const int slot = getFocusSlot (input);
    v.page = getFocusPage (input);
    const auto& layout = currentLayout (input);
    if (slot >= 0)
    {
        const auto& sd = prog.slots[(size_t) slot];
        v.instrument = sd.plugin.name;
        if (layout.map != nullptr && ! layout.map->name.equalsIgnoreCase (sd.plugin.name)) v.instrument << ": " << layout.map->name;
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
    if (knob && cc <= 0) return;     // a knob is a controller: a keyboard's button cannot be one, so keep waiting

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

void SurfaceHub::surfaceKnob (int input, int knob, int value, bool relative)
{
    auto& p = pending[{ input, knob }];
    if (relative) p.delta += (float) value / 100.0f;
    else        { p.absolute = (float) value / 127.0f; p.delta = 0.0f; }
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
        case ControlSurface::Touch::knobTouch:
            break;
    }
}

int SurfaceHub::inputFor (const ControlSurface& s) const
{
    const auto& inputs = engine.getSetup().inputs;
    const auto keys = s.getKeysPortName();
    for (int i = 0; i < (int) inputs.size(); ++i)
        if (inputs[(size_t) i].kind == InputDef::Kind::midi && inputs[(size_t) i].midiDeviceName.containsIgnoreCase (keys)) return i;
    for (int i = 0; i < (int) inputs.size(); ++i)
        if (inputs[(size_t) i].kind == InputDef::Kind::midi) return i;
    return -1;
}

//==============================================================================
bool SurfaceHub::isKompleteKontrolEnabled() const   { return settings.getBoolValue ("surfaceKompleteKontrol", false); }
bool SurfaceHub::isLaunchkeyEnabled() const         { return settings.getBoolValue ("surfaceLaunchkey", true); }
void SurfaceHub::setKompleteKontrolEnabled (bool on) { settings.setValue ("surfaceKompleteKontrol", on); lastSearch = 0; }
void SurfaceHub::setLaunchkeyEnabled (bool on)       { settings.setValue ("surfaceLaunchkey", on); lastSearch = 0; }

std::vector<SurfaceHub::SurfaceInfo> SurfaceHub::getSurfaces() const
{
    std::vector<SurfaceInfo> list;
    for (const auto& s : surfaces)
        list.push_back ({ s->getName(), inputFor (*s), s->hasScreen(), s->hasLights() });
    return list;
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
        const bool off = (kk != nullptr && ! isKompleteKontrolEnabled()) || (lk != nullptr && ! isLaunchkeyEnabled());
        if (off || ! (*it)->isConnected())
        {
            shown.erase (it->get());
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

    if (isLaunchkeyEnabled())
        for (const auto& p : LaunchkeyMk4::find())
        {
            bool have = false;
            for (const auto& s : surfaces)
                if (auto* lk = dynamic_cast<LaunchkeyMk4*> (s.get()); lk != nullptr && lk->getOutputIdentifier() == p.out.identifier) have = true;
            if (have) continue;
            auto lk = std::make_unique<LaunchkeyMk4> (p);
            if (lk->opened()) { surfaces.push_back (std::move (lk)); changed = true; }
            else problems.add ("Could not open the Launchkey's DAW port, " + p.out.name + ".");
        }

    if (changed && onSurfacesChanged) onSurfacesChanged();
}

void SurfaceHub::tick()
{
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

std::vector<LaunchkeyMk4::Ports> LaunchkeyMk4::find()
{
    std::vector<Ports> found;
    MidiDeviceInfo dawIn, dawOut;
    String keys;
    for (const auto& d : MidiInput::getAvailableDevices())
        if (isLaunchkeyMk4 (d.name))
        {
            if (isDawPort (d.name)) { if (dawIn.identifier.isEmpty()) dawIn = d; }
            else if (keys.isEmpty()) keys = d.name;
        }
    for (const auto& d : MidiOutput::getAvailableDevices())
        if (isLaunchkeyMk4 (d.name) && isDawPort (d.name) && dawOut.identifier.isEmpty()) dawOut = d;
    if (dawIn.identifier.isNotEmpty() && dawOut.identifier.isNotEmpty())
    {
        Ports p;
        p.in = dawIn; p.out = dawOut;
        p.mini = dawOut.name.containsIgnoreCase ("Mini");
        p.keysName = p.mini ? "Launchkey Mini" : "Launchkey";
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
              + (errno == EACCES ? ". This user is not allowed to use the device." : ".");
        return;
    }
    const uint8 takeOver[] = { 0xa0, 0x00, 0x00 };      // the computer drives the light guide from now on
    if (::write (fd, takeOver, sizeof (takeOver)) < 0)
    {
        error = "The Komplete Kontrol did not answer (" + String (std::strerror (errno)) + ").";
        ::close (fd);
        fd = -1;
        return;
    }
    reader = std::thread ([this] { read(); });
}

KompleteKontrolMk1::~KompleteKontrolMk1()
{
    stopping = true;
    if (reader.joinable()) reader.join();
    if (fd >= 0)
    {
        const auto dark = lightReport ({}, *device.model);
        [[maybe_unused]] auto n = ::write (fd, dark.data(), dark.size());
        ::close (fd);
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
    if (fd < 0 || (litOnce && v.keyColours == lit)) return;
    const auto report = lightReport (v.keyColours, *device.model);
    if (::write (fd, report.data(), report.size()) < 0 && errno != EAGAIN) gone = true;
    lit = v.keyColours;
    litOnce = true;
}
#else
std::vector<KompleteKontrolMk1::Device> KompleteKontrolMk1::find()                     { return {}; }
KompleteKontrolMk1::KompleteKontrolMk1 (const Device& d, String& error) : device (d)  { error = "The Komplete Kontrol's lights and knobs work on Linux only."; }
KompleteKontrolMk1::~KompleteKontrolMk1()                                             {}
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
