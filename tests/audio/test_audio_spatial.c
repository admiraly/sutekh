#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "sutekh/audio_spatial.h"

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    } \
} while (0)

static int near(double actual, double expected) {
    return fabs(actual - expected) < 0.00001;
}

static SuAudioScene *make_scene(uint32_t emitters, uint32_t curves) {
    SuAudioSceneDesc desc = {sizeof desc, emitters, curves};
    SuAudioScene *scene = NULL;
    CHECK(su_audio_scene_create(&desc, &scene) == SU_OK);
    return scene;
}

static SuAudioEmitter make_emitter(SuAudioScene *scene, double distance) {
    SuAudioEmitterSnapshot desc = {0};
    SuAudioEmitter emitter = 0;
    desc.pos = su_wpos(distance, 0.0, 0.0);
    desc.scale = 1.0f;
    CHECK(su_audio_emitter_create(scene, &desc, &emitter) == SU_OK);
    return emitter;
}

static SuAudioCurveId add_curve(SuAudioScene *scene, SuAudioCurvePoint *points, uint32_t count) {
    SuAudioCurveDesc desc = {sizeof desc, points, count};
    SuAudioCurveId curve = 0;
    CHECK(su_audio_scene_curve_add(scene, &desc, &curve) == SU_OK);
    return curve;
}

static void tables(void) {
    SuAudioScene *scene = make_scene(1u, 3u);
    SuAudioCurvePoint points[] = {{10, 0, 20000, 0, 0, 1}, {30, -20, 2000, 400, 2, 0.5f},
                                  {50, -40, 1000, 800, 3, 0}};
    SuAudioCurveDesc desc = {sizeof desc, points, 3u};
    SuAudioCurveSample sample;
    SuAudioCurveId curve = add_curve(scene, points, 3u);
    SuAudioCurveId other = 0;
    unsigned index;
    CHECK(curve == 1u);
    CHECK(su_audio_scene_curve_evaluate(scene, curve, 0, &sample) == SU_OK);
    CHECK(sample.gain_db == 0 && sample.lowpass_hz == 20000 && sample.highpass_hz == 0 &&
          sample.spread_rad == 0 && sample.scale == 1);
    CHECK(su_audio_scene_curve_evaluate(scene, curve, 20, &sample) == SU_OK);
    CHECK(sample.gain_db == -10 && sample.lowpass_hz == 11000 && sample.highpass_hz == 200 &&
          sample.spread_rad == 1 && sample.scale == 0.75f);
    CHECK(su_audio_scene_curve_evaluate(scene, curve, 30, &sample) == SU_OK);
    CHECK(sample.gain_db == -20 && sample.lowpass_hz == 2000 && sample.highpass_hz == 400 &&
          sample.spread_rad == 2 && sample.scale == 0.5f);
    CHECK(su_audio_scene_curve_evaluate(scene, curve, DBL_MAX, &sample) == SU_OK);
    CHECK(sample.gain_db == -40 && sample.lowpass_hz == 1000 && sample.highpass_hz == 800 &&
          sample.spread_rad == 3 && sample.scale == 0);
    points[0].gain_db = -80;
    CHECK(su_audio_scene_curve_evaluate(scene, curve, 10, &sample) == SU_OK && sample.gain_db == 0);
    CHECK(su_audio_scene_curve_evaluate(scene, 0, 50, &sample) == SU_OK);
    CHECK(sample.gain_db == 0 && sample.lowpass_hz == 0 && sample.highpass_hz == 0 &&
          sample.spread_rad == 0 && sample.scale == 1);
    CHECK(su_audio_scene_curve_evaluate(scene, curve, NAN, &sample) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_scene_curve_evaluate(scene, curve, INFINITY, &sample) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_scene_curve_evaluate(scene, curve, -1, &sample) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_scene_curve_evaluate(scene, 2, 10, &sample) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_scene_curve_evaluate(scene, curve, 10, NULL) == SU_E_INVALID_ARGUMENT);
    for (index = 0; index < 6u; index++) {
        SuAudioCurvePoint bad = {0, 0, 0, 0, 0, 1};
        float *fields[] = {&bad.distance_m, &bad.gain_db, &bad.lowpass_hz, &bad.highpass_hz,
                          &bad.spread_rad, &bad.scale};
        SuAudioCurveDesc invalid = {sizeof invalid, &bad, 1u};
        *fields[index] = NAN;
        CHECK(su_audio_scene_curve_add(scene, &invalid, &other) == SU_E_INVALID_ARGUMENT);
        *fields[index] = INFINITY;
        CHECK(su_audio_scene_curve_add(scene, &invalid, &other) == SU_E_INVALID_ARGUMENT);
        *fields[index] = -1000;
        CHECK(su_audio_scene_curve_add(scene, &invalid, &other) == SU_E_INVALID_ARGUMENT);
    }
    points[1].distance_m = points[0].distance_m;
    CHECK(su_audio_scene_curve_add(scene, &desc, &other) == SU_E_INVALID_ARGUMENT);
    points[1].distance_m = 9;
    CHECK(su_audio_scene_curve_add(scene, &desc, &other) == SU_E_INVALID_ARGUMENT);
    desc.point_count = 0;
    CHECK(su_audio_scene_curve_add(scene, &desc, &other) == SU_E_INVALID_ARGUMENT);
    desc.point_count = SU_AUDIO_CURVE_MAX_POINTS + 1u;
    CHECK(su_audio_scene_curve_add(scene, &desc, &other) == SU_E_INVALID_ARGUMENT);
    desc.point_count = 1;
    desc.struct_size = 0;
    CHECK(su_audio_scene_curve_add(scene, &desc, &other) == SU_E_INVALID_ARGUMENT);
    desc.struct_size = sizeof desc;
    CHECK(su_audio_scene_curve_add(scene, &desc, &other) == SU_OK && other == 2);
    CHECK(su_audio_scene_curve_evaluate(scene, other, 100, &sample) == SU_OK && sample.gain_db == -80);
    {
        SuAudioCurvePoint maximum[SU_AUDIO_CURVE_MAX_POINTS];
        for (index = 0; index < SU_AUDIO_CURVE_MAX_POINTS; index++) {
            maximum[index] = (SuAudioCurvePoint){(float)index, -(float)index, 0, 0, 0, 1};
        }
        CHECK(add_curve(scene, maximum, SU_AUDIO_CURVE_MAX_POINTS) == 3u);
        CHECK(su_audio_scene_curve_evaluate(scene, 3, 62.5, &sample) == SU_OK && sample.gain_db == -62.5f);
    }
    CHECK(su_audio_scene_curve_add(scene, &desc, &other) == SU_E_RESOURCE_LIMIT);
    CHECK(su_audio_scene_curve_evaluate(scene, curve, 10, &sample) == SU_OK && sample.gain_db == 0);
    su_audio_scene_destroy(scene);
}

static void handles(void) {
    SuAudioScene *scene = make_scene(1u, 0u);
    SuAudioEmitter first = make_emitter(scene, 5);
    SuAudioEmitter next = 0;
    SuAudioEmitterSnapshot snapshot = {0};
    SuAudioSpatialSnapshot spatial;
    SuAudioVoiceDesc voice = {0};
    SuAudioVoiceDesc saved;
    SuAudioSceneDesc bad = {sizeof bad, 0, 0};
    SuAudioScene *invalid = scene;
    SuVec3f zero = {0};
    float lowpass = -1;
    snapshot.scale = 1;
    CHECK(su_audio_scene_create(&bad, &invalid) == SU_E_INVALID_ARGUMENT && invalid == NULL);
    bad.max_emitters = SU_AUDIO_SCENE_MAX_EMITTERS + 1u;
    CHECK(su_audio_scene_create(&bad, &invalid) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_emitter_create(scene, &snapshot, &next) == SU_E_RESOURCE_LIMIT);
    CHECK(su_audio_emitter_active(scene, first));
    CHECK(!su_audio_emitter_active(NULL, first));
    CHECK(!su_audio_emitter_active(scene, 0));
    CHECK(!su_audio_emitter_active(scene, UINT64_MAX));
    CHECK(su_audio_emitter_get(scene, first, &snapshot) == SU_OK && snapshot.pos.x == su_wpos(5, 0, 0).x);
    CHECK(su_audio_emitter_get(scene, first, NULL) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_emitter_scale(scene, first, 0) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_emitter_scale(scene, first, NAN) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_emitter_scale(scene, first, INFINITY) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_emitter_scale(scene, first, FLT_MAX) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_emitter_curve(scene, first, 1) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_emitter_update(scene, first, snapshot.pos, (SuVec3f){NAN, 0, 0}) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_emitter_update(scene, first, snapshot.pos, (SuVec3f){FLT_MAX, 0, 0}) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_emitter_destroy(scene, first) == SU_OK);
    CHECK(!su_audio_emitter_active(scene, first));
    CHECK(su_audio_emitter_destroy(scene, first) == SU_E_STALE_HANDLE);
    CHECK(su_audio_emitter_update(scene, first, snapshot.pos, zero) == SU_E_STALE_HANDLE);
    CHECK(su_audio_emitter_scale(scene, first, 1) == SU_E_STALE_HANDLE);
    CHECK(su_audio_emitter_curve(scene, first, 0) == SU_E_STALE_HANDLE);
    CHECK(su_audio_emitter_get(scene, first, &snapshot) == SU_E_STALE_HANDLE);
    CHECK(su_audio_scene_snapshot(scene, first, &spatial) == SU_E_STALE_HANDLE);
    voice.gain = 1;
    saved = voice;
    CHECK(su_audio_scene_resolve(scene, first, &voice, &lowpass) == SU_E_STALE_HANDLE);
    CHECK(memcmp(&voice, &saved, sizeof voice) == 0 && lowpass == -1);
    next = make_emitter(scene, -5);
    CHECK(next != first && (uint32_t)next == (uint32_t)first);
    CHECK(!su_audio_emitter_active(scene, first) && su_audio_emitter_active(scene, next));
    CHECK(su_audio_emitter_create(NULL, &snapshot, &first) == SU_E_INVALID_ARGUMENT);
    su_audio_scene_destroy(scene);
    su_audio_scene_destroy(NULL);
}

static void resolution(void) {
    SuAudioScene *scene = make_scene(2u, 2u);
    SuAudioEmitter emitter = make_emitter(scene, 20);
    SuAudioCurvePoint points[] = {{0, 0, 20000, 0, 0, 1}, {20, -20, 2000, 400, 2, 0.5f}};
    SuAudioCurveId curve = add_curve(scene, points, 2);
    SuAudioCameraListener listener = {0};
    SuAudioCameraListener readback;
    SuAudioSpatialSnapshot snapshot;
    SuAudioVoiceDesc base = {0};
    SuAudioVoiceDesc voice;
    SuAudioVoiceDescV2 extended = {0};
    SuAudioCurveSample effects;
    float lowpass = -1;
    SuVec3f zero = {0};
    base.gain = 0.25f;
    base.min_distance = 2;
    base.max_distance = 100;
    base.rolloff = 2;
    base.priority = 17;
    CHECK(su_audio_scene_listener_get(scene, &listener) == SU_OK && listener.rot.w == 1);
    voice = base;
    CHECK(su_audio_scene_resolve(scene, emitter, &voice, &lowpass) == SU_OK);
    CHECK(voice.gain == base.gain && voice.min_distance == 2 && voice.max_distance == 100 &&
          voice.rolloff == 2 && lowpass == 0 && voice.priority == 17);
    CHECK(su_audio_emitter_scale(scene, emitter, 2) == SU_OK);
    voice = base;
    CHECK(su_audio_scene_resolve(scene, emitter, &voice, NULL) == SU_OK);
    CHECK(voice.min_distance == 4 && voice.max_distance == 200);
    CHECK(su_audio_emitter_curve(scene, emitter, curve) == SU_OK);
    voice = base;
    CHECK(su_audio_scene_resolve(scene, emitter, &voice, &lowpass) == SU_OK);
    CHECK(near(voice.gain, 0.25 * pow(10, -(40.0 / 3.0) / 20)));
    CHECK(lowpass == 8000 && voice.min_distance == FLT_MAX && voice.max_distance == 0);
    CHECK(su_audio_attenuation(20, voice.min_distance, voice.max_distance, voice.rolloff) == 1);
    CHECK(su_audio_scene_snapshot(scene, emitter, &snapshot) == SU_OK);
    CHECK(snapshot.distance_m == 20 && near(snapshot.curve.gain_db, -40.0 / 3.0) &&
          near(snapshot.curve.highpass_hz, (float)(800.0 / 3.0)) && near(snapshot.curve.spread_rad, 4.0 / 3.0) &&
          snapshot.curve.scale == 0.75f && snapshot.emitter.scale == 2);
    extended.struct_size = sizeof extended;
    extended.legacy = base;
    extended.emitter = emitter;
    CHECK(su_audio_scene_resolve_v2(scene, &extended, &voice, &effects) == SU_OK);
    CHECK(near(voice.gain, 0.25 * pow(10, -(40.0 / 3.0) / 20)));
    points[0] = (SuAudioCurvePoint){0, -6, 4000, 100, 0.5f, 1};
    extended.curve = add_curve(scene, points, 1);
    CHECK(su_audio_scene_resolve_v2(scene, &extended, &voice, &effects) == SU_OK);
    CHECK(near(voice.gain, 0.25 * pow(10, -6.0 / 20)) && effects.lowpass_hz == 4000);
    extended.struct_size = sizeof extended - 1u;
    CHECK(su_audio_scene_resolve_v2(scene, &extended, &voice, &effects) == SU_E_INVALID_ARGUMENT);
    extended.struct_size = sizeof extended + 16u;
    extended.curve = UINT32_MAX;
    CHECK(su_audio_scene_resolve_v2(scene, &extended, &voice, &effects) == SU_E_INVALID_ARGUMENT);
    listener.rot = (SuQuatf){0, 2, 0, 0};
    CHECK(su_audio_scene_listener_set(scene, &listener) == SU_OK);
    CHECK(su_audio_scene_listener_get(scene, &readback) == SU_OK && readback.rot.y == 1);
    CHECK(su_audio_scene_snapshot(scene, emitter, &snapshot) == SU_OK && near(snapshot.direction_listener.x, -20));
    listener.rot = (SuQuatf){FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX};
    CHECK(su_audio_scene_listener_set(scene, &listener) == SU_OK);
    CHECK(su_audio_scene_listener_get(scene, &readback) == SU_OK && near(readback.rot.w, 0.5));
    listener.rot = (SuQuatf){0, 0, 0, 0};
    CHECK(su_audio_scene_listener_set(scene, &listener) == SU_E_INVALID_ARGUMENT);
    listener.rot.w = NAN;
    CHECK(su_audio_scene_listener_set(scene, &listener) == SU_E_INVALID_ARGUMENT);
    listener.rot = (SuQuatf){0, 0, 0, 1};
    listener.vel.x = INFINITY;
    CHECK(su_audio_scene_listener_set(scene, &listener) == SU_E_INVALID_ARGUMENT);
    listener.vel.x = FLT_MAX;
    CHECK(su_audio_scene_listener_set(scene, &listener) == SU_E_INVALID_ARGUMENT);
    listener.vel = zero;
    listener.pos = su_wpos(10000000, 0, 0);
    CHECK(su_audio_scene_listener_set(scene, &listener) == SU_OK);
    CHECK(su_audio_emitter_update(scene, emitter, su_wpos(10000000.001, 0, 0), zero) == SU_OK);
    CHECK(su_audio_scene_snapshot(scene, emitter, &snapshot) == SU_OK && near(snapshot.distance_m, 0.001));
    listener.pos = su_wpos(-10000000, 0, 0);
    CHECK(su_audio_scene_listener_set(scene, &listener) == SU_OK);
    CHECK(su_audio_emitter_update(scene, emitter, su_wpos(-10000002, 0, 0), zero) == SU_OK);
    CHECK(su_audio_scene_snapshot(scene, emitter, &snapshot) == SU_OK && snapshot.distance_m == 2 &&
          snapshot.direction_listener.x == -2);
    listener.pos = (SuWorldPos){INT64_MIN, 0, 0};
    CHECK(su_audio_scene_listener_set(scene, &listener) == SU_OK);
    CHECK(su_audio_emitter_update(scene, emitter, (SuWorldPos){INT64_MAX, 0, 0}, zero) == SU_OK);
    CHECK(su_audio_scene_snapshot(scene, emitter, &snapshot) == SU_E_INVALID_ARGUMENT);
    voice = base;
    CHECK(su_audio_scene_resolve(scene, emitter, &voice, &lowpass) == SU_E_INVALID_ARGUMENT);
    listener.pos = (SuWorldPos){INT64_MAX - 1, 0, 0};
    CHECK(su_audio_scene_listener_set(scene, &listener) == SU_OK);
    CHECK(su_audio_scene_snapshot(scene, emitter, &snapshot) == SU_OK && snapshot.distance_m > 0);
    CHECK(su_audio_emitter_scale(scene, emitter, FLT_TRUE_MIN) == SU_OK);
    CHECK(su_audio_scene_snapshot(scene, emitter, &snapshot) == SU_OK && isfinite(snapshot.curve.gain_db));
    voice = base;
    voice.gain = NAN;
    CHECK(su_audio_scene_resolve(scene, emitter, &voice, &lowpass) == SU_E_INVALID_ARGUMENT);
    voice = base;
    voice.pitch = INFINITY;
    CHECK(su_audio_scene_resolve(scene, emitter, &voice, &lowpass) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_scene_resolve(scene, emitter, NULL, &lowpass) == SU_E_INVALID_ARGUMENT);
    su_audio_scene_destroy(scene);
}

static SuAudioMixer *make_mixer(void) {
    SuAudioMixerDesc desc = {sizeof desc, 48000, 2, 32, 343};
    SuAudioMixer *mixer = NULL;
    CHECK(su_audio_mixer_create(&desc, &mixer) == SU_OK);
    return mixer;
}

static void radius_scaling(void) {
    SuAudioScene *scene = make_scene(1, 3);
    SuAudioEmitter emitter = make_emitter(scene, 10);
    SuAudioCurvePoint points[] = {{0, 0, 20000, 0, 0, 2}, {20, -20, 2000, 400, 2, 2}};
    SuAudioCurveId curve = add_curve(scene, points, 2);
    SuAudioSpatialSnapshot snapshot;
    SuAudioVoiceDesc voice = {0};
    SuVec3f zero = {0};
    float lowpass = -1;
    voice.gain = 1;
    CHECK(su_audio_emitter_curve(scene, emitter, curve) == SU_OK);
    CHECK(su_audio_scene_resolve(scene, emitter, &voice, &lowpass) == SU_OK);
    CHECK(near(voice.gain, pow(10, -5.0 / 20)) && lowpass == 15500);
    CHECK(su_audio_emitter_scale(scene, emitter, 2) == SU_OK);
    CHECK(su_audio_scene_snapshot(scene, emitter, &snapshot) == SU_OK);
    CHECK(snapshot.curve.gain_db == -2.5f && snapshot.curve.scale == 2);
    points[0].scale = points[1].scale = 0;
    curve = add_curve(scene, points, 2);
    CHECK(su_audio_emitter_curve(scene, emitter, curve) == SU_OK);
    CHECK(su_audio_scene_snapshot(scene, emitter, &snapshot) == SU_OK);
    CHECK(snapshot.curve.gain_db == -20 && snapshot.curve.scale == 0);
    CHECK(su_audio_emitter_update(scene, emitter, su_wpos(0, 0, 0), zero) == SU_OK);
    CHECK(su_audio_scene_snapshot(scene, emitter, &snapshot) == SU_OK);
    CHECK(snapshot.curve.gain_db == 0 && snapshot.curve.scale == 0);
    points[0].gain_db = 60;
    points[0].scale = FLT_TRUE_MIN;
    points[0].spread_rad = 3.141592653589793f;
    curve = add_curve(scene, points, 1);
    CHECK(su_audio_emitter_curve(scene, emitter, curve) == SU_OK);
    voice.gain = FLT_MAX;
    CHECK(su_audio_scene_resolve(scene, emitter, &voice, NULL) == SU_OK && voice.gain == FLT_MAX);
    CHECK(su_audio_emitter_update(scene, emitter, su_wpos(1, 0, 0), zero) == SU_OK);
    CHECK(su_audio_scene_snapshot(scene, emitter, &snapshot) == SU_OK && snapshot.curve.gain_db == 60);
    su_audio_scene_destroy(scene);
}

static double render_energy(SuAudioMixer *mixer, float output[512]) {
    unsigned index;
    double energy = 0;
    su_audio_mixer_render(mixer, output, 256);
    for (index = 0; index < 512; index++) {
        CHECK(isfinite(output[index]));
        energy += (double)output[index] * output[index];
    }
    return energy;
}

static void playback(void) {
    SuAudioScene *scene = make_scene(1, 1);
    SuAudioEmitter emitter = make_emitter(scene, 10);
    SuAudioCurvePoint point = {0, -6, 0, 100, 1, 0.5f};
    SuAudioCurveId curve = add_curve(scene, &point, 1);
    SuAudioMixer *mixer = make_mixer();
    SuAudioMixer *reference = make_mixer();
    float pcm[256];
    float output[512], expected[512];
    SuAudioClip clip = {pcm, 256, 1, 48000};
    SuAudioVoiceDesc base = {0};
    SuAudioVoiceDesc resolved;
    SuAudioVoice voice = 0, other = 0;
    SuAudioCameraListener camera = {0};
    SuVec3f zero = {0};
    float lowpass;
    double energy;
    unsigned index;
    for (index = 0; index < 256; index++) {
        pcm[index] = 0.25f;
    }
    base.clip = &clip;
    base.gain = 0.5f;
    base.flags = SU_AUDIO_LOOP | SU_AUDIO_SPATIAL;
    base.pos = su_wpos(10, 0, 0);
    base.min_distance = 1;
    base.rolloff = 1;
    resolved = base;
    CHECK(su_audio_scene_listener_apply(scene, mixer) == SU_OK);
    CHECK(su_audio_scene_resolve(scene, emitter, &resolved, &lowpass) == SU_OK);
    CHECK(su_audio_play(mixer, &resolved, &voice) == SU_OK);
    CHECK(su_audio_play(reference, &base, &other) == SU_OK);
    (void)render_energy(mixer, output);
    (void)render_energy(reference, expected);
    CHECK(memcmp(output, expected, sizeof output) == 0);
    CHECK(su_audio_stop(mixer, voice, 0) == SU_OK);
    (void)render_energy(mixer, output);
    CHECK(su_audio_emitter_curve(scene, emitter, curve) == SU_OK);
    resolved = base;
    CHECK(su_audio_scene_resolve(scene, emitter, &resolved, &lowpass) == SU_OK);
    CHECK(su_audio_play(mixer, &resolved, &voice) == SU_OK);
    CHECK(su_audio_voice_set_lowpass(mixer, voice, lowpass) == SU_OK);
    energy = render_energy(mixer, output);
    CHECK(near(energy, 256 * pow(0.25 * 0.5 * pow(10, -6.0 / 20), 2)));
    CHECK(fabs(output[0]) < 0.000001 && output[1] > 0);
    camera.rot = (SuQuatf){0, 1, 0, 0};
    CHECK(su_audio_scene_listener_set(scene, &camera) == SU_OK);
    CHECK(su_audio_scene_listener_apply(scene, mixer) == SU_OK);
    resolved = base;
    CHECK(su_audio_scene_resolve(scene, emitter, &resolved, &lowpass) == SU_OK);
    CHECK(su_audio_voice_update(mixer, voice, resolved.pos, resolved.vel, resolved.gain) == SU_OK);
    (void)render_energy(mixer, output);
    energy = render_energy(mixer, output);
    CHECK(output[0] > 0 && fabs(output[1]) < 0.000001);
    CHECK(near(energy, 256 * pow(0.25 * 0.5 * pow(10, -6.0 / 20), 2)));
    CHECK(su_audio_emitter_update(scene, emitter, su_wpos(0, 0, -10), zero) == SU_OK);
    resolved = base;
    CHECK(su_audio_scene_resolve(scene, emitter, &resolved, &lowpass) == SU_OK);
    CHECK(su_audio_voice_update(mixer, voice, resolved.pos, resolved.vel, resolved.gain) == SU_OK);
    (void)render_energy(mixer, output);
    energy = render_energy(mixer, output);
    CHECK(near(output[0], output[1]));
    CHECK(near(energy, 256 * pow(0.25 * 0.5 * pow(10, -6.0 / 20), 2)));
    CHECK(su_audio_voice_set_lowpass(mixer, voice, 1000) == SU_OK);
    (void)render_energy(mixer, output);
    CHECK(su_audio_emitter_destroy(scene, emitter) == SU_OK);
    CHECK(su_audio_scene_resolve(scene, emitter, &resolved, &lowpass) == SU_E_STALE_HANDLE);
    CHECK(su_audio_voice_active(mixer, voice));
    su_audio_mixer_destroy(mixer);
    su_audio_mixer_destroy(reference);
    su_audio_scene_destroy(scene);
}

int main(void) {
    tables();
    handles();
    resolution();
    radius_scaling();
    playback();
    printf("sizes: emitter=%zu curve_id=%zu voice_legacy=%zu voice_v2=%zu emitter_snapshot=%zu listener=%zu spatial_snapshot=%zu\n",
        sizeof(SuAudioEmitter), sizeof(SuAudioCurveId), sizeof(SuAudioVoiceDesc), sizeof(SuAudioVoiceDescV2),
        sizeof(SuAudioEmitterSnapshot), sizeof(SuAudioCameraListener), sizeof(SuAudioSpatialSnapshot));
    printf("audio spatial: %u checks, %u failures\n", checks, failures);
    return failures != 0u;
}
