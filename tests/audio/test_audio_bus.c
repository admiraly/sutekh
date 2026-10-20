#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sutekh/audio.h"
#include "sutekh/audio_bus.h"

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    } \
} while (0)

#define RATE 48000u
#define BLOCK 128u
#define MASTER 0u
#define MUSIC 1u
#define SFX 2u
#define VOICE 3u

static float ones[BLOCK * 2u];
static float zeros[BLOCK * 2u];

static SuAudioBusDesc bus(SuAudioBusId id, SuAudioBusId parent) {
    SuAudioBusDesc d;
    memset(&d, 0, sizeof d);
    d.id = id;
    d.parent = parent;
    return d;
}

static SuAudioDuckRule rule(SuAudioBusId source, SuAudioBusId target, float max_db) {
    SuAudioDuckRule r;
    memset(&r, 0, sizeof r);
    r.source = source;
    r.target = target;
    r.threshold_db = -20.0f;
    r.ratio = 1000.0f;
    r.max_attenuation_db = max_db;
    return r;
}

static SuAudioBusTreeDesc tree_desc(const SuAudioBusDesc *buses, uint32_t bus_count,
                                    const SuAudioDuckRule *rules, uint32_t rule_count) {
    SuAudioBusTreeDesc d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.sample_rate = RATE;
    d.max_block_frames = BLOCK;
    d.buses = buses;
    d.bus_count = bus_count;
    d.rules = rules;
    d.rule_count = rule_count;
    return d;
}

static SuAudioBusTree *make(const SuAudioBusTreeDesc *d) {
    SuAudioBusTree *tree = NULL;
    SuAudioBusDiagnostic diagnostic;
    CHECK(su_audio_bus_tree_create(d, &tree, &diagnostic) == SU_OK);
    CHECK(diagnostic.code == SU_AUDIO_BUS_DIAG_NONE);
    return tree;
}

static float max_step(const float *out, uint32_t frames, uint32_t channel) {
    uint32_t f;
    float step = 0.0f;
    for (f = 1u; f < frames; ++f) {
        float d = fabsf(out[f * 2u + channel] - out[(f - 1u) * 2u + channel]);
        if (d > step) step = d;
    }
    return step;
}

static void test_validation(void) {
    SuAudioBusDesc buses[4] = {bus(MASTER, SU_AUDIO_BUS_ROOT), bus(MUSIC, MASTER), bus(SFX, MASTER), bus(VOICE, SFX)};
    SuAudioDuckRule rules[2];
    SuAudioBusTreeDesc d = tree_desc(buses, 4u, NULL, 0u);
    SuAudioBusTree *tree = NULL;
    SuAudioBusDiagnostic diagnostic;
    SuAudioBusId id = 99u;
    buses[2].id = MUSIC;
    CHECK(su_audio_bus_tree_create(&d, &tree, &diagnostic) == SU_E_INVALID_ARGUMENT);
    CHECK(diagnostic.code == SU_AUDIO_BUS_DIAG_DUPLICATE && diagnostic.bus_id == MUSIC);
    buses[2].id = SFX;
    buses[1].parent = 42u;
    CHECK(su_audio_bus_tree_create(&d, &tree, &diagnostic) == SU_E_INVALID_ARGUMENT);
    CHECK(diagnostic.code == SU_AUDIO_BUS_DIAG_UNKNOWN_BUS && diagnostic.bus_id == MUSIC);
    buses[1].parent = SU_AUDIO_BUS_ROOT;
    CHECK(su_audio_bus_tree_create(&d, &tree, &diagnostic) == SU_E_INVALID_ARGUMENT);
    CHECK(diagnostic.code == SU_AUDIO_BUS_DIAG_ROOT);
    buses[1].parent = VOICE; /* parent cycle: music -> voice -> sfx -> master is fine; make sfx a child of music */
    buses[2].parent = MUSIC;
    CHECK(su_audio_bus_tree_create(&d, &tree, &diagnostic) == SU_E_SCHEMA_MISMATCH);
    CHECK(diagnostic.code == SU_AUDIO_BUS_DIAG_CYCLE);
    buses[1].parent = MASTER;
    buses[2].parent = MASTER;
    /* sidechain feedback: sfx ducks voice, but voice feeds sfx */
    rules[0] = rule(SFX, VOICE, 6.0f);
    d.rules = rules;
    d.rule_count = 1u;
    CHECK(su_audio_bus_tree_create(&d, &tree, &diagnostic) == SU_E_SCHEMA_MISMATCH);
    CHECK(diagnostic.code == SU_AUDIO_BUS_DIAG_CYCLE);
    CHECK(tree == NULL);
    rules[0].flags = SU_AUDIO_DUCK_DELAY_QUANTUM;
    tree = make(&d);
    su_audio_bus_tree_destroy(tree);
    rules[0].flags = 0u;
    rules[0].ratio = 0.5f;
    CHECK(su_audio_bus_tree_create(&d, &tree, &diagnostic) == SU_E_INVALID_ARGUMENT);
    CHECK(diagnostic.code == SU_AUDIO_BUS_DIAG_DESC && diagnostic.rule_index == 0u);
    rules[0] = rule(SFX, 77u, 6.0f);
    CHECK(su_audio_bus_tree_create(&d, &tree, &diagnostic) == SU_E_INVALID_ARGUMENT);
    CHECK(diagnostic.code == SU_AUDIO_BUS_DIAG_UNKNOWN_BUS && diagnostic.bus_id == 77u);
    d.rule_count = 0u;
    d.hdr.enabled = 1u; /* enabled requires a window */
    CHECK(su_audio_bus_tree_create(&d, &tree, &diagnostic) == SU_E_INVALID_ARGUMENT);
    d.hdr.enabled = 0u;
    d.max_block_frames = 0u;
    CHECK(su_audio_bus_tree_create(&d, &tree, &diagnostic) == SU_E_INVALID_ARGUMENT);
    d.max_block_frames = BLOCK;
    /* execution order: children and sidechain sources first, ties by ID */
    rules[0] = rule(MUSIC, SFX, 6.0f);
    rules[1] = rule(VOICE, MUSIC, 6.0f);
    d.rule_count = 2u;
    tree = make(&d);
    {
        SuAudioBusId order[4];
        uint32_t i;
        for (i = 0u; i < 4u; ++i) CHECK(su_audio_bus_order(tree, i, &order[i]) == SU_OK);
        CHECK(order[0] == VOICE && order[1] == MUSIC && order[2] == SFX && order[3] == MASTER);
    }
    CHECK(su_audio_bus_order(tree, 4u, &id) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_bus_mix(tree, 55u, ones, BLOCK) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_bus_mix(tree, SFX, ones, BLOCK + 1u) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_bus_mix(tree, SFX, ones, 64u) == SU_OK);
    CHECK(su_audio_bus_mix(tree, SFX, ones, 32u) == SU_E_INVALID_ARGUMENT);
    {
        float out[BLOCK * 2u];
        CHECK(su_audio_bus_process(tree, out, 32u) == SU_E_INVALID_ARGUMENT);
        CHECK(su_audio_bus_process(tree, out, 64u) == SU_OK);
    }
    su_audio_bus_tree_destroy(tree);
}

/* Unity tree, no rules, window disabled with stored values: equals a single legacy mixer. */
static void test_disable_all_matches_legacy(void) {
    static float tone_a[RATE / 4u], tone_b[RATE / 4u * 2u];
    static float legacy[4096u * 2u], part_a[4096u * 2u], part_b[4096u * 2u], tree_out[4096u * 2u];
    SuAudioClip clip_a = {tone_a, RATE / 4u, 1u, RATE};
    SuAudioClip clip_b = {tone_b, RATE / 4u, 2u, RATE};
    SuAudioBusDesc buses[3] = {bus(MASTER, SU_AUDIO_BUS_ROOT), bus(MUSIC, MASTER), bus(SFX, MASTER)};
    SuAudioBusTreeDesc d = tree_desc(buses, 3u, NULL, 0u);
    SuAudioMixerDesc md;
    SuAudioVoiceDesc vd;
    SuAudioMixer *all = NULL, *only_a = NULL, *only_b = NULL;
    SuAudioVoice voice;
    SuAudioBusTree *tree;
    uint32_t i, b;
    float worst = 0.0f;
    for (i = 0u; i < RATE / 4u; ++i) {
        tone_a[i] = 0.2f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / (float)RATE);
        tone_b[i * 2u] = 0.1f * sinf(2.0f * 3.14159265f * 660.0f * (float)i / (float)RATE);
        tone_b[i * 2u + 1u] = -tone_b[i * 2u];
    }
    d.hdr.window_db = 30.0f; /* stored but disabled */
    d.hdr.ceiling_db = -100.0f;
    tree = make(&d);
    memset(&md, 0, sizeof md);
    md.struct_size = sizeof md;
    md.sample_rate = RATE;
    CHECK(su_audio_mixer_create(&md, &all) == SU_OK);
    CHECK(su_audio_mixer_create(&md, &only_a) == SU_OK);
    CHECK(su_audio_mixer_create(&md, &only_b) == SU_OK);
    memset(&vd, 0, sizeof vd);
    vd.clip = &clip_a;
    vd.gain = 1.0f;
    CHECK(su_audio_play(all, &vd, &voice) == SU_OK);
    CHECK(su_audio_play(only_a, &vd, &voice) == SU_OK);
    vd.clip = &clip_b;
    CHECK(su_audio_play(all, &vd, &voice) == SU_OK);
    CHECK(su_audio_play(only_b, &vd, &voice) == SU_OK);
    for (b = 0u; b < 4096u / BLOCK; ++b) {
        size_t at = (size_t)b * BLOCK * 2u;
        su_audio_mixer_render(all, legacy + at, BLOCK);
        su_audio_mixer_render(only_a, part_a + at, BLOCK);
        su_audio_mixer_render(only_b, part_b + at, BLOCK);
        CHECK(su_audio_bus_mix(tree, MUSIC, part_a + at, BLOCK) == SU_OK);
        CHECK(su_audio_bus_mix(tree, SFX, part_b + at, BLOCK) == SU_OK);
        CHECK(su_audio_bus_set_loudness(tree, MUSIC, 120.0f) == SU_OK);
        CHECK(su_audio_bus_process(tree, tree_out + at, BLOCK) == SU_OK);
    }
    for (i = 0u; i < 4096u * 2u; ++i) {
        float e = fabsf(legacy[i] - tree_out[i]);
        if (e > worst) worst = e;
        CHECK(tree_out[i] == part_a[i] + part_b[i]); /* the tree itself adds exactly */
    }
    CHECK(worst <= 1e-6f); /* audio_float_local_v1: summation order only */
    printf("disable-all versus legacy mixer: max abs difference %.3g\n", (double)worst);
    su_audio_mixer_destroy(all);
    su_audio_mixer_destroy(only_a);
    su_audio_mixer_destroy(only_b);
    su_audio_bus_tree_destroy(tree);
}

static void test_gain_staging_and_ramps(void) {
    SuAudioBusDesc buses[2] = {bus(MASTER, SU_AUDIO_BUS_ROOT), bus(SFX, MASTER)};
    SuAudioBusTreeDesc d;
    SuAudioBusTree *tree;
    SuAudioBusMeter m;
    static float out[BLOCK * 4u * 2u];
    float g = 0.0f;
    uint32_t b;
    buses[0].gain_db = 6.0f;
    buses[1].gain_db = -12.0f;
    d = tree_desc(buses, 2u, NULL, 0u);
    tree = make(&d);
    CHECK(su_audio_bus_mix(tree, SFX, ones, BLOCK) == SU_OK);
    CHECK(su_audio_bus_process(tree, out, BLOCK) == SU_OK);
    CHECK(fabsf(out[0] - powf(10.0f, -6.0f / 20.0f)) < 1e-6f);
    CHECK(su_audio_bus_gain_at(tree, SFX, &g) == SU_OK && fabsf(g - powf(10.0f, -12.0f / 20.0f)) < 1e-6f);
    CHECK(su_audio_bus_meter_read(tree, MASTER, &m, 1) == SU_OK);
    CHECK(fabsf(m.peak - out[0]) < 1e-6f && fabsf(m.rms - out[0]) < 1e-6f && m.frames == BLOCK);
    CHECK(fabsf(m.fader_db - 6.0f) < 1e-4f);
    CHECK(su_audio_bus_meter_read(tree, MASTER, &m, 0) == SU_OK && m.frames == 0u && m.peak == 0.0f);
    /* a 300-frame fader ramp across blocks is linear and click-free */
    CHECK(su_audio_bus_set_gain(tree, SFX, 0.0f, 300u) == SU_OK);
    for (b = 0u; b < 4u; ++b) {
        CHECK(su_audio_bus_mix(tree, SFX, ones, BLOCK) == SU_OK);
        CHECK(su_audio_bus_process(tree, out + b * BLOCK * 2u, BLOCK) == SU_OK);
    }
    {
        float from = powf(10.0f, -6.0f / 20.0f), to = powf(10.0f, 6.0f / 20.0f);
        CHECK(max_step(out, BLOCK * 4u, 0u) <= (to - from) / 300.0f + 1e-5f);
        CHECK(fabsf(out[299u * 2u] - to) < 1e-5f);
        CHECK(out[300u * 2u] == out[511u * 2u]);
    }
    CHECK(su_audio_bus_set_gain(tree, SFX, NAN, 0u) == SU_E_INVALID_ARGUMENT);
    su_audio_bus_tree_destroy(tree);
}

/* Source bus: 1.0 for 600 frames then silence. Target: 1.0 throughout. Output - source = target gain. */
static void test_duck_envelope(void) {
    SuAudioBusDesc buses[3] = {bus(MASTER, SU_AUDIO_BUS_ROOT), bus(MUSIC, MASTER), bus(VOICE, MASTER)};
    SuAudioDuckRule r = rule(VOICE, MUSIC, 12.0f);
    SuAudioBusTreeDesc d;
    SuAudioBusTree *tree;
    static float out[BLOCK * 24u * 2u];
    uint32_t b, f, bad = 0u;
    float worst_step = 0.0f;
    r.attack_ms = 10.0f;  /* 480 frames for 12 dB */
    r.hold_ms = 5.0f;     /* 240 frames */
    r.release_ms = 20.0f; /* 960 frames for 12 dB */
    d = tree_desc(buses, 3u, &r, 1u);
    tree = make(&d);
    for (b = 0u; b < 24u; ++b) {
        static float src[BLOCK * 2u];
        for (f = 0u; f < BLOCK; ++f) src[f * 2u] = src[f * 2u + 1u] = b * BLOCK + f < 600u ? 1.0f : 0.0f;
        CHECK(su_audio_bus_mix(tree, VOICE, src, BLOCK) == SU_OK);
        CHECK(su_audio_bus_mix(tree, MUSIC, ones, BLOCK) == SU_OK);
        CHECK(su_audio_bus_process(tree, out + b * BLOCK * 2u, BLOCK) == SU_OK);
    }
    for (f = 0u; f < BLOCK * 24u; ++f) {
        float source = f < 600u ? 1.0f : 0.0f;
        float env;
        float music = out[f * 2u] - source;
        if (f < 600u) env = fminf(12.0f, 12.0f / 480.0f * (float)(f + 1u));
        else if (f < 600u + 240u) env = 12.0f;
        else env = fmaxf(0.0f, 12.0f - 12.0f / 960.0f * (float)(f - 839u));
        if (fabsf(music - powf(10.0f, -env / 20.0f)) > 2e-4f) bad++;
        if (out[f * 2u] != out[f * 2u + 1u]) bad++;
        if (f > 0u && f != 600u) {
            float s = fabsf(out[f * 2u] - out[(f - 1u) * 2u]);
            if (s > worst_step) worst_step = s;
        }
    }
    CHECK(bad == 0u);
    CHECK(worst_step < 0.01f); /* only the source's own edge at 600 is a step */
    {
        SuAudioBusMeter m;
        CHECK(su_audio_bus_meter_read(tree, MUSIC, &m, 0) == SU_OK);
        CHECK(m.duck_db == 0.0f);
    }
    su_audio_bus_tree_destroy(tree);
}

static void test_stereo_link_silence_clamp_cascade(void) {
    SuAudioBusDesc buses[4] = {bus(MASTER, SU_AUDIO_BUS_ROOT), bus(MUSIC, MASTER), bus(SFX, MASTER), bus(VOICE, MASTER)};
    SuAudioDuckRule rules[3];
    SuAudioBusTreeDesc d;
    SuAudioBusTree *tree;
    SuAudioBusMeter m;
    static float right_only[BLOCK * 2u], out[BLOCK * 2u];
    uint32_t f;
    float g;
    for (f = 0u; f < BLOCK; ++f) right_only[f * 2u + 1u] = 0.5f;
    /* stereo-linked: a right-only sidechain ducks both channels of the target */
    rules[0] = rule(VOICE, MUSIC, 6.0f);
    d = tree_desc(buses, 4u, rules, 1u);
    tree = make(&d);
    CHECK(su_audio_bus_mix(tree, VOICE, right_only, BLOCK) == SU_OK);
    CHECK(su_audio_bus_mix(tree, MUSIC, ones, BLOCK) == SU_OK);
    CHECK(su_audio_bus_process(tree, out, BLOCK) == SU_OK);
    g = powf(10.0f, -6.0f / 20.0f);
    CHECK(fabsf(out[(BLOCK - 1u) * 2u] - g) < 1e-6f);
    CHECK(fabsf(out[(BLOCK - 1u) * 2u + 1u] - (g + 0.5f)) < 1e-6f);
    /* silence: no detector input, exact passthrough */
    CHECK(su_audio_bus_mix(tree, MUSIC, ones, BLOCK) == SU_OK);
    CHECK(su_audio_bus_process(tree, out, BLOCK) == SU_OK);
    CHECK(out[0] == 1.0f && out[(BLOCK - 1u) * 2u] == 1.0f);
    CHECK(su_audio_bus_gain_at(tree, MUSIC, &g) == SU_OK && g == 1.0f);
    su_audio_bus_tree_destroy(tree);
    /* additive rules clamp at the target's max_duck_db */
    buses[1].max_duck_db = 18.0f;
    rules[0] = rule(VOICE, MUSIC, 12.0f);
    rules[1] = rule(SFX, MUSIC, 12.0f);
    d = tree_desc(buses, 4u, rules, 2u);
    tree = make(&d);
    CHECK(su_audio_bus_mix(tree, VOICE, ones, BLOCK) == SU_OK);
    CHECK(su_audio_bus_mix(tree, SFX, ones, BLOCK) == SU_OK);
    CHECK(su_audio_bus_process(tree, out, BLOCK) == SU_OK);
    CHECK(su_audio_bus_meter_read(tree, MUSIC, &m, 0) == SU_OK && m.duck_db == 18.0f);
    CHECK(su_audio_bus_gain_at(tree, MUSIC, &g) == SU_OK && fabsf(g - powf(10.0f, -18.0f / 20.0f)) < 1e-6f);
    su_audio_bus_tree_destroy(tree);
    buses[1].max_duck_db = 0.0f;
    /* cascade: voice ducks sfx below sfx's own threshold, so sfx no longer ducks music */
    rules[0] = rule(SFX, MUSIC, 12.0f);
    rules[1] = rule(VOICE, SFX, 30.0f);
    rules[1].threshold_db = -40.0f;
    d = tree_desc(buses, 4u, rules, 2u);
    tree = make(&d);
    CHECK(su_audio_bus_mix(tree, SFX, ones, BLOCK) == SU_OK);
    CHECK(su_audio_bus_mix(tree, MUSIC, ones, BLOCK) == SU_OK);
    CHECK(su_audio_bus_process(tree, out, BLOCK) == SU_OK);
    CHECK(su_audio_bus_meter_read(tree, MUSIC, &m, 0) == SU_OK && m.duck_db == 12.0f);
    CHECK(su_audio_bus_mix(tree, VOICE, ones, BLOCK) == SU_OK);
    CHECK(su_audio_bus_mix(tree, SFX, ones, BLOCK) == SU_OK);
    CHECK(su_audio_bus_mix(tree, MUSIC, ones, BLOCK) == SU_OK);
    CHECK(su_audio_bus_process(tree, out, BLOCK) == SU_OK);
    CHECK(su_audio_bus_meter_read(tree, SFX, &m, 0) == SU_OK && m.duck_db == 30.0f);
    CHECK(su_audio_bus_meter_read(tree, MUSIC, &m, 0) == SU_OK && m.duck_db == 0.0f);
    su_audio_bus_tree_destroy(tree);
}

static void test_delayed_feedback(void) {
    /* master output ducks its own child one block later */
    SuAudioBusDesc buses[2] = {bus(MASTER, SU_AUDIO_BUS_ROOT), bus(MUSIC, MASTER)};
    SuAudioDuckRule r = rule(MASTER, MUSIC, 6.0f);
    SuAudioBusTreeDesc d;
    SuAudioBusTree *tree;
    static float out[BLOCK * 2u];
    float g = powf(10.0f, -6.0f / 20.0f);
    r.flags = SU_AUDIO_DUCK_DELAY_QUANTUM;
    d = tree_desc(buses, 2u, &r, 1u);
    tree = make(&d);
    CHECK(su_audio_bus_mix(tree, MUSIC, ones, 64u) == SU_OK);
    CHECK(su_audio_bus_process(tree, out, 64u) == SU_OK);
    CHECK(out[63u * 2u] == 1.0f);
    CHECK(su_audio_bus_mix(tree, MUSIC, ones, 64u) == SU_OK);
    CHECK(su_audio_bus_process(tree, out, 64u) == SU_OK);
    CHECK(out[63u * 2u] == 1.0f); /* still inside the 128-frame delay */
    CHECK(su_audio_bus_mix(tree, MUSIC, ones, BLOCK) == SU_OK);
    CHECK(su_audio_bus_process(tree, out, BLOCK) == SU_OK);
    CHECK(fabsf(out[0] - g) < 1e-6f);
    su_audio_bus_tree_destroy(tree);
}

static void test_hdr_window(void) {
    SuAudioBusDesc buses[3] = {bus(MASTER, SU_AUDIO_BUS_ROOT), bus(MUSIC, MASTER), bus(SFX, MASTER)};
    SuAudioBusTreeDesc d;
    SuAudioBusTree *tree;
    SuAudioBusMeter m;
    static float out[BLOCK * 2u];
    float last = 0.0f;
    uint32_t b;
    buses[1].hdr = 1u;
    buses[2].hdr = 1u;
    d = tree_desc(buses, 3u, NULL, 0u);
    d.hdr.enabled = 1u;
    d.hdr.window_db = 20.0f;
    d.hdr.ceiling_db = 90.0f;
    d.hdr.release_ms = 100.0f; /* 20 dB per 4800 frames */
    tree = make(&d);
    CHECK(su_audio_bus_mix(tree, MUSIC, ones, BLOCK) == SU_OK);
    CHECK(su_audio_bus_mix(tree, SFX, zeros, BLOCK) == SU_OK);
    CHECK(su_audio_bus_set_loudness(tree, SFX, 100.0f) == SU_OK);
    CHECK(su_audio_bus_set_loudness(tree, MUSIC, 70.0f) == SU_OK);
    CHECK(su_audio_bus_process(tree, out, BLOCK) == SU_OK);
    CHECK(su_audio_bus_meter_read(tree, MUSIC, &m, 0) == SU_OK && m.hdr_db == 200.0f);
    CHECK(su_audio_bus_meter_read(tree, SFX, &m, 0) == SU_OK && fabsf(m.hdr_db - 10.0f) < 1e-5f);
    CHECK(out[(BLOCK - 1u) * 2u] == 0.0f);                       /* culled by the end of the block */
    CHECK(max_step(out, BLOCK, 0u) <= 1.0f / (float)BLOCK + 1e-6f); /* ramped, no click */
    /* the loud source ends: the window top falls at the release rate, music re-enters the window */
    for (b = 0u; b < 40u; ++b) {
        CHECK(su_audio_bus_mix(tree, MUSIC, ones, BLOCK) == SU_OK);
        CHECK(su_audio_bus_set_loudness(tree, MUSIC, 70.0f) == SU_OK);
        CHECK(su_audio_bus_process(tree, out, BLOCK) == SU_OK);
        if (b == 0u) CHECK(out[(BLOCK - 1u) * 2u] == 0.0f);
        last = out[(BLOCK - 1u) * 2u];
    }
    CHECK(su_audio_bus_meter_read(tree, MUSIC, &m, 0) == SU_OK && m.hdr_db == 0.0f);
    CHECK(last == 1.0f);
    su_audio_bus_tree_destroy(tree);
}

static void test_true_peak(void) {
    SuAudioBusDesc buses[1] = {bus(MASTER, SU_AUDIO_BUS_ROOT)};
    SuAudioBusTreeDesc d = tree_desc(buses, 1u, NULL, 0u);
    SuAudioBusTree *tree = make(&d);
    SuAudioBusMeter m;
    static float sine[BLOCK * 2u], out[BLOCK * 2u];
    uint32_t f;
    for (f = 0u; f < BLOCK; ++f) /* fs/4 at 45 degrees: samples at +-0.7071, waveform peak 1.0 */
        sine[f * 2u] = sine[f * 2u + 1u] = (f & 2u) ? -0.70710677f : 0.70710677f;
    CHECK(su_audio_bus_mix(tree, MASTER, sine, BLOCK) == SU_OK);
    CHECK(su_audio_bus_process(tree, out, BLOCK) == SU_OK);
    CHECK(su_audio_bus_meter_read(tree, MASTER, &m, 1) == SU_OK);
    CHECK(fabsf(m.peak - 0.70710677f) < 1e-5f);
    CHECK(m.true_peak > 0.85f && m.true_peak < 1.0f);
    printf("true peak estimate: sample peak %.4f, 4x cubic %.4f (analytic 1.0)\n", (double)m.peak, (double)m.true_peak);
    su_audio_bus_tree_destroy(tree);
}

static void test_cost(void) {
    SuAudioBusDesc buses[16];
    SuAudioDuckRule rules[8];
    SuAudioBusTreeDesc d;
    SuAudioBusTree *tree;
    static float in[BLOCK * 2u], out[BLOCK * 2u];
    uint32_t i, b;
    clock_t start;
    double ms;
    buses[0] = bus(0u, SU_AUDIO_BUS_ROOT);
    for (i = 1u; i < 16u; ++i) {
        buses[i] = bus(i, i < 4u ? 0u : (i < 8u ? i - 3u : i - 4u));
        buses[i].hdr = i >= 8u;
        buses[i].max_duck_db = 24.0f;
    }
    for (i = 0u; i < 8u; ++i) {
        rules[i] = rule(8u + i, i < 4u ? 1u + i : 4u + i - 4u, 9.0f);
        rules[i].attack_ms = 5.0f;
        rules[i].hold_ms = 20.0f;
        rules[i].release_ms = 200.0f;
    }
    for (i = 0u; i < BLOCK; ++i) in[i * 2u] = in[i * 2u + 1u] = 0.3f * sinf((float)i * 0.05f);
    d = tree_desc(buses, 16u, rules, 8u);
    d.hdr.enabled = 1u;
    d.hdr.window_db = 30.0f;
    d.hdr.ceiling_db = 100.0f;
    d.hdr.release_ms = 500.0f;
    tree = make(&d);
    start = clock();
    for (b = 0u; b < RATE / BLOCK; ++b) {
        for (i = 4u; i < 16u; ++i) {
            CHECK(su_audio_bus_mix(tree, i, in, BLOCK) == SU_OK);
            if (i >= 8u) CHECK(su_audio_bus_set_loudness(tree, i, 80.0f + (float)((i * 7u + b) % 30u)) == SU_OK);
        }
        CHECK(su_audio_bus_process(tree, out, BLOCK) == SU_OK);
    }
    ms = (double)(clock() - start) * 1000.0 / (double)CLOCKS_PER_SEC;
    printf("bus tree cost: 16 buses (3 levels), 8 duck rules, window on, 48,000 frames in 128-frame blocks: "
           "%.2f ms CPU; prepared %llu bytes\n", ms, (unsigned long long)su_audio_bus_tree_bytes(tree));
    su_audio_bus_tree_destroy(tree);
}

int main(void) {
    uint32_t i;
    for (i = 0u; i < BLOCK * 2u; ++i) ones[i] = 1.0f;
    test_validation();
    test_disable_all_matches_legacy();
    test_gain_staging_and_ramps();
    test_duck_envelope();
    test_stereo_link_silence_clamp_cascade();
    test_delayed_feedback();
    test_hdr_window();
    test_true_peak();
    test_cost();
    printf("audio bus: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
