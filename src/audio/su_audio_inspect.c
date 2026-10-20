#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_inspect.h"
#include "ir/su_sha256.h"
#include "su_audio_event_view.h"

struct SuAudioTelemetry {
    SuAudioTelemetryEntry *ring;
    uint32_t capacity;
    uint32_t head;
    uint32_t queued;
    uint64_t next_seq;
    uint64_t pending_drops;
    SuAudioTelemetryStats stats;
};

/* ---- bounded telemetry ---- */

uint32_t su_audio_trace_begin(uint32_t capacity, SuAudioTelemetry **out) {
    SuAudioTelemetry *telemetry;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (capacity == 0u || capacity > SU_AUDIO_TELEMETRY_MAX) {
        return SU_E_INVALID_ARGUMENT;
    }
    telemetry = (SuAudioTelemetry *)calloc(1u, sizeof *telemetry);
    if (telemetry == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    telemetry->ring = (SuAudioTelemetryEntry *)calloc(capacity, sizeof *telemetry->ring);
    if (telemetry->ring == NULL) {
        free(telemetry);
        return SU_E_RESOURCE_LIMIT;
    }
    telemetry->capacity = capacity;
    telemetry->stats.capacity = capacity;
    *out = telemetry;
    return SU_OK;
}

void su_audio_trace_end(SuAudioTelemetry *telemetry) {
    if (telemetry != NULL) {
        free(telemetry->ring);
        free(telemetry);
    }
}

uint32_t su_audio_trace_reason(uint32_t kind, uint32_t status, uint64_t sample_time, uint64_t render_clock) {
    (void)kind;
    switch (status) {
    case SU_OK:
        return SU_AUDIO_TRACE_REASON_OK;
    case SU_E_RESOURCE_LIMIT:
        return SU_AUDIO_TRACE_REASON_POOL_FULL;
    case SU_E_STALE_REVISION:
        return SU_AUDIO_TRACE_REASON_DUPLICATE;
    case SU_E_STALE_HANDLE:
        return SU_AUDIO_TRACE_REASON_STALE_HANDLE;
    case SU_E_INVALID_ARGUMENT:
        return sample_time < render_clock ? SU_AUDIO_TRACE_REASON_LATE : SU_AUDIO_TRACE_REASON_INVALID;
    default:
        return SU_AUDIO_TRACE_REASON_GRAPH;
    }
}

const char *su_audio_trace_reason_name(uint32_t reason) {
    static const char *const names[SU_AUDIO_TRACE_REASON_COUNT] = {
        "ok", "pool_full", "duplicate", "late", "stale_handle", "graph", "invalid"};
    return reason < SU_AUDIO_TRACE_REASON_COUNT ? names[reason] : "unknown";
}

uint32_t su_audio_trace_record(SuAudioTelemetry *telemetry, SuAudioTelemetryEntry *entry) {
    if (telemetry == NULL || entry == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    entry->reason = su_audio_trace_reason(entry->kind, entry->status, entry->sample_time, entry->render_clock);
    if (telemetry->queued == telemetry->capacity) {
        telemetry->pending_drops++;
        telemetry->stats.dropped++;
        return SU_E_RESOURCE_LIMIT;
    }
    entry->seq = telemetry->next_seq++;
    entry->dropped_before = telemetry->pending_drops;
    telemetry->pending_drops = 0u;
    telemetry->ring[(telemetry->head + telemetry->queued) % telemetry->capacity] = *entry;
    telemetry->queued++;
    telemetry->stats.recorded++;
    telemetry->stats.reasons[entry->reason]++;
    return SU_OK;
}

uint32_t su_audio_trace_drain(SuAudioTelemetry *telemetry, SuAudioTelemetryEntry *out, uint32_t capacity,
    uint32_t *count) {
    uint32_t taken = 0u;
    if (count != NULL) {
        *count = 0u;
    }
    if (telemetry == NULL || count == NULL || (out == NULL && capacity != 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    while (taken < capacity && telemetry->queued != 0u) {
        out[taken++] = telemetry->ring[telemetry->head];
        telemetry->head = (telemetry->head + 1u) % telemetry->capacity;
        telemetry->queued--;
    }
    telemetry->stats.drained += taken;
    *count = taken;
    return SU_OK;
}

void su_audio_trace_stats(const SuAudioTelemetry *telemetry, SuAudioTelemetryStats *out) {
    if (telemetry != NULL && out != NULL) {
        *out = telemetry->stats;
        out->queued = telemetry->queued;
    }
}

uint32_t su_audio_inspect_post(SuAudioEvents *events, SuAudioTelemetry *telemetry, const SuAudioTrigger *trigger,
    SuAudioInstance *out, SuAudioGraphDiagnostic *diagnostic) {
    uint64_t clock = su_audio_events_clock(events);
    uint32_t status = su_audio_event_post(events, trigger, out, diagnostic);
    if (telemetry != NULL && trigger != NULL) {
        SuAudioTelemetryEntry entry;
        memset(&entry, 0, sizeof entry);
        entry.kind = SU_AUDIO_TRACE_POST;
        entry.id = trigger->event_id;
        entry.sample_time = trigger->sample_time;
        entry.render_clock = clock;
        entry.instance = out != NULL ? *out : 0u;
        entry.status = status;
        (void)su_audio_trace_record(telemetry, &entry);
    }
    return status;
}

static void offline_observer(void *user, uint32_t trace_index, const SuAudioTraceCommand *command,
    uint32_t status, uint64_t render_clock, SuAudioInstance instance) {
    SuAudioTelemetryEntry entry;
    (void)trace_index;
    memset(&entry, 0, sizeof entry);
    entry.kind = command->kind;
    entry.id = command->kind == SU_AUDIO_TRACE_POST ? command->event_id
        : command->kind == SU_AUDIO_TRACE_STOP_SCOPE ? command->scope_id : command->emitter_index;
    entry.sample_time = command->sample_time;
    entry.render_clock = render_clock;
    entry.instance = instance;
    entry.status = status;
    (void)su_audio_trace_record((SuAudioTelemetry *)user, &entry);
}

uint32_t su_audio_inspect_attach_offline(SuAudioOffline *offline, SuAudioTelemetry *telemetry) {
    return su_audio_offline_observe(offline, telemetry != NULL ? offline_observer : NULL, telemetry);
}

/* ---- snapshots ---- */

static void add_bus(SuAudioInspectSnapshot *s, uint32_t bus_id, uint32_t state) {
    uint32_t index;
    uint32_t at;
    if (state > 1u) {
        return;
    }
    for (index = 0; index < s->bus_count; index++) {
        if (s->buses[index].bus_id == bus_id) {
            if (state == 0u) {
                s->buses[index].pending++;
            } else {
                s->buses[index].playing++;
            }
            return;
        }
    }
    if (s->bus_count == s->bus_capacity) {
        s->truncated |= SU_AUDIO_INSPECT_TRUNC_BUSES;
        return;
    }
    for (at = s->bus_count; at > 0u && s->buses[at - 1u].bus_id > bus_id; at--) {
        s->buses[at] = s->buses[at - 1u];
    }
    memset(&s->buses[at], 0, sizeof s->buses[at]);
    s->buses[at].bus_id = bus_id;
    if (state == 0u) {
        s->buses[at].pending = 1u;
    } else {
        s->buses[at].playing = 1u;
    }
    s->bus_count++;
}

static void add_emitter(SuAudioInspectSnapshot *s, const SuAudioScene *scene, SuAudioEmitter emitter) {
    SuAudioSpatialSnapshot spatial;
    SuAudioInspectEmitter *e;
    uint32_t index;
    if (emitter == 0u || scene == NULL) {
        return;
    }
    for (index = 0; index < s->emitter_count; index++) {
        if (s->emitters[index].handle == emitter) {
            s->emitters[index].instances++;
            return;
        }
    }
    if (su_audio_scene_snapshot(scene, emitter, &spatial) != SU_OK) {
        return;
    }
    if (s->emitter_count == s->emitter_capacity) {
        s->truncated |= SU_AUDIO_INSPECT_TRUNC_EMITTERS;
        return;
    }
    e = &s->emitters[s->emitter_count++];
    e->handle = emitter;
    e->curve = spatial.emitter.curve;
    e->instances = 1u;
    e->distance_m = spatial.distance_m;
    e->curve_gain_db = spatial.curve.gain_db;
    e->scale = spatial.emitter.scale;
    e->direction_listener = spatial.direction_listener;
}

uint32_t su_audio_inspect(const SuAudioEvents *events, const SuAudioScene *scene, const SuAudioAssets *assets,
    SuAudioInspectSnapshot *s) {
    SuAudioEventLeafView leaves[64];
    SuAudioEventInstanceView view;
    uint32_t slot;
    if (events == NULL || s == NULL || s->struct_size != sizeof *s
        || (s->instances == NULL && s->instance_capacity != 0u) || (s->voices == NULL && s->voice_capacity != 0u)
        || (s->emitters == NULL && s->emitter_capacity != 0u) || (s->buses == NULL && s->bus_capacity != 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    s->instance_count = 0u;
    s->voice_count = 0u;
    s->emitter_count = 0u;
    s->bus_count = 0u;
    s->truncated = 0u;
    memset(&s->assets, 0, sizeof s->assets);
    su_audio_events_stats(events, &s->stats);
    (void)su_audio_commit_cursor_snapshot(events, &s->cursor);
    if (assets != NULL) {
        su_audio_assets_stats(assets, &s->assets);
    }
    for (slot = 0; su_audio_events_view(events, slot, &s->budget, &view, leaves, 64u) == SU_OK; slot++) {
        uint32_t index;
        uint32_t first = s->voice_count;
        if (view.state == 0u) {
            continue;
        }
        add_emitter(s, scene, view.emitter);
        for (index = 0; index < view.count; index++) {
            const SuAudioEventLeafView *leaf;
            if (index >= 64u) {
                s->truncated |= SU_AUDIO_INSPECT_TRUNC_VOICES;
                break;
            }
            leaf = &leaves[index];
            add_bus(s, leaf->data.bus_id, leaf->state);
            if (s->voice_count == s->voice_capacity) {
                s->truncated |= SU_AUDIO_INSPECT_TRUNC_VOICES;
                continue;
            }
            s->voices[s->voice_count].instance = view.handle;
            s->voices[s->voice_count].start_sample = leaf->start_sample;
            s->voices[s->voice_count].node_id = leaf->data.node_id;
            s->voices[s->voice_count].bus_id = leaf->data.bus_id;
            s->voices[s->voice_count].asset_id = leaf->data.asset.id;
            s->voices[s->voice_count].asset_generation = leaf->data.asset.generation;
            s->voices[s->voice_count].state = leaf->state;
            s->voices[s->voice_count].gain = leaf->gain;
            s->voices[s->voice_count].pitch = leaf->pitch;
            s->voices[s->voice_count].lowpass_hz = leaf->lowpass_hz;
            s->voice_count++;
        }
        if (s->instance_count == s->instance_capacity) {
            s->truncated |= SU_AUDIO_INSPECT_TRUNC_INSTANCES;
            continue;
        }
        s->instances[s->instance_count].handle = view.handle;
        s->instances[s->instance_count].emitter = view.emitter;
        s->instances[s->instance_count].order = view.order;
        s->instances[s->instance_count].scope_id = view.scope;
        s->instances[s->instance_count].state = view.state;
        s->instances[s->instance_count].first_voice = first;
        s->instances[s->instance_count].voice_count = s->voice_count - first;
        s->instance_count++;
    }
    return SU_OK;
}

/* ---- text export (measures the required size even when out is short) ---- */

typedef struct Writer {
    char *out;
    size_t capacity;
    size_t length;
    int failed;
} Writer;

static void emit(Writer *w, const char *format, ...) {
    va_list args;
    int n;
    char *at = w->out != NULL && w->length < w->capacity ? w->out + w->length : NULL;
    size_t room = at != NULL ? w->capacity - w->length : 0u;
    va_start(args, format);
    n = vsnprintf(at, room, format, args);
    va_end(args);
    if (n < 0) {
        w->failed = 1;
        return;
    }
    w->length += (size_t)n;
}

static uint32_t finish(Writer *w, size_t *written) {
    *written = w->length;
    if (w->failed) {
        return SU_E_INTERNAL;
    }
    return w->out != NULL && w->length < w->capacity ? SU_OK : SU_E_RESOURCE_LIMIT;
}

static double finite_or_zero(double value) {
    return value == value && value < 1e300 && value > -1e300 ? value : 0.0;
}

uint32_t su_audio_inspect_json(const SuAudioInspectSnapshot *s, char *out, size_t capacity, size_t *written) {
    Writer w = {out, capacity, 0u, 0};
    uint32_t index;
    if (written != NULL) {
        *written = 0u;
    }
    if (s == NULL || written == NULL || s->struct_size != sizeof *s) {
        return SU_E_INVALID_ARGUMENT;
    }
    emit(&w, "{\"schema\":\"su_audio_inspect_v1\",\"budget\":{\"sample_rate\":%u,\"max_instances\":%u,"
        "\"max_leaves_per_instance\":%u,\"max_logical_voices\":%u,\"command_capacity\":%u},",
        s->budget.sample_rate, s->budget.max_instances, s->budget.max_leaves_per_instance,
        s->budget.max_logical_voices, s->budget.command_capacity);
    emit(&w, "\"stats\":{\"render_clock\":%llu,\"delivered_frames\":%llu,\"runtime_bytes\":%llu,\"admitted\":%llu,"
        "\"refused\":%llu,\"duplicates\":%llu,\"late_commands\":%llu,\"render_failures\":%llu,\"instances\":%u,"
        "\"logical_voices\":%u,\"queued_commands\":%u,\"resident_bytes\":%llu,\"refused_admissions\":%llu,"
        "\"session_generation\":%llu,\"next_seq\":%llu,\"truncated\":%u},",
        (unsigned long long)s->stats.render_clock, (unsigned long long)s->stats.delivered_frames,
        (unsigned long long)s->stats.runtime_bytes, (unsigned long long)s->stats.admitted,
        (unsigned long long)s->stats.refused, (unsigned long long)s->stats.duplicates,
        (unsigned long long)s->stats.late_commands, (unsigned long long)s->stats.render_failures,
        s->stats.instances, s->stats.logical_voices, s->stats.queued_commands,
        (unsigned long long)s->assets.resident_bytes, (unsigned long long)s->assets.refused_admissions,
        (unsigned long long)s->cursor.session_generation, (unsigned long long)s->cursor.next_seq, s->truncated);
    emit(&w, "\"instances\":[");
    for (index = 0; index < s->instance_count; index++) {
        const SuAudioInspectInstance *i = &s->instances[index];
        emit(&w, "%s{\"handle\":%llu,\"emitter\":%llu,\"order\":%llu,\"scope_id\":%u,\"state\":%u,"
            "\"first_voice\":%u,\"voice_count\":%u}", index ? "," : "", (unsigned long long)i->handle,
            (unsigned long long)i->emitter, (unsigned long long)i->order, i->scope_id, i->state, i->first_voice,
            i->voice_count);
    }
    emit(&w, "],\"voices\":[");
    for (index = 0; index < s->voice_count; index++) {
        const SuAudioInspectVoice *v = &s->voices[index];
        emit(&w, "%s{\"instance\":%llu,\"start_sample\":%llu,\"node_id\":%u,\"bus_id\":%u,\"asset\":{\"id\":%u,"
            "\"generation\":%u},\"state\":%u,\"gain\":%.6g,\"pitch\":%.6g,\"lowpass_hz\":%.6g}", index ? "," : "",
            (unsigned long long)v->instance, (unsigned long long)v->start_sample, v->node_id, v->bus_id, v->asset_id,
            v->asset_generation, v->state, finite_or_zero(v->gain), finite_or_zero(v->pitch),
            finite_or_zero(v->lowpass_hz));
    }
    emit(&w, "],\"emitters\":[");
    for (index = 0; index < s->emitter_count; index++) {
        const SuAudioInspectEmitter *e = &s->emitters[index];
        emit(&w, "%s{\"handle\":%llu,\"curve\":%u,\"instances\":%u,\"distance_m\":%.6g,\"curve_gain_db\":%.6g,"
            "\"scale\":%.6g,\"direction\":[%.6g,%.6g,%.6g]}", index ? "," : "", (unsigned long long)e->handle,
            e->curve, e->instances, finite_or_zero(e->distance_m), finite_or_zero(e->curve_gain_db),
            finite_or_zero(e->scale), finite_or_zero(e->direction_listener.x),
            finite_or_zero(e->direction_listener.y), finite_or_zero(e->direction_listener.z));
    }
    emit(&w, "],\"buses\":[");
    for (index = 0; index < s->bus_count; index++) {
        emit(&w, "%s{\"bus_id\":%u,\"pending\":%u,\"playing\":%u}", index ? "," : "", s->buses[index].bus_id,
            s->buses[index].pending, s->buses[index].playing);
    }
    emit(&w, "]}");
    return finish(&w, written);
}

uint32_t su_audio_trace_json(const SuAudioTelemetryEntry *entries, uint32_t count, char *out, size_t capacity,
    size_t *written) {
    Writer w = {out, capacity, 0u, 0};
    uint32_t index;
    if (written != NULL) {
        *written = 0u;
    }
    if (written == NULL || (entries == NULL && count != 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    emit(&w, "{\"schema\":\"su_audio_trace_v1\",\"entries\":[");
    for (index = 0; index < count; index++) {
        const SuAudioTelemetryEntry *e = &entries[index];
        emit(&w, "%s{\"seq\":%llu,\"dropped_before\":%llu,\"kind\":%u,\"id\":%u,\"sample_time\":%llu,"
            "\"render_clock\":%llu,\"instance\":%llu,\"status\":\"%s\",\"reason\":\"%s\"}", index ? "," : "",
            (unsigned long long)e->seq, (unsigned long long)e->dropped_before, e->kind, e->id,
            (unsigned long long)e->sample_time, (unsigned long long)e->render_clock,
            (unsigned long long)e->instance, su_status_name(e->status), su_audio_trace_reason_name(e->reason));
    }
    emit(&w, "]}");
    return finish(&w, written);
}

uint32_t su_audio_trace_csv(const SuAudioTelemetryEntry *entries, uint32_t count, char *out, size_t capacity,
    size_t *written) {
    Writer w = {out, capacity, 0u, 0};
    uint32_t index;
    if (written != NULL) {
        *written = 0u;
    }
    if (written == NULL || (entries == NULL && count != 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    emit(&w, "seq,dropped_before,kind,id,sample_time,render_clock,instance,status,reason\n");
    for (index = 0; index < count; index++) {
        const SuAudioTelemetryEntry *e = &entries[index];
        emit(&w, "%llu,%llu,%u,%u,%llu,%llu,%llu,%s,%s\n", (unsigned long long)e->seq,
            (unsigned long long)e->dropped_before, e->kind, e->id, (unsigned long long)e->sample_time,
            (unsigned long long)e->render_clock, (unsigned long long)e->instance, su_status_name(e->status),
            su_audio_trace_reason_name(e->reason));
    }
    return finish(&w, written);
}

/* ---- component diagnostics ---- */

static double db_or_floor(double linear) {
    return linear > 1e-10 ? 20.0 * log10(linear) : -200.0;
}

uint32_t su_audio_inspect_bus_json(SuAudioBusTree *tree, int reset, char *out, size_t capacity, size_t *written) {
    Writer w = {out, capacity, 0u, 0};
    uint32_t index;
    SuAudioBusId id;
    if (written != NULL) {
        *written = 0u;
    }
    if (tree == NULL || written == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    emit(&w, "{\"schema\":\"su_audio_bus_meters_v1\",\"runtime_bytes\":%llu,\"buses\":[",
        (unsigned long long)su_audio_bus_tree_bytes(tree));
    for (index = 0; su_audio_bus_order(tree, index, &id) == SU_OK; index++) {
        SuAudioBusMeter meter;
        float gain = 0.0f;
        if (su_audio_bus_meter_read(tree, id, &meter, reset) != SU_OK || su_audio_bus_gain_at(tree, id, &gain) != SU_OK) {
            return SU_E_INTERNAL;
        }
        emit(&w, "%s{\"bus_id\":%u,\"order\":%u,\"peak_db\":%.3f,\"rms_db\":%.3f,\"true_peak_db\":%.3f,"
            "\"fader_db\":%.3f,\"duck_db\":%.3f,\"hdr_db\":%.3f,\"gain\":%.6g,\"frames\":%llu}",
            index ? "," : "", id, index, db_or_floor(meter.peak), db_or_floor(meter.rms),
            db_or_floor(meter.true_peak), finite_or_zero(meter.fader_db), finite_or_zero(meter.duck_db),
            finite_or_zero(meter.hdr_db), finite_or_zero(gain), (unsigned long long)meter.frames);
    }
    emit(&w, "]}");
    return finish(&w, written);
}

uint32_t su_audio_inspect_voice_pool_json(const SuAudioVoicePool *pool, char *out, size_t capacity,
    size_t *written) {
    Writer w = {out, capacity, 0u, 0};
    SuAudioVoiceStats stats;
    uint32_t index;
    if (written != NULL) {
        *written = 0u;
    }
    if (pool == NULL || written == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    su_audio_voice_pool_stats(pool, &stats);
    emit(&w, "{\"schema\":\"su_audio_voice_pool_v1\",\"clock\":%llu,\"logical\":%u,\"physical\":%u,"
        "\"virtual\":%u,\"transitions\":%u,\"logical_peak\":%u,\"physical_peak\":%u,\"virtual_peak\":%u,"
        "\"admitted\":%llu,\"refused\":%llu,\"stolen\":%llu,\"woken\":%llu,\"expired\":%llu,\"deferred\":%llu,"
        "\"runtime_bytes\":%llu,\"reasons\":{", (unsigned long long)stats.clock, stats.logical, stats.physical,
        stats.virtual_voices, stats.transitions, stats.logical_peak, stats.physical_peak, stats.virtual_peak,
        (unsigned long long)stats.admitted, (unsigned long long)stats.refused, (unsigned long long)stats.stolen,
        (unsigned long long)stats.woken, (unsigned long long)stats.expired, (unsigned long long)stats.deferred,
        (unsigned long long)stats.runtime_bytes);
    for (index = 0; index < SU_AUDIO_REASON_COUNT; index++) {
        emit(&w, "%s\"%s\":%llu", index ? "," : "", su_audio_voice_reason_name(index),
            (unsigned long long)stats.reasons[index]);
    }
    emit(&w, "}}");
    return finish(&w, written);
}

uint32_t su_audio_inspect_params_json(const SuAudioParams *params, char *out, size_t capacity, size_t *written) {
    Writer w = {out, capacity, 0u, 0};
    SuAudioParamStats stats;
    if (written != NULL) {
        *written = 0u;
    }
    if (params == NULL || written == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    su_audio_params_stats(params, &stats);
    emit(&w, "{\"schema\":\"su_audio_params_v1\",\"sets\":%llu,\"clamped\":%llu,\"rejected\":%llu,"
        "\"late\":%llu,\"scoped_values\":%u,\"scoped_capacity\":%u,\"runtime_bytes\":%llu}",
        (unsigned long long)stats.sets, (unsigned long long)stats.clamped, (unsigned long long)stats.rejected,
        (unsigned long long)stats.late, stats.scoped_values, stats.scoped_capacity,
        (unsigned long long)stats.runtime_bytes);
    return finish(&w, written);
}

/* ---- acceptance manifest ---- */

/* Component status for this build. Update when later steps merge. */
static const char *const capabilities[][2] = {
    {"prepared_assets", "implemented"},
    {"spatial_camera", "implemented"},
    {"event_graph", "implemented"},
    {"event_runtime", "implemented"},
    {"offline_render", "implemented"},
    {"inspection", "implemented"},
    {"voice_virtualisation", "implemented as a standalone pool; owner-composed, not wired into the event runtime"},
    {"bus_tree_ducking", "implemented as a standalone bus tree; owner-composed, not wired into the event runtime"},
    {"parameters_music", "implemented as standalone parameters/music; owner-composed, not wired into the event runtime"},
    {"clustering", "gap: not merged in this build"},
    {"bank_format_import", "out of scope: downstream adapter"},
    {"control_plane_methods", "gap: audio.inspect/audio.trace/audio.render.offline are not registered"}};

uint32_t su_audio_acceptance_manifest_json(const SuAudioOffline *offline, const void *wav, size_t wav_bytes,
    char *out, size_t capacity, size_t *written) {
    Writer w = {out, capacity, 0u, 0};
    SuAudioOfflineMetrics m;
    SuSha256 ctx;
    uint8_t digest[32];
    char wav_hex[65];
    char payload_hex[65];
    const uint8_t *bytes = (const uint8_t *)wav;
    size_t index;
    if (written != NULL) {
        *written = 0u;
    }
    if (offline == NULL || written == NULL || wav == NULL || wav_bytes < 44u
        || su_audio_offline_metrics(offline, &m) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (memcmp(bytes, "RIFF", 4u) != 0 || memcmp(bytes + 36, "data", 4u) != 0
        || wav_bytes - 44u != m.frames * 8u) {
        return SU_E_SCHEMA_MISMATCH;
    }
    su_sha256_init(&ctx);
    su_sha256_update(&ctx, bytes + 44, wav_bytes - 44u);
    su_sha256_final(&ctx, digest);
    su_sha256_hex(digest, payload_hex);
    if (strcmp(payload_hex, m.pcm_sha256) != 0) {
        return SU_E_SCHEMA_MISMATCH;
    }
    su_sha256_init(&ctx);
    su_sha256_update(&ctx, wav, wav_bytes);
    su_sha256_final(&ctx, digest);
    su_sha256_hex(digest, wav_hex);
    emit(&w, "{\"schema\":\"su_audio_acceptance_v1\",\"numeric_profile\":\"%s\",\"source\":{\"source_sha256\":\"%s\","
        "\"graph_sha256\":\"%s\"},\"artifact\":{\"format\":\"wav_f32_stereo\",\"bytes\":%llu,\"sha256\":\"%s\","
        "\"pcm_sha256\":\"%s\",\"frames\":%llu,\"sample_rate\":%u,\"start_pts_ns\":%llu,\"end_pts_ns\":%llu},"
        "\"capabilities\":{", SU_AUDIO_OFFLINE_PROFILE, m.source_sha256, m.graph_sha256,
        (unsigned long long)wav_bytes, wav_hex, m.pcm_sha256, (unsigned long long)m.frames, m.sample_rate,
        (unsigned long long)m.start_pts_ns, (unsigned long long)m.end_pts_ns);
    for (index = 0; index < sizeof capabilities / sizeof capabilities[0]; index++) {
        emit(&w, "%s\"%s\":\"%s\"", index ? "," : "", capabilities[index][0], capabilities[index][1]);
    }
    emit(&w, "}}");
    return finish(&w, written);
}
