#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_system.h"
#include "ir/su_sha256.h"
#include "su_audio_int.h"
#include "su_audio_output.h"
#include "platform/su_thread.h"

#define QUANTUM SU_AUDIO_EVENT_QUANTUM
#define SYSTEM_MAGIC 0x53415553u /* "SUAS" little-endian */
#define SYSTEM_HEADER 16u
#define INSPECT_ROWS 256u
#define TRACE_ROWS 1024u
#define OFFLINE_MAX_FRAMES (48000u * 600u)

struct SuAudioSystem {
    SuAudioSystemDesc desc;
    SuAudioScene *scene;
    SuAudioVoicePool *pool;
    SuAudioBusTree *buses;
    SuAudioParams *params;
    SuAudioMusic *music;
    SuAudioCluster *cluster;
    SuAudioEvents *events;
    SuAudioTelemetry *telemetry;
    SuAudioDevice *device;
    SuAudioSystemCommand *journal;
    uint32_t journal_count;
    uint64_t journal_dropped;
    int journal_complete;          /* the journal starts at the system's first frame */
    float *ring;
    uint32_t ring_frames;
    uint32_t ring_target;
    int adaptive;
    uint64_t adaptive_underruns;
    uint64_t adaptive_read;
    SuAudioGap gap;
    _Atomic uint64_t write_pos;
    _Atomic uint64_t read_pos;
    _Atomic uint64_t underruns;
    SuMutex lock;                  /* PULL render: serializes the owner's calls with the reader's renders */
    int lock_ready;
    float scratch[1024u * 2u];
    SuAudioInspectInstance *inspect_instances;
    SuAudioInspectVoice *inspect_voices;
    SuAudioInspectEmitter *inspect_emitters;
    SuAudioInspectBus *inspect_buses;
    SuAudioTelemetryEntry *trace_rows;
    uint64_t bytes;
};

static void message_set(char *message, const char *fmt, ...) {
    va_list ap;
    if (message == NULL) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(message, 256, fmt, ap);
    va_end(ap);
}

static void *system_calloc(SuAudioSystem *system, size_t count, size_t size) {
    void *memory = calloc(count ? count : 1u, size);
    if (memory != NULL) {
        system->bytes += (uint64_t)(count ? count : 1u) * size;
    }
    return memory;
}

static void device_pull(void *user, float *out, uint32_t frames) {
    SuAudioSystem *system = user;
    uint32_t valid = su_audio_system_ring_read(system, out, frames);
    su_audio_gap_apply(&system->gap, out, valid, frames);
}

void su_audio_system_destroy(SuAudioSystem *system) {
    if (system == NULL) {
        return;
    }
    su_audio_device_close(system->device);
    if (system->lock_ready) {
        su_mutex_destroy(&system->lock);
    }
    su_audio_events_destroy(system->events);
    su_audio_music_destroy(system->music);
    su_audio_cluster_destroy(system->cluster);
    su_audio_params_destroy(system->params);
    su_audio_bus_tree_destroy(system->buses);
    su_audio_voice_pool_destroy(system->pool);
    su_audio_scene_destroy(system->scene);
    su_audio_trace_end(system->telemetry);
    free(system->journal);
    free(system->ring);
    free(system->inspect_instances);
    free(system->inspect_voices);
    free(system->inspect_emitters);
    free(system->inspect_buses);
    free(system->trace_rows);
    free(system);
}

uint32_t su_audio_system_create(const SuAudioSystemDesc *desc, SuAudioSystem **out, char message[256]) {
    SuAudioSystem *system;
    SuAudioEventsDesc events_desc;
    SuAudioEventsHook hook;
    SuAudioBusDiagnostic bus_diag;
    uint32_t status;
    uint32_t index;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (desc == NULL || desc->struct_size != sizeof *desc || desc->assets == NULL || desc->graph == NULL
        || desc->output > SU_AUDIO_OUTPUT_DEVICE || (desc->curve_count != 0u && desc->curves == NULL)
        || (desc->segment_count != 0u && (desc->segments == NULL || desc->music == NULL))
        || desc->journal_capacity > 16777216u || desc->telemetry_capacity > SU_AUDIO_TELEMETRY_MAX
        || desc->render > SU_AUDIO_RENDER_PULL) {
        message_set(message, "invalid system descriptor");
        return SU_E_INVALID_ARGUMENT;
    }
    if (desc->output != SU_AUDIO_OUTPUT_OFFLINE
        && (desc->ring_frames < 256u || desc->ring_frames > (1u << 20) || desc->ring_frames % QUANTUM != 0u)) {
        message_set(message, "ring_frames must be a multiple of 128 in 256..1048576");
        return SU_E_INVALID_ARGUMENT;
    }
    system = calloc(1u, sizeof *system);
    if (system == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    system->bytes = sizeof *system;
    system->desc = *desc;
    system->journal_complete = 1;
    atomic_init(&system->write_pos, 0u);
    atomic_init(&system->read_pos, 0u);
    atomic_init(&system->underruns, 0u);
    if (su_mutex_init(&system->lock) != 0) {
        free(system);
        return SU_E_RESOURCE_LIMIT;
    }
    system->lock_ready = 1;
    status = su_audio_scene_create(&desc->scene, &system->scene);
    for (index = 0; status == SU_OK && index < desc->curve_count; index++) {
        SuAudioCurveId id = 0u;
        status = su_audio_scene_curve_add(system->scene, &desc->curves[index], &id);
    }
    if (status != SU_OK) {
        message_set(message, "scene or curve rejected");
        goto fail;
    }
    status = su_audio_voice_pool_create(&desc->voices, &system->pool);
    if (status != SU_OK) {
        message_set(message, "voice pool descriptor rejected");
        goto fail;
    }
    if (desc->buses != NULL) {
        memset(&bus_diag, 0, sizeof bus_diag);
        status = su_audio_bus_tree_create(desc->buses, &system->buses, &bus_diag);
        if (status != SU_OK) {
            message_set(message, "bus tree rejected (diagnostic %u, bus %u, rule %u)", bus_diag.code, bus_diag.bus_id,
                bus_diag.rule_index);
            goto fail;
        }
    }
    if (desc->params != NULL && (status = su_audio_params_create(desc->params, &system->params)) != SU_OK) {
        message_set(message, "parameter descriptor rejected");
        goto fail;
    }
    if (desc->music != NULL) {
        status = su_audio_music_create(desc->music, desc->assets, &system->music);
        for (index = 0; status == SU_OK && index < desc->segment_count; index++) {
            status = su_audio_music_segment_add(system->music, &desc->segments[index]);
        }
        if (status != SU_OK) {
            message_set(message, "music or segment rejected");
            goto fail;
        }
    }
    if (desc->cluster != NULL && (status = su_audio_cluster_create(desc->cluster, &system->cluster)) != SU_OK) {
        message_set(message, "cluster descriptor rejected");
        goto fail;
    }
    events_desc = desc->events;
    status = su_audio_events_create(&events_desc, desc->assets, system->scene, desc->graph, &system->events);
    if (status != SU_OK) {
        message_set(message, "event runtime descriptor rejected");
        goto fail;
    }
    memset(&hook, 0, sizeof hook);
    hook.struct_size = sizeof hook;
    hook.version = SU_AUDIO_EVENTS_HOOK_VERSION;
    hook.voices = system->pool;
    hook.policies = desc->policies;
    hook.policy_count = desc->policy_count;
    hook.default_policy = desc->default_policy;
    hook.params = system->params;
    hook.buses = system->buses;
    hook.music = system->music;
    hook.music_bus_id = desc->music_bus_id;
    hook.limiter_knee = desc->limiter_knee;
    hook.cluster = system->cluster;
    hook.cluster_inputs = desc->cluster_inputs;
    hook.doppler = desc->doppler;
    hook.speed_of_sound = desc->speed_of_sound;
    hook.inputs = desc->inputs;
    hook.input_count = desc->input_count;
    status = su_audio_events_attach(system->events, &hook);
    if (status != SU_OK) {
        message_set(message, "components rejected by the event runtime (rates, capacities, clocks or knee)");
        goto fail;
    }
    if (desc->journal_capacity != 0u) {
        system->journal = system_calloc(system, desc->journal_capacity, sizeof *system->journal);
    }
    if (desc->telemetry_capacity != 0u
        && (status = su_audio_trace_begin(desc->telemetry_capacity, &system->telemetry)) != SU_OK) {
        message_set(message, "telemetry capacity rejected");
        goto fail;
    }
    system->inspect_instances = system_calloc(system, INSPECT_ROWS, sizeof *system->inspect_instances);
    system->inspect_voices = system_calloc(system, INSPECT_ROWS, sizeof *system->inspect_voices);
    system->inspect_emitters = system_calloc(system, INSPECT_ROWS, sizeof *system->inspect_emitters);
    system->inspect_buses = system_calloc(system, INSPECT_ROWS, sizeof *system->inspect_buses);
    system->trace_rows = system_calloc(system, TRACE_ROWS, sizeof *system->trace_rows);
    if ((desc->journal_capacity != 0u && system->journal == NULL) || system->inspect_instances == NULL
        || system->inspect_voices == NULL || system->inspect_emitters == NULL || system->inspect_buses == NULL
        || system->trace_rows == NULL) {
        status = SU_E_RESOURCE_LIMIT;
        goto fail;
    }
    if (desc->output != SU_AUDIO_OUTPUT_OFFLINE) {
        system->ring_frames = desc->ring_frames;
        system->ring_target = desc->ring_frames;
        system->ring = system_calloc(system, (size_t)desc->ring_frames * 2u, sizeof *system->ring);
        if (system->ring == NULL) {
            status = SU_E_RESOURCE_LIMIT;
            goto fail;
        }
    }
    if (desc->output == SU_AUDIO_OUTPUT_DEVICE) {
        SuAudioDeviceInfo info;
        char device_message[256];
        device_message[0] = '\0';
        status = su_audio_system_update(system);
        if (status == SU_OK) {
            status = su_audio_device_open_pull(desc->events.sample_rate, device_pull, system, &system->device, &info,
                device_message);
        }
        if (status != SU_OK) {
            message_set(message, "%s", device_message[0] ? device_message : "output device unavailable");
            goto fail;
        }
    }
    *out = system;
    return SU_OK;
fail:
    su_audio_system_destroy(system);
    return status != SU_OK ? status : SU_E_INVALID_ARGUMENT;
}

SuAudioEvents *su_audio_system_events(SuAudioSystem *system) {
    return system != NULL ? system->events : NULL;
}

SuAudioScene *su_audio_system_scene(SuAudioSystem *system) {
    return system != NULL ? system->scene : NULL;
}

SuAudioVoicePool *su_audio_system_voices(SuAudioSystem *system) {
    return system != NULL ? system->pool : NULL;
}

SuAudioBusTree *su_audio_system_buses(SuAudioSystem *system) {
    return system != NULL ? system->buses : NULL;
}

SuAudioParams *su_audio_system_params(SuAudioSystem *system) {
    return system != NULL ? system->params : NULL;
}

SuAudioMusic *su_audio_system_music(SuAudioSystem *system) {
    return system != NULL ? system->music : NULL;
}

SuAudioCluster *su_audio_system_cluster(SuAudioSystem *system) {
    return system != NULL ? system->cluster : NULL;
}

SuAudioTelemetry *su_audio_system_telemetry(SuAudioSystem *system) {
    return system != NULL ? system->telemetry : NULL;
}

uint64_t su_audio_system_clock(const SuAudioSystem *system) {
    return system != NULL ? su_audio_events_clock(system->events) : 0u;
}

static uint64_t produced(const SuAudioSystem *system) {
    SuAudioEventStats stats;
    su_audio_events_stats(system->events, &stats);
    return stats.delivered_frames;
}

void su_audio_system_stats(const SuAudioSystem *system, SuAudioSystemStats *out) {
    uint64_t write;
    uint64_t read;
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (system == NULL) {
        return;
    }
    out->clock = su_audio_events_clock(system->events);
    out->rendered_frames = produced(system);
    write = atomic_load_explicit(&((SuAudioSystem *)system)->write_pos, memory_order_acquire);
    read = atomic_load_explicit(&((SuAudioSystem *)system)->read_pos, memory_order_acquire);
    out->consumed_frames = read;
    out->ring_fill = (uint32_t)(write - read);
    out->ring_capacity = system->ring_frames;
    out->ring_target = system->ring_target;
    su_audio_device_stats(system->device, &out->device);
    out->underrun_frames = atomic_load_explicit(&((SuAudioSystem *)system)->underruns, memory_order_relaxed);
    out->device_frames = su_audio_device_frames(system->device);
    out->journal_count = system->journal_count;
    out->journal_capacity = system->desc.journal_capacity;
    out->journal_dropped = system->journal_dropped;
    {
        SuAudioEventStats events;
        SuAudioVoiceStats voices;
        SuAudioParamStats params;
        SuAudioClusterStats cluster;
        su_audio_events_stats(system->events, &events);
        su_audio_voice_pool_stats(system->pool, &voices);
        su_audio_params_stats(system->params, &params);
        su_audio_cluster_stats(system->cluster, &cluster);
        out->runtime_bytes = system->bytes + events.runtime_bytes + voices.runtime_bytes + params.runtime_bytes
            + su_audio_bus_tree_bytes(system->buses) + (system->cluster != NULL ? cluster.runtime_bytes : 0u);
    }
}

/* ---- journaled commands ---- */

static SuAudioSystemCommand *journal_next(SuAudioSystem *system, uint32_t kind) {
    SuAudioSystemCommand *command;
    if (system->journal == NULL || system->journal_count == system->desc.journal_capacity) {
        system->journal_dropped++;
        system->journal_complete = 0;
        return NULL;
    }
    command = &system->journal[system->journal_count++];
    memset(command, 0, sizeof *command);
    command->kind = kind;
    command->position = produced(system);
    return command;
}

static void trace(SuAudioSystem *system, uint32_t kind, uint32_t id, uint64_t sample_time, uint64_t handle,
                  uint32_t status) {
    SuAudioTelemetryEntry entry;
    if (system->telemetry == NULL) {
        return;
    }
    memset(&entry, 0, sizeof entry);
    entry.kind = kind;
    entry.id = id;
    entry.sample_time = sample_time;
    entry.render_clock = su_audio_events_clock(system->events);
    entry.instance = handle;
    entry.status = status;
    (void)su_audio_trace_record(system->telemetry, &entry);
}

uint32_t su_audio_system_post(SuAudioSystem *system, const SuAudioTrigger *trigger, SuAudioInstance *out,
                              SuAudioGraphDiagnostic *diagnostic) {
    SuAudioSystemCommand *command;
    uint32_t status;
    if (system == NULL || trigger == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    command = journal_next(system, SU_AUDIO_SYSTEM_CMD_POST);
    if (system->telemetry != NULL) {
        status = su_audio_inspect_post(system->events, system->telemetry, trigger, out, diagnostic);
    } else {
        status = su_audio_event_post(system->events, trigger, out, diagnostic);
    }
    if (command != NULL) {
        command->trigger = *trigger;
        command->status = status;
    }
    return status;
}

uint32_t su_audio_system_stop(SuAudioSystem *system, uint32_t kind, uint64_t target, uint64_t sample_time,
                              uint32_t release_frames) {
    SuAudioSystemCommand *command;
    uint32_t status;
    if (system == NULL || kind < SU_AUDIO_SYSTEM_CMD_STOP_INSTANCE || kind > SU_AUDIO_SYSTEM_CMD_STOP_SCOPE) {
        return SU_E_INVALID_ARGUMENT;
    }
    command = journal_next(system, kind);
    if (kind == SU_AUDIO_SYSTEM_CMD_STOP_INSTANCE) {
        status = su_audio_instance_stop(system->events, target, sample_time, release_frames);
    } else if (kind == SU_AUDIO_SYSTEM_CMD_STOP_EMITTER) {
        status = su_audio_emitter_stop(system->events, target, sample_time, release_frames);
    } else {
        status = su_audio_scope_stop(system->events, (uint32_t)target, sample_time, release_frames);
    }
    if (command != NULL) {
        command->target = target;
        command->sample_time = sample_time;
        command->frames = release_frames;
        command->status = status;
    }
    trace(system, kind == SU_AUDIO_SYSTEM_CMD_STOP_INSTANCE ? SU_AUDIO_TRACE_STOP_INSTANCE
        : kind == SU_AUDIO_SYSTEM_CMD_STOP_EMITTER ? SU_AUDIO_TRACE_STOP_EMITTER : SU_AUDIO_TRACE_STOP_SCOPE,
        kind == SU_AUDIO_SYSTEM_CMD_STOP_SCOPE ? (uint32_t)target : (uint32_t)(target & 0xffffffffu), sample_time,
        kind == SU_AUDIO_SYSTEM_CMD_STOP_INSTANCE ? target : 0u, status);
    return status;
}

uint32_t su_audio_system_param_set(SuAudioSystem *system, SuAudioParamScope scope, SuAudioParamId id, float value,
                                   uint64_t sample_time) {
    SuAudioSystemCommand *command;
    uint32_t status;
    if (system == NULL || system->params == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    command = journal_next(system, SU_AUDIO_SYSTEM_CMD_PARAM);
    status = su_audio_param_set_at(system->params, scope, id, value, sample_time);
    if (command != NULL) {
        command->scope = scope;
        command->id = id;
        command->value = value;
        command->sample_time = sample_time;
        command->status = status;
    }
    return status;
}

uint32_t su_audio_system_switch_set(SuAudioSystem *system, SuAudioParamScope scope, SuAudioSwitchId id,
                                    uint32_t value, uint64_t sample_time) {
    SuAudioSystemCommand *command;
    uint32_t status;
    if (system == NULL || system->params == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    command = journal_next(system, SU_AUDIO_SYSTEM_CMD_SWITCH);
    status = su_audio_switch_set_at(system->params, scope, id, value, sample_time);
    if (command != NULL) {
        command->scope = scope;
        command->id = id;
        command->switch_value = value;
        command->sample_time = sample_time;
        command->status = status;
    }
    return status;
}

uint32_t su_audio_system_emitter_create(SuAudioSystem *system, const SuAudioEmitterSnapshot *desc,
                                        SuAudioEmitter *out) {
    SuAudioSystemCommand *command;
    uint32_t status;
    if (system == NULL || desc == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    command = journal_next(system, SU_AUDIO_SYSTEM_CMD_EMITTER_CREATE);
    status = su_audio_emitter_create(system->scene, desc, out);
    if (command != NULL) {
        command->emitter = *desc;
        command->target = *out;
        command->status = status;
    }
    return status;
}

uint32_t su_audio_system_emitter_update(SuAudioSystem *system, SuAudioEmitter emitter, SuWorldPos pos, SuVec3f vel) {
    SuAudioSystemCommand *command;
    uint32_t status;
    if (system == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    command = journal_next(system, SU_AUDIO_SYSTEM_CMD_EMITTER_UPDATE);
    status = su_audio_emitter_update(system->scene, emitter, pos, vel);
    if (command != NULL) {
        command->target = emitter;
        command->emitter.pos = pos;
        command->emitter.vel = vel;
        command->status = status;
    }
    trace(system, SU_AUDIO_TRACE_EMITTER_MOVE, (uint32_t)(emitter & 0xffffffffu), su_audio_system_clock(system), 0u,
        status);
    return status;
}

uint32_t su_audio_system_emitter_destroy(SuAudioSystem *system, SuAudioEmitter emitter) {
    SuAudioSystemCommand *command;
    uint32_t status;
    if (system == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    command = journal_next(system, SU_AUDIO_SYSTEM_CMD_EMITTER_DESTROY);
    status = su_audio_emitter_destroy(system->scene, emitter);
    if (command != NULL) {
        command->target = emitter;
        command->status = status;
    }
    return status;
}

uint32_t su_audio_system_listener_set(SuAudioSystem *system, const SuAudioCameraListener *listener) {
    SuAudioSystemCommand *command;
    uint32_t status;
    if (system == NULL || listener == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    command = journal_next(system, SU_AUDIO_SYSTEM_CMD_LISTENER);
    status = su_audio_scene_listener_set(system->scene, listener);
    if (command != NULL) {
        command->listener = *listener;
        command->status = status;
    }
    trace(system, SU_AUDIO_TRACE_LISTENER, 0u, su_audio_system_clock(system), 0u, status);
    return status;
}

uint32_t su_audio_system_music_transition(SuAudioSystem *system, const SuMusicTransition *transition,
                                          uint64_t sample_time, uint64_t *boundary) {
    SuAudioSystemCommand *command;
    uint64_t at = 0u;
    uint32_t status;
    if (system == NULL || system->music == NULL || transition == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    command = journal_next(system, SU_AUDIO_SYSTEM_CMD_MUSIC);
    status = su_audio_music_transition(system->music, transition, sample_time, &at);
    if (boundary != NULL) {
        *boundary = at;
    }
    if (command != NULL) {
        command->transition = *transition;
        command->sample_time = sample_time;
        command->status = status;
    }
    return status;
}

uint32_t su_audio_system_bus_gain(SuAudioSystem *system, SuAudioBusId bus, float gain_db, uint32_t ramp_frames) {
    SuAudioSystemCommand *command;
    uint32_t status;
    if (system == NULL || system->buses == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    command = journal_next(system, SU_AUDIO_SYSTEM_CMD_BUS_GAIN);
    status = su_audio_bus_set_gain(system->buses, bus, gain_db, ramp_frames);
    if (command != NULL) {
        command->id = bus;
        command->value = gain_db;
        command->frames = ramp_frames;
        command->status = status;
    }
    return status;
}

/* ---- output ---- */

uint32_t su_audio_system_render(SuAudioSystem *system, float *out, uint32_t frames) {
    if (system == NULL || system->desc.output != SU_AUDIO_OUTPUT_OFFLINE) {
        return SU_E_INVALID_ARGUMENT;
    }
    return su_audio_events_render(system->events, out, frames);
}

static int pull_mode(const SuAudioSystem *system) {
    return system->desc.render == SU_AUDIO_RENDER_PULL && system->desc.output != SU_AUDIO_OUTPUT_OFFLINE;
}

void su_audio_system_lock(SuAudioSystem *system) {
    if (system != NULL && pull_mode(system)) {
        su_mutex_lock(&system->lock);
    }
}

void su_audio_system_unlock(SuAudioSystem *system) {
    if (system != NULL && pull_mode(system)) {
        su_mutex_unlock(&system->lock);
    }
}

/* PULL: renders quanta until at least `frames` are buffered (at most the ring capacity). The reader is the only
 * writer in this mode; the lock keeps the owner's calls out of the render. */
static void pull_render(SuAudioSystem *system, uint32_t frames) {
    uint64_t write;
    su_mutex_lock(&system->lock);
    write = atomic_load_explicit(&system->write_pos, memory_order_relaxed);
    for (;;) {
        uint64_t read = atomic_load_explicit(&system->read_pos, memory_order_acquire);
        if (write - read >= frames || write - read + QUANTUM > system->ring_frames) {
            break;
        }
        if (su_audio_events_render(system->events, system->ring + (size_t)(write % system->ring_frames) * 2u,
                QUANTUM) != SU_OK) {
            break;
        }
        write += QUANTUM;
        atomic_store_explicit(&system->write_pos, write, memory_order_release);
    }
    su_mutex_unlock(&system->lock);
}

uint32_t su_audio_system_ring_adaptive(SuAudioSystem *system, int enabled) {
    if (system == NULL || system->ring == NULL || pull_mode(system)) {
        return SU_E_INVALID_ARGUMENT;
    }
    system->adaptive = enabled != 0;
    system->ring_target = enabled ? 256u : system->ring_frames;
    system->adaptive_underruns = atomic_load(&system->underruns);
    system->adaptive_read = atomic_load(&system->read_pos);
    return SU_OK;
}

uint32_t su_audio_system_update(SuAudioSystem *system) {
    uint64_t write;
    if (system == NULL || system->ring == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (pull_mode(system)) {
        return SU_OK;
    }
    if (system->adaptive) {
        uint64_t underruns = atomic_load(&system->underruns);
        uint64_t read = atomic_load(&system->read_pos);
        if (underruns != system->adaptive_underruns) {
            uint32_t target = system->ring_target * 2u;
            system->ring_target = target < system->ring_frames ? target : system->ring_frames;
            system->adaptive_underruns = underruns;
            system->adaptive_read = read;
        } else if (read - system->adaptive_read >= (uint64_t)system->desc.events.sample_rate * 5u) {
            if (system->ring_target > 256u) {
                system->ring_target -= QUANTUM;
            }
            system->adaptive_read = read;
        }
    }
    write = atomic_load_explicit(&system->write_pos, memory_order_relaxed);
    for (;;) {
        uint64_t read = atomic_load_explicit(&system->read_pos, memory_order_acquire);
        uint32_t status;
        if (write - read + QUANTUM > system->ring_target) {
            break;
        }
        status = su_audio_events_render(system->events,
            system->ring + (size_t)(write % system->ring_frames) * 2u, QUANTUM);
        if (status != SU_OK) {
            return status;
        }
        write += QUANTUM;
        atomic_store_explicit(&system->write_pos, write, memory_order_release);
    }
    return SU_OK;
}

static uint32_t ring_read_chunk(SuAudioSystem *system, float *out, uint32_t frames) {
    uint64_t read;
    uint64_t write;
    uint32_t available;
    uint32_t done = 0u;
    if (system == NULL || system->ring == NULL || (out == NULL && frames != 0u)) {
        return 0u;
    }
    if (pull_mode(system)) {
        pull_render(system, frames);
    }
    read = atomic_load_explicit(&system->read_pos, memory_order_relaxed);
    write = atomic_load_explicit(&system->write_pos, memory_order_acquire);
    available = (uint32_t)(write - read);
    while (done < frames && done < available) {
        uint32_t at = (uint32_t)((read + done) % system->ring_frames);
        uint32_t take = system->ring_frames - at;
        if (take > frames - done) {
            take = frames - done;
        }
        if (take > available - done) {
            take = available - done;
        }
        memcpy(out + (size_t)done * 2u, system->ring + (size_t)at * 2u, (size_t)take * 2u * sizeof *out);
        done += take;
    }
    if (done < frames) {
        memset(out + (size_t)done * 2u, 0, (size_t)(frames - done) * 2u * sizeof *out);
        atomic_fetch_add_explicit(&system->underruns, frames - done, memory_order_relaxed);
    }
    atomic_store_explicit(&system->read_pos, read + done, memory_order_release);
    return done;
}

uint32_t su_audio_system_ring_read(SuAudioSystem *system, float *out, uint32_t frames) {
    uint32_t done = 0u;
    if (system == NULL || system->ring == NULL || (out == NULL && frames != 0u)) {
        return 0u;
    }
    if (!pull_mode(system)) {
        return ring_read_chunk(system, out, frames);
    }
    while (done < frames) {
        uint32_t take = frames - done;
        uint32_t valid = 0u;
        if (take > system->ring_frames) {
            take = system->ring_frames;
        }
        valid = ring_read_chunk(system, out + (size_t)done * 2u, take);
        done += valid;
        if (valid != take) {
            memset(out + (size_t)(done + take - valid) * 2u, 0,
                (size_t)(frames - done - take + valid) * 2u * sizeof *out);
            atomic_fetch_add(&system->underruns, frames - done - take + valid);
            break;
        }
    }
    return done;
}

/* ---- replay ---- */

static uint32_t apply(SuAudioSystem *system, const SuAudioSystemCommand *command) {
    SuAudioInstance instance = 0u;
    SuAudioEmitter emitter = 0u;
    uint64_t boundary = 0u;
    uint32_t status;
    switch (command->kind) {
    case SU_AUDIO_SYSTEM_CMD_POST:
        status = su_audio_system_post(system, &command->trigger, &instance, NULL);
        break;
    case SU_AUDIO_SYSTEM_CMD_STOP_INSTANCE:
    case SU_AUDIO_SYSTEM_CMD_STOP_EMITTER:
    case SU_AUDIO_SYSTEM_CMD_STOP_SCOPE:
        status = su_audio_system_stop(system, command->kind, command->target, command->sample_time, command->frames);
        break;
    case SU_AUDIO_SYSTEM_CMD_PARAM:
        status = su_audio_system_param_set(system, command->scope, command->id, command->value, command->sample_time);
        break;
    case SU_AUDIO_SYSTEM_CMD_SWITCH:
        status = su_audio_system_switch_set(system, command->scope, command->id, command->switch_value,
            command->sample_time);
        break;
    case SU_AUDIO_SYSTEM_CMD_EMITTER_CREATE:
        status = su_audio_system_emitter_create(system, &command->emitter, &emitter);
        if (status == SU_OK && emitter != command->target) {
            status = SU_E_SCHEMA_MISMATCH;
        }
        break;
    case SU_AUDIO_SYSTEM_CMD_EMITTER_UPDATE:
        status = su_audio_system_emitter_update(system, command->target, command->emitter.pos, command->emitter.vel);
        break;
    case SU_AUDIO_SYSTEM_CMD_EMITTER_DESTROY:
        status = su_audio_system_emitter_destroy(system, command->target);
        break;
    case SU_AUDIO_SYSTEM_CMD_LISTENER:
        status = su_audio_system_listener_set(system, &command->listener);
        break;
    case SU_AUDIO_SYSTEM_CMD_MUSIC:
        status = su_audio_system_music_transition(system, &command->transition, command->sample_time, &boundary);
        break;
    case SU_AUDIO_SYSTEM_CMD_BUS_GAIN:
        status = su_audio_system_bus_gain(system, command->id, command->value, command->frames);
        break;
    default:
        return SU_E_SCHEMA_MISMATCH;
    }
    /* a replayed command must reproduce its original outcome */
    return status == command->status ? SU_OK : SU_E_SCHEMA_MISMATCH;
}

static uint32_t render_to(SuAudioSystem *system, uint64_t target, float **out) {
    uint64_t now = produced(system);
    while (now < target) {
        uint64_t left = target - now;
        uint32_t take = left > 1024u ? 1024u : (uint32_t)left;
        float *dst = *out != NULL ? *out : system->scratch;
        uint32_t status = su_audio_events_render(system->events, dst, take);
        if (status != SU_OK) {
            return status;
        }
        if (*out != NULL) {
            *out += (size_t)take * 2u;
        }
        now += take;
    }
    return SU_OK;
}

uint32_t su_audio_system_replay(SuAudioSystem *system, const SuAudioSystemCommand *journal, uint32_t count,
                                uint64_t frames, float *out) {
    uint32_t index;
    uint32_t status = SU_OK;
    if (system == NULL || (journal == NULL && count != 0u) || system->desc.output != SU_AUDIO_OUTPUT_OFFLINE
        || frames < produced(system)) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (index = 0; index < count && journal[index].position <= frames && status == SU_OK; index++) {
        if (journal[index].position < produced(system)) {
            return SU_E_INVALID_ARGUMENT;
        }
        status = render_to(system, journal[index].position, &out);
        if (status == SU_OK) {
            status = apply(system, &journal[index]);
        }
    }
    return status == SU_OK ? render_to(system, frames, &out) : status;
}

uint32_t su_audio_system_journal(const SuAudioSystem *system, const SuAudioSystemCommand **journal,
                                 uint32_t *count) {
    if (system == NULL || journal == NULL || count == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *journal = system->journal;
    *count = system->journal_count;
    return system->journal_complete ? SU_OK : SU_E_RESOURCE_LIMIT;
}

/* ---- checkpoints ---- */

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t su_audio_system_checkpoint_save(SuAudioSystem *system, void *out, size_t capacity, size_t *written) {
    uint32_t status;
    size_t inner = 0u;
    if (written != NULL) {
        *written = 0u;
    }
    if (system == NULL || written == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    status = su_audio_events_checkpoint_save(system->events, NULL, 0u, &inner);
    if (status != SU_OK) {
        return status;
    }
    *written = SYSTEM_HEADER + inner;
    if (out == NULL) {
        return SU_OK;
    }
    if (capacity < *written) {
        return SU_E_RESOURCE_LIMIT;
    }
    memset(out, 0, SYSTEM_HEADER);
    put32((uint8_t *)out, SYSTEM_MAGIC);
    put32((uint8_t *)out + 4, 1u);
    put32((uint8_t *)out + 8, system->journal_count); /* commands issued before the save */
    put32((uint8_t *)out + 12, system->journal_complete ? 1u : 0u);
    return su_audio_events_checkpoint_save(system->events, (uint8_t *)out + SYSTEM_HEADER, inner, &inner);
}

uint32_t su_audio_system_checkpoint_load(SuAudioSystem *system, const void *data, size_t bytes) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t status;
    uint64_t done;
    if (system == NULL || p == NULL || bytes < SYSTEM_HEADER || get32(p) != SYSTEM_MAGIC || get32(p + 4) != 1u) {
        return SU_E_SCHEMA_MISMATCH;
    }
    if (system->device != NULL) {
        return SU_E_INVALID_ARGUMENT; /* the device thread reads the ring; load into a RING or OFFLINE system */
    }
    status = su_audio_events_checkpoint_load(system->events, p + SYSTEM_HEADER, bytes - SYSTEM_HEADER);
    if (status != SU_OK) {
        return status;
    }
    done = produced(system);
    atomic_store_explicit(&system->write_pos, done, memory_order_release);
    atomic_store_explicit(&system->read_pos, done, memory_order_release);
    system->journal_count = 0u;
    system->journal_complete = 0;
    return SU_OK;
}

uint32_t su_audio_system_checkpoint_verify(const SuAudioSystemDesc *desc, const SuAudioSystemCommand *journal,
    uint32_t count, const void *checkpoint, size_t bytes, uint32_t verify_frames) {
    SuAudioSystemDesc offline;
    SuAudioSystem *replayed = NULL;
    SuAudioSystem *restored = NULL;
    SuAudioEventsCheckpointInfo info;
    const uint8_t *p = (const uint8_t *)checkpoint;
    float *a = NULL;
    float *b = NULL;
    uint32_t before;
    uint32_t status;
    if (desc == NULL || p == NULL || bytes < SYSTEM_HEADER || get32(p) != SYSTEM_MAGIC || verify_frames == 0u
        || verify_frames > OFFLINE_MAX_FRAMES) {
        return SU_E_INVALID_ARGUMENT;
    }
    before = get32(p + 8);
    if (get32(p + 12) != 1u || before > count) {
        return SU_E_INVALID_ARGUMENT; /* the journal does not start at the first frame */
    }
    status = su_audio_events_checkpoint_info(p + SYSTEM_HEADER, bytes - SYSTEM_HEADER, &info);
    if (status != SU_OK) {
        return status;
    }
    offline = *desc;
    offline.output = SU_AUDIO_OUTPUT_OFFLINE;
    offline.journal_capacity = 0u;
    offline.telemetry_capacity = 0u;
    offline.inputs = NULL; /* host input is not journaled */
    offline.input_count = 0u;
    a = calloc((size_t)verify_frames * 2u, sizeof *a);
    b = calloc((size_t)verify_frames * 2u, sizeof *b);
    status = a == NULL || b == NULL ? SU_E_RESOURCE_LIMIT : SU_OK;
    if (status == SU_OK) {
        status = su_audio_system_create(&offline, &replayed, NULL);
    }
    if (status == SU_OK) {
        status = su_audio_system_create(&offline, &restored, NULL);
    }
    if (status == SU_OK) {
        status = su_audio_system_replay(replayed, journal, before, info.delivered_frames, NULL);
    }
    if (status == SU_OK) {
        status = su_audio_system_replay(replayed, journal + before, count - before,
            info.delivered_frames + verify_frames, a);
    }
    if (status == SU_OK) {
        status = su_audio_events_checkpoint_load(restored->events, p + SYSTEM_HEADER, bytes - SYSTEM_HEADER);
    }
    if (status == SU_OK) {
        status = su_audio_system_replay(restored, journal + before, count - before,
            info.delivered_frames + verify_frames, b);
    }
    if (status == SU_OK && memcmp(a, b, (size_t)verify_frames * 2u * sizeof *a) != 0) {
        status = SU_E_SCHEMA_MISMATCH;
    }
    su_audio_system_destroy(replayed);
    su_audio_system_destroy(restored);
    free(a);
    free(b);
    return status;
}

/* ---- control-plane methods ---- */

typedef struct Text {
    char *out;
    size_t capacity;
    size_t pos;
    int overflow;
} Text;

static void text_add(Text *text, const char *fmt, ...) {
    va_list ap;
    int n;
    if (text->overflow) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(text->out + text->pos, text->capacity - text->pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= text->capacity - text->pos) {
        text->overflow = 1;
        return;
    }
    text->pos += (size_t)n;
}

/* Appends a component JSON writer's output. */
static void text_json(Text *text, uint32_t status, size_t written) {
    if (text->overflow) {
        return;
    }
    if (status != SU_OK) {
        text->overflow = 1;
        return;
    }
    text->pos += written;
    while (text->pos > 0u && (text->out[text->pos - 1u] == '\n' || text->out[text->pos - 1u] == '\0')) {
        text->pos--;
    }
}

static uint32_t method_inspect(SuAudioSystem *system, Text *text) {
    uint32_t json_status;
    SuAudioInspectSnapshot snapshot;
    SuAudioSystemStats stats;
    size_t written = 0u;
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.struct_size = sizeof snapshot;
    snapshot.instances = system->inspect_instances;
    snapshot.instance_capacity = INSPECT_ROWS;
    snapshot.voices = system->inspect_voices;
    snapshot.voice_capacity = INSPECT_ROWS;
    snapshot.emitters = system->inspect_emitters;
    snapshot.emitter_capacity = INSPECT_ROWS;
    snapshot.buses = system->inspect_buses;
    snapshot.bus_capacity = INSPECT_ROWS;
    if (su_audio_inspect(system->events, system->scene, system->desc.assets, &snapshot) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }
    su_audio_system_stats(system, &stats);
    text_add(text, "{\"schema\":\"su_audio_system_inspect_v1\",\"clock\":%llu,\"rendered_frames\":%llu,"
        "\"consumed_frames\":%llu,\"ring_fill\":%u,\"ring_capacity\":%u,\"underrun_frames\":%llu,"
        "\"journal\":%u,\"journal_dropped\":%llu,\"runtime_bytes\":%llu,",
        (unsigned long long)stats.clock, (unsigned long long)stats.rendered_frames,
        (unsigned long long)stats.consumed_frames, stats.ring_fill, stats.ring_capacity,
        (unsigned long long)stats.underrun_frames, stats.journal_count, (unsigned long long)stats.journal_dropped,
        (unsigned long long)stats.runtime_bytes);
    text_add(text, "\"ring_target\":%u,\"device\":{\"callbacks\":%llu,\"callback_total_us\":%llu,"
        "\"callback_max_us\":%llu,\"callback_over_budget\":%llu,\"interval_max_us\":%llu,"
        "\"empty_buffers\":%llu,\"wait_timeouts\":%llu,\"backend_errors\":%llu,\"stream_latency_ms\":%.3f,"
        "\"callback_us\":[", stats.ring_target, (unsigned long long)stats.device.callbacks,
        (unsigned long long)stats.device.callback_total_us, (unsigned long long)stats.device.callback_max_us,
        (unsigned long long)stats.device.callback_over_budget, (unsigned long long)stats.device.interval_max_us,
        (unsigned long long)stats.device.empty_buffers, (unsigned long long)stats.device.wait_timeouts,
        (unsigned long long)stats.device.backend_errors, stats.device.stream_latency_ms);
    {
        uint32_t bin;
        for (bin = 0u; bin < SU_AUDIO_DEVICE_HISTOGRAM_BINS; bin++) {
            text_add(text, "%s%llu", bin != 0u ? "," : "", (unsigned long long)stats.device.callback_us[bin]);
        }
        text_add(text, "],\"interval_us\":[");
        for (bin = 0u; bin < SU_AUDIO_DEVICE_HISTOGRAM_BINS; bin++) {
            text_add(text, "%s%llu", bin != 0u ? "," : "", (unsigned long long)stats.device.interval_us[bin]);
        }
    }
    text_add(text, "]},\"events\":");
    if (!text->overflow) {
        json_status = su_audio_inspect_json(&snapshot, text->out + text->pos, text->capacity - text->pos, &written);
        text_json(text, json_status, written);
    }
    text_add(text, ",\"voices\":");
    if (!text->overflow) {
        json_status = su_audio_inspect_voice_pool_json(system->pool, text->out + text->pos,
            text->capacity - text->pos, &written);
        text_json(text, json_status, written);
    }
    text_add(text, ",\"buses\":");
    if (system->buses == NULL) {
        text_add(text, "null");
    } else if (!text->overflow) {
        json_status = su_audio_inspect_bus_json(system->buses, 0, text->out + text->pos, text->capacity - text->pos,
            &written);
        text_json(text, json_status, written);
    }
    text_add(text, ",\"params\":");
    if (system->params == NULL) {
        text_add(text, "null");
    } else if (!text->overflow) {
        json_status = su_audio_inspect_params_json(system->params, text->out + text->pos,
            text->capacity - text->pos, &written);
        text_json(text, json_status, written);
    }
    text_add(text, ",\"cluster\":");
    if (system->cluster == NULL) {
        text_add(text, "null");
    } else if (!text->overflow) {
        json_status = su_audio_inspect_cluster_json(system->cluster, text->out + text->pos,
            text->capacity - text->pos, &written);
        text_json(text, json_status, written);
    }
    text_add(text, "}");
    return SU_OK;
}

static uint32_t method_trace_read(SuAudioSystem *system, Text *text) {
    uint32_t json_status;
    uint32_t count = 0u;
    size_t written = 0u;
    if (system->telemetry == NULL) {
        return SU_E_UNSUPPORTED_METHOD;
    }
    if (su_audio_trace_drain(system->telemetry, system->trace_rows, TRACE_ROWS, &count) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }
    json_status = su_audio_trace_json(system->trace_rows, count, text->out, text->capacity, &written);
    text_json(text, json_status, written);
    return SU_OK;
}

static uint32_t method_trace_stats(SuAudioSystem *system, Text *text) {
    SuAudioTelemetryStats stats;
    uint32_t reason;
    if (system->telemetry == NULL) {
        return SU_E_UNSUPPORTED_METHOD;
    }
    su_audio_trace_stats(system->telemetry, &stats);
    text_add(text, "{\"schema\":\"su_audio_trace_stats_v1\",\"capacity\":%u,\"queued\":%u,\"recorded\":%llu,"
        "\"dropped\":%llu,\"drained\":%llu,\"reasons\":{", stats.capacity, stats.queued,
        (unsigned long long)stats.recorded, (unsigned long long)stats.dropped, (unsigned long long)stats.drained);
    for (reason = 0; reason < SU_AUDIO_TRACE_REASON_COUNT; reason++) {
        text_add(text, "%s\"%s\":%llu", reason ? "," : "", su_audio_trace_reason_name(reason),
            (unsigned long long)stats.reasons[reason]);
    }
    text_add(text, "}}");
    return SU_OK;
}

static int params_frames(const char *params, size_t len, uint64_t *frames) {
    char buffer[256];
    const char *key;
    char *end = NULL;
    unsigned long long value;
    if (params == NULL || len == 0u) {
        return 0;
    }
    if (len >= sizeof buffer) {
        return -1;
    }
    memcpy(buffer, params, len);
    buffer[len] = '\0';
    key = strstr(buffer, "\"frames\"");
    if (key == NULL) {
        return 0;
    }
    key += 8;
    while (*key == ' ') {
        key++;
    }
    if (*key++ != ':') {
        return -1;
    }
    while (*key == ' ') {
        key++;
    }
    if (*key < '0' || *key > '9') {
        return -1;
    }
    value = strtoull(key, &end, 10);
    *frames = value;
    return end != NULL && end != key ? 1 : -1;
}

static uint32_t method_render_offline(SuAudioSystem *system, const char *params, size_t params_len, Text *text) {
    SuAudioSystemDesc offline;
    SuAudioSystem *fresh = NULL;
    SuSha256 sha;
    uint8_t digest[32];
    char hex[65];
    uint64_t frames = produced(system);
    float *pcm;
    double lufs = 0.0;
    double true_peak = 0.0;
    double peak = 0.0;
    uint64_t index;
    uint32_t status;
    int parsed = params_frames(params, params_len, &frames);
    if (parsed < 0 || frames == 0u || frames > OFFLINE_MAX_FRAMES) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (!system->journal_complete || system->journal == NULL) {
        return SU_E_RESOURCE_LIMIT; /* the journal cannot reproduce the session */
    }
    offline = system->desc;
    offline.output = SU_AUDIO_OUTPUT_OFFLINE;
    offline.journal_capacity = 0u;
    offline.telemetry_capacity = 0u;
    offline.inputs = NULL; /* host input is not journaled */
    offline.input_count = 0u;
    pcm = calloc((size_t)frames * 2u, sizeof *pcm);
    if (pcm == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    status = su_audio_system_create(&offline, &fresh, NULL);
    if (status == SU_OK) {
        status = su_audio_system_replay(fresh, system->journal, system->journal_count, frames, pcm);
    }
    if (status == SU_OK) {
        status = su_audio_measure_stereo(pcm, frames, system->desc.events.sample_rate, &lufs, &true_peak);
    }
    if (status == SU_OK) {
        for (index = 0; index < frames * 2u; index++) {
            double magnitude = fabs((double)pcm[index]);
            peak = magnitude > peak ? magnitude : peak;
        }
        su_sha256_init(&sha);
        su_sha256_update(&sha, pcm, (size_t)frames * 2u * sizeof *pcm);
        su_sha256_final(&sha, digest);
        su_sha256_hex(digest, hex);
        text_add(text, "{\"schema\":\"su_audio_system_offline_v1\",\"profile\":\"%s\",\"sample_rate\":%u,"
            "\"frames\":%llu,\"commands\":%u,\"pcm_sha256\":\"%s\",\"sample_peak\":%.6f,\"true_peak\":%.6f,"
            "\"integrated_lufs\":", SU_AUDIO_OFFLINE_PROFILE, system->desc.events.sample_rate,
            (unsigned long long)frames, system->journal_count, hex, peak, true_peak);
        if (isfinite(lufs)) {
            text_add(text, "%.3f}", lufs);
        } else {
            text_add(text, "null}");
        }
    }
    su_audio_system_destroy(fresh);
    free(pcm);
    return status;
}

static const char *const methods[] = {"audio.inspect", "audio.trace.read", "audio.trace.stats", "audio.render.offline"};

const char *su_audio_system_method(uint32_t index) {
    return index < sizeof methods / sizeof methods[0] ? methods[index] : NULL;
}

uint32_t su_audio_system_control(SuAudioSystem *system, const char *method, const char *params, size_t params_len,
                                 char *out, size_t capacity, size_t *written) {
    Text text;
    uint32_t status;
    if (written != NULL) {
        *written = 0u;
    }
    if (system == NULL || method == NULL || out == NULL || capacity == 0u || written == NULL
        || (params == NULL && params_len != 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    text.out = out;
    text.capacity = capacity;
    text.pos = 0u;
    text.overflow = 0;
    out[0] = '\0';
    if (strcmp(method, "audio.inspect") == 0) {
        status = method_inspect(system, &text);
    } else if (strcmp(method, "audio.trace.read") == 0) {
        status = method_trace_read(system, &text);
    } else if (strcmp(method, "audio.trace.stats") == 0) {
        status = method_trace_stats(system, &text);
    } else if (strcmp(method, "audio.render.offline") == 0) {
        status = method_render_offline(system, params, params_len, &text);
    } else {
        return SU_E_UNSUPPORTED_METHOD;
    }
    if (status == SU_OK && text.overflow) {
        status = SU_E_RESOURCE_LIMIT;
    }
    if (status != SU_OK) {
        out[0] = '\0';
        return status;
    }
    out[text.pos] = '\0';
    *written = text.pos;
    return SU_OK;
}
