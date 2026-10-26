/* Portable device events (include/sutekh/input.h): the USB HID usage to scan code set 1 table, the application of
 * synthetic events to a live state (keys, mouse, wheel, focus, gamepad slots, clamping, refusals), sampling, and
 * cross-platform record identity: one physical input sequence expressed as HID/SDL-style events and as the state
 * the Win32 backend builds (set 1 codes written directly from the SU_KEY_* constants, XInput ranges) must give
 * byte-identical records through the shipped default map, tick by tick.
 *
 * Usage: test_input_events   (run from the repository root: it reads examples/input/default_map.json) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static SuInputEvent ev(uint32_t type, uint32_t code, uint32_t device, int32_t value, uint64_t t) {
    SuInputEvent e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.code = code;
    e.device = device;
    e.value = value;
    e.time_ns = t;
    return e;
}

static SuInputEvent motion(int32_t x, int32_t y, uint64_t t) {
    SuInputEvent e = ev(SU_INPUT_EV_MOUSE_MOTION, 0, 0, 0, t);
    e.x = x;
    e.y = y;
    return e;
}

static void test_hid_table(void) {
    uint32_t seen[SU_INPUT_KEY_COUNT];
    uint32_t u;
    uint32_t k;
    uint32_t named = 0;
    memset(seen, 0, sizeof seen);
    for (u = 0; u < 0x100u; u++) {
        uint32_t c = su_input_key_from_hid(u);
        CHECK(c < SU_INPUT_KEY_COUNT);
        if (c != 0u && c < SU_INPUT_KEY_COUNT) {
            seen[c]++;
        }
    }
    /* Every named key (the keys a map can bind) has exactly one usage; backslash has two (ANSI 0x31, ISO 0x32). */
    for (k = 1; k < SU_INPUT_KEY_COUNT; k++) {
        if (su_input_key_name(k) != NULL) {
            named++;
            CHECK(seen[k] == (k == SU_KEY_BACKSLASH ? 2u : 1u));
            if (seen[k] != (k == SU_KEY_BACKSLASH ? 2u : 1u)) {
                fprintf(stderr, "  key %s (0x%x): %u usages\n", su_input_key_name(k), (unsigned)k, (unsigned)seen[k]);
            }
        } else {
            CHECK(seen[k] <= 1u);
        }
    }
    CHECK(named >= 100u);
    /* Spot checks against the HID Usage Tables (page 0x07). */
    CHECK(su_input_key_from_hid(0x04) == SU_KEY_A);
    CHECK(su_input_key_from_hid(0x1A) == SU_KEY_W);
    CHECK(su_input_key_from_hid(0x16) == SU_KEY_S);
    CHECK(su_input_key_from_hid(0x07) == SU_KEY_D);
    CHECK(su_input_key_from_hid(0x1D) == SU_KEY_Z);
    CHECK(su_input_key_from_hid(0x27) == SU_KEY_0);
    CHECK(su_input_key_from_hid(0x29) == SU_KEY_ESCAPE);
    CHECK(su_input_key_from_hid(0x2C) == SU_KEY_SPACE);
    CHECK(su_input_key_from_hid(0x44) == SU_KEY_F11);
    CHECK(su_input_key_from_hid(0x45) == SU_KEY_F12);
    CHECK(su_input_key_from_hid(0x4F) == SU_KEY_RIGHT);
    CHECK(su_input_key_from_hid(0x52) == SU_KEY_UP);
    CHECK(su_input_key_from_hid(0x58) == SU_KEY_KP_ENTER);
    CHECK(su_input_key_from_hid(0x60) == SU_KEY_KP_8);
    CHECK(su_input_key_from_hid(0x64) == 0x56u);
    CHECK(su_input_key_from_hid(0xE0) == SU_KEY_LCTRL);
    CHECK(su_input_key_from_hid(0xE1) == SU_KEY_LSHIFT);
    CHECK(su_input_key_from_hid(0xE6) == SU_KEY_RALT);
    CHECK(su_input_key_from_hid(0xE7) == SU_KEY_RMETA);
    CHECK(su_input_key_from_hid(0x48) == 0u); /* Pause: no set 1 make code (E1 sequence) */
    CHECK(su_input_key_from_hid(0x00) == 0u);
    CHECK(su_input_key_from_hid(0xE8) == 0u);
    CHECK(su_input_key_from_hid(0x10000u) == 0u);
}

static void test_apply(void) {
    SuInputState s;
    SuInputState o;
    SuInputState before;
    SuInputEvent e;
    memset(&s, 0, sizeof s);
    s.focused = 1u;

    /* keys */
    e = ev(SU_INPUT_EV_KEY, 0x1A, 0, 1, 100);
    CHECK(su_input_state_apply(&s, &e) == SU_OK);
    CHECK(su_input_key_down(&s, SU_KEY_W) && s.key_ns == 100u);
    e = ev(SU_INPUT_EV_KEY, 0x48, 0, 1, 150); /* unmapped: ignored */
    CHECK(su_input_state_apply(&s, &e) == SU_OK && s.key_ns == 100u);
    e = ev(SU_INPUT_EV_KEY, 0x1A, 0, 0, 200);
    CHECK(su_input_state_apply(&s, &e) == SU_OK && !su_input_key_down(&s, SU_KEY_W) && s.key_ns == 200u);

    /* mouse: accumulation, saturation, buttons, wheel */
    e = motion(5, -3, 300);
    CHECK(su_input_state_apply(&s, &e) == SU_OK);
    e = motion(2, 1, 310);
    CHECK(su_input_state_apply(&s, &e) == SU_OK);
    CHECK(s.mouse_dx == 7 && s.mouse_dy == -2 && s.mouse_ns == 310u);
    e = motion(INT32_MAX, INT32_MIN, 320);
    CHECK(su_input_state_apply(&s, &e) == SU_OK);
    CHECK(s.mouse_dx == INT32_MAX && s.mouse_dy == INT32_MIN);
    e = ev(SU_INPUT_EV_WHEEL, 0, 0, 120, 330);
    CHECK(su_input_state_apply(&s, &e) == SU_OK && s.wheel == 120);
    e = ev(SU_INPUT_EV_WHEEL, 0, 0, -360, 331);
    CHECK(su_input_state_apply(&s, &e) == SU_OK && s.wheel == -240);
    e = ev(SU_INPUT_EV_MOUSE_BUTTON, SU_INPUT_MOUSE_RIGHT, 0, 1, 340);
    CHECK(su_input_state_apply(&s, &e) == SU_OK && s.mouse_buttons == SU_INPUT_MOUSE_RIGHT);
    before = s;
    e = ev(SU_INPUT_EV_MOUSE_BUTTON, SU_INPUT_MOUSE_LEFT | SU_INPUT_MOUSE_RIGHT, 0, 1, 341);
    CHECK(su_input_state_apply(&s, &e) == SU_E_INVALID_ARGUMENT);
    e = ev(SU_INPUT_EV_MOUSE_BUTTON, 0x20u, 0, 1, 342);
    CHECK(su_input_state_apply(&s, &e) == SU_E_INVALID_ARGUMENT);
    e = ev(SU_INPUT_EV_MOUSE_BUTTON, 0u, 0, 1, 343);
    CHECK(su_input_state_apply(&s, &e) == SU_E_INVALID_ARGUMENT);
    e = ev(99u, 0u, 0, 1, 344);
    CHECK(su_input_state_apply(&s, &e) == SU_E_INVALID_ARGUMENT);
    CHECK(memcmp(&before, &s, sizeof s) == 0);
    CHECK(su_input_state_apply(NULL, &e) == SU_E_INVALID_ARGUMENT && su_input_state_apply(&s, NULL) == SU_E_INVALID_ARGUMENT);

    /* sampling clears the relative quantities only */
    su_input_state_take(&s, 400, &o);
    CHECK(o.sample_ns == 400u && o.mouse_dx == INT32_MAX && o.wheel == -240 && o.mouse_buttons == SU_INPUT_MOUSE_RIGHT);
    CHECK(s.mouse_dx == 0 && s.mouse_dy == 0 && s.wheel == 0 && s.mouse_buttons == SU_INPUT_MOUSE_RIGHT);

    /* focus loss releases keys and buttons once, and is reported by exactly one sample */
    e = ev(SU_INPUT_EV_KEY, 0xE1, 0, 1, 410);
    (void)su_input_state_apply(&s, &e);
    e = ev(SU_INPUT_EV_FOCUS, 0, 0, 0, 420);
    CHECK(su_input_state_apply(&s, &e) == SU_OK);
    CHECK(!su_input_key_down(&s, SU_KEY_LSHIFT) && s.mouse_buttons == 0u && s.focused == 0u);
    su_input_state_take(&s, 430, &o);
    CHECK((o.flags & SU_INPUT_FLAG_FOCUS_LOST) != 0u && o.focused == 0u);
    su_input_state_take(&s, 440, &o);
    CHECK((o.flags & SU_INPUT_FLAG_FOCUS_LOST) == 0u);
    e = ev(SU_INPUT_EV_FOCUS, 0, 0, 1, 450);
    CHECK(su_input_state_apply(&s, &e) == SU_OK && s.focused == 1u);

    /* gamepads: nothing before connect; clamping; refusals; removal resets the slot */
    e = ev(SU_INPUT_EV_PAD_BUTTON, SU_PAD_A, 1, 1, 500);
    CHECK(su_input_state_apply(&s, &e) == SU_OK && s.pads[1].buttons == 0u && !s.pads[1].connected);
    e = ev(SU_INPUT_EV_PAD_CONNECT, 0, 1, 1, 510);
    CHECK(su_input_state_apply(&s, &e) == SU_OK && s.pads[1].connected == 1u && s.pads[1].changed_ns == 510u);
    {
        uint32_t p0 = s.pads[1].packet;
        e = ev(SU_INPUT_EV_PAD_BUTTON, SU_PAD_A, 1, 1, 520);
        CHECK(su_input_state_apply(&s, &e) == SU_OK && s.pads[1].buttons == SU_PAD_A && s.pads[1].packet != p0);
    }
    e = ev(SU_INPUT_EV_PAD_AXIS, SU_INPUT_PAD_AXIS_LX, 1, 40000, 530);
    CHECK(su_input_state_apply(&s, &e) == SU_OK && s.pads[1].lx == 32767);
    e = ev(SU_INPUT_EV_PAD_AXIS, SU_INPUT_PAD_AXIS_RY, 1, -40000, 531);
    CHECK(su_input_state_apply(&s, &e) == SU_OK && s.pads[1].ry == -32768);
    e = ev(SU_INPUT_EV_PAD_AXIS, SU_INPUT_PAD_AXIS_LT, 1, 300, 532);
    CHECK(su_input_state_apply(&s, &e) == SU_OK && s.pads[1].lt == 255u);
    e = ev(SU_INPUT_EV_PAD_AXIS, SU_INPUT_PAD_AXIS_RT, 1, -5, 533);
    CHECK(su_input_state_apply(&s, &e) == SU_OK && s.pads[1].rt == 0u);
    before = s;
    e = ev(SU_INPUT_EV_PAD_AXIS, 6u, 1, 1, 540);
    CHECK(su_input_state_apply(&s, &e) == SU_E_INVALID_ARGUMENT);
    e = ev(SU_INPUT_EV_PAD_BUTTON, 0x0400u, 1, 1, 541); /* not an XInput button bit */
    CHECK(su_input_state_apply(&s, &e) == SU_E_INVALID_ARGUMENT);
    e = ev(SU_INPUT_EV_PAD_BUTTON, SU_PAD_A, SU_INPUT_MAX_PADS, 1, 542);
    CHECK(su_input_state_apply(&s, &e) == SU_E_INVALID_ARGUMENT);
    CHECK(memcmp(&before, &s, sizeof s) == 0);
    e = ev(SU_INPUT_EV_PAD_CONNECT, 0, 1, 0, 550);
    CHECK(su_input_state_apply(&s, &e) == SU_OK);
    CHECK(s.pads[1].connected == 0u && s.pads[1].buttons == 0u && s.pads[1].lx == 0 && s.pads[1].lt == 0u &&
          s.pads[1].changed_ns == 550u);
}

/* ---- cross-platform identity ---- */

#define FRAMES 60u
#define TICKS_PER_FRAME 3u

/* The Win32 backend's view of frame f: set 1 codes, raw counts, XInput state (written directly). */
static void win32_frame(uint32_t f, SuInputState *s) {
    memset(s, 0, sizeof *s);
    s->focused = 1u;
    su_input_set_key(s, SU_KEY_W, f < 30u);
    su_input_set_key(s, SU_KEY_LSHIFT, f >= 10u && f < 20u);
    su_input_set_key(s, SU_KEY_RIGHT, f >= 20u && f < 40u);
    su_input_set_key(s, SU_KEY_E, f >= 40u && f < 50u);
    if (f < 40u) {
        s->mouse_dx = 7;
        s->mouse_dy = -3;
    }
    if (f >= 30u && f < 36u) {
        s->mouse_buttons = SU_INPUT_MOUSE_LEFT;
    }
    if (f >= 5u) {
        s->pads[0].connected = 1u;
        s->pads[0].ly = f < 26u ? 20000 : 0;
        s->pads[0].rx = f >= 25u && f < 35u ? -32768 : 0;
        s->pads[0].rt = f >= 15u && f < 31u ? 200u : 0u;
        s->pads[0].buttons = (f >= 12u && f < 15u ? SU_PAD_A : 0u) | (f == 50u ? SU_PAD_Y : 0u);
    }
}

/* The same physical input as events (HID usages, SDL-style edges): applied to a live state, sampled per frame. */
static void linux_frame(uint32_t f, SuInputState *live, SuInputState *out) {
    uint64_t t = 1000u + f;
    SuInputEvent e;
#define APPLY(x)                                     \
    do {                                             \
        e = (x);                                     \
        CHECK(su_input_state_apply(live, &e) == SU_OK); \
    } while (0)
    if (f == 0u) {
        APPLY(ev(SU_INPUT_EV_FOCUS, 0, 0, 1, t));
        APPLY(ev(SU_INPUT_EV_KEY, 0x1A, 0, 1, t)); /* W */
    }
    if (f == 30u) APPLY(ev(SU_INPUT_EV_KEY, 0x1A, 0, 0, t));
    if (f == 10u) APPLY(ev(SU_INPUT_EV_KEY, 0xE1, 0, 1, t)); /* left shift */
    if (f == 20u) APPLY(ev(SU_INPUT_EV_KEY, 0xE1, 0, 0, t));
    if (f == 20u) APPLY(ev(SU_INPUT_EV_KEY, 0x4F, 0, 1, t)); /* right arrow */
    if (f == 40u) APPLY(ev(SU_INPUT_EV_KEY, 0x4F, 0, 0, t));
    if (f == 40u) APPLY(ev(SU_INPUT_EV_KEY, 0x08, 0, 1, t)); /* E */
    if (f == 50u) APPLY(ev(SU_INPUT_EV_KEY, 0x08, 0, 0, t));
    if (f < 40u) { /* two motion reports per frame */
        APPLY(motion(4, -1, t));
        APPLY(motion(3, -2, t));
    }
    if (f == 30u) APPLY(ev(SU_INPUT_EV_MOUSE_BUTTON, SU_INPUT_MOUSE_LEFT, 0, 1, t));
    if (f == 36u) APPLY(ev(SU_INPUT_EV_MOUSE_BUTTON, SU_INPUT_MOUSE_LEFT, 0, 0, t));
    if (f == 5u) {
        APPLY(ev(SU_INPUT_EV_PAD_CONNECT, 0, 0, 1, t));
        APPLY(ev(SU_INPUT_EV_PAD_AXIS, SU_INPUT_PAD_AXIS_LY, 0, 20000, t));
    }
    if (f == 26u) APPLY(ev(SU_INPUT_EV_PAD_AXIS, SU_INPUT_PAD_AXIS_LY, 0, 0, t));
    if (f == 25u) APPLY(ev(SU_INPUT_EV_PAD_AXIS, SU_INPUT_PAD_AXIS_RX, 0, -32768, t));
    if (f == 35u) APPLY(ev(SU_INPUT_EV_PAD_AXIS, SU_INPUT_PAD_AXIS_RX, 0, 0, t));
    if (f == 15u) APPLY(ev(SU_INPUT_EV_PAD_AXIS, SU_INPUT_PAD_AXIS_RT, 0, 200, t));
    if (f == 31u) APPLY(ev(SU_INPUT_EV_PAD_AXIS, SU_INPUT_PAD_AXIS_RT, 0, 0, t));
    if (f == 12u) APPLY(ev(SU_INPUT_EV_PAD_BUTTON, SU_PAD_A, 0, 1, t));
    if (f == 15u) APPLY(ev(SU_INPUT_EV_PAD_BUTTON, SU_PAD_A, 0, 0, t));
    if (f == 50u) APPLY(ev(SU_INPUT_EV_PAD_BUTTON, SU_PAD_Y, 0, 1, t));
    if (f == 51u) APPLY(ev(SU_INPUT_EV_PAD_BUTTON, SU_PAD_Y, 0, 0, t));
#undef APPLY
    su_input_state_take(live, t, out);
}

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *b;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    b = malloc((size_t)n + 1u);
    if (b == NULL || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static void test_identity(void) {
    size_t len = 0;
    uint8_t *json = slurp("examples/input/default_map.json", &len);
    SuInputMap map;
    SuInputMapper mw;
    SuInputMapper ml;
    SuInputState live;
    char msg[160];
    uint32_t f;
    uint32_t k;
    uint32_t differ = 0;
    uint32_t nonzero_axes = 0;
    uint32_t buttons_seen = 0;
    uint64_t tick = 0;
    CHECK(su_input_map_from_json(json, len, &map, msg) == SU_OK);
    free(json);
    CHECK(su_input_mapper_init(&mw, &map) == SU_OK && su_input_mapper_init(&ml, &map) == SU_OK);
    memset(&live, 0, sizeof live);
    for (f = 0; f < FRAMES; f++) {
        SuInputState sw;
        SuInputState sl;
        win32_frame(f, &sw);
        linux_frame(f, &live, &sl);
        for (k = 0; k < TICKS_PER_FRAME; k++) {
            SuInputState pw;
            SuInputState pl;
            SuInputRecord rw;
            SuInputRecord rl;
            uint8_t bw[SU_INPUT_RECORD_BYTES];
            uint8_t bl[SU_INPUT_RECORD_BYTES];
            uint32_t a;
            CHECK(su_input_state_split(&sw, TICKS_PER_FRAME, k, &pw) == SU_OK);
            CHECK(su_input_state_split(&sl, TICKS_PER_FRAME, k, &pl) == SU_OK);
            CHECK(su_input_mapper_tick(&mw, &pw, tick, &rw) == SU_OK);
            CHECK(su_input_mapper_tick(&ml, &pl, tick, &rl) == SU_OK);
            su_input_record_encode(&rw, bw);
            su_input_record_encode(&rl, bl);
            if (memcmp(bw, bl, sizeof bw) != 0) {
                differ++;
                if (differ <= 3u) {
                    fprintf(stderr, "  frame %u tick %llu: records differ (buttons %x / %x)\n", (unsigned)f,
                            (unsigned long long)tick, (unsigned)rw.buttons, (unsigned)rl.buttons);
                }
            }
            for (a = 0; a < rw.axis_count; a++) {
                nonzero_axes += rw.axes[a] != 0;
            }
            buttons_seen |= rw.buttons;
            tick++;
        }
    }
    CHECK(differ == 0u);
    /* the scenario exercises the map: every axis moved at some point, and boost, camera_next were pressed */
    CHECK(nonzero_axes > 100u);
    CHECK((buttons_seen & 1u) != 0u && (buttons_seen & 4u) != 0u);
    printf("identity: %llu ticks, %u differing records, %u non-zero axis values, buttons seen 0x%x\n",
           (unsigned long long)tick, (unsigned)differ, (unsigned)nonzero_axes, (unsigned)buttons_seen);
}

int main(void) {
    test_hid_table();
    test_apply();
    test_identity();
    printf("test_input_events: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
