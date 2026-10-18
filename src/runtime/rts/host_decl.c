/* Host-operator declarations and capsule loading (M3-04a). See host_decl.h. */
#define _POSIX_C_SOURCE 200809L

#include "runtime/rts/host_decl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_json.h"
#include "ir/su_ops03.h"
#include "ir/su_sha256.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"

#define HOST_CAPSULE_MAX_BYTES (16u * 1024u * 1024u)
#define HOST_MSG_SET(msg, cap, ...)                                  \
    do {                                                             \
        if ((msg) != NULL && (cap) > 0u) {                           \
            snprintf((msg), (cap), __VA_ARGS__);                     \
        }                                                            \
    } while (0)


/* Numeric profile of a host operator: fixed_q32_v1 when any field is a typed (non-u32) lane, else u32_mod_v1. */
static const char *host_profile(const SuHostOpDesc *d) {
    uint32_t i;
    for (i = 0; i < d->field_count; i++) {
        if (d->fields[i].type != SU_TYPE_U32) {
            return SU_NUMERIC_PROFILE_FIXED_Q32_V1;
        }
    }
    return SU_NUMERIC_PROFILE_U32_MOD_V1;
}

/* Lane of field i of a validated descriptor: 1 for the second entry of an i64/q32 pair, else 0. */
static uint32_t host_lane(const SuHostOpDesc *d, uint32_t i) {
    uint32_t k = 0;
    while (k < i) {
        k += su_type_lanes(d->fields[k].type) == 2u ? 2u : 1u;
    }
    return k == i ? 0u : 1u;
}

uint32_t su_host_desc_validate(const SuHostOpDesc *d) {
    uint32_t i;
    if (d == NULL || d->system_id == NULL || d->schema_id == NULL || d->run == NULL || d->fields == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (strlen(d->system_id) == 0u || strlen(d->system_id) >= SU_DIAG_SYSTEM_BYTES) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (strlen(d->schema_id) == 0u || strlen(d->schema_id) > SU_WORLD_NAME_MAX) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (d->operator_version == 0u || d->field_count == 0u || d->field_count > SU_LIMIT_FIELDS) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < d->field_count; i++) {
        const SuHostField *f = &d->fields[i];
        if (f->name == NULL || strlen(f->name) == 0u || strlen(f->name) > SU_WORLD_NAME_MAX) {
            return SU_E_INVALID_ARGUMENT;
        }
        if (su_type_lanes(f->type) == 0u || f->access == 0u || (f->access & ~(SU_ACCESS_READ | SU_ACCESS_WRITE)) != 0u) {
            return SU_E_INVALID_ARGUMENT;
        }
        if (i > 0u && f->id <= d->fields[i - 1u].id) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    /* Typed lanes (IR 0.3 value types in SuHostField.type): an i64/q32 field is two adjacent entries with IDs
     * I and I+1, the same type and the same access (lane 0 then lane 1). */
    for (i = 0; i < d->field_count; i++) {
        const SuHostField *f = &d->fields[i];
        if (su_type_lanes(f->type) != 2u) {
            continue;
        }
        if (i + 1u >= d->field_count || d->fields[i + 1u].id != f->id + 1u || d->fields[i + 1u].type != f->type ||
            d->fields[i + 1u].access != f->access) {
            return SU_E_INVALID_ARGUMENT;
        }
        i++;
    }
    return SU_OK;
}

static char *dup_cstr(const char *s) {
    size_t n = strlen(s);
    char *p = malloc(n + 1u);
    if (p != NULL) {
        memcpy(p, s, n + 1u);
    }
    return p;
}

/* Appends a NUL-terminated string to the digest. */
static void put(SuSha256 *c, const char *s) {
    su_sha256_update(c, s, strlen(s));
}

/* Appends s as a JSON string literal (names are identifiers and hex, so no escaping is needed). */
static void json_put_str(SuSha256 *c, const char *s) {
    put(c, "\"");
    put(c, s);
    put(c, "\"");
}

/* Semantic identity of a host operator (A3, contract 3.1 "Host identity", r4): SHA-256 of the canonical JSON
 * (sorted keys, no whitespace) of {"schema_id","system_id","operator_version","constants_hash","fields":
 * [{"access","id","name","type"} ascending id]}. */
static void host_semantic_digest(const SuHostOpDesc *d, uint8_t out[32]) {
    SuSha256 c;
    char hex[65];
    char num[32];
    uint32_t i;
    su_sha256_init(&c);
    su_sha256_hex(d->constants_hash, hex);
    put(&c, "{\"constants_hash\":");
    json_put_str(&c, hex);
    put(&c, ",\"fields\":[");
    for (i = 0; i < d->field_count; i++) {
        const char *acc = d->fields[i].access == SU_ACCESS_READ ? "read"
                          : d->fields[i].access == SU_ACCESS_WRITE ? "write" : "read_write";
        if (i > 0u) {
            put(&c, ",");
        }
        put(&c, "{\"access\":");
        json_put_str(&c, acc);
        snprintf(num, sizeof num, ",\"id\":%u,\"name\":", (unsigned)d->fields[i].id);
        put(&c, num);
        json_put_str(&c, d->fields[i].name);
        put(&c, ",\"type\":");
        json_put_str(&c, su_type_name(d->fields[i].type)); /* "u32" for every u32 lane: RTS identities unchanged */
        put(&c, "}");
    }
    put(&c, "],\"operator_version\":");
    snprintf(num, sizeof num, "%u", (unsigned)d->operator_version);
    put(&c, num);
    put(&c, ",\"schema_id\":");
    json_put_str(&c, d->schema_id);
    put(&c, ",\"system_id\":");
    json_put_str(&c, d->system_id);
    put(&c, "}");
    su_sha256_final(&c, out);
}

uint32_t su_host_decl_build(const SuHostOpDesc *d, SuLirModule *out) {
    uint32_t i;
    if (out == NULL || su_host_desc_validate(d) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    out->artifact_format = SU_ARTIFACT_FORMAT_VERSION;
    out->system_id = dup_cstr(d->system_id);
    out->query = dup_cstr(d->schema_id);
    out->numeric_profile = dup_cstr(host_profile(d));
    out->field_count = d->field_count;
    out->fields = calloc(d->field_count, sizeof *out->fields);
    if (out->system_id == NULL || out->query == NULL || out->numeric_profile == NULL || out->fields == NULL) {
        su_lir_module_free(out);
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0; i < d->field_count; i++) {
        out->fields[i].id = d->fields[i].id;
        out->fields[i].type = SU_TYPE_U32; /* storage lane */
        out->fields[i].value_type = d->fields[i].type;
        out->fields[i].lane = host_lane(d, i);
        out->fields[i].access = d->fields[i].access;
        out->fields[i].name = dup_cstr(d->fields[i].name);
        if (out->fields[i].name == NULL) {
            su_lir_module_free(out);
            return SU_E_RESOURCE_LIMIT;
        }
    }
    host_semantic_digest(d, out->semantic_hash);
    su_sha256_hex(out->semantic_hash, out->semantic_hash_hex);
    /* A host entry has no source: source_hash is the SHA-256 of the empty byte string (r4). */
    {
        SuSha256 e;
        su_sha256_init(&e);
        su_sha256_final(&e, out->source_hash);
        su_sha256_hex(out->source_hash, out->source_hash_hex);
    }
    return SU_OK;
}

void su_host_decl_free(SuLirModule *m) {
    if (m == NULL) {
        return;
    }
    su_lir_module_free(m);
    free(m);
}

/* ---------- capsule loading ---------- */

static uint32_t access_of_name(const char *s) {
    if (strcmp(s, "read") == 0) {
        return SU_ACCESS_READ;
    }
    if (strcmp(s, "write") == 0) {
        return SU_ACCESS_WRITE;
    }
    if (strcmp(s, "read_write") == 0) {
        return SU_ACCESS_READ | SU_ACCESS_WRITE;
    }
    return 0u;
}

static const char *jstr(const SuJsonValue *v) {
    return (v != NULL && v->type == SU_JSON_STRING) ? v->u.string.data : NULL;
}

static const SuJsonValue *jmember(const SuJsonValue *o, const char *key) {
    if (o == NULL || o->type != SU_JSON_OBJECT) {
        return NULL;
    }
    return su_json_member(o, key);
}

static int juint(const SuJsonValue *v, uint64_t *out) {
    return v != NULL && su_json_get_uint(v, out) != 0;
}

/* Reads the whole file (bounded). Returns SU_OK, SU_E_IO or SU_E_RESOURCE_LIMIT. */
static uint32_t read_capsule_bytes(const char *path, uint8_t **buf, size_t *len) {
    FILE *fh = fopen(path, "rb");
    long sz = -1;
    size_t got;
    *buf = NULL;
    *len = 0;
    if (fh == NULL) {
        return SU_E_IO;
    }
    if (fseek(fh, 0, SEEK_END) == 0) {
        sz = ftell(fh);
    }
    if (sz < 0 || (unsigned long)sz > HOST_CAPSULE_MAX_BYTES) {
        fclose(fh);
        return sz < 0 ? SU_E_IO : SU_E_RESOURCE_LIMIT;
    }
    rewind(fh);
    *buf = malloc((size_t)sz + 1u);
    if (*buf == NULL) {
        fclose(fh);
        return SU_E_RESOURCE_LIMIT;
    }
    got = fread(*buf, 1, (size_t)sz, fh);
    fclose(fh);
    if (got != (size_t)sz) {
        free(*buf);
        *buf = NULL;
        return SU_E_IO;
    }
    *len = got;
    return SU_OK;
}

/* Checks the host object against the descriptor. Returns SU_OK or the refusal code with msg set. */
static uint32_t check_host_object(const SuJsonValue *host, const SuHostOpDesc *d, char *msg, size_t cap) {
    const SuJsonValue *fields = jmember(host, "fields");
    uint64_t ver = 0;
    char hex[65];
    uint32_t i;

    if (host == NULL || host->type != SU_JSON_OBJECT) {
        HOST_MSG_SET(msg, cap, "host capsule has no host object");
        return SU_E_SCHEMA_MISMATCH;
    }
    /* Operator family: rts_ops for descriptors with only u32 lanes (M3), "<schema_id>_ops" for descriptors with
     * typed lanes (space3d_ops for the space3d schema). */
    char operator_name[SU_WORLD_NAME_MAX + 32u];
    snprintf(operator_name, sizeof operator_name, "%s_ops_v%u",
             strcmp(host_profile(d), SU_NUMERIC_PROFILE_U32_MOD_V1) == 0 ? "rts" : d->schema_id,
             (unsigned)d->operator_version);
    if (strcmp(jstr(jmember(host, "operator")) != NULL ? jstr(jmember(host, "operator")) : "", operator_name) != 0) {
        HOST_MSG_SET(msg, cap, "host.operator is not %s", operator_name);
        return SU_E_SCHEMA_MISMATCH;
    }
    if (!juint(jmember(host, "operator_version"), &ver) || ver != d->operator_version) {
        HOST_MSG_SET(msg, cap, "host.operator_version differs from the descriptor (%u)", (unsigned)d->operator_version);
        return SU_E_SCHEMA_MISMATCH;
    }
    su_sha256_hex(d->constants_hash, hex);
    if (jstr(jmember(host, "constants_hash")) == NULL || strcmp(jstr(jmember(host, "constants_hash")), hex) != 0) {
        HOST_MSG_SET(msg, cap, "host.constants_hash differs from the descriptor");
        return SU_E_SCHEMA_MISMATCH;
    }
    if (fields == NULL || fields->type != SU_JSON_ARRAY || fields->u.array.count != d->field_count) {
        HOST_MSG_SET(msg, cap, "host.fields differs from the descriptor field table (count)");
        return SU_E_SCHEMA_MISMATCH;
    }
    for (i = 0; i < d->field_count; i++) {
        const SuJsonValue *f = fields->u.array.items[i];
        const char *name = jstr(jmember(f, "name"));
        const char *acc = jstr(jmember(f, "access"));
        uint64_t id = 0;
        if (f == NULL || f->type != SU_JSON_OBJECT || !juint(jmember(f, "id"), &id) || id != d->fields[i].id ||
            name == NULL || strcmp(name, d->fields[i].name) != 0 ||
            jstr(jmember(f, "type")) == NULL || strcmp(jstr(jmember(f, "type")), su_type_name(d->fields[i].type)) != 0 ||
            acc == NULL || access_of_name(acc) != d->fields[i].access) {
            HOST_MSG_SET(msg, cap, "host.fields[%u] differs from the descriptor (id, name, type or access)",
                         (unsigned)i);
            return SU_E_SCHEMA_MISMATCH;
        }
    }
    return SU_OK;
}

uint32_t su_host_load(const char *capsule_path, const char *expect_capsule_hash, const SuHostOpDesc *d,
                      SuLirModule **out, uint32_t *schema_version, uint32_t *layout_generation, char *msg,
                      size_t msg_cap) {
    static const SuJsonLimits lim = {16u * 1024u * 1024u, 32u};
    uint8_t *bytes = NULL;
    size_t len = 0;
    SuJsonDoc doc;
    SuJsonError jerr;
    SuSha256 sha;
    uint8_t digest[32];
    char hex[65];
    const SuJsonValue *root;
    const SuJsonValue *schema;
    const SuJsonValue *sched;
    SuLirModule *decl = NULL;
    uint64_t sv = 0;
    uint64_t gen = 0;
    uint32_t st;

    if (out == NULL || schema_version == NULL || layout_generation == NULL || capsule_path == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    *schema_version = 0u;
    *layout_generation = 0u;
    if (su_host_desc_validate(d) != SU_OK) {
        HOST_MSG_SET(msg, msg_cap, "host descriptor is malformed");
        return SU_E_INVALID_ARGUMENT;
    }
    st = read_capsule_bytes(capsule_path, &bytes, &len);
    if (st != SU_OK) {
        HOST_MSG_SET(msg, msg_cap, "host capsule cannot be read");
        return st;
    }
    su_sha256_init(&sha);
    su_sha256_update(&sha, bytes, len);
    su_sha256_final(&sha, digest);
    su_sha256_hex(digest, hex);
    if (expect_capsule_hash != NULL && strcmp(expect_capsule_hash, hex) != 0) {
        free(bytes);
        HOST_MSG_SET(msg, msg_cap, "host capsule bytes differ from capsule_sha256");
        return SU_E_CANDIDATE_FAILED;
    }
    memset(&doc, 0, sizeof doc);
    st = su_json_parse(bytes, len, &lim, &doc, &jerr);
    if (st != SU_OK) {
        /* A duplicate-key error leaves a complete tree in doc; free it either way. */
        su_json_doc_free(&doc);
        free(bytes);
        HOST_MSG_SET(msg, msg_cap, "host capsule is not valid strict JSON: %s", jerr.message);
        return st != SU_OK ? st : SU_E_PARSE;
    }
    root = doc.root;
    if (root == NULL || root->type != SU_JSON_OBJECT) {
        st = SU_E_TYPE;
        HOST_MSG_SET(msg, msg_cap, "host capsule top level must be an object");
        goto done;
    }
    if (jmember(root, "source") != NULL || jmember(root, "fields") != NULL || jmember(root, "ir_version") != NULL) {
        st = SU_E_SCHEMA_MISMATCH;
        HOST_MSG_SET(msg, msg_cap, "host capsule carries an IR-only key (source, fields or ir_version)");
        goto done;
    }
    if (jstr(jmember(root, "capsule_format")) == NULL || strcmp(jstr(jmember(root, "capsule_format")), "0.1") != 0) {
        st = SU_E_SCHEMA_MISMATCH;
        HOST_MSG_SET(msg, msg_cap, "capsule_format is not 0.1");
        goto done;
    }
    if (jstr(jmember(root, "system_id")) == NULL || strcmp(jstr(jmember(root, "system_id")), d->system_id) != 0) {
        st = SU_E_INVALID_ARGUMENT;
        HOST_MSG_SET(msg, msg_cap, "host capsule system_id is not %s", d->system_id);
        goto done;
    }
    schema = jmember(root, "schema");
    if (schema == NULL || schema->type != SU_JSON_OBJECT || jstr(jmember(schema, "id")) == NULL ||
        strcmp(jstr(jmember(schema, "id")), d->schema_id) != 0 || !juint(jmember(schema, "version"), &sv) ||
        !juint(jmember(schema, "layout_generation"), &gen) || sv == 0u || gen == 0u || sv > UINT32_MAX ||
        gen > UINT32_MAX) {
        st = SU_E_SCHEMA_MISMATCH;
        HOST_MSG_SET(msg, msg_cap, "host capsule schema differs from the world schema %s", d->schema_id);
        goto done;
    }
    if (jstr(jmember(root, "numeric_profile")) == NULL ||
        strcmp(jstr(jmember(root, "numeric_profile")), host_profile(d)) != 0) {
        st = SU_E_TYPE;
        HOST_MSG_SET(msg, msg_cap, "host capsule numeric_profile is not %s", host_profile(d));
        goto done;
    }
    sched = jmember(root, "schedule");
    if (sched == NULL || sched->type != SU_JSON_OBJECT || jstr(jmember(sched, "phase")) == NULL) {
        st = SU_E_SCHEMA_MISMATCH;
        HOST_MSG_SET(msg, msg_cap, "host capsule has no schedule block with a phase");
        goto done;
    }
    st = check_host_object(jmember(root, "host"), d, msg, msg_cap);
    if (st != SU_OK) {
        goto done;
    }
    decl = calloc(1u, sizeof *decl);
    if (decl == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        HOST_MSG_SET(msg, msg_cap, "out of memory for the host declaration");
        goto done;
    }
    st = su_host_decl_build(d, decl);
    if (st != SU_OK) {
        free(decl);
        decl = NULL;
        HOST_MSG_SET(msg, msg_cap, "host declaration could not be built");
        goto done;
    }
    memcpy(decl->capsule_hash, digest, sizeof digest);
    memcpy(decl->capsule_hash_hex, hex, sizeof hex);
    *out = decl;
    decl = NULL;
    *schema_version = (uint32_t)sv;
    *layout_generation = (uint32_t)gen;
    st = SU_OK;

done:
    su_json_doc_free(&doc);
    free(bytes);
    if (decl != NULL) {
        su_host_decl_free(decl);
    }
    return st;
}

int su_host_capsule_is_host(const char *path) {
    static const SuJsonLimits lim = {16u * 1024u * 1024u, 32u};
    uint8_t *bytes = NULL;
    size_t len = 0;
    SuJsonDoc doc;
    SuJsonError jerr;
    int is_host = 0;
    if (path == NULL || read_capsule_bytes(path, &bytes, &len) != SU_OK) {
        free(bytes);
        return 0;
    }
    memset(&doc, 0, sizeof doc);
    if (su_json_parse(bytes, len, &lim, &doc, &jerr) == SU_OK && doc.root != NULL &&
        doc.root->type == SU_JSON_OBJECT && jmember(doc.root, "host") != NULL) {
        is_host = 1;
    }
    su_json_doc_free(&doc);
    free(bytes);
    return is_host;
}
