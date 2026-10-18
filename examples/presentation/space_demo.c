/* Presentation demo: a headless Sutekh world (sutekh/host.h) with fixed-point (IR 0.3, Q32.32) ships, fed by the
 * native input layer (sutekh/input.h) and drawn by the Vulkan renderer (sutekh/render.h).
 *
 * The world runs at 120 Hz on the fixed-step clock (sutekh/clock.h); frames are drawn whenever the swapchain
 * allows, interpolating every ship between the last two ticks with the clock's alpha. Each tick, the device
 * sample of the frame (split over the frame's ticks) goes through the input map to a fixed-layout record, whose
 * axes are written to the player ship's input fields with su_host_input_push. The renderer only reads: ship
 * positions and rotations come from su_host_read_i64; nothing it computes reaches the world. Gate crossings are
 * events emitted by the flight system and read per tick with su_host_events_for_tick.
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
 *   options: --no-vsync, --validation, --map file, --width W --height H, --exercise-window (resize, minimize,
 *            restore and resize back during the run, to drive swapchain recreation)
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

#include "sutekh/clock.h"
#include "sutekh/host.h"
#include "sutekh/input.h"
#include "sutekh/render.h"

#define MAX_SHIPS 12u
#define TICK_HZ 120u
#define GATE_EVENT 1u

/* Field ids of examples/presentation/flight.capsule.json (q32 fields use id and id + 1). */
enum {
    F_POS_X = 1, F_POS_Y = 3, F_POS_Z = 5, F_VEL_X = 7, F_VEL_Y = 9, F_VEL_Z = 11,
    F_ROT_W = 13, F_ROT_X = 15, F_ROT_Y = 17, F_ROT_Z = 19, F_DRAG = 21, F_SPRING = 23,
    F_IN_PITCH = 25, F_IN_YAW = 26, F_IN_ROLL = 27, F_IN_THRUST = 28
};

typedef struct Ship {
    SuVec3f pos;
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
    uint64_t frames_drawn;
    uint64_t frames_paused;   /* su_render_frame returned SU_E_RESOURCE_LIMIT (minimized or out of date) */
    uint64_t frames_failed;
    uint32_t last_buttons;
    double gate_flash;        /* seconds left */
    uint64_t gate_events;
    uint64_t shadow_mismatch;
    double step_ms_sum;
    uint64_t step_count;
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

/* Reads every ship's committed state (the renderer's only view of the world). */
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

/* Deterministic scripted input for unattended runs: throttle, then pitch, yaw and roll phases. */
static void scripted_state(uint64_t tick, SuInputState *s) {
    uint64_t t = tick % 960u;
    memset(s, 0, sizeof *s);
    s->focused = 1;
    su_input_set_key(s, SU_KEY_W, t < 600u);
    su_input_set_key(s, SU_KEY_UP, t >= 240u && t < 300u);
    su_input_set_key(s, SU_KEY_RIGHT, t >= 360u && t < 420u);
    su_input_set_key(s, SU_KEY_E, t >= 480u && t < 540u);
    su_input_set_key(s, SU_KEY_S, t >= 700u && t < 760u);
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
        if (ev[i].kind == GATE_EVENT) {
            g.gate_events++;
            g.gate_flash = 0.5;
            printf("tick %llu: ship %u crossed the gate\n", (unsigned long long)ev[i].tick, (unsigned)ev[i].a);
        }
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

/* Builds the frame from interpolated ship states. */
static void draw(float alpha, double dt, SuChaseCamera *chase, SuFreeCamera *freec, uint32_t width, uint32_t height,
                 const SuInputRecord *axes) {
    static SuRenderInstance inst[MAX_SHIPS + 1u];
    static SuRenderLineVertex lines[8192];
    SuDebugLines dl;
    SuRenderView view;
    SuVec3f c0 = {0, 0, 0};
    SuQuatf id = {0, 0, 0, 1};
    uint32_t i;
    Ship s[MAX_SHIPS];
    memset(&dl, 0, sizeof dl);
    dl.v = lines;
    dl.cap = 8192;
    for (i = 0; i < g.n; i++) {
        s[i].pos = su_vec3f_lerp(g.prev[i].pos, g.cur[i].pos, alpha);
        s[i].rot = su_quatf_nlerp(g.prev[i].rot, g.cur[i].rot, alpha);
        s[i].vel = su_vec3f_lerp(g.prev[i].vel, g.cur[i].vel, alpha);
        memset(&inst[i], 0, sizeof inst[i]);
        inst[i].mesh = SU_MESH_SHIP;
        inst[i].pos[0] = s[i].pos.x;
        inst[i].pos[1] = s[i].pos.y;
        inst[i].pos[2] = s[i].pos.z;
        inst[i].scale = i == 0u ? 4.0f : 3.0f;
        inst[i].rot[0] = s[i].rot.x;
        inst[i].rot[1] = s[i].rot.y;
        inst[i].rot[2] = s[i].rot.z;
        inst[i].rot[3] = s[i].rot.w;
        inst[i].color[0] = i == 0u ? 1.0f : 0.3f + 0.05f * (float)i;
        inst[i].color[1] = i == 0u ? 0.6f : 0.5f;
        inst[i].color[2] = i == 0u ? 0.1f : 0.9f;
        inst[i].color[3] = 1.0f;
        {
            SuVec3f vv = s[i].vel;
            vv.x *= 0.5f;
            vv.y *= 0.5f;
            vv.z *= 0.5f;
            su_debug_vector(&dl, s[i].pos, vv, 0xFF40FF40u);
        }
    }
    /* The origin as a sphere, the gate as an ellipse in z = 0, a reference grid below, world axes. */
    memset(&inst[g.n], 0, sizeof inst[g.n]);
    inst[g.n].mesh = SU_MESH_SPHERE;
    inst[g.n].scale = 2.0f;
    inst[g.n].rot[3] = 1.0f;
    inst[g.n].color[0] = 0.8f;
    inst[g.n].color[1] = 0.8f;
    inst[g.n].color[2] = 0.7f;
    inst[g.n].color[3] = 1.0f;
    {
        SuVec3f u = {6, 0, 0};
        SuVec3f v = {0, 6, 0};
        su_debug_ellipse(&dl, c0, u, v, 64, g.gate_flash > 0.0 ? rgba(1, 1, 0.2f) : rgba(0.2f, 0.6f, 1));
    }
    {
        SuVec3f c = {0, -25, 0};
        SuVec3f u = {60, 0, 0};
        SuVec3f v = {0, 0, 60};
        su_debug_plane(&dl, c, u, v, 12, rgba(0.25f, 0.25f, 0.3f));
    }
    su_debug_axes(&dl, c0, id, 4.0f);
    memset(&view, 0, sizeof view);
    if (g.fixed_cam) {
        SuVec3f eye = {0.0f, 45.0f, 95.0f};
        SuVec3f up = {0.0f, 1.0f, 0.0f};
        view.view = su_mat4f_look_at(eye, c0, up);
    } else if (g.free_cam) {
        SuVec3f mv;
        float k = (float)dt * 40.0f;
        mv.x = k * (float)axes->axes[4] / SU_INPUT_ONE;
        mv.y = k * (float)axes->axes[5] / SU_INPUT_ONE;
        mv.z = k * (float)axes->axes[3] / SU_INPUT_ONE;
        view.view = su_free_camera_update(freec, mv, -(float)dt * 1.5f * (float)axes->axes[1] / SU_INPUT_ONE,
                                          (float)dt * 1.5f * (float)axes->axes[0] / SU_INPUT_ONE);
    } else {
        view.view = su_chase_camera_update(chase, s[0].pos, s[0].rot, (float)dt);
    }
    view.proj = su_mat4f_perspective(1.0471976f, (float)width / (float)(height > 0u ? height : 1u), 0.1f, 1000.0f);
    view.clear[0] = 0.01f;
    view.clear[1] = 0.01f;
    view.clear[2] = 0.025f;
    view.clear[3] = 1.0f;
    view.light_dir.x = -0.4f;
    view.light_dir.y = -0.8f;
    view.light_dir.z = -0.45f;
    view.ambient = 0.3f;
    {
        uint32_t st = su_render_frame(g.r, &view, inst, g.n + 1u, lines, dl.count);
        if (st == SU_OK) {
            g.frames_drawn++;
        } else if (st == SU_E_RESOURCE_LIMIT) {
            g.frames_paused++;
        } else {
            g.frames_failed++;
            if (g.frames_failed <= 3u) {
                fprintf(stderr, "frame failed: %s\n", su_render_last_error(g.r));
            }
        }
    }
}

int main(int argc, char **argv) {
    const char *map_path = "examples/input/default_map.json";
    const char *offscreen = NULL;
    uint64_t max_frames = 0;
    double seconds = 0.0;
    int script = 0;
    int exercise = 0;
    int headless = 0;
    uint32_t flags = 0;
    uint32_t width = 1280;
    uint32_t height = 720;
    int presentation = 1;
    SuHostClock clock;
    SuChaseCamera chase;
    SuFreeCamera freec;
    SuInputRecord last_rec;
    uint64_t frames = 0;
    double t_start;
    double t_prev;
    double t_report;
    uint64_t frames_report = 0;
    uint64_t ticks_report = 0;
    int i;
    char msg[256];

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
        } else {
            fprintf(stderr, "unknown option %s (see the comment at the top of space_demo.c)\n", argv[i]);
            return 1;
        }
    }
    if (offscreen != NULL && max_frames == 0u) {
        max_frames = 240u;
    }
    memset(&g, 0, sizeof g);
    g.w = make_world();
    g.n = MAX_SHIPS;
    for (i = 0; i < (int)g.n; i++) {
        g.ents[i] = spawn_ship(g.w, (uint32_t)i);
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
            (void)su_render_device(g.r, &info);
            printf("renderer: %s, %s target %ux%u\n", info.name, offscreen != NULL ? "offscreen" : "window",
                   (unsigned)width, (unsigned)height);
        }
    } else {
        presentation = 0;
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
        dt = offscreen != NULL ? 1.0 / 60.0 : now - t_prev;
        t_prev = now;
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
            (void)su_render_stats(g.r, &rs);
            draw((float)alpha, dt, &chase, &freec, rs.width != 0u ? rs.width : width, rs.height != 0u ? rs.height : height,
                 &last_rec);
        } else if (offscreen == NULL) {
            Sleep(1);
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
            snprintf(title, sizeof title,
                     "Sutekh demo | %.1f fps | %.1f ticks/s | render cpu %.3f ms wait %.3f present %.3f gpu %.3f ms | step %.3f ms | gates %llu | %s",
                     (double)frames_report / (now - t_report), (double)ticks_report / (now - t_report), rs.cpu_ms,
                     rs.wait_ms, rs.present_ms, rs.gpu_ms, g.step_count ? g.step_ms_sum / (double)g.step_count : 0.0,
                     (unsigned long long)g.gate_events, g.free_cam ? "free camera" : "chase camera");
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
        (void)su_host_digest_fast(g.w, &fast);
        printf("frames %llu, ticks %llu, dropped ticks %llu, gate events %llu, mean step %.3f ms\n",
               (unsigned long long)frames, (unsigned long long)su_host_tick(g.w),
               (unsigned long long)clock.dropped_ticks, (unsigned long long)g.gate_events,
               g.step_count ? g.step_ms_sum / (double)g.step_count : 0.0);
        printf("digest %s fast %016llx\n", hex, (unsigned long long)fast);
        if (presentation) {
            (void)su_render_stats(g.r, &rs);
            printf("render: frames %llu, last cpu %.3f ms, last gpu %.3f ms, draws %u, swapchain recreations %u, "
                   "validation %s (%u errors), present mode %u\n",
                   (unsigned long long)rs.frame, rs.cpu_ms, rs.gpu_ms, rs.draws, rs.swapchain_recreations,
                   rs.validation_enabled ? "on" : "off", rs.validation_errors, rs.present_mode);
            printf("frames drawn %llu, paused %llu, failed %llu\n", (unsigned long long)g.frames_drawn,
                   (unsigned long long)g.frames_paused, (unsigned long long)g.frames_failed);
            ok = rs.validation_errors == 0u && g.frames_failed == 0u;
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
        su_input_win32_destroy(g.input);
        su_render_destroy(g.r);
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
