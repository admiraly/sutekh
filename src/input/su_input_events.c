/* Portable device events of sutekh/input.h: USB HID usage to scan code set 1, and the application of discrete
 * device events to a live SuInputState with the Win32 backend's conventions. No allocation, no floating point. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "sutekh/input.h"

#include <string.h>

/* USB HID Usage Tables, keyboard/keypad page (0x07), to scan code set 1 make codes (0x100 + code for E0-prefixed
 * keys). Zero: no set 1 position (Pause is an E1 sequence, which Raw Input does not report either). */
static const uint16_t k_hid_to_set1[0xE8] = {
    /* 0x04 a .. 0x1D z */
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20, [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22,
    [0x0B] = 0x23, [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26, [0x10] = 0x32, [0x11] = 0x31,
    [0x12] = 0x18, [0x13] = 0x19, [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14, [0x18] = 0x16,
    [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D, [0x1C] = 0x15, [0x1D] = 0x2C,
    /* 0x1E 1 .. 0x27 0 */
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08,
    [0x25] = 0x09, [0x26] = 0x0A, [0x27] = 0x0B,
    [0x28] = 0x1C,  /* enter */
    [0x29] = 0x01,  /* escape */
    [0x2A] = 0x0E,  /* backspace */
    [0x2B] = 0x0F,  /* tab */
    [0x2C] = 0x39,  /* space */
    [0x2D] = 0x0C, [0x2E] = 0x0D, [0x2F] = 0x1A, [0x30] = 0x1B, [0x31] = 0x2B,
    [0x32] = 0x2B,  /* ISO # (left of enter): the backslash position */
    [0x33] = 0x27, [0x34] = 0x28, [0x35] = 0x29, [0x36] = 0x33, [0x37] = 0x34, [0x38] = 0x35,
    [0x39] = 0x3A,  /* caps lock */
    /* F1 .. F12 */
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E, [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41,
    [0x41] = 0x42, [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,
    [0x46] = 0x137, /* print screen */
    [0x47] = 0x46,  /* scroll lock */
    [0x49] = 0x152, [0x4A] = 0x147, [0x4B] = 0x149, [0x4C] = 0x153, [0x4D] = 0x14F, [0x4E] = 0x151,
    [0x4F] = 0x14D, [0x50] = 0x14B, [0x51] = 0x150, [0x52] = 0x148,
    [0x53] = 0x45,  /* num lock */
    [0x54] = 0x135, [0x55] = 0x37, [0x56] = 0x4A, [0x57] = 0x4E, [0x58] = 0x11C,
    [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51, [0x5C] = 0x4B, [0x5D] = 0x4C, [0x5E] = 0x4D, [0x5F] = 0x47,
    [0x60] = 0x48, [0x61] = 0x49, [0x62] = 0x52, [0x63] = 0x53,
    [0x64] = 0x56,  /* ISO key between left shift and z */
    [0x65] = 0x15D, /* application (menu) */
    [0x66] = 0x15E, /* power */
    [0x67] = 0x59,  /* keypad = */
    /* F13 .. F24 */
    [0x68] = 0x64, [0x69] = 0x65, [0x6A] = 0x66, [0x6B] = 0x67, [0x6C] = 0x68, [0x6D] = 0x69, [0x6E] = 0x6A,
    [0x6F] = 0x6B, [0x70] = 0x6C, [0x71] = 0x6D, [0x72] = 0x6E, [0x73] = 0x76,
    /* international 1..5 (Ro, Katakana/Hiragana, Yen, Henkan, Muhenkan) */
    [0x87] = 0x73, [0x88] = 0x70, [0x89] = 0x7D, [0x8A] = 0x79, [0x8B] = 0x7B,
    /* modifiers */
    [0xE0] = 0x1D, [0xE1] = 0x2A, [0xE2] = 0x38, [0xE3] = 0x15B, [0xE4] = 0x11D, [0xE5] = 0x36, [0xE6] = 0x138,
    [0xE7] = 0x15C,
};

uint32_t su_input_key_from_hid(uint32_t usage) {
    return usage < sizeof k_hid_to_set1 / sizeof k_hid_to_set1[0] ? k_hid_to_set1[usage] : 0u;
}

static int one_bit(uint32_t v, uint32_t allowed) {
    return v != 0u && (v & (v - 1u)) == 0u && (v & ~allowed) == 0u;
}

static int32_t sat_add(int32_t a, int32_t b) {
    int64_t s = (int64_t)a + (int64_t)b;
    return s > INT32_MAX ? INT32_MAX : (s < INT32_MIN ? INT32_MIN : (int32_t)s);
}

#define MOUSE_BITS (SU_INPUT_MOUSE_LEFT | SU_INPUT_MOUSE_RIGHT | SU_INPUT_MOUSE_MIDDLE | SU_INPUT_MOUSE_X1 | SU_INPUT_MOUSE_X2)
#define PAD_BITS                                                                                                  \
    (SU_PAD_DPAD_UP | SU_PAD_DPAD_DOWN | SU_PAD_DPAD_LEFT | SU_PAD_DPAD_RIGHT | SU_PAD_START | SU_PAD_BACK |       \
     SU_PAD_LTHUMB | SU_PAD_RTHUMB | SU_PAD_LSHOULDER | SU_PAD_RSHOULDER | SU_PAD_A | SU_PAD_B | SU_PAD_X | SU_PAD_Y)

static int16_t clamp16(int32_t v) {
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

static uint8_t clamp8(int32_t v) {
    return (uint8_t)(v > 255 ? 255 : (v < 0 ? 0 : v));
}

uint32_t su_input_state_apply(SuInputState *s, const SuInputEvent *e) {
    SuInputPad *p;
    if (s == NULL || e == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    switch (e->type) {
    case SU_INPUT_EV_KEY: {
        uint32_t key = su_input_key_from_hid(e->code);
        if (key != 0u) {
            su_input_set_key(s, key, e->value != 0);
            s->key_ns = e->time_ns;
        }
        return SU_OK;
    }
    case SU_INPUT_EV_MOUSE_MOTION:
        s->mouse_dx = sat_add(s->mouse_dx, e->x);
        s->mouse_dy = sat_add(s->mouse_dy, e->y);
        s->mouse_ns = e->time_ns;
        return SU_OK;
    case SU_INPUT_EV_MOUSE_BUTTON:
        if (!one_bit(e->code, MOUSE_BITS)) {
            return SU_E_INVALID_ARGUMENT;
        }
        if (e->value != 0) {
            s->mouse_buttons |= e->code;
        } else {
            s->mouse_buttons &= ~e->code;
        }
        s->mouse_ns = e->time_ns;
        return SU_OK;
    case SU_INPUT_EV_WHEEL:
        s->wheel = sat_add(s->wheel, e->value);
        s->mouse_ns = e->time_ns;
        return SU_OK;
    case SU_INPUT_EV_FOCUS:
        if (e->value == 0) {
            memset(s->keys, 0, sizeof s->keys);
            s->mouse_buttons = 0;
            s->flags |= SU_INPUT_FLAG_FOCUS_LOST;
        }
        s->focused = e->value != 0 ? 1u : 0u;
        return SU_OK;
    default:
        break;
    }
    if (e->type < SU_INPUT_EV_PAD_CONNECT || e->type > SU_INPUT_EV_PAD_AXIS || e->device >= SU_INPUT_MAX_PADS) {
        return SU_E_INVALID_ARGUMENT;
    }
    p = &s->pads[e->device];
    if (e->type == SU_INPUT_EV_PAD_CONNECT) {
        uint32_t packet = p->packet;
        memset(p, 0, sizeof *p);
        p->connected = e->value != 0 ? 1u : 0u;
        p->packet = p->connected ? packet + 1u : 0u;
        p->changed_ns = e->time_ns;
        return SU_OK;
    }
    if (e->type == SU_INPUT_EV_PAD_BUTTON && !one_bit(e->code, PAD_BITS)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (e->type == SU_INPUT_EV_PAD_AXIS && e->code > SU_INPUT_PAD_AXIS_RT) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (!p->connected) {
        return SU_OK;
    }
    if (e->type == SU_INPUT_EV_PAD_BUTTON) {
        if (e->value != 0) {
            p->buttons |= e->code;
        } else {
            p->buttons &= ~e->code;
        }
    } else {
        switch (e->code) {
        case SU_INPUT_PAD_AXIS_LX: p->lx = clamp16(e->value); break;
        case SU_INPUT_PAD_AXIS_LY: p->ly = clamp16(e->value); break;
        case SU_INPUT_PAD_AXIS_RX: p->rx = clamp16(e->value); break;
        case SU_INPUT_PAD_AXIS_RY: p->ry = clamp16(e->value); break;
        case SU_INPUT_PAD_AXIS_LT: p->lt = clamp8(e->value); break;
        default: p->rt = clamp8(e->value); break;
        }
    }
    p->packet++;
    p->changed_ns = e->time_ns;
    return SU_OK;
}

void su_input_state_take(SuInputState *live, uint64_t now_ns, SuInputState *out) {
    if (live == NULL || out == NULL) {
        return;
    }
    live->sample_ns = now_ns;
    *out = *live;
    live->mouse_dx = 0;
    live->mouse_dy = 0;
    live->wheel = 0;
    live->flags &= ~SU_INPUT_FLAG_FOCUS_LOST;
}
