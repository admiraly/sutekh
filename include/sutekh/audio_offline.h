#ifndef SUTEKH_AUDIO_OFFLINE_H
#define SUTEKH_AUDIO_OFFLINE_H

/* Deterministic offline event-audio rendering, replay checkpoints and measured
 * export (A09). Additive to audio_event.h; no existing ABI changes. */

#include "sutekh/audio_event.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SU_AUDIO_OFFLINE_PROFILE "audio_float_local_v1"
#define SU_AUDIO_OFFLINE_MAX_TRACE 1048576u
#define SU_AUDIO_OFFLINE_MAX_ASSET_REFS 4096u
#define SU_AUDIO_CHECKPOINT_VERSION 1u
/* Fixed header size; add 8 bytes per pinned asset reference. */
#define SU_AUDIO_CHECKPOINT_HEADER_BYTES 256u

enum {
    SU_AUDIO_TRACE_POST = 1,
    SU_AUDIO_TRACE_STOP_INSTANCE = 2,
    SU_AUDIO_TRACE_STOP_EMITTER = 3,
    SU_AUDIO_TRACE_STOP_SCOPE = 4,
    SU_AUDIO_TRACE_EMITTER_MOVE = 5,
    SU_AUDIO_TRACE_LISTENER = 6
};

/* One accepted presentation command. Commands are sorted by sample_time
 * (ties keep trace order). Emitters are 1-based manifest indices (0 = none);
 * STOP_INSTANCE targets the trace index of an earlier POST. Transforms are
 * applied at the start of the 128-frame quantum containing sample_time. */
typedef struct SuAudioTraceCommand {
    uint32_t kind;
    uint32_t event_id;
    uint64_t sample_time;
    uint64_t commit_seq;
    uint32_t emitter_index;
    uint32_t scope_id;
    uint32_t target;
    uint32_t release_frames;
    SuWorldPos pos;
    SuVec3f vel;
    SuQuatf rot;
} SuAudioTraceCommand;

/* Inputs that define one render. The registry and prepared graph are borrowed
 * and must outlive the session. graph_source is the exact prepared JSON (hashed
 * for reports/checkpoints). Emitter curve fields name 1-based manifest curves. */
typedef struct SuAudioOfflineManifest {
    uint32_t struct_size;
    SuAudioEventsDesc events;
    SuAudioAssets *assets;
    SuAudioGraph *graph;
    const void *graph_source;
    size_t graph_source_bytes;
    const SuAudioAssetRef *asset_refs;
    uint32_t asset_ref_count;
    const SuAudioCurveDesc *curves;
    uint32_t curve_count;
    const SuAudioEmitterSnapshot *emitters;
    uint32_t emitter_count;
    SuAudioCameraListener listener;
} SuAudioOfflineManifest;

typedef struct SuAudioOffline SuAudioOffline;

typedef struct SuAudioCheckpointInfo {
    uint32_t version;
    uint32_t asset_ref_count;
    uint64_t position;
    uint32_t trace_next;
    SuAudioCommitCursor cursor;
    uint8_t source_sha256[32];
    uint8_t graph_sha256[32];
} SuAudioCheckpointInfo;

typedef struct SuAudioOfflineMetrics {
    uint32_t sample_rate;
    uint64_t start_sample;
    uint64_t frames;
    uint64_t start_pts_ns;
    uint64_t end_pts_ns;
    double sample_peak;
    double true_peak;
    double sample_peak_dbfs;
    double true_peak_dbtp;
    double integrated_lufs;  /* -HUGE_VAL when every block is gated */
    uint64_t loudness_blocks;
    uint32_t peak_instances;
    uint32_t peak_logical_voices;
    uint64_t admitted;
    uint64_t refused;
    uint64_t late_commands;
    uint32_t trace_applied;
    uint32_t trace_failed;
    uint32_t replayed_frames_on_begin;
    double render_seconds;
    char source_sha256[65];
    char graph_sha256[65];
    char pcm_sha256[65];
} SuAudioOfflineMetrics;

/* Creates a session. With a checkpoint, the manifest/trace must hash to the
 * checkpoint's source digest and every pinned generation must still be
 * resident; state is then reconstructed by deterministic replay to the saved
 * position and verified against the saved PCM digest state. */
uint32_t su_audio_offline_begin(const SuAudioOfflineManifest *manifest, const SuAudioTraceCommand *trace,
    uint32_t trace_count, const void *checkpoint, size_t checkpoint_bytes, SuAudioOffline **out);
void su_audio_offline_end(SuAudioOffline *offline);
/* Renders interleaved stereo float frames. Chunk size never changes output. */
uint32_t su_audio_offline_render(SuAudioOffline *offline, float *out, uint32_t frames);
uint64_t su_audio_offline_position(const SuAudioOffline *offline);
const SuAudioEvents *su_audio_offline_events(const SuAudioOffline *offline);
uint32_t su_audio_checkpoint_encode(const SuAudioOffline *offline, void *out, size_t capacity, size_t *written);
uint32_t su_audio_checkpoint_decode(const void *data, size_t bytes, SuAudioCheckpointInfo *out);
uint32_t su_audio_offline_metrics(const SuAudioOffline *offline, SuAudioOfflineMetrics *out);
/* Writes a JSON report (NUL-terminated); written excludes the terminator. */
uint32_t su_audio_offline_report_json(const SuAudioOffline *offline, char *out, size_t capacity, size_t *written);

/* Borrowed scene owned by the session (valid until end). */
SuAudioScene *su_audio_offline_scene(const SuAudioOffline *offline);
/* Optional observer called once per submitted trace command, after the
 * runtime accepted or refused it. Commands replayed while restoring a
 * checkpoint are not reported. Pass NULL to detach. */
typedef void (*SuAudioOfflineObserver)(void *user, uint32_t trace_index, const SuAudioTraceCommand *command,
    uint32_t status, uint64_t render_clock, SuAudioInstance instance);
uint32_t su_audio_offline_observe(SuAudioOffline *offline, SuAudioOfflineObserver observer, void *user);

/* IEEE float32 stereo WAV encoding into caller memory (44-byte header). */
uint32_t su_audio_wav_encode_f32(const float *stereo, uint64_t frames, uint32_t sample_rate,
    void *out, size_t capacity, size_t *written);
/* Standalone measurement: BS.1770-4 integrated loudness and 4x true peak. */
uint32_t su_audio_measure_stereo(const float *stereo, uint64_t frames, uint32_t sample_rate,
    double *integrated_lufs, double *true_peak);

#ifdef __cplusplus
}
#endif
#endif
