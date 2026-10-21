#ifndef SUTEKH_AUDIO_CLUSTER_H
#define SUTEKH_AUDIO_CLUSTER_H

#include "sutekh/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A08: optional persistent directional clustering of rendered spatial objects.
 * Clustering changes the spatial representation only: every input object is
 * always rendered, nothing is dropped and voice admission is not involved.
 * Single-owner presentation state; storage is bounded at creation. */

typedef struct SuAudioCluster SuAudioCluster;

#define SU_AUDIO_CLUSTER_MAX_OBJECTS 65536u
#define SU_AUDIO_CLUSTER_MAX_SPEAKERS 16u

typedef struct SuAudioSpeakerLayout {
    uint32_t count;
    SuVec3f directions[SU_AUDIO_CLUSTER_MAX_SPEAKERS];
    uint32_t bed_left;
    uint32_t bed_right;
} SuAudioSpeakerLayout;

enum {
    SU_AUDIO_CLUSTER_BED = 1u,      /* passes through to the bed, never spatialized */
    SU_AUDIO_CLUSTER_EXCLUDE = 2u   /* always a dedicated output object */
};

enum {
    SU_AUDIO_CLUSTER_OUTPUT_CLUSTER = 1u,
    SU_AUDIO_CLUSTER_OUTPUT_DEDICATED = 2u
};

enum {
    SU_AUDIO_CLUSTER_ROLE_BED = 0u,
    SU_AUDIO_CLUSTER_ROLE_CLUSTERED = 1u,
    SU_AUDIO_CLUSTER_ROLE_EXCLUDED = 2u,   /* flag */
    SU_AUDIO_CLUSTER_ROLE_PRIORITY = 3u,   /* priority >= priority_threshold */
    SU_AUDIO_CLUSTER_ROLE_PROXIMITY = 4u   /* distance < proximity_m */
};

typedef struct SuAudioClusterDesc {
    uint32_t struct_size;
    float width_rad;          /* join angle, (0, pi] */
    float leave_ratio;        /* >= 1: leave angle = width_rad * leave_ratio */
    uint32_t hold_frames;     /* minimum membership before leaving */
    uint32_t crossfade_frames;
    float proximity_m;        /* closer objects stay dedicated; 0 disables */
    uint32_t priority_threshold; /* 0 disables */
    uint32_t output_limit;    /* clusters + dedicated objects, >= 1 */
    uint32_t max_objects;
} SuAudioClusterDesc;

/* direction is listener-relative (need not be normalized; zero is non-directional).
 * pcm is interleaved with `channels` (1 or 2) and read by render only. */
typedef struct SuAudioClusterObject {
    uint64_t id;
    SuVec3f direction;
    float distance_m;
    float rms;
    uint32_t priority;
    uint32_t flags;
    uint32_t metadata;
    uint32_t channels;
    const float *pcm;
} SuAudioClusterObject;

typedef struct SuAudioClusterOutput {
    uint32_t output_id;
    uint32_t kind;
    SuVec3f direction;        /* unit vector, or zero when directions cancel */
    float rms_weight;
    uint32_t members;
    uint32_t metadata;
    uint32_t draining;        /* no members; still fading out previous members */
    uint64_t object_id;       /* dedicated outputs */
} SuAudioClusterOutput;

typedef struct SuAudioClusterAssoc {
    uint64_t object_id;
    uint32_t role;
    uint32_t output_id;       /* 0 for beds */
    uint32_t previous_output_id; /* nonzero while crossfading */
    float gain;
    float previous_gain;
} SuAudioClusterAssoc;

typedef struct SuAudioClusterStats {
    uint64_t clock;
    uint32_t objects;
    uint32_t beds;
    uint32_t clusters;
    uint32_t draining;
    uint32_t dedicated;
    uint32_t outputs;          /* clusters + dedicated, excluding draining */
    uint32_t infeasible;       /* budget could not be met by compatible merges */
    uint32_t fading;
    uint64_t created;
    uint64_t joins;
    uint64_t moves;
    uint64_t merges;
    uint64_t runtime_bytes;
} SuAudioClusterStats;

uint32_t su_audio_cluster_create(const SuAudioClusterDesc *desc, SuAudioCluster **out);
void su_audio_cluster_destroy(SuAudioCluster *cluster);
/* Reassigns objects at the current clock. Inputs are processed in object-ID
 * order, so membership is independent of array order. Duplicate IDs, too many
 * objects or non-finite values are refused without changing state. */
uint32_t su_audio_cluster_update(SuAudioCluster *cluster, const SuAudioClusterObject *objects, uint32_t count);
/* CPU stereo reference for the objects passed to the last update (same array).
 * Advances crossfades and the clock by `frames`. */
uint32_t su_audio_cluster_render(SuAudioCluster *cluster, const SuAudioClusterObject *objects, uint32_t count,
                                 uint32_t frames, float *out_stereo);
uint32_t su_audio_speaker_layout_validate(const SuAudioSpeakerLayout *layout);
uint32_t su_audio_cluster_render_speakers(SuAudioCluster *cluster, const SuAudioClusterObject *objects,
                                         uint32_t count, uint32_t frames, const SuAudioSpeakerLayout *layout,
                                         float *out);
uint32_t su_audio_cluster_outputs(const SuAudioCluster *cluster, SuAudioClusterOutput *out, uint32_t capacity,
                                  uint32_t *count);
uint32_t su_audio_cluster_assoc(const SuAudioCluster *cluster, SuAudioClusterAssoc *out, uint32_t capacity,
                                uint32_t *count);
void su_audio_cluster_stats(const SuAudioCluster *cluster, SuAudioClusterStats *out);

#ifdef __cplusplus
}
#endif
#endif
