#include "su_lir.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "su_json.h"
#include "su_sha256.h"

#define SU_NUMERIC_PROFILE "u32_mod_v1"
#define SU_IR_VERSION "0.1"
#define SU_CAPSULE_FORMAT "0.1"

typedef struct Str {
    const char *p;
    size_t n;
} Str;

enum {
    K_STRING,   /* JSON string (may be empty) */
    K_NAME,     /* JSON string, non-empty */
    K_U32,      /* integer 0..2^32-1 */
    K_U32_POS,  /* integer 1..2^32-1 */
    K_UINT,     /* non-negative integer fitting uint64 */
    K_BOOL,
    K_ARRAY,
    K_OBJECT
};

typedef struct KeySpec {
    const char *name;
    int kind;
} KeySpec;

typedef struct RegOperand {
    const char *key;
    uint32_t want; /* SU_TYPE_U32 or SU_TYPE_BOOL */
} RegOperand;

typedef struct OpSpec {
    const char *name;
    uint32_t code;
    uint32_t dst_type;   /* 0 when the op defines no register */
    size_t nkeys;
    KeySpec keys[5];
    size_t nregs;
    RegOperand regs[3];  /* register operands, in SuInsn operand order */
    int field_mode;      /* 0 none, 1 read (load), 2 write (store) */
    const char *field_key;
} OpSpec;

static const OpSpec OPS[] = {
    {"const_u32", SU_OP_CONST_U32, SU_TYPE_U32, 3,
     {{"op", K_NAME}, {"dst", K_NAME}, {"value", K_U32}}, 0, {{0, 0}}, 0, NULL},
    {"load_u32", SU_OP_LOAD_U32, SU_TYPE_U32, 3,
     {{"op", K_NAME}, {"dst", K_NAME}, {"field", K_NAME}}, 0, {{0, 0}}, 1, "field"},
    {"add_u32", SU_OP_ADD_U32, SU_TYPE_U32, 4,
     {{"op", K_NAME}, {"dst", K_NAME}, {"a", K_NAME}, {"b", K_NAME}}, 2,
     {{"a", SU_TYPE_U32}, {"b", SU_TYPE_U32}, {0, 0}}, 0, NULL},
    {"sub_u32", SU_OP_SUB_U32, SU_TYPE_U32, 4,
     {{"op", K_NAME}, {"dst", K_NAME}, {"a", K_NAME}, {"b", K_NAME}}, 2,
     {{"a", SU_TYPE_U32}, {"b", SU_TYPE_U32}, {0, 0}}, 0, NULL},
    {"mul_u32", SU_OP_MUL_U32, SU_TYPE_U32, 4,
     {{"op", K_NAME}, {"dst", K_NAME}, {"a", K_NAME}, {"b", K_NAME}}, 2,
     {{"a", SU_TYPE_U32}, {"b", SU_TYPE_U32}, {0, 0}}, 0, NULL},
    {"eq_u32", SU_OP_EQ_U32, SU_TYPE_BOOL, 4,
     {{"op", K_NAME}, {"dst", K_NAME}, {"a", K_NAME}, {"b", K_NAME}}, 2,
     {{"a", SU_TYPE_U32}, {"b", SU_TYPE_U32}, {0, 0}}, 0, NULL},
    {"lt_u32", SU_OP_LT_U32, SU_TYPE_BOOL, 4,
     {{"op", K_NAME}, {"dst", K_NAME}, {"a", K_NAME}, {"b", K_NAME}}, 2,
     {{"a", SU_TYPE_U32}, {"b", SU_TYPE_U32}, {0, 0}}, 0, NULL},
    {"select_u32", SU_OP_SELECT_U32, SU_TYPE_U32, 5,
     {{"op", K_NAME}, {"dst", K_NAME}, {"cond", K_NAME}, {"on_true", K_NAME}, {"on_false", K_NAME}}, 3,
     {{"cond", SU_TYPE_BOOL}, {"on_true", SU_TYPE_U32}, {"on_false", SU_TYPE_U32}}, 0, NULL},
    {"store_u32", SU_OP_STORE_U32, 0, 3,
     {{"op", K_NAME}, {"field", K_NAME}, {"src", K_NAME}}, 1,
     {{"src", SU_TYPE_U32}, {0, 0}, {0, 0}}, 2, "field"},
};

typedef struct CapInfo {
    Str system;
    Str schema_id;
    Str profile;
    const SuJsonValue *fields;
    uint64_t max_instructions;
} CapInfo;

typedef struct IrInfo {
    Str system;
    Str query;
    Str profile;
    const SuJsonValue *insns;
} IrInfo;

typedef struct CapField {
    uint32_t id;
    uint32_t access;
    Str name;
} CapField;

typedef struct RegDef {
    Str name;
    uint32_t type;
} RegDef;

/* ---------- small helpers ---------- */

static Str jstr(const SuJsonValue *v) {
    Str s;
    s.p = v->u.string.data;
    s.n = v->u.string.len;
    return s;
}

static int str_is(Str s, const char *lit) {
    size_t n = strlen(lit);
    return s.n == n && memcmp(s.p, lit, n) == 0;
}

static int str_eq(Str a, Str b) {
    return a.n == b.n && memcmp(a.p, b.p, a.n) == 0;
}

static int clampi(size_t n) {
    return n > 64u ? 64 : (int)n;
}

static void pbuf_put(char *buf, size_t cap, size_t *n, const char *s, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        if (*n + 1 < cap) {
            buf[*n] = s[i];
            (*n)++;
        }
    }
    buf[*n] = '\0';
}

/* out = base + "/" + escaped(key). */
static void ptr_child(char *out, size_t cap, const char *base, const char *key, size_t klen) {
    size_t n = 0, k;
    pbuf_put(out, cap, &n, base, strlen(base));
    pbuf_put(out, cap, &n, "/", 1);
    for (k = 0; k < klen; k++) {
        if (key[k] == '~') {
            pbuf_put(out, cap, &n, "~0", 2);
        } else if (key[k] == '/') {
            pbuf_put(out, cap, &n, "~1", 2);
        } else {
            pbuf_put(out, cap, &n, key + k, 1);
        }
    }
}

static uint32_t set_diag(SuDiag *d, uint32_t code, const char *ptr, const char *fmt, ...) {
    va_list ap;
    size_t n = 0;
    d->code = code;
    pbuf_put(d->json_pointer, sizeof d->json_pointer, &n, ptr, strlen(ptr));
    va_start(ap, fmt);
    vsnprintf(d->message, sizeof d->message, fmt, ap);
    va_end(ap);
    return code;
}

static uint32_t from_json(SuDiag *d, const SuJsonError *je, const char *doc) {
    size_t n = 0;
    d->code = je->code;
    d->byte_offset = je->offset;
    n = 0;
    pbuf_put(d->json_pointer, sizeof d->json_pointer, &n, je->pointer, strlen(je->pointer));
    n = 0;
    pbuf_put(d->message, sizeof d->message, &n, doc, strlen(doc));
    pbuf_put(d->message, sizeof d->message, &n, je->message, strlen(je->message));
    return je->code;
}

static void diag_system(SuDiag *d, Str s) {
    size_t n = 0;
    pbuf_put(d->system_id, sizeof d->system_id, &n, s.p, s.n);
}

void su_diag_init(SuDiag *d) {
    memset(d, 0, sizeof *d);
    d->byte_offset = SIZE_MAX;
}

static int kind_ok(const SuJsonValue *v, int kind) {
    uint64_t u = 0;
    switch (kind) {
    case K_STRING: return v->type == SU_JSON_STRING;
    case K_NAME: return v->type == SU_JSON_STRING && v->u.string.len > 0;
    case K_U32: return su_json_get_uint(v, &u) && u <= 0xFFFFFFFFu;
    case K_U32_POS: return su_json_get_uint(v, &u) && u >= 1u && u <= 0xFFFFFFFFu;
    case K_UINT: return su_json_get_uint(v, &u);
    case K_BOOL: return v->type == SU_JSON_BOOL;
    case K_ARRAY: return v->type == SU_JSON_ARRAY;
    case K_OBJECT: return v->type == SU_JSON_OBJECT;
    default: return 0;
    }
}

static uint32_t u32_of(const SuJsonValue *v) {
    uint64_t u = 0;
    su_json_get_uint(v, &u);
    return (uint32_t)u;
}

/* Checks an object's members against a spec: unknown keys, wrong JSON types
 * and missing required keys (all keys are required). */
static uint32_t check_keys(SuDiag *d, const SuJsonValue *obj, const char *base,
                           const KeySpec *spec, size_t nspec) {
    size_t i, j;
    char ptr[320];

    if (obj->type != SU_JSON_OBJECT) {
        return set_diag(d, SU_E_TYPE, base, "expected a JSON object");
    }
    for (i = 0; i < obj->u.object.count; i++) {
        const SuJsonMember *m = &obj->u.object.members[i];
        const KeySpec *ks = NULL;
        for (j = 0; j < nspec; j++) {
            size_t nl = strlen(spec[j].name);
            if (nl == m->key_len && memcmp(spec[j].name, m->key, nl) == 0) {
                ks = &spec[j];
                break;
            }
        }
        if (ks == NULL) {
            ptr_child(ptr, sizeof ptr, base, m->key, m->key_len);
            return set_diag(d, SU_E_TYPE, ptr, "unknown key '%.*s'", clampi(m->key_len), m->key);
        }
        if (!kind_ok(m->value, ks->kind)) {
            ptr_child(ptr, sizeof ptr, base, ks->name, strlen(ks->name));
            return set_diag(d, SU_E_TYPE, ptr, "key '%s' has the wrong JSON type or value", ks->name);
        }
    }
    for (j = 0; j < nspec; j++) {
        if (su_json_member(obj, spec[j].name) == NULL) {
            ptr_child(ptr, sizeof ptr, base, spec[j].name, strlen(spec[j].name));
            return set_diag(d, SU_E_TYPE, ptr, "missing required key '%s'", spec[j].name);
        }
    }
    return SU_OK;
}

static int ident_ok(Str s) {
    size_t i;
    if (s.n == 0) {
        return 0;
    }
    for (i = 0; i < s.n; i++) {
        char c = s.p[i];
        int alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        int digit = c >= '0' && c <= '9';
        if (i == 0 ? !alpha : !(alpha || digit)) {
            return 0;
        }
    }
    return 1;
}

/* ---------- stage 1: limits from parsed trees ---------- */

static uint32_t stage1_limits(SuDiag *d, const SuJsonValue *cap_root, const SuJsonValue *ir_root) {
    const SuJsonValue *f;
    if (cap_root != NULL && cap_root->type == SU_JSON_OBJECT) {
        f = su_json_member(cap_root, "fields");
        if (f != NULL && f->type == SU_JSON_ARRAY && f->u.array.count > SU_LIMIT_FIELDS) {
            return set_diag(d, SU_E_RESOURCE_LIMIT, "/fields", "capsule declares more than %u fields",
                            (unsigned)SU_LIMIT_FIELDS);
        }
    }
    if (ir_root != NULL && ir_root->type == SU_JSON_OBJECT) {
        f = su_json_member(ir_root, "instructions");
        if (f != NULL && f->type == SU_JSON_ARRAY && f->u.array.count > SU_LIMIT_INSTRUCTIONS) {
            return set_diag(d, SU_E_RESOURCE_LIMIT, "/instructions", "IR has more than %u instructions",
                            (unsigned)SU_LIMIT_INSTRUCTIONS);
        }
    }
    return SU_OK;
}

/* ---------- stage 4: top-level and nested shape ---------- */

static uint32_t check_capsule(SuDiag *d, const SuJsonValue *root, CapInfo *ci) {
    static const KeySpec top[] = {
        {"capsule_format", K_STRING}, {"system_id", K_NAME}, {"source", K_NAME},
        {"schema", K_OBJECT}, {"numeric_profile", K_STRING}, {"fields", K_ARRAY},
        {"schedule", K_OBJECT}, {"limits", K_OBJECT}, {"tests", K_ARRAY}};
    static const KeySpec schema_keys[] = {
        {"id", K_NAME}, {"version", K_U32_POS}, {"layout_generation", K_U32_POS}};
    static const KeySpec field_keys[] = {
        {"id", K_U32_POS}, {"name", K_NAME}, {"type", K_NAME}, {"read", K_BOOL}, {"write", K_BOOL}};
    static const KeySpec sched_keys[] = {
        {"phase", K_NAME}, {"after", K_ARRAY}, {"before", K_ARRAY}, {"parallel", K_STRING}};
    static const KeySpec limit_keys[] = {
        {"max_instructions", K_UINT}, {"max_rows", K_UINT}, {"allocation", K_STRING}};
    const SuJsonValue *v, *arr;
    uint32_t rc;
    size_t i;
    uint64_t u;

    rc = check_keys(d, root, "", top, sizeof top / sizeof top[0]);
    if (rc != SU_OK) {
        return rc;
    }
    v = su_json_member(root, "capsule_format");
    if (!str_is(jstr(v), SU_CAPSULE_FORMAT)) {
        return set_diag(d, SU_E_TYPE, "/capsule_format", "unsupported capsule_format");
    }
    v = su_json_member(root, "numeric_profile");
    if (!str_is(jstr(v), SU_NUMERIC_PROFILE)) {
        return set_diag(d, SU_E_TYPE, "/numeric_profile", "unsupported numeric_profile");
    }
    ci->profile = jstr(v);
    ci->system = jstr(su_json_member(root, "system_id"));

    v = su_json_member(root, "schema");
    rc = check_keys(d, v, "/schema", schema_keys, sizeof schema_keys / sizeof schema_keys[0]);
    if (rc != SU_OK) {
        return rc;
    }
    ci->schema_id = jstr(su_json_member(v, "id"));

    arr = su_json_member(root, "fields");
    if (arr->u.array.count == 0) {
        return set_diag(d, SU_E_TYPE, "/fields", "capsule needs at least one field");
    }
    for (i = 0; i < arr->u.array.count; i++) {
        char base[64];
        const SuJsonValue *fv = arr->u.array.items[i];
        snprintf(base, sizeof base, "/fields/%zu", i);
        rc = check_keys(d, fv, base, field_keys, sizeof field_keys / sizeof field_keys[0]);
        if (rc != SU_OK) {
            return rc;
        }
        if (!ident_ok(jstr(su_json_member(fv, "name")))) {
            return set_diag(d, SU_E_TYPE, base, "field name must match [A-Za-z_][A-Za-z0-9_]*");
        }
        if (!str_is(jstr(su_json_member(fv, "type")), "u32")) {
            return set_diag(d, SU_E_TYPE, base, "M0 fields must have type \"u32\"");
        }
    }
    ci->fields = arr;

    v = su_json_member(root, "schedule");
    rc = check_keys(d, v, "/schedule", sched_keys, sizeof sched_keys / sizeof sched_keys[0]);
    if (rc != SU_OK) {
        return rc;
    }
    if (!str_is(jstr(su_json_member(v, "parallel")), "serial")) {
        return set_diag(d, SU_E_TYPE, "/schedule/parallel", "M0 requires parallel \"serial\"");
    }
    {
        const char *names[2] = {"after", "before"};
        size_t k;
        for (k = 0; k < 2; k++) {
            const SuJsonValue *a = su_json_member(v, names[k]);
            size_t j;
            for (j = 0; j < a->u.array.count; j++) {
                if (a->u.array.items[j]->type != SU_JSON_STRING || a->u.array.items[j]->u.string.len == 0) {
                    char ptr[64];
                    snprintf(ptr, sizeof ptr, "/schedule/%s/%zu", names[k], j);
                    return set_diag(d, SU_E_TYPE, ptr, "schedule entries must be non-empty strings");
                }
            }
        }
    }

    v = su_json_member(root, "limits");
    rc = check_keys(d, v, "/limits", limit_keys, sizeof limit_keys / sizeof limit_keys[0]);
    if (rc != SU_OK) {
        return rc;
    }
    su_json_get_uint(su_json_member(v, "max_instructions"), &u);
    if (u < 1u || u > SU_LIMIT_INSTRUCTIONS) {
        return set_diag(d, SU_E_TYPE, "/limits/max_instructions", "max_instructions must be 1..%u",
                        (unsigned)SU_LIMIT_INSTRUCTIONS);
    }
    ci->max_instructions = u;
    su_json_get_uint(su_json_member(v, "max_rows"), &u);
    if (u > SU_LIMIT_ROWS) {
        return set_diag(d, SU_E_TYPE, "/limits/max_rows", "max_rows exceeds the M0 default limit");
    }
    if (!str_is(jstr(su_json_member(v, "allocation")), "forbidden")) {
        return set_diag(d, SU_E_TYPE, "/limits/allocation", "M0 requires allocation \"forbidden\"");
    }

    arr = su_json_member(root, "tests");
    if (arr->u.array.count == 0) {
        return set_diag(d, SU_E_TYPE, "/tests", "tests must list at least one file");
    }
    for (i = 0; i < arr->u.array.count; i++) {
        if (arr->u.array.items[i]->type != SU_JSON_STRING || arr->u.array.items[i]->u.string.len == 0) {
            return set_diag(d, SU_E_TYPE, "/tests", "tests entries must be non-empty strings");
        }
    }
    return SU_OK;
}

static uint32_t check_ir(SuDiag *d, const SuJsonValue *root, IrInfo *ii) {
    static const KeySpec top[] = {
        {"ir_version", K_STRING}, {"system_id", K_NAME}, {"numeric_profile", K_STRING},
        {"query", K_NAME}, {"instructions", K_ARRAY}};
    uint32_t rc = check_keys(d, root, "", top, sizeof top / sizeof top[0]);
    if (rc != SU_OK) {
        return rc;
    }
    if (!str_is(jstr(su_json_member(root, "ir_version")), SU_IR_VERSION)) {
        return set_diag(d, SU_E_TYPE, "/ir_version", "unsupported ir_version");
    }
    if (!str_is(jstr(su_json_member(root, "numeric_profile")), SU_NUMERIC_PROFILE)) {
        return set_diag(d, SU_E_TYPE, "/numeric_profile", "unsupported numeric_profile");
    }
    ii->system = jstr(su_json_member(root, "system_id"));
    ii->query = jstr(su_json_member(root, "query"));
    ii->profile = jstr(su_json_member(root, "numeric_profile"));
    ii->insns = su_json_member(root, "instructions");
    return SU_OK;
}

/* ---------- stage 5: capsule and IR cross-checks ---------- */

static uint32_t field_id_of(const SuJsonValue *fv) {
    return u32_of(su_json_member(fv, "id"));
}

static Str field_name_of(const SuJsonValue *fv) {
    return jstr(su_json_member(fv, "name"));
}

static uint32_t check_schema_semantics(SuDiag *d, const CapInfo *ci, const IrInfo *ii) {
    size_t i, j, nf = ci->fields->u.array.count;
    for (i = 0; i < nf; i++) {
        const SuJsonValue *a = ci->fields->u.array.items[i];
        char ptr[64];
        for (j = 0; j < i; j++) {
            const SuJsonValue *b = ci->fields->u.array.items[j];
            if (field_id_of(a) == field_id_of(b)) {
                snprintf(ptr, sizeof ptr, "/fields/%zu/id", i);
                return set_diag(d, SU_E_SCHEMA_MISMATCH, ptr, "duplicate field id %u",
                                (unsigned)field_id_of(a));
            }
            if (str_eq(field_name_of(a), field_name_of(b))) {
                snprintf(ptr, sizeof ptr, "/fields/%zu/name", i);
                return set_diag(d, SU_E_SCHEMA_MISMATCH, ptr, "duplicate field name '%.*s'",
                                clampi(field_name_of(a).n), field_name_of(a).p);
            }
        }
    }
    if (!str_eq(ii->system, ci->system)) {
        return set_diag(d, SU_E_SCHEMA_MISMATCH, "/system_id", "IR system_id differs from capsule system_id");
    }
    if (!str_eq(ii->query, ci->schema_id)) {
        return set_diag(d, SU_E_SCHEMA_MISMATCH, "/query", "IR query differs from capsule schema.id");
    }
    if (!str_eq(ii->profile, ci->profile)) {
        return set_diag(d, SU_E_SCHEMA_MISMATCH, "/numeric_profile",
                        "IR numeric_profile differs from capsule numeric_profile");
    }
    return SU_OK;
}

static int cmp_field(const void *a, const void *b) {
    const CapField *x = (const CapField *)a;
    const CapField *y = (const CapField *)b;
    return (x->id > y->id) - (x->id < y->id);
}

static int find_field(const CapField *tab, uint32_t nf, Str name, uint32_t *idx) {
    uint32_t i;
    for (i = 0; i < nf; i++) {
        if (str_eq(tab[i].name, name)) {
            *idx = i;
            return 1;
        }
    }
    return 0;
}

static int find_reg(const RegDef *regs, uint32_t nr, Str name, uint32_t *idx) {
    uint32_t i;
    for (i = 0; i < nr; i++) {
        if (str_eq(regs[i].name, name)) {
            *idx = i;
            return 1;
        }
    }
    return 0;
}

static const OpSpec *find_op(Str name) {
    size_t i;
    for (i = 0; i < sizeof OPS / sizeof OPS[0]; i++) {
        size_t n = strlen(OPS[i].name);
        if (n == name.n && memcmp(OPS[i].name, name.p, n) == 0) {
            return &OPS[i];
        }
    }
    return NULL;
}

/* ---------- stage 6: instructions ---------- */

typedef struct Body {
    CapField *table;
    uint32_t nf;
    uint32_t *store_count;
    RegDef *regs;
    uint32_t nregs;
    SuInsn *out;
} Body;

static uint32_t check_instructions(SuDiag *d, const IrInfo *ii, Body *b) {
    size_t i, ninsn = ii->insns->u.array.count;

    for (i = 0; i < ninsn; i++) {
        const SuJsonValue *ins = ii->insns->u.array.items[i];
        char base[64], ptr[160];
        const SuJsonValue *opv;
        const OpSpec *spec;
        uint32_t idx[3] = {SU_OPERAND_NONE, SU_OPERAND_NONE, SU_OPERAND_NONE};
        uint32_t rtype[3] = {0, 0, 0};
        uint32_t dst = SU_OPERAND_NONE, fi = SU_OPERAND_NONE, k, rc;
        SuInsn in;

        snprintf(base, sizeof base, "/instructions/%zu", i);
        if (ins->type != SU_JSON_OBJECT) {
            return set_diag(d, SU_E_TYPE, base, "instruction must be a JSON object");
        }
        opv = su_json_member(ins, "op");
        if (opv == NULL) {
            return set_diag(d, SU_E_TYPE, base, "instruction is missing key 'op'");
        }
        if (opv->type != SU_JSON_STRING) {
            ptr_child(ptr, sizeof ptr, base, "op", 2);
            return set_diag(d, SU_E_TYPE, ptr, "op must be a string");
        }
        spec = find_op(jstr(opv));
        ptr_child(ptr, sizeof ptr, base, "op", 2);
        if (spec == NULL) {
            return set_diag(d, SU_E_UNKNOWN_OPCODE, ptr, "unknown opcode '%.*s'",
                            clampi(jstr(opv).n), jstr(opv).p);
        }

        rc = check_keys(d, ins, base, spec->keys, spec->nkeys);
        if (rc != SU_OK) {
            return rc;
        }

        /* Undefined registers. */
        for (k = 0; k < spec->nregs; k++) {
            const SuJsonValue *rv = su_json_member(ins, spec->regs[k].key);
            if (!find_reg(b->regs, b->nregs, jstr(rv), &idx[k])) {
                ptr_child(ptr, sizeof ptr, base, spec->regs[k].key, strlen(spec->regs[k].key));
                return set_diag(d, SU_E_UNDEFINED_REGISTER, ptr, "register '%.*s' is not defined before use",
                                clampi(jstr(rv).n), jstr(rv).p);
            }
            rtype[k] = b->regs[idx[k]].type;
        }

        /* Duplicate destination. */
        if (spec->dst_type != 0) {
            Str dn = jstr(su_json_member(ins, "dst"));
            uint32_t dummy;
            if (find_reg(b->regs, b->nregs, dn, &dummy)) {
                ptr_child(ptr, sizeof ptr, base, "dst", 3);
                return set_diag(d, SU_E_DUPLICATE_REGISTER, ptr, "register '%.*s' is defined twice",
                                clampi(dn.n), dn.p);
            }
        }

        /* Operand register types. */
        for (k = 0; k < spec->nregs; k++) {
            if (rtype[k] != spec->regs[k].want) {
                ptr_child(ptr, sizeof ptr, base, spec->regs[k].key, strlen(spec->regs[k].key));
                return set_diag(d, SU_E_TYPE, ptr, "operand '%s' has type %s, expected %s",
                                spec->regs[k].key, rtype[k] == SU_TYPE_BOOL ? "bool" : "u32",
                                spec->regs[k].want == SU_TYPE_BOOL ? "bool" : "u32");
            }
        }

        /* Field permissions. */
        if (spec->field_mode != 0) {
            Str fname = jstr(su_json_member(ins, spec->field_key));
            ptr_child(ptr, sizeof ptr, base, spec->field_key, strlen(spec->field_key));
            if (!find_field(b->table, b->nf, fname, &fi)) {
                if (spec->field_mode == 1) {
                    return set_diag(d, SU_E_UNDECLARED_READ, ptr, "field '%.*s' is not declared in the capsule",
                                    clampi(fname.n), fname.p);
                }
                return set_diag(d, SU_E_UNDECLARED_WRITE, ptr, "field '%.*s' is not declared in the capsule",
                                clampi(fname.n), fname.p);
            }
            if (spec->field_mode == 1 && !(b->table[fi].access & SU_ACCESS_READ)) {
                return set_diag(d, SU_E_UNDECLARED_READ, ptr, "field '%.*s' is not readable",
                                clampi(fname.n), fname.p);
            }
            if (spec->field_mode == 2) {
                if (!(b->table[fi].access & SU_ACCESS_WRITE)) {
                    return set_diag(d, SU_E_UNDECLARED_WRITE, ptr, "field '%.*s' is not writable",
                                    clampi(fname.n), fname.p);
                }
                if (b->store_count[fi] != 0) {
                    return set_diag(d, SU_E_UNDECLARED_WRITE, ptr,
                                    "field '%.*s' is stored more than once per row",
                                    clampi(fname.n), fname.p);
                }
            }
        }

        /* Commit: constants, dst definition, instruction words. */
        memset(&in, 0, sizeof in);
        in.op = spec->code;
        in.dst = SU_OPERAND_NONE;
        in.a = SU_OPERAND_NONE;
        in.b = SU_OPERAND_NONE;
        in.c = SU_OPERAND_NONE;
        if (spec->dst_type != 0) {
            if (b->nregs >= SU_LIMIT_REGISTERS) {
                return set_diag(d, SU_E_RESOURCE_LIMIT, base, "more than %u registers",
                                (unsigned)SU_LIMIT_REGISTERS);
            }
            dst = b->nregs;
            b->regs[dst].name = jstr(su_json_member(ins, "dst"));
            b->regs[dst].type = spec->dst_type;
            b->nregs++;
            in.dst = dst;
        }
        switch (spec->code) {
        case SU_OP_CONST_U32:
            in.a = u32_of(su_json_member(ins, "value"));
            break;
        case SU_OP_LOAD_U32:
            in.a = fi;
            break;
        case SU_OP_STORE_U32:
            in.a = fi;
            in.b = idx[0];
            b->store_count[fi]++;
            break;
        case SU_OP_SELECT_U32:
            in.a = idx[0];
            in.b = idx[1];
            in.c = idx[2];
            break;
        default:
            in.a = idx[0];
            in.b = idx[1];
            break;
        }
        b->out[i] = in;
    }
    return SU_OK;
}

/* ---------- stage 7 and module construction ---------- */

static void hash_u32(SuSha256 *c, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)((v >> 8) & 0xFFu);
    b[2] = (uint8_t)((v >> 16) & 0xFFu);
    b[3] = (uint8_t)((v >> 24) & 0xFFu);
    su_sha256_update(c, b, 4);
}

static void hash_str(SuSha256 *c, Str s) {
    hash_u32(c, (uint32_t)s.n);
    su_sha256_update(c, s.p, s.n);
}

static void compute_semantic(SuLirModule *m, Str profile, Str system, Str query) {
    SuSha256 c;
    uint32_t i;
    static const uint8_t prefix[10] = {'S', 'U', 'L', 'I', 'R', '-', 'S', 'E', 'M', 0};
    Str sp, ss, sq;

    sp.p = profile.p; sp.n = profile.n;
    ss.p = system.p; ss.n = system.n;
    sq.p = query.p; sq.n = query.n;

    su_sha256_init(&c);
    su_sha256_update(&c, prefix, sizeof prefix);
    hash_u32(&c, SU_ARTIFACT_FORMAT_VERSION);
    hash_str(&c, sp);
    hash_str(&c, ss);
    hash_str(&c, sq);
    hash_u32(&c, m->field_count);
    for (i = 0; i < m->field_count; i++) {
        hash_u32(&c, m->fields[i].id);
        hash_u32(&c, m->fields[i].type);
        hash_u32(&c, m->fields[i].access);
    }
    hash_u32(&c, m->insn_count);
    for (i = 0; i < m->insn_count; i++) {
        const SuInsn *in = &m->insns[i];
        uint32_t a = in->a;
        if (in->op == SU_OP_LOAD_U32 || in->op == SU_OP_STORE_U32) {
            a = m->fields[in->a].id;
        }
        hash_u32(&c, in->op);
        hash_u32(&c, in->dst);
        hash_u32(&c, a);
        hash_u32(&c, in->b);
        hash_u32(&c, in->c);
    }
    su_sha256_final(&c, m->semantic_hash);
    su_sha256_hex(m->semantic_hash, m->semantic_hash_hex);
}

static char *dup_str(Str s) {
    char *r = malloc(s.n + 1u);
    if (r != NULL) {
        memcpy(r, s.p, s.n);
        r[s.n] = '\0';
    }
    return r;
}

static void sha_of(const uint8_t *p, size_t n, uint8_t out[32], char hex[65]) {
    SuSha256 c;
    su_sha256_init(&c);
    su_sha256_update(&c, p, n);
    su_sha256_final(&c, out);
    su_sha256_hex(out, hex);
}

void su_lir_module_free(SuLirModule *m) {
    uint32_t i;
    if (m == NULL) {
        return;
    }
    free(m->system_id);
    free(m->query);
    free(m->numeric_profile);
    if (m->fields != NULL) {
        for (i = 0; i < m->field_count; i++) {
            free(m->fields[i].name);
        }
    }
    free(m->fields);
    free(m->insns);
    memset(m, 0, sizeof *m);
}

uint32_t su_lir_verify(const uint8_t *capsule, size_t capsule_len,
                       const uint8_t *lir, size_t lir_len,
                       SuLirModule *out, SuDiag *d) {
    SuJsonLimits lim;
    SuJsonDoc cdoc, ldoc;
    SuJsonError cerr, lerr;
    uint32_t cr, lr, rc;
    CapInfo ci;
    IrInfo ii;
    Body b;
    uint32_t nf, i;
    size_t ninsn;
    SuLirModule m;

    su_diag_init(d);
    memset(out, 0, sizeof *out);
    memset(&cdoc, 0, sizeof cdoc);
    memset(&ldoc, 0, sizeof ldoc);
    memset(&ci, 0, sizeof ci);
    memset(&ii, 0, sizeof ii);
    memset(&b, 0, sizeof b);
    lim.max_bytes = SU_LIMIT_IR_SOURCE_BYTES;
    lim.max_depth = SU_LIMIT_JSON_DEPTH;

    /* Stages 1-3 are evaluated for both documents before any shape check. */
    cr = su_json_parse(capsule, capsule_len, &lim, &cdoc, &cerr);
    lr = su_json_parse(lir, lir_len, &lim, &ldoc, &lerr);

    /* Stage 1: limits (capsule first). */
    if (cr == SU_E_RESOURCE_LIMIT) {
        rc = from_json(d, &cerr, "capsule: ");
        goto done;
    }
    if (lr == SU_E_RESOURCE_LIMIT) {
        rc = from_json(d, &lerr, "lir: ");
        goto done;
    }
    rc = stage1_limits(d, cdoc.root, ldoc.root);
    if (rc != SU_OK) {
        goto done;
    }
    /* Stage 2: syntax / UTF-8 (capsule first). */
    if (cr == SU_E_PARSE) {
        rc = from_json(d, &cerr, "capsule: ");
        goto done;
    }
    if (lr == SU_E_PARSE) {
        rc = from_json(d, &lerr, "lir: ");
        goto done;
    }
    /* Stage 3: duplicate keys (capsule first). */
    if (cr == SU_E_DUPLICATE_KEY) {
        rc = from_json(d, &cerr, "capsule: ");
        goto done;
    }
    if (lr == SU_E_DUPLICATE_KEY) {
        rc = from_json(d, &lerr, "lir: ");
        goto done;
    }

    /* Stage 4: top-level and nested shape (capsule first). */
    rc = check_capsule(d, cdoc.root, &ci);
    if (rc != SU_OK) {
        goto done;
    }
    rc = check_ir(d, ldoc.root, &ii);
    if (rc != SU_OK) {
        goto done;
    }
    diag_system(d, ii.system);

    /* Stage 5: schema semantics and IR/capsule agreement. */
    rc = check_schema_semantics(d, &ci, &ii);
    if (rc != SU_OK) {
        goto done;
    }
    ninsn = ii.insns->u.array.count;
    if ((uint64_t)ninsn > ci.max_instructions) {
        rc = set_diag(d, SU_E_RESOURCE_LIMIT, "/instructions",
                      "IR has %zu instructions; capsule limits.max_instructions is %llu",
                      ninsn, (unsigned long long)ci.max_instructions);
        goto done;
    }

    /* Verified field table: capsule fields sorted ascending by id. */
    nf = ci.fields->u.array.count;
    b.table = calloc(nf, sizeof *b.table);
    b.store_count = calloc(nf, sizeof *b.store_count);
    b.regs = calloc(ninsn + 1u, sizeof *b.regs);
    b.out = calloc(ninsn + 1u, sizeof *b.out);
    if (b.table == NULL || b.store_count == NULL || b.regs == NULL || b.out == NULL) {
        rc = set_diag(d, SU_E_INTERNAL, "", "out of memory");
        goto done;
    }
    b.nf = nf;
    for (i = 0; i < nf; i++) {
        const SuJsonValue *fv = ci.fields->u.array.items[i];
        const SuJsonValue *rv = su_json_member(fv, "read");
        const SuJsonValue *wv = su_json_member(fv, "write");
        b.table[i].id = field_id_of(fv);
        b.table[i].name = field_name_of(fv);
        b.table[i].access = (rv->u.boolean ? SU_ACCESS_READ : 0u) | (wv->u.boolean ? SU_ACCESS_WRITE : 0u);
    }
    qsort(b.table, nf, sizeof *b.table, cmp_field);

    /* Stage 6: instructions in ascending order. */
    rc = check_instructions(d, &ii, &b);
    if (rc != SU_OK) {
        goto done;
    }

    /* Stage 7: every writable field stored exactly once. */
    for (i = 0; i < nf; i++) {
        if ((b.table[i].access & SU_ACCESS_WRITE) && b.store_count[i] == 0) {
            rc = set_diag(d, SU_E_MISSING_STORE, "/instructions",
                          "writable field '%.*s' is never stored",
                          clampi(b.table[i].name.n), b.table[i].name.p);
            goto done;
        }
    }

    /* Build the module. */
    memset(&m, 0, sizeof m);
    m.artifact_format = SU_ARTIFACT_FORMAT_VERSION;
    m.system_id = dup_str(ii.system);
    m.query = dup_str(ii.query);
    m.numeric_profile = dup_str(ii.profile);
    m.field_count = nf;
    m.fields = calloc(nf ? nf : 1u, sizeof *m.fields);
    m.register_count = b.nregs;
    m.insn_count = (uint32_t)ninsn;
    m.insns = calloc(ninsn + 1u, sizeof *m.insns);
    if (m.system_id == NULL || m.query == NULL || m.numeric_profile == NULL ||
        m.fields == NULL || m.insns == NULL) {
        su_lir_module_free(&m);
        rc = set_diag(d, SU_E_INTERNAL, "", "out of memory");
        goto done;
    }
    for (i = 0; i < nf; i++) {
        m.fields[i].id = b.table[i].id;
        m.fields[i].type = SU_TYPE_U32;
        m.fields[i].access = b.table[i].access;
        m.fields[i].name = dup_str(b.table[i].name);
        if (m.fields[i].name == NULL) {
            su_lir_module_free(&m);
            rc = set_diag(d, SU_E_INTERNAL, "", "out of memory");
            goto done;
        }
    }
    m.field_count = nf;
    if (ninsn > 0) {
        memcpy(m.insns, b.out, ninsn * sizeof *m.insns);
    }
    sha_of(lir, lir_len, m.source_hash, m.source_hash_hex);
    sha_of(capsule, capsule_len, m.capsule_hash, m.capsule_hash_hex);
    compute_semantic(&m, ii.profile, ii.system, ii.query);

    *out = m;
    rc = SU_OK;

done:
    free(b.table);
    free(b.store_count);
    free(b.regs);
    free(b.out);
    su_json_doc_free(&cdoc);
    su_json_doc_free(&ldoc);
    return rc;
}
