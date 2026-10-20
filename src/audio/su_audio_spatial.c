#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio_spatial.h"
#include "su_audio_cluster_int.h"
#include "su_audio_state.h"

typedef struct EmitterSlot {
    SuAudioEmitterSnapshot value;
    uint32_t generation;
    int active;
} EmitterSlot;

typedef struct CurveTable {
    uint32_t count;
    SuAudioCurvePoint points[SU_AUDIO_CURVE_MAX_POINTS];
} CurveTable;

struct SuAudioScene {
    uint32_t emitter_capacity;
    uint32_t curve_capacity;
    uint32_t curve_count;
    EmitterSlot *emitters;
    CurveTable *curves;
    SuAudioCameraListener listener;
};

static int finite_vec(SuVec3f value) {
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z) &&
        fabsf(value.x) <= 1000000.0f && fabsf(value.y) <= 1000000.0f && fabsf(value.z) <= 1000000.0f;
}

static int valid_scale(float scale) {
    return isfinite(scale) && scale > 0.0f && scale <= 1000000.0f;
}

static int valid_curve(const SuAudioScene *scene, SuAudioCurveId curve) {
    return curve <= scene->curve_count;
}

static EmitterSlot *emitter_slot(const SuAudioScene *scene, SuAudioEmitter emitter) {
    uint32_t index = (uint32_t)emitter;
    uint32_t generation = (uint32_t)(emitter >> 32);
    EmitterSlot *slot;
    if (scene == NULL || index == 0u || index > scene->emitter_capacity || generation == 0u) {
        return NULL;
    }
    slot = &scene->emitters[index - 1u];
    return slot->active && slot->generation == generation ? slot : NULL;
}

uint32_t su_audio_scene_create(const SuAudioSceneDesc *desc, SuAudioScene **out) {
    SuAudioScene *scene;
    uint32_t index;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (desc == NULL || desc->struct_size < sizeof *desc || desc->max_emitters == 0u ||
        desc->max_emitters > SU_AUDIO_SCENE_MAX_EMITTERS || desc->max_curves > SU_AUDIO_SCENE_MAX_CURVES) {
        return SU_E_INVALID_ARGUMENT;
    }
    scene = calloc(1, sizeof *scene);
    if (scene == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    scene->emitter_capacity = desc->max_emitters;
    scene->curve_capacity = desc->max_curves;
    scene->emitters = calloc(scene->emitter_capacity, sizeof *scene->emitters);
    if (scene->curve_capacity != 0u) {
        scene->curves = calloc(scene->curve_capacity, sizeof *scene->curves);
    }
    if (scene->emitters == NULL || (scene->curve_capacity != 0u && scene->curves == NULL)) {
        su_audio_scene_destroy(scene);
        return SU_E_RESOURCE_LIMIT;
    }
    for (index = 0; index < scene->emitter_capacity; index++) {
        scene->emitters[index].generation = 1u;
    }
    scene->listener.rot.w = 1.0f;
    *out = scene;
    return SU_OK;
}

void su_audio_scene_destroy(SuAudioScene *scene) {
    if (scene != NULL) {
        free(scene->emitters);
        free(scene->curves);
        free(scene);
    }
}

static int valid_point(const SuAudioCurvePoint *point) {
    return isfinite(point->distance_m) && point->distance_m >= 0.0f &&
        isfinite(point->gain_db) && point->gain_db >= -160.0f && point->gain_db <= 60.0f &&
        isfinite(point->lowpass_hz) && point->lowpass_hz >= 0.0f && point->lowpass_hz <= 384000.0f &&
        isfinite(point->highpass_hz) && point->highpass_hz >= 0.0f && point->highpass_hz <= 384000.0f &&
        isfinite(point->spread_rad) && point->spread_rad >= 0.0f && point->spread_rad <= 3.141592653589793f &&
        isfinite(point->scale) && point->scale >= 0.0f && point->scale <= 64.0f;
}

uint32_t su_audio_scene_curve_add(SuAudioScene *scene, const SuAudioCurveDesc *desc, SuAudioCurveId *out) {
    uint32_t index;
    CurveTable *table;
    if (scene == NULL || desc == NULL || out == NULL || desc->struct_size < sizeof *desc ||
        desc->points == NULL || desc->point_count == 0u || desc->point_count > SU_AUDIO_CURVE_MAX_POINTS) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (index = 0; index < desc->point_count; index++) {
        if (!valid_point(&desc->points[index]) ||
            (index > 0u && desc->points[index].distance_m <= desc->points[index - 1u].distance_m)) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    if (scene->curve_count == scene->curve_capacity) {
        return SU_E_RESOURCE_LIMIT;
    }
    table = &scene->curves[scene->curve_count];
    memcpy(table->points, desc->points, desc->point_count * sizeof *desc->points);
    table->count = desc->point_count;
    *out = ++scene->curve_count;
    return SU_OK;
}

static float interpolate(float first, float last, double amount) {
    return (float)((double)first + ((double)last - first) * amount);
}

uint32_t su_audio_scene_curve_evaluate(const SuAudioScene *scene, SuAudioCurveId curve, double distance_m,
                                       SuAudioCurveSample *out) {
    SuAudioCurveSample sample = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    const CurveTable *table;
    const SuAudioCurvePoint *first, *last;
    uint32_t index;
    double amount = 0.0;
    if (scene == NULL || out == NULL || !valid_curve(scene, curve) || !isfinite(distance_m) || distance_m < 0.0) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (curve == 0u) {
        *out = sample;
        return SU_OK;
    }
    table = &scene->curves[curve - 1u];
    first = last = &table->points[0];
    for (index = 1u; index < table->count && distance_m > last->distance_m; index++) {
        first = last;
        last = &table->points[index];
    }
    if (distance_m >= last->distance_m) {
        first = last;
    } else if (first != last) {
        amount = (distance_m - first->distance_m) / ((double)last->distance_m - first->distance_m);
    }
    sample.gain_db = interpolate(first->gain_db, last->gain_db, amount);
    sample.lowpass_hz = interpolate(first->lowpass_hz, last->lowpass_hz, amount);
    sample.highpass_hz = interpolate(first->highpass_hz, last->highpass_hz, amount);
    sample.spread_rad = interpolate(first->spread_rad, last->spread_rad, amount);
    sample.scale = interpolate(first->scale, last->scale, amount);
    *out = sample;
    return SU_OK;
}

uint32_t su_audio_emitter_create(SuAudioScene *scene, const SuAudioEmitterSnapshot *desc, SuAudioEmitter *out) {
    uint32_t index;
    if (scene == NULL || desc == NULL || out == NULL || !finite_vec(desc->vel) ||
        !valid_scale(desc->scale) || !valid_curve(scene, desc->curve)) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (index = 0u; index < scene->emitter_capacity; index++) {
        EmitterSlot *slot = &scene->emitters[index];
        if (!slot->active && slot->generation != 0u) {
            slot->value = *desc;
            slot->active = 1;
            *out = ((uint64_t)slot->generation << 32) | (index + 1u);
            return SU_OK;
        }
    }
    return SU_E_RESOURCE_LIMIT;
}

uint32_t su_audio_emitter_update(SuAudioScene *scene, SuAudioEmitter emitter, SuWorldPos pos, SuVec3f vel) {
    EmitterSlot *slot = emitter_slot(scene, emitter);
    if (scene == NULL || !finite_vec(vel)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (slot == NULL) {
        return SU_E_STALE_HANDLE;
    }
    slot->value.pos = pos;
    slot->value.vel = vel;
    return SU_OK;
}

uint32_t su_audio_emitter_destroy(SuAudioScene *scene, SuAudioEmitter emitter) {
    EmitterSlot *slot = emitter_slot(scene, emitter);
    if (scene == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (slot == NULL) {
        return SU_E_STALE_HANDLE;
    }
    slot->active = 0;
    slot->generation++;
    return SU_OK;
}

uint32_t su_audio_emitter_curve(SuAudioScene *scene, SuAudioEmitter emitter, SuAudioCurveId curve) {
    EmitterSlot *slot = emitter_slot(scene, emitter);
    if (scene == NULL || !valid_curve(scene, curve)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (slot == NULL) {
        return SU_E_STALE_HANDLE;
    }
    slot->value.curve = curve;
    return SU_OK;
}

uint32_t su_audio_emitter_scale(SuAudioScene *scene, SuAudioEmitter emitter, float scale) {
    EmitterSlot *slot = emitter_slot(scene, emitter);
    if (scene == NULL || !valid_scale(scale)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (slot == NULL) {
        return SU_E_STALE_HANDLE;
    }
    slot->value.scale = scale;
    return SU_OK;
}

int su_audio_emitter_active(const SuAudioScene *scene, SuAudioEmitter emitter) {
    return emitter_slot(scene, emitter) != NULL;
}

uint32_t su_audio_emitter_get(const SuAudioScene *scene, SuAudioEmitter emitter, SuAudioEmitterSnapshot *out) {
    EmitterSlot *slot = emitter_slot(scene, emitter);
    if (scene == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (slot == NULL) {
        return SU_E_STALE_HANDLE;
    }
    *out = slot->value;
    return SU_OK;
}

uint32_t su_audio_scene_listener_set(SuAudioScene *scene, const SuAudioCameraListener *listener) {
    double length;
    SuAudioCameraListener normalized;
    if (scene == NULL || listener == NULL || !finite_vec(listener->vel) ||
        !isfinite(listener->rot.x) || !isfinite(listener->rot.y) ||
        !isfinite(listener->rot.z) || !isfinite(listener->rot.w)) {
        return SU_E_INVALID_ARGUMENT;
    }
    length = hypot(hypot((double)listener->rot.x, listener->rot.y), hypot((double)listener->rot.z, listener->rot.w));
    if (length == 0.0) {
        return SU_E_INVALID_ARGUMENT;
    }
    normalized = *listener;
    normalized.rot.x = (float)(listener->rot.x / length);
    normalized.rot.y = (float)(listener->rot.y / length);
    normalized.rot.z = (float)(listener->rot.z / length);
    normalized.rot.w = (float)(listener->rot.w / length);
    scene->listener = normalized;
    return SU_OK;
}

uint32_t su_audio_scene_listener_get(const SuAudioScene *scene, SuAudioCameraListener *out) {
    if (scene == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = scene->listener;
    return SU_OK;
}

uint32_t su_audio_scene_listener_apply(const SuAudioScene *scene, SuAudioMixer *mixer) {
    if (scene == NULL || mixer == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    return su_audio_listener(mixer, scene->listener.pos, scene->listener.rot, scene->listener.vel);
}

static int representable_delta(int64_t first, int64_t last) {
    return first >= last ? (uint64_t)first - (uint64_t)last <= INT64_MAX :
        (uint64_t)last - (uint64_t)first <= INT64_MAX;
}

static uint32_t spatial_snapshot(const SuAudioScene *scene, SuAudioEmitter emitter,
                                  SuAudioCurveId override, SuAudioSpatialSnapshot *out) {
    SuAudioSpatialSnapshot snapshot;
    SuVec3f delta;
    SuQuatf inverse;
    double distance;
    float radius_scale;
    uint32_t status = su_audio_emitter_get(scene, emitter, &snapshot.emitter);
    if (status != SU_OK) {
        return status;
    }
    snapshot.listener = scene->listener;
    if (!representable_delta(snapshot.emitter.pos.x, snapshot.listener.pos.x) ||
        !representable_delta(snapshot.emitter.pos.y, snapshot.listener.pos.y) ||
        !representable_delta(snapshot.emitter.pos.z, snapshot.listener.pos.z)) {
        return SU_E_INVALID_ARGUMENT;
    }
    delta = su_wpos_delta(snapshot.emitter.pos, snapshot.listener.pos);
    snapshot.distance_m = hypot(hypot((double)delta.x, delta.y), delta.z);
    inverse = snapshot.listener.rot;
    inverse.x = -inverse.x;
    inverse.y = -inverse.y;
    inverse.z = -inverse.z;
    snapshot.direction_listener = su_quatf_rotate(inverse, delta);
    if (override != 0u) {
        snapshot.emitter.curve = override;
    }
    distance = snapshot.distance_m / snapshot.emitter.scale;
    status = su_audio_scene_curve_evaluate(scene, snapshot.emitter.curve, distance, &snapshot.curve);
    if (status == SU_OK && snapshot.emitter.curve != 0u) {
        radius_scale = snapshot.curve.scale;
        distance = radius_scale > 0.0f ? distance / radius_scale : (distance > 0.0 ? DBL_MAX : 0.0);
        status = su_audio_scene_curve_evaluate(scene, snapshot.emitter.curve, distance, &snapshot.curve);
        snapshot.curve.scale = radius_scale;
    }
    if (status == SU_OK) {
        *out = snapshot;
    }
    return status;
}

uint32_t su_audio_scene_snapshot(const SuAudioScene *scene, SuAudioEmitter emitter, SuAudioSpatialSnapshot *out) {
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    return spatial_snapshot(scene, emitter, 0u, out);
}

static uint32_t resolve(const SuAudioScene *scene, SuAudioEmitter emitter, SuAudioCurveId override,
                          SuAudioVoiceDesc *in_out, SuAudioCurveSample *effects) {
    SuAudioSpatialSnapshot snapshot;
    SuAudioVoiceDesc resolved;
    double gain;
    uint32_t status;
    if (in_out == NULL || !isfinite(in_out->gain) || in_out->gain < 0.0f ||
        !isfinite(in_out->pitch) || in_out->pitch < 0.0f || in_out->pitch > 64.0f ||
        !isfinite(in_out->min_distance) || in_out->min_distance < 0.0f ||
        !isfinite(in_out->max_distance) || in_out->max_distance < 0.0f ||
        !isfinite(in_out->rolloff) || in_out->rolloff < 0.0f ||
        !isfinite(in_out->doppler) || in_out->doppler < 0.0f || in_out->doppler > 1.0f) {
        return SU_E_INVALID_ARGUMENT;
    }
    status = spatial_snapshot(scene, emitter, override, &snapshot);
    if (status != SU_OK) {
        return status;
    }
    resolved = *in_out;
    resolved.flags |= SU_AUDIO_SPATIAL;
    resolved.pos = snapshot.emitter.pos;
    resolved.vel = snapshot.emitter.vel;
    if (snapshot.emitter.curve != 0u) {
        gain = (double)resolved.gain * pow(10.0, snapshot.curve.gain_db / 20.0);
        resolved.gain = (float)fmin(gain, FLT_MAX);
        resolved.min_distance = FLT_MAX;
        resolved.max_distance = 0.0f;
        resolved.rolloff = 1.0f;
    } else if (snapshot.emitter.scale != 1.0f) {
        double minimum = resolved.min_distance > 0.0f ? resolved.min_distance : 1.0;
        resolved.min_distance = (float)fmin(minimum * snapshot.emitter.scale, FLT_MAX);
        if (resolved.min_distance == 0.0f) {
            resolved.min_distance = FLT_MIN;
        }
        resolved.max_distance = (float)fmin((double)resolved.max_distance * snapshot.emitter.scale, FLT_MAX);
        if (in_out->max_distance > 0.0f && resolved.max_distance == 0.0f) {
            resolved.max_distance = FLT_MIN;
        }
    }
    *in_out = resolved;
    if (effects != NULL) {
        *effects = snapshot.curve;
    }
    return SU_OK;
}

uint32_t su_audio_scene_resolve(const SuAudioScene *scene, SuAudioEmitter emitter,
                                SuAudioVoiceDesc *in_out, float *lowpass_hz) {
    SuAudioCurveSample effects;
    uint32_t status = resolve(scene, emitter, 0u, in_out, &effects);
    if (status == SU_OK && lowpass_hz != NULL) {
        *lowpass_hz = effects.lowpass_hz;
    }
    return status;
}

uint32_t su_audio_scene_resolve_v2(const SuAudioScene *scene, const SuAudioVoiceDescV2 *desc,
                                   SuAudioVoiceDesc *out, SuAudioCurveSample *effects) {
    SuAudioVoiceDesc resolved;
    uint32_t status;
    if (desc == NULL || out == NULL || desc->struct_size < sizeof *desc) {
        return SU_E_INVALID_ARGUMENT;
    }
    resolved = desc->legacy;
    status = resolve(scene, desc->emitter, desc->curve, &resolved, effects);
    if (status == SU_OK) {
        *out = resolved;
    }
    return status;
}

void su_audio_scene_capacity(const SuAudioScene *scene, uint32_t *emitters, uint32_t *curves, uint32_t *curve_count) {
    *emitters = scene != NULL ? scene->emitter_capacity : 0u;
    *curves = scene != NULL ? scene->curve_capacity : 0u;
    *curve_count = scene != NULL ? scene->curve_count : 0u;
}

uint32_t su_audio_scene_state(SuAudioScene *scene, SuAudioStateIo *io) {
    uint32_t index;
    if (scene == NULL || io == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (index = 0; index < scene->emitter_capacity; index++) {
        EmitterSlot *slot = &scene->emitters[index];
        SU_STATE(io, slot->generation);
        SU_STATE(io, slot->active);
        if (slot->active) {
            SU_STATE(io, slot->value.pos);
            SU_STATE(io, slot->value.vel);
            SU_STATE(io, slot->value.curve);
            SU_STATE(io, slot->value.scale);
        }
    }
    SU_STATE(io, scene->listener.pos);
    SU_STATE(io, scene->listener.rot);
    SU_STATE(io, scene->listener.vel);
    return io->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}
