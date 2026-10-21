#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_event.h"
#include "su_audio_event_int.h"
#include "su_audio_graph_int.h"
#include "su_audio_event_view.h"
#include "su_audio_voice_int.h"
#include "su_audio_bus_int.h"
#include "su_audio_parameter_int.h"
#include "su_audio_cluster_int.h"
#include "su_audio_state.h"

enum { EVENT_CHECKPOINT_LAYOUT = 2 };

typedef struct EventLeaf {
    SuAudioGraphLeaf data;
    SuAudioVoiceDesc voice_desc;
    SuAudioVoice voice;
    uint64_t at;
    uint32_t state;
    float lowpass_hz;
    SuAudioLogicalVoice logical;   /* integrated mode */
    const SuAudioClip *clip;
    float ctl_gain;
    float ctl_pitch;
    SuVec3f ctl_dir;               /* listener-relative direction (zero when not spatial) */
    float ctl_distance;
} EventLeaf;

typedef struct EventInstance {
    uint32_t generation;
    uint32_t state;
    uint32_t count;
    uint32_t scope;
    uint32_t event_id;     /* the posted event: SU_AUDIO_LIMIT_EVENT scopes of its voices */
    uint64_t order;
    uint64_t start;
    uint64_t sequence;
    SuAudioEmitter emitter;
    EventLeaf *leaves;
} EventInstance;

typedef struct EventStop {
    uint64_t target;
    uint64_t at;
    uint32_t type;
    uint32_t release_frames;
} EventStop;

struct SuAudioEvents {
    SuAudioEventsDesc desc;
    SuAudioAssets *assets;
    SuAudioScene *scene;
    SuAudioGraph *graph;
    SuAudioMixer *mixer;
    EventInstance *instances;
    EventLeaf *leaves;
    SuAudioGraphLeaf *scratch;
    EventStop *stops;
    uint32_t stop_count;
    uint64_t order;
    SuAudioCommitCursor cursor;
    SuAudioEventStats stats;
    float quantum[SU_AUDIO_EVENT_QUANTUM * 2u];
    uint32_t cached;
    uint32_t consumed;
    /* integrated mode */
    SuAudioVoicePool *pool;
    SuAudioVoicePolicyRule *policies;
    uint32_t policy_count;
    SuAudioVoicePolicy default_policy;
    uint64_t trigger_seq;
    float *route_mem;
    float **routes;
    uint32_t route_count;
    SuAudioParams *params;
    SuAudioGraphParameter *param_values;
    SuAudioGraphSwitch *switch_values;
    uint32_t param_count;
    uint32_t switch_count;
    SuAudioBusGain *bus_gains;
    uint32_t bus_gain_capacity;
    SuAudioBusTree *buses;
    SuAudioBusId *route_ids;
    uint32_t root_route;
    SuAudioMusic *music;
    uint32_t music_bus_id;
    float limiter_knee;
    float music_buffer[SU_AUDIO_EVENT_QUANTUM * 2u];
    float doppler;
    float speed_of_sound;
    SuAudioBusInput inputs[SU_AUDIO_EVENTS_MAX_INPUTS];
    uint32_t input_count;
    float input_buffer[SU_AUDIO_EVENT_QUANTUM * 2u];
    SuAudioCluster *cluster;
    uint32_t cluster_inputs;
    SuAudioVoiceCapture *captures;
    float *capture_mem;
    uint32_t capture_count;
    SuAudioClusterObject *objects;
    uint32_t object_capacity;
    SuAudioBusId *bed_ids;
    float *bed_mem;
    uint32_t bed_count;
    EventLeaf **leaf_by_voice;
    uint32_t leaf_map_size;
    uint32_t *due;
    float cluster_mix[SU_AUDIO_EVENT_QUANTUM * 2u];
};

static SuAudioInstance instance_handle(uint32_t slot, uint32_t generation) {
    return ((uint64_t)generation << 32) | ((uint64_t)slot + 1u);
}

static EventInstance *find_instance(const SuAudioEvents *events, SuAudioInstance handle) {
    uint32_t slot = (uint32_t)handle;
    if (slot == 0u || slot > events->desc.max_instances) {
        return NULL;
    }
    if (events->instances[slot - 1u].generation != (uint32_t)(handle >> 32)
        || events->instances[slot - 1u].state != 1u) {
        return NULL;
    }
    return &events->instances[slot - 1u];
}

uint32_t su_audio_events_create(const SuAudioEventsDesc *desc, SuAudioAssets *assets,
                                SuAudioScene *scene, SuAudioGraph *graph, SuAudioEvents **out) {
    SuAudioEvents *events;
    SuAudioMixerDesc mixer_desc;
    size_t leaf_count;
    uint32_t status;
    uint32_t index;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (desc == NULL || desc->struct_size != sizeof *desc || assets == NULL || graph == NULL
        || desc->max_instances == 0u || desc->max_instances > 4096u
        || desc->max_leaves_per_instance == 0u || desc->max_leaves_per_instance > 4096u
        || desc->max_logical_voices == 0u || desc->max_logical_voices > 4096u
        || desc->command_capacity == 0u || desc->command_capacity > 65536u
        || desc->sample_rate < 1000u || desc->sample_rate > 384000u
        || desc->sample_rate != su_audio_graph_sample_rate(graph)
        || desc->session_generation == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    leaf_count = (size_t)desc->max_instances * desc->max_leaves_per_instance;
    if (leaf_count > SIZE_MAX / sizeof(EventLeaf)) {
        return SU_E_RESOURCE_LIMIT;
    }
    events = calloc(1, sizeof *events);
    if (events == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    events->desc = *desc;
    events->instances = calloc(desc->max_instances, sizeof *events->instances);
    events->leaves = calloc(leaf_count, sizeof *events->leaves);
    events->scratch = calloc(desc->max_leaves_per_instance, sizeof *events->scratch);
    events->stops = calloc(desc->command_capacity, sizeof *events->stops);
    if (events->instances == NULL || events->leaves == NULL || events->scratch == NULL || events->stops == NULL) {
        su_audio_events_destroy(events);
        return SU_E_RESOURCE_LIMIT;
    }
    memset(&mixer_desc, 0, sizeof mixer_desc);
    mixer_desc.struct_size = sizeof mixer_desc;
    mixer_desc.sample_rate = desc->sample_rate;
    mixer_desc.max_voices = desc->max_logical_voices;
    mixer_desc.command_capacity = desc->max_logical_voices * 4u + 16u;
    status = su_audio_mixer_create(&mixer_desc, &events->mixer);
    if (status != SU_OK) {
        su_audio_events_destroy(events);
        return status;
    }
    events->assets = assets;
    events->scene = scene;
    status = su_audio_graph_retain(graph);
    if (status != SU_OK) {
        su_audio_events_destroy(events);
        return status;
    }
    events->graph = graph;
    events->cursor.session_generation = desc->session_generation;
    events->stats.render_clock = desc->start_sample;
    events->stats.runtime_bytes = sizeof *events + (uint64_t)desc->max_instances * sizeof *events->instances
        + (uint64_t)leaf_count * sizeof *events->leaves
        + (uint64_t)desc->max_leaves_per_instance * sizeof *events->scratch
        + (uint64_t)desc->command_capacity * sizeof *events->stops;
    for (index = 0; index < desc->max_instances; index++) {
        events->instances[index].leaves = events->leaves + (size_t)index * desc->max_leaves_per_instance;
    }
    *out = events;
    return SU_OK;
}

void su_audio_events_destroy(SuAudioEvents *events) {
    uint32_t slot;
    if (events == NULL) {
        return;
    }
    su_audio_mixer_destroy(events->mixer);
    if (events->assets != NULL && events->instances != NULL) {
        for (slot = 0; slot < events->desc.max_instances; slot++) {
            uint32_t index;
            for (index = 0; index < events->instances[slot].count; index++) {
                (void)su_audio_asset_release(events->assets, events->instances[slot].leaves[index].data.asset);
            }
        }
    }
    if (events->graph != NULL) {
        su_audio_graph_release(events->graph);
    }
    if (events->buses != NULL && events->bed_ids != NULL) {
        uint32_t bed;
        for (bed = 0; bed < events->bed_count; bed++) {
            (void)su_audio_bus_capture(events->buses, events->bed_ids[bed], NULL);
        }
    }
    if (events->pool != NULL && events->captures != NULL) {
        (void)su_audio_voice_pool_capture(events->pool, NULL, 0u);
    }
    free(events->captures);
    free(events->capture_mem);
    free(events->objects);
    free(events->bed_ids);
    free(events->bed_mem);
    free(events->leaf_by_voice);
    free(events->due);
    free(events->param_values);
    free(events->switch_values);
    free(events->bus_gains);
    free(events->route_ids);
    free(events->routes);
    free(events->route_mem);
    free(events->policies);
    free(events->stops);
    free(events->scratch);
    free(events->leaves);
    free(events->instances);
    free(events);
}

void su_audio_events_update(SuAudioEvents *events) {
    uint32_t slot;
    if (events == NULL) {
        return;
    }
    for (slot = 0; slot < events->desc.max_instances; slot++) {
        EventInstance *instance = &events->instances[slot];
        uint32_t index;
        if (instance->state != 2u) {
            continue;
        }
        for (index = 0; index < instance->count; index++) {
            (void)su_audio_asset_release(events->assets, instance->leaves[index].data.asset);
        }
        if (events->params != NULL) {
            SuAudioParamScope scope;
            scope.kind = SU_AUDIO_SCOPE_INSTANCE;
            scope.key = instance_handle(slot, instance->generation);
            (void)su_audio_param_scope_clear(events->params, scope);
        }
        events->stats.logical_voices -= instance->count;
        events->stats.instances--;
        instance->count = 0u;
        instance->state = 0u;
    }
}

uint32_t su_audio_event_post(SuAudioEvents *events, const SuAudioTrigger *trigger,
                             SuAudioInstance *out, SuAudioGraphDiagnostic *diagnostic) {
    size_t count = 0u;
    size_t index;
    uint32_t slot;
    uint32_t status;
    EventInstance *instance = NULL;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = 0u;
    if (events == NULL || trigger == NULL || trigger->struct_size != sizeof *trigger
        || trigger->session_generation != events->cursor.session_generation || trigger->commit_seq == UINT64_MAX) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (trigger->commit_seq < events->cursor.next_seq) {
        events->stats.duplicates++;
        return SU_E_STALE_REVISION;
    }
    if (trigger->sample_time < events->stats.render_clock) {
        events->stats.late_commands++;
        return SU_E_INVALID_ARGUMENT;
    }
    su_audio_events_update(events);
    for (slot = 0; slot < events->desc.max_instances; slot++) {
        if (events->instances[slot].state == 0u && events->instances[slot].generation != UINT32_MAX) {
            instance = &events->instances[slot];
            break;
        }
    }
    if (instance == NULL || events->order == UINT64_MAX) {
        events->stats.refused++;
        return SU_E_RESOURCE_LIMIT;
    }
    if (events->params != NULL) {
        SuAudioGraphExpandState state;
        SuAudioParamTarget target;
        target.emitter = trigger->emitter;
        target.instance = instance_handle(slot,
            instance->generation + 1u == 0u ? 1u : instance->generation + 1u);
        status = su_audio_param_expand_state(events->params, target, trigger->sample_time, trigger->sample_time,
            events->param_values, events->param_count, events->switch_values, events->switch_count, &state);
        if (status == SU_OK) {
            status = su_audio_graph_expand_live(events->graph, trigger->event_id, events->desc.project_seed,
                trigger->emitter, trigger->commit_seq, &state, events->params, target,
                trigger->sample_time, trigger->sample_time, events->scratch,
                events->desc.max_leaves_per_instance, &count, diagnostic);
        }
    } else {
        status = su_audio_graph_expand(events->graph, trigger->event_id, events->desc.project_seed,
            trigger->emitter, trigger->commit_seq, events->scratch, events->desc.max_leaves_per_instance, &count,
            diagnostic);
    }
    if (status != SU_OK) {
        events->stats.refused++;
        return status;
    }
    if (count > events->desc.max_logical_voices - events->stats.logical_voices) {
        events->stats.refused++;
        return SU_E_RESOURCE_LIMIT;
    }
    for (index = 0; index < count; index++) {
        EventLeaf *leaf = &instance->leaves[index];
        const SuAudioClip *clip = NULL;
        memset(leaf, 0, sizeof *leaf);
        leaf->data = events->scratch[index];
        if (leaf->data.delay_frames > UINT64_MAX - trigger->sample_time) {
            status = SU_E_INVALID_ARGUMENT;
            break;
        }
        status = su_audio_asset_acquire(events->assets, leaf->data.asset, &clip);
        if (status != SU_OK) {
            break;
        }
        instance->count++;
        leaf->clip = clip;
        leaf->voice_desc.clip = clip;
        leaf->voice_desc.gain = leaf->data.gain;
        leaf->voice_desc.pitch = leaf->data.pitch;
        leaf->voice_desc.flags = leaf->data.flags;
        leaf->at = trigger->sample_time + leaf->data.delay_frames;
        if (trigger->emitter != 0u) {
            status = su_audio_scene_resolve(events->scene, trigger->emitter, &leaf->voice_desc, &leaf->lowpass_hz);
            if (status != SU_OK) {
                break;
            }
        }
    }
    if (status != SU_OK || count == 0u) {
        for (index = 0; index < instance->count; index++) {
            (void)su_audio_asset_release(events->assets, instance->leaves[index].data.asset);
        }
        instance->count = 0u;
        events->stats.refused++;
        return status != SU_OK ? status : SU_E_SCHEMA_MISMATCH;
    }
    instance->generation++;
    if (instance->generation == 0u) {
        instance->generation = 1u;
    }
    instance->state = 1u;
    instance->scope = trigger->scope_id;
    instance->event_id = trigger->event_id;
    instance->emitter = trigger->emitter;
    instance->order = ++events->order;
    instance->start = trigger->sample_time;
    instance->sequence = trigger->commit_seq;
    if (events->params != NULL) {
        memcpy(events->switch_values + ((size_t)slot + 1u) * events->switch_count,
            events->switch_values, (size_t)events->switch_count * sizeof *events->switch_values);
    }
    events->stats.instances++;
    events->stats.logical_voices += (uint32_t)count;
    events->stats.admitted++;
    events->cursor.next_seq = trigger->commit_seq + 1u;
    *out = instance_handle(slot, instance->generation);
    return SU_OK;
}

static uint32_t queue_stop(SuAudioEvents *events, uint32_t type, uint64_t target,
                            uint64_t sample_time, uint32_t release_frames) {
    EventStop *stop;
    if (events == NULL || sample_time > UINT64_MAX - release_frames) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (sample_time < events->stats.render_clock) {
        events->stats.late_commands++;
        return SU_E_INVALID_ARGUMENT;
    }
    if (events->stop_count == events->desc.command_capacity) {
        events->stats.refused++;
        return SU_E_RESOURCE_LIMIT;
    }
    stop = &events->stops[events->stop_count++];
    stop->type = type;
    stop->target = target;
    stop->at = sample_time;
    stop->release_frames = release_frames;
    return SU_OK;
}

uint32_t su_audio_instance_stop(SuAudioEvents *events, SuAudioInstance instance,
                                uint64_t sample_time, uint32_t release_frames) {
    if (events == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (find_instance(events, instance) == NULL) {
        return SU_E_STALE_HANDLE;
    }
    return queue_stop(events, 1u, instance, sample_time, release_frames);
}

uint32_t su_audio_emitter_stop(SuAudioEvents *events, SuAudioEmitter emitter,
                               uint64_t sample_time, uint32_t release_frames) {
    return emitter == 0u ? SU_E_INVALID_ARGUMENT : queue_stop(events, 2u, emitter, sample_time, release_frames);
}

uint32_t su_audio_scope_stop(SuAudioEvents *events, uint32_t scope_id,
                             uint64_t sample_time, uint32_t release_frames) {
    return queue_stop(events, 3u, scope_id, sample_time, release_frames);
}

static void collect_finished(SuAudioEvents *events) {
    uint32_t slot;
    su_audio_update(events->mixer);
    for (slot = 0; slot < events->desc.max_instances; slot++) {
        EventInstance *instance = &events->instances[slot];
        uint32_t index;
        uint32_t remaining = 0u;
        if (instance->state != 1u) {
            continue;
        }
        for (index = 0; index < instance->count; index++) {
            EventLeaf *leaf = &instance->leaves[index];
            if (leaf->state == 1u && !su_audio_voice_active(events->mixer, leaf->voice)) {
                leaf->state = 2u;
            }
            if (leaf->state != 2u) {
                remaining++;
            }
        }
        if (remaining == 0u) {
            instance->state = 2u;
        }
    }
}

static void apply_stops(SuAudioEvents *events, uint64_t now) {
    uint32_t command = 0u;
    while (command < events->stop_count) {
        EventStop stop = events->stops[command];
        uint32_t slot;
        if (stop.at != now) {
            command++;
            continue;
        }
        for (slot = 0; slot < events->desc.max_instances; slot++) {
            EventInstance *instance = &events->instances[slot];
            uint32_t index;
            if (instance->state != 1u || !((stop.type == 1u && stop.target == instance_handle(slot, instance->generation))
                || (stop.type == 2u && stop.target == instance->emitter)
                || (stop.type == 3u && stop.target == instance->scope))) {
                continue;
            }
            for (index = 0; index < instance->count; index++) {
                EventLeaf *leaf = &instance->leaves[index];
                if (leaf->state == 0u) {
                    leaf->state = 2u;
                } else if (leaf->state == 1u) {
                    uint32_t status = su_audio_event_voice_release(events->mixer, leaf->voice, stop.release_frames);
                    if (status != SU_OK) {
                        events->stats.render_failures++;
                    } else if (stop.release_frames == 0u) {
                        leaf->state = 2u;
                    }
                }
            }
        }
        events->stop_count--;
        memmove(&events->stops[command], &events->stops[command + 1u],
            (size_t)(events->stop_count - command) * sizeof *events->stops);
    }
}

static uint64_t start_due(SuAudioEvents *events, uint64_t now, uint64_t next) {
    uint64_t previous = 0u;
    uint32_t visited;
    for (visited = 0; visited < events->desc.max_instances; visited++) {
        EventInstance *instance = NULL;
        uint32_t slot;
        uint32_t index;
        for (slot = 0; slot < events->desc.max_instances; slot++) {
            EventInstance *candidate = &events->instances[slot];
            if (candidate->state == 1u && candidate->order > previous
                && (instance == NULL || candidate->order < instance->order)) {
                instance = candidate;
            }
        }
        if (instance == NULL) {
            break;
        }
        previous = instance->order;
        for (index = 0; index < instance->count; index++) {
            EventLeaf *leaf = &instance->leaves[index];
            if (leaf->state == 0u && leaf->at == now) {
                uint32_t status = su_audio_play(events->mixer, &leaf->voice_desc, &leaf->voice);
                leaf->state = status == SU_OK ? 1u : 2u;
                if (status != SU_OK) {
                    events->stats.render_failures++;
                } else if (leaf->lowpass_hz > 0.0f) {
                    status = su_audio_voice_set_lowpass(events->mixer, leaf->voice, leaf->lowpass_hz);
                    if (status != SU_OK) {
                        events->stats.render_failures++;
                    }
                }
            } else if (leaf->state == 0u && leaf->at < next) {
                next = leaf->at;
            }
        }
    }
    return next;
}

static void refresh_scene(SuAudioEvents *events) {
    uint32_t slot;
    if (events->scene == NULL) {
        return;
    }
    if (su_audio_scene_listener_apply(events->scene, events->mixer) != SU_OK) {
        events->stats.render_failures++;
    }
    for (slot = 0; slot < events->desc.max_instances; slot++) {
        EventInstance *instance = &events->instances[slot];
        uint32_t index;
        if (instance->state != 1u || instance->emitter == 0u) {
            continue;
        }
        for (index = 0; index < instance->count; index++) {
            EventLeaf *leaf = &instance->leaves[index];
            uint32_t status;
            if (leaf->state == 2u) {
                continue;
            }
            leaf->voice_desc.gain = leaf->data.gain;
            leaf->voice_desc.min_distance = 0.0f;
            leaf->voice_desc.max_distance = 0.0f;
            leaf->voice_desc.rolloff = 0.0f;
            status = su_audio_scene_resolve(events->scene, instance->emitter, &leaf->voice_desc, &leaf->lowpass_hz);
            if (status != SU_OK) {
                if (leaf->state == 1u) {
                    (void)su_audio_event_voice_release(events->mixer, leaf->voice, 0u);
                } else {
                    leaf->state = 2u;
                }
            } else if (leaf->state == 1u) {
                status = su_audio_event_voice_spatial(events->mixer, leaf->voice, &leaf->voice_desc);
                if (status == SU_OK) {
                    status = su_audio_voice_set_lowpass(events->mixer, leaf->voice, leaf->lowpass_hz);
                }
                if (status != SU_OK) {
                    events->stats.render_failures++;
                }
            }
        }
    }
}

/* ---- integrated mode: leaves drive the voice pool ---- */

static const SuAudioVoicePolicy *leaf_policy(const SuAudioEvents *events, const SuAudioGraphLeaf *data) {
    uint32_t index;
    for (index = 0; index < events->policy_count; index++) {
        const SuAudioVoicePolicyRule *rule = &events->policies[index];
        if ((rule->node_id == 0u || rule->node_id == data->node_id)
            && (rule->bus_id == 0u || rule->bus_id == data->bus_id)) {
            return &rule->policy;
        }
    }
    return &events->default_policy;
}

static SuVec3f rotate_inverse(SuQuatf q, SuVec3f v) {
    SuQuatf c;
    c.x = -q.x;
    c.y = -q.y;
    c.z = -q.z;
    c.w = q.w;
    return su_quatf_rotate(c, v);
}

/* Spatial and authored controls of one leaf at the current quantum (the legacy mixer's spatial law). */
static uint32_t leaf_controls(SuAudioEvents *events, const EventInstance *instance, EventLeaf *leaf,
                              const SuAudioCameraListener *listener, SuAudioVoiceControls *out, float *audibility) {
    float gain = leaf->data.gain;
    float pitch = (leaf->data.pitch > 0.0f ? leaf->data.pitch : 1.0f)
        * ((float)leaf->clip->sample_rate / (float)events->desc.sample_rate);
    memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    leaf->lowpass_hz = 0.0f;
    if (instance->emitter != 0u && events->scene != NULL) {
        SuAudioVoiceDesc desc = leaf->voice_desc;
        SuVec3f rel;
        SuVec3f local;
        float distance;
        float attenuation;
        float left;
        float right;
        uint32_t status;
        desc.gain = leaf->data.gain;
        desc.min_distance = 0.0f;
        desc.max_distance = 0.0f;
        desc.rolloff = 0.0f;
        status = su_audio_scene_resolve(events->scene, instance->emitter, &desc, &leaf->lowpass_hz);
        if (status != SU_OK) {
            return status;
        }
        rel = su_wpos_delta(desc.pos, listener->pos);
        local = rotate_inverse(listener->rot, rel);
        distance = sqrtf(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
        attenuation = su_audio_attenuation(distance, desc.min_distance, desc.max_distance, desc.rolloff);
        su_audio_pan(local, &left, &right);
        /* cos/sin of the pan angle round to tiny negatives at the hard sides (cosf(pi/2) < 0); the pool rejects
         * negative gains, which used to leave such a voice uncontrolled at unit gain */
        left = left > 0.0f ? left : 0.0f;
        right = right > 0.0f ? right : 0.0f;
        if (events->doppler > 0.0f) {
            SuVec3f origin = {0.0f, 0.0f, 0.0f};
            pitch *= su_audio_doppler(origin, listener->vel, rel, desc.vel, events->speed_of_sound, events->doppler);
        }
        gain = desc.gain * attenuation;
        out->gain_left = left * gain;
        out->gain_right = right * gain;
        out->mono_mix = 1u;
        leaf->ctl_dir = local;
        leaf->ctl_distance = distance;
    } else {
        out->gain_left = gain;
        out->gain_right = gain;
        leaf->ctl_dir.x = leaf->ctl_dir.y = leaf->ctl_dir.z = 0.0f;
        leaf->ctl_distance = 0.0f;
    }
    if (events->params != NULL) {
        SuAudioParamTarget target;
        float gain_db = 0.0f;
        float ratio = 1.0f;
        float cutoff = 0.0f;
        target.emitter = instance->emitter;
        target.instance = instance_handle((uint32_t)(instance - events->instances), instance->generation);
        (void)su_audio_param_voice_controls(events->params, target, leaf->data.bus_id,
            events->stats.render_clock + SU_AUDIO_EVENT_QUANTUM, &gain_db, &ratio, &cutoff);
        if (gain_db != 0.0f) {
            float scale = powf(10.0f, gain_db / 20.0f);
            gain *= scale;
            out->gain_left *= scale;
            out->gain_right *= scale;
        }
        pitch *= ratio;
        if (cutoff < (float)events->desc.sample_rate * 0.5f
            && (leaf->lowpass_hz <= 0.0f || cutoff < leaf->lowpass_hz)) {
            leaf->lowpass_hz = cutoff > 0.0f ? cutoff : 1.0f;
        }
    }
    if (pitch > 64.0f) {
        pitch = 64.0f;
    }
    out->pitch = pitch;
    out->lowpass_hz = leaf->lowpass_hz;
    leaf->ctl_gain = gain;
    leaf->ctl_pitch = pitch;
    *audibility = gain;
    return SU_OK;
}

static void pooled_start(SuAudioEvents *events, uint64_t now, uint64_t end) {
    uint32_t due = 0u;
    uint32_t slot;
    uint32_t visited;
    /* instances with a leaf starting in this quantum, in admission order */
    for (slot = 0; slot < events->desc.max_instances; slot++) {
        const EventInstance *candidate = &events->instances[slot];
        uint32_t index;
        if (candidate->state != 1u) {
            continue;
        }
        for (index = 0; index < candidate->count; index++) {
            if (candidate->leaves[index].state == 0u && candidate->leaves[index].at < end) {
                uint32_t at = due++;
                while (at > 0u && events->instances[events->due[at - 1u]].order > candidate->order) {
                    events->due[at] = events->due[at - 1u];
                    at--;
                }
                events->due[at] = slot;
                break;
            }
        }
    }
    for (visited = 0; visited < due; visited++) {
        EventInstance *instance = &events->instances[events->due[visited]];
        uint32_t index;
        for (index = 0; index < instance->count; index++) {
            EventLeaf *leaf = &instance->leaves[index];
            SuAudioLogicalDesc desc;
            uint32_t status;
            if (leaf->state != 0u || leaf->at >= end) {
                continue;
            }
            memset(&desc, 0, sizeof desc);
            desc.struct_size = sizeof desc;
            desc.policy = *leaf_policy(events, &leaf->data);
            if ((leaf->data.flags & SU_AUDIO_LOOP) != 0u) {
                desc.policy.flags |= SU_AUDIO_VOICE_LOOP;
            }
            desc.clip = leaf->clip;
            desc.gain = 1.0f;
            desc.event_id = instance->event_id;
            desc.node_id = leaf->data.node_id;
            desc.bus_id = leaf->data.bus_id;
            desc.emitter = instance->emitter;
            desc.instance = instance_handle((uint32_t)(instance - events->instances), instance->generation);
            desc.trigger_seq = ++events->trigger_seq;
            status = su_audio_voice_add_at(events->pool, &desc, leaf->at < now ? now : leaf->at, &leaf->logical);
            if (status == SU_OK) {
                status = su_audio_voice_set_tag(events->pool, leaf->logical,
                    ((uint64_t)leaf->data.asset.generation << 32) | leaf->data.asset.id);
            }
            if (status == SU_OK && events->buses != NULL) {
                uint32_t route = events->root_route;
                uint32_t r;
                for (r = 0; r < events->route_count; r++) {
                    if (events->route_ids[r] == leaf->data.bus_id) {
                        route = r;
                        break;
                    }
                }
                status = su_audio_voice_set_route(events->pool, leaf->logical, route);
            }
            leaf->state = status == SU_OK ? 1u : 2u;
            if (status != SU_OK) {
                events->stats.render_failures++;
            } else if (events->leaf_by_voice != NULL) {
                events->leaf_by_voice[(uint32_t)leaf->logical - 1u] = leaf;
            }
        }
    }
}

static void pooled_stops(SuAudioEvents *events, uint64_t end) {
    uint32_t command = 0u;
    while (command < events->stop_count) {
        EventStop stop = events->stops[command];
        uint32_t slot;
        if (stop.at >= end) {
            command++;
            continue;
        }
        for (slot = 0; slot < events->desc.max_instances; slot++) {
            EventInstance *instance = &events->instances[slot];
            uint32_t index;
            if (instance->state != 1u || !((stop.type == 1u && stop.target == instance_handle(slot, instance->generation))
                || (stop.type == 2u && stop.target == instance->emitter)
                || (stop.type == 3u && stop.target == instance->scope))) {
                continue;
            }
            for (index = 0; index < instance->count; index++) {
                EventLeaf *leaf = &instance->leaves[index];
                if (leaf->state == 0u) {
                    leaf->state = 2u;
                } else if (leaf->state == 1u
                    && su_audio_voice_stop_at(events->pool, leaf->logical, stop.at, stop.release_frames) != SU_OK) {
                    leaf->state = 3u;
                }
            }
        }
        events->stop_count--;
        memmove(&events->stops[command], &events->stops[command + 1u],
            (size_t)(events->stop_count - command) * sizeof *events->stops);
    }
}

static void pooled_controls(SuAudioEvents *events) {
    SuAudioCameraListener listener;
    uint32_t slot;
    memset(&listener, 0, sizeof listener);
    listener.rot.w = 1.0f;
    if (events->scene != NULL && su_audio_scene_listener_get(events->scene, &listener) != SU_OK) {
        events->stats.render_failures++;
    }
    for (slot = 0; slot < events->desc.max_instances; slot++) {
        EventInstance *instance = &events->instances[slot];
        uint32_t index;
        if (instance->state != 1u) {
            continue;
        }
        for (index = 0; index < instance->count; index++) {
            EventLeaf *leaf = &instance->leaves[index];
            SuAudioVoiceControls controls;
            float audibility = 0.0f;
            if (leaf->state != 1u) {
                continue;
            }
            if (leaf_controls(events, instance, leaf, &listener, &controls, &audibility) != SU_OK) {
                /* the emitter is gone: end the voice like the legacy runtime */
                (void)su_audio_voice_stop_at(events->pool, leaf->logical, events->stats.render_clock, 0u);
                continue;
            }
            if (su_audio_voice_set_controls(events->pool, leaf->logical, &controls) != SU_OK
                || su_audio_voice_set_audibility(events->pool, leaf->logical, audibility, 0.0f) != SU_OK) {
                events->stats.render_failures++;
            }
        }
    }
}

static void pooled_collect(SuAudioEvents *events) {
    uint32_t slot;
    for (slot = 0; slot < events->desc.max_instances; slot++) {
        EventInstance *instance = &events->instances[slot];
        uint32_t index;
        uint32_t remaining = 0u;
        if (instance->state != 1u) {
            continue;
        }
        for (index = 0; index < instance->count; index++) {
            EventLeaf *leaf = &instance->leaves[index];
            SuAudioVoiceInfo info;
            if (leaf->state == 1u && su_audio_voice_info(events->pool, leaf->logical, &info) != SU_OK) {
                leaf->state = 3u; /* voice ended; a steal tail may still read the clip */
            }
            if (leaf->state == 3u && !su_audio_voice_pool_clip_busy(events->pool, leaf->clip)) {
                leaf->state = 2u;
            }
            if (leaf->state != 2u) {
                remaining++;
            }
        }
        if (remaining == 0u) {
            instance->state = 2u;
        }
    }
}

static float block_rms(const float *pcm, uint32_t samples) {
    double sum = 0.0;
    uint32_t index;
    for (index = 0; index < samples; index++) {
        sum += (double)pcm[index] * pcm[index];
    }
    return (float)sqrt(sum / (double)samples);
}

/* Objects for the clustering stage: rendered voices (logical identity, listener direction, mono pre-pan
 * signal) and pre-master bus outputs as beds. The stereo output does not depend on it. */
static uint32_t feed_cluster(SuAudioEvents *events) {
    uint32_t count = 0u;
    uint32_t index;
    if ((events->cluster_inputs & SU_AUDIO_EVENTS_CLUSTER_VOICES) != 0u) {
        uint32_t captured = su_audio_voice_pool_captured(events->pool);
        for (index = 0; index < captured && count < events->object_capacity; index++) {
            const SuAudioVoiceCapture *capture = &events->captures[index];
            SuAudioClusterObject *object = &events->objects[count++];
            uint32_t slot = (uint32_t)capture->voice - 1u;
            const EventLeaf *leaf = slot < events->leaf_map_size ? events->leaf_by_voice[slot] : NULL;
            memset(object, 0, sizeof *object);
            object->id = capture->voice | (capture->tail ? UINT64_C(1) << 63 : 0u);
            if (leaf != NULL && leaf->logical == capture->voice) {
                object->direction = leaf->ctl_dir;
                object->distance_m = leaf->ctl_distance;
                object->metadata = leaf->data.bus_id;
                object->flags = leaf->ctl_dir.x == 0.0f && leaf->ctl_dir.y == 0.0f && leaf->ctl_dir.z == 0.0f
                    ? SU_AUDIO_CLUSTER_BED : 0u;
            } else {
                object->flags = SU_AUDIO_CLUSTER_BED;
            }
            object->rms = block_rms(capture->mono, SU_AUDIO_EVENT_QUANTUM);
            object->channels = 1u;
            object->pcm = capture->mono;
        }
    }
    if ((events->cluster_inputs & SU_AUDIO_EVENTS_CLUSTER_BUSES) != 0u) {
        for (index = 0; index < events->bed_count && count < events->object_capacity; index++) {
            SuAudioClusterObject *object = &events->objects[count++];
            const float *pcm = events->bed_mem + (size_t)index * SU_AUDIO_EVENT_QUANTUM * 2u;
            memset(object, 0, sizeof *object);
            object->id = (UINT64_C(1) << 62) | events->bed_ids[index];
            object->flags = SU_AUDIO_CLUSTER_BED;
            object->metadata = events->bed_ids[index];
            object->rms = block_rms(pcm, SU_AUDIO_EVENT_QUANTUM * 2u);
            object->channels = 2u;
            object->pcm = pcm;
        }
    }
    if (su_audio_cluster_update(events->cluster, events->objects, count) != SU_OK) {
        return SU_E_RESOURCE_LIMIT;
    }
    return su_audio_cluster_render(events->cluster, events->objects, count, SU_AUDIO_EVENT_QUANTUM,
        events->cluster_mix);
}

static uint32_t live_branches(SuAudioEvents *events) {
    uint32_t slot;
    if (events->params == NULL) return SU_OK;
    for (slot = 0u; slot < events->desc.max_instances; slot++) {
        EventInstance *instance = &events->instances[slot];
        SuAudioGraphExpandState initial;
        SuAudioParamTarget target;
        size_t count = 0u;
        uint32_t status, index;
        uint64_t now = events->stats.render_clock;
        if (instance->state != 1u) continue;
        memset(&initial, 0, sizeof initial);
        initial.switches = events->switch_values + ((size_t)slot + 1u) * events->switch_count;
        initial.switch_count = events->switch_count;
        target.emitter = instance->emitter;
        target.instance = instance_handle(slot, instance->generation);
        if (now < instance->start) now = instance->start;
        status = su_audio_graph_expand_live(events->graph, instance->event_id, events->desc.project_seed,
            instance->emitter, instance->sequence, &initial, events->params, target, instance->start, now,
            events->scratch, events->desc.max_leaves_per_instance, &count, NULL);
        if (status != SU_OK) return status;
        if (count != instance->count) return SU_E_SCHEMA_MISMATCH;
        for (index = 0u; index < instance->count; index++)
            instance->leaves[index].data.gain = events->scratch[index].gain;
    }
    return SU_OK;
}

static uint32_t render_quantum_pooled(SuAudioEvents *events) {
    uint64_t now = events->stats.render_clock;
    uint64_t end;
    uint32_t route;
    uint32_t index;
    uint32_t status;
    if (now > UINT64_MAX - SU_AUDIO_EVENT_QUANTUM) {
        return SU_E_RESOURCE_LIMIT;
    }
    end = now + SU_AUDIO_EVENT_QUANTUM;
    if (events->params != NULL && su_audio_params_advance(events->params, now) != SU_OK) {
        events->stats.render_failures++;
    }
    status = live_branches(events);
    if (status != SU_OK) return status;
    pooled_start(events, now, end);
    pooled_stops(events, end);
    pooled_controls(events);
    memset(events->route_mem, 0, (size_t)events->route_count * SU_AUDIO_EVENT_QUANTUM * 2u * sizeof(float));
    status = su_audio_voice_pool_quantum(events->pool, events->routes, events->route_count);
    if (status != SU_OK) {
        return status;
    }
    if (events->music != NULL) {
        status = su_audio_music_render(events->music, events->music_buffer, SU_AUDIO_EVENT_QUANTUM);
        if (status != SU_OK) {
            return status;
        }
    }
    if (events->buses != NULL) {
        for (route = 0u; route < events->route_count && status == SU_OK; route++) {
            status = su_audio_bus_mix(events->buses, events->route_ids[route], events->routes[route],
                SU_AUDIO_EVENT_QUANTUM);
        }
        if (events->music != NULL && status == SU_OK) {
            status = su_audio_bus_mix(events->buses, events->music_bus_id, events->music_buffer,
                SU_AUDIO_EVENT_QUANTUM);
        }
        for (index = 0; index < events->input_count && status == SU_OK; index++) {
            const SuAudioBusInput *input = &events->inputs[index];
            memset(events->input_buffer, 0, sizeof events->input_buffer);
            input->render(input->user, events->input_buffer, SU_AUDIO_EVENT_QUANTUM, now);
            status = su_audio_bus_mix(events->buses,
                input->bus_id != 0u ? input->bus_id : events->route_ids[events->root_route], events->input_buffer,
                SU_AUDIO_EVENT_QUANTUM);
        }
        if (events->params != NULL && events->bus_gain_capacity != 0u && status == SU_OK) {
            uint32_t count = 0u;
            status = su_audio_param_bus_gains(events->params, end, events->bus_gains, events->bus_gain_capacity,
                &count);
            for (index = 0; index < count && status == SU_OK; index++) {
                float db = events->bus_gains[index].gain_db;
                status = su_audio_bus_set_gain(events->buses, events->bus_gains[index].bus_id,
                    db > 120.0f ? 120.0f : db, SU_AUDIO_EVENT_QUANTUM);
            }
        }
        if (status == SU_OK) {
            status = su_audio_bus_process(events->buses, events->quantum, SU_AUDIO_EVENT_QUANTUM);
        }
        if (status != SU_OK) {
            return status;
        }
    } else {
        uint32_t input;
        memcpy(events->quantum, events->routes[0], sizeof events->quantum);
        if (events->music != NULL) {
            for (index = 0; index < SU_AUDIO_EVENT_QUANTUM * 2u; index++) {
                events->quantum[index] += events->music_buffer[index];
            }
        }
        for (input = 0; input < events->input_count; input++) {
            memset(events->input_buffer, 0, sizeof events->input_buffer);
            events->inputs[input].render(events->inputs[input].user, events->input_buffer, SU_AUDIO_EVENT_QUANTUM,
                now);
            for (index = 0; index < SU_AUDIO_EVENT_QUANTUM * 2u; index++) {
                events->quantum[index] += events->input_buffer[index];
            }
        }
    }
    if (events->cluster != NULL && feed_cluster(events) != SU_OK) {
        events->stats.render_failures++;
    }
    if (events->limiter_knee > 0.0f) {
        float knee = events->limiter_knee;
        for (index = 0; index < SU_AUDIO_EVENT_QUANTUM * 2u; index++) {
            float sample = events->quantum[index];
            float magnitude = fabsf(sample);
            if (magnitude > knee) {
                float bent = knee + (1.0f - knee) * tanhf((magnitude - knee) / (1.0f - knee));
                events->quantum[index] = sample < 0.0f ? -bent : bent;
            }
        }
    } else {
        for (index = 0; index < SU_AUDIO_EVENT_QUANTUM * 2u; index++) {
            float sample = events->quantum[index];
            events->quantum[index] = sample > 1.0f ? 1.0f : sample < -1.0f ? -1.0f : sample;
        }
    }
    pooled_collect(events);
    events->stats.render_clock = end;
    events->cached = SU_AUDIO_EVENT_QUANTUM;
    events->consumed = 0u;
    return SU_OK;
}

static void cluster_detach(SuAudioEvents *events) {
    free(events->captures);
    free(events->capture_mem);
    free(events->objects);
    free(events->bed_ids);
    free(events->bed_mem);
    free(events->leaf_by_voice);
    events->captures = NULL;
    events->capture_mem = NULL;
    events->objects = NULL;
    events->bed_ids = NULL;
    events->bed_mem = NULL;
    events->leaf_by_voice = NULL;
    events->cluster = NULL;
}

static uint32_t cluster_attach(SuAudioEvents *events, SuAudioVoicePool *pool, SuAudioBusTree *buses,
                               SuAudioCluster *cluster, uint32_t inputs) {
    SuAudioVoicePoolDesc pool_desc;
    uint32_t captures = 0u;
    uint32_t beds = 0u;
    uint32_t index;
    if (inputs == 0u || (inputs & ~(SU_AUDIO_EVENTS_CLUSTER_VOICES | SU_AUDIO_EVENTS_CLUSTER_BUSES)) != 0u
        || ((inputs & SU_AUDIO_EVENTS_CLUSTER_BUSES) != 0u && buses == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    su_audio_voice_pool_desc(pool, &pool_desc);
    if ((inputs & SU_AUDIO_EVENTS_CLUSTER_VOICES) != 0u) {
        captures = pool_desc.max_physical + pool_desc.transition_reserve;
    }
    if ((inputs & SU_AUDIO_EVENTS_CLUSTER_BUSES) != 0u) {
        for (index = 0; index < su_audio_bus_count(buses); index++) {
            SuAudioBusId id = 0u;
            SuAudioBusId parent = 0u;
            if (su_audio_bus_order(buses, index, &id) == SU_OK && su_audio_bus_parent(buses, id, &parent) == SU_OK
                && parent != SU_AUDIO_BUS_ROOT) {
                SuAudioBusId grand = 0u;
                if (su_audio_bus_parent(buses, parent, &grand) == SU_OK && grand == SU_AUDIO_BUS_ROOT) {
                    beds++;
                }
            }
        }
    }
    if (captures + beds == 0u || captures + beds > su_audio_cluster_capacity(cluster)) {
        return SU_E_INVALID_ARGUMENT;
    }
    events->captures = calloc(captures + 1u, sizeof *events->captures);
    events->capture_mem = calloc(((size_t)captures + 1u) * SU_AUDIO_EVENT_QUANTUM, sizeof *events->capture_mem);
    events->objects = calloc(captures + beds, sizeof *events->objects);
    events->bed_ids = calloc(beds + 1u, sizeof *events->bed_ids);
    events->bed_mem = calloc(((size_t)beds + 1u) * SU_AUDIO_EVENT_QUANTUM * 2u, sizeof *events->bed_mem);
    events->leaf_by_voice = calloc(pool_desc.max_logical, sizeof *events->leaf_by_voice);
    if (events->captures == NULL || events->capture_mem == NULL || events->objects == NULL || events->bed_ids == NULL
        || events->bed_mem == NULL || events->leaf_by_voice == NULL) {
        cluster_detach(events);
        return SU_E_RESOURCE_LIMIT;
    }
    for (index = 0; index < captures; index++) {
        events->captures[index].mono = events->capture_mem + (size_t)index * SU_AUDIO_EVENT_QUANTUM;
    }
    events->bed_count = 0u;
    for (index = 0; beds != 0u && index < su_audio_bus_count(buses); index++) {
        SuAudioBusId id = 0u;
        SuAudioBusId parent = 0u;
        SuAudioBusId grand = 0u;
        if (su_audio_bus_order(buses, index, &id) == SU_OK && su_audio_bus_parent(buses, id, &parent) == SU_OK
            && parent != SU_AUDIO_BUS_ROOT && su_audio_bus_parent(buses, parent, &grand) == SU_OK
            && grand == SU_AUDIO_BUS_ROOT) {
            events->bed_ids[events->bed_count] = id;
            (void)su_audio_bus_capture(buses, id, events->bed_mem + (size_t)events->bed_count * SU_AUDIO_EVENT_QUANTUM * 2u);
            events->bed_count++;
        }
    }
    if (captures != 0u && su_audio_voice_pool_capture(pool, events->captures, captures) != SU_OK) {
        cluster_detach(events);
        return SU_E_INVALID_ARGUMENT;
    }
    events->capture_count = captures;
    events->object_capacity = captures + beds;
    events->leaf_map_size = pool_desc.max_logical;
    events->cluster = cluster;
    events->cluster_inputs = inputs;
    events->stats.runtime_bytes += (uint64_t)(captures + 1u) * (sizeof *events->captures
        + SU_AUDIO_EVENT_QUANTUM * sizeof(float)) + (uint64_t)(captures + beds) * sizeof *events->objects
        + (uint64_t)(beds + 1u) * (sizeof(SuAudioBusId) + SU_AUDIO_EVENT_QUANTUM * 2u * sizeof(float))
        + (uint64_t)pool_desc.max_logical * sizeof(EventLeaf *);
    return SU_OK;
}

uint32_t su_audio_events_attach(SuAudioEvents *events, const SuAudioEventsHook *hook) {
    SuAudioVoiceStats pool_stats;
    SuAudioVoicePoolDesc pool_desc;
    SuAudioVoicePolicyRule *policies = NULL;
    SuAudioParams *params = NULL;
    SuAudioBusTree *buses = NULL;
    SuAudioMusic *music = NULL;
    SuAudioGraphParameter *param_values;
    SuAudioGraphSwitch *switch_values;
    SuAudioBusGain *bus_gains;
    SuAudioBusId *route_ids;
    uint32_t music_bus_id = 0u;
    uint32_t route_count = 1u;
    uint32_t param_count = 0u;
    uint32_t switch_count = 0u;
    uint32_t bus_bindings = 0u;
    uint32_t index;
    uint32_t status;
    float knee = 0.0f;
    float doppler = 0.0f;
    float speed_of_sound = 0.0f;
    const SuAudioBusInput *host_inputs = NULL;
    uint32_t input_count = 0u;
    float *route_mem;
    float **routes;
    if (events == NULL || hook == NULL || hook->struct_size < offsetof(SuAudioEventsHook, params)
        || hook->struct_size > sizeof *hook || hook->version != SU_AUDIO_EVENTS_HOOK_VERSION
        || hook->voices == NULL || hook->policy_count > 4096u || (hook->policy_count != 0u && hook->policies == NULL)
        || events->pool != NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    su_audio_events_update(events);
    su_audio_voice_pool_stats(hook->voices, &pool_stats);
    su_audio_voice_pool_desc(hook->voices, &pool_desc);
    if (pool_desc.max_logical < events->desc.max_logical_voices
        || (pool_desc.sample_rate != 0u && pool_desc.sample_rate != events->desc.sample_rate)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (events->stats.instances != 0u || events->stop_count != 0u || events->consumed != events->cached
        || pool_stats.logical != 0u) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (SU_AUDIO_EVENTS_HOOK_HAS(hook, params)) {
        params = hook->params;
    }
    if (SU_AUDIO_EVENTS_HOOK_HAS(hook, buses)) {
        buses = hook->buses;
    }
    if (SU_AUDIO_EVENTS_HOOK_HAS(hook, music)) {
        music = hook->music;
    }
    if (SU_AUDIO_EVENTS_HOOK_HAS(hook, music_bus_id)) {
        music_bus_id = hook->music_bus_id;
    }
    if (SU_AUDIO_EVENTS_HOOK_HAS(hook, limiter_knee)) {
        knee = hook->limiter_knee;
    }
    if (SU_AUDIO_EVENTS_HOOK_HAS(hook, cluster) && hook->cluster != NULL) {
        uint32_t inputs = SU_AUDIO_EVENTS_HOOK_HAS(hook, cluster_inputs) ? hook->cluster_inputs : 0u;
        status = cluster_attach(events, hook->voices, hook->buses, hook->cluster, inputs);
        if (status != SU_OK) {
            return status;
        }
    }
    if (!(knee == 0.0f || (knee >= 0.1f && knee < 1.0f))) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (SU_AUDIO_EVENTS_HOOK_HAS(hook, doppler)) {
        doppler = hook->doppler;
    }
    if (SU_AUDIO_EVENTS_HOOK_HAS(hook, speed_of_sound)) {
        speed_of_sound = hook->speed_of_sound;
    }
    if (SU_AUDIO_EVENTS_HOOK_HAS(hook, input_count)) {
        input_count = hook->input_count;
        host_inputs = hook->inputs;
    }
    if (!(doppler >= 0.0f && doppler <= 1.0f) || !(speed_of_sound >= 0.0f && speed_of_sound <= 1e6f)
        || input_count > SU_AUDIO_EVENTS_MAX_INPUTS || (input_count != 0u && host_inputs == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (index = 0; index < input_count; index++) {
        float probe;
        if (host_inputs[index].render == NULL || (host_inputs[index].bus_id != 0u
            && (buses == NULL || su_audio_bus_gain_at(buses, host_inputs[index].bus_id, &probe) != SU_OK))) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    if (buses != NULL) {
        route_count = su_audio_bus_count(buses);
        if (su_audio_bus_max_block(buses) < SU_AUDIO_EVENT_QUANTUM) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    if (music != NULL) {
        SuAudioMusicState music_state;
        su_audio_music_state(music, &music_state);
        if (music_state.clock != events->stats.render_clock) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    if (params != NULL) {
        su_audio_params_counts(params, &param_count, &switch_count, &bus_bindings);
    }
    route_mem = calloc((size_t)route_count * SU_AUDIO_EVENT_QUANTUM * 2u, sizeof *route_mem);
    routes = calloc(route_count, sizeof *routes);
    route_ids = calloc(route_count, sizeof *route_ids);
    param_values = calloc(param_count + 1u, sizeof *param_values);
    switch_values = calloc(((size_t)events->desc.max_instances + 1u) * switch_count + 1u, sizeof *switch_values);
    bus_gains = calloc(bus_bindings + 1u, sizeof *bus_gains);
    events->due = calloc(events->desc.max_instances, sizeof *events->due);
    if (hook->policy_count != 0u) {
        policies = calloc(hook->policy_count, sizeof *policies);
    }
    status = route_mem == NULL || routes == NULL || route_ids == NULL || param_values == NULL
        || switch_values == NULL || bus_gains == NULL || events->due == NULL
        || (hook->policy_count != 0u && policies == NULL)
        ? SU_E_RESOURCE_LIMIT : SU_OK;
    for (index = 0; status == SU_OK && buses != NULL && index < route_count; index++) {
        status = su_audio_bus_order(buses, index, &route_ids[index]);
    }
    if (status == SU_OK && music != NULL && buses != NULL) {
        if (music_bus_id == 0u) {
            music_bus_id = route_ids[route_count - 1u];
        }
        for (index = 0; index < route_count && route_ids[index] != music_bus_id; index++) {
        }
        status = index == route_count ? SU_E_INVALID_ARGUMENT : SU_OK;
    }
    if (status == SU_OK && su_audio_voice_pool_set_clock(hook->voices, events->stats.render_clock) != SU_OK) {
        status = SU_E_INVALID_ARGUMENT;
    }
    if (status != SU_OK) {
        if (events->cluster != NULL) {
            uint32_t bed;
            for (bed = 0; bed < events->bed_count; bed++) {
                (void)su_audio_bus_capture(buses, events->bed_ids[bed], NULL);
            }
            (void)su_audio_voice_pool_capture(hook->voices, NULL, 0u);
            cluster_detach(events);
        }
        free(route_mem);
        free(routes);
        free(route_ids);
        free(param_values);
        free(switch_values);
        free(bus_gains);
        free(policies);
        free(events->due);
        events->due = NULL;
        return status;
    }
    if (policies != NULL) {
        memcpy(policies, hook->policies, (size_t)hook->policy_count * sizeof *policies);
    }
    for (index = 0; index < route_count; index++) {
        routes[index] = route_mem + (size_t)index * SU_AUDIO_EVENT_QUANTUM * 2u;
    }
    events->params = params;
    events->param_values = param_values;
    events->switch_values = switch_values;
    events->param_count = param_count;
    events->switch_count = switch_count;
    events->bus_gains = bus_gains;
    events->bus_gain_capacity = bus_bindings;
    events->buses = buses;
    events->route_ids = route_ids;
    events->root_route = route_count - 1u; /* execution order ends with the root */
    events->music = music;
    events->music_bus_id = music_bus_id;
    events->limiter_knee = knee;
    events->doppler = doppler;
    events->speed_of_sound = speed_of_sound > 0.0f ? speed_of_sound : 343.0f;
    if (input_count != 0u) {
        memcpy(events->inputs, host_inputs, (size_t)input_count * sizeof *host_inputs);
    }
    events->input_count = input_count;
    events->policies = policies;
    events->policy_count = hook->policy_count;
    events->default_policy = hook->default_policy;
    events->route_mem = route_mem;
    events->routes = routes;
    events->route_count = route_count;
    events->pool = hook->voices;
    events->stats.runtime_bytes += (uint64_t)route_count * (SU_AUDIO_EVENT_QUANTUM * 2u * sizeof *route_mem
        + sizeof *routes + sizeof *route_ids) + (uint64_t)(param_count + 1u) * sizeof *param_values
        + (((uint64_t)events->desc.max_instances + 1u) * switch_count + 1u) * sizeof *switch_values
        + (uint64_t)(bus_bindings + 1u) * sizeof *bus_gains
        + (uint64_t)hook->policy_count * sizeof *policies
        + (uint64_t)events->desc.max_instances * sizeof *events->due;
    return SU_OK;
}

static uint32_t render_quantum(SuAudioEvents *events) {
    uint64_t now = events->stats.render_clock;
    uint64_t end;
    uint32_t offset = 0u;
    if (now > UINT64_MAX - SU_AUDIO_EVENT_QUANTUM) {
        return SU_E_RESOURCE_LIMIT;
    }
    end = now + SU_AUDIO_EVENT_QUANTUM;
    refresh_scene(events);
    while (now < end) {
        uint32_t command;
        uint32_t frames;
        apply_stops(events, now);
        collect_finished(events);
        uint64_t next = start_due(events, now, end);
        for (command = 0; command < events->stop_count; command++) {
            if (events->stops[command].at < next) {
                next = events->stops[command].at;
            }
        }
        frames = (uint32_t)(next - now);
        su_audio_mixer_render(events->mixer, events->quantum + (size_t)offset * 2u, frames);
        collect_finished(events);
        now = next;
        offset += frames;
    }
    events->stats.render_clock = end;
    events->cached = SU_AUDIO_EVENT_QUANTUM;
    events->consumed = 0u;
    return SU_OK;
}

uint32_t su_audio_events_render(SuAudioEvents *events, float *out, uint32_t frames) {
    uint32_t done = 0u;
    if (events == NULL || (out == NULL && frames != 0u)
        || events->stats.delivered_frames > UINT64_MAX - frames) {
        return SU_E_INVALID_ARGUMENT;
    }
    while (done < frames) {
        uint32_t take;
        if (events->consumed == events->cached) {
            uint32_t status = events->pool != NULL ? render_quantum_pooled(events) : render_quantum(events);
            if (status != SU_OK) {
                return status;
            }
        }
        take = events->cached - events->consumed;
        if (take > frames - done) {
            take = frames - done;
        }
        memcpy(out + (size_t)done * 2u, events->quantum + (size_t)events->consumed * 2u,
            (size_t)take * 2u * sizeof *out);
        done += take;
        events->consumed += take;
        events->stats.delivered_frames += take;
    }
    return SU_OK;
}

int su_audio_instance_active(const SuAudioEvents *events, SuAudioInstance instance) {
    return events != NULL && find_instance(events, instance) != NULL;
}

uint64_t su_audio_events_clock(const SuAudioEvents *events) {
    return events != NULL ? events->stats.render_clock : 0u;
}

void su_audio_events_stats(const SuAudioEvents *events, SuAudioEventStats *out) {
    if (events != NULL && out != NULL) {
        *out = events->stats;
        out->queued_commands = events->stop_count;
    }
}

uint32_t su_audio_commit_cursor_snapshot(const SuAudioEvents *events, SuAudioCommitCursor *out) {
    if (events == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = events->cursor;
    return SU_OK;
}

uint32_t su_audio_commit_cursor_restore(SuAudioEvents *events, const SuAudioCommitCursor *cursor) {
    if (events == NULL || cursor == NULL || cursor->session_generation != events->cursor.session_generation) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (cursor->next_seq < events->cursor.next_seq) {
        return SU_E_STALE_REVISION;
    }
    events->cursor = *cursor;
    return SU_OK;
}

uint32_t su_audio_events_session(SuAudioEvents *events, uint64_t session_generation) {
    if (events == NULL || session_generation <= events->cursor.session_generation) {
        return SU_E_INVALID_ARGUMENT;
    }
    su_audio_events_update(events);
    if (events->stats.instances != 0u || events->stop_count != 0u || events->consumed != events->cached) {
        return SU_E_RESOURCE_LIMIT;
    }
    events->cursor.session_generation = session_generation;
    events->cursor.next_seq = 0u;
    return SU_OK;
}

uint32_t su_audio_events_view(const SuAudioEvents *events, uint32_t slot, SuAudioEventsDesc *desc,
    SuAudioEventInstanceView *instance, SuAudioEventLeafView *leaves, uint32_t capacity) {
    const EventInstance *source;
    uint32_t index;
    if (events == NULL || desc == NULL || instance == NULL || (leaves == NULL && capacity != 0u)
        || slot >= events->desc.max_instances) {
        return SU_E_INVALID_ARGUMENT;
    }
    *desc = events->desc;
    source = &events->instances[slot];
    memset(instance, 0, sizeof *instance);
    instance->state = source->state;
    if (source->state == 0u) {
        return SU_OK;
    }
    instance->handle = instance_handle(slot, source->generation);
    instance->emitter = source->emitter;
    instance->order = source->order;
    instance->scope = source->scope;
    instance->count = source->count;
    for (index = 0; index < source->count && index < capacity; index++) {
        const EventLeaf *leaf = &source->leaves[index];
        leaves[index].data = leaf->data;
        leaves[index].start_sample = leaf->at;
        leaves[index].state = leaf->state;
        leaves[index].state = leaf->state == 3u ? 2u : leaf->state;
        leaves[index].gain = events->pool != NULL ? leaf->ctl_gain : leaf->voice_desc.gain;
        leaves[index].pitch = events->pool != NULL ? leaf->ctl_pitch : leaf->voice_desc.pitch;
        leaves[index].lowpass_hz = leaf->lowpass_hz;
    }
    return SU_OK;
}

/* ---- direct checkpoints ---- */

#define CHECKPOINT_MAGIC 0x45415553u /* "SUAE" little-endian */

static uint64_t fnv_bytes(uint64_t hash, const void *data, size_t bytes) {
    const uint8_t *p = (const uint8_t *)data;
    size_t index;
    for (index = 0; index < bytes; index++) {
        hash ^= p[index];
        hash *= UINT64_C(0x100000001b3);
    }
    return hash;
}

static uint64_t fnv_u64(uint64_t hash, uint64_t value) {
    uint8_t bytes[8];
    uint32_t index;
    for (index = 0; index < 8u; index++) {
        bytes[index] = (uint8_t)(value >> (8u * index));
    }
    return fnv_bytes(hash, bytes, sizeof bytes);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u64(uint8_t *p, uint64_t v) {
    put_u32(p, (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p) {
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

/* Configuration that must match between save and load. */
static uint64_t layout_fingerprint(const SuAudioEvents *events) {
    SuAudioVoicePoolDesc pool_desc;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    uint32_t emitters = 0u;
    uint32_t curves = 0u;
    uint32_t curve_count = 0u;
    uint32_t params = 0u;
    uint32_t switches = 0u;
    uint32_t bus_bindings = 0u;
    uint32_t index;
    uint32_t knee_bits;
    su_audio_voice_pool_desc(events->pool, &pool_desc);
    hash = fnv_u64(hash, sizeof(EventLeaf));
    hash = fnv_u64(hash, sizeof(EventInstance));
    hash = fnv_u64(hash, EVENT_CHECKPOINT_LAYOUT);
    hash = fnv_u64(hash, events->desc.sample_rate);
    hash = fnv_u64(hash, events->desc.max_instances);
    hash = fnv_u64(hash, events->desc.max_leaves_per_instance);
    hash = fnv_u64(hash, events->desc.max_logical_voices);
    hash = fnv_u64(hash, events->desc.command_capacity);
    hash = fnv_u64(hash, events->desc.project_seed);
    hash = fnv_u64(hash, pool_desc.max_logical);
    hash = fnv_u64(hash, pool_desc.max_physical);
    hash = fnv_u64(hash, pool_desc.hero_reserve);
    hash = fnv_u64(hash, pool_desc.transition_reserve);
    hash = fnv_u64(hash, events->policy_count);
    hash = fnv_u64(hash, events->route_count);
    for (index = 0; events->route_ids != NULL && index < events->route_count; index++) {
        hash = fnv_u64(hash, events->route_ids[index]);
    }
    su_audio_params_counts(events->params, &params, &switches, &bus_bindings);
    hash = fnv_u64(hash, events->params != NULL);
    hash = fnv_u64(hash, ((uint64_t)params << 32) | switches);
    hash = fnv_u64(hash, bus_bindings);
    hash = fnv_u64(hash, events->music != NULL);
    hash = fnv_u64(hash, events->cluster != NULL ? su_audio_cluster_capacity(events->cluster) : 0u);
    hash = fnv_u64(hash, events->cluster_inputs);
    hash = fnv_u64(hash, events->bed_count);
    su_audio_scene_capacity(events->scene, &emitters, &curves, &curve_count);
    hash = fnv_u64(hash, ((uint64_t)emitters << 32) | curves);
    hash = fnv_u64(hash, curve_count);
    memcpy(&knee_bits, &events->limiter_knee, sizeof knee_bits);
    hash = fnv_u64(hash, knee_bits);
    memcpy(&knee_bits, &events->doppler, sizeof knee_bits);
    hash = fnv_u64(hash, knee_bits);
    memcpy(&knee_bits, &events->speed_of_sound, sizeof knee_bits);
    hash = fnv_u64(hash, knee_bits);
    return hash;
}

static void instance_io(SuAudioStateIo *io, EventInstance *instance) {
    SU_STATE(io, instance->generation);
    SU_STATE(io, instance->state);
    SU_STATE(io, instance->count);
    SU_STATE(io, instance->scope);
    SU_STATE(io, instance->event_id);
    SU_STATE(io, instance->order);
    SU_STATE(io, instance->emitter);
    SU_STATE(io, instance->start);
    SU_STATE(io, instance->sequence);
}

static void leaf_io(SuAudioStateIo *io, EventLeaf *leaf) {
    SU_STATE(io, leaf->data);
    SU_STATE(io, leaf->voice_desc.gain);
    SU_STATE(io, leaf->voice_desc.pitch);
    SU_STATE(io, leaf->voice_desc.flags);
    SU_STATE(io, leaf->voice_desc.pos);
    SU_STATE(io, leaf->voice_desc.vel);
    SU_STATE(io, leaf->voice_desc.min_distance);
    SU_STATE(io, leaf->voice_desc.max_distance);
    SU_STATE(io, leaf->voice_desc.rolloff);
    SU_STATE(io, leaf->voice_desc.doppler);
    SU_STATE(io, leaf->at);
    SU_STATE(io, leaf->state);
    SU_STATE(io, leaf->lowpass_hz);
    SU_STATE(io, leaf->logical);
    SU_STATE(io, leaf->ctl_gain);
    SU_STATE(io, leaf->ctl_pitch);
    SU_STATE(io, leaf->ctl_dir);
    SU_STATE(io, leaf->ctl_distance);
}

/* Runtime section. On load, leaves get their clips from fresh pins. */
static uint32_t events_io(SuAudioEvents *events, SuAudioStateIo *io) {
    uint32_t slot;
    uint32_t index;
    SU_STATE(io, events->order);
    SU_STATE(io, events->trigger_seq);
    SU_STATE(io, events->cursor);
    SU_STATE(io, events->stats);
    SU_STATE(io, events->stop_count);
    if (io->failed || events->stop_count > events->desc.command_capacity) {
        return SU_E_SCHEMA_MISMATCH;
    }
    for (index = 0; index < events->stop_count; index++) {
        SU_STATE(io, events->stops[index]);
    }
    SU_STATE(io, events->cached);
    SU_STATE(io, events->consumed);
    SU_STATE(io, events->quantum);
    for (slot = 0; slot < events->desc.max_instances; slot++) {
        EventInstance *instance = &events->instances[slot];
        instance_io(io, instance);
        if (io->failed || instance->count > events->desc.max_leaves_per_instance) {
            return SU_E_SCHEMA_MISMATCH;
        }
        if (instance->state == 0u) {
            if (!io->save && io->data != NULL) {
                instance->count = 0u;
            }
            continue;
        }
        for (index = 0u; index < events->switch_count; index++) {
            SU_STATE(io, events->switch_values[((size_t)slot + 1u) * events->switch_count + index]);
        }
        for (index = 0; index < instance->count; index++) {
            EventLeaf *leaf = &instance->leaves[index];
            leaf_io(io, leaf);
            if (!io->save && io->data != NULL && !io->failed) {
                const SuAudioClip *clip = NULL;
                if (su_audio_asset_acquire(events->assets, leaf->data.asset, &clip) != SU_OK) {
                    return SU_E_SCHEMA_MISMATCH;
                }
                leaf->clip = clip;
                leaf->voice_desc.clip = clip;
                leaf->voice_desc.stream = NULL;
                if (events->leaf_by_voice != NULL && leaf->state != 0u && leaf->state != 2u && leaf->logical != 0u
                    && (uint32_t)leaf->logical - 1u < events->leaf_map_size) {
                    events->leaf_by_voice[(uint32_t)leaf->logical - 1u] = leaf;
                }
            }
        }
    }
    return io->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* Walks the runtime section of a blob into temporaries and checks every leaf asset is resident. */
static uint32_t scan_assets(SuAudioEvents *events, const uint8_t *body, size_t bytes) {
    SuAudioStateIo io;
    SuAudioEvents copy = *events;
    EventStop stop;
    EventInstance instance;
    EventLeaf leaf;
    float quantum[SU_AUDIO_EVENT_QUANTUM * 2u];
    uint32_t slot;
    uint32_t index;
    memset(&io, 0, sizeof io);
    io.data = (uint8_t *)(uintptr_t)body;
    io.capacity = bytes;
    SU_STATE(&io, copy.order);
    SU_STATE(&io, copy.trigger_seq);
    SU_STATE(&io, copy.cursor);
    SU_STATE(&io, copy.stats);
    SU_STATE(&io, copy.stop_count);
    if (io.failed || copy.stop_count > events->desc.command_capacity) {
        return SU_E_SCHEMA_MISMATCH;
    }
    for (index = 0; index < copy.stop_count; index++) {
        SU_STATE(&io, stop);
    }
    SU_STATE(&io, copy.cached);
    SU_STATE(&io, copy.consumed);
    SU_STATE(&io, quantum);
    for (slot = 0; slot < events->desc.max_instances; slot++) {
        instance_io(&io, &instance);
        if (io.failed || instance.count > events->desc.max_leaves_per_instance) {
            return SU_E_SCHEMA_MISMATCH;
        }
        if (instance.state == 0u) {
            continue;
        }
        for (index = 0u; index < events->switch_count; index++) {
            SuAudioGraphSwitch initial;
            SU_STATE(&io, initial);
        }
        for (index = 0; index < instance.count; index++) {
            const SuAudioClip *clip = NULL;
            leaf_io(&io, &leaf);
            if (io.failed || su_audio_asset_acquire(events->assets, leaf.data.asset, &clip) != SU_OK) {
                return SU_E_SCHEMA_MISMATCH;
            }
            (void)su_audio_asset_release(events->assets, leaf.data.asset);
        }
    }
    return io.failed ? SU_E_SCHEMA_MISMATCH : SU_OK;
}

static const SuAudioClip *resolve_tag(void *user, uint64_t tag) {
    SuAudioEvents *events = (SuAudioEvents *)user;
    SuAudioAssetRef ref;
    const SuAudioClip *clip = NULL;
    ref.id = (uint32_t)tag;
    ref.generation = (uint32_t)(tag >> 32);
    if (su_audio_asset_acquire(events->assets, ref, &clip) != SU_OK) {
        return NULL;
    }
    (void)su_audio_asset_release(events->assets, ref); /* the owning leaf holds the pin */
    return clip;
}

static uint32_t body_io(SuAudioEvents *events, SuAudioStateIo *io) {
    uint32_t status = events_io(events, io);
    if (status == SU_OK) {
        status = su_audio_voice_pool_state(events->pool, io, resolve_tag, events);
    }
    if (status == SU_OK && events->buses != NULL) {
        status = su_audio_bus_tree_state(events->buses, io);
    }
    if (status == SU_OK && events->params != NULL) {
        status = su_audio_params_state(events->params, io);
    }
    if (status == SU_OK && events->music != NULL) {
        status = su_audio_music_state_io(events->music, io);
    }
    if (status == SU_OK && events->cluster != NULL) {
        status = su_audio_cluster_state(events->cluster, io);
    }
    if (status == SU_OK && events->scene != NULL) {
        status = su_audio_scene_state(events->scene, io);
    }
    return status;
}

uint32_t su_audio_events_checkpoint_save(SuAudioEvents *events, void *out, size_t capacity, size_t *written) {
    SuAudioStateIo io;
    uint8_t *p = (uint8_t *)out;
    uint32_t status;
    size_t body;
    if (written != NULL) {
        *written = 0u;
    }
    if (events == NULL || written == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (events->pool == NULL) {
        return SU_E_UNSUPPORTED_METHOD;
    }
    memset(&io, 0, sizeof io);
    io.save = 1;
    status = body_io(events, &io);
    if (status != SU_OK) {
        return status;
    }
    body = io.pos;
    *written = SU_AUDIO_EVENTS_CHECKPOINT_HEADER + body;
    if (out == NULL) {
        return SU_OK;
    }
    if (capacity < *written) {
        return SU_E_RESOURCE_LIMIT;
    }
    memset(p, 0, SU_AUDIO_EVENTS_CHECKPOINT_HEADER);
    memset(&io, 0, sizeof io);
    io.save = 1;
    io.data = p + SU_AUDIO_EVENTS_CHECKPOINT_HEADER;
    io.capacity = body;
    status = body_io(events, &io);
    if (status != SU_OK) {
        return status;
    }
    put_u32(p, CHECKPOINT_MAGIC);
    put_u32(p + 4, SU_AUDIO_EVENTS_CHECKPOINT_VERSION);
    put_u32(p + 8, SU_AUDIO_EVENTS_CHECKPOINT_HEADER);
    put_u64(p + 16, layout_fingerprint(events));
    put_u64(p + 24, body);
    put_u64(p + 32, fnv_bytes(UINT64_C(0xcbf29ce484222325), p + SU_AUDIO_EVENTS_CHECKPOINT_HEADER, body));
    put_u64(p + 40, events->stats.render_clock);
    put_u64(p + 48, events->stats.delivered_frames);
    return SU_OK;
}

uint32_t su_audio_events_checkpoint_info(const void *data, size_t bytes, SuAudioEventsCheckpointInfo *out) {
    const uint8_t *p = (const uint8_t *)data;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    if (p == NULL || bytes < SU_AUDIO_EVENTS_CHECKPOINT_HEADER || get_u32(p) != CHECKPOINT_MAGIC
        || get_u32(p + 8) != SU_AUDIO_EVENTS_CHECKPOINT_HEADER) {
        return SU_E_SCHEMA_MISMATCH;
    }
    out->version = get_u32(p + 4);
    out->fingerprint = get_u64(p + 16);
    out->body_bytes = get_u64(p + 24);
    out->clock = get_u64(p + 40);
    out->delivered_frames = get_u64(p + 48);
    if (out->version != SU_AUDIO_EVENTS_CHECKPOINT_VERSION
        || out->body_bytes != (uint64_t)(bytes - SU_AUDIO_EVENTS_CHECKPOINT_HEADER)
        || get_u64(p + 32) != fnv_bytes(UINT64_C(0xcbf29ce484222325), p + SU_AUDIO_EVENTS_CHECKPOINT_HEADER,
            bytes - SU_AUDIO_EVENTS_CHECKPOINT_HEADER)) {
        return SU_E_SCHEMA_MISMATCH;
    }
    return SU_OK;
}

uint32_t su_audio_events_checkpoint_load(SuAudioEvents *events, const void *data, size_t bytes) {
    SuAudioEventsCheckpointInfo info;
    SuAudioStateIo io;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t status;
    uint32_t slot;
    if (events == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (events->pool == NULL) {
        return SU_E_UNSUPPORTED_METHOD;
    }
    status = su_audio_events_checkpoint_info(data, bytes, &info);
    if (status != SU_OK) {
        return status;
    }
    if (info.fingerprint != layout_fingerprint(events)) {
        return SU_E_SCHEMA_MISMATCH;
    }
    status = scan_assets(events, p + SU_AUDIO_EVENTS_CHECKPOINT_HEADER, (size_t)info.body_bytes);
    if (status != SU_OK) {
        return status;
    }
    /* drop the current pins; the load pins the saved leaves */
    for (slot = 0; slot < events->desc.max_instances; slot++) {
        EventInstance *instance = &events->instances[slot];
        uint32_t index;
        for (index = 0; instance->state != 0u && index < instance->count; index++) {
            (void)su_audio_asset_release(events->assets, instance->leaves[index].data.asset);
        }
        instance->count = 0u;
        instance->state = 0u;
    }
    if (events->leaf_by_voice != NULL) {
        memset(events->leaf_by_voice, 0, (size_t)events->leaf_map_size * sizeof *events->leaf_by_voice);
    }
    memset(&io, 0, sizeof io);
    io.data = (uint8_t *)(uintptr_t)(p + SU_AUDIO_EVENTS_CHECKPOINT_HEADER);
    io.capacity = (size_t)info.body_bytes;
    status = body_io(events, &io);
    if (status == SU_OK && io.pos != io.capacity) {
        status = SU_E_SCHEMA_MISMATCH;
    }
    if (status != SU_OK) {
        events->stats.render_failures++;
    }
    return status;
}
