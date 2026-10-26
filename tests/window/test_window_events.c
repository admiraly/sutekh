/* SDL3 event translation of the Linux window backend (src/platform/window_sdl3.c, su_window_sdl3_translate) with
 * synthetic SDL events built from the pinned SDL3 headers: no SDL library and no display are needed. Covers keys
 * (SDL scancodes are HID usages), mouse motion and wheel with carried fractions (nothing lost or invented), the
 * physical wheel direction under "natural" scrolling, mouse buttons, focus, gamepad slot assignment and removal,
 * the stick y flip to XInput's +up, trigger scaling, unmapped buttons; and that a state built from SDL events
 * gives the same record as the equivalent Win32 state through the shipped default map.
 *
 * Usage: test_window_events   (run from the repository root: it reads examples/input/default_map.json) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_events.h>

#include "platform/su_window_sdl3_int.h"
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

static SuSdlTranslate g_t;
static SuInputEvent g_out[SU_SDL_MAX_EVENTS_PER_SDL_EVENT];

static uint32_t tr(const SDL_Event *e) {
    return su_window_sdl3_translate(&g_t, e, 42u, g_out, NULL);
}

static SDL_Event key(SDL_Scancode sc, int down) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    e.key.down = down != 0;
    return e;
}

static SDL_Event mmove(float x, float y) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.xrel = x;
    e.motion.yrel = y;
    return e;
}

static SDL_Event wheel(float y, int flipped) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.y = y;
    e.wheel.direction = flipped ? SDL_MOUSEWHEEL_FLIPPED : SDL_MOUSEWHEEL_NORMAL;
    return e;
}

static SDL_Event pad_dev(Uint32 type, SDL_JoystickID id) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.gdevice.which = id;
    return e;
}

static SDL_Event pad_axis(SDL_JoystickID id, SDL_GamepadAxis axis, Sint16 v) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = id;
    e.gaxis.axis = (Uint8)axis;
    e.gaxis.value = v;
    return e;
}

static SDL_Event pad_button(SDL_JoystickID id, SDL_GamepadButton b, int down) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.which = id;
    e.gbutton.button = (Uint8)b;
    e.gbutton.down = down != 0;
    return e;
}

static void test_keys_mouse(void) {
    SDL_Event e;
    int64_t sum = 0;
    uint32_t i;
    memset(&g_t, 0, sizeof g_t);
    e = key(SDL_SCANCODE_W, 1);
    CHECK(tr(&e) == 1u && g_out[0].type == SU_INPUT_EV_KEY && g_out[0].code == 0x1Au && g_out[0].value == 1 &&
          g_out[0].time_ns == 42u);
    CHECK(su_input_key_from_hid(g_out[0].code) == SU_KEY_W);
    e = key(SDL_SCANCODE_RALT, 0);
    CHECK(tr(&e) == 1u && su_input_key_from_hid(g_out[0].code) == SU_KEY_RALT && g_out[0].value == 0);

    /* fractions carry: 0.25 x 3 -> nothing, then 1.0 total at the fourth */
    e = mmove(0.25f, 0.0f);
    CHECK(tr(&e) == 0u);
    CHECK(tr(&e) == 0u);
    CHECK(tr(&e) == 0u);
    CHECK(tr(&e) == 1u && g_out[0].type == SU_INPUT_EV_MOUSE_MOTION && g_out[0].x == 1 && g_out[0].y == 0);
    e = mmove(-0.5f, 3.0f); /* truncated: 0, remainder -0.5 */
    CHECK(tr(&e) == 1u && g_out[0].x == 0 && g_out[0].y == 3);
    e = mmove(-0.75f, 0.0f); /* -1.25: -1, remainder -0.25 */
    CHECK(tr(&e) == 1u && g_out[0].x == -1 && g_out[0].y == 0);
    e = mmove(0.25f, 0.0f); /* back to 0: nothing */
    CHECK(tr(&e) == 0u);
    /* conservation over many exact binary fractions: emitted + carried remainder = sum of inputs, exactly */
    memset(&g_t, 0, sizeof g_t);
    for (i = 0; i < 1000u; i++) {
        float v = (float)((int)(i % 7u) - 3) * 0.375f; /* -1.125 .. 1.125 */
        e = mmove(v, 0.0f);
        if (tr(&e) == 1u) {
            sum += g_out[0].x;
        }
    }
    {
        double total = 0.0;
        for (i = 0; i < 1000u; i++) {
            total += (double)((int)(i % 7u) - 3) * 0.375;
        }
        CHECK(total - (double)sum == g_t.rem_x && g_t.rem_x > -1.0 && g_t.rem_x < 1.0);
    }

    /* wheel: 120 per notch, physical direction */
    memset(&g_t, 0, sizeof g_t);
    e = wheel(1.0f, 0);
    CHECK(tr(&e) == 1u && g_out[0].type == SU_INPUT_EV_WHEEL && g_out[0].value == 120);
    e = wheel(1.0f, 1);
    CHECK(tr(&e) == 1u && g_out[0].value == -120);
    e = wheel(0.5f, 0);
    CHECK(tr(&e) == 1u && g_out[0].value == 60);
    e = wheel(0.004f, 0); /* 0.48 of a 120th: carried */
    CHECK(tr(&e) == 0u);
    e = wheel(0.005f, 0); /* 0.48 + 0.6 = 1.08 */
    CHECK(tr(&e) == 1u && g_out[0].value == 1);

    /* buttons */
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.button = SDL_BUTTON_X2;
    e.button.down = true;
    CHECK(tr(&e) == 1u && g_out[0].type == SU_INPUT_EV_MOUSE_BUTTON && g_out[0].code == SU_INPUT_MOUSE_X2 &&
          g_out[0].value == 1);
    e.button.button = SDL_BUTTON_MIDDLE;
    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.down = false;
    CHECK(tr(&e) == 1u && g_out[0].code == SU_INPUT_MOUSE_MIDDLE && g_out[0].value == 0);
    e.button.button = 9;
    CHECK(tr(&e) == 0u);

    /* focus */
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    CHECK(tr(&e) == 1u && g_out[0].type == SU_INPUT_EV_FOCUS && g_out[0].value == 0);
    e.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    CHECK(tr(&e) == 1u && g_out[0].value == 1);
    /* events without input meaning */
    e.type = SDL_EVENT_WINDOW_RESIZED;
    CHECK(tr(&e) == 0u);
    CHECK(su_window_sdl3_translate(NULL, &e, 0, g_out, NULL) == 0u);
}

static void test_pads(void) {
    SDL_Event e;
    uint32_t slot = 0;
    memset(&g_t, 0, sizeof g_t);
    e = pad_axis(17, SDL_GAMEPAD_AXIS_LEFTX, 100);
    CHECK(tr(&e) == 0u); /* unknown pad */
    e = pad_dev(SDL_EVENT_GAMEPAD_ADDED, 17);
    CHECK(su_window_sdl3_translate(&g_t, &e, 1, g_out, &slot) == 1u && slot == 0u);
    CHECK(g_out[0].type == SU_INPUT_EV_PAD_CONNECT && g_out[0].device == 0u && g_out[0].value == 1);
    CHECK(su_window_sdl3_translate(&g_t, &e, 1, g_out, &slot) == 0u && slot == UINT32_MAX); /* again: ignored */
    e = pad_dev(SDL_EVENT_GAMEPAD_ADDED, 23);
    CHECK(su_window_sdl3_translate(&g_t, &e, 1, g_out, &slot) == 1u && slot == 1u);
    e = pad_dev(SDL_EVENT_GAMEPAD_ADDED, 31);
    CHECK(su_window_sdl3_translate(&g_t, &e, 1, g_out, &slot) == 1u && slot == 2u);
    e = pad_dev(SDL_EVENT_GAMEPAD_ADDED, 32);
    CHECK(su_window_sdl3_translate(&g_t, &e, 1, g_out, &slot) == 1u && slot == 3u);
    e = pad_dev(SDL_EVENT_GAMEPAD_ADDED, 33);
    CHECK(su_window_sdl3_translate(&g_t, &e, 1, g_out, &slot) == 0u && slot == UINT32_MAX); /* fifth: no slot */

    /* sticks: SDL y is +down, XInput +up; -(-32768) clamps to 32767 */
    e = pad_axis(23, SDL_GAMEPAD_AXIS_LEFTY, -32768);
    CHECK(tr(&e) == 1u && g_out[0].type == SU_INPUT_EV_PAD_AXIS && g_out[0].device == 1u &&
          g_out[0].code == SU_INPUT_PAD_AXIS_LY && g_out[0].value == 32767);
    e = pad_axis(23, SDL_GAMEPAD_AXIS_LEFTY, 32767);
    CHECK(tr(&e) == 1u && g_out[0].value == -32767);
    e = pad_axis(23, SDL_GAMEPAD_AXIS_RIGHTY, 1000);
    CHECK(tr(&e) == 1u && g_out[0].code == SU_INPUT_PAD_AXIS_RY && g_out[0].value == -1000);
    e = pad_axis(23, SDL_GAMEPAD_AXIS_LEFTX, -32768);
    CHECK(tr(&e) == 1u && g_out[0].code == SU_INPUT_PAD_AXIS_LX && g_out[0].value == -32768);
    e = pad_axis(23, SDL_GAMEPAD_AXIS_RIGHTX, 5);
    CHECK(tr(&e) == 1u && g_out[0].code == SU_INPUT_PAD_AXIS_RX && g_out[0].value == 5);
    /* triggers: 0..32767 -> 0..255 (value >> 7) */
    e = pad_axis(17, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    CHECK(tr(&e) == 1u && g_out[0].device == 0u && g_out[0].code == SU_INPUT_PAD_AXIS_RT && g_out[0].value == 255);
    e = pad_axis(17, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 128);
    CHECK(tr(&e) == 1u && g_out[0].code == SU_INPUT_PAD_AXIS_LT && g_out[0].value == 1);
    e = pad_axis(17, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 127);
    CHECK(tr(&e) == 1u && g_out[0].value == 0);
    /* buttons: positional (south = A), XInput bits; buttons XInput lacks are dropped */
    e = pad_button(17, SDL_GAMEPAD_BUTTON_SOUTH, 1);
    CHECK(tr(&e) == 1u && g_out[0].type == SU_INPUT_EV_PAD_BUTTON && g_out[0].code == SU_PAD_A && g_out[0].value == 1);
    e = pad_button(17, SDL_GAMEPAD_BUTTON_NORTH, 0);
    CHECK(tr(&e) == 1u && g_out[0].code == SU_PAD_Y && g_out[0].value == 0);
    e = pad_button(17, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 1);
    CHECK(tr(&e) == 1u && g_out[0].code == SU_PAD_RSHOULDER);
    e = pad_button(17, SDL_GAMEPAD_BUTTON_DPAD_LEFT, 1);
    CHECK(tr(&e) == 1u && g_out[0].code == SU_PAD_DPAD_LEFT);
    e = pad_button(17, SDL_GAMEPAD_BUTTON_BACK, 1);
    CHECK(tr(&e) == 1u && g_out[0].code == SU_PAD_BACK);
    e = pad_button(17, SDL_GAMEPAD_BUTTON_GUIDE, 1);
    CHECK(tr(&e) == 0u);
    e = pad_button(17, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, 1);
    CHECK(tr(&e) == 0u);
    /* removal frees the slot; the next pad takes it */
    e = pad_dev(SDL_EVENT_GAMEPAD_REMOVED, 17);
    CHECK(su_window_sdl3_translate(&g_t, &e, 1, g_out, &slot) == 1u && slot == 0u && g_out[0].value == 0 &&
          g_out[0].type == SU_INPUT_EV_PAD_CONNECT);
    e = pad_button(17, SDL_GAMEPAD_BUTTON_SOUTH, 1);
    CHECK(tr(&e) == 0u);
    e = pad_dev(SDL_EVENT_GAMEPAD_ADDED, 33);
    CHECK(su_window_sdl3_translate(&g_t, &e, 1, g_out, &slot) == 1u && slot == 0u);
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

static void feed(SuInputState *s, const SDL_Event *e) {
    uint32_t n = su_window_sdl3_translate(&g_t, e, 7u, g_out, NULL);
    uint32_t i;
    for (i = 0; i < n; i++) {
        CHECK(su_input_state_apply(s, &g_out[i]) == SU_OK);
    }
}

/* SDL events -> state -> record equals the Win32 state's record (default map, one tick). */
static void test_record_identity(void) {
    size_t len = 0;
    uint8_t *json = slurp("examples/input/default_map.json", &len);
    SuInputMap map;
    SuInputMapper ma;
    SuInputMapper mb;
    SuInputState live;
    SuInputState a;
    SuInputState b;
    SuInputRecord ra;
    SuInputRecord rb;
    uint8_t ba[SU_INPUT_RECORD_BYTES];
    uint8_t bb[SU_INPUT_RECORD_BYTES];
    SDL_Event e;
    char msg[160];
    CHECK(su_input_map_from_json(json, len, &map, msg) == SU_OK);
    free(json);
    CHECK(su_input_mapper_init(&ma, &map) == SU_OK && su_input_mapper_init(&mb, &map) == SU_OK);
    memset(&g_t, 0, sizeof g_t);
    memset(&live, 0, sizeof live);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    feed(&live, &e);
    e = key(SDL_SCANCODE_W, 1);
    feed(&live, &e);
    e = key(SDL_SCANCODE_LSHIFT, 1);
    feed(&live, &e);
    e = key(SDL_SCANCODE_RIGHT, 1);
    feed(&live, &e);
    e = mmove(10.5f, -4.25f);
    feed(&live, &e);
    e = mmove(9.5f, -0.75f); /* 20, -5 in total */
    feed(&live, &e);
    e = pad_dev(SDL_EVENT_GAMEPAD_ADDED, 5);
    feed(&live, &e);
    e = pad_axis(5, SDL_GAMEPAD_AXIS_LEFTY, -20000); /* pushed up */
    feed(&live, &e);
    e = pad_axis(5, SDL_GAMEPAD_AXIS_LEFTX, 16000);
    feed(&live, &e);
    e = pad_axis(5, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 25600); /* 200 */
    feed(&live, &e);
    e = pad_button(5, SDL_GAMEPAD_BUTTON_SOUTH, 1);
    feed(&live, &e);
    su_input_state_take(&live, 99u, &a);

    memset(&b, 0, sizeof b); /* what Raw Input + XInput give for the same hands */
    b.focused = 1u;
    su_input_set_key(&b, SU_KEY_W, 1);
    su_input_set_key(&b, SU_KEY_LSHIFT, 1);
    su_input_set_key(&b, SU_KEY_RIGHT, 1);
    b.mouse_dx = 20;
    b.mouse_dy = -5;
    b.pads[0].connected = 1u;
    b.pads[0].ly = 20000;
    b.pads[0].lx = 16000;
    b.pads[0].rt = 200u;
    b.pads[0].buttons = SU_PAD_A;
    CHECK(a.mouse_dx == 20 && a.mouse_dy == -5 && a.pads[0].ly == 20000 && a.pads[0].rt == 200u &&
          memcmp(a.keys, b.keys, sizeof a.keys) == 0);
    CHECK(su_input_mapper_tick(&ma, &a, 0, &ra) == SU_OK && su_input_mapper_tick(&mb, &b, 0, &rb) == SU_OK);
    su_input_record_encode(&ra, ba);
    su_input_record_encode(&rb, bb);
    CHECK(memcmp(ba, bb, sizeof ba) == 0);
    CHECK(ra.buttons != 0u && ra.axes[0] != 0 && ra.axes[3] != 0);
}

int main(void) {
    test_keys_mouse();
    test_pads();
    test_record_identity();
    printf("test_window_events: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
