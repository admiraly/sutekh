#ifndef SUTEKH_AUDIO_SYSTEM_H
#define SUTEKH_AUDIO_SYSTEM_H

/* One owner object for the integrated event-audio runtime: event runtime, voice pool, scene, optional bus
 * tree, parameters, music and clustering, output (offline render, a render-ahead ring, or the output device),
 * a command journal for offline replay and checkpoint verification, telemetry, direct checkpoints and
 * control-plane methods. Single owner: every call is serialized by the owner; only the device thread reads
 * the ring. The legacy sutekh/audio.h mixer API is separate and unchanged. */

#include "sutekh/audio_inspect.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SU_AUDIO_OUTPUT_OFFLINE = 0u, /* su_audio_system_render pulls frames on demand */
    SU_AUDIO_OUTPUT_RING = 1u,    /* su_audio_system_update renders ahead into a ring read by su_audio_system_ring_read */
    SU_AUDIO_OUTPUT_DEVICE = 2u   /* as RING, read by the output device thread */
};

typedef struct SuAudioSystem SuAudioSystem;

/* Configuration. Every pointer (assets, graph and the descriptor tables) is borrowed and must outlive the
 * system: render.offline and checkpoint verification build fresh systems from it. */
typedef struct SuAudioSystemDesc {
    uint32_t struct_size;
    uint32_t output;                       /* SU_AUDIO_OUTPUT_* */
    uint32_t ring_frames;                  /* RING/DEVICE: render-ahead capacity, a multiple of 128 in 256..2^20 */
    SuAudioAssets *assets;
    SuAudioGraph *graph;
    SuAudioEventsDesc events;
    SuAudioSceneDesc scene;
    const SuAudioCurveDesc *curves;        /* added to the scene in order (curve IDs 1..curve_count) */
    uint32_t curve_count;
    SuAudioVoicePoolDesc voices;           /* sample_rate 0 or the event rate */
    const SuAudioVoicePolicyRule *policies;
    uint32_t policy_count;
    SuAudioVoicePolicy default_policy;
    const SuAudioBusTreeDesc *buses;       /* optional */
    const SuAudioParamsDesc *params;       /* optional */
    const SuAudioMusicDesc *music;         /* optional; start_sample must equal events.start_sample */
    const SuAudioMusicSegmentDesc *segments;
    uint32_t segment_count;
    uint32_t music_bus_id;
    const SuAudioClusterDesc *cluster;     /* optional */
    uint32_t cluster_inputs;               /* SU_AUDIO_EVENTS_CLUSTER_* */
    float limiter_knee;                    /* 0 = hard clamp, else [0.1, 1) */
    uint32_t journal_capacity;             /* commands kept for replay (render.offline, verification); 0 = none */
    uint32_t telemetry_capacity;           /* A10 trace ring; 0 = none */
} SuAudioSystemDesc;

enum {
    SU_AUDIO_SYSTEM_CMD_POST = 1u,
    SU_AUDIO_SYSTEM_CMD_STOP_INSTANCE = 2u,
    SU_AUDIO_SYSTEM_CMD_STOP_EMITTER = 3u,
    SU_AUDIO_SYSTEM_CMD_STOP_SCOPE = 4u,
    SU_AUDIO_SYSTEM_CMD_PARAM = 5u,
    SU_AUDIO_SYSTEM_CMD_SWITCH = 6u,
    SU_AUDIO_SYSTEM_CMD_EMITTER_CREATE = 7u,
    SU_AUDIO_SYSTEM_CMD_EMITTER_UPDATE = 8u,
    SU_AUDIO_SYSTEM_CMD_EMITTER_DESTROY = 9u,
    SU_AUDIO_SYSTEM_CMD_LISTENER = 10u,
    SU_AUDIO_SYSTEM_CMD_MUSIC = 11u,
    SU_AUDIO_SYSTEM_CMD_BUS_GAIN = 12u
};

/* One journaled presentation command. position is the number of frames the event runtime had produced when
 * the command was issued; replay issues it at the same position, so the replayed stream is identical. */
typedef struct SuAudioSystemCommand {
    uint32_t kind;
    uint32_t status;                       /* result of the original call */
    uint64_t position;
    SuAudioTrigger trigger;                /* POST */
    uint64_t target;                       /* STOP_*: instance, emitter or scope; EMITTER_*: emitter */
    uint64_t sample_time;                  /* STOP_*, PARAM, SWITCH, MUSIC */
    uint32_t id;                           /* PARAM/SWITCH id, BUS_GAIN bus */
    uint32_t frames;                       /* STOP_* release, BUS_GAIN ramp */
    float value;                           /* PARAM value, BUS_GAIN dB */
    uint32_t switch_value;
    SuAudioParamScope scope;
    SuAudioEmitterSnapshot emitter;        /* EMITTER_CREATE/UPDATE (pos, vel) */
    SuAudioCameraListener listener;
    SuMusicTransition transition;
} SuAudioSystemCommand;

typedef struct SuAudioSystemStats {
    uint64_t clock;                        /* first sample not yet rendered by the event runtime */
    uint64_t rendered_frames;              /* frames produced by the event runtime */
    uint64_t consumed_frames;              /* RING/DEVICE: frames read from the ring */
    uint32_t ring_fill;
    uint32_t ring_capacity;
    uint64_t underrun_frames;              /* ring reads that found no audio (silence was delivered) */
    uint64_t device_frames;
    uint32_t journal_count;
    uint32_t journal_capacity;
    uint64_t journal_dropped;              /* commands not journaled because the journal was full */
    uint64_t runtime_bytes;                /* components and system storage, excluding allocator overhead */
} SuAudioSystemStats;

uint32_t su_audio_system_create(const SuAudioSystemDesc *desc, SuAudioSystem **out, char message[256]);
void su_audio_system_destroy(SuAudioSystem *system);

/* Component access for reads and configuration-free queries (inspection, meters, cluster outputs). Changing
 * component state directly bypasses the journal. */
SuAudioEvents *su_audio_system_events(SuAudioSystem *system);
SuAudioScene *su_audio_system_scene(SuAudioSystem *system);
SuAudioVoicePool *su_audio_system_voices(SuAudioSystem *system);
SuAudioBusTree *su_audio_system_buses(SuAudioSystem *system);
SuAudioParams *su_audio_system_params(SuAudioSystem *system);
SuAudioMusic *su_audio_system_music(SuAudioSystem *system);
SuAudioCluster *su_audio_system_cluster(SuAudioSystem *system);
SuAudioTelemetry *su_audio_system_telemetry(SuAudioSystem *system);
uint64_t su_audio_system_clock(const SuAudioSystem *system);
void su_audio_system_stats(const SuAudioSystem *system, SuAudioSystemStats *out);

/* Journaled presentation commands (same semantics and status codes as the component calls). */
uint32_t su_audio_system_post(SuAudioSystem *system, const SuAudioTrigger *trigger, SuAudioInstance *out,
                              SuAudioGraphDiagnostic *diagnostic);
uint32_t su_audio_system_stop(SuAudioSystem *system, uint32_t kind, uint64_t target, uint64_t sample_time,
                              uint32_t release_frames); /* kind: SU_AUDIO_SYSTEM_CMD_STOP_* */
uint32_t su_audio_system_param_set(SuAudioSystem *system, SuAudioParamScope scope, SuAudioParamId id, float value,
                                   uint64_t sample_time);
uint32_t su_audio_system_switch_set(SuAudioSystem *system, SuAudioParamScope scope, SuAudioSwitchId id,
                                    uint32_t value, uint64_t sample_time);
uint32_t su_audio_system_emitter_create(SuAudioSystem *system, const SuAudioEmitterSnapshot *desc,
                                        SuAudioEmitter *out);
uint32_t su_audio_system_emitter_update(SuAudioSystem *system, SuAudioEmitter emitter, SuWorldPos pos, SuVec3f vel);
uint32_t su_audio_system_emitter_destroy(SuAudioSystem *system, SuAudioEmitter emitter);
uint32_t su_audio_system_listener_set(SuAudioSystem *system, const SuAudioCameraListener *listener);
uint32_t su_audio_system_music_transition(SuAudioSystem *system, const SuMusicTransition *transition,
                                          uint64_t sample_time, uint64_t *boundary);
uint32_t su_audio_system_bus_gain(SuAudioSystem *system, SuAudioBusId bus, float gain_db, uint32_t ramp_frames);

/* OFFLINE: renders interleaved stereo frames (any chunking gives the same stream). */
uint32_t su_audio_system_render(SuAudioSystem *system, float *out, uint32_t frames);
/* RING/DEVICE: renders whole quanta until the ring is full; call once per game frame. Never blocks. */
uint32_t su_audio_system_update(SuAudioSystem *system);
/* RING: reads up to frames from the ring (what the device thread does); missing audio is delivered as silence
 * and counted in underrun_frames. Returns the number of frames that came from the ring. */
uint32_t su_audio_system_ring_read(SuAudioSystem *system, float *out, uint32_t frames);

/* Applies journal commands at their positions while rendering until the event runtime has produced `frames`
 * frames; out (optional) receives the frames rendered by this call. The system must be OFFLINE and must not
 * have rendered past the first command it applies. */
uint32_t su_audio_system_replay(SuAudioSystem *system, const SuAudioSystemCommand *journal, uint32_t count,
                                uint64_t frames, float *out);
uint32_t su_audio_system_journal(const SuAudioSystem *system, const SuAudioSystemCommand **journal,
                                 uint32_t *count);

/* Direct checkpoints (su_audio_events_checkpoint_*; the system adds no state). Loading empties the ring. */
uint32_t su_audio_system_checkpoint_save(SuAudioSystem *system, void *out, size_t capacity, size_t *written);
uint32_t su_audio_system_checkpoint_load(SuAudioSystem *system, const void *data, size_t bytes);
/* Replay verifier: builds two OFFLINE systems from desc, replays the journal to the checkpoint position in one
 * and loads the checkpoint into the other, then renders verify_frames more in both (applying later journal
 * commands) and requires identical PCM. SU_E_SCHEMA_MISMATCH when they differ. */
uint32_t su_audio_system_checkpoint_verify(const SuAudioSystemDesc *desc, const SuAudioSystemCommand *journal,
    uint32_t count, const void *checkpoint, size_t bytes, uint32_t verify_frames);

/* Control-plane methods for a host's structured control plane: "audio.inspect", "audio.trace.read",
 * "audio.trace.stats", "audio.render.offline" (params {"frames":N}: replays the journal into a fresh offline
 * system and reports frame count, PCM SHA-256, sample/true peak and integrated loudness). Writes one JSON
 * object (NUL-terminated; written excludes the terminator). Unknown methods: SU_E_UNSUPPORTED_METHOD. */
uint32_t su_audio_system_control(SuAudioSystem *system, const char *method, const char *params, size_t params_len,
                                 char *out, size_t capacity, size_t *written);
/* Method names by index; NULL past the end. */
const char *su_audio_system_method(uint32_t index);

#ifdef __cplusplus
}
#endif
#endif
