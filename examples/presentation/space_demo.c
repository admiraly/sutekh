/* Presentation demo: a headless Sutekh world (sutekh/host.h) with fixed-point (IR 0.3, Q32.32) ships, fed by the
 * native input layer (sutekh/input.h), drawn by the Vulkan renderer (sutekh/render.h) and heard through the audio
 * mixer (sutekh/audio.h).
 *
 * The world runs at 120 Hz on the fixed-step clock (sutekh/clock.h); frames are drawn whenever the swapchain
 * allows, interpolating every ship between the last two ticks with the clock's alpha. Each tick, the device
 * sample of the frame (split over the frame's ticks) goes through the input map to a fixed-layout record, whose
 * axes are written to the player ship's input fields with su_host_input_push. Presentation only reads: ship
 * positions (Q32.32, used directly as camera-relative world positions) and rotations come from su_host_read_i64;
 * nothing presentation computes reaches the world. Gate crossings are events emitted by the flight system, read
 * per tick with su_host_events_for_tick; they flash the gate, burst effects and play a sound once each (rollback
 * re-emits them with the same sequence numbers, and the audio event cursor does not replay them).
 *
 * The scene is a procedural pack (demo_content.c: ship, gate, station, asteroids with two LODs) uploaded
 * asynchronously, a debris field of instanced asteroids, engine ribbon trails, boost and gate effects, a HUD,
 * and optionally a local glTF/GLB file or cooked pack (--model) loaded and cooked on a background thread.
 *
 * Usage (from the repository root):
 *   space_demo                         window, devices, until closed
 *   space_demo --frames N | --seconds S   stop after N frames or S seconds
 *   space_demo --script                scripted input instead of devices (unattended runs)
 *   space_demo --offscreen out.ppm     no window: render --frames frames (default 240) at a fixed 60 Hz frame
 *                                      time with scripted input, write the last frame, and check every tick
 *                                      against a shadow world that has no renderer (exit 1 on any difference);
 *                                      the camera is a fixed overview
 *   space_demo --headless              no renderer at all
 *   --expect-digest HEX                fail unless the final canonical digest is HEX
 *   --model FILE                       a .gltf/.glb (cooked in the background) or .supak to place in the scene
 *   --model-scale S                    its uniform scale (default 1)
 *   --debris N                         asteroids in the debris field (default 3000, 0 for none)
 *   --world-offset KM                  shift the whole presentation KM kilometres along +x (camera-relative
 *                                      rendering keeps it stable; the simulation is unchanged)
 *   --no-audio                         no mixer (default: device in a window, offline mixer otherwise)
 *   --no-env                           paint materials (the gate rings) take ambient from the procedural sky instead of a baked cube
 *   --no-bloom, --no-glare             turn off the HDR bloom (default on: emissive gates, engine trails, flares) or the
 *                                      sun's glare streaks
 *   --auto-exposure                    stateless automatic exposure (SU_POST_AUTO_EXPOSURE)
 *   --legacy                           draw with the first-slice debug renderer (su_render_frame)
 *   --pipeline-cache FILE              persist the pipeline cache there
 *   options: --no-vsync, --validation, --map file, --width W --height H, --exercise-window (resize, minimize,
 *            restore and resize back during the run, to drive swapchain recreation), --rollback-check (every
 *            8 ticks, so every tick is re-simulated once: restore the state of 8 ticks ago in place, re-push the
 *            recorded input records, re-simulate, and require the same fast digest and events)
 * Offscreen runs, and headless runs with --script, use a fixed 60 Hz frame time, so they are reproducible.
 * Exit status: 0 success, 1 failure, 3 presentation unavailable (the simulation still ran headless). */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demo_content.h"
#include "sutekh/asset.h"
#include "sutekh/audio.h"
#include "sutekh/clock.h"
#include "sutekh/host.h"
#include "sutekh/input.h"
#include "sutekh/render.h"

#define MAX_SHIPS 12u
#define TICK_HZ 120u
#define GATE_EVENT 1u
#define RB_DEPTH 8u             /* --rollback-check: ticks re-simulated per drill */
#define RB_SNAP_BYTES 65536u    /* one snapshot of this 12-ship world, event log included */
#define MAX_DEBRIS 20000u
#define TRAIL_POINTS 96u
#define AUDIO_RATE 48000u

/* Field ids of examples/presentation/flight.capsule.json (q32 fields use id and id + 1). */
enum {
    F_POS_X = 1, F_POS_Y = 3, F_POS_Z = 5, F_VEL_X = 7, F_VEL_Y = 9, F_VEL_Z = 11,
    F_ROT_W = 13, F_ROT_X = 15, F_ROT_Y = 17, F_ROT_Z = 19, F_DRAG = 21, F_SPRING = 23,
    F_IN_PITCH = 25, F_IN_YAW = 26, F_IN_ROLL = 27, F_IN_THRUST = 28
};

typedef struct Ship {
    SuWorldPos w;        /* Q32.32, as committed */
    SuVec3f pos;         /* float, relative to the world origin (debug drawing and the camera rig) */
    SuQuatf rot;
    SuVec3f vel;
} Ship;

typedef struct Demo {
    SuHostWorld *w;
    SuHostWorld *shadow;      /* --offscreen: same inputs, never drawn */
    uint64_t ents[MAX_SHIPS];
    uint32_t n;
    Ship prev[MAX_SHIPS];
    Ship cur[MAX_SHIPS];
    SuInputMapper mapper;
    SuInputFields fields;
    SuInputWin32 *input;
    SuRenderer *r;
    HWND hwnd;
    int quit;
    int free_cam;
    int fixed_cam;            /* --offscreen: a fixed overview camera */
    int legacy;
    int no_bloom;
    int no_env;                /* --no-env: paint materials take ambient from the procedural sky */
    SuRenderHandle env;        /* image-based ambient baked from a brighter sky than the visible one */
    int auto_exposure;
    int no_glare;
    uint64_t frames_drawn;
    uint64_t frames_paused;   /* the frame call returned SU_E_RESOURCE_LIMIT (minimized or out of date) */
    uint64_t frames_failed;
    uint32_t last_buttons;
    double gate_flash;        /* seconds left */
    uint64_t gate_events;
    uint64_t shadow_mismatch;
    int rollback;
    uint8_t rb_snap[RB_DEPTH][RB_SNAP_BYTES];
    size_t rb_len[RB_DEPTH];
    uint64_t rb_seq[RB_DEPTH];
    SuInputRecord rb_rec[RB_DEPTH];
    uint64_t rollbacks;
    uint64_t rb_mismatch;
    uint64_t rb_events;      /* events compared across drills */
    double rb_ms_sum;
    double step_ms_sum;
    uint64_t step_count;
    /* presentation content */
    SuWorldPos origin;        /* --world-offset */
    uint8_t *pack_bytes;
    SuAssetPak *pack;
    SuRenderPak rpak;
    SuRenderHandle m_ship, m_gate, m_station, m_rock;
    SuRenderObject *debris;
    uint32_t n_debris;
    SuAssetLoader *loader;
    SuAssetPak *user_asset;
    SuRenderPak user_pak;
    int user_state;           /* 0 none, 1 loading, 2 uploading, 3 ready, 4 failed */
    float user_scale;
    uint64_t user_frames;     /* frames drawn while uploading */
    uint64_t user_bytes0;     /* upload_bytes_total when the upload started */
    double user_t0;
    SuTrail trail[MAX_SHIPS];
    SuTrailPoint trail_pts[MAX_SHIPS][TRAIL_POINTS];
    double time_s;            /* presentation time */
    /* audio */
    SuAudioMixer *mixer;
    SuAudioDevice *adev;
    SuAudioClip *clip_engine;
    SuAudioClip *clip_chime;
    SuAudioVoice engine_voice;
    SuAudioEventCursor cursor;
    uint64_t gate_sounds;
    uint64_t replayed_events;  /* re-emitted events the cursor let through (must stay 0) */
    float audio_buf[AUDIO_RATE / 10u * 2u];
    /* per-frame timings for the end report (observations) */
    float *t_gpu;
    float *t_cpu;
    uint32_t t_n;
    uint32_t t_cap;
} Demo;

static Demo g;

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long n;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot read %s (run from the repository root)\n", path);
        exit(1);
    }
    b = malloc((size_t)n + 1u);
    if (b == NULL || fread(b, 1, (size_t)n, f) != (size_t)n) {
        exit(1);
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static double now_s(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

/* Exact Q32.32 of num / den for small integers. */
static int64_t q32(int64_t num, int64_t den) {
    return num * (INT64_C(1) << 32) / den;
}

static void put_q32(SuHostValue *v, uint32_t *n, uint32_t field, int64_t raw) {
    v[*n].field_id = field;
    v[*n].value = (uint32_t)((uint64_t)raw & 0xFFFFFFFFu);
    v[*n + 1u].field_id = field + 1u;
    v[*n + 1u].value = (uint32_t)((uint64_t)raw >> 32);
    *n += 2u;
}

/* Ship i: the player (0) starts behind the gate; the others orbit the origin on ellipses in tilted planes. */
static uint64_t spawn_ship(SuHostWorld *w, uint32_t i) {
    SuHostValue v[32];
    SuHostError e;
    uint64_t h = 0;
    uint32_t n = 0;
    int64_t r = 10 + 3 * (int64_t)i;
    put_q32(v, &n, F_ROT_W, q32(1, 1));
    put_q32(v, &n, F_ROT_X, 0);
    put_q32(v, &n, F_ROT_Y, 0);
    put_q32(v, &n, F_ROT_Z, 0);
    put_q32(v, &n, F_SPRING, q32(1, 16));    /* omega = 1/4 rad/s */
    if (i == 0u) {
        put_q32(v, &n, F_POS_X, 0);
        put_q32(v, &n, F_POS_Y, 0);
        put_q32(v, &n, F_POS_Z, q32(30, 1));
        put_q32(v, &n, F_VEL_X, 0);
        put_q32(v, &n, F_VEL_Y, 0);
        put_q32(v, &n, F_VEL_Z, 0);
        put_q32(v, &n, F_DRAG, q32(1, 2));
    } else {
        /* Start at radius r in direction (c, 0, s) of a Pythagorean triple (exact in Q32.32), moving
         * perpendicular to it at r * omega * k / 4 (k = 4: circle, 3: ellipse) with a vertical tilt. */
        static const int64_t trip[6][3] = {{3, 4, 5}, {5, 12, 13}, {8, 15, 17}, {7, 24, 25}, {20, 21, 29}, {12, 35, 37}};
        const int64_t *t = trip[i % 6u];
        int64_t cx = i % 4u < 2u ? t[0] : -t[0];
        int64_t cz = i % 2u ? t[1] : -t[1];
        int64_t k = (i % 2u) ? 4 : 3;
        put_q32(v, &n, F_POS_X, q32(r * cx, t[2]));
        put_q32(v, &n, F_POS_Y, 0);
        put_q32(v, &n, F_POS_Z, q32(r * cz, t[2]));
        put_q32(v, &n, F_VEL_X, q32(-r * cz * k, t[2] * 16));
        put_q32(v, &n, F_VEL_Y, q32(r * k * (int64_t)(i % 3u), 32));
        put_q32(v, &n, F_VEL_Z, q32(r * cx * k, t[2] * 16));
        put_q32(v, &n, F_DRAG, 0);
    }
    if (su_host_spawn(w, v, n, i, &h, &e) != SU_OK) {
        fprintf(stderr, "spawn: %s\n", e.message);
        exit(1);
    }
    return h;
}

static SuHostWorld *make_world(void) {
    static uint8_t *cap = NULL;
    static uint8_t *lir = NULL;
    static size_t cap_len = 0;
    static size_t lir_len = 0;
    SuHostCapsule c;
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError e;
    if (cap == NULL) {
        cap = slurp("examples/presentation/flight.capsule.json", &cap_len);
        lir = slurp("examples/presentation/flight.lir.json", &lir_len);
    }
    c.capsule_json = cap;
    c.capsule_len = cap_len;
    c.lir_json = lir;
    c.lir_len = lir_len;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.capsule_count = 1;
    d.capsules = &c;
    d.max_entities = MAX_SHIPS;
    d.tick_rate_hz = TICK_HZ;
    d.event_log_max = 256;
    if (su_host_world_create(&d, &w, &e) != SU_OK) {
        fprintf(stderr, "world: %s\n", e.message);
        exit(1);
    }
    return w;
}

static float rawf(int64_t raw) {
    return (float)((double)raw / 4294967296.0);
}

/* Reads every ship's committed state (presentation's only view of the world). */
static void read_ships(Ship *out) {
    static const uint32_t ids[10] = {F_POS_X, F_POS_Y, F_POS_Z, F_ROT_W, F_ROT_X, F_ROT_Y, F_ROT_Z, F_VEL_X, F_VEL_Y, F_VEL_Z};
    int64_t col[10][MAX_SHIPS];
    uint32_t k;
    uint32_t i;
    for (k = 0; k < 10u; k++) {
        if (su_host_read_i64(g.w, ids[k], 0, g.n, col[k]) != SU_OK) {
            fprintf(stderr, "read_i64 failed\n");
            exit(1);
        }
    }
    for (i = 0; i < g.n; i++) {
        out[i].w.x = col[0][i];
        out[i].w.y = col[1][i];
        out[i].w.z = col[2][i];
        out[i].pos.x = rawf(col[0][i]);
        out[i].pos.y = rawf(col[1][i]);
        out[i].pos.z = rawf(col[2][i]);
        out[i].rot.w = rawf(col[3][i]);
        out[i].rot.x = rawf(col[4][i]);
        out[i].rot.y = rawf(col[5][i]);
        out[i].rot.z = rawf(col[6][i]);
        out[i].vel.x = rawf(col[7][i]);
        out[i].vel.y = rawf(col[8][i]);
        out[i].vel.z = rawf(col[9][i]);
    }
}

/* Deterministic scripted input for unattended runs: throttle, then pitch, yaw and roll phases; boost while
 * climbing (presentation only: the flight system has no boost). */
static void scripted_state(uint64_t tick, SuInputState *s) {
    uint64_t t = tick % 960u;
    memset(s, 0, sizeof *s);
    s->focused = 1;
    su_input_set_key(s, SU_KEY_W, t < 600u);
    su_input_set_key(s, SU_KEY_UP, t >= 240u && t < 300u);
    su_input_set_key(s, SU_KEY_RIGHT, t >= 360u && t < 420u);
    su_input_set_key(s, SU_KEY_E, t >= 480u && t < 540u);
    su_input_set_key(s, SU_KEY_S, t >= 700u && t < 760u);
    su_input_set_key(s, SU_KEY_LSHIFT, t >= 100u && t < 220u);
}

static void push_and_step(SuHostWorld *w, uint64_t tick, const SuInputRecord *rec) {
    SuHostInput writes[1 + SU_INPUT_MAX_AXES];
    uint32_t n = 0;
    uint64_t done = 0;
    SuHostError e;
    if (su_input_record_writes(rec, &g.fields, g.ents[0], writes, &n) != SU_OK ||
        su_host_input_push(w, tick, writes, n, &e) != SU_OK || su_host_step(w, 1, &done, &e) != SU_OK) {
        fprintf(stderr, "tick %llu failed: %s\n", (unsigned long long)tick, e.message);
        exit(1);
    }
}

/* Prediction/reconciliation drill: restore the state of RB_DEPTH ticks ago in place, re-push the recorded input
 * records, re-simulate all RB_DEPTH ticks in one step call, and require the same fast digest and the same events
 * (same sequence numbers) as before the rollback. The re-emitted events then go through the audio cursor, which
 * must not let any of them play again. */
static void rollback_check(uint64_t now_tick) {
    uint64_t base = now_tick - RB_DEPTH;
    uint32_t b = (uint32_t)(base % RB_DEPTH);
    SuHostEvent before[64];
    SuHostEvent after[64];
    uint32_t n_before = 0;
    uint32_t n_after = 0;
    uint64_t cur = 0;
    uint64_t fa = 0;
    uint64_t fb = 0;
    uint64_t done = 0;
    uint64_t k;
    uint32_t i;
    SuHostError e;
    double t0;
    (void)su_host_digest_fast(g.w, &fa);
    (void)su_host_events_read(g.w, g.rb_seq[b], before, 64, &n_before, &cur, NULL);
    t0 = now_s();
    if (su_host_snapshot_restore_inplace(g.w, g.rb_snap[b], g.rb_len[b], &e) != SU_OK) {
        fprintf(stderr, "rollback restore: %s\n", e.message);
        exit(1);
    }
    for (k = base; k < now_tick; k++) {
        SuHostInput writes[1 + SU_INPUT_MAX_AXES];
        uint32_t n = 0;
        if (su_input_record_writes(&g.rb_rec[k % RB_DEPTH], &g.fields, g.ents[0], writes, &n) != SU_OK ||
            su_host_input_push(g.w, k, writes, n, &e) != SU_OK) {
            fprintf(stderr, "rollback input: %s\n", e.message);
            exit(1);
        }
    }
    if (su_host_step(g.w, RB_DEPTH, &done, &e) != SU_OK || done != RB_DEPTH) {
        fprintf(stderr, "rollback step: %s\n", e.message);
        exit(1);
    }
    (void)su_host_digest_fast(g.w, &fb);
    g.rb_ms_sum += (now_s() - t0) * 1000.0;
    (void)su_host_events_read(g.w, g.rb_seq[b], after, 64, &n_after, &cur, NULL);
    g.rollbacks++;
    g.rb_events += n_after;
    if (fa != fb || n_before != n_after || memcmp(before, after, (size_t)n_before * sizeof before[0]) != 0) {
        g.rb_mismatch++;
        fprintf(stderr, "rollback at tick %llu: re-simulation differs\n", (unsigned long long)now_tick);
    }
    if (g.mixer != NULL) {
        for (i = 0; i < n_after; i++) {
            g.replayed_events += (uint64_t)su_audio_event_new(&g.cursor, &after[i]);
        }
    }
}

/* A committed gate event: flash, and a chime at the gate (once per event sequence number). */
static void on_event(const SuHostEvent *ev) {
    if (ev->kind != GATE_EVENT) {
        return;
    }
    g.gate_events++;
    g.gate_flash = 0.6;
    printf("tick %llu: ship %u crossed the gate\n", (unsigned long long)ev->tick, (unsigned)ev->a);
    if (g.mixer != NULL && su_audio_event_new(&g.cursor, ev)) {
        SuAudioVoiceDesc d;
        SuAudioVoice v;
        memset(&d, 0, sizeof d);
        d.clip = g.clip_chime;
        d.gain = 0.6f;
        d.flags = SU_AUDIO_SPATIAL;
        d.pos = g.origin;
        d.min_distance = 10.0f;
        d.priority = 2;
        if (su_audio_play(g.mixer, &d, &v) == SU_OK) {
            g.gate_sounds++;
        }
    }
}

/* Runs one tick: map the state, push the record, step, read events, and step the shadow world. */
static void tick(const SuInputState *s) {
    SuInputRecord rec;
    SuHostEvent ev[16];
    uint32_t nev = 0;
    uint64_t t = su_host_tick(g.w);
    double t0;
    uint32_t i;
    (void)su_input_mapper_tick(&g.mapper, s, t, &rec);
    /* Rising edge of camera_next (button 2) toggles the camera; presentation state only. */
    if ((rec.buttons & 4u) != 0u && (g.last_buttons & 4u) == 0u) {
        g.free_cam = !g.free_cam;
    }
    g.last_buttons = rec.buttons;
    memcpy(g.prev, g.cur, sizeof g.prev);
    if (g.rollback) {
        /* Keep the state before tick t, its event cursor and its input record for rollback_check. */
        uint32_t k = (uint32_t)(t % RB_DEPTH);
        if (su_host_snapshot_write(g.w, g.rb_snap[k], RB_SNAP_BYTES, &g.rb_len[k]) != SU_OK) {
            fprintf(stderr, "rollback snapshot does not fit\n");
            exit(1);
        }
        g.rb_seq[k] = su_host_event_next_seq(g.w);
        g.rb_rec[k] = rec;
    }
    t0 = now_s();
    push_and_step(g.w, t, &rec);
    g.step_ms_sum += (now_s() - t0) * 1000.0;
    g.step_count++;
    if (g.shadow != NULL) {
        uint64_t a = 0;
        uint64_t b = 0;
        push_and_step(g.shadow, t, &rec);
        (void)su_host_digest_fast(g.w, &a);
        (void)su_host_digest_fast(g.shadow, &b);
        g.shadow_mismatch += a != b;
    }
    read_ships(g.cur);
    (void)su_host_events_for_tick(g.w, t + 1u, ev, 16, &nev, NULL, NULL);
    for (i = 0; i < nev; i++) {
        on_event(&ev[i]);
    }
    if (g.rollback && (t + 1u) % RB_DEPTH == 0u) {
        rollback_check(t + 1u);
    }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g.input != NULL) {
        (void)su_input_win32_message(g.input, hwnd, msg, (uintptr_t)wp, (intptr_t)lp);
    }
    switch (msg) {
    case WM_SIZE:
        if (g.r != NULL) {
            su_render_resize(g.r, LOWORD(lp), HIWORD(lp));
        }
        return 0;
    case WM_CLOSE:
        g.quit = 1;
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            g.quit = 1;
        }
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static HWND make_window(uint32_t w, uint32_t h) {
    WNDCLASSW wc;
    RECT rc = {0, 0, (LONG)w, (LONG)h};
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"SutekhSpaceDemo";
    RegisterClassW(&wc);
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowExW(0, wc.lpszClassName, L"Sutekh presentation demo", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL,
                           wc.hInstance, NULL);
}

static uint32_t rgba(float r, float gg, float b) {
    return 0xFF000000u | ((uint32_t)(b * 255.0f) << 16) | ((uint32_t)(gg * 255.0f) << 8) | (uint32_t)(r * 255.0f);
}

static SuVec3f v3(float x, float y, float z) {
    SuVec3f r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}

/* Camera orientation from a view matrix (whose rotation rows are the camera axes in world space). */
static SuQuatf quat_from_view(SuMat4f v) {
    float r[3][3];
    float tr;
    SuQuatf q;
    /* r[row][col] of camera-to-world = view[col][row] (transpose) */
    r[0][0] = v.m[0];
    r[0][1] = v.m[1];
    r[0][2] = v.m[2];
    r[1][0] = v.m[4];
    r[1][1] = v.m[5];
    r[1][2] = v.m[6];
    r[2][0] = v.m[8];
    r[2][1] = v.m[9];
    r[2][2] = v.m[10];
    tr = r[0][0] + r[1][1] + r[2][2];
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (r[2][1] - r[1][2]) / s;
        q.y = (r[0][2] - r[2][0]) / s;
        q.z = (r[1][0] - r[0][1]) / s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        float s = sqrtf(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
        q.w = (r[2][1] - r[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (r[0][1] + r[1][0]) / s;
        q.z = (r[0][2] + r[2][0]) / s;
    } else if (r[1][1] > r[2][2]) {
        float s = sqrtf(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
        q.w = (r[0][2] - r[2][0]) / s;
        q.x = (r[0][1] + r[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (r[1][2] + r[2][1]) / s;
    } else {
        float s = sqrtf(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
        q.w = (r[1][0] - r[0][1]) / s;
        q.x = (r[0][2] + r[2][0]) / s;
        q.y = (r[1][2] + r[2][1]) / s;
        q.z = 0.25f * s;
    }
    return q;
}

/* Integer sum of two Q32.32 positions (the world offset plus a committed position). */
static SuWorldPos wadd(SuWorldPos a, SuWorldPos b) {
    a.x = (int64_t)((uint64_t)a.x + (uint64_t)b.x);
    a.y = (int64_t)((uint64_t)a.y + (uint64_t)b.y);
    a.z = (int64_t)((uint64_t)a.z + (uint64_t)b.z);
    return a;
}

/* Eye position of a view matrix: -R^T t. */
static SuVec3f eye_of(SuMat4f v) {
    SuVec3f t = v3(v.m[12], v.m[13], v.m[14]);
    return v3(-(v.m[0] * t.x + v.m[1] * t.y + v.m[2] * t.z), -(v.m[4] * t.x + v.m[5] * t.y + v.m[6] * t.z),
              -(v.m[8] * t.x + v.m[9] * t.y + v.m[10] * t.z));
}

static uint32_t lcg(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static float frand(uint32_t *s) {
    return (float)(lcg(s) & 0xFFFFu) / 65535.0f;
}

/* Debris: asteroids in a band around the course (presentation only). */
static void make_debris(uint32_t n) {
    uint32_t seed = 12345u;
    uint32_t i;
    g.debris = calloc(n > 0u ? n : 1u, sizeof *g.debris);
    g.n_debris = n;
    for (i = 0; i < n; i++) {
        SuRenderObject *o = &g.debris[i];
        float a = frand(&seed) * 6.2831853f;
        float rad = 70.0f + 120.0f * frand(&seed);
        float y = -30.0f + 60.0f * frand(&seed);
        float s = 0.6f + 3.5f * frand(&seed) * frand(&seed);
        SuVec3f axis = v3(frand(&seed) - 0.5f, frand(&seed) - 0.5f, frand(&seed) - 0.5f);
        o->pos = su_wpos_add(g.origin, v3(cosf(a) * rad, y, sinf(a) * rad));
        o->rot = su_quatf_axis_angle(axis, frand(&seed) * 6.2831853f);
        o->scale = s;
        o->tint[0] = o->tint[1] = o->tint[2] = 0.8f + 0.4f * frand(&seed);
        o->tint[3] = 1.0f;
        o->emissive = 1.0f;
        o->model = g.m_rock;
    }
}

static void setup_content(void) {
    size_t len = 0;
    char msg[256];
    g.pack_bytes = demo_build_pack(&len);
    if (g.pack_bytes == NULL || su_asset_pak_open(g.pack_bytes, len, 0, &g.pack, msg) != SU_OK) {
        fprintf(stderr, "procedural pack: %s\n", g.pack_bytes == NULL ? "build failed" : msg);
        exit(1);
    }
    if (su_render_upload_pak(g.r, g.pack, demo_id("model/ship"), &g.rpak) != SU_OK) {
        fprintf(stderr, "procedural pack upload: %s\n", su_render_last_error(g.r));
        exit(1);
    }
    g.m_ship = su_render_pak_handle(&g.rpak, demo_id("model/ship"));
    g.m_gate = su_render_pak_handle(&g.rpak, demo_id("model/gate"));
    g.m_station = su_render_pak_handle(&g.rpak, demo_id("model/station"));
    g.m_rock = su_render_pak_handle(&g.rpak, demo_id("model/rock"));
    printf("content: procedural pack %zu bytes, %u meshes, %u materials, %u models (uploading in the background)\n",
           len, (unsigned)g.rpak.meshes, (unsigned)g.rpak.materials, (unsigned)g.rpak.models);
}

/* Background model: poll the loader, then upload; the scene shows it once its meshes are READY. */
static void poll_user_model(void) {
    SuAssetLoadResult res;
    if (g.user_state == 2) {
        uint32_t st = su_render_pak_state(g.r, &g.user_pak);
        g.user_frames++;
        if (st != SU_RENDER_STATE_PENDING) {
            SuRenderStats rs;
            (void)su_render_stats(g.r, &rs);
            printf("model: %s after %llu frames, %.1f s, %llu bytes uploaded\n",
                   st == SU_RENDER_STATE_READY ? "every resource READY" : "an upload FAILED", (unsigned long long)g.user_frames,
                   now_s() - g.user_t0, (unsigned long long)(rs.upload_bytes_total - g.user_bytes0));
            g.user_state = st == SU_RENDER_STATE_READY ? 3 : 4;
        }
        return;
    }
    if (g.loader == NULL || g.user_state != 1 || !su_asset_loader_poll(g.loader, &res)) {
        return;
    }
    if (res.status != SU_OK) {
        printf("model: load failed (%s): %s\n", su_host_status_name(res.status), res.message);
        g.user_state = 4;
        return;
    }
    g.user_asset = res.pak;
    printf("model: loaded and validated in %.1f ms on the loader thread; %u resources\n", res.load_ms,
           (unsigned)su_asset_pak_entry_count(res.pak));
    if (su_render_upload_pak(g.r, g.user_asset, 0, &g.user_pak) != SU_OK) {
        printf("model: upload refused: %s\n", su_render_last_error(g.r));
        g.user_state = 4;
        return;
    }
    g.user_state = 2;
    {
        SuRenderStats rs;
        (void)su_render_stats(g.r, &rs);
        g.user_bytes0 = rs.upload_bytes_total;
        g.user_t0 = now_s();
    }
}

static SuRenderObject object(SuRenderHandle model, SuWorldPos pos, SuQuatf rot, float scale, float r, float gg, float b) {
    SuRenderObject o;
    memset(&o, 0, sizeof o);
    o.pos = pos;
    o.rot = rot;
    o.scale = scale;
    o.tint[0] = r;
    o.tint[1] = gg;
    o.tint[2] = b;
    o.tint[3] = 1.0f;
    o.emissive = 1.0f;
    o.model = model;
    return o;
}

/* The view matrix (demo-local float coordinates around the world origin) for this frame's camera mode. */
static SuMat4f camera_view(double dt, const Ship *player, SuChaseCamera *chase, SuFreeCamera *freec,
                           const SuInputRecord *axes) {
    if (g.fixed_cam) {
        return su_mat4f_look_at(v3(0.0f, 45.0f, 95.0f), v3(0, 0, 0), v3(0.0f, 1.0f, 0.0f));
    }
    if (g.free_cam) {
        SuVec3f mv;
        float k = (float)dt * 40.0f;
        mv.x = k * (float)axes->axes[4] / SU_INPUT_ONE;
        mv.y = k * (float)axes->axes[5] / SU_INPUT_ONE;
        mv.z = k * (float)axes->axes[3] / SU_INPUT_ONE;
        return su_free_camera_update(freec, mv, -(float)dt * 1.5f * (float)axes->axes[1] / SU_INPUT_ONE,
                                     (float)dt * 1.5f * (float)axes->axes[0] / SU_INPUT_ONE);
    }
    return su_chase_camera_update(chase, player->pos, player->rot, (float)dt);
}

/* The first-slice debug frame (--legacy), as before. */
static void draw_legacy(const Ship *s, SuMat4f view_m, uint32_t width, uint32_t height) {
    static SuRenderInstance inst[MAX_SHIPS + 1u];
    static SuRenderLineVertex lines[8192];
    SuDebugLines dl;
    SuRenderView view;
    SuVec3f c0 = {0, 0, 0};
    SuQuatf id = {0, 0, 0, 1};
    uint32_t i;
    uint32_t st;
    memset(&dl, 0, sizeof dl);
    dl.v = lines;
    dl.cap = 8192;
    for (i = 0; i < g.n; i++) {
        memset(&inst[i], 0, sizeof inst[i]);
        inst[i].mesh = SU_MESH_SHIP;
        inst[i].pos[0] = s[i].pos.x;
        inst[i].pos[1] = s[i].pos.y;
        inst[i].pos[2] = s[i].pos.z;
        inst[i].scale = i == 0u ? 4.0f : 3.0f;
        memcpy(inst[i].rot, &s[i].rot, sizeof inst[i].rot);
        inst[i].color[0] = i == 0u ? 1.0f : 0.3f + 0.05f * (float)i;
        inst[i].color[1] = i == 0u ? 0.6f : 0.5f;
        inst[i].color[2] = i == 0u ? 0.1f : 0.9f;
        inst[i].color[3] = 1.0f;
        su_debug_vector(&dl, s[i].pos, v3(s[i].vel.x * 0.5f, s[i].vel.y * 0.5f, s[i].vel.z * 0.5f), 0xFF40FF40u);
    }
    memset(&inst[g.n], 0, sizeof inst[g.n]);
    inst[g.n].mesh = SU_MESH_SPHERE;
    inst[g.n].scale = 2.0f;
    inst[g.n].rot[3] = 1.0f;
    inst[g.n].color[0] = inst[g.n].color[1] = 0.8f;
    inst[g.n].color[2] = 0.7f;
    inst[g.n].color[3] = 1.0f;
    su_debug_ellipse(&dl, c0, v3(6, 0, 0), v3(0, 6, 0), 64, g.gate_flash > 0.0 ? rgba(1, 1, 0.2f) : rgba(0.2f, 0.6f, 1));
    su_debug_plane(&dl, v3(0, -25, 0), v3(60, 0, 0), v3(0, 0, 60), 12, rgba(0.25f, 0.25f, 0.3f));
    su_debug_axes(&dl, c0, id, 4.0f);
    memset(&view, 0, sizeof view);
    view.view = view_m;
    view.proj = su_mat4f_perspective(1.0471976f, (float)width / (float)(height > 0u ? height : 1u), 0.1f, 1000.0f);
    view.clear[0] = 0.01f;
    view.clear[1] = 0.01f;
    view.clear[2] = 0.025f;
    view.clear[3] = 1.0f;
    view.light_dir = v3(-0.4f, -0.8f, -0.45f);
    view.ambient = 0.3f;
    st = su_render_frame(g.r, &view, inst, g.n + 1u, lines, dl.count);
    g.frames_drawn += st == SU_OK;
    g.frames_paused += st == SU_E_RESOURCE_LIMIT;
    if (st != SU_OK && st != SU_E_RESOURCE_LIMIT && ++g.frames_failed <= 3u) {
        fprintf(stderr, "frame failed: %s\n", su_render_last_error(g.r));
    }
}

/* Builds and draws the camera-relative frame from interpolated ship states. */
static void draw(float alpha, double dt, SuChaseCamera *chase, SuFreeCamera *freec, uint32_t width, uint32_t height,
                 const SuInputRecord *axes, double fps) {
    static SuRenderObject objs[MAX_SHIPS + 8u];
    static SuRenderObject *all = NULL;
    static SuRenderLineVertex lines[4096];
    static SuFxVertex fx[24576];
    static SuHudVertex hud_v[8192];
    static SuVec3f last_eye;
    static int have_eye = 0;
    SuDebugLines dl;
    SuFxBuffer fb;
    SuHud hud;
    SuRenderFrameDesc f;
    SuMat4f view_m;
    SuMat4f view_rot;
    SuVec3f eye;
    SuVec3f cam_vel = {0, 0, 0};
    Ship s[MAX_SHIPS];
    uint32_t i;
    uint32_t n_obj = 0;
    uint32_t st;
    float speed;
    for (i = 0; i < g.n; i++) {
        s[i].w = su_wpos_lerp(g.prev[i].w, g.cur[i].w, alpha);
        s[i].pos = su_vec3f_lerp(g.prev[i].pos, g.cur[i].pos, alpha);
        s[i].rot = su_quatf_nlerp(g.prev[i].rot, g.cur[i].rot, alpha);
        s[i].vel = su_vec3f_lerp(g.prev[i].vel, g.cur[i].vel, alpha);
    }
    view_m = camera_view(dt, &s[0], chase, freec, axes);
    if (g.legacy) {
        draw_legacy(s, view_m, width, height);
        return;
    }
    eye = eye_of(view_m);
    if (have_eye && dt > 0.0) {
        cam_vel = v3((eye.x - last_eye.x) / (float)dt, (eye.y - last_eye.y) / (float)dt, (eye.z - last_eye.z) / (float)dt);
    }
    last_eye = eye;
    have_eye = 1;
    memset(&f, 0, sizeof f);
    f.struct_size = sizeof f;
    f.camera.pos = su_wpos_add(g.origin, eye);
    f.camera.rot = quat_from_view(view_m);
    f.camera.fov_y = 1.0471976f;
    f.camera.near_z = 0.1f;
    view_rot = su_render_camera_view(&f.camera);
    f.light.sun_dir = v3(-0.4f, -0.8f, -0.45f);
    f.light.sun_color = v3(1.0f, 0.96f, 0.9f);
    f.light.sun_intensity = 3.0f;
    f.light.sky_zenith = v3(0.004f, 0.005f, 0.012f);
    f.light.sky_horizon = v3(0.01f, 0.009f, 0.016f);
    f.light.sky_nadir = v3(0.002f, 0.002f, 0.003f);
    f.light.ambient = 6.0f; /* object ambient from the (dark) sky colours */
    f.light.sun_disc = 0.01f;
    f.light.up = v3(0, 1, 0);
    f.light.exposure = 1.0f;
    f.light.tonemap = SU_TONEMAP_ACES;
    if (g.env == 0u && !g.no_env) {
        /* a cube baked from a nebula-like sky: warm horizon, cold zenith and the sun glow (only paint materials use it) */
        SuRenderLighting nb = f.light;
        nb.sky_zenith = v3(0.05f, 0.08f, 0.2f);
        nb.sky_horizon = v3(0.35f, 0.18f, 0.22f);
        nb.sky_nadir = v3(0.02f, 0.02f, 0.04f);
        nb.sun_intensity = 6.0f;
        if (su_render_env_from_sky(g.r, 64, &nb, &g.env) != SU_OK) {
            g.env = 0;
            g.no_env = 1;
        }
    }
    f.env = g.no_env ? 0u : g.env;
    f.env_intensity = 1.0f;

    /* objects: ships, gate, station, the optional model; then the debris field */
    for (i = 0; i < g.n; i++) {
        float sc = i == 0u ? 4.0f : 3.0f;
        objs[n_obj++] = i == 0u ? object(g.m_ship, wadd(g.origin, s[i].w), s[i].rot, sc, 1.0f, 0.55f, 0.2f)
                                : object(g.m_ship, wadd(g.origin, s[i].w), s[i].rot, sc, 0.35f + 0.05f * (float)i, 0.55f,
                                         0.95f);
    }
    objs[n_obj] = object(g.m_gate, g.origin, (SuQuatf){0, 0, 0, 1}, 1.0f, 1, 1, 1);
    objs[n_obj].emissive = g.gate_flash > 0.0 ? 1.0f + 6.0f * (float)g.gate_flash : 1.0f;
    n_obj++;
    objs[n_obj++] = object(g.m_station, su_wpos_add(g.origin, v3(0.0f, 40.0f, -260.0f)),
                           su_quatf_axis_angle(v3(0, 0, 1), (float)(g.time_s * 0.05)), 1.0f, 1, 1, 1);
    if (g.user_state >= 2 && g.user_state != 4) {
        objs[n_obj++] = object(g.user_pak.model, su_wpos_add(g.origin, v3(0.0f, -15.0f, -70.0f)), (SuQuatf){0, 0, 0, 1},
                               g.user_scale, 1, 1, 1);
    }
    if (all == NULL) {
        all = malloc((MAX_SHIPS + 8u + MAX_DEBRIS) * sizeof *all);
    }
    memcpy(all, objs, n_obj * sizeof *objs);
    memcpy(all + n_obj, g.debris, g.n_debris * sizeof *g.debris);
    f.objects = all;
    f.object_count = n_obj + g.n_debris;

    /* debug lines around the world origin: velocity vectors and a reference grid */
    memset(&dl, 0, sizeof dl);
    dl.v = lines;
    dl.cap = 4096;
    for (i = 0; i < g.n; i++) {
        su_debug_vector(&dl, s[i].pos, v3(s[i].vel.x * 0.5f, s[i].vel.y * 0.5f, s[i].vel.z * 0.5f), 0xFF40FF40u);
    }
    su_debug_plane(&dl, v3(0, -25, 0), v3(60, 0, 0), v3(0, 0, 60), 12, rgba(0.18f, 0.18f, 0.22f));
    f.lines = lines;
    f.line_vertex_count = dl.count;
    f.debug_origin = g.origin;

    /* effects: engine trails from the interpolated tails, boost flare, gate burst */
    memset(&fb, 0, sizeof fb);
    fb.v = fx;
    fb.cap = sizeof fx / sizeof fx[0];
    for (i = 0; i < g.n; i++) {
        float sc = i == 0u ? 4.0f : 3.0f;
        SuVec3f tail = su_quatf_rotate(s[i].rot, v3(0.0f, 0.0f, 0.5f * sc));
        SuWorldPos tw = su_wpos_add(wadd(g.origin, s[i].w), tail);
        float head[4] = {1.0f * 2.0f, 0.5f * 2.0f, 0.15f * 2.0f, 1.0f};
        float tl[4] = {0.3f, 0.1f, 0.6f, 0.0f};
        if (i != 0u) {
            head[0] = 0.3f * 2.0f;
            head[1] = 0.6f * 2.0f;
            head[2] = 1.0f * 2.0f;
        }
        su_trail_push(&g.trail[i], tw, g.time_s);
        su_trail_build(&g.trail[i], g.time_s, f.camera.pos, 0.5f * sc, 0.05f, head, tl, &fb);
        if (i == 0u && (g.last_buttons & 1u) != 0u) {
            float flare[4] = {3.0f, 1.6f, 0.6f, 1.0f};
            su_fx_billboard(&fb, su_wpos_delta(tw, f.camera.pos), 1.2f + 0.3f * sinf((float)g.time_s * 40.0f),
                            (float)g.time_s * 3.0f, flare, view_rot);
        }
    }
    if (g.gate_flash > 0.0) {
        float c[4] = {0.4f, 0.9f, 2.5f, (float)g.gate_flash};
        for (i = 0; i < 24u; i++) {
            float a = (float)i * 6.2831853f / 24.0f;
            float rr = 6.0f + (0.6f - (float)g.gate_flash) * 10.0f;
            su_fx_billboard(&fb, su_wpos_delta(su_wpos_add(g.origin, v3(cosf(a) * rr, sinf(a) * rr, 0.0f)), f.camera.pos),
                            1.5f, a, c, view_rot);
        }
    }
    /* the sun: a very bright core with glare streaks, when it is in front of the camera */
    if (!g.no_glare) {
        SuVec3f toward_sun = v3(0.4f, 0.8f, 0.45f); /* opposite of the light direction */
        SuVec3f cam_fwd = su_quatf_rotate(f.camera.rot, v3(0.0f, 0.0f, -1.0f));
        if (cam_fwd.x * toward_sun.x + cam_fwd.y * toward_sun.y + cam_fwd.z * toward_sun.z > 0.3f) {
            float sun[4] = {60.0f, 56.0f, 48.0f, 1.0f};
            SuVec3f p = v3(toward_sun.x * 4000.0f, toward_sun.y * 4000.0f, toward_sun.z * 4000.0f);
            su_fx_glare(&fb, p, 60.0f, 700.0f, sun, view_rot);
        }
    }
    f.fx = fx;
    f.fx_vertex_count = fb.count;
    /* HDR bloom on emissive surfaces, engine trails and flares (additive values above 1) */
    if (!g.no_bloom || g.auto_exposure) {
        f.post.flags = (g.no_bloom ? 0u : SU_POST_BLOOM) | (g.auto_exposure ? SU_POST_AUTO_EXPOSURE : 0u);
        f.post.bloom_threshold = 1.0f;
        f.post.bloom_knee = 0.5f;
        f.post.bloom_intensity = 0.9f;
        f.post.bloom_radius = 1.0f;
        f.post.auto_key = 0.12f;
    }

    /* HUD */
    speed = sqrtf(s[0].vel.x * s[0].vel.x + s[0].vel.y * s[0].vel.y + s[0].vel.z * s[0].vel.z);
    su_hud_begin(&hud, hud_v, sizeof hud_v / sizeof hud_v[0], width, height);
    su_hud_rect(&hud, 16, 16, 260, 92, 0x90100804u);
    (void)su_hud_textf(&hud, 28, 26, 2.0f, 0xFFFFFFFFu, "SPEED %6.1f m/s", (double)speed);
    (void)su_hud_textf(&hud, 28, 48, 2.0f, 0xFF40D0FFu, "GATES %u", (unsigned)g.gate_events);
    (void)su_hud_textf(&hud, 28, 70, 2.0f, 0xFFFFFFFFu, "TIME  %02d:%05.2f", (int)(g.time_s / 60.0),
                       fmod(g.time_s, 60.0));
    su_hud_rect(&hud, 28, 96, 236.0f * (speed / 40.0f > 1.0f ? 1.0f : speed / 40.0f), 6, 0xFF20A0FFu);
    {
        SuRenderStats rs;
        (void)su_render_stats(g.r, &rs);
        (void)su_hud_textf(&hud, 16, (float)height - 40.0f, 1.0f, 0xFFB0B0B0u,
                           "%.0f fps | gpu %.2f ms | cpu %.2f ms (scene %.2f) | draws %u | objects %u, culled %u, "
                           "lod %u/%u | uploads %u",
                           fps, rs.gpu_ms, rs.cpu_ms, rs.scene_cpu_ms, rs.draws, rs.objects, rs.objects_culled,
                           rs.lod_nodes[0], rs.lod_nodes[1], rs.uploads_pending);
        (void)su_hud_textf(&hud, 16, (float)height - 24.0f, 1.0f, 0xFFB0B0B0u,
                           "tick %llu | %s camera | model %s | gate sounds %llu",
                           (unsigned long long)su_host_tick(g.w), g.fixed_cam ? "fixed" : g.free_cam ? "free" : "chase",
                           g.user_state == 0 ? "none" : g.user_state == 1 ? "loading" : g.user_state == 4 ? "failed"
                                                     : su_render_pak_state(g.r, &g.user_pak) == SU_RENDER_STATE_READY ? "ready"
                                                                                                                       : "uploading",
                           (unsigned long long)g.gate_sounds);
    }
    f.hud = hud_v;
    f.hud_vertex_count = hud.count;

    /* audio follows the same interpolated transforms */
    if (g.mixer != NULL) {
        (void)su_audio_listener(g.mixer, f.camera.pos, f.camera.rot, cam_vel);
        if (g.engine_voice != 0u) {
            (void)su_audio_voice_update(g.mixer, g.engine_voice, objs[0].pos, s[0].vel, 0.08f + 0.25f * speed / 40.0f);
        }
    }

    st = su_render_frame_ex(g.r, &f);
    g.frames_drawn += st == SU_OK;
    g.frames_paused += st == SU_E_RESOURCE_LIMIT;
    if (st != SU_OK && st != SU_E_RESOURCE_LIMIT && ++g.frames_failed <= 3u) {
        fprintf(stderr, "frame failed: %s\n", su_render_last_error(g.r));
    }
}

static int cmp_f(const void *a, const void *b) {
    float x = *(const float *)a;
    float y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* Median and 95th percentile of the recorded values (sorted in place). */
static void dist(float *v, uint32_t n, double *med, double *p95) {
    if (n == 0u) {
        *med = *p95 = -1.0;
        return;
    }
    qsort(v, n, sizeof *v, cmp_f);
    *med = v[n / 2u];
    *p95 = v[(uint32_t)((double)(n - 1u) * 0.95)];
}

static void record_times(void) {
    SuRenderStats rs;
    if (g.r == NULL || su_render_stats(g.r, &rs) != SU_OK) {
        return;
    }
    if (g.t_n == g.t_cap) {
        uint32_t nc = g.t_cap ? g.t_cap * 2u : 1024u;
        float *a = realloc(g.t_gpu, nc * sizeof *a);
        float *b;
        if (a == NULL) {
            return;
        }
        g.t_gpu = a;
        b = realloc(g.t_cpu, nc * sizeof *b);
        if (b == NULL) {
            return;
        }
        g.t_cpu = b;
        g.t_cap = nc;
    }
    g.t_gpu[g.t_n] = (float)rs.gpu_ms;
    g.t_cpu[g.t_n] = (float)rs.cpu_ms;
    g.t_n++;
}

static void setup_audio(int device) {
    SuAudioMixerDesc md;
    char msg[256];
    SuAudioVoiceDesc d;
    memset(&md, 0, sizeof md);
    md.struct_size = sizeof md;
    md.sample_rate = AUDIO_RATE;
    if (su_audio_mixer_create(&md, &g.mixer) != SU_OK ||
        su_audio_clip_tone(SU_AUDIO_WAVE_SAW, 55.0f, 1.0f, AUDIO_RATE, 1, &g.clip_engine) != SU_OK ||
        su_audio_clip_tone(SU_AUDIO_WAVE_SINE, 880.0f, 0.4f, AUDIO_RATE, 0, &g.clip_chime) != SU_OK) {
        fprintf(stderr, "audio setup failed\n");
        exit(1);
    }
    memset(&d, 0, sizeof d);
    d.clip = g.clip_engine;
    d.gain = 0.1f;
    d.flags = SU_AUDIO_LOOP | SU_AUDIO_SPATIAL;
    d.min_distance = 8.0f;
    d.doppler = 0.5f;
    d.priority = 3;
    (void)su_audio_play(g.mixer, &d, &g.engine_voice);
    if (device) {
        SuAudioDeviceInfo info;
        if (su_audio_device_open(g.mixer, &g.adev, &info, msg) == SU_OK) {
            printf("audio: %s, %u Hz, buffer %u frames\n", info.name, (unsigned)info.sample_rate,
                   (unsigned)info.buffer_frames);
        } else {
            printf("audio: no output (%s); mixing offline\n", msg);
        }
    }
}

int main(int argc, char **argv) {
    const char *map_path = "examples/input/default_map.json";
    const char *offscreen = NULL;
    const char *expect_digest = NULL;
    const char *model_path = NULL;
    const char *cache_path = NULL;
    int fixed_dt = 0;
    uint64_t max_frames = 0;
    double seconds = 0.0;
    int script = 0;
    int exercise = 0;
    int rollback = 0;
    int headless = 0;
    int no_audio = 0;
    uint32_t flags = 0;
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t n_debris = 3000;
    double offset_km = 0.0;
    int presentation = 1;
    SuHostClock clock;
    SuChaseCamera chase;
    SuFreeCamera freec;
    SuInputRecord last_rec;
    uint64_t frames = 0;
    double t_start;
    double t_prev;
    double t_report;
    double fps = 0.0;
    uint64_t frames_report = 0;
    uint64_t ticks_report = 0;
    int i;
    char msg[256];

    g.user_scale = 1.0f;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else if (strcmp(argv[i], "--script") == 0) {
            script = 1;
        } else if (strcmp(argv[i], "--offscreen") == 0 && i + 1 < argc) {
            offscreen = argv[++i];
            script = 1;
        } else if (strcmp(argv[i], "--expect-digest") == 0 && i + 1 < argc) {
            expect_digest = argv[++i];
        } else if (strcmp(argv[i], "--rollback-check") == 0) {
            rollback = 1;
        } else if (strcmp(argv[i], "--exercise-window") == 0) {
            exercise = 1;
        } else if (strcmp(argv[i], "--headless") == 0) {
            headless = 1;
        } else if (strcmp(argv[i], "--no-vsync") == 0) {
            flags |= SU_RENDER_FLAG_NO_VSYNC;
        } else if (strcmp(argv[i], "--validation") == 0) {
            flags |= SU_RENDER_FLAG_VALIDATION;
        } else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            map_path = argv[++i];
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model_path = argv[++i];
        } else if (strcmp(argv[i], "--model-scale") == 0 && i + 1 < argc) {
            g.user_scale = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--debris") == 0 && i + 1 < argc) {
            n_debris = (uint32_t)strtoul(argv[++i], NULL, 10);
            n_debris = n_debris > MAX_DEBRIS ? MAX_DEBRIS : n_debris;
        } else if (strcmp(argv[i], "--world-offset") == 0 && i + 1 < argc) {
            offset_km = atof(argv[++i]);
        } else if (strcmp(argv[i], "--no-audio") == 0) {
            no_audio = 1;
        } else if (strcmp(argv[i], "--no-env") == 0) {
            g.no_env = 1;
        } else if (strcmp(argv[i], "--no-bloom") == 0) {
            g.no_bloom = 1;
        } else if (strcmp(argv[i], "--no-glare") == 0) {
            g.no_glare = 1;
        } else if (strcmp(argv[i], "--auto-exposure") == 0) {
            g.auto_exposure = 1;
        } else if (strcmp(argv[i], "--legacy") == 0) {
            g.legacy = 1;
        } else if (strcmp(argv[i], "--pipeline-cache") == 0 && i + 1 < argc) {
            cache_path = argv[++i];
        } else {
            fprintf(stderr, "unknown option %s (see the comment at the top of space_demo.c)\n", argv[i]);
            return 1;
        }
    }
    if (offscreen != NULL && max_frames == 0u) {
        max_frames = 240u;
    }
    g.rollback = rollback;
    g.origin = su_wpos(offset_km * 1000.0, 0.0, 0.0);
    g.w = make_world();
    g.n = MAX_SHIPS;
    for (i = 0; i < (int)g.n; i++) {
        g.ents[i] = spawn_ship(g.w, (uint32_t)i);
        su_trail_init(&g.trail[i], g.trail_pts[i], TRAIL_POINTS, 1.2f, 0.6f);
    }
    if (offscreen != NULL) {
        g.fixed_cam = 1;
        g.shadow = make_world();
        for (i = 0; i < (int)g.n; i++) {
            (void)spawn_ship(g.shadow, (uint32_t)i);
        }
    }
    {
        size_t len = 0;
        uint8_t *json = slurp(map_path, &len);
        SuInputMap map;
        if (su_input_map_from_json(json, len, &map, msg) != SU_OK || su_input_mapper_init(&g.mapper, &map) != SU_OK) {
            fprintf(stderr, "%s: %s\n", map_path, msg);
            return 1;
        }
        free(json);
    }
    g.fields.buttons = SU_INPUT_NO_FIELD;
    for (i = 0; i < (int)SU_INPUT_MAX_AXES; i++) {
        g.fields.axes[i] = SU_INPUT_NO_FIELD;
    }
    g.fields.axes[0] = F_IN_PITCH;  /* the map's axis order: pitch, yaw, roll, throttle, strafe_x, strafe_y */
    g.fields.axes[1] = F_IN_YAW;
    g.fields.axes[2] = F_IN_ROLL;
    g.fields.axes[3] = F_IN_THRUST;
    read_ships(g.cur);
    memcpy(g.prev, g.cur, sizeof g.prev);

    if (!headless) {
        SuRenderDesc d;
        uint32_t st;
        if (offscreen == NULL) {
            g.hwnd = make_window(width, height);
            if (g.hwnd == NULL) {
                fprintf(stderr, "window creation failed\n");
                return 1;
            }
        }
        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d;
        d.flags = flags;
        d.hwnd = g.hwnd;
        d.width = width;
        d.height = height;
        d.device_index = -1;
        d.pipeline_cache_path = cache_path;
        d.max_draw_instances = MAX_DEBRIS * 2u + 256u;
        st = su_render_create(&d, &g.r, msg);
        if (st != SU_OK) {
            printf("presentation unavailable (%s): %s; running the simulation headless\n", su_host_status_name(st), msg);
            presentation = 0;
            if (g.hwnd != NULL) {
                DestroyWindow(g.hwnd);
                g.hwnd = NULL;
            }
        } else {
            SuRenderDeviceInfo info;
            SuRenderStats rs;
            (void)su_render_device(g.r, &info);
            (void)su_render_stats(g.r, &rs);
            printf("renderer: %s, %s target %ux%u, pipelines %.1f ms (cache %u)\n", info.name,
                   offscreen != NULL ? "offscreen" : "window", (unsigned)width, (unsigned)height, rs.pipeline_create_ms,
                   (unsigned)rs.pipeline_cache);
            if (!g.legacy) {
                setup_content();
                make_debris(n_debris);
                if (model_path != NULL) {
                    if (su_asset_loader_create(su_asset_import_available() ? su_asset_cook_gltf : NULL, &g.loader) != SU_OK ||
                        su_asset_loader_request(g.loader, model_path, 1) != SU_OK) {
                        fprintf(stderr, "model loader failed\n");
                        return 1;
                    }
                    g.user_state = 1;
                    printf("model: %s queued on the loader thread%s\n", model_path,
                           su_asset_import_available() ? "" : " (no glTF importer in this build: packs only)");
                }
            }
        }
    } else {
        presentation = 0;
    }
    if (!no_audio) {
        setup_audio(g.hwnd != NULL);
    }
    if (g.hwnd != NULL && !script) {
        if (su_input_win32_create(g.hwnd, 0, &g.input) != SU_OK) {
            printf("input: Raw Input registration failed; using scripted input\n");
            script = 1;
        }
    }
    if (!presentation && seconds == 0.0 && max_frames == 0u) {
        seconds = 2.0;
    }

    /* Offscreen and headless scripted runs use a fixed 60 Hz frame time: reproducible ticks, image and digest. */
    fixed_dt = offscreen != NULL || (script && !presentation && headless);
    (void)su_host_clock_init(&clock, TICK_HZ, 8);
    memset(&chase, 0, sizeof chase);
    chase.offset.y = 3.0f;
    chase.offset.z = 12.0f;
    chase.look_ahead.z = -20.0f;
    chase.stiffness = 6.0f;
    memset(&freec, 0, sizeof freec);
    freec.pos.y = 20.0f;
    freec.pos.z = 80.0f;
    freec.pitch = -0.2f;
    memset(&last_rec, 0, sizeof last_rec);
    t_start = t_prev = t_report = now_s();
    while (!g.quit) {
        MSG m;
        SuInputState frame_state;
        double now;
        double dt;
        double alpha = 0.0;
        uint32_t ticks;
        uint32_t k;
        while (g.hwnd != NULL && PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        now = now_s();
        /* Offscreen runs use a fixed 60 Hz frame time so the image and digest are reproducible. */
        dt = fixed_dt ? 1.0 / 60.0 : now - t_prev;
        t_prev = now;
        g.time_s += dt;
        ticks = su_host_clock_advance(&clock, (uint64_t)(dt * 1e9), &alpha);
        if (!script && g.input != NULL) {
            (void)su_input_win32_sample(g.input, &frame_state);
        }
        for (k = 0; k < ticks; k++) {
            SuInputState s;
            if (script) {
                scripted_state(su_host_tick(g.w), &s);
            } else if (g.input != NULL) {
                (void)su_input_state_split(&frame_state, ticks, k, &s);
            } else {
                memset(&s, 0, sizeof s);
            }
            tick(&s);
            last_rec.buttons = g.last_buttons;
        }
        {
            /* The free camera reads the latest record's axes (presentation only). */
            SuInputRecord rec;
            SuInputMapper tmp = g.mapper;
            SuInputState s;
            if (script) {
                scripted_state(su_host_tick(g.w), &s);
            } else if (g.input != NULL) {
                s = frame_state;
            } else {
                memset(&s, 0, sizeof s);
            }
            (void)su_input_mapper_tick(&tmp, &s, 0, &rec);
            last_rec = rec;
        }
        if (g.gate_flash > 0.0) {
            g.gate_flash -= dt;
        }
        if (presentation) {
            SuRenderStats rs;
            poll_user_model();
            (void)su_render_stats(g.r, &rs);
            draw((float)alpha, dt, &chase, &freec, rs.width != 0u ? rs.width : width, rs.height != 0u ? rs.height : height,
                 &last_rec, fps);
            if (frames >= 30u) { /* skip the warm-up (uploads, first frames) */
                record_times();
            }
        } else if (!fixed_dt) {
            Sleep(1);
        }
        if (g.mixer != NULL) {
            if (g.adev == NULL) {
                /* no device: mix offline at the frame's length so the event path still runs */
                uint32_t n = (uint32_t)(dt * AUDIO_RATE + 0.5);
                n = n > AUDIO_RATE / 10u ? AUDIO_RATE / 10u : n;
                su_audio_mixer_render(g.mixer, g.audio_buf, n);
            }
            su_audio_update(g.mixer);
        }
        frames++;
        /* --exercise-window: resize, minimize, restore and resize back, to drive swapchain recreation. */
        if (exercise && g.hwnd != NULL) {
            if (frames == 20u) {
                SetWindowPos(g.hwnd, NULL, 0, 0, 800, 600, SWP_NOMOVE | SWP_NOZORDER);
            } else if (frames == 40u) {
                ShowWindow(g.hwnd, SW_MINIMIZE);
            } else if (frames == 70u) {
                ShowWindow(g.hwnd, SW_RESTORE);
            } else if (frames == 90u) {
                SetWindowPos(g.hwnd, NULL, 0, 0, 1296, 759, SWP_NOMOVE | SWP_NOZORDER);
            }
        }
        ticks_report += ticks;
        frames_report++;
        if (now - t_report >= 1.0 && offscreen == NULL) {
            char title[256];
            SuRenderStats rs;
            memset(&rs, 0, sizeof rs);
            rs.gpu_ms = -1.0;
            if (presentation) {
                (void)su_render_stats(g.r, &rs);
            }
            fps = (double)frames_report / (now - t_report);
            snprintf(title, sizeof title,
                     "Sutekh demo | %.1f fps | %.1f ticks/s | render cpu %.3f ms wait %.3f present %.3f gpu %.3f ms | step %.3f ms | gates %llu | %s",
                     fps, (double)ticks_report / (now - t_report), rs.cpu_ms, rs.wait_ms, rs.present_ms, rs.gpu_ms,
                     g.step_count ? g.step_ms_sum / (double)g.step_count : 0.0, (unsigned long long)g.gate_events,
                     g.free_cam ? "free camera" : "chase camera");
            printf("%s\n", title + 14);
            if (g.hwnd != NULL) {
                SetWindowTextA(g.hwnd, title);
            }
            frames_report = 0;
            ticks_report = 0;
            t_report = now;
        }
        if ((max_frames != 0u && frames >= max_frames) || (seconds > 0.0 && now - t_start >= seconds)) {
            break;
        }
    }

    {
        uint8_t dg[32];
        char hex[65];
        uint64_t fast = 0;
        SuRenderStats rs;
        int ok = 1;
        (void)su_host_digest(g.w, dg);
        su_host_hex(dg, hex);
        if (expect_digest != NULL && strcmp(expect_digest, hex) != 0) {
            printf("digest differs from --expect-digest %s\n", expect_digest);
            ok = 0;
        }
        (void)su_host_digest_fast(g.w, &fast);
        printf("frames %llu, ticks %llu, dropped ticks %llu, gate events %llu, mean step %.3f ms\n",
               (unsigned long long)frames, (unsigned long long)su_host_tick(g.w),
               (unsigned long long)clock.dropped_ticks, (unsigned long long)g.gate_events,
               g.step_count ? g.step_ms_sum / (double)g.step_count : 0.0);
        printf("digest %s fast %016llx\n", hex, (unsigned long long)fast);
        if (presentation) {
            (void)su_render_stats(g.r, &rs);
            printf("render: frames %llu, last cpu %.3f ms (scene %.3f), last gpu %.3f ms, draws %u, objects %u "
                   "(culled %u, nodes drawn %u, LOD0 %u, LOD1 %u), draw instances %u, fx vertices %u, hud vertices %u, "
                   "uploaded %llu bytes, swapchain recreations %u, validation %s (%u errors, %u warnings), present mode %u\n",
                   (unsigned long long)rs.frame, rs.cpu_ms, rs.scene_cpu_ms, rs.gpu_ms, rs.draws, rs.objects,
                   rs.objects_culled, rs.nodes_drawn, rs.lod_nodes[0], rs.lod_nodes[1], rs.draw_instances,
                   rs.fx_vertices, rs.hud_vertices, (unsigned long long)rs.upload_bytes_total, rs.swapchain_recreations,
                   rs.validation_enabled ? "on" : "off", rs.validation_errors, rs.validation_warnings, rs.present_mode);
            printf("frames drawn %llu, paused %llu, failed %llu\n", (unsigned long long)g.frames_drawn,
                   (unsigned long long)g.frames_paused, (unsigned long long)g.frames_failed);
            {
                double gm;
                double g95;
                double cm;
                double c95;
                dist(g.t_gpu, g.t_n, &gm, &g95);
                dist(g.t_cpu, g.t_n, &cm, &c95);
                printf("timings over %u frames after warm-up (observations): gpu median %.3f ms p95 %.3f ms; render cpu "
                       "median %.3f ms p95 %.3f ms\n",
                       (unsigned)g.t_n, gm, g95, cm, c95);
            }
            ok = rs.validation_errors == 0u && g.frames_failed == 0u;
        }
        if (g.rollback) {
            printf("rollback drills: %llu (restore in place, re-push %u records, step %u ticks), %llu differed, "
                   "%llu re-emitted events compared, mean %.3f ms each\n",
                   (unsigned long long)g.rollbacks, RB_DEPTH, RB_DEPTH, (unsigned long long)g.rb_mismatch,
                   (unsigned long long)g.rb_events, g.rollbacks ? g.rb_ms_sum / (double)g.rollbacks : 0.0);
            ok = ok && g.rb_mismatch == 0u && g.rollbacks > 0u;
        }
        if (g.mixer != NULL) {
            SuAudioStats as;
            su_audio_mixer_stats(g.mixer, &as);
            printf("audio: gate sounds %llu for %llu gate events, re-emitted events let through %llu, %llu frames mixed "
                   "(%s), voices stolen %u, refused %u, commands dropped %u\n",
                   (unsigned long long)g.gate_sounds, (unsigned long long)g.gate_events,
                   (unsigned long long)g.replayed_events, (unsigned long long)as.frames_rendered,
                   g.adev != NULL ? "device" : "offline", as.voices_stolen, as.plays_refused, as.commands_dropped);
            ok = ok && g.gate_sounds == g.gate_events && g.replayed_events == 0u;
        }
        if (g.shadow != NULL) {
            printf("shadow world (no renderer): %llu tick digests differed\n", (unsigned long long)g.shadow_mismatch);
            ok = ok && g.shadow_mismatch == 0u;
        }
        if (offscreen != NULL && presentation) {
            uint8_t *px = malloc((size_t)width * height * 4u);
            if (px == NULL || su_render_readback(g.r, px, (size_t)width * height * 4u) != SU_OK ||
                su_render_write_ppm(offscreen, px, width, height) != SU_OK) {
                fprintf(stderr, "readback failed\n");
                ok = 0;
            } else {
                printf("wrote %s\n", offscreen);
            }
            free(px);
        }
        su_audio_device_close(g.adev);
        su_input_win32_destroy(g.input);
        if (g.r != NULL) {
            if (g.user_pak.handles != NULL) {
                su_render_pak_release(g.r, &g.user_pak);
            }
            if (g.rpak.handles != NULL) {
                su_render_pak_release(g.r, &g.rpak);
            }
        }
        su_render_destroy(g.r);
        su_asset_loader_destroy(g.loader);
        su_asset_pak_close(g.user_asset);
        su_asset_pak_close(g.pack);
        free(g.pack_bytes);
        free(g.debris);
        free(g.t_gpu);
        free(g.t_cpu);
        su_audio_mixer_destroy(g.mixer);
        su_audio_clip_free(g.clip_engine);
        su_audio_clip_free(g.clip_chime);
        if (g.hwnd != NULL) {
            DestroyWindow(g.hwnd);
        }
        su_host_world_destroy(g.shadow);
        su_host_world_destroy(g.w);
        if (!ok) {
            return 1;
        }
        return presentation || headless ? 0 : 3;
    }
}
