#ifndef SUTEKH_AUDIO_INSPECT_H
#define SUTEKH_AUDIO_INSPECT_H

/* Structured event-audio inspection, bounded telemetry and acceptance
 * manifests (A10). Additive; snapshots carry numeric IDs and handles only,
 * never pointers or file paths. Nothing here writes files. */

#include "sutekh/audio_offline.h"
#include "sutekh/audio_bus.h"
#include "sutekh/audio_parameter.h"
#include "sutekh/audio_voice_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SU_AUDIO_INSPECT_VERSION 1u
#define SU_AUDIO_TELEMETRY_MAX 1048576u

/* Stable reason codes for trace entries. */
enum {
    SU_AUDIO_TRACE_REASON_OK = 0,
    SU_AUDIO_TRACE_REASON_POOL_FULL = 1,      /* instance pool, voice budget or stop queue full */
    SU_AUDIO_TRACE_REASON_DUPLICATE = 2,      /* commit sequence at or below the watermark */
    SU_AUDIO_TRACE_REASON_LATE = 3,           /* targets already rendered audio */
    SU_AUDIO_TRACE_REASON_STALE_HANDLE = 4,   /* instance/emitter handle no longer valid */
    SU_AUDIO_TRACE_REASON_GRAPH = 5,          /* unknown event, expansion or asset failure */
    SU_AUDIO_TRACE_REASON_INVALID = 6,
    SU_AUDIO_TRACE_REASON_COUNT = 7
};

/* Telemetry entry kinds reuse SU_AUDIO_TRACE_* command kinds. */
typedef struct SuAudioTelemetryEntry {
    uint64_t seq;            /* accepted-entry order, gap-free */
    uint64_t dropped_before;  /* entries refused by backpressure since the previous one */
    uint64_t sample_time;
    uint64_t render_clock;
    uint64_t instance;
    uint32_t kind;
    uint32_t id;             /* event, scope or 1-based emitter index */
    uint32_t status;
    uint32_t reason;
} SuAudioTelemetryEntry;

typedef struct SuAudioTelemetry SuAudioTelemetry;

typedef struct SuAudioTelemetryStats {
    uint32_t capacity;
    uint32_t queued;
    uint64_t recorded;
    uint64_t dropped;
    uint64_t drained;
    uint64_t reasons[SU_AUDIO_TRACE_REASON_COUNT];
} SuAudioTelemetryStats;

uint32_t su_audio_trace_begin(uint32_t capacity, SuAudioTelemetry **out);
void su_audio_trace_end(SuAudioTelemetry *telemetry);
/* Fills seq/dropped_before/reason. A full ring refuses the entry (counted in
 * dropped) and returns SU_E_RESOURCE_LIMIT; queued entries are never overwritten. */
uint32_t su_audio_trace_record(SuAudioTelemetry *telemetry, SuAudioTelemetryEntry *entry);
/* Moves up to capacity oldest entries out, in record order. */
uint32_t su_audio_trace_drain(SuAudioTelemetry *telemetry, SuAudioTelemetryEntry *out, uint32_t capacity,
    uint32_t *count);
void su_audio_trace_stats(const SuAudioTelemetry *telemetry, SuAudioTelemetryStats *out);
uint32_t su_audio_trace_reason(uint32_t kind, uint32_t status, uint64_t sample_time, uint64_t render_clock);
const char *su_audio_trace_reason_name(uint32_t reason);

/* Posting wrapper that records the outcome. */
uint32_t su_audio_inspect_post(SuAudioEvents *events, SuAudioTelemetry *telemetry, const SuAudioTrigger *trigger,
    SuAudioInstance *out, SuAudioGraphDiagnostic *diagnostic);
/* Records every submitted command of an offline session into telemetry. */
uint32_t su_audio_inspect_attach_offline(SuAudioOffline *offline, SuAudioTelemetry *telemetry);

typedef struct SuAudioInspectInstance {
    uint64_t handle;
    uint64_t emitter;
    uint64_t order;
    uint32_t scope_id;
    uint32_t state;          /* 1 active, 2 completed awaiting update */
    uint32_t first_voice;
    uint32_t voice_count;
} SuAudioInspectInstance;

typedef struct SuAudioInspectVoice {
    uint64_t instance;
    uint64_t start_sample;
    uint32_t node_id;
    uint32_t bus_id;
    uint32_t asset_id;
    uint32_t asset_generation;
    uint32_t state;          /* 0 pending, 1 playing, 2 finished */
    float gain;
    float pitch;
    float lowpass_hz;
} SuAudioInspectVoice;

typedef struct SuAudioInspectEmitter {
    uint64_t handle;
    uint32_t curve;
    uint32_t instances;
    double distance_m;
    float curve_gain_db;
    float scale;
    SuVec3f direction_listener;
} SuAudioInspectEmitter;

typedef struct SuAudioInspectBus {
    uint32_t bus_id;
    uint32_t pending;
    uint32_t playing;
} SuAudioInspectBus;

enum {
    SU_AUDIO_INSPECT_TRUNC_INSTANCES = 1,
    SU_AUDIO_INSPECT_TRUNC_VOICES = 2,
    SU_AUDIO_INSPECT_TRUNC_EMITTERS = 4,
    SU_AUDIO_INSPECT_TRUNC_BUSES = 8
};

/* Caller supplies the arrays and capacities; counts and truncation are filled.
 * Order is slot order, then expansion order, so equal state gives equal output. */
typedef struct SuAudioInspectSnapshot {
    uint32_t struct_size;
    SuAudioInspectInstance *instances;
    uint32_t instance_capacity;
    uint32_t instance_count;
    SuAudioInspectVoice *voices;
    uint32_t voice_capacity;
    uint32_t voice_count;
    SuAudioInspectEmitter *emitters;
    uint32_t emitter_capacity;
    uint32_t emitter_count;
    SuAudioInspectBus *buses;
    uint32_t bus_capacity;
    uint32_t bus_count;
    uint32_t truncated;
    SuAudioEventsDesc budget;
    SuAudioEventStats stats;
    SuAudioAssetStats assets;
    SuAudioCommitCursor cursor;
} SuAudioInspectSnapshot;

/* scene and assets are optional (NULL omits emitters/asset stats). */
uint32_t su_audio_inspect(const SuAudioEvents *events, const SuAudioScene *scene, const SuAudioAssets *assets,
    SuAudioInspectSnapshot *snapshot);
uint32_t su_audio_inspect_json(const SuAudioInspectSnapshot *snapshot, char *out, size_t capacity, size_t *written);
uint32_t su_audio_trace_json(const SuAudioTelemetryEntry *entries, uint32_t count, char *out, size_t capacity,
    size_t *written);
uint32_t su_audio_trace_csv(const SuAudioTelemetryEntry *entries, uint32_t count, char *out, size_t capacity,
    size_t *written);

/* Component diagnostics. Bus meters are read in execution order (reset != 0
 * restarts each meter window); voice pools and parameters report their stats
 * with stable reason names. These components are composed by the owner; they
 * are not wired into the event runtime. */
uint32_t su_audio_inspect_bus_json(SuAudioBusTree *tree, int reset, char *out, size_t capacity, size_t *written);
uint32_t su_audio_inspect_voice_pool_json(const SuAudioVoicePool *pool, char *out, size_t capacity,
    size_t *written);
uint32_t su_audio_inspect_params_json(const SuAudioParams *params, char *out, size_t capacity, size_t *written);

/* Source/artifact manifest for an offline export. Verifies that the WAV payload
 * is exactly the session's PCM (SU_E_SCHEMA_MISMATCH otherwise) and lists the
 * component capabilities and remaining compatibility gaps. */
uint32_t su_audio_acceptance_manifest_json(const SuAudioOffline *offline, const void *wav, size_t wav_bytes,
    char *out, size_t capacity, size_t *written);

#ifdef __cplusplus
}
#endif
#endif
