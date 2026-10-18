/* Input mapping (include/sutekh/input.h, portable part) with synthetic device states: key names, splitting of
 * relative motion, digital and analog bindings, radial and axial dead zones, curves, triggers as buttons, the mouse
 * virtual stick, record encoding, the JSON map format, and records driving a host world through su_host_input_push.
 * Expected values are derived by hand in the comments (Q16.16, ONE = 65536).
 *
 * Usage: test_input   (run from the repository root: it reads examples/) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/host.h"
#include "sutekh/input.h"

static long g_checks = 0;
static long g_fail = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            g_fail++;                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

#define ONE SU_INPUT_ONE

typedef struct Bytes {
    uint8_t *data;
    size_t len;
} Bytes;

static Bytes slurp(const char *path) {
    Bytes b = {NULL, 0};
    FILE *f = fopen(path, "rb");
    long n;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    b.data = malloc((size_t)n + 1u);
    if (b.data == NULL || fread(b.data, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    b.len = (size_t)n;
    return b;
}

static int near(int32_t a, int32_t b, int32_t tol) {
    return a - b <= tol && b - a <= tol;
}

static void map_init(SuInputMap *m, uint32_t axes, uint32_t buttons) {
    memset(m, 0, sizeof *m);
    m->axis_count = axes;
    m->button_count = buttons;
}

static void add(SuInputMap *m, SuInputBinding b) {
    m->bindings[m->binding_count++] = b;
}

static SuInputRecord run(SuInputMapper *mp, const SuInputState *s) {
    SuInputRecord r;
    CHECK(su_input_mapper_tick(mp, s, mp->ticks, &r) == SU_OK);
    return r;
}

static void test_key_names(void) {
    uint32_t k = 0;
    uint32_t code;
    int named = 0;
    CHECK(su_input_key_from_name("W", &k) == SU_OK && k == SU_KEY_W);
    CHECK(su_input_key_from_name("UP", &k) == SU_OK && k == SU_KEY_UP && k == 0x148u);
    CHECK(su_input_key_from_name("w", &k) == SU_E_INVALID_ARGUMENT);
    CHECK(su_input_key_from_name("NOPE", &k) == SU_E_INVALID_ARGUMENT);
    for (code = 0; code < SU_INPUT_KEY_COUNT; code++) {
        const char *n = su_input_key_name(code);
        if (n != NULL) {
            named++;
            CHECK(su_input_key_from_name(n, &k) == SU_OK && k == code);
        }
    }
    CHECK(named == 102); /* every SU_KEY_* constant of input.h */
}

static void test_split(void) {
    SuInputState s;
    SuInputState p;
    int32_t v;
    uint32_t n;
    memset(&s, 0, sizeof s);
    s.mouse_dx = 7;
    s.mouse_dy = -7;
    s.wheel = 120;
    su_input_set_key(&s, SU_KEY_SPACE, 1);
    /* 7 over 3: floor(7k/3) = 0, 2, 4, 7 -> 2, 2, 3. -7: 0, -3, -5, -7 -> -3, -2, -2. */
    CHECK(su_input_state_split(&s, 3, 0, &p) == SU_OK && p.mouse_dx == 2 && p.mouse_dy == -3 && p.wheel == 40);
    CHECK(su_input_key_down(&p, SU_KEY_SPACE));
    CHECK(su_input_state_split(&s, 3, 1, &p) == SU_OK && p.mouse_dx == 2 && p.mouse_dy == -2);
    CHECK(su_input_state_split(&s, 3, 2, &p) == SU_OK && p.mouse_dx == 3 && p.mouse_dy == -2);
    CHECK(su_input_state_split(&s, 3, 3, &p) == SU_E_INVALID_ARGUMENT);
    CHECK(su_input_state_split(&s, 0, 0, &p) == SU_E_INVALID_ARGUMENT);
    /* Property: parts sum to the whole and differ by at most one. */
    for (v = -1000; v <= 1000; v += 37) {
        for (n = 1; n <= 9; n++) {
            int32_t sum = 0;
            int32_t lo = INT32_MAX;
            int32_t hi = INT32_MIN;
            uint32_t i;
            s.mouse_dx = v;
            for (i = 0; i < n; i++) {
                (void)su_input_state_split(&s, n, i, &p);
                sum += p.mouse_dx;
                lo = p.mouse_dx < lo ? p.mouse_dx : lo;
                hi = p.mouse_dx > hi ? p.mouse_dx : hi;
            }
            CHECK(sum == v && hi - lo <= 1);
        }
    }
}

static void test_digital(void) {
    SuInputMap m;
    SuInputMapper mp;
    SuInputState s;
    SuInputRecord r;
    SuInputBinding b;
    map_init(&m, 2, 2);
    add(&m, su_input_binding(SU_INPUT_SRC_KEY, SU_KEY_W, 0, SU_INPUT_AXIS(0)));
    b = su_input_binding(SU_INPUT_SRC_KEY, SU_KEY_S, 0, SU_INPUT_AXIS(0));
    b.scale = -ONE;
    add(&m, b);
    b = su_input_binding(SU_INPUT_SRC_KEY, SU_KEY_LSHIFT, 0, SU_INPUT_AXIS(1));
    b.scale = ONE / 2; /* half */
    add(&m, b);
    add(&m, su_input_binding(SU_INPUT_SRC_KEY, SU_KEY_RSHIFT, 0, SU_INPUT_AXIS(1)));
    add(&m, su_input_binding(SU_INPUT_SRC_KEY, SU_KEY_CAPSLOCK, 0, SU_INPUT_AXIS(1)));
    add(&m, su_input_binding(SU_INPUT_SRC_KEY, SU_KEY_SPACE, 0, SU_INPUT_BUTTON(1)));
    add(&m, su_input_binding(SU_INPUT_SRC_MOUSE_BUTTON, SU_INPUT_MOUSE_LEFT, 0, SU_INPUT_BUTTON(0)));
    add(&m, su_input_binding(SU_INPUT_SRC_PAD_BUTTON, SU_PAD_A, 2, SU_INPUT_BUTTON(0)));
    CHECK(su_input_mapper_init(&mp, &m) == SU_OK);
    memset(&s, 0, sizeof s);
    r = run(&mp, &s);
    CHECK(r.axes[0] == 0 && r.axes[1] == 0 && r.buttons == 0u && r.axis_count == 2u && r.tick == 0u);
    su_input_set_key(&s, SU_KEY_W, 1);
    r = run(&mp, &s);
    CHECK(r.axes[0] == ONE && r.tick == 1u);
    su_input_set_key(&s, SU_KEY_S, 1);
    r = run(&mp, &s);
    CHECK(r.axes[0] == 0);
    su_input_set_key(&s, SU_KEY_W, 0);
    r = run(&mp, &s);
    CHECK(r.axes[0] == -ONE);
    su_input_set_key(&s, SU_KEY_LSHIFT, 1);
    r = run(&mp, &s);
    CHECK(r.axes[1] == ONE / 2);
    su_input_set_key(&s, SU_KEY_RSHIFT, 1);
    su_input_set_key(&s, SU_KEY_CAPSLOCK, 1);
    r = run(&mp, &s);
    CHECK(r.axes[1] == ONE); /* 2.5 clamped */
    su_input_set_key(&s, SU_KEY_SPACE, 1);
    r = run(&mp, &s);
    CHECK(r.buttons == 2u);
    s.mouse_buttons = SU_INPUT_MOUSE_LEFT;
    r = run(&mp, &s);
    CHECK(r.buttons == 3u);
    s.mouse_buttons = 0;
    s.pads[2].buttons = SU_PAD_A; /* not connected: ignored */
    r = run(&mp, &s);
    CHECK(r.buttons == 2u);
    s.pads[2].connected = 1;
    r = run(&mp, &s);
    CHECK(r.buttons == 3u);
}

static void test_sticks(void) {
    SuInputMap m;
    SuInputMapper mp;
    SuInputState s;
    SuInputRecord r;
    SuInputBinding b;
    int32_t dz = ONE / 5; /* 0.2 = 13107.2 -> 13107 */
    map_init(&m, 4, 0);
    b = su_input_binding(SU_INPUT_SRC_PAD_STICK, SU_INPUT_STICK_LX, 0, SU_INPUT_AXIS(0));
    b.dead_zone = dz;
    add(&m, b);
    b.code = SU_INPUT_STICK_LY;
    b.target = SU_INPUT_AXIS(1);
    add(&m, b);
    b.code = SU_INPUT_STICK_LX;
    b.target = SU_INPUT_AXIS(2);
    b.flags = SU_INPUT_BIND_AXIAL;
    add(&m, b);
    b.code = SU_INPUT_STICK_LY;
    b.target = SU_INPUT_AXIS(3);
    add(&m, b);
    CHECK(su_input_mapper_init(&mp, &m) == SU_OK);
    memset(&s, 0, sizeof s);
    s.pads[0].connected = 1;
    /* Full up: ly 32767 -> ONE; magnitude ONE -> conditioned ONE; components (0, ONE). */
    s.pads[0].ly = 32767;
    r = run(&mp, &s);
    CHECK(r.axes[0] == 0 && r.axes[1] == ONE && r.axes[3] == ONE);
    s.pads[0].ly = -32768; /* -1.00003, clamped to -ONE */
    r = run(&mp, &s);
    CHECK(r.axes[1] == -ONE && r.axes[3] == -ONE);
    /* Inside the radial dead zone: (0.1, 0.1) has magnitude 0.141 < 0.2 -> both 0. */
    s.pads[0].lx = 3277;
    s.pads[0].ly = 3277;
    r = run(&mp, &s);
    CHECK(r.axes[0] == 0 && r.axes[1] == 0 && r.axes[2] == 0 && r.axes[3] == 0);
    /* Radial keeps direction: (0.1, 0.9): magnitude 0.9055; (0.9055 - 0.2) / 0.8 = 0.8819; components
     * 0.1 / 0.9055 * 0.8819 = 0.0974 and 0.9 / 0.9055 * 0.8819 = 0.8765. Axial zeroes x (0.1 < 0.2) and maps
     * y to (0.9 - 0.2) / 0.8 = 0.875. */
    s.pads[0].lx = 3277;   /* 0.1000 */
    s.pads[0].ly = 29490;  /* 0.9000 */
    r = run(&mp, &s);
    CHECK(near(r.axes[0], 6383, 40));   /* 0.0974 * 65536 */
    CHECK(near(r.axes[1], 57443, 40));  /* 0.8765 * 65536 */
    CHECK(r.axes[2] == 0);
    CHECK(near(r.axes[3], 57344, 8));   /* 0.875 * 65536 */
    /* Disconnected: zero. */
    s.pads[0].connected = 0;
    r = run(&mp, &s);
    CHECK(r.axes[1] == 0 && r.axes[3] == 0);
}

static void test_curves_triggers(void) {
    SuInputMap m;
    SuInputMapper mp;
    SuInputState s;
    SuInputRecord r;
    SuInputBinding b;
    uint32_t bad = 0;
    map_init(&m, 1, 1);
    m.curve_count = 1;
    m.curves[0].count = 3;
    m.curves[0].x[0] = 0;
    m.curves[0].y[0] = 0;
    m.curves[0].x[1] = ONE / 2;
    m.curves[0].y[1] = ONE / 4;
    m.curves[0].x[2] = ONE;
    m.curves[0].y[2] = ONE;
    b = su_input_binding(SU_INPUT_SRC_PAD_TRIGGER, 0, 1, SU_INPUT_AXIS(0));
    b.curve = 1;
    add(&m, b);
    b = su_input_binding(SU_INPUT_SRC_PAD_TRIGGER, 1, 1, SU_INPUT_BUTTON(0));
    add(&m, b); /* threshold ONE / 2 */
    CHECK(su_input_mapper_init(&mp, &m) == SU_OK);
    memset(&s, 0, sizeof s);
    s.pads[1].connected = 1;
    /* lt 255 -> ONE -> curve ONE. */
    s.pads[1].lt = 255;
    r = run(&mp, &s);
    CHECK(r.axes[0] == ONE);
    /* lt 51 -> 51 * 65536 / 255 = 13107 (0.2): curve 13107 * 16384 / 32768 = 6553. */
    s.pads[1].lt = 51;
    r = run(&mp, &s);
    CHECK(r.axes[0] == 6553);
    /* lt 204 -> 52428 (0.8): 16384 + (52428 - 32768) * 49152 / 32768 = 16384 + 29490 = 45874. */
    s.pads[1].lt = 204;
    r = run(&mp, &s);
    CHECK(r.axes[0] == 45874);
    /* rt 127 -> 32639 < 32768: off; 128 -> 32896: on. */
    s.pads[1].rt = 127;
    r = run(&mp, &s);
    CHECK(r.buttons == 0u);
    s.pads[1].rt = 128;
    r = run(&mp, &s);
    CHECK(r.buttons == 1u);
    /* Invalid curves and bindings are refused. */
    m.curves[0].y[1] = ONE + 1;
    CHECK(su_input_map_validate(&m, &bad) == SU_E_INVALID_ARGUMENT && bad == SU_INPUT_MAX_BINDINGS);
    m.curves[0].y[1] = ONE / 4;
    m.bindings[0].scale = 0;
    CHECK(su_input_map_validate(&m, &bad) == SU_E_INVALID_ARGUMENT && bad == 0u);
    m.bindings[0].scale = ONE;
    m.bindings[0].dead_zone = ONE;
    CHECK(su_input_map_validate(&m, &bad) == SU_E_INVALID_ARGUMENT && bad == 0u);
    m.bindings[0].dead_zone = 0;
    m.bindings[1].target = SU_INPUT_BUTTON(1);
    CHECK(su_input_map_validate(&m, &bad) == SU_E_INVALID_ARGUMENT && bad == 1u);
    m.bindings[1].target = SU_INPUT_BUTTON(0);
    m.bindings[1].code = 2;
    CHECK(su_input_map_validate(&m, &bad) == SU_E_INVALID_ARGUMENT && bad == 1u);
    m.bindings[1].code = 1;
    m.bindings[1].curve = 2;
    CHECK(su_input_map_validate(&m, &bad) == SU_E_INVALID_ARGUMENT && bad == 1u);
    m.bindings[1].curve = 0;
    CHECK(su_input_map_validate(&m, &bad) == SU_OK);
}

static void test_mouse(void) {
    SuInputMap m;
    SuInputMapper mp;
    SuInputState s;
    SuInputRecord r;
    SuInputBinding b;
    map_init(&m, 2, 0);
    b = su_input_binding(SU_INPUT_SRC_MOUSE_STICK, SU_INPUT_MOUSE_AXIS_X, 0, SU_INPUT_AXIS(0));
    b.counts = 400;
    b.recenter = ONE / 10; /* 6553.6 -> 6554 */
    b.recenter = 6554;
    add(&m, b);
    b = su_input_binding(SU_INPUT_SRC_MOUSE_DELTA, SU_INPUT_MOUSE_AXIS_Y, 0, SU_INPUT_AXIS(1));
    b.counts = 400;
    b.scale = -ONE;
    add(&m, b);
    CHECK(su_input_mapper_init(&mp, &m) == SU_OK);
    memset(&s, 0, sizeof s);
    /* Stick: +200 counts = 0.5 (32768), minus recenter 6554 -> 26214. */
    s.mouse_dx = 200;
    s.mouse_dy = 100; /* delta: 0.25, inverted */
    r = run(&mp, &s);
    CHECK(r.axes[0] == 26214 && r.axes[1] == -16384);
    /* No motion: stick recenters by 6554 per tick; delta is 0. */
    s.mouse_dx = 0;
    s.mouse_dy = 0;
    r = run(&mp, &s);
    CHECK(r.axes[0] == 19660 && r.axes[1] == 0);
    r = run(&mp, &s);
    r = run(&mp, &s);
    r = run(&mp, &s);
    CHECK(r.axes[0] == 0); /* 19660 - 3 x 6554 < 0 -> clamped at 0, not overshooting */
    /* Large motion saturates at ONE, then recenters from ONE. */
    s.mouse_dx = 100000;
    s.mouse_dy = -100000;
    r = run(&mp, &s);
    CHECK(r.axes[0] == ONE - 6554 && r.axes[1] == ONE);
    s.mouse_dx = -800; /* -2.0 from 58982 -> clamped -ONE, + recenter */
    r = run(&mp, &s);
    CHECK(r.axes[0] == -ONE + 6554);
    su_input_mapper_reset(&mp);
    s.mouse_dx = 0;
    s.mouse_dy = 0;
    r = run(&mp, &s);
    CHECK(r.axes[0] == 0);
}

static void test_records(void) {
    SuInputRecord r;
    SuInputRecord d;
    SuInputFields f;
    SuHostInput w[1 + SU_INPUT_MAX_AXES];
    uint8_t buf[SU_INPUT_RECORD_BYTES];
    uint8_t bad[SU_INPUT_RECORD_BYTES];
    uint32_t n = 0;
    uint32_t i;
    memset(&r, 0, sizeof r);
    r.tick = UINT64_C(0x0102030405060708);
    r.buttons = 0x80000001u;
    r.axis_count = 3;
    r.axes[0] = -ONE;
    r.axes[1] = 12345;
    r.axes[2] = ONE;
    su_input_record_encode(&r, buf);
    CHECK(memcmp(buf, "SUINREC", 8) == 0 && buf[8] == 1u && buf[12] == 0x08u && buf[19] == 0x01u);
    CHECK(su_input_record_decode(buf, sizeof buf, &d) == SU_OK && memcmp(&d, &r, sizeof r) == 0);
    CHECK(su_input_record_decode(buf, sizeof buf - 1u, &d) == SU_E_PARSE);
    memcpy(bad, buf, sizeof bad);
    bad[24] = 17; /* axis_count */
    CHECK(su_input_record_decode(bad, sizeof bad, &d) == SU_E_PARSE);
    memcpy(bad, buf, sizeof bad);
    bad[28 + 4 * 3] = 1; /* axis beyond axis_count nonzero */
    CHECK(su_input_record_decode(bad, sizeof bad, &d) == SU_E_PARSE);
    memcpy(bad, buf, sizeof bad);
    bad[28 + 4 * 2] = 1; /* axis 2 = ONE + 1 */
    CHECK(su_input_record_decode(bad, sizeof bad, &d) == SU_E_PARSE);
    f.buttons = 9;
    for (i = 0; i < SU_INPUT_MAX_AXES; i++) {
        f.axes[i] = SU_INPUT_NO_FIELD;
    }
    f.axes[0] = 4;
    f.axes[2] = 5;
    CHECK(su_input_record_writes(&r, &f, 77, w, &n) == SU_OK && n == 3u);
    CHECK(w[0].field_id == 9u && w[0].value == 0x80000001u && w[0].entity == 77u);
    CHECK(w[1].field_id == 4u && w[1].value == 0xFFFF0000u);
    CHECK(w[2].field_id == 5u && w[2].value == 0x00010000u);
    f.axes[2] = 9;
    CHECK(su_input_record_writes(&r, &f, 77, w, &n) == SU_E_INVALID_ARGUMENT);
}

static uint32_t parse(const char *json, SuInputMap *m, char *msg) {
    return su_input_map_from_json((const uint8_t *)json, strlen(json), m, msg);
}

static void test_json(void) {
    static const char *good =
        "{\"format\":\"sutekh.input_map\",\"version\":1,\"axes\":[\"pitch\",\"yaw\"],\"buttons\":[\"boost\"],"
        "\"curves\":[{\"name\":\"soft\",\"points_milli\":[[0,0],[500,250],[1000,1000]]}],"
        "\"bindings\":["
        "{\"target\":\"pitch\",\"source\":\"pad_stick\",\"pad\":1,\"stick\":\"ly\",\"dead_zone_milli\":150,"
        "\"saturation_milli\":950,\"curve\":\"soft\",\"scale_milli\":-1000,\"axial\":true},"
        "{\"target\":\"pitch\",\"source\":\"key\",\"key\":\"S\",\"scale_milli\":1000},"
        "{\"target\":\"yaw\",\"source\":\"mouse_stick\",\"axis\":\"x\",\"counts\":400,\"recenter_milli\":20},"
        "{\"target\":\"boost\",\"source\":\"pad_trigger\",\"trigger\":\"right\",\"threshold_milli\":500},"
        "{\"target\":\"boost\",\"source\":\"pad_button\",\"pad\":0,\"button\":\"A\"},"
        "{\"target\":\"boost\",\"source\":\"mouse_button\",\"button\":\"x2\"},"
        "{\"target\":\"yaw\",\"source\":\"mouse_delta\",\"axis\":\"wheel\",\"counts\":120}]}";
    static const struct {
        const char *json;
        uint32_t code;
    } bad[] = {
        {"{\"format\":\"sutekh.input_map\",\"version\":2,\"bindings\":[]}", SU_E_INVALID_ARGUMENT},
        {"{\"format\":\"other\",\"version\":1,\"bindings\":[]}", SU_E_INVALID_ARGUMENT},
        {"{\"format\":\"sutekh.input_map\",\"version\":1,\"bindings\":[],\"extra\":1}", SU_E_INVALID_ARGUMENT},
        {"{\"format\":\"sutekh.input_map\",\"version\":1,\"axes\":[\"a\"],\"bindings\":[{\"target\":\"a\","
         "\"source\":\"key\",\"key\":\"NOPE\"}]}",
         SU_E_INVALID_ARGUMENT},
        {"{\"format\":\"sutekh.input_map\",\"version\":1,\"axes\":[\"a\"],\"bindings\":[{\"target\":\"b\","
         "\"source\":\"key\",\"key\":\"W\"}]}",
         SU_E_INVALID_ARGUMENT},
        {"{\"format\":\"sutekh.input_map\",\"version\":1,\"axes\":[\"a\"],\"bindings\":[{\"target\":\"a\","
         "\"source\":\"pad_stick\",\"stick\":\"lx\",\"dead_zone_milli\":600,\"saturation_milli\":500}]}",
         SU_E_INVALID_ARGUMENT},
        {"{\"format\":\"sutekh.input_map\",\"version\":1,\"axes\":[\"a\"],\"curves\":[{\"name\":\"c\","
         "\"points_milli\":[[0,0],[600,500],[500,600],[1000,1000]]}],\"bindings\":[]}",
         SU_E_INVALID_ARGUMENT},
        {"{\"format\":\"sutekh.input_map\",\"version\":1,\"axes\":[\"a\",\"a\"],\"bindings\":[]}",
         SU_E_INVALID_ARGUMENT},
        {"{\"format\":\"sutekh.input_map\",\"version\":1,\"axes\":[\"a\"],\"buttons\":[\"a\"],\"bindings\":[]}",
         SU_E_INVALID_ARGUMENT},
        {"{\"format\":\"sutekh.input_map\",\"version\":1,\"axes\":[\"a\"],\"bindings\":[{\"target\":\"a\","
         "\"source\":\"key\",\"key\":\"W\",\"pad\":0}]}",
         SU_E_INVALID_ARGUMENT},
        {"{\"format\":\"sutekh.input_map\",\"version\":1,\"axes\":[\"a\"],\"bindings\":[{\"target\":\"a\","
         "\"source\":\"key\",\"key\":\"W\",\"scale_milli\":0.5}]}",
         SU_E_INVALID_ARGUMENT},
        {"{\"format\":\"sutekh.input_map\",\"version\":1,\"axes\":[\"a\"],\"bindings\":[{\"target\":\"a\","
         "\"source\":\"mouse_stick\",\"axis\":\"x\",\"counts\":0}]}",
         SU_E_INVALID_ARGUMENT},
        {"{\"format\":\"sutekh.input_map\",\"version\":1,\"bindings\":[}", SU_E_PARSE},
        {"[]", SU_E_TYPE},
    };
    SuInputMap m;
    char msg[160];
    size_t i;
    CHECK(parse(good, &m, msg) == SU_OK);
    CHECK(m.axis_count == 2u && m.button_count == 1u && m.binding_count == 7u && m.curve_count == 1u);
    CHECK(strcmp(m.axis_names[1], "yaw") == 0 && strcmp(m.button_names[0], "boost") == 0);
    CHECK(m.curves[0].x[1] == 32768 && m.curves[0].y[1] == 16384);
    CHECK(m.bindings[0].source == SU_INPUT_SRC_PAD_STICK && m.bindings[0].code == SU_INPUT_STICK_LY &&
          m.bindings[0].device == 1u && m.bindings[0].target == SU_INPUT_AXIS(0));
    /* 150 -> 150 * 65536 / 1000 = 9830.4 -> 9830; 950 -> 62259.2 -> 62259; -1000 -> -65536. */
    CHECK(m.bindings[0].dead_zone == 9830 && m.bindings[0].saturation == 62259 && m.bindings[0].scale == -ONE);
    CHECK(m.bindings[0].curve == 1u && m.bindings[0].flags == SU_INPUT_BIND_AXIAL);
    CHECK(m.bindings[1].code == SU_KEY_S);
    /* recenter 20 -> 1310.72 -> 1311. */
    CHECK(m.bindings[2].source == SU_INPUT_SRC_MOUSE_STICK && m.bindings[2].counts == 400 &&
          m.bindings[2].recenter == 1311);
    CHECK(m.bindings[3].code == 1u && m.bindings[3].device == 0u && m.bindings[3].target == SU_INPUT_BUTTON(0));
    CHECK(m.bindings[4].code == SU_PAD_A && m.bindings[5].code == SU_INPUT_MOUSE_X2);
    CHECK(m.bindings[6].code == SU_INPUT_MOUSE_AXIS_WHEEL && m.bindings[6].counts == 120);
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint32_t st = parse(bad[i].json, &m, msg);
        CHECK(st == bad[i].code);
        if (st != bad[i].code) {
            fprintf(stderr, "  case %u: got %u (%s)\n", (unsigned)i, (unsigned)st, msg);
        }
        CHECK(msg[0] != '\0');
    }
    {
        Bytes f = slurp("examples/input/default_map.json");
        CHECK(su_input_map_from_json(f.data, f.len, &m, msg) == SU_OK);
        if (msg[0] != '\0') {
            fprintf(stderr, "default map: %s\n", msg);
        }
        free(f.data);
    }
}

/* Records drive a world: axis 0 -> velocity_x (two's complement), position_x += velocity_x per tick. The same
 * device states give byte-identical recorded inputs on a second run. */
static void run_world(Bytes cap, Bytes lir, Bytes fx, uint8_t *recorded, size_t *recorded_len, uint32_t *pos0) {
    SuHostCapsule c;
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError e;
    SuInputMap m;
    SuInputMapper mp;
    SuInputFields f;
    SuInputState s;
    uint64_t ent = 0;
    uint32_t i;
    size_t off = 0;
    c.capsule_json = cap.data;
    c.capsule_len = cap.len;
    c.lir_json = lir.data;
    c.lir_len = lir.len;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.capsule_count = 1;
    d.capsules = &c;
    d.fixture_json = fx.data;
    d.fixture_len = fx.len;
    d.tick_rate_hz = 120;
    CHECK(su_host_world_create(&d, &w, &e) == SU_OK);
    CHECK(su_host_entity_at(w, 3, &ent) == SU_OK);
    map_init(&m, 1, 0);
    add(&m, su_input_binding(SU_INPUT_SRC_KEY, SU_KEY_D, 0, SU_INPUT_AXIS(0)));
    {
        SuInputBinding b = su_input_binding(SU_INPUT_SRC_KEY, SU_KEY_A, 0, SU_INPUT_AXIS(0));
        b.scale = -ONE;
        add(&m, b);
    }
    CHECK(su_input_mapper_init(&mp, &m) == SU_OK);
    f.buttons = SU_INPUT_NO_FIELD;
    for (i = 0; i < SU_INPUT_MAX_AXES; i++) {
        f.axes[i] = SU_INPUT_NO_FIELD;
    }
    CHECK(su_host_field_id(w, "velocity_x", &f.axes[0]) == SU_OK);
    memset(&s, 0, sizeof s);
    for (i = 0; i < 6; i++) {
        SuInputRecord r;
        SuHostInput writes[1 + SU_INPUT_MAX_AXES];
        uint32_t n = 0;
        uint64_t done = 0;
        size_t got = 0;
        su_input_set_key(&s, SU_KEY_D, i < 2u);
        su_input_set_key(&s, SU_KEY_A, i >= 2u && i < 5u);
        CHECK(su_input_mapper_tick(&mp, &s, su_host_tick(w), &r) == SU_OK);
        CHECK(su_input_record_writes(&r, &f, ent, writes, &n) == SU_OK && n == 1u);
        CHECK(su_host_input_push(w, su_host_tick(w), writes, n, &e) == SU_OK);
        CHECK(su_host_input_encode(w, su_host_tick(w), recorded + off, 4096 - off, &got) == SU_OK);
        off += got;
        CHECK(su_host_step(w, 1, &done, &e) == SU_OK && done == 1u);
    }
    *recorded_len = off;
    CHECK(su_host_read(w, 1, 3, 1, pos0) == SU_OK);
    su_host_world_destroy(w);
}

static void test_world(void) {
    Bytes cap = slurp("examples/movement.capsule.json");
    Bytes lir = slurp("examples/movement.lir.json");
    Bytes fx = slurp("examples/world.json");
    uint8_t a[4096];
    uint8_t b[4096];
    size_t na = 0;
    size_t nb = 0;
    uint32_t pa = 0;
    uint32_t pb = 0;
    run_world(cap, lir, fx, a, &na, &pa);
    run_world(cap, lir, fx, b, &nb, &pb);
    CHECK(na == nb && na > 0u && memcmp(a, b, na) == 0);
    /* Entity 3 starts at position 7. Velocities ONE, ONE, -ONE, -ONE, -ONE, 0 over six ticks:
     * 7 + 2 * 65536 - 3 * 65536 = 7 - 65536 (mod 2^32). */
    CHECK(pa == pb && pa == (uint32_t)(7 - 65536));
    free(cap.data);
    free(lir.data);
    free(fx.data);
}

int main(void) {
    test_key_names();
    test_split();
    test_digital();
    test_sticks();
    test_curves_triggers();
    test_mouse();
    test_records();
    test_json();
    test_world();
    printf("test_input: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
