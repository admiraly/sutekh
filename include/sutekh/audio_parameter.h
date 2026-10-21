#ifndef SUTEKH_AUDIO_PARAMETER_H
#define SUTEKH_AUDIO_PARAMETER_H

#include "sutekh/audio_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A07: scoped parameters, switches and interactive music transitions.
 * Every object here is single-owner presentation state; calls are serialized by
 * the owner like SuAudioEvents. All storage is bounded at creation. */

typedef uint32_t SuAudioParamId;
typedef uint32_t SuAudioSwitchId;
typedef struct SuAudioParams SuAudioParams;
typedef struct SuAudioMusic SuAudioMusic;

#define SU_AUDIO_PARAM_PENDING 8u
#define SU_AUDIO_PARAM_MAX_DEFS 4096u
#define SU_AUDIO_PARAM_MAX_BINDING_POINTS 16u

enum {
    SU_AUDIO_SCOPE_GLOBAL = 0u,
    SU_AUDIO_SCOPE_EMITTER = 1u,
    SU_AUDIO_SCOPE_INSTANCE = 2u
};

enum { SU_AUDIO_PARAM_CLAMP = 0u, SU_AUDIO_PARAM_REJECT = 1u };

enum {
    SU_AUDIO_SWITCH_FIRST_ONLY = 0u, /* value latched when the instance starts */
    SU_AUDIO_SWITCH_CONTINUE = 1u,   /* follows changes with a hard cut */
    SU_AUDIO_SWITCH_FADE = 2u        /* follows changes with a linear crossfade */
};

enum {
    SU_AUDIO_CONTROL_GAIN_DB = 0u,
    SU_AUDIO_CONTROL_PITCH = 1u,
    SU_AUDIO_CONTROL_LOWPASS_HZ = 2u,
    SU_AUDIO_CONTROL_BUS_GAIN_DB = 3u
};

/* key: emitter handle for EMITTER scope, instance handle for INSTANCE scope,
 * ignored (must be zero) for GLOBAL scope. */
typedef struct SuAudioParamScope {
    uint32_t kind;
    uint64_t key;
} SuAudioParamScope;

/* Resolution target: instance value, else emitter value, else global value. */
typedef struct SuAudioParamTarget {
    SuAudioEmitter emitter;
    SuAudioInstance instance;
} SuAudioParamTarget;

typedef struct SuAudioParamDef {
    SuAudioParamId id;
    float min_value;
    float max_value;
    float default_value;
    uint32_t smoothing_frames;
    uint32_t range_policy;
} SuAudioParamDef;

typedef struct SuAudioSwitchDef {
    SuAudioSwitchId id;
    uint32_t default_value;
    uint32_t policy;
    uint32_t fade_frames;
} SuAudioSwitchDef;

typedef struct SuAudioBindingPoint {
    float value;
    float output;
} SuAudioBindingPoint;

/* Maps one parameter through a clamped piecewise-linear curve onto a control.
 * Gains add in dB, pitch ratios multiply, low-pass cutoffs take the minimum.
 * Sum groups combine several parameters additively: bindings with the same nonzero sum_group and control add their
 * outputs first (GAIN_DB bindings in a group output linear amplitude, PITCH a ratio term, LOWPASS_HZ hertz; all
 * >= 0), and the sum then joins the other bindings as one value (amplitude converted to dB, a ratio <= 0 or a
 * cutoff of 0 ignored). Example: gain = 0.08 + 0.40 throttle + 0.35 strafe as three bindings in one group.
 * BUS_GAIN_DB bindings cannot be grouped. */
typedef struct SuAudioParamBinding {
    SuAudioParamId param_id;
    uint32_t control;
    uint32_t bus_id;  /* BUS_GAIN_DB: the fader's bus. GAIN_DB/PITCH/LOWPASS_HZ in the integrated runtime: 0 = every
                       * voice, else only voices whose graph leaf bus_id equals it (su_audio_param_controls and
                       * SuAudioParamVoice have no voice bus and apply every binding) */
    const SuAudioBindingPoint *points;
    uint32_t point_count;
    uint32_t sum_group;  /* 0 = none; see above */
} SuAudioParamBinding;
#define SU_AUDIO_PARAM_SUM_GROUPS 16u /* distinct (group, control) sums per evaluation; more are rejected */

typedef struct SuAudioParamsDesc {
    uint32_t struct_size;
    uint32_t sample_rate;
    const SuAudioParamDef *params;
    uint32_t param_count;
    const SuAudioSwitchDef *switches;
    uint32_t switch_count;
    const SuAudioParamBinding *bindings;
    uint32_t binding_count;
    uint32_t max_scoped_values;
    uint64_t start_sample;
} SuAudioParamsDesc;

typedef struct SuAudioParamControls {
    float gain_db;
    float pitch;
    float lowpass_hz;
    float bus_gain_db;
    uint32_t bus_id;
} SuAudioParamControls;

typedef struct SuAudioSwitchMix {
    uint32_t old_value;
    uint32_t new_value;
    float old_gain;
    float new_gain;
} SuAudioSwitchMix;

typedef struct SuAudioParamStats {
    uint64_t sets;
    uint64_t clamped;
    uint64_t rejected;
    uint64_t late;
    uint32_t scoped_values;
    uint32_t scoped_capacity;
    uint64_t runtime_bytes;
} SuAudioParamStats;

uint32_t su_audio_params_create(const SuAudioParamsDesc *desc, SuAudioParams **out);
void su_audio_params_destroy(SuAudioParams *params);
/* Commands earlier than the advanced clock are late and refused. */
uint32_t su_audio_param_set_at(SuAudioParams *params, SuAudioParamScope scope, SuAudioParamId id,
                               float value, uint64_t sample_time);
uint32_t su_audio_switch_set_at(SuAudioParams *params, SuAudioParamScope scope, SuAudioSwitchId id,
                                uint32_t value, uint64_t sample_time);
uint32_t su_audio_param_get(const SuAudioParams *params, SuAudioParamTarget target, SuAudioParamId id,
                            uint64_t sample_time, float *out);
uint32_t su_audio_switch_get(const SuAudioParams *params, SuAudioParamTarget target, SuAudioSwitchId id,
                             uint64_t sample_time, uint32_t *out);
/* instance_start latches FIRST_ONLY switches; FADE reports complementary gains. */
uint32_t su_audio_switch_mix(const SuAudioParams *params, SuAudioParamTarget target, SuAudioSwitchId id,
                             uint64_t instance_start, uint64_t sample_time, SuAudioSwitchMix *out);
uint32_t su_audio_param_controls(const SuAudioParams *params, SuAudioParamTarget target,
                                 uint64_t sample_time, SuAudioParamControls *out);
/* Drops superseded ramps; later commands must be at or after this clock. */
uint32_t su_audio_params_advance(SuAudioParams *params, uint64_t sample_time);
/* Frees the scoped values of one emitter or instance (for example after retirement). */
uint32_t su_audio_param_scope_clear(SuAudioParams *params, SuAudioParamScope scope);
/* Fills caller arrays for su_audio_graph_expand_ex (all defined params and switches). */
uint32_t su_audio_param_expand_state(const SuAudioParams *params, SuAudioParamTarget target,
    uint64_t instance_start, uint64_t sample_time, SuAudioGraphParameter *parameters,
    size_t parameter_capacity, SuAudioGraphSwitch *switches, size_t switch_capacity,
    SuAudioGraphExpandState *out);
void su_audio_params_stats(const SuAudioParams *params, SuAudioParamStats *out);

/* Reference parameter-driven voice: renders one resident clip through the
 * controls of one target with a continuous fractional cursor. */
typedef struct SuAudioParamVoice {
    const SuAudioClip *clip;
    double cursor;
    float lowpass_state[2];
    uint64_t clock;
    SuAudioParamTarget target;
    uint32_t loop;
} SuAudioParamVoice;

uint32_t su_audio_param_voice_init(SuAudioParamVoice *voice, const SuAudioClip *clip,
                                   SuAudioParamTarget target, uint64_t start_sample, uint32_t loop);
uint32_t su_audio_param_voice_render(SuAudioParamVoice *voice, const SuAudioParams *params,
                                     float *out_stereo, uint32_t frames);

/* Interactive music. Tempo maps are integer: tempo points start at absolute
 * segment beats and give tempo in milli-BPM. Frame positions are computed from
 * each tempo point with one 128-bit floor division, so they never drift. */
enum {
    SU_AUDIO_MUSIC_IMMEDIATE = 0u,
    SU_AUDIO_MUSIC_BEAT = 1u,
    SU_AUDIO_MUSIC_BAR = 2u,
    SU_AUDIO_MUSIC_CUE = 3u,
    SU_AUDIO_MUSIC_SEGMENT_END = 4u
};

enum {
    SU_AUDIO_MUSIC_LATE_REJECT = 0u,      /* boundary already passed: refuse */
    SU_AUDIO_MUSIC_LATE_NEXT = 1u,        /* wait for the next occurrence (next loop) */
    SU_AUDIO_MUSIC_LATE_IMMEDIATE = 2u    /* start at the request time */
};

#define SU_AUDIO_MUSIC_MAX_STEMS 16u
#define SU_AUDIO_MUSIC_MAX_TEMPO 256u
#define SU_AUDIO_MUSIC_MAX_CUES 256u
#define SU_AUDIO_MUSIC_STINGERS 4u

typedef struct SuAudioTempoPoint {
    uint64_t beat;
    uint32_t bpm_milli;
} SuAudioTempoPoint;

typedef struct SuAudioMusicStreamSource {
    const char *path;
    const void *bytes;
    size_t byte_count;
    uint32_t ring_frames;
} SuAudioMusicStreamSource;

typedef struct SuAudioMusicSegmentDesc {
    uint32_t struct_size;
    uint32_t segment_id;
    const SuAudioAssetRef *stems;
    uint32_t stem_count;
    const SuAudioTempoPoint *tempo;
    uint32_t tempo_count;
    uint32_t beats_per_bar;
    const uint64_t *cue_frames;
    uint32_t cue_count;
    uint64_t length_frames;
    uint32_t loop;
    const SuAudioMusicStreamSource *stream_stems;
} SuAudioMusicSegmentDesc;

typedef struct SuMusicTransition {
    uint32_t struct_size;
    uint32_t segment_id;
    uint32_t sync;
    uint32_t cue;
    uint32_t fade_in_frames;
    uint32_t fade_out_frames;
    uint32_t late_policy;
    SuAudioAssetRef stinger;  /* id 0: none; plays at the boundary */
    float stinger_gain;
} SuMusicTransition;

typedef struct SuAudioMusicDesc {
    uint32_t struct_size;
    uint32_t sample_rate;
    uint32_t max_segments;
    uint64_t start_sample;
} SuAudioMusicDesc;

typedef struct SuAudioMusicState {
    uint64_t clock;
    uint32_t segment_id;
    uint64_t segment_start;
    uint64_t segment_frame;
    uint32_t pending_segment_id;
    uint64_t pending_at;
    uint32_t fading_segment_id;
    uint32_t stingers;
} SuAudioMusicState;

uint32_t su_audio_music_create(const SuAudioMusicDesc *desc, SuAudioAssets *assets, SuAudioMusic **out);
void su_audio_music_destroy(SuAudioMusic *music);
uint32_t su_audio_music_segment_add(SuAudioMusic *music, const SuAudioMusicSegmentDesc *desc);
/* Segment-local frame of an integer beat, from the tempo map alone. */
uint32_t su_audio_music_beat_frame(const SuAudioMusic *music, uint32_t segment_id, uint64_t beat, uint64_t *out);
/* Schedules a transition requested at sample_time and reports its boundary. A
 * new request replaces a pending one; boundaries never precede an active fade's end. */
uint32_t su_audio_music_transition(SuAudioMusic *music, const SuMusicTransition *transition,
                                   uint64_t sample_time, uint64_t *boundary);
uint32_t su_audio_music_render(SuAudioMusic *music, float *out_stereo, uint32_t frames);
void su_audio_music_state(const SuAudioMusic *music, SuAudioMusicState *out);

#ifdef __cplusplus
}
#endif
#endif
