/*  A CLAP note effect for the tests: everything it hears comes back out, the
    notes an octave up. Small enough to trust, so EngineTest can tell what a
    MIDI processor in a slot's chain does to the notes after it -- and that
    a plugin's MIDI output makes it back from the host process at all. */

#include <clap/clap.h>
#include <cstring>

namespace
{
const char* const features[] = { CLAP_PLUGIN_FEATURE_NOTE_EFFECT, nullptr };

const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT,
    "studio.stagerig.test-octave-up",
    "StageRig Test Octave Up",
    "StageRig",
    "", "", "", "1.0",
    "Plays every note an octave higher (for tests)",
    features
};

uint32_t notePortCount (const clap_plugin_t*, bool) { return 1; }

bool notePortGet (const clap_plugin_t*, uint32_t index, bool isInput, clap_note_port_info_t* info)
{
    if (index != 0) return false;
    info->id = isInput ? 0 : 1;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::strncpy (info->name, isInput ? "In" : "Out", sizeof (info->name));
    return true;
}

const clap_plugin_note_ports_t notePorts = { notePortCount, notePortGet };

uint32_t audioPortCount (const clap_plugin_t*, bool) { return 0; }
bool audioPortGet (const clap_plugin_t*, uint32_t, bool, clap_audio_port_info_t*) { return false; }
const clap_plugin_audio_ports_t audioPorts = { audioPortCount, audioPortGet };

int16_t up (int16_t key) { return key + 12 <= 127 ? (int16_t) (key + 12) : key; }

clap_process_status process (const clap_plugin_t*, const clap_process_t* p)
{
    for (uint32_t ch = 0; ch < p->audio_outputs_count; ++ch)
        for (uint32_t c = 0; c < p->audio_outputs[ch].channel_count; ++c)
            std::memset (p->audio_outputs[ch].data32[c], 0, sizeof (float) * p->frames_count);

    const uint32_t n = p->in_events->size (p->in_events);
    for (uint32_t i = 0; i < n; ++i)
    {
        const auto* h = p->in_events->get (p->in_events, i);
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF)
        {
            clap_event_note_t e = *reinterpret_cast<const clap_event_note_t*> (h);
            e.key = up (e.key);
            e.port_index = 0;
            p->out_events->try_push (p->out_events, &e.header);
        }
        else if (h->type == CLAP_EVENT_MIDI)
        {
            clap_event_midi_t e = *reinterpret_cast<const clap_event_midi_t*> (h);
            const int status = e.data[0] & 0xf0;
            if (status == 0x80 || status == 0x90) e.data[1] = (uint8_t) up (e.data[1]);
            p->out_events->try_push (p->out_events, &e.header);
        }
    }
    return CLAP_PROCESS_CONTINUE;
}

const void* getExtension (const clap_plugin_t*, const char* id)
{
    if (std::strcmp (id, CLAP_EXT_NOTE_PORTS) == 0)  return &notePorts;
    if (std::strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0) return &audioPorts;
    return nullptr;
}

bool init (const clap_plugin_t*)                                   { return true; }
void destroy (const clap_plugin_t* p)                              { delete p; }
bool activate (const clap_plugin_t*, double, uint32_t, uint32_t)   { return true; }
void deactivate (const clap_plugin_t*)                             {}
bool startProcessing (const clap_plugin_t*)                        { return true; }
void stopProcessing (const clap_plugin_t*)                         {}
void reset (const clap_plugin_t*)                                  {}
void onMainThread (const clap_plugin_t*)                           {}

uint32_t factoryCount (const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* factoryDescriptor (const clap_plugin_factory_t*, uint32_t i) { return i == 0 ? &descriptor : nullptr; }

const clap_plugin_t* factoryCreate (const clap_plugin_factory_t*, const clap_host_t*, const char* id)
{
    if (std::strcmp (id, descriptor.id) != 0) return nullptr;
    auto* p = new clap_plugin_t {};
    p->desc = &descriptor;
    p->init = init;
    p->destroy = destroy;
    p->activate = activate;
    p->deactivate = deactivate;
    p->start_processing = startProcessing;
    p->stop_processing = stopProcessing;
    p->reset = reset;
    p->process = process;
    p->get_extension = getExtension;
    p->on_main_thread = onMainThread;
    return p;
}

const clap_plugin_factory_t factory = { factoryCount, factoryDescriptor, factoryCreate };

bool entryInit (const char*) { return true; }
void entryDeinit()           {}
const void* entryFactory (const char* id) { return std::strcmp (id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr; }
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory };
