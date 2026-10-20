#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sutekh/audio_voice_policy.h"

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    } \
} while (0)

#define RAMP_FRAMES 48000u
#define Q SU_AUDIO_VOICE_QUANTUM

static float ramp_samples[RAMP_FRAMES];
static float dc_samples[4096];
static float zero_samples[4096];
static float short_samples[256];
static SuAudioClip ramp_clip = {ramp_samples, RAMP_FRAMES, 1u, 48000u};
static SuAudioClip dc_clip = {dc_samples, 4096u, 1u, 48000u};
static SuAudioClip zero_clip = {zero_samples, 4096u, 1u, 48000u};
static SuAudioClip short_clip = {short_samples, 256u, 1u, 48000u};

static void synthesize(void) {
    uint32_t i;
    for (i = 0u; i < RAMP_FRAMES; ++i) ramp_samples[i] = (float)(i + 1u) / 65536.0f; /* exact in float */
    for (i = 0u; i < 4096u; ++i) dc_samples[i] = 0.5f;
    for (i = 0u; i < 256u; ++i) short_samples[i] = 0.25f;
}

static SuAudioVoicePool *pool_make(uint32_t logical, uint32_t physical, uint32_t hero, uint32_t transitions) {
    SuAudioVoicePoolDesc desc;
    SuAudioVoicePool *pool = NULL;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.sample_rate = 48000u;
    desc.max_logical = logical;
    desc.max_physical = physical;
    desc.hero_reserve = hero;
    desc.transition_reserve = transitions;
    CHECK(su_audio_voice_pool_create(&desc, &pool) == SU_OK);
    return pool;
}

static SuAudioLogicalDesc voice_desc(const SuAudioClip *clip, float gain, int32_t priority, uint64_t seq) {
    SuAudioLogicalDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.clip = clip;
    desc.gain = gain;
    desc.policy.priority = priority;
    desc.trigger_seq = seq;
    desc.node_id = 1u;
    desc.event_id = 1u;
    return desc;
}

static SuAudioLogicalVoice add(SuAudioVoicePool *pool, const SuAudioLogicalDesc *desc) {
    SuAudioLogicalVoice voice = 0u;
    CHECK(su_audio_voice_add(pool, desc, &voice) == SU_OK);
    CHECK(voice != 0u);
    return voice;
}

static uint32_t state_of(SuAudioVoicePool *pool, SuAudioLogicalVoice voice) {
    SuAudioVoiceInfo info;
    if (su_audio_voice_info(pool, voice, &info) != SU_OK) return SU_AUDIO_VOICE_DEAD;
    return info.state;
}

static uint32_t reason_of(SuAudioVoicePool *pool, SuAudioLogicalVoice voice) {
    SuAudioVoiceInfo info;
    CHECK(su_audio_voice_info(pool, voice, &info) == SU_OK);
    return info.reason;
}

static float scratch[Q * 2u * 64u];

static void render(SuAudioVoicePool *pool, uint32_t frames) {
    CHECK(frames <= Q * 64u);
    CHECK(su_audio_voice_pool_render(pool, scratch, frames) == SU_OK);
}

static void test_validation(void) {
    SuAudioVoicePoolDesc desc;
    SuAudioVoicePool *pool = NULL;
    SuAudioLogicalDesc v = voice_desc(&dc_clip, 1.0f, 0, 1u);
    SuAudioLogicalVoice handle = 0u;
    memset(&desc, 0, sizeof desc);
    CHECK(su_audio_voice_pool_create(&desc, &pool) == SU_E_INVALID_ARGUMENT);
    desc.struct_size = sizeof desc;
    desc.max_logical = 4u;
    desc.max_physical = 2u;
    desc.hero_reserve = 2u;
    CHECK(su_audio_voice_pool_create(&desc, &pool) == SU_E_INVALID_ARGUMENT);
    CHECK(pool == NULL);
    desc.hero_reserve = 0u;
    desc.max_physical = 5u;
    CHECK(su_audio_voice_pool_create(&desc, &pool) == SU_E_INVALID_ARGUMENT);
    pool = pool_make(2u, 1u, 0u, 0u);
    v.pan = 2.0f;
    CHECK(su_audio_voice_add(pool, &v, &handle) == SU_E_INVALID_ARGUMENT);
    v.pan = 0.0f;
    v.policy.virtual_mode = 9u;
    CHECK(su_audio_voice_add(pool, &v, &handle) == SU_E_INVALID_ARGUMENT);
    v.policy.virtual_mode = 0u;
    handle = add(pool, &v);
    CHECK(su_audio_voice_set_audibility(pool, handle, NAN, 1.0f) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_voice_set_audibility(pool, handle, 1.0f, -1.0f) == SU_E_INVALID_ARGUMENT);
    CHECK(su_audio_voice_stop(pool, handle, 0u) == SU_OK);
    CHECK(su_audio_voice_stop(pool, handle, 0u) == SU_E_STALE_HANDLE);
    CHECK(su_audio_voice_info(pool, handle, &(SuAudioVoiceInfo){0}) == SU_E_STALE_HANDLE);
    CHECK(strcmp(su_audio_voice_reason_name(SU_AUDIO_REASON_SCOPE_LIMIT), "scope_limit") == 0);
    CHECK(strcmp(su_audio_voice_reason_name(999u), "unknown") == 0);
    su_audio_voice_pool_destroy(pool);
}

static void test_nearest_quiet_vs_far_loud(void) {
    SuAudioVoicePool *pool = pool_make(4u, 1u, 0u, 0u);
    SuAudioLogicalDesc quiet = voice_desc(&dc_clip, 0.2f, 0, 1u);
    SuAudioLogicalDesc loud = voice_desc(&dc_clip, 1.0f, 0, 2u);
    SuAudioLogicalVoice a = add(pool, &quiet), b = add(pool, &loud);
    CHECK(su_audio_voice_set_audibility(pool, a, 1.0f, 1.0f) == SU_OK);  /* 0.2 */
    CHECK(su_audio_voice_set_audibility(pool, b, 1.0f, 10.0f) == SU_OK); /* 0.1 */
    render(pool, Q);
    CHECK(state_of(pool, a) == SU_AUDIO_VOICE_PHYSICAL);
    CHECK(state_of(pool, b) == SU_AUDIO_VOICE_VIRTUAL);
    CHECK(reason_of(pool, b) == SU_AUDIO_REASON_RANK);
    /* move the loud one close: it wins at the next boundary */
    CHECK(su_audio_voice_set_audibility(pool, b, 1.0f, 2.0f) == SU_OK); /* 0.5 */
    render(pool, Q);
    CHECK(state_of(pool, a) == SU_AUDIO_VOICE_VIRTUAL);
    CHECK(state_of(pool, b) == SU_AUDIO_VOICE_PHYSICAL);
    /* distance offset biases ranking */
    {
        SuAudioLogicalDesc biased = voice_desc(&dc_clip, 1.0f, 0, 3u);
        SuAudioLogicalVoice c;
        biased.policy.distance_offset = 100.0f;
        c = add(pool, &biased);
        CHECK(su_audio_voice_set_audibility(pool, c, 1.0f, 0.0f) == SU_OK); /* 0.01 */
        render(pool, Q);
        CHECK(state_of(pool, c) == SU_AUDIO_VOICE_VIRTUAL);
    }
    su_audio_voice_pool_destroy(pool);
}

static void test_priority_and_hero_reservation(void) {
    SuAudioVoicePool *pool = pool_make(16u, 4u, 1u, 4u);
    SuAudioLogicalVoice normal[6], hero, low, high;
    SuAudioVoiceStats stats;
    uint32_t i, physical = 0u, reserved = 0u;
    for (i = 0u; i < 6u; ++i) {
        SuAudioLogicalDesc d = voice_desc(&dc_clip, 1.0f, 0, 10u + i);
        normal[i] = add(pool, &d);
    }
    render(pool, Q);
    for (i = 0u; i < 6u; ++i) {
        if (state_of(pool, normal[i]) == SU_AUDIO_VOICE_PHYSICAL) physical++;
        if (reason_of(pool, normal[i]) == SU_AUDIO_REASON_HERO_RESERVED) reserved++;
    }
    CHECK(physical == 3u);
    CHECK(reserved == 3u);
    CHECK(state_of(pool, normal[0]) == SU_AUDIO_VOICE_PHYSICAL);
    CHECK(state_of(pool, normal[3]) == SU_AUDIO_VOICE_VIRTUAL);
    {
        SuAudioLogicalDesc d = voice_desc(&dc_clip, 0.01f, 0, 100u);
        d.policy.flags = SU_AUDIO_VOICE_HERO;
        hero = add(pool, &d);
    }
    render(pool, Q);
    CHECK(state_of(pool, hero) == SU_AUDIO_VOICE_PHYSICAL);
    /* a high-priority voice preempts under saturation regardless of audibility */
    {
        SuAudioLogicalDesc d = voice_desc(&dc_clip, 0.001f, 5, 200u);
        high = add(pool, &d);
        d = voice_desc(&dc_clip, 1.0f, -5, 201u);
        low = add(pool, &d);
    }
    render(pool, Q);
    CHECK(state_of(pool, high) == SU_AUDIO_VOICE_PHYSICAL);
    CHECK(state_of(pool, low) == SU_AUDIO_VOICE_VIRTUAL);
    CHECK(state_of(pool, normal[2]) == SU_AUDIO_VOICE_VIRTUAL);
    su_audio_voice_pool_stats(pool, &stats);
    CHECK(stats.physical == 4u);
    CHECK(stats.physical_peak == 4u);
    CHECK(stats.logical == 9u);
    CHECK(stats.stolen >= 1u);
    su_audio_voice_pool_destroy(pool);
}

static void test_equal_score_order(void) {
    /* insertion order must not matter: lower trigger sequence, then node ID wins */
    uint32_t pass;
    for (pass = 0u; pass < 2u; ++pass) {
        SuAudioVoicePool *pool = pool_make(8u, 2u, 0u, 0u);
        SuAudioLogicalVoice v[4];
        uint32_t i;
        for (i = 0u; i < 4u; ++i) {
            uint32_t k = pass ? 3u - i : i;
            SuAudioLogicalDesc d = voice_desc(&dc_clip, 1.0f, 0, k < 2u ? 7u : 8u);
            d.node_id = 10u + (k & 1u);
            v[k] = add(pool, &d);
        }
        render(pool, Q);
        CHECK(state_of(pool, v[0]) == SU_AUDIO_VOICE_PHYSICAL);
        CHECK(state_of(pool, v[1]) == SU_AUDIO_VOICE_PHYSICAL);
        CHECK(state_of(pool, v[2]) == SU_AUDIO_VOICE_VIRTUAL);
        CHECK(state_of(pool, v[3]) == SU_AUDIO_VOICE_VIRTUAL);
        su_audio_voice_pool_destroy(pool);
    }
}

static void test_scope_limits(void) {
    SuAudioVoicePool *pool = pool_make(16u, 8u, 0u, 0u);
    SuAudioLogicalVoice family[4], emitter[3], other;
    uint32_t i, phys = 0u;
    for (i = 0u; i < 4u; ++i) {
        SuAudioLogicalDesc d = voice_desc(&dc_clip, 1.0f - 0.1f * (float)i, 0, i + 1u);
        d.event_id = 7u;
        d.emitter = 100u + i;
        d.policy.physical_limit = 2u;
        d.policy.limit_scope = SU_AUDIO_LIMIT_EVENT;
        family[i] = add(pool, &d);
    }
    for (i = 0u; i < 3u; ++i) {
        SuAudioLogicalDesc d = voice_desc(&dc_clip, 1.0f, 0, 10u + i);
        d.event_id = 8u + i;
        d.emitter = 500u;
        d.policy.physical_limit = 1u;
        d.policy.limit_scope = SU_AUDIO_LIMIT_EMITTER;
        emitter[i] = add(pool, &d);
    }
    {
        SuAudioLogicalDesc d = voice_desc(&dc_clip, 1.0f, 0, 20u);
        d.event_id = 70u; /* another family, global scope: not limited */
        other = add(pool, &d);
    }
    render(pool, Q);
    CHECK(state_of(pool, family[0]) == SU_AUDIO_VOICE_PHYSICAL);
    CHECK(state_of(pool, family[1]) == SU_AUDIO_VOICE_PHYSICAL);
    CHECK(state_of(pool, family[2]) == SU_AUDIO_VOICE_VIRTUAL);
    CHECK(reason_of(pool, family[3]) == SU_AUDIO_REASON_SCOPE_LIMIT);
    for (i = 0u; i < 3u; ++i) phys += state_of(pool, emitter[i]) == SU_AUDIO_VOICE_PHYSICAL;
    CHECK(phys == 1u);
    CHECK(state_of(pool, emitter[0]) == SU_AUDIO_VOICE_PHYSICAL);
    CHECK(reason_of(pool, emitter[2]) == SU_AUDIO_REASON_SCOPE_LIMIT);
    CHECK(state_of(pool, other) == SU_AUDIO_VOICE_PHYSICAL);
    /* stopping a family member frees its scope count */
    CHECK(su_audio_voice_stop(pool, family[0], 0u) == SU_OK);
    render(pool, Q);
    CHECK(state_of(pool, family[2]) == SU_AUDIO_VOICE_PHYSICAL);
    su_audio_voice_pool_destroy(pool);
}

/* A ramp voice is stolen by a silent high-priority voice at quantum 2 and woken at quantum 5. */
static void wake_case(uint32_t mode, uint32_t release, float *out, uint32_t frames) {
    SuAudioVoicePool *pool = pool_make(4u, 1u, 0u, 1u);
    SuAudioLogicalDesc d = voice_desc(&ramp_clip, 1.0f, 0, 1u);
    SuAudioLogicalVoice ramp, blocker;
    d.policy.virtual_mode = mode;
    d.policy.release_frames = release;
    ramp = add(pool, &d);
    CHECK(su_audio_voice_pool_render(pool, out, 2u * Q) == SU_OK);
    d = voice_desc(&zero_clip, 1.0f, 9, 2u);
    blocker = add(pool, &d);
    CHECK(su_audio_voice_pool_render(pool, out + 4u * Q, 3u * Q) == SU_OK);
    CHECK(state_of(pool, ramp) == (uint32_t)(mode == SU_AUDIO_VIRTUAL_DISCARD ? SU_AUDIO_VOICE_DEAD : SU_AUDIO_VOICE_VIRTUAL));
    CHECK(su_audio_voice_stop(pool, blocker, 0u) == SU_OK);
    CHECK(su_audio_voice_pool_render(pool, out + 10u * Q, frames - 5u * Q) == SU_OK);
    su_audio_voice_pool_destroy(pool);
}

static void test_virtual_wake_positions(void) {
    static float out[16u * Q * 2u];
    uint32_t f, frames = 16u * Q, bad = 0u;
    float expected;
    /* advance: resume where the sound would be had it kept playing */
    wake_case(SU_AUDIO_VIRTUAL_ADVANCE, 0u, out, frames);
    for (f = 0u; f < frames; ++f) {
        expected = (f >= 2u * Q && f < 5u * Q) ? 0.0f : (float)(f + 1u) / 65536.0f;
        if (out[f * 2u] != expected || out[f * 2u + 1u] != expected) bad++;
    }
    CHECK(bad == 0u);
    /* pause: resume at the frame where it was stolen */
    wake_case(SU_AUDIO_VIRTUAL_PAUSE, 0u, out, frames);
    bad = 0u;
    for (f = 5u * Q; f < frames; ++f)
        if (out[f * 2u] != (float)(f - 3u * Q + 1u) / 65536.0f) bad++;
    CHECK(bad == 0u);
    /* restart: from frame 0 */
    wake_case(SU_AUDIO_VIRTUAL_RESTART, 0u, out, frames);
    bad = 0u;
    for (f = 5u * Q; f < frames; ++f)
        if (out[f * 2u] != (float)(f - 5u * Q + 1u) / 65536.0f) bad++;
    CHECK(bad == 0u);
    /* discard: never heard again */
    wake_case(SU_AUDIO_VIRTUAL_DISCARD, 0u, out, frames);
    bad = 0u;
    for (f = 2u * Q; f < frames; ++f) if (out[f * 2u] != 0.0f) bad++;
    CHECK(bad == 0u);
    /* advance with a 64-frame fade: exact ramp envelopes and exact position after the fade */
    wake_case(SU_AUDIO_VIRTUAL_ADVANCE, 64u, out, frames);
    bad = 0u;
    for (f = 0u; f < 64u; ++f) {
        float tail = (float)(2u * Q + f + 1u) / 65536.0f * ((float)(63u - f) / 64.0f);
        float wake = (float)(5u * Q + f + 1u) / 65536.0f * ((float)f / 64.0f);
        if (out[(2u * Q + f) * 2u] != tail) bad++;
        if (out[(5u * Q + f) * 2u] != wake) bad++;
    }
    for (f = 5u * Q + 64u; f < frames; ++f) if (out[f * 2u] != (float)(f + 1u) / 65536.0f) bad++;
    CHECK(bad == 0u);
}

static float max_step(const float *out, uint32_t frames) {
    uint32_t f;
    float step = 0.0f;
    for (f = 1u; f < frames; ++f) {
        float d = fabsf(out[f * 2u] - out[(f - 1u) * 2u]);
        if (d > step) step = d;
    }
    return step;
}

static void test_fade_continuity(void) {
    static float out[12u * Q * 2u];
    SuAudioVoicePool *pool = pool_make(4u, 1u, 0u, 1u);
    SuAudioLogicalDesc d = voice_desc(&dc_clip, 1.0f, 0, 1u);
    SuAudioLogicalVoice dc, blocker;
    d.policy.release_frames = 100u;
    d.policy.flags = SU_AUDIO_VOICE_LOOP;
    dc = add(pool, &d);
    CHECK(su_audio_voice_pool_render(pool, out, 2u * Q) == SU_OK);
    d = voice_desc(&zero_clip, 1.0f, 9, 2u);
    blocker = add(pool, &d);
    CHECK(su_audio_voice_pool_render(pool, out + 4u * Q, 4u * Q) == SU_OK);
    CHECK(su_audio_voice_stop(pool, blocker, 0u) == SU_OK);
    CHECK(su_audio_voice_pool_render(pool, out + 12u * Q, 4u * Q) == SU_OK);
    /* 0.5 over 100 frames: no step above one envelope increment */
    CHECK(max_step(out, 10u * Q) <= 0.5f / 100.0f + 1e-6f);
    CHECK(out[(2u * Q) * 2u] > 0.49f);
    CHECK(out[(3u * Q) * 2u] == 0.0f);
    CHECK(out[(9u * Q) * 2u] == 0.5f);
    /* owner stop release */
    CHECK(su_audio_voice_stop(pool, dc, 50u) == SU_OK);
    CHECK(state_of(pool, dc) == SU_AUDIO_VOICE_STOPPING);
    CHECK(su_audio_voice_pool_render(pool, out, 2u * Q) == SU_OK);
    CHECK(max_step(out, 2u * Q) <= 0.5f / 50.0f + 1e-6f);
    CHECK(out[49u * 2u] == 0.0f);
    CHECK(state_of(pool, dc) == SU_AUDIO_VOICE_DEAD);
    su_audio_voice_pool_destroy(pool);
}

static void test_one_shot_expiry(void) {
    SuAudioVoicePool *pool = pool_make(4u, 1u, 0u, 0u);
    SuAudioLogicalDesc d = voice_desc(&short_clip, 0.1f, 0, 1u);
    SuAudioLogicalVoice shot, loop, blocker;
    SuAudioVoiceStats stats;
    uint32_t f, bad = 0u;
    d = voice_desc(&zero_clip, 1.0f, 9, 0u);
    d.policy.flags = SU_AUDIO_VOICE_LOOP;
    blocker = add(pool, &d);
    d = voice_desc(&short_clip, 0.1f, 0, 1u);
    shot = add(pool, &d);
    d = voice_desc(&dc_clip, 0.1f, 0, 2u);
    d.policy.flags = SU_AUDIO_VOICE_LOOP;
    d.policy.wake_deadline_frames = 3u * Q;
    loop = add(pool, &d);
    render(pool, 4u * Q);
    CHECK(state_of(pool, shot) == SU_AUDIO_VOICE_DEAD);
    CHECK(state_of(pool, loop) == SU_AUDIO_VOICE_DEAD);
    su_audio_voice_pool_stats(pool, &stats);
    CHECK(stats.expired == 2u);
    CHECK(stats.reasons[SU_AUDIO_REASON_EXPIRED_END] == 1u);
    CHECK(stats.reasons[SU_AUDIO_REASON_EXPIRED_DEADLINE] == 1u);
    CHECK(su_audio_voice_stop(pool, blocker, 0u) == SU_OK);
    render(pool, 4u * Q);
    for (f = 0u; f < 8u * Q; ++f) if (scratch[f] != 0.0f) bad++;
    CHECK(bad == 0u); /* the expired one-shot is not replayed */
    /* a one-shot that plays to its end finishes */
    CHECK(su_audio_voice_add(pool, &(SuAudioLogicalDesc){0}, &shot) == SU_E_INVALID_ARGUMENT);
    CHECK(shot == 0u);
    d = voice_desc(&short_clip, 1.0f, 0, 3u);
    shot = add(pool, &d);
    render(pool, 3u * Q);
    CHECK(scratch[0] == 0.25f && scratch[255u * 2u] == 0.25f && scratch[256u * 2u] == 0.0f);
    CHECK(state_of(pool, shot) == SU_AUDIO_VOICE_DEAD);
    su_audio_voice_pool_destroy(pool);
}

static void test_exhaustion_and_transition_reserve(void) {
    SuAudioVoicePool *pool = pool_make(4u, 2u, 0u, 1u);
    SuAudioLogicalDesc d = voice_desc(&dc_clip, 0.5f, 0, 1u);
    SuAudioLogicalVoice a, b, c, e, refused = 1u;
    SuAudioVoiceStats stats;
    d.policy.release_frames = 200u; /* longer than a quantum */
    d.policy.flags = SU_AUDIO_VOICE_LOOP;
    a = add(pool, &d);
    d.trigger_seq = 2u;
    b = add(pool, &d);
    render(pool, Q);
    d = voice_desc(&dc_clip, 0.5f, 5, 3u);
    d.policy.flags = SU_AUDIO_VOICE_LOOP;
    c = add(pool, &d);
    d.trigger_seq = 4u;
    e = add(pool, &d);
    CHECK(su_audio_voice_add(pool, &d, &refused) == SU_E_RESOURCE_LIMIT);
    CHECK(refused == 0u);
    render(pool, Q);
    /* only one fade tail: one steal now, the second promotion is deferred */
    su_audio_voice_pool_stats(pool, &stats);
    CHECK(stats.refused == 1u);
    CHECK(stats.transitions == 1u);
    CHECK(stats.deferred == 1u);
    CHECK(state_of(pool, c) == SU_AUDIO_VOICE_PHYSICAL);
    CHECK(reason_of(pool, e) == SU_AUDIO_REASON_TRANSITION_FULL);
    CHECK(state_of(pool, a) == SU_AUDIO_VOICE_PHYSICAL);
    CHECK(state_of(pool, b) == SU_AUDIO_VOICE_VIRTUAL);
    render(pool, Q); /* tail still active (200 > 128) */
    CHECK(state_of(pool, e) == SU_AUDIO_VOICE_VIRTUAL);
    render(pool, Q);
    CHECK(state_of(pool, e) == SU_AUDIO_VOICE_PHYSICAL);
    CHECK(state_of(pool, a) == SU_AUDIO_VOICE_VIRTUAL);
    su_audio_voice_pool_stats(pool, &stats);
    CHECK(stats.transition_peak == 1u);
    CHECK(stats.physical_peak == 2u);
    CHECK(stats.stolen == 2u);
    CHECK(stats.virtual_peak == 2u);
    su_audio_voice_pool_destroy(pool);
}

static uint64_t scenario(const uint32_t *chunks, uint32_t chunk_count, float *out, uint32_t frames) {
    SuAudioVoicePool *pool = pool_make(64u, 8u, 1u, 4u);
    uint32_t i, done = 0u, k = 0u;
    uint64_t hash = 1469598103934665603u;
    SuAudioLogicalVoice v[32];
    for (i = 0u; i < 32u; ++i) {
        SuAudioLogicalDesc d = voice_desc(i & 1u ? &ramp_clip : &dc_clip, 0.05f + 0.01f * (float)(i % 7u),
                                          (int32_t)(i % 3u), i);
        d.pan = (float)((int)(i % 5u) - 2) * 0.5f;
        d.policy.virtual_mode = i % 3u;
        d.policy.release_frames = 32u + i;
        d.policy.flags = (i % 4u ? SU_AUDIO_VOICE_LOOP : 0u) | (i == 5u ? SU_AUDIO_VOICE_HERO : 0u);
        d.emitter = i % 6u;
        d.policy.physical_limit = i % 2u ? 3u : 0u;
        d.policy.limit_scope = SU_AUDIO_LIMIT_EMITTER;
        v[i] = add(pool, &d);
    }
    while (done < frames) {
        uint32_t n = chunks[k++ % chunk_count];
        if (n > frames - done) n = frames - done;
        /* presentation updates happen at fixed frame positions independent of chunking */
        CHECK(su_audio_voice_pool_render(pool, out + (size_t)done * 2u, n) == SU_OK);
        done += n;
    }
    (void)v;
    for (i = 0u; i < frames * 2u; ++i) {
        uint32_t bits;
        memcpy(&bits, &out[i], sizeof bits);
        hash = (hash ^ bits) * 1099511628211u;
    }
    su_audio_voice_pool_destroy(pool);
    return hash;
}

static void test_chunk_independence(void) {
    static float a[4096u * 2u], b[4096u * 2u];
    static const uint32_t whole[] = {4096u};
    static const uint32_t odd[] = {1u, 127u, 129u, 300u, 7u};
    uint64_t ha = scenario(whole, 1u, a, 4096u);
    uint64_t hb = scenario(odd, 5u, b, 4096u);
    CHECK(ha == hb);
    CHECK(memcmp(a, b, sizeof a) == 0);
    CHECK(ha == scenario(whole, 1u, a, 4096u));
    printf("voice policy trace hash %016llx\n", (unsigned long long)ha);
}

static void test_cost(void) {
    SuAudioVoicePool *pool = pool_make(1024u, 64u, 4u, 16u);
    static float out[48000u * 2u];
    SuAudioLogicalVoice v[1024];
    SuAudioVoiceStats stats;
    uint32_t i, q;
    clock_t start;
    double ms;
    for (i = 0u; i < 1024u; ++i) {
        SuAudioLogicalDesc d = voice_desc(&ramp_clip, 0.001f, (int32_t)(i % 4u), i);
        d.policy.flags = SU_AUDIO_VOICE_LOOP;
        d.policy.release_frames = 64u;
        d.emitter = i % 32u;
        d.policy.physical_limit = 4u;
        d.policy.limit_scope = SU_AUDIO_LIMIT_EMITTER;
        v[i] = add(pool, &d);
    }
    start = clock();
    for (q = 0u; q < 48000u / Q; ++q) {
        for (i = 0u; i < 1024u; ++i)
            CHECK(su_audio_voice_set_audibility(pool, v[i], 1.0f, (float)((i * 7u + q * 13u) % 200u)) == SU_OK);
        CHECK(su_audio_voice_pool_render(pool, out + (size_t)q * Q * 2u, Q) == SU_OK);
    }
    ms = (double)(clock() - start) * 1000.0 / (double)CLOCKS_PER_SEC;
    su_audio_voice_pool_stats(pool, &stats);
    CHECK(stats.physical_peak <= 64u);
    CHECK(stats.transition_peak <= 16u);
    printf("voice policy cost: 1024 logical / 64 physical / 16 fade tails, 48,000 frames, audibility updated "
           "every quantum: %.2f ms CPU (%llu steals, %llu wakes, %llu deferred); payload %llu bytes\n",
           ms, (unsigned long long)stats.stolen, (unsigned long long)stats.woken,
           (unsigned long long)stats.deferred, (unsigned long long)stats.runtime_bytes);
    su_audio_voice_pool_destroy(pool);
}

int main(void) {
    synthesize();
    test_validation();
    test_nearest_quiet_vs_far_loud();
    test_priority_and_hero_reservation();
    test_equal_score_order();
    test_scope_limits();
    test_virtual_wake_positions();
    test_fade_continuity();
    test_one_shot_expiry();
    test_exhaustion_and_transition_reserve();
    test_chunk_independence();
    test_cost();
    printf("audio voice policy: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
