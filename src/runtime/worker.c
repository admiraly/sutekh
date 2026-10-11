#define _POSIX_C_SOURCE 200809L

#include "runtime/worker.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "agent/su_path.h"
#include "agent/su_proto.h"
#include "ir/su_json.h"
#include "ir/su_lir.h"
#include "runtime/registry.h"
#include "runtime/reload.h"
#include "runtime/schedule.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "vm/su_vm.h"
#include "world/su_world.h"

#define WORKER_MAX_WORLDS 4u
#define WORKER_FIXTURE_MAX (8u * 1024u * 1024u)
#define WORKER_CAPSULE_MAX (64u * 1024u)
#define WORKER_TEST_MAX (256u * 1024u)
#define WORKER_INSPECT_DEFAULT_ROWS 16u
#define WORKER_INSPECT_MAX_ROWS 256u
#define WORKER_MAX_TICKS 100000u
#define WORKER_MSG_CAP 512u
#define WORKER_MAX_FAILURES 32u
#define WORKER_NAME_MAX 255u
#define WORKER_ID_CAP 16u

/* Materialized committed state of one world: fixed rows and stable entity IDs. */
typedef struct WorldInst {
    SuWorld *w;
    uint32_t *ids;
    uint64_t rows;
} WorldInst;

/* One world: its schedule (ordered verified systems) and committed state. */
typedef struct WorkerWorld {
    int used;
    char id[WORKER_ID_CAP];
    uint64_t revision;
    WorldInst inst;
    SuSchedule sched;
} WorkerWorld;

typedef struct WorkerState {
    WorkerWorld worlds[WORKER_MAX_WORLDS];
    uint64_t next_id;
    SuRegistry reg;       /* version registry: every revision this worker has seen */
    SuWorkerHooks hooks;
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

/* ---------- world instances ---------- */

static void inst_free(WorldInst *in) {
    su_world_destroy(in->w);
    free(in->ids);
    memset(in, 0, sizeof *in);
}

/* Opens a fresh world (tick 0, columns by name) for the schedule's schema and union
 * field table. The fixture must carry every field of the table. */
static uint32_t world_open(const SuSchedule *s, const char *fixture_path, WorldInst *in,
                           WorkerFail *f) {
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
    if (strcmp(str_of(root, "schema_id") != NULL ? str_of(root, "schema_id") : "", s->schema_id) != 0) {
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

    descs = malloc((s->field_count > 0 ? s->field_count : 1u) * sizeof *descs);
    if (descs == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        fail(f, st, "allocation failed");
        goto out;
    }
    for (i = 0; i < s->field_count; i++) {
        descs[i].id = s->fields[i].id;
        descs[i].type = s->fields[i].type;
        descs[i].access = s->fields[i].access;
        descs[i].name = s->fields[i].name;
    }
    memset(&desc, 0, sizeof desc);
    desc.schema_id = s->schema_id;
    desc.schema_version = s->schema_version;
    desc.layout_generation = s->layout_generation;
    desc.row_count = rows;
    desc.entity_ids = in->ids;
    desc.fields = descs;
    desc.field_count = s->field_count;
    st = su_world_create(&desc, &in->w);
    if (st != SU_OK) {
        fail(f, st, "world creation failed");
        goto out;
    }
    for (i = 0; i < s->field_count; i++) {
        const SuJsonValue *arr = su_json_member(cols, s->fields[i].name);
        st = read_u32_array(arr, rows, &vals);
        if (st != SU_OK) {
            fail(f, st, "fixture column is missing, mis-sized or not u32");
            goto out;
        }
        st = su_world_init_committed(in->w, s->fields[i].id, vals, rows);
        free(vals);
        vals = NULL;
        if (st != SU_OK) {
            fail(f, st, "fixture column initialization failed");
            goto out;
        }
    }
    st = SU_OK;
out:
    free(descs);
    free(vals);
    su_json_doc_free(&doc);
    if (st != SU_OK) {
        inst_free(in);
    }
    return st;
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
    inst_free(&ww->inst);
    su_sched_free(&ww->sched);
    memset(ww, 0, sizeof *ww);
}

static WorkerWorld *world_param(WorkerState *s, const SuJsonValue *params, WorkerFail *f, uint32_t *st) {
    WorkerWorld *ww = find_world(s, str_of(params, "world_id"));
    if (ww == NULL) {
        fail(f, SU_E_INVALID_ARGUMENT, "unknown world_id");
        *st = SU_E_INVALID_ARGUMENT;
    } else {
        *st = SU_OK;
    }
    return ww;
}

/* Writes the schedule as a JSON array in schedule order. */
static void write_manifest(SuBuf *res, const SuSchedule *s) {
    uint32_t i;
    su_buf_str(res, "[");
    for (i = 0; i < s->count; i++) {
        const SuSysEntry *e = &s->sys[i];
        if (i > 0) {
            su_buf_str(res, ",");
        }
        su_buf_str(res, "{\"system_id\":");
        su_buf_quoted(res, e->system_id, strlen(e->system_id));
        su_buf_str(res, ",\"semantic_hash\":");
        su_buf_quoted(res, e->semantic_hash, strlen(e->semantic_hash));
        su_buf_str(res, ",\"source_hash\":");
        su_buf_quoted(res, e->source_hash, strlen(e->source_hash));
        su_buf_str(res, ",\"capsule_hash\":");
        su_buf_quoted(res, e->capsule_hash, strlen(e->capsule_hash));
        su_buf_str(res, ",\"txid\":");
        su_buf_u64(res, e->txid);
        su_buf_str(res, ",\"system_revision\":");
        su_buf_u64(res, e->system_rev);
        su_buf_str(res, ",\"installed_tick\":");
        su_buf_u64(res, e->installed_tick);
        su_buf_str(res, "}");
    }
    su_buf_str(res, "]");
}

/* Parses the capsules array of world.create and builds a finalized schedule. Systems
 * are added in array order; the first failure leaves sched holding only what was added
 * (the caller frees it). */
static uint32_t load_schedule(const SuJsonValue *caps, SuSchedule *sched, WorkerFail *f) {
    size_t i;
    uint32_t st;

    if (caps == NULL || caps->type != SU_JSON_ARRAY || caps->u.array.count == 0 ||
        caps->u.array.count > SU_SCHED_MAX_SYSTEMS) {
        fail(f, SU_E_TYPE, "capsules must be an array of 1..8 {capsule_path, source_path} objects");
        return SU_E_TYPE;
    }
    for (i = 0; i < caps->u.array.count; i++) {
        const SuJsonValue *item = caps->u.array.items[i];
        const char *cap = str_of(item, "capsule_path");
        const char *src = str_of(item, "source_path");
        SuLirModule *mod = NULL;
        uint32_t sv = 0;
        uint32_t gen = 0;
        char msg[WORKER_MSG_CAP];

        if (cap == NULL || src == NULL) {
            fail(f, SU_E_TYPE, "each capsules entry needs capsule_path and source_path");
            return SU_E_TYPE;
        }
        st = su_reload_load(cap, src, NULL, NULL, &mod, &sv, &gen, msg, sizeof msg);
        if (st != SU_OK) {
            fail(f, st, msg);
            return st;
        }
        st = su_sched_add(sched, mod, sv, gen, 0, 1, 0);
        if (st != SU_OK) {
            su_reload_module_free(mod);
            fail(f, st, "system cannot join this world's schedule (schema or duplicate system_id)");
            return st;
        }
    }
    st = su_sched_finalize(sched);
    if (st != SU_OK) {
        fail(f, st, "systems disagree on a field's ID, name or type");
    }
    return st;
}

/* ---------- handlers ---------- */

static uint32_t h_world_create(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    const char *fixture = str_of(params, "fixture_path");
    const SuJsonValue *caps = su_json_member(params, "capsules");
    WorkerWorld *ww = NULL;
    WorldInst inst;
    SuSchedule sched;
    char id[WORKER_ID_CAP];
    uint32_t i;
    uint32_t st;
    uint64_t txid;

    if (fixture == NULL) {
        fail(f, SU_E_TYPE, "world.create needs fixture_path and capsules");
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
    su_sched_init(&sched);
    st = load_schedule(caps, &sched, f);
    if (st != SU_OK) {
        su_sched_free(&sched);
        return st;
    }
    st = world_open(&sched, fixture, &inst, f);
    if (st != SU_OK) {
        su_sched_free(&sched);
        return st;
    }
    snprintf(id, sizeof id, "world-%llu", (unsigned long long)(s->next_id + 1u));

    /* Each initial system is a revision: register it as INSTALLED at tick 0. */
    for (i = 0; i < sched.count; i++) {
        SuSysEntry *e = &sched.sys[i];
        txid = su_reg_begin(&s->reg, id, "creation", e->source_hash, e->capsule_hash);
        if (txid == 0) {
            inst_free(&inst);
            su_sched_free(&sched);
            fail(f, SU_E_RESOURCE_LIMIT, "version registry is full");
            return SU_E_RESOURCE_LIMIT;
        }
        su_reg_set_system(&s->reg, txid, e->system_id);
        (void)su_reg_advance(&s->reg, txid, SU_REV_SOURCE_VALIDATED);
        (void)su_reg_advance(&s->reg, txid, SU_REV_EFFECTS_VERIFIED);
        (void)su_reg_advance(&s->reg, txid, SU_REV_PREPARED);
        (void)su_reg_advance(&s->reg, txid, SU_REV_ADMITTED);
        (void)su_reg_advance(&s->reg, txid, SU_REV_PENDING_BOUNDARY);
        (void)su_reg_install(&s->reg, txid, 1, 0, e->semantic_hash);
        e->txid = txid;
    }

    s->next_id++;
    snprintf(ww->id, sizeof ww->id, "%s", id);
    ww->revision = 1;
    ww->inst = inst;
    ww->sched = sched;
    ww->used = 1;

    {
        char rev[64];
        const SuSysEntry *first = &ww->sched.sys[0];
        rev_string(ww, rev, sizeof rev);
        su_buf_str(res, "{\"world_id\":");
        su_buf_quoted(res, ww->id, strlen(ww->id));
        su_buf_str(res, ",\"world_revision\":");
        su_buf_quoted(res, rev, strlen(rev));
        su_buf_str(res, ",\"worker_pid\":");
        su_buf_u64(res, (uint64_t)getpid());
        su_buf_str(res, ",\"tick\":");
        su_buf_u64(res, su_world_tick(ww->inst.w));
        su_buf_str(res, ",\"row_count\":");
        su_buf_u64(res, ww->inst.rows);
        su_buf_str(res, ",\"schema_id\":");
        su_buf_quoted(res, ww->sched.schema_id, strlen(ww->sched.schema_id));
        su_buf_str(res, ",\"system_id\":");
        su_buf_quoted(res, first->system_id, strlen(first->system_id));
        su_buf_str(res, ",\"source_hash\":\"");
        su_buf_str(res, first->source_hash);
        su_buf_str(res, "\",\"capsule_hash\":\"");
        su_buf_str(res, first->capsule_hash);
        su_buf_str(res, "\",\"semantic_hash\":\"");
        su_buf_str(res, first->semantic_hash);
        su_buf_str(res, "\",\"semantic_manifest\":");
        write_manifest(res, &ww->sched);
        su_buf_str(res, "}");
    }
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

static uint32_t h_world_inspect(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    WorkerWorld *ww;
    uint64_t limit = WORKER_INSPECT_DEFAULT_ROWS;
    uint64_t offset = 0;
    uint64_t shown;
    const SuJsonValue *fields;
    const SuJsonValue *lim;
    const SuJsonValue *off;
    uint64_t i;
    uint32_t st = SU_OK;
    uint32_t in_use;
    uint32_t installed;
    char rev[64];

    ww = world_param(s, params, f, &st);
    if (st != SU_OK) {
        return st;
    }
    lim = su_json_member(params, "limit");
    if (lim != NULL && (!su_json_get_uint(lim, &limit) || limit < 1u || limit > WORKER_INSPECT_MAX_ROWS)) {
        fail(f, SU_E_TYPE, "limit must be an integer 1..256");
        return SU_E_TYPE;
    }
    off = su_json_member(params, "offset");
    if (off != NULL && !su_json_get_uint(off, &offset)) {
        fail(f, SU_E_TYPE, "offset must be a non-negative integer");
        return SU_E_TYPE;
    }
    if (offset > ww->inst.rows) {
        fail(f, SU_E_INVALID_ARGUMENT, "offset is beyond row_count");
        return SU_E_INVALID_ARGUMENT;
    }
    shown = ww->inst.rows - offset < limit ? ww->inst.rows - offset : limit;
    rev_string(ww, rev, sizeof rev);
    su_reg_stats(&s->reg, &in_use, &installed);

    su_buf_str(res, "{\"world_id\":");
    su_buf_quoted(res, ww->id, strlen(ww->id));
    su_buf_str(res, ",\"world_revision\":");
    su_buf_quoted(res, rev, strlen(rev));
    su_buf_str(res, ",\"worker_pid\":");
    su_buf_u64(res, (uint64_t)getpid());
    su_buf_str(res, ",\"tick\":");
    su_buf_u64(res, su_world_tick(ww->inst.w));
    su_buf_str(res, ",\"row_count\":");
    su_buf_u64(res, ww->inst.rows);
    su_buf_str(res, ",\"offset\":");
    su_buf_u64(res, offset);
    su_buf_str(res, ",\"returned_rows\":");
    su_buf_u64(res, shown);
    su_buf_str(res, ",\"entity_ids\":[");
    for (i = 0; i < shown; i++) {
        if (i > 0) {
            su_buf_str(res, ",");
        }
        su_buf_u64(res, ww->inst.ids[offset + i]);
    }
    su_buf_str(res, "],\"columns\":{");

    fields = su_json_member(params, "fields");
    if (fields != NULL && fields->type != SU_JSON_ARRAY) {
        fail(f, SU_E_TYPE, "fields must be an array of strings");
        return SU_E_TYPE;
    }
    {
        uint32_t n = 0;
        uint32_t count = fields != NULL ? (uint32_t)fields->u.array.count : ww->sched.field_count;
        uint32_t k;
        for (k = 0; k < count; k++) {
            const char *name;
            uint32_t j;
            const SuJsonValue *item = fields != NULL ? fields->u.array.items[k] : NULL;
            const uint32_t *data = NULL;
            uint64_t rc = 0;
            uint64_t r;
            int known = 0;

            name = fields != NULL ? (su_json_is_string(item) ? item->u.string.data : NULL)
                                  : ww->sched.fields[k].name;
            if (name == NULL) {
                fail(f, SU_E_TYPE, "fields must be an array of strings");
                return SU_E_TYPE;
            }
            for (j = 0; j < ww->sched.field_count; j++) {
                if (strcmp(ww->sched.fields[j].name, name) == 0) {
                    known = 1;
                }
            }
            if (!known || su_world_read_committed_by_name(ww->inst.w, name, &data, &rc) != SU_OK) {
                fail(f, SU_E_SCHEMA_MISMATCH, "unknown field name");
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
                su_buf_u64(res, data[offset + r]);
            }
            su_buf_str(res, "]");
            n++;
        }
    }
    su_buf_str(res, "},\"schema_id\":");
    su_buf_quoted(res, ww->sched.schema_id, strlen(ww->sched.schema_id));
    su_buf_str(res, ",\"semantic_manifest\":");
    write_manifest(res, &ww->sched);
    su_buf_str(res, ",\"registry\":{\"capacity\":");
    su_buf_u64(res, SU_REG_CAPACITY);
    su_buf_str(res, ",\"in_use\":");
    su_buf_u64(res, in_use);
    su_buf_str(res, ",\"installed\":");
    su_buf_u64(res, installed);
    su_buf_str(res, ",\"evictions\":");
    su_buf_u64(res, s->reg.evictions);
    su_buf_str(res, "}}");
    if (res->failed) {
        fail(f, SU_E_RESOURCE_LIMIT, "inspect result exceeds the message limit");
        return SU_E_RESOURCE_LIMIT;
    }
    return SU_OK;
}

static int fault_thunk(unsigned idx, void *ctx) {
    WorkerState *s = ctx;
    return s->hooks.fail_system != NULL && s->hooks.fail_system(idx) != 0;
}

static uint32_t h_world_step(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    WorkerWorld *ww;
    const char *expect = str_of(params, "expected_world_revision");
    uint64_t ticks = 0;
    uint64_t done = 0;
    uint32_t failed_index = UINT32_MAX;
    char rev[64];
    uint32_t st = SU_OK;

    ww = world_param(s, params, f, &st);
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
    st = su_sched_run_ticks(ww->inst.w, &ww->sched, ticks, s->hooks.fail_system != NULL ? fault_thunk : NULL,
                            s, &done, &failed_index);
    ww->revision += done;
    if (st != SU_OK) {
        char more[WORKER_MSG_CAP];
        rev_string(ww, rev, sizeof rev);
        if (failed_index != UINT32_MAX) {
            snprintf(more, sizeof more,
                     "system %u (%s) failed; tick discarded, committed state unchanged; completed %llu of %llu ticks, world_revision now %s",
                     (unsigned)failed_index, ww->sched.sys[failed_index].system_id, (unsigned long long)done,
                     (unsigned long long)ticks, rev);
        } else {
            snprintf(more, sizeof more, "tick failed; completed %llu of %llu ticks, world_revision now %s",
                     (unsigned long long)done, (unsigned long long)ticks, rev);
        }
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
    su_buf_str(res, ",\"worker_pid\":");
    su_buf_u64(res, (uint64_t)getpid());
    su_buf_str(res, "}");
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* Installs a candidate into a live world (see reload.h for the steps). */
static uint32_t h_candidate_install(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    WorkerWorld *ww;
    SuReloadRequest req;
    SuReloadResult out;
    char rev[64];
    const char *cand = str_of(params, "candidate_id");
    const char *capsule = str_of(params, "capsule_path");
    const char *source = str_of(params, "source_path");
    const char *exp_src = str_of(params, "expected_source_hash");
    const char *exp_cap = str_of(params, "expected_capsule_hash");
    const char *evidence = str_of(params, "evidence");
    const char *expect = str_of(params, "expected_world_revision");
    uint64_t allowed = 0;
    uint32_t st = SU_OK;

    ww = world_param(s, params, f, &st);
    if (st != SU_OK) {
        return st;
    }
    if (cand == NULL || capsule == NULL || source == NULL || exp_src == NULL || exp_cap == NULL ||
        evidence == NULL || expect == NULL || !su_json_get_uint(su_json_member(params, "allowed_effective_tick"), &allowed)) {
        fail(f, SU_E_TYPE, "candidate.install needs candidate, paths, hashes, evidence, expected_world_revision and allowed_effective_tick");
        return SU_E_TYPE;
    }
    rev_string(ww, rev, sizeof rev);
    memset(&req, 0, sizeof req);
    req.world_id = ww->id;
    req.candidate_id = cand;
    req.capsule_path = capsule;
    req.source_path = source;
    req.expect_source_hash = exp_src;
    req.expect_capsule_hash = exp_cap;
    req.evidence = evidence;
    req.expected_revision = expect;
    req.current_revision = rev;
    req.current_tick = su_world_tick(ww->inst.w);
    req.allowed_effective_tick = allowed;

    st = su_reload_install(&s->reg, &ww->sched, &req, &out);
    if (st != SU_OK) {
        fail(f, st, out.msg);
        return st;
    }
    ww->revision++;
    rev_string(ww, rev, sizeof rev);
    su_buf_str(res, "{\"world_id\":");
    su_buf_quoted(res, ww->id, strlen(ww->id));
    su_buf_str(res, ",\"candidate_id\":");
    su_buf_quoted(res, cand, strlen(cand));
    su_buf_str(res, ",\"txid\":");
    su_buf_u64(res, out.txid);
    su_buf_str(res, ",\"state\":\"installed\",\"system_id\":");
    su_buf_quoted(res, out.system_id, strlen(out.system_id));
    su_buf_str(res, ",\"system_revision\":");
    su_buf_u64(res, out.system_rev);
    su_buf_str(res, ",\"effective_tick\":");
    su_buf_u64(res, out.effective_tick);
    su_buf_str(res, ",\"previous_txid\":");
    su_buf_u64(res, out.previous_txid);
    su_buf_str(res, ",\"previous_semantic_hash\":");
    su_buf_quoted(res, out.previous_semantic_hash, strlen(out.previous_semantic_hash));
    su_buf_str(res, ",\"semantic_hash\":");
    su_buf_quoted(res, out.semantic_hash, strlen(out.semantic_hash));
    su_buf_str(res, ",\"source_hash\":");
    su_buf_quoted(res, out.source_hash, strlen(out.source_hash));
    su_buf_str(res, ",\"capsule_hash\":");
    su_buf_quoted(res, out.capsule_hash, strlen(out.capsule_hash));
    su_buf_str(res, ",\"world_revision\":");
    su_buf_quoted(res, rev, strlen(rev));
    su_buf_str(res, ",\"tick\":");
    su_buf_u64(res, su_world_tick(ww->inst.w));
    su_buf_str(res, ",\"worker_pid\":");
    su_buf_u64(res, (uint64_t)getpid());
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
static int case_columns_match(const SuSchedule *s, const WorldInst *in, const SuJsonValue *expected,
                              char *reason, size_t reason_cap) {
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
        for (j = 0; j < s->field_count; j++) {
            if (strcmp(s->fields[j].name, name) == 0) {
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
                snprintf(reason, reason_cap, "column_mismatch:%s[%llu]", name, (unsigned long long)r);
                return 0;
            }
        }
    }
    return 1;
}

static uint32_t h_candidate_evaluate(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    const char *capsule = str_of(params, "capsule_path");
    const char *source = str_of(params, "source_path");
    const char *test_path = str_of(params, "test_path");
    const char *world_path = str_of(params, "world_path");
    const char *exp_src = str_of(params, "expected_source_hash");
    const char *exp_cap = str_of(params, "expected_capsule_hash");
    SuSchedule sched;
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
    SuLirModule *mod = NULL;
    uint32_t sv = 0;
    uint32_t gen = 0;
    char msg[WORKER_MSG_CAP];

    if (s->hooks.before_eval != NULL) {
        s->hooks.before_eval();
    }
    if (capsule == NULL || source == NULL || test_path == NULL || world_path == NULL ||
        exp_src == NULL || exp_cap == NULL) {
        fail(f, SU_E_TYPE, "candidate evaluation parameters are incomplete");
        return SU_E_TYPE;
    }
    memset(&cf, 0, sizeof cf);
    su_sched_init(&sched);
    st = su_reload_load(capsule, source, exp_src, exp_cap, &mod, &sv, &gen, msg, sizeof msg);
    if (st != SU_OK) {
        fail(f, st, msg);
        return st;
    }
    st = su_sched_add(&sched, mod, sv, gen, 0, 1, 0);
    if (st != SU_OK) {
        su_reload_module_free(mod);
        fail(f, st, "candidate cannot be scheduled");
        return st;
    }
    st = su_sched_finalize(&sched);
    if (st != SU_OK) {
        su_sched_free(&sched);
        fail(f, st, "candidate field table is inconsistent");
        return st;
    }
    st = su_path_read_file(test_path, WORKER_TEST_MAX, &tdata, &tlen);
    if (st != SU_OK) {
        su_sched_free(&sched);
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
        su_sched_free(&sched);
        fail(f, st, jerr.message);
        return st;
    }
    cases = doc.root != NULL && doc.root->type == SU_JSON_OBJECT ? su_json_member(doc.root, "cases") : NULL;
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
        WorldInst in;
        uint64_t done = 0;
        uint32_t failed_index = UINT32_MAX;
        char reason[WORKER_MSG_CAP];

        if (name == NULL || ticks_v == NULL || !su_json_get_uint(ticks_v, &ticks) ||
            ticks > WORKER_MAX_TICKS || exp == NULL) {
            st = SU_E_TYPE;
            fail(f, st, "each case needs name, ticks (<= 100000) and expected_columns");
            goto out;
        }
        total++;
        st = world_open(&sched, world_path, &in, f);
        if (st != SU_OK) {
            goto out;
        }
        st = su_sched_run_ticks(in.w, &sched, ticks, s->hooks.fail_system != NULL ? fault_thunk : NULL, s,
                                &done, &failed_index);
        if (st != SU_OK) {
            char r2[WORKER_MSG_CAP];
            snprintf(r2, sizeof r2, "tick_failed:%s", su_status_name(st));
            note_failure(&cf, name, r2);
        } else if (!case_columns_match(&sched, &in, exp, reason, sizeof reason)) {
            note_failure(&cf, name, reason);
        } else {
            passed++;
        }
        inst_free(&in);
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
    su_sched_free(&sched);
    return st;
}

/* ---------- dispatch and main loop ---------- */

static void handle_line(WorkerState *s, const uint8_t *line, size_t len, int overflow, SuBuf *out) {
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
    } else if (strcmp(req.method, "internal.candidate.install") == 0) {
        st = h_candidate_install(s, req.params, &res, &f);
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
        su_proto_error(out, req.has_id, req.id, SU_E_RESOURCE_LIMIT, "response exceeds the 1 MiB message limit");
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

int su_worker_main_with_hooks(int in_fd, int out_fd, const SuWorkerHooks *hooks) {
    WorkerState *s;
    SuLineReader r;
    struct sigaction ign;
    struct sigaction old;
    int ret = 0;

    s = calloc(1, sizeof *s);
    if (s == NULL) {
        return 1;
    }
    su_reg_init(&s->reg);
    if (hooks != NULL) {
        s->hooks = *hooks;
    }
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

int su_worker_main_with_hook(int in_fd, int out_fd, SuWorkerEvalHook before_eval) {
    SuWorkerHooks h;
    memset(&h, 0, sizeof h);
    h.before_eval = before_eval;
    return su_worker_main_with_hooks(in_fd, out_fd, &h);
}

int su_worker_main(int in_fd, int out_fd) {
    return su_worker_main_with_hooks(in_fd, out_fd, NULL);
}
