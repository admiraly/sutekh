#ifndef SUTEKH_AUDIO_EVENT_H
#define SUTEKH_AUDIO_EVENT_H

#include "sutekh/audio_asset.h"
#include "sutekh/audio_spatial.h"
#include "sutekh/audio_graph.h"
#include "sutekh/audio_voice_policy.h"
#include "sutekh/audio_bus.h"
#include "sutekh/audio_cluster.h"

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

#define SU_AUDIO_EVENTS_MAX_LOUDNESS_RULES 256u

typedef struct SuAudioLoudnessRule {
    uint32_t node_id;
    uint32_t bus_id;
    float loudness_db;
} SuAudioLoudnessRule;

typedef void (*SuAudioObjectOutputFn)(void *user, const float *pcm, uint32_t frames,
                                     uint32_t channels, uint64_t sample_time);

/* Host bus input: PCM the owner supplies into a bus once per 128-frame quantum, from inside the runtime's render
 * (so on the owner's thread), for sources the runtime does not play itself (for example a long track decoded by a
 * legacy SuAudioStream). render receives interleaved stereo cleared to zero, the frame count and the sample time
 * of the quantum's first frame; it must not call back into the runtime. The input then passes the bus tree like
 * any voice (faders, ducking, meters, the master limiter). Host input is not journaled: replays render it as
 * silence. */
typedef void (*SuAudioBusInputFn)(void *user, float *stereo, uint32_t frames, uint64_t sample_time);
typedef struct SuAudioBusInput {
    uint32_t bus_id;                       /* 0 or an unknown-free bus ID; 0 = the root (without buses: the output) */
    SuAudioBusInputFn render;
    void *user;
} SuAudioBusInput;
#define SU_AUDIO_EVENTS_MAX_INPUTS 16u

typedef struct SuAudioEventsHook {
    uint32_t struct_size;
    uint32_t version;                      /* SU_AUDIO_EVENTS_HOOK_VERSION */
    SuAudioVoicePool *voices;              /* required; idle, max_logical >= max_logical_voices */
    const SuAudioVoicePolicyRule *policies; /* copied */
    uint32_t policy_count;                 /* <= 4096 */
    SuAudioVoicePolicy default_policy;
    /* Appended fields; read only when struct_size covers them (SU_AUDIO_EVENTS_HOOK_HAS). */
    struct SuAudioParams *params;          /* optional: expansion snapshots, voice and bus-gain controls */
    SuAudioBusTree *buses;                 /* optional: voices mix into their leaf bus_id (unknown/0: root) */
    struct SuAudioMusic *music;            /* optional: rendered into music_bus_id (0: root) each quantum */
    uint32_t music_bus_id;
    float limiter_knee;                    /* master soft limiter knee in [0.1, 1); 0 = hard clamp */
    SuAudioCluster *cluster;               /* optional: fed every quantum from cluster_inputs */
    uint32_t cluster_inputs;               /* SU_AUDIO_EVENTS_CLUSTER_* */
    float doppler;                         /* spatial voices: Doppler amount in [0, 1] (0 = none), su_audio_doppler law
                                            * from the emitter and listener velocities, applied to the pitch */
    float speed_of_sound;                  /* metres/second for the Doppler law; 0 = 343 */
    const SuAudioBusInput *inputs;         /* copied; at most SU_AUDIO_EVENTS_MAX_INPUTS */
    uint32_t input_count;
    const SuAudioLoudnessRule *loudness_rules;
    uint32_t loudness_rule_count;
    const SuAudioSpeakerLayout *speaker_layout;
    SuAudioObjectOutputFn object_output;
    void *object_user;
} SuAudioEventsHook;

/* Clustering inputs: rendered voices as objects (ID = logical voice handle, steal tails with bit 63 set;
 * listener-relative direction and distance; mono pre-pan signal; non-spatial voices as beds) and the
 * pre-master bus outputs (direct children of the root, ID = bit 62 | bus ID) as stereo beds. Clustering
 * describes an object representation for an object renderer; the runtime's stereo output does not depend on
 * it. The cluster's max_objects must cover max_physical + transition_reserve (+ pre-master buses). */
#define SU_AUDIO_EVENTS_CLUSTER_VOICES 1u
#define SU_AUDIO_EVENTS_CLUSTER_BUSES 2u

#define SU_AUDIO_EVENTS_HOOK_HAS(hook, field) \
    ((hook)->struct_size >= offsetof(SuAudioEventsHook, field) + sizeof((hook)->field))

/* Valid only while no instance, stop command or partially consumed quantum exists. A stop with
 * release_frames == 0 uses the voice policy release.
 *
 * Optional components: with params, su_audio_event_post expands through su_audio_param_expand_state for the
 * (emitter, new instance) target at the trigger time (switch and blend nodes), and every voice applies its
 * GAIN_DB/PITCH/LOWPASS_HZ bindings at the end of each quantum; BUS_GAIN_DB bindings drive their bus faders
 * (owned by the bindings). The params clock advances with the runtime, and retired instances' scoped values
 * are cleared. With buses, every voice renders into its bus, sidechain ducking detects the real bus signals and
 * the tree output feeds the master limiter. Music must share the runtime clock and sample rate. All referenced
 * objects must outlive the runtime and are then driven only by it (their own render calls must not be used). */
uint32_t su_audio_events_attach(SuAudioEvents *events, const SuAudioEventsHook *hook);

/* ---- Direct checkpoints of the integrated runtime ----
 * One blob holds the event runtime (instances, leaves, queued stops, commit cursor, stats, the partially
 * consumed quantum), the voice pool (logical and physical voices, cursors, filters, ramps, steal tails), the
 * bus tree (faders, ramps, duck envelopes, delayed sidechains, meters), parameters, music, clustering and the
 * scene's emitters and listener. Assets are written as (id, generation) and re-pinned on load; no pointers are
 * written. Loading writes that state into a runtime attached to identically configured components (a layout
 * fingerprint is checked), so restore cost depends only on the configured capacities, never on the position.
 * The layout is the build's in-memory numeric representation (profile audio_float_local_v1): load only in the
 * build that saved. Save with out == NULL reports the size. Requires an attached runtime
 * (SU_E_UNSUPPORTED_METHOD otherwise). Load validates the header, fingerprint, checksum and that every pinned
 * asset generation is still resident before changing anything (SU_E_SCHEMA_MISMATCH otherwise). */
#define SU_AUDIO_EVENTS_CHECKPOINT_VERSION 1u
#define SU_AUDIO_EVENTS_CHECKPOINT_HEADER 64u

typedef struct SuAudioEventsCheckpointInfo {
    uint32_t version;
    uint64_t fingerprint;
    uint64_t body_bytes;
    uint64_t clock;                        /* render clock at save */
    uint64_t delivered_frames;
} SuAudioEventsCheckpointInfo;

uint32_t su_audio_events_checkpoint_save(SuAudioEvents *events, void *out, size_t capacity, size_t *written);
uint32_t su_audio_events_checkpoint_load(SuAudioEvents *events, const void *data, size_t bytes);
uint32_t su_audio_events_checkpoint_info(const void *data, size_t bytes, SuAudioEventsCheckpointInfo *out);

#ifdef __cplusplus
}
#endif
#endif
