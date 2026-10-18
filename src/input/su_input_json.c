/* su_input_map_from_json: the "sutekh.input_map" version 1 format of sutekh/input.h, read with the strict JSON
 * reader (exact integers, no floating point). */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "sutekh/input.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ir/su_json.h"

#define JSON_MAX_BYTES (1024u * 1024u)
#define JSON_MAX_DEPTH 8u
#define CURVE_NAME_MAX 32u

typedef struct Ctx {
    SuInputMap *map;
    char curve_names[SU_INPUT_MAX_CURVES][CURVE_NAME_MAX];
    char *msg;
} Ctx;

static uint32_t fail(Ctx *c, uint32_t code, const char *fmt, ...) {
    if (c->msg != NULL) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(c->msg, 160, fmt, ap);
        va_end(ap);
    }
    return code;
}

/* Signed integer member within [lo, hi]. 1: found and valid, 0: absent, -1: present but invalid. */
static int get_int(const SuJsonValue *obj, const char *key, int64_t lo, int64_t hi, int64_t *out) {
    const SuJsonValue *v = su_json_member(obj, key);
    int64_t x;
    if (v == NULL) {
        return 0;
    }
    if (v->type != SU_JSON_NUMBER || !v->u.number.is_integer || v->u.number.overflow ||
        v->u.number.magnitude > (uint64_t)INT32_MAX) {
        return -1;
    }
    x = (int64_t)v->u.number.magnitude;
    if (v->u.number.negative) {
        x = -x;
    }
    if (x < lo || x > hi) {
        return -1;
    }
    *out = x;
    return 1;
}

/* Thousandths to Q16.16, exact rational rounding half away from zero. */
static int32_t milli_to_q16(int64_t milli) {
    int64_t p = milli * SU_INPUT_ONE;
    return (int32_t)(p >= 0 ? (p + 500) / 1000 : -((-p + 500) / 1000));
}

static const char *get_str(const SuJsonValue *obj, const char *key) {
    const SuJsonValue *v = su_json_member(obj, key);
    return v != NULL && v->type == SU_JSON_STRING ? v->u.string.data : NULL;
}

/* Every member key of obj must be in allowed (NULL-terminated). */
static const char *unknown_member(const SuJsonValue *obj, const char *const *allowed) {
    size_t i;
    for (i = 0; i < obj->u.object.count; i++) {
        const char *k = obj->u.object.members[i].key;
        const char *const *a;
        int ok = 0;
        for (a = allowed; *a != NULL; a++) {
            if (strcmp(*a, k) == 0) {
                ok = 1;
                break;
            }
        }
        if (!ok) {
            return k;
        }
    }
    return NULL;
}

static uint32_t read_names(Ctx *c, const SuJsonValue *arr, char (*names)[SU_INPUT_NAME_MAX], uint32_t cap,
                           uint32_t *count, const char *what) {
    size_t i;
    *count = 0;
    if (arr == NULL) {
        return SU_OK;
    }
    if (arr->type != SU_JSON_ARRAY || arr->u.array.count > cap) {
        return fail(c, SU_E_TYPE, "%s must be an array of at most %u names", what, (unsigned)cap);
    }
    for (i = 0; i < arr->u.array.count; i++) {
        const SuJsonValue *v = arr->u.array.items[i];
        size_t k;
        if (v->type != SU_JSON_STRING || v->u.string.len == 0u || v->u.string.len >= SU_INPUT_NAME_MAX) {
            return fail(c, SU_E_TYPE, "%s[%u] must be a name of 1..%u bytes", what, (unsigned)i,
                        (unsigned)(SU_INPUT_NAME_MAX - 1u));
        }
        for (k = 0; k < i; k++) {
            if (strcmp(names[k], v->u.string.data) == 0) {
                return fail(c, SU_E_INVALID_ARGUMENT, "%s: duplicate name %s", what, v->u.string.data);
            }
        }
        memcpy(names[i], v->u.string.data, v->u.string.len + 1u);
    }
    *count = (uint32_t)arr->u.array.count;
    return SU_OK;
}

static uint32_t read_curves(Ctx *c, const SuJsonValue *arr) {
    static const char *const allowed[] = {"name", "points_milli", NULL};
    size_t i;
    if (arr == NULL) {
        return SU_OK;
    }
    if (arr->type != SU_JSON_ARRAY || arr->u.array.count > SU_INPUT_MAX_CURVES) {
        return fail(c, SU_E_TYPE, "curves must be an array of at most %u curves", (unsigned)SU_INPUT_MAX_CURVES);
    }
    for (i = 0; i < arr->u.array.count; i++) {
        const SuJsonValue *o = arr->u.array.items[i];
        const SuJsonValue *pts;
        const char *name;
        const char *bad;
        SuInputCurve *cv = &c->map->curves[i];
        size_t k;
        if (o->type != SU_JSON_OBJECT) {
            return fail(c, SU_E_TYPE, "curves[%u] must be an object", (unsigned)i);
        }
        if ((bad = unknown_member(o, allowed)) != NULL) {
            return fail(c, SU_E_INVALID_ARGUMENT, "curves[%u]: unknown member %s", (unsigned)i, bad);
        }
        name = get_str(o, "name");
        pts = su_json_member(o, "points_milli");
        if (name == NULL || strlen(name) == 0u || strlen(name) >= CURVE_NAME_MAX || strcmp(name, "linear") == 0) {
            return fail(c, SU_E_TYPE, "curves[%u]: name must be 1..31 bytes and not \"linear\"", (unsigned)i);
        }
        for (k = 0; k < i; k++) {
            if (strcmp(c->curve_names[k], name) == 0) {
                return fail(c, SU_E_INVALID_ARGUMENT, "curves: duplicate name %s", name);
            }
        }
        strcpy(c->curve_names[i], name);
        if (pts == NULL || pts->type != SU_JSON_ARRAY || pts->u.array.count < 2u ||
            pts->u.array.count > SU_INPUT_MAX_CURVE_POINTS) {
            return fail(c, SU_E_TYPE, "curves[%u]: points_milli must hold 2..%u points", (unsigned)i,
                        (unsigned)SU_INPUT_MAX_CURVE_POINTS);
        }
        cv->count = (uint32_t)pts->u.array.count;
        for (k = 0; k < pts->u.array.count; k++) {
            const SuJsonValue *p = pts->u.array.items[k];
            const SuJsonValue *xv;
            const SuJsonValue *yv;
            if (p->type != SU_JSON_ARRAY || p->u.array.count != 2u) {
                return fail(c, SU_E_TYPE, "curves[%u] point %u must be [x, y]", (unsigned)i, (unsigned)k);
            }
            xv = p->u.array.items[0];
            yv = p->u.array.items[1];
            if (xv->type != SU_JSON_NUMBER || yv->type != SU_JSON_NUMBER || !xv->u.number.is_integer ||
                !yv->u.number.is_integer || xv->u.number.negative || yv->u.number.negative ||
                xv->u.number.magnitude > 1000u || yv->u.number.magnitude > 1000u) {
                return fail(c, SU_E_TYPE, "curves[%u] point %u: integers 0..1000", (unsigned)i, (unsigned)k);
            }
            cv->x[k] = milli_to_q16((int64_t)xv->u.number.magnitude);
            cv->y[k] = milli_to_q16((int64_t)yv->u.number.magnitude);
        }
    }
    c->map->curve_count = (uint32_t)arr->u.array.count;
    return SU_OK;
}

static int find_name(char (*names)[SU_INPUT_NAME_MAX], uint32_t n, const char *s) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (strcmp(names[i], s) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int pick(const char *s, const char *const *names) {
    int i;
    for (i = 0; s != NULL && names[i] != NULL; i++) {
        if (strcmp(s, names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static uint32_t read_binding(Ctx *c, const SuJsonValue *o, uint32_t index) {
    static const char *const allowed[] = {"target", "source", "key", "button", "pad", "stick", "trigger", "axis",
                                          "dead_zone_milli", "saturation_milli", "curve", "scale_milli",
                                          "threshold_milli", "counts", "recenter_milli", "axial", NULL};
    static const char *const sources[] = {"key", "mouse_button", "pad_button", "pad_stick", "pad_trigger",
                                          "mouse_delta", "mouse_stick", NULL};
    static const uint32_t source_ids[] = {SU_INPUT_SRC_KEY, SU_INPUT_SRC_MOUSE_BUTTON, SU_INPUT_SRC_PAD_BUTTON,
                                          SU_INPUT_SRC_PAD_STICK, SU_INPUT_SRC_PAD_TRIGGER,
                                          SU_INPUT_SRC_MOUSE_DELTA, SU_INPUT_SRC_MOUSE_STICK};
    static const char *const mouse_buttons[] = {"left", "right", "middle", "x1", "x2", NULL};
    static const char *const pad_buttons[] = {"DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "START", "BACK",
                                              "LTHUMB", "RTHUMB", "LSHOULDER", "RSHOULDER", "", "", "A", "B",
                                              "X", "Y", NULL};
    static const char *const sticks[] = {"lx", "ly", "rx", "ry", NULL};
    static const char *const triggers[] = {"left", "right", NULL};
    static const char *const axes[] = {"x", "y", "wheel", NULL};
    SuInputBinding *b = &c->map->bindings[index];
    const char *bad;
    const char *target;
    const char *s;
    const SuJsonValue *axial;
    int64_t v = 0;
    int r;
    int k;

    if (o->type != SU_JSON_OBJECT) {
        return fail(c, SU_E_TYPE, "bindings[%u] must be an object", (unsigned)index);
    }
    if ((bad = unknown_member(o, allowed)) != NULL) {
        return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: unknown member %s", (unsigned)index, bad);
    }
    k = pick(get_str(o, "source"), sources);
    if (k < 0) {
        return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: unknown or missing source", (unsigned)index);
    }
    target = get_str(o, "target");
    if (target == NULL) {
        return fail(c, SU_E_TYPE, "bindings[%u]: target must be an axis or button name", (unsigned)index);
    }
    if (find_name(c->map->axis_names, c->map->axis_count, target) >= 0) {
        *b = su_input_binding(source_ids[k], 0, 0,
                              SU_INPUT_AXIS(find_name(c->map->axis_names, c->map->axis_count, target)));
    } else if (find_name(c->map->button_names, c->map->button_count, target) >= 0) {
        *b = su_input_binding(source_ids[k], 0, 0,
                              SU_INPUT_BUTTON(find_name(c->map->button_names, c->map->button_count, target)));
    } else {
        return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: target %s is not a declared axis or button",
                    (unsigned)index, target);
    }
    switch (b->source) {
    case SU_INPUT_SRC_KEY:
        s = get_str(o, "key");
        if (s == NULL || su_input_key_from_name(s, &b->code) != SU_OK) {
            return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: unknown key", (unsigned)index);
        }
        break;
    case SU_INPUT_SRC_MOUSE_BUTTON:
        k = pick(get_str(o, "button"), mouse_buttons);
        if (k < 0) {
            return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: unknown mouse button", (unsigned)index);
        }
        b->code = 1u << k;
        break;
    case SU_INPUT_SRC_PAD_BUTTON:
        s = get_str(o, "button");
        k = (s != NULL && s[0] != '\0') ? pick(s, pad_buttons) : -1;
        if (k < 0) {
            return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: unknown pad button", (unsigned)index);
        }
        b->code = 1u << k;
        break;
    case SU_INPUT_SRC_PAD_STICK:
        k = pick(get_str(o, "stick"), sticks);
        if (k < 0) {
            return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: stick must be lx, ly, rx or ry", (unsigned)index);
        }
        b->code = (uint32_t)k;
        break;
    case SU_INPUT_SRC_PAD_TRIGGER:
        k = pick(get_str(o, "trigger"), triggers);
        if (k < 0) {
            return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: trigger must be left or right", (unsigned)index);
        }
        b->code = (uint32_t)k;
        break;
    default:
        k = pick(get_str(o, "axis"), axes);
        if (k < 0) {
            return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: axis must be x, y or wheel", (unsigned)index);
        }
        b->code = (uint32_t)k;
        break;
    }
    if (b->source == SU_INPUT_SRC_PAD_BUTTON || b->source == SU_INPUT_SRC_PAD_STICK ||
        b->source == SU_INPUT_SRC_PAD_TRIGGER) {
        r = get_int(o, "pad", 0, SU_INPUT_MAX_PADS - 1u, &v);
        if (r < 0) {
            return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: pad must be 0..3", (unsigned)index);
        }
        b->device = r > 0 ? (uint32_t)v : 0u;
    } else if (su_json_member(o, "pad") != NULL) {
        return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: pad applies to pad sources only", (unsigned)index);
    }
    if ((r = get_int(o, "scale_milli", -32000, 32000, &v)) != 0) {
        if (r < 0 || v == 0) {
            return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: scale_milli must be nonzero, -32000..32000",
                        (unsigned)index);
        }
        b->scale = milli_to_q16(v);
    }
    if ((r = get_int(o, "dead_zone_milli", 0, 999, &v)) != 0) {
        if (r < 0) {
            return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: dead_zone_milli must be 0..999", (unsigned)index);
        }
        b->dead_zone = milli_to_q16(v);
    }
    if ((r = get_int(o, "saturation_milli", 1, 1000, &v)) != 0) {
        if (r < 0) {
            return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: saturation_milli must be 1..1000", (unsigned)index);
        }
        b->saturation = milli_to_q16(v);
    }
    if ((r = get_int(o, "threshold_milli", 1, 1000, &v)) != 0) {
        if (r < 0) {
            return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: threshold_milli must be 1..1000", (unsigned)index);
        }
        b->threshold = milli_to_q16(v);
    }
    if ((r = get_int(o, "counts", 1, 1000000, &v)) != 0) {
        if (r < 0) {
            return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: counts must be 1..1000000", (unsigned)index);
        }
        b->counts = (int32_t)v;
    }
    if ((r = get_int(o, "recenter_milli", 0, 1000, &v)) != 0) {
        if (r < 0) {
            return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: recenter_milli must be 0..1000", (unsigned)index);
        }
        b->recenter = milli_to_q16(v);
    }
    s = get_str(o, "curve");
    if (s != NULL && strcmp(s, "linear") != 0) {
        uint32_t i;
        b->curve = 0;
        for (i = 0; i < c->map->curve_count; i++) {
            if (strcmp(c->curve_names[i], s) == 0) {
                b->curve = i + 1u;
            }
        }
        if (b->curve == 0u) {
            return fail(c, SU_E_INVALID_ARGUMENT, "bindings[%u]: unknown curve %s", (unsigned)index, s);
        }
    } else if (s == NULL && su_json_member(o, "curve") != NULL) {
        return fail(c, SU_E_TYPE, "bindings[%u]: curve must be a name", (unsigned)index);
    }
    axial = su_json_member(o, "axial");
    if (axial != NULL) {
        if (axial->type != SU_JSON_BOOL) {
            return fail(c, SU_E_TYPE, "bindings[%u]: axial must be true or false", (unsigned)index);
        }
        if (axial->u.boolean) {
            b->flags |= SU_INPUT_BIND_AXIAL;
        }
    }
    return SU_OK;
}

uint32_t su_input_map_from_json(const uint8_t *json, size_t len, SuInputMap *map, char message[160]) {
    static const char *const allowed[] = {"format", "version", "axes", "buttons", "curves", "bindings", NULL};
    SuJsonDoc doc;
    SuJsonLimits lim;
    SuJsonError jerr;
    SuInputMap tmp;
    Ctx c;
    const SuJsonValue *root;
    const SuJsonValue *binds;
    const char *bad;
    const char *fmt;
    int64_t version = 0;
    uint32_t st;
    uint32_t badi = 0;
    size_t i;

    memset(&c, 0, sizeof c);
    memset(&tmp, 0, sizeof tmp);
    memset(&doc, 0, sizeof doc);
    c.map = &tmp;
    c.msg = message;
    if (message != NULL) {
        message[0] = '\0';
    }
    if (json == NULL || map == NULL) {
        return fail(&c, SU_E_INVALID_ARGUMENT, "invalid arguments");
    }
    if (len > JSON_MAX_BYTES) {
        return fail(&c, SU_E_RESOURCE_LIMIT, "input map exceeds 1 MiB");
    }
    lim.max_bytes = len;
    lim.max_depth = JSON_MAX_DEPTH;
    st = su_json_parse(json, len, &lim, &doc, &jerr);
    if (st != SU_OK) {
        su_json_doc_free(&doc);
        return fail(&c, st == SU_E_DUPLICATE_KEY ? SU_E_PARSE : st, "JSON: %s at %s", jerr.message, jerr.pointer);
    }
    root = doc.root;
    if (root == NULL || root->type != SU_JSON_OBJECT) {
        st = fail(&c, SU_E_TYPE, "the map must be a JSON object");
        goto out;
    }
    if ((bad = unknown_member(root, allowed)) != NULL) {
        st = fail(&c, SU_E_INVALID_ARGUMENT, "unknown member %s", bad);
        goto out;
    }
    fmt = get_str(root, "format");
    if (fmt == NULL || strcmp(fmt, "sutekh.input_map") != 0 || get_int(root, "version", 1, 1, &version) != 1) {
        st = fail(&c, SU_E_INVALID_ARGUMENT, "format must be \"sutekh.input_map\" and version 1");
        goto out;
    }
    st = read_names(&c, su_json_member(root, "axes"), tmp.axis_names, SU_INPUT_MAX_AXES, &tmp.axis_count, "axes");
    if (st == SU_OK) {
        st = read_names(&c, su_json_member(root, "buttons"), tmp.button_names, SU_INPUT_MAX_BUTTONS,
                        &tmp.button_count, "buttons");
    }
    for (i = 0; st == SU_OK && i < tmp.axis_count; i++) {
        if (find_name(tmp.button_names, tmp.button_count, tmp.axis_names[i]) >= 0) {
            st = fail(&c, SU_E_INVALID_ARGUMENT, "%s is both an axis and a button", tmp.axis_names[i]);
        }
    }
    if (st == SU_OK) {
        st = read_curves(&c, su_json_member(root, "curves"));
    }
    if (st != SU_OK) {
        goto out;
    }
    binds = su_json_member(root, "bindings");
    if (binds == NULL || binds->type != SU_JSON_ARRAY || binds->u.array.count > SU_INPUT_MAX_BINDINGS) {
        st = fail(&c, SU_E_TYPE, "bindings must be an array of at most %u bindings", (unsigned)SU_INPUT_MAX_BINDINGS);
        goto out;
    }
    for (i = 0; i < binds->u.array.count; i++) {
        st = read_binding(&c, binds->u.array.items[i], (uint32_t)i);
        if (st != SU_OK) {
            goto out;
        }
    }
    tmp.binding_count = (uint32_t)binds->u.array.count;
    st = su_input_map_validate(&tmp, &badi);
    if (st != SU_OK) {
        st = fail(&c, st,
                  badi < SU_INPUT_MAX_BINDINGS ? "bindings[%u]: inconsistent values (dead zone, saturation or scale)"
                                               : "curve %u is not monotonic from (0, 0) to x = 1000",
                  badi < SU_INPUT_MAX_BINDINGS ? (unsigned)badi : (unsigned)(badi - SU_INPUT_MAX_BINDINGS));
        goto out;
    }
    *map = tmp;
out:
    su_json_doc_free(&doc);
    return st;
}
