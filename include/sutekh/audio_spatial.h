#ifndef SUTEKH_AUDIO_SPATIAL_H
#define SUTEKH_AUDIO_SPATIAL_H

#include "sutekh/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SU_AUDIO_SCENE_MAX_EMITTERS 65536u
#define SU_AUDIO_SCENE_MAX_CURVES 4096u
#define SU_AUDIO_CURVE_MAX_POINTS 64u

typedef struct SuAudioScene SuAudioScene;
typedef uint64_t SuAudioEmitter;
typedef uint32_t SuAudioCurveId;

typedef struct SuAudioSceneDesc {
    uint32_t struct_size;
    uint32_t max_emitters;
    uint32_t max_curves;
} SuAudioSceneDesc;

typedef struct SuAudioCurvePoint {
    float distance_m;
    float gain_db;
    float lowpass_hz;
    float highpass_hz;
    float spread_rad;
    float scale;
} SuAudioCurvePoint;

typedef struct SuAudioCurveDesc {
    uint32_t struct_size;
    const SuAudioCurvePoint *points;
    uint32_t point_count;
} SuAudioCurveDesc;

typedef struct SuAudioCurveSample {
    float gain_db;
    float lowpass_hz;
    float highpass_hz;
    float spread_rad;
    float scale;
} SuAudioCurveSample;

typedef struct SuAudioEmitterSnapshot {
    SuWorldPos pos;
    SuVec3f vel;
    SuAudioCurveId curve;
    float scale;
} SuAudioEmitterSnapshot;

typedef struct SuAudioCameraListener {
    SuWorldPos pos;
    SuQuatf rot;
    SuVec3f vel;
} SuAudioCameraListener;

typedef struct SuAudioSpatialSnapshot {
    SuAudioEmitterSnapshot emitter;
    SuAudioCameraListener listener;
    SuVec3f direction_listener;
    double distance_m;
    SuAudioCurveSample curve;
} SuAudioSpatialSnapshot;

typedef struct SuAudioVoiceDescV2 {
    uint32_t struct_size;
    SuAudioVoiceDesc legacy;
    SuAudioEmitter emitter;
    SuAudioCurveId curve;
} SuAudioVoiceDescV2;

uint32_t su_audio_scene_create(const SuAudioSceneDesc *desc, SuAudioScene **out);
void su_audio_scene_destroy(SuAudioScene *scene);
uint32_t su_audio_scene_curve_add(SuAudioScene *scene, const SuAudioCurveDesc *desc, SuAudioCurveId *out);
uint32_t su_audio_scene_curve_evaluate(const SuAudioScene *scene, SuAudioCurveId curve, double distance_m,
                                       SuAudioCurveSample *out);
uint32_t su_audio_emitter_create(SuAudioScene *scene, const SuAudioEmitterSnapshot *desc, SuAudioEmitter *out);
uint32_t su_audio_emitter_update(SuAudioScene *scene, SuAudioEmitter emitter, SuWorldPos pos, SuVec3f vel);
uint32_t su_audio_emitter_destroy(SuAudioScene *scene, SuAudioEmitter emitter);
uint32_t su_audio_emitter_curve(SuAudioScene *scene, SuAudioEmitter emitter, SuAudioCurveId curve);
uint32_t su_audio_emitter_scale(SuAudioScene *scene, SuAudioEmitter emitter, float scale);
int su_audio_emitter_active(const SuAudioScene *scene, SuAudioEmitter emitter);
uint32_t su_audio_emitter_get(const SuAudioScene *scene, SuAudioEmitter emitter, SuAudioEmitterSnapshot *out);
uint32_t su_audio_scene_listener_set(SuAudioScene *scene, const SuAudioCameraListener *listener);
uint32_t su_audio_scene_listener_get(const SuAudioScene *scene, SuAudioCameraListener *out);
uint32_t su_audio_scene_listener_apply(const SuAudioScene *scene, SuAudioMixer *mixer);
uint32_t su_audio_scene_snapshot(const SuAudioScene *scene, SuAudioEmitter emitter, SuAudioSpatialSnapshot *out);
uint32_t su_audio_scene_resolve(const SuAudioScene *scene, SuAudioEmitter emitter,
                                SuAudioVoiceDesc *in_out, float *lowpass_hz);
uint32_t su_audio_scene_resolve_v2(const SuAudioScene *scene, const SuAudioVoiceDescV2 *desc,
                                   SuAudioVoiceDesc *out, SuAudioCurveSample *effects);

#ifdef __cplusplus
}
#endif

#endif
