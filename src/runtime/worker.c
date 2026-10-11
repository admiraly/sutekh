#define _POSIX_C_SOURCE 200809L

#include "runtime/worker.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent/su_module.h"
#include "agent/su_path.h"
#include "agent/su_proto.h"
#include "ir/su_json.h"
#include "ir/su_lir.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "vm/su_vm.h"
#include "world/su_world.h"

#define WORKER_MAX_WORLDS 4u
#define WORKER_CAPSULE_MAX (64u * 1024u)
#define WORKER_TEST_MAX (256u * 1024u)
#define WORKER_FIXTURE_MAX (8u * 1024u * 1024u)
#define WORKER_INSPECT_DEFAULT_ROWS 16u
#define WORKER_INSPECT_MAX_ROWS 256u
#define WORKER_MAX_TICKS 100000u
#define WORKER_MSG_CAP 256u
#define WORKER_MAX_FAILURES 32u
#define WORKER_NAME_MAX 255u

/* Loaded capsule: owned verified module plus the schema generation it binds. */
typedef struct WorkerModule {
    SuLirModule mod;
    uint32_t schema_version;
    uint32_t gen;
} WorkerModule;

/* One world instance: fixture state on a fixed-row table with its VM context.
 * ctx borrows &module->mod, so the module must outlive the instance. */
typedef struct WorkerInstance {
    SuWorld *w;
    SuExecContext *ctx;
    uint32_t *ids;
    uint64_t rows;
} WorkerInstance;

typedef struct WorkerWorld {
    int used;
    char id[16];
    uint64_t revision;
    WorkerModule module;
    WorkerInstance inst;
} WorkerWorld;

typedef struct WorkerState {
    WorkerWorld worlds[WORKER_MAX_WORLDS];
    uint64_t next_id;
    SuWorkerEvalHook before_eval;
} WorkerState;

typedef struct WorkerFail {
    uint32_t code;
    char msg[WORKER_MSG_CAP];
} WorkerFail;

static void fail(WorkerFail *f, uint32_t code, const char *msg) {
    f->code = code;
    snprintf(f->msg, sizeof f->msg, "%s", msg);
}

static const char *str_of(const SuJsonValue *obj, const char *key) {
    const SuJsonValue *v = obj != NULL && obj->type == SU_JSON_OBJECT ? su_json_member(obj, key) : NULL;
    return su_json_is_string(v) ? v->u.string.data : NULL;
}

/* ---------- module ---------- */

static void module_free(WorkerModule *m) {
    su_lir_module_free(&m->mod);
    memset(m, 0, sizeof *m);
}

/* Reads and verifies a capsule and its source. Optional expected hashes must match
 * the captured bytes (candidate identity check). */
static uint32_t module_load(WorkerModule *m, const char *capsule_path, const char *source_path,
                            const char *expect_source_hash, const char *expect_capsule_hash,
                            WorkerFail *f) {
    uint8_t *cap = NULL;
    uint8_t *lir = NULL;
    size_t cap_len = 0;
    size_t lir_len = 0;
    char source_name[WORKER_NAME_MAX + 1];
    SuDiag diag;
    uint32_t st;

    memset(m, 0, sizeof *m);
    st = su_path_read_file(capsule_path, WORKER_CAPSULE_MAX, &cap, &cap_len);
    if (st != SU_OK) {
        fail(f, st, "cannot read capsule");
        goto done;
    }
    st = su_path_read_file(source_path, SU_LIMIT_IR_SOURCE_BYTES, &lir, &lir_len);
    if (st != SU_OK) {
        fail(f, st, "cannot read source module");
        goto done;
    }
    st = su_module_capsule_meta(cap, cap_len, source_name, sizeof source_name,
                                &m->schema_version, &m->gen);
    if (st != SU_OK) {
        fail(f, st, "capsule metadata is malformed");
        goto done;
    }
    su_diag_init(&diag);
    st = su_lir_verify(cap, cap_len, lir, lir_len, &m->mod, &diag);
    if (st != SU_OK) {
        fail(f, st, diag.message);
        goto done;
    }
    if ((expect_source_hash != NULL && strcmp(expect_source_hash, m->mod.source_hash_hex) != 0) ||
        (expect_capsule_hash != NULL && strcmp(expect_capsule_hash, m->mod.capsule_hash_hex) != 0)) {
        fail(f, SU_E_CANDIDATE_FAILED, "candidate bytes changed since submission");
        module_free(m);
        goto done;
    }
    st = SU_OK;
done:
    free(cap);
    free(lir);
    return st;
}

/* ---------- instance ---------- */

static void instance_free(WorkerInstance *in) {
    su_vm_context_destroy(in->ctx);
    su_world_destroy(in->w);
    free(in->ids);
    memset(in, 0, sizeof *in);
}

static uint32_t read_u32_array(const SuJsonValue *arr, uint64_t expect, uint32_t **out) {
    size_t i;
    uint32_t *vals;
    if (arr == NULL || arr->type != SU_JSON_ARRAY || arr->u.array.count != expect) {
        return SU_E_SCHEMA_MISMATCH;
    }
    vals = malloc(expect > 0 ? (size_t)expect * sizeof *vals : sizeof *vals);
    if (vals == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0; i < expect; i++) {
        uint64_t u;
        if (!su_json_get_uint(arr->u.array.items[i], &u) || u > UINT32_MAX) {
            free(vals);
            return SU_E_TYPE;
        }
        vals[i] = (uint32_t)u;
    }
    *out = vals;
    return SU_OK;
}

/* Opens a fresh instance from a fixture world file (tick 0, columns by name). */
static uint32_t instance_open(const WorkerModule *m, const char *fixture_path,
                              WorkerInstance *in, WorkerFail *f) {
    uint8_t *data = NULL;
    size_t len = 0;
    SuJsonLimits limits;
    SuJsonDoc doc;
    SuJsonError jerr;
    const SuJsonValue *root;
    const SuJsonValue *cols;
    const SuJsonValue *ids_arr;
    uint64_t tick = 1;
    uint64_t rows;
    SuWorldFieldDesc *descs = NULL;
    SuWorldDesc desc;
    uint32_t *vals = NULL;
    uint32_t i;
    uint32_t st;

    memset(in, 0, sizeof *in);
    st = su_path_read_file(fixture_path, WORKER_FIXTURE_MAX, &data, &len);
    if (st != SU_OK) {
        fail(f, st, "cannot read world fixture");
        return st;
    }
    limits.max_bytes = len;
    limits.max_depth = SU_PROTO_MAX_DEPTH + 4u;
    memset(&doc, 0, sizeof doc);
    st = su_json_parse(data, len, &limits, &doc, &jerr);
    free(data);
    if (st != SU_OK) {
        su_json_doc_free(&doc);
        fail(f, st, jerr.message);
        return st;
    }
    root = doc.root;
    if (root == NULL || root->type != SU_JSON_OBJECT) {
        st = SU_E_TYPE;
        fail(f, st, "world fixture must be a JSON object");
        goto out;
    }
    if (!su_json_get_uint(su_json_member(root, "tick"), &tick) || tick != 0) {
        st = SU_E_TYPE;
        fail(f, st, "world fixture tick must be 0");
        goto out;
    }
    if (strcmp(str_of(root, "schema_id") != NULL ? str_of(root, "schema_id") : "", m->mod.query) != 0) {
        st = SU_E_SCHEMA_MISMATCH;
        fail(f, st, "fixture schema_id does not match the capsule schema");
        goto out;
    }
    ids_arr = su_json_member(root, "entity_ids");
    if (ids_arr == NULL || ids_arr->type != SU_JSON_ARRAY) {
        st = SU_E_TYPE;
        fail(f, st, "entity_ids must be an array");
        goto out;
    }
    rows = ids_arr->u.array.count;
    if (rows > SU_LIMIT_ROWS) {
        st = SU_E_RESOURCE_LIMIT;
        fail(f, st, "fixture row count exceeds the row limit");
        goto out;
    }
    st = read_u32_array(ids_arr, rows, &in->ids);
    if (st != SU_OK) {
        fail(f, st, "entity_ids must be u32 integers");
        goto out;
    }
    in->rows = rows;
    cols = su_json_member(root, "columns");
    if (cols == NULL || cols->type != SU_JSON_OBJECT) {
        st = SU_E_TYPE;
        fail(f, st, "columns must be an object keyed by field name");
        goto out;
    }

    descs = malloc((m->mod.field_count > 0 ? m->mod.field_count : 1u) * sizeof *descs);
    if (descs == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        fail(f, st, "allocation failed");
        goto out;
    }
    for (i = 0; i < m->mod.field_count; i++) {
        descs[i].id = m->mod.fields[i].id;
        descs[i].type = m->mod.fields[i].type;
        descs[i].access = m->mod.fields[i].access;
        descs[i].name = m->mod.fields[i].name;
    }
    memset(&desc, 0, sizeof desc);
    desc.schema_id = m->mod.query;
    desc.schema_version = m->schema_version;
    desc.layout_generation = m->gen;
    desc.row_count = rows;
    desc.entity_ids = in->ids;
    desc.fields = descs;
    desc.field_count = m->mod.field_count;
    st = su_world_create(&desc, &in->w);
    if (st != SU_OK) {
        fail(f, st, "world creation failed");
        goto out;
    }
    for (i = 0; i < m->mod.field_count; i++) {
        const SuJsonValue *arr = su_json_member(cols, m->mod.fields[i].name);
        st = read_u32_array(arr, rows, &vals);
        if (st != SU_OK) {
            fail(f, st, "fixture column is missing, mis-sized or not u32");
            goto out;
        }
        st = su_world_init_committed(in->w, m->mod.fields[i].id, vals, rows);
        free(vals);
        vals = NULL;
        if (st != SU_OK) {
            fail(f, st, "fixture column initialization failed");
            goto out;
        }
    }
    st = su_vm_context_create(&m->mod, m->gen, &in->ctx);
    if (st != SU_OK) {
        fail(f, st, "VM context creation failed");
        goto out;
    }
    st = SU_OK;
out:
    free(descs);
    free(vals);
    su_json_doc_free(&doc);
    if (st != SU_OK) {
        instance_free(in);
    }
    return st;
}

/* Runs `ticks` complete ticks: begin, run the system, publish or abort. Stops at
 * the first failure; *done counts published ticks. */
static uint32_t instance_run_ticks(WorkerInstance *in, uint64_t ticks, uint64_t *done,
                                   WorkerFail *f) {
    uint64_t i;
    uint32_t st;

    *done = 0;
    for (i = 0; i < ticks; i++) {
        st = su_world_begin_tick(in->w);
        if (st != SU_OK) {
            fail(f, st, "begin_tick failed");
            return st;
        }
        st = su_vm_run_system(in->w, in->ctx);
        if (st != SU_OK) {
            su_world_abort(in->w);
            fail(f, st, "system failed; tick discarded, committed state unchanged");
            return st;
        }
        st = su_world_publish(in->w);
        if (st != SU_OK) {
            su_world_abort(in->w);
            fail(f, st, "publish failed; tick discarded");
            return st;
        }
        *done = i + 1u;
    }
    return SU_OK;
}

/* ---------- world table ---------- */

static void rev_string(const WorkerWorld *ww, char *out, size_t cap) {
    snprintf(out, cap, "%s.r%llu", ww->id, (unsigned long long)ww->revision);
}

static WorkerWorld *find_world(WorkerState *s, const char *id) {
    uint32_t i;
    if (id == NULL) {
        return NULL;
    }
    for (i = 0; i < WORKER_MAX_WORLDS; i++) {
        if (s->worlds[i].used && strcmp(s->worlds[i].id, id) == 0) {
            return &s->worlds[i];
        }
    }
    return NULL;
}

static void world_drop(WorkerWorld *ww) {
    instance_free(&ww->inst);
    module_free(&ww->module);
    memset(ww, 0, sizeof *ww);
}

static uint32_t get_world_param(WorkerState *s, const SuJsonValue *params, WorkerWorld **out,
                                WorkerFail *f) {
    const char *id = str_of(params, "world_id");
    *out = find_world(s, id);
    if (*out == NULL) {
        fail(f, SU_E_INVALID_ARGUMENT, "unknown world_id");
        return SU_E_INVALID_ARGUMENT;
    }
    return SU_OK;
}

/* ---------- handlers ---------- */

static uint32_t h_world_create(WorkerState *s, const SuJsonValue *params, SuBuf *res,
                               WorkerFail *f) {
    const char *fixture = str_of(params, "fixture_path");
    const char *capsule = str_of(params, "capsule_path");
    const char *source = str_of(params, "source_path");
    WorkerWorld *ww = NULL;
    uint32_t i;
    uint32_t st;
    SuBuf tmp;

    if (fixture == NULL || capsule == NULL || source == NULL) {
        fail(f, SU_E_TYPE, "world.create needs fixture_path, capsule_path and source_path");
        return SU_E_TYPE;
    }
    for (i = 0; i < WORKER_MAX_WORLDS; i++) {
        if (!s->worlds[i].used) {
            ww = &s->worlds[i];
            break;
        }
    }
    if (ww == NULL) {
        fail(f, SU_E_RESOURCE_LIMIT, "worker already holds the maximum number of worlds");
        return SU_E_RESOURCE_LIMIT;
    }
    st = module_load(&ww->module, capsule, source, NULL, NULL, f);
    if (st != SU_OK) {
        memset(ww, 0, sizeof *ww);
        return st;
    }
    st = instance_open(&ww->module, fixture, &ww->inst, f);
    if (st != SU_OK) {
        module_free(&ww->module);
        memset(ww, 0, sizeof *ww);
        return st;
    }
    s->next_id++;
    snprintf(ww->id, sizeof ww->id, "world-%llu", (unsigned long long)s->next_id);
    ww->revision = 1;
    ww->used = 1;

    su_buf_init(&tmp);
    su_buf_str(&tmp, "{\"world_id\":");
    su_buf_quoted(&tmp, ww->id, strlen(ww->id));
    {
        char rev[64];
        rev_string(ww, rev, sizeof rev);
        su_buf_str(&tmp, ",\"world_revision\":");
        su_buf_quoted(&tmp, rev, strlen(rev));
    }
    su_buf_str(&tmp, ",\"tick\":");
    su_buf_u64(&tmp, su_world_tick(ww->inst.w));
    su_buf_str(&tmp, ",\"row_count\":");
    su_buf_u64(&tmp, ww->inst.rows);
    su_buf_str(&tmp, ",\"schema_id\":");
    su_buf_quoted(&tmp, ww->module.mod.query, strlen(ww->module.mod.query));
    su_buf_str(&tmp, ",\"system_id\":");
    su_buf_quoted(&tmp, ww->module.mod.system_id, strlen(ww->module.mod.system_id));
    su_buf_str(&tmp, ",\"source_hash\":\"");
    su_buf_str(&tmp, ww->module.mod.source_hash_hex);
    su_buf_str(&tmp, "\",\"capsule_hash\":\"");
    su_buf_str(&tmp, ww->module.mod.capsule_hash_hex);
    su_buf_str(&tmp, "\",\"semantic_hash\":\"");
    su_buf_str(&tmp, ww->module.mod.semantic_hash_hex);
    su_buf_str(&tmp, "\"}");
    su_buf_put(res, tmp.data, tmp.len);
    su_buf_free(&tmp);
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

static uint32_t h_world_inspect(WorkerState *s, const SuJsonValue *params, SuBuf *res,
                                WorkerFail *f) {
    WorkerWorld *ww;
    uint64_t limit = WORKER_INSPECT_DEFAULT_ROWS;
    uint64_t shown;
    const SuJsonValue *fields;
    const SuJsonValue *lim;
    uint32_t i;
    uint32_t st = get_world_param(s, params, &ww, f);
    char rev[64];

    if (st != SU_OK) {
        return st;
    }
    lim = su_json_member(params, "limit");
    if (lim != NULL && (!su_json_get_uint(lim, &limit) || limit < 1u || limit > WORKER_INSPECT_MAX_ROWS)) {
        fail(f, SU_E_TYPE, "limit must be an integer 1..256");
        return SU_E_TYPE;
    }
    shown = ww->inst.rows < limit ? ww->inst.rows : limit;
    rev_string(ww, rev, sizeof rev);

    su_buf_str(res, "{\"world_id\":");
    su_buf_quoted(res, ww->id, strlen(ww->id));
    su_buf_str(res, ",\"world_revision\":");
    su_buf_quoted(res, rev, strlen(rev));
    su_buf_str(res, ",\"tick\":");
    su_buf_u64(res, su_world_tick(ww->inst.w));
    su_buf_str(res, ",\"row_count\":");
    su_buf_u64(res, ww->inst.rows);
    su_buf_str(res, ",\"returned_rows\":");
    su_buf_u64(res, shown);
    su_buf_str(res, ",\"entity_ids\":[");
    for (i = 0; i < shown; i++) {
        if (i > 0) {
            su_buf_str(res, ",");
        }
        su_buf_u64(res, ww->inst.ids[i]);
    }
    su_buf_str(res, "],\"columns\":{");

    fields = su_json_member(params, "fields");
    {
        uint32_t n = 0;
        uint32_t count = ww->module.mod.field_count;
        uint32_t k;
        if (fields != NULL) {
            count = fields->type == SU_JSON_ARRAY ? (uint32_t)fields->u.array.count : 0u;
        }
        for (k = 0; k < count; k++) {
            const char *name;
            uint32_t j;
            const SuJsonValue *item = fields != NULL ? fields->u.array.items[k] : NULL;
            name = fields != NULL ? (su_json_is_string(item) ? item->u.string.data : NULL)
                                  : ww->module.mod.fields[k].name;
            if (name == NULL) {
                fail(f, SU_E_TYPE, "fields must be an array of strings");
                return SU_E_TYPE;
            }
            {
                int known = 0;
                for (j = 0; j < ww->module.mod.field_count; j++) {
                    if (strcmp(ww->module.mod.fields[j].name, name) == 0) {
                        known = 1;
                    }
                }
                if (!known) {
                    fail(f, SU_E_SCHEMA_MISMATCH, "unknown field name");
                    return SU_E_SCHEMA_MISMATCH;
                }
            }
            {
                const uint32_t *data = NULL;
                uint64_t rc = 0;
                uint64_t r;
                if (su_world_read_committed_by_name(ww->inst.w, name, &data, &rc) != SU_OK) {
                    fail(f, SU_E_SCHEMA_MISMATCH, "field not readable");
                    return SU_E_SCHEMA_MISMATCH;
                }
                if (n > 0) {
                    su_buf_str(res, ",");
                }
                su_buf_quoted(res, name, strlen(name));
                su_buf_str(res, ":[");
                for (r = 0; r < shown; r++) {
                    if (r > 0) {
                        su_buf_str(res, ",");
                    }
                    su_buf_u64(res, data[r]);
                }
                su_buf_str(res, "]");
            }
            n++;
        }
    }
    su_buf_str(res, "}}");
    if (res->failed) {
        fail(f, SU_E_RESOURCE_LIMIT, "inspect result exceeds the message limit");
        return SU_E_RESOURCE_LIMIT;
    }
    return SU_OK;
}

static uint32_t h_world_step(WorkerState *s, const SuJsonValue *params, SuBuf *res,
                             WorkerFail *f) {
    WorkerWorld *ww;
    const char *expect = str_of(params, "expected_world_revision");
    uint64_t ticks = 0;
    uint64_t done = 0;
    char rev[64];
    uint32_t st = get_world_param(s, params, &ww, f);

    if (st != SU_OK) {
        return st;
    }
    if (expect == NULL || !su_json_get_uint(su_json_member(params, "ticks"), &ticks) ||
        ticks > WORKER_MAX_TICKS) {
        fail(f, SU_E_TYPE, "world.step needs expected_world_revision and ticks <= 100000");
        return SU_E_TYPE;
    }
    rev_string(ww, rev, sizeof rev);
    if (strcmp(expect, rev) != 0) {
        fail(f, SU_E_STALE_REVISION, "expected_world_revision does not match the active world");
        return SU_E_STALE_REVISION;
    }
    st = instance_run_ticks(&ww->inst, ticks, &done, f);
    ww->revision += done;
    if (st != SU_OK) {
        char more[2u * WORKER_MSG_CAP];
        rev_string(ww, rev, sizeof rev);
        snprintf(more, sizeof more, "%.200s; completed %llu of %llu ticks, world_revision now %s",
                 f->msg, (unsigned long long)done, (unsigned long long)ticks, rev);
        fail(f, st, more);
        return st;
    }
    rev_string(ww, rev, sizeof rev);
    su_buf_str(res, "{\"world_id\":");
    su_buf_quoted(res, ww->id, strlen(ww->id));
    su_buf_str(res, ",\"world_revision\":");
    su_buf_quoted(res, rev, strlen(rev));
    su_buf_str(res, ",\"ticks_completed\":");
    su_buf_u64(res, done);
    su_buf_str(res, ",\"tick\":");
    su_buf_u64(res, su_world_tick(ww->inst.w));
    su_buf_str(res, "}");
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

typedef struct CaseFailures {
    uint32_t count;
    char names[WORKER_MAX_FAILURES][WORKER_NAME_MAX + 1];
    char reasons[WORKER_MAX_FAILURES][WORKER_MSG_CAP];
} CaseFailures;

static void note_failure(CaseFailures *cf, const char *name, const char *reason) {
    if (cf->count < WORKER_MAX_FAILURES) {
        snprintf(cf->names[cf->count], sizeof cf->names[0], "%s", name);
        snprintf(cf->reasons[cf->count], sizeof cf->reasons[0], "%s", reason);
    }
    cf->count++;
}

/* Compares expected columns of one case against the committed state. */
static int case_columns_match(const WorkerModule *m, const WorkerInstance *in,
                              const SuJsonValue *expected, char *reason, size_t reason_cap) {
    size_t i;
    if (expected == NULL || expected->type != SU_JSON_OBJECT) {
        snprintf(reason, reason_cap, "expected_columns_malformed");
        return 0;
    }
    for (i = 0; i < expected->u.object.count; i++) {
        const SuJsonMember *mem = &expected->u.object.members[i];
        char name[WORKER_NAME_MAX + 1];
        const uint32_t *data = NULL;
        uint64_t rc = 0;
        uint64_t r;
        int known = 0;
        size_t j;
        if (mem->key_len > WORKER_NAME_MAX) {
            snprintf(reason, reason_cap, "unknown_field");
            return 0;
        }
        memcpy(name, mem->key, mem->key_len);
        name[mem->key_len] = '\0';
        for (j = 0; j < m->mod.field_count; j++) {
            if (strcmp(m->mod.fields[j].name, name) == 0) {
                known = 1;
            }
        }
        if (!known || su_world_read_committed_by_name(in->w, name, &data, &rc) != SU_OK) {
            snprintf(reason, reason_cap, "unknown_field:%s", name);
            return 0;
        }
        if (mem->value->type != SU_JSON_ARRAY || mem->value->u.array.count != rc) {
            snprintf(reason, reason_cap, "column_length:%s", name);
            return 0;
        }
        for (r = 0; r < rc; r++) {
            uint64_t want;
            if (!su_json_get_uint(mem->value->u.array.items[r], &want) || want != data[r]) {
                snprintf(reason, reason_cap, "column_mismatch:%s[%llu]", name,
                         (unsigned long long)r);
                return 0;
            }
        }
    }
    return 1;
}

static uint32_t h_candidate_evaluate(WorkerState *s, const SuJsonValue *params, SuBuf *res,
                                     WorkerFail *f) {
    const char *capsule = str_of(params, "capsule_path");
    const char *source = str_of(params, "source_path");
    const char *test_path = str_of(params, "test_path");
    const char *world_path = str_of(params, "world_path");
    const char *exp_src = str_of(params, "expected_source_hash");
    const char *exp_cap = str_of(params, "expected_capsule_hash");
    WorkerModule m;
    uint8_t *tdata = NULL;
    size_t tlen = 0;
    SuJsonLimits limits;
    SuJsonDoc doc;
    SuJsonError jerr;
    const SuJsonValue *cases;
    CaseFailures cf;
    uint64_t total = 0;
    uint64_t passed = 0;
    size_t i;
    uint32_t st;

    if (s->before_eval != NULL) {
        s->before_eval();
    }
    if (capsule == NULL || source == NULL || test_path == NULL || world_path == NULL ||
        exp_src == NULL || exp_cap == NULL) {
        fail(f, SU_E_TYPE, "candidate evaluation parameters are incomplete");
        return SU_E_TYPE;
    }
    memset(&cf, 0, sizeof cf);
    st = module_load(&m, capsule, source, exp_src, exp_cap, f);
    if (st != SU_OK) {
        return st;
    }
    st = su_path_read_file(test_path, WORKER_TEST_MAX, &tdata, &tlen);
    if (st != SU_OK) {
        module_free(&m);
        fail(f, st, "cannot read test file");
        return st;
    }
    limits.max_bytes = tlen;
    limits.max_depth = SU_PROTO_MAX_DEPTH;
    memset(&doc, 0, sizeof doc);
    st = su_json_parse(tdata, tlen, &limits, &doc, &jerr);
    free(tdata);
    if (st != SU_OK) {
        su_json_doc_free(&doc);
        module_free(&m);
        fail(f, st, jerr.message);
        return st;
    }
    cases = doc.root != NULL && doc.root->type == SU_JSON_OBJECT ? su_json_member(doc.root, "cases")
                                                                  : NULL;
    if (cases == NULL || cases->type != SU_JSON_ARRAY) {
        st = SU_E_TYPE;
        fail(f, st, "test file needs a cases array");
        goto out;
    }
    for (i = 0; i < cases->u.array.count; i++) {
        const SuJsonValue *c = cases->u.array.items[i];
        const char *name = str_of(c, "name");
        const SuJsonValue *ticks_v = c != NULL && c->type == SU_JSON_OBJECT ? su_json_member(c, "ticks") : NULL;
        const SuJsonValue *exp = c != NULL && c->type == SU_JSON_OBJECT ? su_json_member(c, "expected_columns") : NULL;
        uint64_t ticks = 0;
        WorkerInstance in;
        uint64_t done = 0;
        char reason[WORKER_MSG_CAP];

        if (name == NULL || ticks_v == NULL || !su_json_get_uint(ticks_v, &ticks) ||
            ticks > WORKER_MAX_TICKS || exp == NULL) {
            st = SU_E_TYPE;
            fail(f, st, "each case needs name, ticks (<= 100000) and expected_columns");
            goto out;
        }
        total++;
        st = instance_open(&m, world_path, &in, f);
        if (st != SU_OK) {
            goto out;
        }
        st = instance_run_ticks(&in, ticks, &done, f);
        if (st != SU_OK) {
            char r2[WORKER_MSG_CAP];
            snprintf(r2, sizeof r2, "tick_failed:%s", su_status_name(st));
            note_failure(&cf, name, r2);
        } else if (!case_columns_match(&m, &in, exp, reason, sizeof reason)) {
            note_failure(&cf, name, reason);
        } else {
            passed++;
        }
        instance_free(&in);
    }
    su_buf_str(res, "{\"status\":\"");
    su_buf_str(res, passed == total ? "passed" : "failed");
    su_buf_str(res, "\",\"cases_total\":");
    su_buf_u64(res, total);
    su_buf_str(res, ",\"cases_passed\":");
    su_buf_u64(res, passed);
    su_buf_str(res, ",\"failures\":[");
    for (i = 0; i < cf.count && i < WORKER_MAX_FAILURES; i++) {
        if (i > 0) {
            su_buf_str(res, ",");
        }
        su_buf_str(res, "{\"name\":");
        su_buf_quoted(res, cf.names[i], strlen(cf.names[i]));
        su_buf_str(res, ",\"reason\":");
        su_buf_quoted(res, cf.reasons[i], strlen(cf.reasons[i]));
        su_buf_str(res, "}");
    }
    su_buf_str(res, "]}");
    st = res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
out:
    su_json_doc_free(&doc);
    module_free(&m);
    return st;
}

/* ---------- dispatch and main loop ---------- */

static void handle_line(WorkerState *s, const uint8_t *line, size_t len, int overflow,
                        SuBuf *out) {
    SuJsonDoc doc;
    SuRequest req;
    char msg[WORKER_MSG_CAP];
    WorkerFail f;
    SuBuf res;
    uint32_t st;

    st = su_proto_parse_request(line, len, overflow, &doc, &req, msg, sizeof msg);
    if (st != SU_OK) {
        su_proto_error(out, req.has_id, req.id, st, msg);
        su_json_doc_free(&doc);
        return;
    }
    su_buf_init(&res);
    memset(&f, 0, sizeof f);
    if (strcmp(req.method, "world.create") == 0) {
        st = h_world_create(s, req.params, &res, &f);
    } else if (strcmp(req.method, "world.inspect") == 0) {
        st = h_world_inspect(s, req.params, &res, &f);
    } else if (strcmp(req.method, "world.step") == 0) {
        st = h_world_step(s, req.params, &res, &f);
    } else if (strcmp(req.method, "internal.candidate.evaluate") == 0) {
        st = h_candidate_evaluate(s, req.params, &res, &f);
    } else {
        st = SU_E_UNSUPPORTED_METHOD;
        fail(&f, st, "worker does not implement this method");
    }
    if (st == SU_OK) {
        su_proto_ok(out, req.has_id, req.id, &res);
    } else {
        su_proto_error(out, req.has_id, req.id, f.code != 0 ? f.code : st, f.msg);
    }
    if (out->failed) {
        su_buf_free(out);
        su_buf_init(out);
        su_proto_error(out, req.has_id, req.id, SU_E_RESOURCE_LIMIT,
                       "response exceeds the 1 MiB message limit");
    }
    su_buf_free(&res);
    su_json_doc_free(&doc);
}

static void free_worlds(WorkerState *s) {
    uint32_t i;
    for (i = 0; i < WORKER_MAX_WORLDS; i++) {
        if (s->worlds[i].used) {
            world_drop(&s->worlds[i]);
        }
    }
}

int su_worker_main_with_hook(int in_fd, int out_fd, SuWorkerEvalHook before_eval) {
    WorkerState *s;
    SuLineReader r;
    struct sigaction ign;
    struct sigaction old;
    int ret = 0;

    s = calloc(1, sizeof *s);
    if (s == NULL) {
        return 1;
    }
    s->before_eval = before_eval;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, &old);

    su_line_reader_init(&r, in_fd, -1);
    for (;;) {
        const uint8_t *line = NULL;
        size_t len = 0;
        int overflow = 0;
        int rc = su_line_next(&r, &line, &len, &overflow);
        SuBuf out;
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            ret = 1;
            break;
        }
        su_buf_init(&out);
        handle_line(s, line, len, overflow, &out);
        if (su_write_all(out_fd, out.data, out.len, -1) != 0 ||
            su_write_all(out_fd, (const uint8_t *)"\n", 1, -1) != 0) {
            su_buf_free(&out);
            ret = 1;
            break;
        }
        su_buf_free(&out);
    }
    su_line_reader_free(&r);
    free_worlds(s);
    free(s);
    sigaction(SIGPIPE, &old, NULL);
    return ret;
}

int su_worker_main(int in_fd, int out_fd) {
    return su_worker_main_with_hook(in_fd, out_fd, NULL);
}
