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
#include "runtime/checkpoint.h"
#include "runtime/registry.h"
#include "runtime/reload.h"
#include "runtime/schedule.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "vm/su_vm.h"
#include "world/snapshot.h"
#include "world/su_world.h"

#define WORKER_MAX_WORLDS 4u
/* Default and hard ceiling for world fixture JSON read by the worker. The default can be changed with
 * the SUTEKH_FIXTURE_MAX_BYTES environment variable (1 .. hard ceiling); invalid values are ignored. */
#define WORKER_FIXTURE_DEFAULT_MAX (64u * 1024u * 1024u)
#define WORKER_FIXTURE_HARD_MAX (256u * 1024u * 1024u)

static size_t worker_fixture_max(void) {
    const char *env = getenv("SUTEKH_FIXTURE_MAX_BYTES");
    if (env != NULL && env[0] >= '0' && env[0] <= '9') {
        char *end = NULL;
        unsigned long long v = strtoull(env, &end, 10);
        if (end != NULL && *end == '\0' && v >= 1u && v <= WORKER_FIXTURE_HARD_MAX) {
            return (size_t)v;
        }
    }
    return WORKER_FIXTURE_DEFAULT_MAX;
}
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

/* Supervisor-resolved paths of the revision installed in one schedule slot. Restore
 * reloads a checkpointed revision from these paths and verifies its hashes again. */
typedef struct RevPaths {
    char capsule[SU_PATH_BUF];
    char source[SU_PATH_BUF];
} RevPaths;

/* One world: its schedule (ordered verified systems) and committed state. paths[i]
 * belongs to sched.sys[i]. */
typedef struct WorkerWorld {
    int used;
    char id[WORKER_ID_CAP];
    uint64_t revision;
    WorldInst inst;
    SuSchedule sched;
    RevPaths paths[SU_SCHED_MAX_SYSTEMS];
} WorkerWorld;

/* What a checkpoint needs beyond the snapshot bytes: the world it belongs to and the
 * installed revision of each system at capture (M0-08). Records are kept for the same
 * ids as the checkpoint store; a record whose id the store no longer holds is inert. */
typedef struct CkptSys {
    char system_id[SU_DIAG_SYSTEM_BYTES];
    char semantic_hash[65];
    char source_hash[65];
    char capsule_hash[65];
    RevPaths paths;
} CkptSys;

typedef struct CkptRec {
    int used;
    uint64_t id;                 /* checkpoint store id */
    char world_id[WORKER_ID_CAP];
    char snapshot_id[65];
    uint32_t count;
    CkptSys sys[SU_SCHED_MAX_SYSTEMS];
} CkptRec;

typedef struct WorkerState {
    WorkerWorld worlds[WORKER_MAX_WORLDS];
    uint64_t next_id;
    SuRegistry reg;       /* version registry: every revision this worker has seen */
    SuWorkerHooks hooks;
    SuCheckpointStore *store; /* bounded snapshot store; retain SU_CHECKPOINT_DEFAULT_RETAIN */
    CkptRec ckpts[SU_CHECKPOINT_DEFAULT_RETAIN];
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
    st = su_path_read_file(fixture_path, worker_fixture_max(), &data, &len);
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
static uint32_t load_schedule(const SuJsonValue *caps, SuSchedule *sched, RevPaths *paths,
                              WorkerFail *f) {
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
        /* su_sched_add appended at index i, so paths[i] describes sched->sys[i]. */
        snprintf(paths[i].capsule, sizeof paths[i].capsule, "%s", cap);
        snprintf(paths[i].source, sizeof paths[i].source, "%s", src);
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
    memset(ww->paths, 0, sizeof ww->paths);
    st = load_schedule(caps, &sched, ww->paths, f);
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
    {
        /* The installed slot now runs this candidate's files (checkpoint restore needs them). */
        int j = su_sched_find(&ww->sched, out.system_id);
        if (j >= 0) {
            snprintf(ww->paths[j].capsule, sizeof ww->paths[j].capsule, "%s", capsule);
            snprintf(ww->paths[j].source, sizeof ww->paths[j].source, "%s", source);
        }
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

/* ---------- checkpoints: world.snapshot / world.restore (M0-08) ---------- */

/* Builds a su_world descriptor for the schedule's schema over the given rows. The
 * field table is borrowed from the schedule; *descs (malloc) must be freed by the caller. */
static uint32_t sched_desc(const SuSchedule *s, const uint32_t *ids, uint64_t rows, SuWorldDesc *desc,
                           SuWorldFieldDesc **descs) {
    SuWorldFieldDesc *d;
    uint32_t i;

    *descs = NULL;
    d = malloc((s->field_count > 0 ? s->field_count : 1u) * sizeof *d);
    if (d == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0; i < s->field_count; i++) {
        d[i].id = s->fields[i].id;
        d[i].type = s->fields[i].type;
        d[i].access = s->fields[i].access;
        d[i].name = s->fields[i].name;
    }
    memset(desc, 0, sizeof *desc);
    desc->schema_id = s->schema_id;
    desc->schema_version = s->schema_version;
    desc->layout_generation = s->layout_generation;
    desc->row_count = rows;
    desc->entity_ids = ids;
    desc->fields = d;
    desc->field_count = s->field_count;
    *descs = d;
    return SU_OK;
}

static void sched_manifest(const SuSchedule *s, SuSnapshotManifestEntry *m) {
    uint32_t i;
    for (i = 0; i < s->count; i++) {
        m[i].system_id = s->sys[i].system_id;
        snprintf(m[i].semantic_hash, sizeof m[i].semantic_hash, "%s", s->sys[i].semantic_hash);
    }
}

/* The newest record for this world and snapshot id (identical states captured twice
 * share an id; the newest is the one whose revisions are most recent). */
static CkptRec *ckpt_find(WorkerState *s, const char *world_id, const char *snapshot_id) {
    CkptRec *best = NULL;
    uint32_t i;
    for (i = 0; i < SU_CHECKPOINT_DEFAULT_RETAIN; i++) {
        CkptRec *r = &s->ckpts[i];
        if (r->used && strcmp(r->world_id, world_id) == 0 && strcmp(r->snapshot_id, snapshot_id) == 0 &&
            (best == NULL || r->id > best->id)) {
            best = r;
        }
    }
    return best;
}

/* Stores the record of checkpoint id. The table has as many slots as the store has
 * retained entries, and ids grow monotonically, so the slot with the smallest id is the
 * one the store has evicted (or is about to) when no slot is free. */
static void ckpt_record(WorkerState *s, const WorkerWorld *ww, uint64_t id, const char *snapshot_id) {
    CkptRec *slot = NULL;
    uint32_t i;

    for (i = 0; i < SU_CHECKPOINT_DEFAULT_RETAIN; i++) {
        if (!s->ckpts[i].used) {
            slot = &s->ckpts[i];
            break;
        }
    }
    if (slot == NULL) {
        slot = &s->ckpts[0];
        for (i = 1; i < SU_CHECKPOINT_DEFAULT_RETAIN; i++) {
            if (s->ckpts[i].id < slot->id) {
                slot = &s->ckpts[i];
            }
        }
    }
    memset(slot, 0, sizeof *slot);
    slot->used = 1;
    slot->id = id;
    snprintf(slot->world_id, sizeof slot->world_id, "%s", ww->id);
    snprintf(slot->snapshot_id, sizeof slot->snapshot_id, "%s", snapshot_id);
    slot->count = ww->sched.count;
    for (i = 0; i < ww->sched.count; i++) {
        CkptSys *c = &slot->sys[i];
        snprintf(c->system_id, sizeof c->system_id, "%s", ww->sched.sys[i].system_id);
        snprintf(c->semantic_hash, sizeof c->semantic_hash, "%s", ww->sched.sys[i].semantic_hash);
        snprintf(c->source_hash, sizeof c->source_hash, "%s", ww->sched.sys[i].source_hash);
        snprintf(c->capsule_hash, sizeof c->capsule_hash, "%s", ww->sched.sys[i].capsule_hash);
        c->paths = ww->paths[i];
    }
}

/* world.snapshot {world_id, expected_world_revision}. Captures the committed root without
 * changing the world revision. */
static uint32_t h_world_snapshot(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    WorkerWorld *ww;
    const char *expect = str_of(params, "expected_world_revision");
    SuSnapshotManifestEntry man[SU_SCHED_MAX_SYSTEMS];
    SuWorldDesc desc;
    SuWorldFieldDesc *descs = NULL;
    SuCheckpointInfo info;
    char rev[64];
    uint64_t id = 0;
    uint32_t st = SU_OK;

    ww = world_param(s, params, f, &st);
    if (st != SU_OK) {
        return st;
    }
    if (expect == NULL) {
        fail(f, SU_E_TYPE, "world.snapshot needs expected_world_revision");
        return SU_E_TYPE;
    }
    rev_string(ww, rev, sizeof rev);
    if (strcmp(expect, rev) != 0) {
        fail(f, SU_E_STALE_REVISION, "expected_world_revision does not match the active world");
        return SU_E_STALE_REVISION;
    }
    sched_manifest(&ww->sched, man);
    st = sched_desc(&ww->sched, ww->inst.ids, ww->inst.rows, &desc, &descs);
    if (st != SU_OK) {
        fail(f, st, "allocation failed");
        return st;
    }
    st = su_checkpoint_create(s->store, ww->inst.w, &desc, rev, man, ww->sched.count, &id);
    free(descs);
    if (st != SU_OK) {
        fail(f, st, "snapshot could not be captured; nothing was stored");
        return st;
    }
    if (su_checkpoint_info(s->store, id, &info) != SU_OK) {
        fail(f, SU_E_INTERNAL, "new checkpoint is not in the store");
        return SU_E_INTERNAL;
    }
    ckpt_record(s, ww, id, info.snapshot_id);

    su_buf_str(res, "{\"snapshot_id\":");
    su_buf_quoted(res, info.snapshot_id, strlen(info.snapshot_id));
    su_buf_str(res, ",\"world_revision\":");
    su_buf_quoted(res, rev, strlen(rev));
    su_buf_str(res, ",\"tick\":");
    su_buf_u64(res, info.tick);
    su_buf_str(res, ",\"bytes\":");
    su_buf_u64(res, info.bytes);
    su_buf_str(res, ",\"sha256\":");
    su_buf_quoted(res, info.snapshot_id, strlen(info.snapshot_id));
    su_buf_str(res, ",\"capture_ns\":");
    if (info.capture_ns_known) {
        su_buf_u64(res, info.capture_ns);
    } else {
        su_buf_str(res, "null");
    }
    su_buf_str(res, "}");
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* Rebuilds the snapshot's schedule from the recorded revisions. Every file is reloaded
 * with the hashes recorded at capture, so changed bytes are refused. On SU_OK *nsched
 * owns the new entries (and must be freed by the caller). */
static uint32_t rebuild_schedule(const CkptRec *rec, const SuSnapshotData *data, SuSchedule *nsched,
                                 WorkerFail *f) {
    uint32_t i;
    uint32_t st;
    char why[WORKER_MSG_CAP];

    su_sched_init(nsched);
    if (data->manifest_count != rec->count) {
        fail(f, SU_E_SCHEMA_MISMATCH, "snapshot manifest does not list this world's systems");
        return SU_E_SCHEMA_MISMATCH;
    }
    for (i = 0; i < rec->count; i++) {
        const CkptSys *c = &rec->sys[i];
        SuLirModule *mod = NULL;
        uint32_t sv = 0;
        uint32_t gen = 0;

        if (strcmp(data->manifest[i].system_id, c->system_id) != 0 ||
            strcmp(data->manifest[i].semantic_hash, c->semantic_hash) != 0) {
            fail(f, SU_E_SCHEMA_MISMATCH, "snapshot manifest does not match the recorded revisions");
            return SU_E_SCHEMA_MISMATCH;
        }
        st = su_reload_load(c->paths.capsule, c->paths.source, c->source_hash, c->capsule_hash, &mod,
                            &sv, &gen, why, sizeof why);
        if (st != SU_OK) {
            snprintf(f->msg, sizeof f->msg, "snapshot revision cannot be reloaded: %.300s", why);
            f->code = SU_E_SCHEMA_MISMATCH;
            return SU_E_SCHEMA_MISMATCH;
        }
        st = su_sched_add(nsched, mod, sv, gen, 0, 1, 0);
        if (st != SU_OK) {
            su_reload_module_free(mod);
            fail(f, SU_E_SCHEMA_MISMATCH, "snapshot revision cannot join this world's schedule");
            return SU_E_SCHEMA_MISMATCH;
        }
    }
    if (su_sched_finalize(nsched) != SU_OK) {
        fail(f, SU_E_SCHEMA_MISMATCH, "snapshot revisions disagree on a field's ID, name or type");
        return SU_E_SCHEMA_MISMATCH;
    }
    return SU_OK;
}

/* world.restore {world_id, snapshot_id, expected_world_revision}.
 *
 * Order: stale revision (before any work); lookup of the snapshot for this world (unknown
 * or evicted -> SU_E_INVALID_ARGUMENT); strict parse against the current schema (M0
 * snapshot rules); rebuild of the schedule from the checkpointed revisions with hash
 * checks (SU_E_SCHEMA_MISMATCH); field-table check; a NEW world built from the snapshot
 * (su_snapshot_restore); registry transactions for changed revisions; then the instance
 * swap, which cannot fail. Any failure before the swap leaves the world, its schedule and
 * the registry's installed records unchanged (begun transactions are finished REJECTED). */
static uint32_t h_world_restore(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    WorkerWorld *ww;
    const char *snap_id = str_of(params, "snapshot_id");
    const char *expect = str_of(params, "expected_world_revision");
    CkptRec *rec;
    const SuSnapshot *snap = NULL;
    SuWorldDesc desc;
    SuWorldFieldDesc *descs = NULL;
    SuWorldDesc desc2;
    SuWorldFieldDesc *descs2 = NULL;
    SuSnapshotData data;
    SuSnapshotError perr;
    SuSchedule nsched;
    SuWorld *neww = NULL;
    uint64_t txids[SU_SCHED_MAX_SYSTEMS];
    uint32_t nbegun = 0;
    int parsed = 0;
    char rev[64];
    uint32_t i;
    uint32_t st = SU_OK;

    ww = world_param(s, params, f, &st);
    if (st != SU_OK) {
        return st;
    }
    if (snap_id == NULL || expect == NULL) {
        fail(f, SU_E_TYPE, "world.restore needs snapshot_id and expected_world_revision");
        return SU_E_TYPE;
    }
    rev_string(ww, rev, sizeof rev);
    if (strcmp(expect, rev) != 0) {
        fail(f, SU_E_STALE_REVISION, "expected_world_revision does not match the active world");
        return SU_E_STALE_REVISION;
    }
    rec = ckpt_find(s, ww->id, snap_id);
    if (rec == NULL || su_checkpoint_get(s->store, rec->id, &snap) != SU_OK) {
        fail(f, SU_E_INVALID_ARGUMENT, "unknown or evicted snapshot_id for this world");
        return SU_E_INVALID_ARGUMENT;
    }

    memset(&data, 0, sizeof data);
    memset(&nsched, 0, sizeof nsched);
    st = sched_desc(&ww->sched, ww->inst.ids, ww->inst.rows, &desc, &descs);
    if (st != SU_OK) {
        fail(f, st, "allocation failed");
        goto out;
    }
    st = su_snapshot_parse(snap->bytes, snap->len, &desc, &data, &perr);
    if (st != SU_OK) {
        fail(f, st, perr.message);
        goto out;
    }
    parsed = 1;
    st = rebuild_schedule(rec, &data, &nsched, f);
    if (st != SU_OK) {
        goto out;
    }
    /* The restored schedule must expose the same field table the snapshot was taken with. */
    if (nsched.field_count != ww->sched.field_count) {
        st = SU_E_SCHEMA_MISMATCH;
        fail(f, st, "restored revisions bind a different field table");
        goto out;
    }
    for (i = 0; i < nsched.field_count; i++) {
        if (nsched.fields[i].id != ww->sched.fields[i].id || nsched.fields[i].type != ww->sched.fields[i].type ||
            strcmp(nsched.fields[i].name, ww->sched.fields[i].name) != 0) {
            st = SU_E_SCHEMA_MISMATCH;
            fail(f, st, "restored revisions bind a different field table");
            goto out;
        }
    }
    for (i = 0; i < nsched.count; i++) {
        if (strcmp(nsched.sys[i].semantic_hash, data.manifest[i].semantic_hash) != 0) {
            st = SU_E_SCHEMA_MISMATCH;
            fail(f, st, "restored semantic hash differs from the snapshot manifest");
            goto out;
        }
    }

    st = sched_desc(&nsched, ww->inst.ids, ww->inst.rows, &desc2, &descs2);
    if (st != SU_OK) {
        fail(f, st, "allocation failed");
        goto out;
    }
    st = su_snapshot_restore(&desc2, &data, &neww, NULL, NULL);
    if (st != SU_OK) {
        fail(f, st, "snapshot does not restore into this world; world unchanged");
        goto out;
    }

    /* Registry: a system whose semantic hash changed gets a new INSTALLED revision at the
     * snapshot tick; unchanged systems keep their transaction. */
    for (i = 0; i < nsched.count; i++) {
        SuSysEntry *n = &nsched.sys[i];
        SuSysEntry *c = &ww->sched.sys[i];
        uint64_t txid;
        if (strcmp(n->semantic_hash, c->semantic_hash) == 0) {
            n->txid = c->txid;
            n->system_rev = c->system_rev;
            n->installed_tick = c->installed_tick;
            continue;
        }
        txid = su_reg_begin(&s->reg, ww->id, "restore", n->source_hash, n->capsule_hash);
        if (txid == 0) {
            uint32_t k;
            for (k = 0; k < nbegun; k++) {
                su_reg_finish(&s->reg, txids[k], SU_REV_REJECTED, SU_E_RESOURCE_LIMIT, "restore aborted");
            }
            st = SU_E_RESOURCE_LIMIT;
            fail(f, st, "version registry is full");
            goto out;
        }
        txids[nbegun++] = txid;
        su_reg_set_system(&s->reg, txid, n->system_id);
        (void)su_reg_advance(&s->reg, txid, SU_REV_SOURCE_VALIDATED);
        (void)su_reg_advance(&s->reg, txid, SU_REV_EFFECTS_VERIFIED);
        (void)su_reg_advance(&s->reg, txid, SU_REV_PREPARED);
        (void)su_reg_advance(&s->reg, txid, SU_REV_ADMITTED);
        (void)su_reg_advance(&s->reg, txid, SU_REV_PENDING_BOUNDARY);
        n->txid = txid;
    }
    for (i = 0; i < nsched.count; i++) {
        SuSysEntry *n = &nsched.sys[i];
        SuSysEntry *c = &ww->sched.sys[i];
        if (strcmp(n->semantic_hash, c->semantic_hash) == 0) {
            continue;
        }
        (void)su_reg_install(&s->reg, n->txid, c->system_rev + 1, su_world_tick(neww), n->semantic_hash);
        if (c->txid != 0) {
            (void)su_reg_retire(&s->reg, c->txid);
        }
        n->system_rev = c->system_rev + 1;
        n->installed_tick = su_world_tick(neww);
    }

    /* Swap. Nothing below can fail. The old world and schedule are destroyed only after the
     * new ones are in place; no pointer into them is kept (su_sched_run_ticks takes the world
     * per call, and contexts borrow modules owned by the schedule being replaced). */
    {
        SuWorld *old = ww->inst.w;
        ww->inst.w = neww;
        neww = NULL;
        su_world_destroy(old);
    }
    su_sched_free(&ww->sched);
    ww->sched = nsched;
    memset(&nsched, 0, sizeof nsched);
    for (i = 0; i < ww->sched.count; i++) {
        ww->paths[i] = rec->sys[i].paths;
    }
    ww->revision++;
    st = SU_OK;

    rev_string(ww, rev, sizeof rev);
    su_buf_str(res, "{\"world_id\":");
    su_buf_quoted(res, ww->id, strlen(ww->id));
    su_buf_str(res, ",\"world_revision\":");
    su_buf_quoted(res, rev, strlen(rev));
    su_buf_str(res, ",\"tick\":");
    su_buf_u64(res, su_world_tick(ww->inst.w));
    su_buf_str(res, ",\"restored_from\":");
    su_buf_quoted(res, rec->snapshot_id, strlen(rec->snapshot_id));
    su_buf_str(res, ",\"worker_pid\":");
    su_buf_u64(res, (uint64_t)getpid());
    su_buf_str(res, ",\"semantic_manifest\":");
    write_manifest(res, &ww->sched);
    su_buf_str(res, "}");

out:
    if (neww != NULL) {
        su_world_destroy(neww);
    }
    su_sched_free(&nsched);
    if (parsed) {
        su_snapshot_data_free(&data);
    }
    free(descs2);
    free(descs);
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
    } else if (strcmp(req.method, "world.snapshot") == 0) {
        st = h_world_snapshot(s, req.params, &res, &f);
    } else if (strcmp(req.method, "world.restore") == 0) {
        st = h_world_restore(s, req.params, &res, &f);
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
    if (su_checkpoint_store_create(0, &s->store) != SU_OK) {
        free(s);
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
    su_checkpoint_store_destroy(s->store);
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
