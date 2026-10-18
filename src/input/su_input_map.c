/* Portable part of sutekh/input.h: key names, state splitting, binding validation, the integer mapper and the
 * record encoding. No allocation, no floating point. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "sutekh/input.h"

#include <string.h>

#define ONE ((int64_t)SU_INPUT_ONE)

/* ---------- key names ---------- */

typedef struct KeyName {
    const char *name;
    uint32_t code;
} KeyName;

static const KeyName k_keys[] = {
    {"ESCAPE", SU_KEY_ESCAPE}, {"1", SU_KEY_1}, {"2", SU_KEY_2}, {"3", SU_KEY_3}, {"4", SU_KEY_4},
    {"5", SU_KEY_5}, {"6", SU_KEY_6}, {"7", SU_KEY_7}, {"8", SU_KEY_8}, {"9", SU_KEY_9}, {"0", SU_KEY_0},
    {"MINUS", SU_KEY_MINUS}, {"EQUALS", SU_KEY_EQUALS}, {"BACKSPACE", SU_KEY_BACKSPACE}, {"TAB", SU_KEY_TAB},
    {"Q", SU_KEY_Q}, {"W", SU_KEY_W}, {"E", SU_KEY_E}, {"R", SU_KEY_R}, {"T", SU_KEY_T}, {"Y", SU_KEY_Y},
    {"U", SU_KEY_U}, {"I", SU_KEY_I}, {"O", SU_KEY_O}, {"P", SU_KEY_P}, {"LBRACKET", SU_KEY_LBRACKET},
    {"RBRACKET", SU_KEY_RBRACKET}, {"ENTER", SU_KEY_ENTER}, {"LCTRL", SU_KEY_LCTRL}, {"A", SU_KEY_A},
    {"S", SU_KEY_S}, {"D", SU_KEY_D}, {"F", SU_KEY_F}, {"G", SU_KEY_G}, {"H", SU_KEY_H}, {"J", SU_KEY_J},
    {"K", SU_KEY_K}, {"L", SU_KEY_L}, {"SEMICOLON", SU_KEY_SEMICOLON}, {"APOSTROPHE", SU_KEY_APOSTROPHE},
    {"GRAVE", SU_KEY_GRAVE}, {"LSHIFT", SU_KEY_LSHIFT}, {"BACKSLASH", SU_KEY_BACKSLASH}, {"Z", SU_KEY_Z},
    {"X", SU_KEY_X}, {"C", SU_KEY_C}, {"V", SU_KEY_V}, {"B", SU_KEY_B}, {"N", SU_KEY_N}, {"M", SU_KEY_M},
    {"COMMA", SU_KEY_COMMA}, {"PERIOD", SU_KEY_PERIOD}, {"SLASH", SU_KEY_SLASH}, {"RSHIFT", SU_KEY_RSHIFT},
    {"KP_MULTIPLY", SU_KEY_KP_MULTIPLY}, {"LALT", SU_KEY_LALT}, {"SPACE", SU_KEY_SPACE},
    {"CAPSLOCK", SU_KEY_CAPSLOCK}, {"F1", SU_KEY_F1}, {"F2", SU_KEY_F2}, {"F3", SU_KEY_F3}, {"F4", SU_KEY_F4},
    {"F5", SU_KEY_F5}, {"F6", SU_KEY_F6}, {"F7", SU_KEY_F7}, {"F8", SU_KEY_F8}, {"F9", SU_KEY_F9},
    {"F10", SU_KEY_F10}, {"NUMLOCK", SU_KEY_NUMLOCK}, {"SCROLLLOCK", SU_KEY_SCROLLLOCK}, {"KP_7", SU_KEY_KP_7},
    {"KP_8", SU_KEY_KP_8}, {"KP_9", SU_KEY_KP_9}, {"KP_MINUS", SU_KEY_KP_MINUS}, {"KP_4", SU_KEY_KP_4},
    {"KP_5", SU_KEY_KP_5}, {"KP_6", SU_KEY_KP_6}, {"KP_PLUS", SU_KEY_KP_PLUS}, {"KP_1", SU_KEY_KP_1},
    {"KP_2", SU_KEY_KP_2}, {"KP_3", SU_KEY_KP_3}, {"KP_0", SU_KEY_KP_0}, {"KP_PERIOD", SU_KEY_KP_PERIOD},
    {"F11", SU_KEY_F11}, {"F12", SU_KEY_F12}, {"KP_ENTER", SU_KEY_KP_ENTER}, {"RCTRL", SU_KEY_RCTRL},
    {"KP_DIVIDE", SU_KEY_KP_DIVIDE}, {"RALT", SU_KEY_RALT}, {"HOME", SU_KEY_HOME}, {"UP", SU_KEY_UP},
    {"PAGEUP", SU_KEY_PAGEUP}, {"LEFT", SU_KEY_LEFT}, {"RIGHT", SU_KEY_RIGHT}, {"END", SU_KEY_END},
    {"DOWN", SU_KEY_DOWN}, {"PAGEDOWN", SU_KEY_PAGEDOWN}, {"INSERT", SU_KEY_INSERT}, {"DELETE", SU_KEY_DELETE},
    {"LMETA", SU_KEY_LMETA}, {"RMETA", SU_KEY_RMETA}, {"MENU", SU_KEY_MENU},
};

uint32_t su_input_key_from_name(const char *name, uint32_t *key) {
    size_t i;
    if (name == NULL || key == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++) {
        if (strcmp(k_keys[i].name, name) == 0) {
            *key = k_keys[i].code;
            return SU_OK;
        }
    }
    return SU_E_INVALID_ARGUMENT;
}

const char *su_input_key_name(uint32_t key) {
    size_t i;
    for (i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++) {
        if (k_keys[i].code == key) {
            return k_keys[i].name;
        }
    }
    return NULL;
}

/* ---------- state splitting ---------- */

/* floor(a * b / n) for |a| < 2^31, 0 <= b <= n, n >= 1. */
static int64_t floor_part(int64_t a, int64_t b, int64_t n) {
    int64_t p = a * b;
    int64_t q = p / n;
    if ((p % n != 0) && (p < 0)) {
        q--;
    }
    return q;
}

static int32_t split_one(int32_t v, uint32_t n, uint32_t i) {
    return (int32_t)(floor_part(v, (int64_t)i + 1, n) - floor_part(v, i, n));
}

uint32_t su_input_state_split(const SuInputState *frame, uint32_t n, uint32_t index, SuInputState *out) {
    if (frame == NULL || out == NULL || n == 0u || index >= n) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (out != frame) {
        *out = *frame;
    }
    out->mouse_dx = split_one(frame->mouse_dx, n, index);
    out->mouse_dy = split_one(frame->mouse_dy, n, index);
    out->wheel = split_one(frame->wheel, n, index);
    return SU_OK;
}

/* ---------- bindings and validation ---------- */

SuInputBinding su_input_binding(uint32_t source, uint32_t code, uint32_t device, uint32_t target) {
    SuInputBinding b;
    memset(&b, 0, sizeof b);
    b.source = source;
    b.code = code;
    b.device = device;
    b.target = target;
    b.scale = SU_INPUT_ONE;
    b.dead_zone = 0;
    b.saturation = SU_INPUT_ONE;
    b.curve = 0;
    b.threshold = SU_INPUT_ONE / 2;
    b.counts = 1;
    b.recenter = 0;
    return b;
}

static int one_bit(uint32_t v) {
    return v != 0u && (v & (v - 1u)) == 0u;
}

static int binding_ok(const SuInputMap *m, const SuInputBinding *b) {
    uint32_t ti = SU_INPUT_TARGET_INDEX(b->target);
    if ((b->target & ~0x1FFu) != 0u) {
        return 0;
    }
    if (SU_INPUT_TARGET_IS_BUTTON(b->target) ? ti >= m->button_count : ti >= m->axis_count) {
        return 0;
    }
    if (b->scale == 0 || b->scale < -32 * SU_INPUT_ONE || b->scale > 32 * SU_INPUT_ONE) {
        return 0;
    }
    if (b->dead_zone < 0 || b->saturation <= b->dead_zone || b->saturation > SU_INPUT_ONE) {
        return 0;
    }
    if (b->curve > m->curve_count || b->threshold <= 0 || b->threshold > SU_INPUT_ONE) {
        return 0;
    }
    if ((b->flags & ~SU_INPUT_BIND_AXIAL) != 0u) {
        return 0;
    }
    switch (b->source) {
    case SU_INPUT_SRC_KEY:
        return b->code < SU_INPUT_KEY_COUNT && b->device == 0u;
    case SU_INPUT_SRC_MOUSE_BUTTON:
        return one_bit(b->code) && b->code <= SU_INPUT_MOUSE_X2 && b->device == 0u;
    case SU_INPUT_SRC_PAD_BUTTON:
        return one_bit(b->code) && b->code <= 0x8000u && b->device < SU_INPUT_MAX_PADS;
    case SU_INPUT_SRC_PAD_STICK:
        return b->code <= SU_INPUT_STICK_RY && b->device < SU_INPUT_MAX_PADS;
    case SU_INPUT_SRC_PAD_TRIGGER:
        return b->code <= 1u && b->device < SU_INPUT_MAX_PADS;
    case SU_INPUT_SRC_MOUSE_DELTA:
    case SU_INPUT_SRC_MOUSE_STICK:
        return b->code <= SU_INPUT_MOUSE_AXIS_WHEEL && b->device == 0u && b->counts >= 1 && b->recenter >= 0 &&
               b->recenter <= SU_INPUT_ONE;
    default:
        return 0;
    }
}

static int curve_ok(const SuInputCurve *c) {
    uint32_t i;
    if (c->count < 2u || c->count > SU_INPUT_MAX_CURVE_POINTS || c->x[0] != 0 || c->y[0] != 0 ||
        c->x[c->count - 1u] != SU_INPUT_ONE) {
        return 0;
    }
    for (i = 1; i < c->count; i++) {
        if (c->x[i] <= c->x[i - 1u] || c->y[i] < c->y[i - 1u] || c->y[i] > SU_INPUT_ONE) {
            return 0;
        }
    }
    return 1;
}

uint32_t su_input_map_validate(const SuInputMap *map, uint32_t *bad) {
    uint32_t i;
    if (bad != NULL) {
        *bad = UINT32_MAX;
    }
    if (map == NULL || map->axis_count > SU_INPUT_MAX_AXES || map->button_count > SU_INPUT_MAX_BUTTONS ||
        map->binding_count > SU_INPUT_MAX_BINDINGS || map->curve_count > SU_INPUT_MAX_CURVES) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < map->curve_count; i++) {
        if (!curve_ok(&map->curves[i])) {
            if (bad != NULL) {
                *bad = SU_INPUT_MAX_BINDINGS + i;
            }
            return SU_E_INVALID_ARGUMENT;
        }
    }
    for (i = 0; i < map->binding_count; i++) {
        if (!binding_ok(map, &map->bindings[i])) {
            if (bad != NULL) {
                *bad = i;
            }
            return SU_E_INVALID_ARGUMENT;
        }
    }
    return SU_OK;
}

/* ---------- conditioning ---------- */

static int64_t clamp64(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/* Integer square root (floor) of a 64-bit value. */
static uint64_t isqrt64(uint64_t v) {
    uint64_t r = 0;
    uint64_t bit = (uint64_t)1 << 62;
    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0u) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static int64_t curve_eval(const SuInputMap *m, uint32_t curve, int64_t t) {
    const SuInputCurve *c;
    uint32_t i;
    if (curve == 0u) {
        return t;
    }
    c = &m->curves[curve - 1u];
    for (i = 1; i < c->count; i++) {
        if (t <= c->x[i]) {
            int64_t x0 = c->x[i - 1u];
            int64_t y0 = c->y[i - 1u];
            return y0 + (t - x0) * ((int64_t)c->y[i] - y0) / ((int64_t)c->x[i] - x0);
        }
    }
    return c->y[c->count - 1u];
}

/* Magnitude in [0, ONE] -> conditioned magnitude in [0, ONE]. */
static int64_t condition_mag(const SuInputMap *m, const SuInputBinding *b, int64_t mag) {
    int64_t dz = b->dead_zone;
    int64_t sat = b->saturation;
    int64_t t;
    if (mag <= dz) {
        return 0;
    }
    if (mag > sat) {
        mag = sat;
    }
    t = (mag - dz) * ONE / (sat - dz);
    return curve_eval(m, b->curve, t);
}

static int64_t condition_axial(const SuInputMap *m, const SuInputBinding *b, int64_t v) {
    int64_t c = condition_mag(m, b, v < 0 ? -v : v);
    return v < 0 ? -c : c;
}

/* Stick component in Q16.16: -32768..32767 -> about [-ONE, ONE], clamped. */
static int64_t stick_norm(int16_t raw) {
    return clamp64((int64_t)raw * ONE / 32767, -ONE, ONE);
}

static int64_t stick_value(const SuInputMap *m, const SuInputBinding *b, const SuInputPad *p) {
    int64_t x;
    int64_t y;
    int64_t own;
    int64_t mag;
    int64_t c;
    int left = b->code <= SU_INPUT_STICK_LY;
    x = stick_norm(left ? p->lx : p->rx);
    y = stick_norm(left ? p->ly : p->ry);
    own = (b->code == SU_INPUT_STICK_LX || b->code == SU_INPUT_STICK_RX) ? x : y;
    if ((b->flags & SU_INPUT_BIND_AXIAL) != 0u) {
        return condition_axial(m, b, own);
    }
    mag = (int64_t)isqrt64((uint64_t)(x * x + y * y));
    if (mag == 0) {
        return 0;
    }
    c = condition_mag(m, b, mag > ONE ? ONE : mag);
    return clamp64(own * c / mag, -ONE, ONE);
}

static int32_t mouse_axis(const SuInputState *s, uint32_t code) {
    return code == SU_INPUT_MOUSE_AXIS_X ? s->mouse_dx : code == SU_INPUT_MOUSE_AXIS_Y ? s->mouse_dy : s->wheel;
}

/* ---------- mapper ---------- */

uint32_t su_input_mapper_init(SuInputMapper *m, const SuInputMap *map) {
    uint32_t st;
    if (m == NULL || map == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = su_input_map_validate(map, NULL);
    if (st != SU_OK) {
        return st;
    }
    memset(m, 0, sizeof *m);
    m->map = *map;
    return SU_OK;
}

void su_input_mapper_reset(SuInputMapper *m) {
    if (m != NULL) {
        memset(m->stick, 0, sizeof m->stick);
    }
}

uint32_t su_input_mapper_tick(SuInputMapper *m, const SuInputState *s, uint64_t tick, SuInputRecord *out) {
    int64_t sum[SU_INPUT_MAX_AXES];
    uint32_t i;
    if (m == NULL || s == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(sum, 0, sizeof sum);
    memset(out, 0, sizeof *out);
    out->tick = tick;
    out->axis_count = m->map.axis_count;
    for (i = 0; i < m->map.binding_count; i++) {
        const SuInputBinding *b = &m->map.bindings[i];
        const SuInputPad *pad = &s->pads[b->device < SU_INPUT_MAX_PADS ? b->device : 0u];
        int digital = -1; /* -1: analog source */
        int64_t v = 0;
        switch (b->source) {
        case SU_INPUT_SRC_KEY:
            digital = su_input_key_down(s, b->code);
            break;
        case SU_INPUT_SRC_MOUSE_BUTTON:
            digital = (s->mouse_buttons & b->code) != 0u;
            break;
        case SU_INPUT_SRC_PAD_BUTTON:
            digital = pad->connected != 0u && (pad->buttons & b->code) != 0u;
            break;
        case SU_INPUT_SRC_PAD_STICK:
            v = pad->connected != 0u ? stick_value(&m->map, b, pad) : 0;
            break;
        case SU_INPUT_SRC_PAD_TRIGGER:
            v = pad->connected != 0u ? condition_axial(&m->map, b, (int64_t)(b->code == 0u ? pad->lt : pad->rt) * ONE / 255)
                                     : 0;
            break;
        case SU_INPUT_SRC_MOUSE_DELTA:
            /* Motion this tick as a fraction of a full deflection; dead zone and curve on the clamped value. */
            v = condition_axial(&m->map, b, clamp64((int64_t)mouse_axis(s, b->code) * ONE / b->counts, -ONE, ONE));
            break;
        case SU_INPUT_SRC_MOUSE_STICK: {
            int64_t p = m->stick[i] + (int64_t)mouse_axis(s, b->code) * ONE / b->counts;
            p = clamp64(p, -ONE, ONE);
            if (p > 0) {
                p = p > b->recenter ? p - b->recenter : 0;
            } else if (p < 0) {
                p = -p > b->recenter ? p + b->recenter : 0;
            }
            m->stick[i] = (int32_t)p;
            v = condition_axial(&m->map, b, p);
            break;
        }
        default:
            return SU_E_INTERNAL;
        }
        if (SU_INPUT_TARGET_IS_BUTTON(b->target)) {
            int on = digital >= 0 ? digital : (v * b->scale / ONE >= b->threshold);
            if (on) {
                out->buttons |= 1u << SU_INPUT_TARGET_INDEX(b->target);
            }
        } else {
            if (digital >= 0) {
                v = digital ? ONE : 0;
            }
            sum[SU_INPUT_TARGET_INDEX(b->target)] += v * b->scale / ONE;
        }
    }
    for (i = 0; i < m->map.axis_count; i++) {
        out->axes[i] = (int32_t)clamp64(sum[i], -ONE, ONE);
    }
    m->ticks++;
    return SU_OK;
}

/* ---------- records ---------- */

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void su_input_record_encode(const SuInputRecord *r, uint8_t out[SU_INPUT_RECORD_BYTES]) {
    uint32_t i;
    memcpy(out, "SUINREC", 8u);
    put32(out + 8, 1u);
    put32(out + 12, (uint32_t)r->tick);
    put32(out + 16, (uint32_t)(r->tick >> 32));
    put32(out + 20, r->buttons);
    put32(out + 24, r->axis_count);
    for (i = 0; i < SU_INPUT_MAX_AXES; i++) {
        put32(out + 28 + 4u * i, i < r->axis_count ? (uint32_t)r->axes[i] : 0u);
    }
}

uint32_t su_input_record_decode(const uint8_t *buf, size_t len, SuInputRecord *r) {
    uint32_t i;
    SuInputRecord t;
    if (buf == NULL || r == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (len != SU_INPUT_RECORD_BYTES || memcmp(buf, "SUINREC", 8u) != 0 || get32(buf + 8) != 1u) {
        return SU_E_PARSE;
    }
    memset(&t, 0, sizeof t);
    t.tick = (uint64_t)get32(buf + 12) | ((uint64_t)get32(buf + 16) << 32);
    t.buttons = get32(buf + 20);
    t.axis_count = get32(buf + 24);
    if (t.axis_count > SU_INPUT_MAX_AXES) {
        return SU_E_PARSE;
    }
    for (i = 0; i < SU_INPUT_MAX_AXES; i++) {
        int32_t a = (int32_t)get32(buf + 28 + 4u * i);
        if (i >= t.axis_count ? a != 0 : (a < -SU_INPUT_ONE || a > SU_INPUT_ONE)) {
            return SU_E_PARSE;
        }
        t.axes[i] = a;
    }
    *r = t;
    return SU_OK;
}

uint32_t su_input_record_writes(const SuInputRecord *r, const SuInputFields *fields, uint64_t entity,
                                SuHostInput *out, uint32_t *count) {
    uint32_t n = 0;
    uint32_t i;
    uint32_t j;
    if (r == NULL || fields == NULL || out == NULL || count == NULL || r->axis_count > SU_INPUT_MAX_AXES) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (fields->buttons != SU_INPUT_NO_FIELD) {
        out[n].entity = entity;
        out[n].field_id = fields->buttons;
        out[n].value = r->buttons;
        n++;
    }
    for (i = 0; i < r->axis_count; i++) {
        if (fields->axes[i] == SU_INPUT_NO_FIELD) {
            continue;
        }
        out[n].entity = entity;
        out[n].field_id = fields->axes[i];
        out[n].value = (uint32_t)r->axes[i];
        n++;
    }
    for (i = 0; i < n; i++) {
        for (j = i + 1u; j < n; j++) {
            if (out[i].field_id == out[j].field_id) {
                return SU_E_INVALID_ARGUMENT;
            }
        }
    }
    *count = n;
    return SU_OK;
}
