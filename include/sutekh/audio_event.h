#ifndef SUTEKH_AUDIO_EVENT_H
#define SUTEKH_AUDIO_EVENT_H

#include "sutekh/audio_asset.h"
#include "sutekh/audio_spatial.h"
#include "sutekh/audio_graph.h"
#include "sutekh/audio_voice_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SU_AUDIO_EVENT_QUANTUM 128u

typedef uint64_t SuAudioInstance;
typedef struct SuAudioEvents SuAudioEvents;

typedef struct SuAudioEventsDesc {
    uint32_t struct_size;
    uint32_t sample_rate;
    uint32_t max_instances;
    uint32_t max_leaves_per_instance;
    uint32_t max_logical_voices;
    uint32_t command_capacity;
    uint64_t project_seed;
    uint64_t session_generation;
    uint64_t start_sample;
} SuAudioEventsDesc;

typedef struct SuAudioTrigger {
    uint32_t struct_size;
    uint32_t event_id;
    SuAudioEmitter emitter;
    uint64_t sample_time;
    uint64_t commit_seq;
    uint64_t session_generation;
    uint32_t scope_id;
} SuAudioTrigger;

typedef struct SuAudioCommitCursor {
    uint64_t session_generation;
    uint64_t next_seq;
} SuAudioCommitCursor;

typedef struct SuAudioEventStats {
    uint64_t render_clock;
    uint64_t delivered_frames;
    uint64_t runtime_bytes;
    uint64_t admitted;
    uint64_t refused;
    uint64_t duplicates;
    uint64_t late_commands;
    uint64_t render_failures;
    uint32_t instances;
    uint32_t logical_voices;
    uint32_t queued_commands;
} SuAudioEventStats;

uint32_t su_audio_events_create(const SuAudioEventsDesc *desc, SuAudioAssets *assets,
                                SuAudioScene *scene, SuAudioGraph *graph, SuAudioEvents **out);
void su_audio_events_destroy(SuAudioEvents *events);
uint32_t su_audio_event_post(SuAudioEvents *events, const SuAudioTrigger *trigger,
                             SuAudioInstance *out, SuAudioGraphDiagnostic *diagnostic);
uint32_t su_audio_instance_stop(SuAudioEvents *events, SuAudioInstance instance,
                                uint64_t sample_time, uint32_t release_frames);
uint32_t su_audio_emitter_stop(SuAudioEvents *events, SuAudioEmitter emitter,
                               uint64_t sample_time, uint32_t release_frames);
uint32_t su_audio_scope_stop(SuAudioEvents *events, uint32_t scope_id,
                             uint64_t sample_time, uint32_t release_frames);
uint32_t su_audio_events_render(SuAudioEvents *events, float *out, uint32_t frames);
void su_audio_events_update(SuAudioEvents *events);
int su_audio_instance_active(const SuAudioEvents *events, SuAudioInstance instance);
uint64_t su_audio_events_clock(const SuAudioEvents *events);
void su_audio_events_stats(const SuAudioEvents *events, SuAudioEventStats *out);
uint32_t su_audio_commit_cursor_snapshot(const SuAudioEvents *events, SuAudioCommitCursor *out);
uint32_t su_audio_commit_cursor_restore(SuAudioEvents *events, const SuAudioCommitCursor *cursor);
uint32_t su_audio_events_session(SuAudioEvents *events, uint64_t session_generation);

/* ---- Integrated voice runtime (versioned hook) ----
 * Attaching a hook replaces the private legacy mixer of an idle runtime: admitted leaves become logical voices
 * of the A05 pool (ranking, virtualisation, stealing and release), started and stopped at their exact sample
 * times, with pitch, camera spatialisation (attenuation, equal-power pan) and the curve low-pass applied per
 * voice by the pool's controlled render path. Later hook versions append fields; struct_size selects them. */
#define SU_AUDIO_EVENTS_HOOK_VERSION 1u

/* First matching rule wins: node_id (0 = any) and bus_id (0 = any) of the graph leaf. */
typedef struct SuAudioVoicePolicyRule {
    uint32_t node_id;
    uint32_t bus_id;
    SuAudioVoicePolicy policy;
} SuAudioVoicePolicyRule;

typedef struct SuAudioEventsHook {
    uint32_t struct_size;
    uint32_t version;                      /* SU_AUDIO_EVENTS_HOOK_VERSION */
    SuAudioVoicePool *voices;              /* required; idle, max_logical >= max_logical_voices */
    const SuAudioVoicePolicyRule *policies; /* copied */
    uint32_t policy_count;                 /* <= 4096 */
    SuAudioVoicePolicy default_policy;
} SuAudioEventsHook;

/* Valid only while no instance, stop command or partially consumed quantum exists. A stop with
 * release_frames == 0 uses the voice policy release. */
uint32_t su_audio_events_attach(SuAudioEvents *events, const SuAudioEventsHook *hook);

#ifdef __cplusplus
}
#endif
#endif
