#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE /* syscall(2) for the test-only io_uring probe */

#include "runtime/worker.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "agent/su_path.h"
#include "agent/su_proto.h"
#include "agent/su_sandbox.h"
#include "ir/su_json.h"
#include "ir/su_lir.h"
#include "ir/su_sha256.h"
#include "runtime/checkpoint.h"
#include "runtime/registry.h"
#include "runtime/reload.h"
#include "runtime/jobs/su_effects.h"
#include "runtime/jobs/su_jobs.h"
#include "runtime/schedule.h"
#ifdef SU_HAVE_NATIVE
#include "backends/native/su_native.h"
#include "runtime/native_versions.h"
#endif
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
    SuSandboxState sandbox; /* worker self-sandbox (execve, fork and clone denial), set at startup */
    SuWorkerHooks hooks;
    SuCheckpointStore *store; /* bounded snapshot store; retain SU_CHECKPOINT_DEFAULT_RETAIN */
    CkptRec ckpts[SU_CHECKPOINT_DEFAULT_RETAIN];
    WorkerWorld *cur_ww;  /* world of the step in progress (fault seam only) */
#ifdef SU_TEST_FAULT_HOOKS
    /* Test-only crash seam, compiled only into test workers. See test_kill_check. */
    int test_kill_set;
    uint64_t test_kill_tick;
    int test_kill_abort;
    char test_kill_src[65];   /* when set, only a world with a system of this source hash dies */
    char test_kill_token[SU_PATH_BUF]; /* when set, the seam fires once: the file is unlinked first */
#endif
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
static uint32_t world_open_ex(const SuSchedule *s, const char *fixture_path, const char *fixture_sha,
                              uint64_t max_entities, WorldInst *in, WorkerFail *f);

static uint32_t world_open(const SuSchedule *s, const char *fixture_path, const char *fixture_sha,
                           WorldInst *in, WorkerFail *f) {
    return world_open_ex(s, fixture_path, fixture_sha, 0u, in, f);
}

static uint32_t world_open_ex(const SuSchedule *s, const char *fixture_path, const char *fixture_sha,
                              uint64_t max_entities, WorldInst *in, WorkerFail *f) {
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
    if (fixture_sha != NULL) {
        SuSha256 ctx;
        uint8_t dig[32];
        char hex[65];
        su_sha256_init(&ctx);
        su_sha256_update(&ctx, data, len);
        su_sha256_final(&ctx, dig);
        su_sha256_hex(dig, hex);
        if (strcmp(hex, fixture_sha) != 0) {
            free(data);
            st = SU_E_SCHEMA_MISMATCH;
            fail(f, st, "world fixture bytes do not match the recorded sha256");
            return st;
        }
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
    st = su_world_create_ex(&desc, max_entities, &in->w);
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
#ifdef SU_HAVE_NATIVE
        /* Execution tier of the system (M1-02b). Present only in native builds. */
        su_buf_str(res, ",\"execution_tier\":");
        if (e->native_seq == 0u) {
            su_buf_quoted(res, "vm", 2u);
        } else {
            uint32_t bid = 0;
            const char *tier = "native_scalar";
            if (su_nv_backend_id(e->native_seq, &bid) == SU_OK && bid == SU_NATIVE_BACKEND_ASMJIT_AVX2) {
                tier = "native_avx2";
            }
            su_buf_quoted(res, tier, strlen(tier));
        }
#endif
        su_buf_str(res, "}");
    }
    su_buf_str(res, "]");
}

/* ---------- schedule facts and effect check (M1 closure B) ---------- */

/* Reads a capsule file's schedule block (phase, after, before). The strings point into doc,
 * which the caller frees with su_json_doc_free. A capsule without a schedule block has phase ""
 * and no edges. buf is malloc'd and must be freed after doc. */
static uint32_t read_schedule_facts(const char *cap, SuJsonDoc *doc, uint8_t **buf, const char **phase,
                                    const SuJsonValue **after, const SuJsonValue **before, WorkerFail *f) {
    static const SuJsonLimits lim = {16u * 1024u * 1024u, 32u};
    FILE *fh = fopen(cap, "rb");
    long sz = -1;
    size_t got = 0;
    SuJsonError err;
    const SuJsonValue *sch;
    const SuJsonValue *ph;

    *buf = NULL;
    *phase = "";
    *after = NULL;
    *before = NULL;
    memset(doc, 0, sizeof *doc);
    if (fh == NULL) {
        fail(f, SU_E_IO, "capsule cannot be read for its schedule block");
        return SU_E_IO;
    }
    if (fseek(fh, 0, SEEK_END) == 0) {
        sz = ftell(fh);
    }
    if (sz < 0 || (size_t)sz > lim.max_bytes) {
        fclose(fh);
        fail(f, SU_E_IO, "capsule cannot be read for its schedule block");
        return SU_E_IO;
    }
    rewind(fh);
    *buf = malloc((size_t)sz + 1u);
    if (*buf == NULL) {
        fclose(fh);
        fail(f, SU_E_RESOURCE_LIMIT, "out of memory reading a capsule");
        return SU_E_RESOURCE_LIMIT;
    }
    got = fread(*buf, 1, (size_t)sz, fh);
    fclose(fh);
    if (got != (size_t)sz || su_json_parse(*buf, got, &lim, doc, &err) != SU_OK) {
        free(*buf);
        *buf = NULL;
        fail(f, SU_E_PARSE, "capsule is not valid JSON");
        return SU_E_PARSE;
    }
    sch = su_json_member(doc->root, "schedule");
    if (sch == NULL) {
        return SU_OK;
    }
    if (sch->type != SU_JSON_OBJECT) {
        fail(f, SU_E_TYPE, "capsule schedule must be an object");
        return SU_E_TYPE;
    }
    ph = su_json_member(sch, "phase");
    if (ph != NULL && ph->type == SU_JSON_STRING) {
        *phase = ph->u.string.data;
    }
    *after = su_json_member(sch, "after");
    *before = su_json_member(sch, "before");
    return SU_OK;
}

/* Collects string items of a JSON array into out (at most cap), returns the count. */
static uint32_t string_items(const SuJsonValue *arr, const char **out, uint32_t cap) {
    uint32_t k;
    uint32_t n = 0;
    if (arr == NULL || arr->type != SU_JSON_ARRAY) {
        return 0;
    }
    for (k = 0; k < arr->u.array.count && n < cap; k++) {
        const SuJsonValue *e = arr->u.array.items[k];
        if (e->type == SU_JSON_STRING) {
            out[n++] = e->u.string.data;
        }
    }
    return n;
}

/* Effect analysis (su_effects.h) over the world's systems, run at world.create when there is more
 * than one system. Every system is declared parallel, so an unordered pair with a write/write or
 * read/write conflict is rejected (SU_E_SCHEMA_MISMATCH). Ordering cycles and unknown or
 * cross-phase edges are rejected by su_effects_build (SU_E_INVALID_ARGUMENT). The message names
 * the pair, the field or the cycle. */
static uint32_t check_system_effects(const SuSchedule *sched, const RevPaths *paths, WorkerFail *f) {
    uint32_t n = sched->count;
    uint32_t i;
    uint32_t st = SU_OK;
    SuJsonDoc *docs = NULL;
    uint8_t **bufs = NULL;
    SuEffectInput *in = NULL;
    const char **afters = NULL;
    const char **befores = NULL;
    uint32_t *after_n = NULL;
    uint32_t *before_n = NULL;
    SuEffectGraph *g = NULL;
    char err[WORKER_MSG_CAP];

    if (n < 2u) {
        return SU_OK;
    }
    docs = calloc(n, sizeof *docs);
    bufs = calloc(n, sizeof *bufs);
    in = calloc(n, sizeof *in);
    afters = calloc((size_t)n * SU_SCHED_MAX_SYSTEMS, sizeof *afters);
    befores = calloc((size_t)n * SU_SCHED_MAX_SYSTEMS, sizeof *befores);
    after_n = calloc(n, sizeof *after_n);
    before_n = calloc(n, sizeof *before_n);
    if (docs == NULL || bufs == NULL || in == NULL || afters == NULL || befores == NULL || after_n == NULL ||
        before_n == NULL) {
        fail(f, SU_E_RESOURCE_LIMIT, "out of memory for the effect analysis");
        st = SU_E_RESOURCE_LIMIT;
        goto done;
    }
    for (i = 0; i < n; i++) {
        const SuJsonValue *aft = NULL;
        const SuJsonValue *bef = NULL;
        const char *phase = "";
        st = read_schedule_facts(paths[i].capsule, &docs[i], &bufs[i], &phase, &aft, &bef, f);
        if (st != SU_OK) {
            goto done;
        }
        after_n[i] = string_items(aft, &afters[(size_t)i * SU_SCHED_MAX_SYSTEMS], SU_SCHED_MAX_SYSTEMS);
        before_n[i] = string_items(bef, &befores[(size_t)i * SU_SCHED_MAX_SYSTEMS], SU_SCHED_MAX_SYSTEMS);
        in[i].system_id = sched->sys[i].system_id;
        in[i].mod = sched->sys[i].mod;
        in[i].phase = phase;
        in[i].priority = 0u;
        in[i].declared_parallel = 1u;
        in[i].after = &afters[(size_t)i * SU_SCHED_MAX_SYSTEMS];
        in[i].after_count = after_n[i];
        in[i].before = &befores[(size_t)i * SU_SCHED_MAX_SYSTEMS];
        in[i].before_count = before_n[i];
    }
    st = su_effects_build(in, n, &g, err, sizeof err);
    if (st != SU_OK) {
        fail(f, st, err);
        goto done;
    }
    /* The world runs its systems in capsule order, so every `after` edge must point at an earlier
     * system of this world. An unknown target is already refused by su_effects_build. */
    for (i = 0; i < n && st == SU_OK; i++) {
        uint32_t k;
        for (k = 0; k < after_n[i] && st == SU_OK; k++) {
            uint32_t j;
            for (j = 0; j < n; j++) {
                if (strcmp(in[i].after[k], sched->sys[j].system_id) == 0) {
                    break;
                }
            }
            if (j < n && j >= i) {
                snprintf(err, sizeof err, "schedule edge %s after %s runs against the capsule order",
                         sched->sys[i].system_id, in[i].after[k]);
                fail(f, SU_E_INVALID_ARGUMENT, err);
                st = SU_E_INVALID_ARGUMENT;
            }
        }
    }

done:
    su_effects_free(g);
    for (i = 0; docs != NULL && bufs != NULL && i < n; i++) {
        su_json_doc_free(&docs[i]);
        free(bufs[i]);
    }
    free(docs);
    free(bufs);
    free(in);
    free(afters);
    free(befores);
    free(after_n);
    free(before_n);
    return st;
}

/* Parses the capsules array of world.create and builds a finalized schedule. Systems
 * are added in array order; the first failure leaves sched holding only what was added
 * (the caller frees it). */
static uint32_t load_schedule(const SuJsonValue *caps, SuSchedule *sched, RevPaths *paths,
                              WorkerFail *f) {
    /* Optional capsule_sha256 / source_sha256 pin the bytes the file must still have. */
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
        st = su_reload_load(cap, src, str_of(item, "source_sha256"), str_of(item, "capsule_sha256"), &mod,
                            &sv, &gen, msg, sizeof msg);
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
        return st;
    }
    return check_system_effects(sched, paths, f);
}

/* ---------- handlers ---------- */

/* Parses "world-N" (N = 1..999999999, no leading zero). */
static int parse_world_id(const char *id, uint64_t *n) {
    uint64_t v = 0;
    size_t i;
    size_t digits;
    if (strncmp(id, "world-", 6) != 0) {
        return 0;
    }
    digits = strlen(id + 6);
    if (digits == 0 || digits > 9 || (id[6] == '0')) {
        return 0;
    }
    for (i = 0; i < digits; i++) {
        if (id[6 + i] < '0' || id[6 + i] > '9') {
            return 0;
        }
        v = v * 10u + (uint64_t)(id[6 + i] - '0');
    }
    *n = v;
    return 1;
}

/* ---------- entity control plane (M2-00c3b, contract M2_ENTITIES_CONTRACT.md sections 2, 4, 5) ----------
 *
 * Every world owns an entity runtime (ww->sched.ent): the bounded event log and the per-tick caps. The
 * control plane spawns and despawns between ticks directly on the committed root (never queued, never
 * inside a tick). Each mutating method bumps the world revision and appends one control-plane record
 * (system_index SU_SYSTEM_INDEX_HOST) to the log. */
#define WORKER_EVENTS_MAX 256u

/* Optional unsigned integer member. Absent keeps dflt; a non-integer is SU_E_TYPE; a value outside
 * [lo, hi] is SU_E_RESOURCE_LIMIT. */
static uint32_t opt_uint(const SuJsonValue *params, const char *key, uint64_t dflt, uint64_t lo, uint64_t hi,
                         uint64_t *out, WorkerFail *f) {
    const SuJsonValue *v = su_json_member(params, key);
    char msg[WORKER_MSG_CAP];
    *out = dflt;
    if (v == NULL) {
        return SU_OK;
    }
    if (!su_json_get_uint(v, out)) {
        snprintf(msg, sizeof msg, "%s must be an integer", key);
        fail(f, SU_E_TYPE, msg);
        *out = dflt;
        return SU_E_TYPE;
    }
    if (*out < lo || *out > hi) {
        snprintf(msg, sizeof msg, "%s is outside its range", key);
        fail(f, SU_E_RESOURCE_LIMIT, msg);
        *out = dflt;
        return SU_E_RESOURCE_LIMIT;
    }
    return SU_OK;
}

static SuEventLog *ent_log(const WorkerWorld *ww) {
    return ww->sched.ent == NULL ? NULL : su_ent_rt_log(ww->sched.ent);
}

/* Appends one control-plane record at the next event_seq; tick is the committed tick. */
static uint32_t ent_log_one(WorkerWorld *ww, uint32_t kind, uint32_t slot, uint32_t a, uint32_t b) {
    SuEventLog *log = ent_log(ww);
    SuEvent ev;
    if (log == NULL) {
        return SU_E_INTERNAL;
    }
    memset(&ev, 0, sizeof ev);
    ev.event_seq = su_evlog_next_seq(log);
    ev.tick = su_world_tick(ww->inst.w);
    ev.kind = kind;
    ev.system_index = SU_SYSTEM_INDEX_HOST;
    ev.slot = slot;
    ev.seq = 0u;
    ev.a = a;
    ev.b = b;
    return su_evlog_append(log, &ev, 1u);
}

/* Event log state for a snapshot. The retained events are copied into a buffer the caller frees. A
 * world without a log snapshots an empty log with the default capacity. */
typedef struct EntSnap {
    SuEvent *buf;
    SuSnapshotEvents ev;
} EntSnap;

static uint32_t ent_snap_events(const WorkerWorld *ww, EntSnap *e, WorkerFail *f) {
    SuEventLog *log = ent_log(ww);
    uint64_t n;
    uint64_t got = 0u;

    memset(e, 0, sizeof *e);
    e->ev.log_max = SU_SNAPSHOT_DEFAULT_EVENT_LOG_MAX;
    if (log == NULL) {
        return SU_OK;
    }
    n = su_evlog_count(log);
    e->buf = malloc((size_t)(n > 0u ? n : 1u) * sizeof *e->buf);
    if (e->buf == NULL) {
        fail(f, SU_E_RESOURCE_LIMIT, "allocation failed");
        return SU_E_RESOURCE_LIMIT;
    }
    if (su_evlog_copy_retained(log, e->buf, n, &got) != SU_OK) {
        free(e->buf);
        e->buf = NULL;
        fail(f, SU_E_INTERNAL, "event log could not be read");
        return SU_E_INTERNAL;
    }
    e->ev.next_seq = su_evlog_next_seq(log);
    e->ev.dropped = su_evlog_dropped(log);
    e->ev.log_max = su_evlog_capacity(log);
    e->ev.entries = e->buf;
    e->ev.count = got;
    return SU_OK;
}

static void ent_snap_free(EntSnap *e) {
    free(e->buf);
    e->buf = NULL;
}

/* Stale or missing expected_world_revision. */
static uint32_t check_expected(const WorkerWorld *ww, const char *expect, WorkerFail *f) {
    char rev[64];
    if (expect == NULL) {
        fail(f, SU_E_TYPE, "expected_world_revision is required");
        return SU_E_TYPE;
    }
    rev_string(ww, rev, sizeof rev);
    if (strcmp(expect, rev) != 0) {
        fail(f, SU_E_STALE_REVISION, "expected_world_revision does not match the active world");
        return SU_E_STALE_REVISION;
    }
    return SU_OK;
}

static uint32_t h_world_create(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    const char *fixture = str_of(params, "fixture_path");
    const char *want = str_of(params, "world_id");
    const SuJsonValue *caps = su_json_member(params, "capsules");
    WorkerWorld *ww = NULL;
    WorldInst inst;
    SuSchedule sched;
    char id[WORKER_ID_CAP];
    uint64_t want_n = 0;
    uint32_t i;
    uint32_t st;
    uint64_t txid;

    uint64_t max_entities = 0;
    uint64_t max_struct = 0;
    uint64_t max_events = 0;
    uint64_t log_max = 0;

    if (fixture == NULL) {
        fail(f, SU_E_TYPE, "world.create needs fixture_path and capsules");
        return SU_E_TYPE;
    }
    if (opt_uint(params, "max_entities", 0u, 0u, SU_LIMIT_ROWS, &max_entities, f) != SU_OK ||
        opt_uint(params, "max_struct_per_tick", SU_CMDQ_DEFAULT_PER_TICK_CAP, 1u, SU_CMDQ_MAX_TICK_CAP,
                 &max_struct, f) != SU_OK ||
        opt_uint(params, "max_events_per_tick", SU_CMDQ_DEFAULT_PER_TICK_CAP, 1u, SU_CMDQ_MAX_TICK_CAP,
                 &max_events, f) != SU_OK ||
        opt_uint(params, "event_log_max", SU_CMDQ_DEFAULT_EVENT_LOG_MAX, 1u, SU_CMDQ_MAX_LOG_CAPACITY,
                 &log_max, f) != SU_OK) {
        return f->code;
    }
    if (want != NULL && (!parse_world_id(want, &want_n) || find_world(s, want) != NULL)) {
        fail(f, SU_E_INVALID_ARGUMENT, "world_id is malformed or already in use");
        return SU_E_INVALID_ARGUMENT;
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
    st = su_sched_enable_entities(&sched, max_struct, max_events, log_max);
    if (st != SU_OK) {
        su_sched_free(&sched);
        fail(f, st, "entity runtime could not be configured");
        return st;
    }
    st = world_open_ex(&sched, fixture, str_of(params, "fixture_sha256"), max_entities, &inst, f);
    if (st != SU_OK) {
        su_sched_free(&sched);
        return st;
    }
    if (want != NULL) {
        /* Supervisor recovery: the world keeps the id it had before the worker died. */
        snprintf(id, sizeof id, "%s", want);
        if (want_n > s->next_id) {
            s->next_id = want_n;
        }
    } else {
        snprintf(id, sizeof id, "world-%llu", (unsigned long long)(s->next_id + 1u));
    }

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

    if (want == NULL) {
        s->next_id++;
    }
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

/* world.spawn {world_id, expected_world_revision, values:{<field>: u32}, external_id?: u32}. */
static uint32_t h_world_spawn(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    WorkerWorld *ww;
    const char *expect = str_of(params, "expected_world_revision");
    const SuJsonValue *vals = su_json_member(params, "values");
    const SuJsonValue *xid = su_json_member(params, "external_id");
    SuWorldSpawnValue sv[SU_LIMIT_FIELDS];
    uint64_t ext = 0;
    uint64_t v = 0;
    SuEntity h = SU_ENTITY_INVALID;
    uint32_t n = 0;
    uint32_t i;
    uint32_t st = SU_OK;
    char rev[64];

    ww = world_param(s, params, f, &st);
    if (st != SU_OK) {
        return st;
    }
    st = check_expected(ww, expect, f);
    if (st != SU_OK) {
        return st;
    }
    if (vals == NULL || vals->type != SU_JSON_OBJECT || ent_log(ww) == NULL) {
        fail(f, SU_E_TYPE, "world.spawn needs values as an object of field names to u32");
        return SU_E_TYPE;
    }
    if (xid != NULL && (!su_json_get_uint(xid, &ext) || ext > UINT32_MAX)) {
        fail(f, SU_E_TYPE, "external_id must be a u32 integer");
        return SU_E_TYPE;
    }
    if (vals->u.object.count > SU_LIMIT_FIELDS) {
        fail(f, SU_E_SCHEMA_MISMATCH, "values names an unknown field");
        return SU_E_SCHEMA_MISMATCH;
    }
    for (i = 0; i < vals->u.object.count; i++) {
        const SuJsonMember *m = &vals->u.object.members[i];
        int found = -1;
        uint32_t k;
        for (k = 0; k < ww->sched.field_count; k++) {
            const char *nm = ww->sched.fields[k].name;
            if (strlen(nm) == m->key_len && memcmp(nm, m->key, m->key_len) == 0) {
                found = (int)k;
                break;
            }
        }
        if (found < 0) {
            fail(f, SU_E_SCHEMA_MISMATCH, "values names an unknown field");
            return SU_E_SCHEMA_MISMATCH;
        }
        if (!su_json_get_uint(m->value, &v) || v > UINT32_MAX) {
            fail(f, SU_E_TYPE, "field values must be u32 integers");
            return SU_E_TYPE;
        }
        sv[n].field_id = ww->sched.fields[found].id;
        sv[n].value = (uint32_t)v;
        n++;
    }
    st = su_world_spawn(ww->inst.w, sv, n, (uint32_t)ext, &h);
    if (st != SU_OK) {
        fail(f, st, "spawn refused; world unchanged");
        return st;
    }
    st = ent_log_one(ww, SU_EVENT_SPAWNED, SU_ENTITY_SLOT(h), SU_ENTITY_SLOT(h), SU_ENTITY_GEN(h));
    if (st != SU_OK) {
        fail(f, SU_E_INTERNAL, "spawn record could not be appended");
        return SU_E_INTERNAL;
    }
    ww->revision++;
    rev_string(ww, rev, sizeof rev);
    su_buf_str(res, "{\"world_id\":");
    su_buf_quoted(res, ww->id, strlen(ww->id));
    su_buf_str(res, ",\"world_revision\":");
    su_buf_quoted(res, rev, strlen(rev));
    su_buf_str(res, ",\"entity\":");
    su_buf_u64(res, h);
    su_buf_str(res, ",\"slot\":");
    su_buf_u64(res, SU_ENTITY_SLOT(h));
    su_buf_str(res, ",\"generation\":");
    su_buf_u64(res, SU_ENTITY_GEN(h));
    su_buf_str(res, "}");
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* world.despawn {world_id, expected_world_revision, entity}. Stale, dead, retired, out-of-range, zero
 * and generation-0 handles are SU_E_STALE_HANDLE and change nothing. */
static uint32_t h_world_despawn(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    WorkerWorld *ww;
    const char *expect = str_of(params, "expected_world_revision");
    const SuJsonValue *eh = su_json_member(params, "entity");
    uint64_t hv = 0;
    uint32_t st = SU_OK;
    char rev[64];

    ww = world_param(s, params, f, &st);
    if (st != SU_OK) {
        return st;
    }
    st = check_expected(ww, expect, f);
    if (st != SU_OK) {
        return st;
    }
    if (eh == NULL || !su_json_get_uint(eh, &hv) || ent_log(ww) == NULL) {
        fail(f, SU_E_TYPE, "world.despawn needs entity as an integer handle");
        return SU_E_TYPE;
    }
    st = su_world_despawn(ww->inst.w, (SuEntity)hv);
    if (st != SU_OK) {
        if (st == SU_E_INVALID_ARGUMENT) {
            fail(f, st, "despawn is not allowed in this state");
        } else {
            fail(f, SU_E_STALE_HANDLE, "entity handle is stale");
            st = SU_E_STALE_HANDLE;
        }
        return st;
    }
    st = ent_log_one(ww, SU_EVENT_DESPAWNED, SU_ENTITY_SLOT(hv), SU_ENTITY_SLOT(hv), SU_ENTITY_GEN(hv));
    if (st != SU_OK) {
        fail(f, SU_E_INTERNAL, "despawn record could not be appended");
        return SU_E_INTERNAL;
    }
    ww->revision++;
    rev_string(ww, rev, sizeof rev);
    su_buf_str(res, "{\"world_id\":");
    su_buf_quoted(res, ww->id, strlen(ww->id));
    su_buf_str(res, ",\"world_revision\":");
    su_buf_quoted(res, rev, strlen(rev));
    su_buf_str(res, ",\"entity\":");
    su_buf_u64(res, hv);
    su_buf_str(res, "}");
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* events.read {world_id, cursor, max <= 256}: read-only, no revision. */
static uint32_t h_events_read(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    WorkerWorld *ww;
    uint64_t cursor = 0;
    uint64_t max = 0;
    uint64_t n = 0;
    uint64_t next_cursor = 0;
    uint64_t skipped = 0;
    uint64_t dropped = 0;
    uint64_t i;
    uint32_t st = SU_OK;
    SuEvent out[WORKER_EVENTS_MAX];
    SuEventLog *log;

    ww = world_param(s, params, f, &st);
    if (st != SU_OK) {
        return st;
    }
    if (!su_json_get_uint(su_json_member(params, "cursor"), &cursor) ||
        !su_json_get_uint(su_json_member(params, "max"), &max)) {
        fail(f, SU_E_TYPE, "events.read needs cursor and max as integers");
        return SU_E_TYPE;
    }
    if (max < 1u || max > WORKER_EVENTS_MAX) {
        fail(f, SU_E_RESOURCE_LIMIT, "max must be 1..256");
        return SU_E_RESOURCE_LIMIT;
    }
    log = ent_log(ww);
    if (log == NULL) {
        fail(f, SU_E_INTERNAL, "world has no event log");
        return SU_E_INTERNAL;
    }
    st = su_evlog_read(log, cursor, max, out, &n, &next_cursor, &skipped, &dropped);
    if (st != SU_OK) {
        fail(f, st, "cursor is ahead of the next event sequence");
        return st;
    }
    su_buf_str(res, "{\"world_id\":");
    su_buf_quoted(res, ww->id, strlen(ww->id));
    su_buf_str(res, ",\"events\":[");
    for (i = 0; i < n; i++) {
        const SuEvent *e = &out[i];
        if (i > 0u) {
            su_buf_str(res, ",");
        }
        su_buf_str(res, "{\"event_seq\":");
        su_buf_u64(res, e->event_seq);
        su_buf_str(res, ",\"tick\":");
        su_buf_u64(res, e->tick);
        su_buf_str(res, ",\"kind\":");
        su_buf_u64(res, e->kind);
        su_buf_str(res, ",\"system_index\":");
        su_buf_u64(res, e->system_index);
        su_buf_str(res, ",\"slot\":");
        su_buf_u64(res, e->slot);
        su_buf_str(res, ",\"seq\":");
        su_buf_u64(res, e->seq);
        su_buf_str(res, ",\"a\":");
        su_buf_u64(res, e->a);
        su_buf_str(res, ",\"b\":");
        su_buf_u64(res, e->b);
        su_buf_str(res, "}");
    }
    su_buf_str(res, "],\"next_cursor\":");
    su_buf_u64(res, next_cursor);
    su_buf_str(res, ",\"skipped\":");
    su_buf_u64(res, skipped);
    su_buf_str(res, ",\"dropped\":");
    su_buf_u64(res, dropped);
    su_buf_str(res, "}");
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

static uint32_t h_world_inspect(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    WorkerWorld *ww;
    uint64_t limit = WORKER_INSPECT_DEFAULT_ROWS;
    uint64_t offset = 0;
    uint64_t shown;
    const SuJsonValue *fields;
    const SuJsonValue *lim;
    const SuJsonValue *ent_h = NULL;
    uint64_t eh_val = 0;
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
    ent_h = su_json_member(params, "entity");
    if (ent_h != NULL && !su_json_get_uint(ent_h, &eh_val)) {
        fail(f, SU_E_TYPE, "entity must be an integer handle");
        return SU_E_TYPE;
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
    su_buf_str(res, "],\"entities\":{\"capacity\":");
    su_buf_u64(res, su_ents_capacity(su_world_entities_committed(ww->inst.w)));
    su_buf_str(res, ",\"alive\":");
    su_buf_u64(res, su_ents_alive_count(su_world_entities_committed(ww->inst.w)));
    su_buf_str(res, ",\"retired\":");
    su_buf_u64(res, su_ents_retired_count(su_world_entities_committed(ww->inst.w)));
    su_buf_str(res, ",\"high_water\":");
    su_buf_u64(res, su_ents_high_water(su_world_entities_committed(ww->inst.w)));
    su_buf_str(res, "}");
    if (ent_h != NULL) {
        const SuEnts *ents = su_world_entities_committed(ww->inst.w);
        uint32_t ext = 0;
        uint32_t k;
        uint32_t slot = SU_ENTITY_SLOT(eh_val);
        if (su_ents_validate(ents, eh_val) != SU_OK) {
            fail(f, SU_E_STALE_HANDLE, "entity handle is stale");
            return SU_E_STALE_HANDLE;
        }
        (void)su_ents_get_external(ents, eh_val, &ext);
        su_buf_str(res, ",\"entity\":{\"handle\":");
        su_buf_u64(res, eh_val);
        su_buf_str(res, ",\"slot\":");
        su_buf_u64(res, slot);
        su_buf_str(res, ",\"generation\":");
        su_buf_u64(res, SU_ENTITY_GEN(eh_val));
        su_buf_str(res, ",\"external_id\":");
        su_buf_u64(res, ext);
        su_buf_str(res, ",\"values\":{");
        for (k = 0; k < ww->sched.field_count; k++) {
            const uint32_t *data = NULL;
            uint64_t rc = 0;
            const char *nm = ww->sched.fields[k].name;
            if (su_world_read_committed_by_name(ww->inst.w, nm, &data, &rc) != SU_OK || slot >= rc) {
                fail(f, SU_E_INTERNAL, "entity row could not be read");
                return SU_E_INTERNAL;
            }
            if (k > 0u) {
                su_buf_str(res, ",");
            }
            su_buf_quoted(res, nm, strlen(nm));
            su_buf_str(res, ":");
            su_buf_u64(res, data[slot]);
        }
        su_buf_str(res, "}}");
    }
    su_buf_str(res, ",\"columns\":{");

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

#ifdef SU_TEST_FAULT_HOOKS
/* Test-only crash seam (compiled only into workers built with -DSU_TEST_FAULT_HOOKS; the
 * production binary contains none of these strings). Before system 0 of the tick that would
 * compute tick N, the worker dies by SIGKILL (or abort) when:
 *   SUTEKH_TEST_KILL_AT_TICK=N            (required, decimal)
 *   SUTEKH_TEST_KILL_SOURCE_SHA=<hex>     (optional) the world has a system with this source hash
 *   SUTEKH_TEST_KILL_TOKEN=<path>         (optional) the seam fires once: the file must exist and
 *                                         is unlinked first; replacements inherit the environment
 *   SUTEKH_TEST_KILL_MODE=abort           (optional) abort() instead of SIGKILL */
static void test_kill_configure(WorkerState *s) {
    const char *v = getenv("SUTEKH_TEST_KILL_AT_TICK");
    const char *src = getenv("SUTEKH_TEST_KILL_SOURCE_SHA");
    const char *tok = getenv("SUTEKH_TEST_KILL_TOKEN");
    const char *mode = getenv("SUTEKH_TEST_KILL_MODE");
    char *end = NULL;
    unsigned long long n;

    if (v == NULL || v[0] < '1' || v[0] > '9') {
        return;
    }
    n = strtoull(v, &end, 10);
    if (end == NULL || *end != '\0' || n == 0) {
        return;
    }
    s->test_kill_set = 1;
    s->test_kill_tick = (uint64_t)n;
    s->test_kill_abort = mode != NULL && strcmp(mode, "abort") == 0;
    if (src != NULL && strlen(src) == 64) {
        snprintf(s->test_kill_src, sizeof s->test_kill_src, "%s", src);
    }
    if (tok != NULL && tok[0] != '\0') {
        snprintf(s->test_kill_token, sizeof s->test_kill_token, "%s", tok);
    }
}

/* M1-02c-2: SU_TEST_FAULT_NATIVE_TICK=N (decimal) arms a SIGSEGV inside the installed kernel that
 * computes tick N; SU_TEST_FAULT_NATIVE_BACKEND=avx2|scalar restricts it to one backend (default: any).
 * Read only in this build; the production binary has no such string. */
#ifdef SU_HAVE_NATIVE
static void test_native_crash_configure(void) {
    const char *v = getenv("SU_TEST_FAULT_NATIVE_TICK");
    const char *b = getenv("SU_TEST_FAULT_NATIVE_BACKEND");
    char *end = NULL;
    unsigned long long n;
    uint32_t backend = 0u;

    if (v == NULL || v[0] < '1' || v[0] > '9') {
        return;
    }
    n = strtoull(v, &end, 10);
    if (end == NULL || *end != '\0') {
        return;
    }
    if (b != NULL && strcmp(b, "avx2") == 0) {
        backend = SU_NATIVE_BACKEND_ASMJIT_AVX2;
    } else if (b != NULL && strcmp(b, "scalar") == 0) {
        backend = SU_NATIVE_BACKEND_ASMJIT_SCALAR;
    }
    su_nv_test_arm_native_crash((uint64_t)n, backend);
}

#endif /* SU_HAVE_NATIVE */

static void test_kill_check(WorkerState *s, unsigned idx) {
    WorkerWorld *ww = s->cur_ww;
    uint32_t i;
    int gate = s->test_kill_src[0] == '\0';

    if (idx != 0 || !s->test_kill_set || ww == NULL || su_world_tick(ww->inst.w) + 1u != s->test_kill_tick) {
        return;
    }
    for (i = 0; !gate && i < ww->sched.count; i++) {
        if (strcmp(ww->sched.sys[i].source_hash, s->test_kill_src) == 0) {
            gate = 1;
        }
    }
    if (!gate) {
        return;
    }
    if (s->test_kill_token[0] != '\0' && unlink(s->test_kill_token) != 0) {
        return;
    }
    if (s->test_kill_abort) {
        abort();
    }
    kill(getpid(), SIGKILL);
    for (;;) {
        pause();
    }
}
#endif

static int fault_thunk(unsigned idx, void *ctx) {
    WorkerState *s = ctx;
#ifdef SU_TEST_FAULT_HOOKS
    test_kill_check(s, idx);
#endif
    return s->hooks.fail_system != NULL && s->hooks.fail_system(idx) != 0;
}

/* The fault seam runs before every system of every tick when a hook or a test crash is set. */
static int fault_seam_wanted(const WorkerState *s) {
#ifdef SU_TEST_FAULT_HOOKS
    if (s->test_kill_set) {
        return 1;
    }
#endif
    return s->hooks.fail_system != NULL;
}

/* Optional tick options of world.step (the supervisor sends them from SUTEKH_TICK_WORKERS and
 * SUTEKH_TICK_CHUNK_MIN_ROWS). Absent: serial execution, threshold 4096 rows. */
static uint32_t tick_opts_of(const SuJsonValue *params, SuSchedTickOpts *o, WorkerFail *f) {
    const SuJsonValue *v;
    uint64_t x = 0;
    memset(o, 0, sizeof *o);
    o->workers = 1u;
    o->chunk_min_rows = 4096u;
    v = su_json_member(params, "tick_workers");
    if (v != NULL) {
        if (!su_json_get_uint(v, &x) || x < 1u || x > SU_JOBS_MAX_WORKERS) {
            fail(f, SU_E_TYPE, "tick_workers must be an integer 1..64");
            return SU_E_TYPE;
        }
        o->workers = (uint32_t)x;
    }
    v = su_json_member(params, "chunk_min_rows");
    if (v != NULL) {
        if (!su_json_get_uint(v, &x) || x < 1u || x > UINT32_MAX) {
            fail(f, SU_E_TYPE, "chunk_min_rows must be an integer 1..4294967295");
            return SU_E_TYPE;
        }
        o->chunk_min_rows = x;
    }
    return SU_OK;
}

static uint32_t h_world_step(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    WorkerWorld *ww;
    SuSchedTickOpts topt;
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
    st = tick_opts_of(params, &topt, f);
    if (st != SU_OK) {
        return st;
    }
    s->cur_ww = ww;
    st = su_sched_run_ticks_opt(ww->inst.w, &ww->sched, ticks, fault_seam_wanted(s) ? fault_thunk : NULL,
                                s, &topt, &done, &failed_index);
    s->cur_ww = NULL;
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
    /* Engine timings (reload.c): bindings prepared before publish; monotonic-clock durations in
     * nanoseconds. null when the clock could not be read. */
    su_buf_str(res, ",\"bindings_prepared\":");
    su_buf_str(res, out.bindings_prepared ? "true" : "false");
    su_buf_str(res, ",\"prepare_ns\":");
    if (out.clock_ok) {
        su_buf_u64(res, out.prepare_ns);
    } else {
        su_buf_str(res, "null");
    }
    su_buf_str(res, ",\"critical_section_ns\":");
    if (out.clock_ok) {
        su_buf_u64(res, out.critical_section_ns);
    } else {
        su_buf_str(res, "null");
    }
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
/* M1-02c-3: the first differing word of an own test case, for the admission detail. */
typedef struct CaseMis {
    char field[WORKER_NAME_MAX + 1];
    uint64_t row;
    uint64_t expected;
    uint64_t actual;
} CaseMis;

static int case_columns_match(const SuSchedule *s, const WorldInst *in, const SuJsonValue *expected,
                              char *reason, size_t reason_cap, CaseMis *cm) {
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
            uint64_t want = 0;
            if (!su_json_get_uint(mem->value->u.array.items[r], &want) || want != data[r]) {
                snprintf(reason, reason_cap, "column_mismatch:%s[%llu]", name, (unsigned long long)r);
                if (cm != NULL) {
                    snprintf(cm->field, sizeof cm->field, "%s", name);
                    cm->row = r;
                    cm->expected = want;
                    cm->actual = data[r];
                }
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
        st = world_open(&sched, world_path, NULL, &in, f);
        if (st != SU_OK) {
            goto out;
        }
        st = su_sched_run_ticks(in.w, &sched, ticks, s->hooks.fail_system != NULL ? fault_thunk : NULL, s,
                                &done, &failed_index);
        if (st != SU_OK) {
            char r2[WORKER_MSG_CAP];
            snprintf(r2, sizeof r2, "tick_failed:%s", su_status_name(st));
            note_failure(&cf, name, r2);
        } else if (!case_columns_match(&sched, &in, exp, reason, sizeof reason, NULL)) {
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

/* Releases every checkpoint of world_id: the store frees the snapshot bytes and the
 * record is cleared. Records whose store entry was already evicted are cleared without
 * being counted. Returns the number of snapshots freed by this call. */
static uint64_t ckpt_release_world(WorkerState *s, const char *world_id) {
    uint64_t released = 0;
    uint32_t i;
    for (i = 0; i < SU_CHECKPOINT_DEFAULT_RETAIN; i++) {
        CkptRec *r = &s->ckpts[i];
        if (!r->used || strcmp(r->world_id, world_id) != 0) {
            continue;
        }
        if (su_checkpoint_release(s->store, r->id) == SU_OK) {
            released++;
        }
        memset(r, 0, sizeof *r);
    }
    return released;
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
    EntSnap es;
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
    st = ent_snap_events(ww, &es, f);
    if (st == SU_OK) {
        st = su_checkpoint_create_ex(s->store, ww->inst.w, &desc, rev, man, ww->sched.count, &es.ev, &id);
    }
    ent_snap_free(&es);
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
/* Restores snapshot bytes into ww. rec supplies the revision identities (capsule and source
 * paths with their hashes, system ids, semantic hashes) that the schedule is rebuilt from.
 * exact == 0 advances the world revision by one (client world.restore); exact == 1 sets it
 * to exact_rev (supervisor recovery and quarantine). Checks, registry and the swap are as
 * described for world.restore above. */
static uint32_t restore_core(WorkerState *s, WorkerWorld *ww, const CkptRec *rec, const uint8_t *snap_bytes,
                             size_t snap_len, int exact, uint64_t exact_rev, const char *restored_from,
                             SuBuf *res, WorkerFail *f) {
    SuWorldDesc desc;
    SuWorldFieldDesc *descs = NULL;
    SuWorldDesc desc2;
    SuWorldFieldDesc *descs2 = NULL;
    SuSnapshotData data;
    SuSnapshotError perr;
    SuSchedule nsched;
    SuWorld *neww = NULL;
    SuEntRt *nrt = NULL;
    uint64_t txids[SU_SCHED_MAX_SYSTEMS];
    uint32_t nbegun = 0;
    int parsed = 0;
    char rev[64];
    uint32_t i;
    uint32_t st = SU_OK;

    memset(&data, 0, sizeof data);
    memset(&nsched, 0, sizeof nsched);
    st = sched_desc(&ww->sched, ww->inst.ids, ww->inst.rows, &desc, &descs);
    if (st != SU_OK) {
        fail(f, st, "allocation failed");
        goto out;
    }
    st = su_snapshot_parse(snap_bytes, snap_len, &desc, &data, &perr);
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
    /* Entity runtime: the per-tick caps stay those of the world being restored; the event log is
     * rebuilt from the snapshot (snapshot 0.2). A 0.1 snapshot carries no log: it restores empty with
     * the current capacity. */
    {
        uint64_t ms = 0;
        uint64_t me = 0;
        uint64_t lm = 0;
        if (su_ent_rt_caps(ww->sched.ent, &ms, &me, &lm) != SU_OK) {
            st = SU_E_INTERNAL;
            fail(f, st, "entity runtime is missing");
            goto out;
        }
        if (data.format_minor == 2) {
            st = su_ent_rt_create_restored(ms, me, data.event_log_max, data.event_next_seq, data.event_dropped,
                                           data.events, data.event_count, &nrt);
        } else {
            st = su_ent_rt_create_restored(ms, me, lm, 0u, 0u, NULL, 0u, &nrt);
        }
        if (st != SU_OK) {
            fail(f, st, "event log could not be restored; world unchanged");
            goto out;
        }
        st = su_sched_adopt_entities(&nsched, nrt);
        if (st != SU_OK) {
            su_ent_rt_destroy(nrt);
            nrt = NULL;
            fail(f, st, "event log could not be restored; world unchanged");
            goto out;
        }
        nrt = NULL;
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
    ww->revision = exact ? exact_rev : ww->revision + 1u;
    st = SU_OK;

    rev_string(ww, rev, sizeof rev);
    su_buf_str(res, "{\"world_id\":");
    su_buf_quoted(res, ww->id, strlen(ww->id));
    su_buf_str(res, ",\"world_revision\":");
    su_buf_quoted(res, rev, strlen(rev));
    su_buf_str(res, ",\"tick\":");
    su_buf_u64(res, su_world_tick(ww->inst.w));
    su_buf_str(res, ",\"restored_from\":");
    su_buf_quoted(res, restored_from, strlen(restored_from));
    su_buf_str(res, ",\"worker_pid\":");
    su_buf_u64(res, (uint64_t)getpid());
    su_buf_str(res, ",\"semantic_manifest\":");
    write_manifest(res, &ww->sched);
    su_buf_str(res, "}");

out:
    if (neww != NULL) {
        su_world_destroy(neww);
    }
    su_ent_rt_destroy(nrt);
    su_sched_free(&nsched);
    if (parsed) {
        su_snapshot_data_free(&data);
    }
    free(descs2);
    free(descs);
    return st;
}


static uint32_t h_world_restore(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    WorkerWorld *ww;
    const char *snap_id = str_of(params, "snapshot_id");
    const char *expect = str_of(params, "expected_world_revision");
    CkptRec *rec;
    const SuSnapshot *snap = NULL;
    char rev[64];
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

    return restore_core(s, ww, rec, snap->bytes, snap->len, 0, 0, rec->snapshot_id, res, f);
}

/* ---------- supervisor recovery support (internal methods, M1-02) ----------
 *
 * These are reached only from the supervisor's recovery path. The supervisor passes paths
 * it owns (its spool directory) with the sha256 of the bytes they must hold; the worker
 * checks the bytes before using them. */

/* Writes data to a new file (O_EXCL, mode 0600). */
static uint32_t write_new_file(const char *path, const uint8_t *data, size_t len, WorkerFail *f) {
    size_t off = 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        fail(f, SU_E_IO, "spool file could not be created");
        return SU_E_IO;
    }
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            close(fd);
            unlink(path);
            fail(f, SU_E_IO, "spool file could not be written");
            return SU_E_IO;
        }
        off += (size_t)n;
    }
    if (close(fd) != 0) {
        unlink(path);
        fail(f, SU_E_IO, "spool file could not be closed");
        return SU_E_IO;
    }
    return SU_OK;
}

/* Canonical snapshot of ww's committed root. The caller destroys snap. */
static uint32_t world_capture(const WorkerWorld *ww, SuSnapshot *snap, WorkerFail *f) {
    SuSnapshotManifestEntry man[SU_SCHED_MAX_SYSTEMS];
    SuWorldDesc desc;
    SuWorldFieldDesc *descs = NULL;
    EntSnap es;
    char rev[64];
    uint32_t st;

    rev_string(ww, rev, sizeof rev);
    sched_manifest(&ww->sched, man);
    st = sched_desc(&ww->sched, ww->inst.ids, ww->inst.rows, &desc, &descs);
    if (st != SU_OK) {
        fail(f, st, "allocation failed");
        return st;
    }
    st = ent_snap_events(ww, &es, f);
    if (st == SU_OK) {
        st = su_snapshot_capture_ex(ww->inst.w, &desc, rev, man, ww->sched.count, &es.ev, snap);
    }
    ent_snap_free(&es);
    free(descs);
    if (st != SU_OK) {
        fail(f, st, "canonical state could not be captured");
    }
    return st;
}

/* internal.world.seed {next_id}: raises the id counter so a replacement worker never issues
 * an id the supervisor has already handed out. */
static uint32_t h_internal_world_seed(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    uint64_t n = 0;
    if (!su_json_get_uint(su_json_member(params, "next_id"), &n) || n > 999999999u) {
        fail(f, SU_E_TYPE, "next_id must be an integer 0..999999999");
        return SU_E_TYPE;
    }
    if (n > s->next_id) {
        s->next_id = n;
    }
    su_buf_str(res, "{\"next_id\":");
    su_buf_u64(res, s->next_id);
    su_buf_str(res, "}");
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* internal.world.digest {world_id}: canonical state digest; nothing is stored. */
static uint32_t h_internal_world_digest(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    WorkerWorld *ww;
    SuSnapshot snap;
    char rev[64];
    uint32_t st = SU_OK;

    ww = world_param(s, params, f, &st);
    if (st != SU_OK) {
        return st;
    }
    memset(&snap, 0, sizeof snap);
    st = world_capture(ww, &snap, f);
    if (st != SU_OK) {
        return st;
    }
    rev_string(ww, rev, sizeof rev);
    su_buf_str(res, "{\"world_id\":");
    su_buf_quoted(res, ww->id, strlen(ww->id));
    su_buf_str(res, ",\"world_revision\":");
    su_buf_quoted(res, rev, strlen(rev));
    su_buf_str(res, ",\"tick\":");
    su_buf_u64(res, snap.tick);
    su_buf_str(res, ",\"row_count\":");
    su_buf_u64(res, ww->inst.rows);
    su_buf_str(res, ",\"sha256\":");
    su_buf_quoted(res, snap.id, strlen(snap.id));
    su_buf_str(res, "}");
    su_snapshot_destroy(&snap);
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* internal.world.capture {world_id, expected_world_revision, spool_path}: the canonical bytes
 * are written to a new file the supervisor owns. Unlike world.snapshot, nothing is retained in
 * the worker's own store. */
static uint32_t h_internal_world_capture(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    WorkerWorld *ww;
    const char *expect = str_of(params, "expected_world_revision");
    const char *path = str_of(params, "spool_path");
    SuSnapshot snap;
    char rev[64];
    uint32_t st = SU_OK;

    ww = world_param(s, params, f, &st);
    if (st != SU_OK) {
        return st;
    }
    if (expect == NULL || path == NULL) {
        fail(f, SU_E_TYPE, "capture needs expected_world_revision and spool_path");
        return SU_E_TYPE;
    }
    rev_string(ww, rev, sizeof rev);
    if (strcmp(expect, rev) != 0) {
        fail(f, SU_E_STALE_REVISION, "expected_world_revision does not match the active world");
        return SU_E_STALE_REVISION;
    }
    memset(&snap, 0, sizeof snap);
    st = world_capture(ww, &snap, f);
    if (st != SU_OK) {
        return st;
    }
    st = write_new_file(path, snap.bytes, snap.len, f);
    if (st != SU_OK) {
        su_snapshot_destroy(&snap);
        return st;
    }
    su_buf_str(res, "{\"world_id\":");
    su_buf_quoted(res, ww->id, strlen(ww->id));
    su_buf_str(res, ",\"world_revision\":");
    su_buf_quoted(res, rev, strlen(rev));
    su_buf_str(res, ",\"tick\":");
    su_buf_u64(res, snap.tick);
    su_buf_str(res, ",\"bytes\":");
    su_buf_u64(res, (uint64_t)snap.len);
    su_buf_str(res, ",\"row_count\":");
    su_buf_u64(res, ww->inst.rows);
    su_buf_str(res, ",\"field_count\":");
    su_buf_u64(res, ww->sched.field_count);
    su_buf_str(res, ",\"sha256\":");
    su_buf_quoted(res, snap.id, strlen(snap.id));
    su_buf_str(res, ",\"semantic_manifest\":");
    write_manifest(res, &ww->sched);
    su_buf_str(res, "}");
    su_snapshot_destroy(&snap);
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* Copies one capsules entry of internal.world.restore_bytes into c. Returns 0 if a field is
 * missing or does not fit its buffer. */
static int restore_entry_fill(CkptSys *c, const SuJsonValue *item) {
    static const char *const keys[6] = {"system_id", "capsule_path", "source_path", "capsule_sha256",
                                        "source_sha256", "semantic_hash"};
    const size_t caps_max[6] = {SU_DIAG_SYSTEM_BYTES, SU_PATH_BUF, SU_PATH_BUF, 65, 65, 65};
    const char *vals[6];
    size_t k;

    for (k = 0; k < 6; k++) {
        vals[k] = str_of(item, keys[k]);
        if (vals[k] == NULL || strlen(vals[k]) >= caps_max[k]) {
            return 0;
        }
    }
    snprintf(c->system_id, sizeof c->system_id, "%s", vals[0]);
    snprintf(c->paths.capsule, sizeof c->paths.capsule, "%s", vals[1]);
    snprintf(c->paths.source, sizeof c->paths.source, "%s", vals[2]);
    snprintf(c->capsule_hash, sizeof c->capsule_hash, "%s", vals[3]);
    snprintf(c->source_hash, sizeof c->source_hash, "%s", vals[4]);
    snprintf(c->semantic_hash, sizeof c->semantic_hash, "%s", vals[5]);
    return 1;
}

/* internal.world.restore_bytes {world_id, expected_world_revision, snapshot_path, snapshot_sha256,
 * mode: "advance" | "exact", world_revision (exact only), capsules: [{system_id, capsule_path,
 * source_path, capsule_sha256, source_sha256, semantic_hash}, ...]}. The capsules list is the
 * schedule the snapshot was captured with, in schedule order. The file must hash to
 * snapshot_sha256 (SU_E_INTERNAL otherwise). */
static uint32_t h_internal_world_restore_bytes(WorkerState *s, const SuJsonValue *params, SuBuf *res,
                                               WorkerFail *f) {
    WorkerWorld *ww;
    const char *expect = str_of(params, "expected_world_revision");
    const char *path = str_of(params, "snapshot_path");
    const char *sha = str_of(params, "snapshot_sha256");
    const char *mode = str_of(params, "mode");
    const SuJsonValue *caps = su_json_member(params, "capsules");
    uint64_t exact_rev = 0;
    int exact;
    uint8_t *data = NULL;
    size_t len = 0;
    CkptRec *rec = NULL;
    char rev[64];
    size_t i;
    uint32_t st = SU_OK;

    ww = world_param(s, params, f, &st);
    if (st != SU_OK) {
        return st;
    }
    if (expect == NULL || path == NULL || sha == NULL || mode == NULL || caps == NULL ||
        caps->type != SU_JSON_ARRAY || caps->u.array.count == 0 || caps->u.array.count > SU_SCHED_MAX_SYSTEMS) {
        fail(f, SU_E_TYPE, "restore_bytes needs expected_world_revision, snapshot_path, snapshot_sha256, mode and 1..8 capsules");
        return SU_E_TYPE;
    }
    if (strcmp(mode, "exact") == 0) {
        exact = 1;
        if (!su_json_get_uint(su_json_member(params, "world_revision"), &exact_rev)) {
            fail(f, SU_E_TYPE, "exact mode needs an integer world_revision");
            return SU_E_TYPE;
        }
    } else if (strcmp(mode, "advance") == 0) {
        exact = 0;
    } else {
        fail(f, SU_E_TYPE, "mode must be advance or exact");
        return SU_E_TYPE;
    }
    rev_string(ww, rev, sizeof rev);
    if (strcmp(expect, rev) != 0) {
        fail(f, SU_E_STALE_REVISION, "expected_world_revision does not match the active world");
        return SU_E_STALE_REVISION;
    }
    st = su_path_read_file(path, SU_SNAPSHOT_MAX_BYTES, &data, &len);
    if (st != SU_OK) {
        fail(f, st, "snapshot file could not be read");
        return st;
    }
    {
        SuSha256 ctx;
        uint8_t dig[32];
        char hex[65];
        su_sha256_init(&ctx);
        su_sha256_update(&ctx, data, len);
        su_sha256_final(&ctx, dig);
        su_sha256_hex(dig, hex);
        if (strcmp(hex, sha) != 0) {
            free(data);
            fail(f, SU_E_INTERNAL, "snapshot file does not match its recorded sha256");
            return SU_E_INTERNAL;
        }
    }
    rec = calloc(1, sizeof *rec);
    if (rec == NULL) {
        free(data);
        fail(f, SU_E_RESOURCE_LIMIT, "allocation failed");
        return SU_E_RESOURCE_LIMIT;
    }
    rec->used = 1;
    snprintf(rec->snapshot_id, sizeof rec->snapshot_id, "%s", sha);
    rec->count = (uint32_t)caps->u.array.count;
    for (i = 0; i < caps->u.array.count; i++) {
        if (!restore_entry_fill(&rec->sys[i], caps->u.array.items[i])) {
            free(rec);
            free(data);
            fail(f, SU_E_TYPE, "each capsules entry needs system_id, capsule_path, source_path, capsule_sha256, source_sha256 and semantic_hash");
            return SU_E_TYPE;
        }
    }
    st = restore_core(s, ww, rec, data, len, exact, exact_rev, sha, res, f);
#ifdef SU_TEST_FAULT_HOOKS
    if (st == SU_OK && exact && getenv("SUTEKH_TEST_SKEW_EXACT") != NULL) {
        /* Test-only: one extra committed tick the snapshot does not describe. The supervisor's
         * digest of the rebuilt world must then differ from the recorded one. */
        uint64_t done = 0;
        uint32_t fi = 0;
        (void)su_sched_run_ticks(ww->inst.w, &ww->sched, 1, NULL, NULL, &done, &fi);
    }
#endif
    free(rec);
    free(data);
    return st;
}

/* world.close {world_id, expected_world_revision}. Checks come first and change nothing:
 * an unknown id is SU_E_INVALID_ARGUMENT (the code world.inspect uses), a stale revision is
 * SU_E_STALE_REVISION. Then the world's schedule and contexts, its instance, its checkpoints
 * and its registry records are freed, and the slot is free for world.create. World ids
 * come from the monotonic s->next_id, so a closed id is never issued again. */
static uint32_t h_world_close(WorkerState *s, const SuJsonValue *params, SuBuf *res, WorkerFail *f) {
    WorkerWorld *ww;
    const char *expect = str_of(params, "expected_world_revision");
    char id[WORKER_ID_CAP];
    char rev[64];
    uint64_t snapshots;
    uint64_t systems;
    uint32_t st = SU_OK;

    ww = world_param(s, params, f, &st);
    if (st != SU_OK) {
        return st;
    }
    if (expect == NULL) {
        fail(f, SU_E_TYPE, "world.close needs expected_world_revision");
        return SU_E_TYPE;
    }
    rev_string(ww, rev, sizeof rev);
    if (strcmp(expect, rev) != 0) {
        fail(f, SU_E_STALE_REVISION, "expected_world_revision does not match the active world");
        return SU_E_STALE_REVISION;
    }

    snprintf(id, sizeof id, "%s", ww->id);
    systems = ww->sched.count;
    snapshots = ckpt_release_world(s, id);
    su_reg_release_world(&s->reg, id);
    world_drop(ww);

    su_buf_str(res, "{\"world_id\":");
    su_buf_quoted(res, id, strlen(id));
    su_buf_str(res, ",\"closed\":true,\"released\":{\"snapshots\":");
    su_buf_u64(res, snapshots);
    su_buf_str(res, ",\"systems\":");
    su_buf_u64(res, systems);
    su_buf_str(res, "}}");
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* internal.stats {}: counts this worker holds, read from the structures themselves. The
 * supervisor never forwards it (it is not advertised); the control tests use it to check
 * that world.close releases worlds, checkpoints and registry records. */
#ifdef SU_TEST_FAULT_HOOKS
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter 426
#endif
/* M1 closure D: io_uring escape probe (test seam only). Sets up a ring with a raw io_uring_setup and submits
 * one IORING_OP_SOCKET (AF_INET, SOCK_STREAM). *setup_errno is 0 when the ring exists, otherwise errno.
 * *op_res is the completion result (a socket fd, which is closed, or -errno), or -1000 when not attempted. */
static void test_uring_socket(int *setup_errno, long *op_res) {
    struct io_uring_params p;
    size_t sq_sz;
    size_t cq_sz;
    size_t sqe_sz;
    void *sqp = MAP_FAILED;
    void *cqp = MAP_FAILED;
    void *sqep = MAP_FAILED;
    int rfd;
    *setup_errno = 0;
    *op_res = -1000;
    memset(&p, 0, sizeof p);
    rfd = (int)syscall(__NR_io_uring_setup, 4u, &p);
    if (rfd < 0) {
        *setup_errno = errno;
        return;
    }
    sq_sz = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
    cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    if ((p.features & IORING_FEAT_SINGLE_MMAP) && cq_sz > sq_sz) {
        sq_sz = cq_sz;
    }
    sqe_sz = p.sq_entries * sizeof(struct io_uring_sqe);
    sqp = mmap(NULL, sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, rfd, IORING_OFF_SQ_RING);
    sqep = mmap(NULL, sqe_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, rfd, IORING_OFF_SQES);
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        cqp = sqp;
    } else {
        cqp = mmap(NULL, cq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, rfd, IORING_OFF_CQ_RING);
    }
    if (sqp != MAP_FAILED && sqep != MAP_FAILED && cqp != MAP_FAILED) {
        uint32_t *sq_tail = (uint32_t *)((char *)sqp + p.sq_off.tail);
        uint32_t sq_mask = *(uint32_t *)((char *)sqp + p.sq_off.ring_mask);
        uint32_t *sq_array = (uint32_t *)((char *)sqp + p.sq_off.array);
        struct io_uring_sqe *sqes = (struct io_uring_sqe *)sqep;
        uint32_t *cq_head = (uint32_t *)((char *)cqp + p.cq_off.head);
        uint32_t *cq_tail = (uint32_t *)((char *)cqp + p.cq_off.tail);
        uint32_t cq_mask = *(uint32_t *)((char *)cqp + p.cq_off.ring_mask);
        struct io_uring_cqe *cqes = (struct io_uring_cqe *)((char *)cqp + p.cq_off.cqes);
        uint32_t tail = __atomic_load_n(sq_tail, __ATOMIC_RELAXED);
        uint32_t slot = tail & sq_mask;
        memset(&sqes[0], 0, sizeof sqes[0]);
        sqes[0].opcode = IORING_OP_SOCKET;
        sqes[0].fd = AF_INET;
        sqes[0].off = SOCK_STREAM;
        sqes[0].len = 0;
        sq_array[slot] = 0;
        __atomic_store_n(sq_tail, tail + 1u, __ATOMIC_RELEASE);
        if (syscall(__NR_io_uring_enter, rfd, 1u, 1u, IORING_ENTER_GETEVENTS, NULL, 0) < 0) {
            *op_res = -(long)errno;
        } else {
            uint32_t head = __atomic_load_n(cq_head, __ATOMIC_ACQUIRE);
            if (__atomic_load_n(cq_tail, __ATOMIC_ACQUIRE) != head) {
                int32_t res = cqes[head & cq_mask].res;
                *op_res = (long)res;
                if (res >= 0) {
                    close((int)res);
                }
            }
        }
    } else {
        *op_res = -(long)ENOMEM;
    }
    if (sqep != MAP_FAILED) {
        munmap(sqep, sqe_sz);
    }
    if (cqp != MAP_FAILED && cqp != sqp) {
        munmap(cqp, cq_sz);
    }
    if (sqp != MAP_FAILED) {
        munmap(sqp, sq_sz);
    }
    close(rfd);
}

/* M1 closure D: child-side sandbox probe (test seam). Under the worker self-sandbox, execve("/bin/true") and
 * fork() must fail with EPERM. Reports both errnos; no process is created when they are denied. */
static uint32_t h_internal_test_sandbox(SuBuf *res) {
    char *argv[] = {NULL};
    char *envp[] = {NULL};
    int exec_errno;
    int fork_errno = 0;
    pid_t pid;
    (void)execve("/bin/true", argv, envp);
    exec_errno = errno;
    pid = fork();
    if (pid < 0) {
        fork_errno = errno;
    } else if (pid == 0) {
        _exit(0);
    } else {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    su_buf_str(res, "{\"execve_errno\":");
    su_buf_u64(res, (uint64_t)exec_errno);
    su_buf_str(res, ",\"fork_errno\":");
    su_buf_u64(res, (uint64_t)fork_errno);
    su_buf_str(res, "}");
    return SU_OK;
}

/* M1 closure D: file creation probe (test seam). Tries to create and then remove one file in each of the
 * directories named by SU_TEST_FS_PROJECT, SU_TEST_FS_TMP and SU_TEST_CONTAINMENT_DIR. Reports errno per dir
 * (0 = created). A directory variable that is unset reports -1. */
static void fs_create_probe(const char *dir, uint64_t *out) {
    char path[600];
    int fd;
    if (dir == NULL || strlen(dir) > 500u) {
        *out = (uint64_t)-1;
        return;
    }
    snprintf(path, sizeof path, "%s/sutekh-fs-probe-%ld.tmp", dir, (long)getpid());
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        *out = (uint64_t)errno;
        return;
    }
    close(fd);
    unlink(path);
    *out = 0u;
}

static uint32_t h_internal_test_fs(SuBuf *res) {
    uint64_t proj;
    uint64_t tmp;
    uint64_t scratch;
    fs_create_probe(getenv("SU_TEST_FS_PROJECT"), &proj);
    fs_create_probe(getenv("SU_TEST_FS_TMP"), &tmp);
    fs_create_probe(getenv("SU_TEST_CONTAINMENT_DIR"), &scratch);
    su_buf_str(res, "{\"project_errno\":");
    su_buf_u64(res, proj);
    su_buf_str(res, ",\"tmp_errno\":");
    su_buf_u64(res, tmp);
    su_buf_str(res, ",\"scratch_errno\":");
    su_buf_u64(res, scratch);
    su_buf_str(res, "}");
    return SU_OK;
}

/* M1 closure D: foreign-ABI and x32 socket calls (test seam). Each issues socket(AF_INET, SOCK_STREAM) through
 * an entry the x86-64 filter must refuse: int 0x80 (i386 ABI, eax 359) and the x32 number 0x40000000|41. Under
 * the filter the process is killed (SIGSYS) before the call returns, so an answer means the call ran. */
static uint32_t h_internal_test_i386_socket(SuBuf *res) {
#if defined(__x86_64__)
    long ret = -1;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(359L), "b"(2L), "c"(1L), "d"(0L) : "memory");
    if (ret >= 0) {
        close((int)ret);
    }
    su_buf_str(res, "{\"returned\":");
    su_buf_u64(res, (uint64_t)ret);
    su_buf_str(res, "}");
    return SU_OK;
#else
    (void)res;
    return SU_E_UNSUPPORTED_METHOD;
#endif
}

static uint32_t h_internal_test_x32_socket(SuBuf *res) {
#if defined(__x86_64__)
    long ret = syscall(0x40000000L | 41L, 2L, 1L, 0L);
    if (ret >= 0) {
        close((int)ret);
    }
    su_buf_str(res, "{\"returned\":");
    su_buf_u64(res, (uint64_t)ret);
    su_buf_str(res, "}");
    return SU_OK;
#else
    (void)res;
    return SU_E_UNSUPPORTED_METHOD;
#endif
}
#endif

#ifdef SU_TEST_FAULT_HOOKS
#include <sys/socket.h>
/* Test-only containment probe (C07), compiled only into test workers. It runs inside whatever child
 * the test spawned, so it reports what that child actually enforces: socket(2) errno, bytes written
 * to a file in SU_TEST_CONTAINMENT_DIR until a write fails, descriptors opened until open(2) fails, and
 * the child's own /proc/self/status Seccomp line and /proc/self/limits text. */
static uint32_t h_internal_test_containment(SuBuf *res) {
    static char buf[65536];
    static char proc[8192];
    const char *dir = getenv("SU_TEST_CONTAINMENT_DIR");
    char path[600];
    uint64_t written = 0;
    uint64_t opened = 0;
    int werr = 0;
    int ferr = 0;
    int serr = 0;
    int sfd;
    int fd;
    int k;
    ssize_t n;
    size_t nread = 0;
    const char *sec;
    size_t i;

    if (dir == NULL || strlen(dir) > 400u) {
        return SU_E_INVALID_ARGUMENT;
    }
    sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd >= 0) {
        close(sfd);
    } else {
        serr = errno;
    }
    snprintf(path, sizeof path, "%s/probe.bin", dir);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return SU_E_INTERNAL;
    }
    memset(buf, 'x', sizeof buf);
    for (k = 0; k < 128; k++) {
        n = write(fd, buf, sizeof buf);
        if (n < 0) {
            werr = errno;
            break;
        }
        written += (uint64_t)n;
    }
    close(fd);
    {
        int fds[256];
        int nfd = 0;
        while (nfd < 256) {
            int g = open("/dev/null", O_RDONLY);
            if (g < 0) {
                ferr = errno;
                break;
            }
            fds[nfd++] = g;
        }
        opened = (uint64_t)nfd;
        for (i = 0; i < (size_t)nfd; i++) {
            close(fds[i]);
        }
    }
    fd = open("/proc/self/status", O_RDONLY);
    if (fd >= 0) {
        while (nread + 1u < sizeof proc) {
            n = read(fd, proc + nread, sizeof proc - 1u - nread);
            if (n <= 0) {
                break;
            }
            nread += (size_t)n;
        }
        proc[nread] = '\0';
        close(fd);
    } else {
        proc[0] = '\0';
    }
    sec = strstr(proc, "\nSeccomp:");
    {
        int pair_errno = 0;
        int uring_errno = 0;
        long uring_res = -1000;
        const char *filt = strstr(proc, "\nSeccomp_filters:");
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            close(sv[0]);
            close(sv[1]);
        } else {
            pair_errno = errno;
        }
        test_uring_socket(&uring_errno, &uring_res);
        su_buf_str(res, "{\"socketpair_errno\":");
        su_buf_u64(res, (uint64_t)pair_errno);
        su_buf_str(res, ",\"uring_setup_errno\":");
        su_buf_u64(res, (uint64_t)uring_errno);
        su_buf_str(res, ",\"uring_socket_result\":");
        su_buf_str(res, uring_res == -1000 ? "null" : "");
        if (uring_res != -1000) {
            char num[32];
            snprintf(num, sizeof num, "%ld", uring_res);
            su_buf_str(res, num);
        }
        su_buf_str(res, ",\"seccomp_filters_line\":");
        if (filt != NULL) {
            const char *e = strchr(filt + 1, '\n');
            size_t len = e != NULL ? (size_t)(e - (filt + 1)) : strlen(filt + 1);
            su_buf_quoted(res, filt + 1, len);
        } else {
            su_buf_str(res, "null");
        }
        su_buf_str(res, ",\"seccomp_filters_present\":");
        su_buf_str(res, filt != NULL ? "true" : "false");
    }
    su_buf_str(res, ",\"socket_errno\":");
    su_buf_u64(res, (uint64_t)serr);
    su_buf_str(res, ",\"socket_created\":");
    su_buf_str(res, serr == 0 ? "true" : "false");
    su_buf_str(res, ",\"bytes_written\":");
    su_buf_u64(res, written);
    su_buf_str(res, ",\"write_errno\":");
    su_buf_u64(res, (uint64_t)werr);
    su_buf_str(res, ",\"fds_opened\":");
    su_buf_u64(res, opened);
    su_buf_str(res, ",\"open_errno\":");
    su_buf_u64(res, (uint64_t)ferr);
    su_buf_str(res, ",\"seccomp_line\":");
    if (sec != NULL) {
        const char *e = strchr(sec + 1, '\n');
        size_t len = e != NULL ? (size_t)(e - (sec + 1)) : strlen(sec + 1);
        su_buf_quoted(res, sec + 1, len);
    } else {
        su_buf_str(res, "null");
    }
    fd = open("/proc/self/limits", O_RDONLY);
    nread = 0;
    if (fd >= 0) {
        while (nread + 1u < sizeof proc) {
            n = read(fd, proc + nread, sizeof proc - 1u - nread);
            if (n <= 0) {
                break;
            }
            nread += (size_t)n;
        }
        proc[nread] = '\0';
        close(fd);
    } else {
        proc[0] = '\0';
        nread = 0;
    }
    su_buf_str(res, ",\"limits\":");
    su_buf_quoted(res, proc, nread);
    su_buf_str(res, "}");
    return SU_OK;
}
#endif

static uint32_t h_internal_stats(WorkerState *s, SuBuf *res) {
    SuCheckpointStats cs;
    uint32_t worlds = 0;
    uint32_t in_use = 0;
    uint32_t installed = 0;
    uint64_t records = 0;
    uint32_t i;

    for (i = 0; i < WORKER_MAX_WORLDS; i++) {
        worlds += s->worlds[i].used ? 1u : 0u;
    }
    for (i = 0; i < SU_CHECKPOINT_DEFAULT_RETAIN; i++) {
        records += s->ckpts[i].used ? 1u : 0u;
    }
    su_reg_stats(&s->reg, &in_use, &installed);
    if (su_checkpoint_stats(s->store, &cs) != SU_OK) {
        return SU_E_INTERNAL;
    }
    su_buf_str(res, "{\"self_sandbox\":{\"state\":\"");
    su_buf_str(res, su_sandbox_state_name(s->sandbox.state));
    su_buf_str(res, "\",\"filters\":");
    su_buf_u64(res, s->sandbox.filters);
    su_buf_str(res, ",\"reason\":");
    su_buf_quoted(res, s->sandbox.reason, strlen(s->sandbox.reason));
    su_buf_str(res, "},\"worlds\":");
    su_buf_u64(res, worlds);
    su_buf_str(res, ",\"checkpoints\":");
    su_buf_u64(res, cs.retained_count);
    su_buf_str(res, ",\"checkpoint_bytes\":");
    su_buf_u64(res, cs.retained_bytes);
    su_buf_str(res, ",\"checkpoint_records\":");
    su_buf_u64(res, records);
    su_buf_str(res, ",\"registry_in_use\":");
    su_buf_u64(res, in_use);
    su_buf_str(res, ",\"registry_installed\":");
    su_buf_u64(res, installed);
    su_buf_str(res, ",\"world_seq\":");
    su_buf_u64(res, s->next_id);
    su_buf_str(res, "}");
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* ---------- dispatch and main loop ---------- */

#ifdef SU_HAVE_NATIVE
/* M1-02c-1: world.specialize is a supervisor operation (src/agent/su_agent.c, docs/AGENT_RPC.md section
 * 9). The worker answers three internal requests:
 *   internal.specialize.identity  active worker, read-only: identity of an installed system.
 *   internal.specialize.admit     DISPOSABLE worker only: compile, then differential admission over the
 *                                 seeded corpus and the system's own test cases. The only place a
 *                                 candidate kernel executes.
 *   internal.specialize.install   active worker: recompile (compilation only), require the admitted
 *                                 artifact_hash, recheck the revision and semantic hash, publish.
 * The active worker never executes a kernel whose artifact_hash was not admitted. */

#define SPEC_ADMIT_SEED 20261011u

static uint32_t spec_caps(const char *backend, WorkerFail *f, uint32_t *caps, int *tier_auto) {
    uint32_t host = su_native_host_caps();
    *tier_auto = 0;
    if (strcmp(backend, "scalar") == 0) {
        *caps = SU_NCAP_X86_64_SCALAR;
        return SU_OK;
    }
    if (strcmp(backend, "avx2") == 0) {
        if ((host & SU_NCAP_X86_64_AVX2) == 0u) {
            fail(f, SU_E_CAPABILITY_DENIED,
                 "avx2 is not usable on this host; fallback: backend scalar (request auto to fall back automatically)");
            return SU_E_CAPABILITY_DENIED;
        }
        *caps = SU_NCAP_X86_64_AVX2;
        return SU_OK;
    }
    if (strcmp(backend, "auto") == 0) {
        *caps = su_native_best_backend();
        *tier_auto = 1;
        return SU_OK;
    }
    fail(f, SU_E_INVALID_ARGUMENT, "backend must be scalar, avx2 or auto");
    return SU_E_INVALID_ARGUMENT;
}

/* Compiles m. An auto request whose avx2 frame bound is exceeded falls back to scalar. Both the
 * disposable worker and the active worker use this, so the two compile the same artifact. */
static uint32_t spec_prepare(const SuLirModule *m, uint32_t gen, int tier_auto, uint32_t *caps,
                             uint64_t *seq, char *fallback, size_t fb_cap, char *msg, size_t cap) {
    uint32_t st = su_nv_prepare(m, gen, *caps, seq, msg, cap);
    if (st == SU_E_RESOURCE_LIMIT && tier_auto && *caps == SU_NCAP_X86_64_AVX2) {
        snprintf(fallback, fb_cap, "avx2 frame bound exceeded; fell back to scalar");
        *caps = SU_NCAP_X86_64_SCALAR;
        st = su_nv_prepare(m, gen, *caps, seq, msg, cap);
    }
    return st;
}

#ifdef SU_TEST_FAULT_HOOKS
/* Test-only seams. Read from the environment ONLY in worker builds that define SU_TEST_FAULT_HOOKS
 * (tests/native/qualification). SU_TEST_FAULT selects one mode for the disposable admission
 * (wrong_kernel, wrong_world, crash, hang, delay; delay uses SU_TEST_FAULT_MS) or perturb_recompile
 * for the active install. */
static void spec_fault_arm_admit(void) {
    const char *m = getenv("SU_TEST_FAULT");
    const char *ms = getenv("SU_TEST_FAULT_MS");
    uint32_t mode = SU_NV_FAULT_NONE;
    if (m != NULL) {
        if (strcmp(m, "wrong_kernel") == 0) {
            mode = SU_NV_FAULT_WRONG_KERNEL;
        } else if (strcmp(m, "wrong_world") == 0) {
            mode = SU_NV_FAULT_WRONG_WORLD;
        } else if (strcmp(m, "crash") == 0) {
            mode = SU_NV_FAULT_CRASH;
        } else if (strcmp(m, "hang") == 0) {
            mode = SU_NV_FAULT_HANG;
        } else if (strcmp(m, "delay") == 0) {
            mode = SU_NV_FAULT_DELAY;
        }
    }
    su_nv_test_arm(mode, ms != NULL ? (uint32_t)strtoul(ms, NULL, 10) : 0u);
}
static int spec_fault_perturb_install(void) {
    const char *m = getenv("SU_TEST_FAULT");
    return m != NULL && strcmp(m, "perturb_recompile") == 0;
}
/* perturb_trust: the hash handed to su_nv_trust_admitted differs from the admitted one. The equality
 * check before it refuses the install first, so this reaches the second (defense-in-depth) check. */
static int spec_fault_perturb_trust(void) {
    const char *m = getenv("SU_TEST_FAULT");
    return m != NULL && strcmp(m, "perturb_trust") == 0;
}
#define SPEC_FAULT_ARM_ADMIT() spec_fault_arm_admit()
#define SPEC_FAULT_PERTURB() spec_fault_perturb_install()
#else
#define SPEC_FAULT_ARM_ADMIT() ((void)0)
#define SPEC_FAULT_PERTURB() 0
#endif

/* internal.specialize.identity {world_id, expected_world_revision, system_id, expected_semantic_hash?}.
 * Read-only: nothing is compiled or executed. */
static uint32_t h_internal_specialize_identity(WorkerState *s, const SuJsonValue *params, SuBuf *res,
                                               WorkerFail *f) {
    WorkerWorld *ww;
    const char *expect = str_of(params, "expected_world_revision");
    const char *system_id = str_of(params, "system_id");
    const char *exp_sem = str_of(params, "expected_semantic_hash");
    char rev[64];
    uint32_t st = SU_OK;
    int j;

    ww = world_param(s, params, f, &st);
    if (st != SU_OK) {
        return st;
    }
    if (expect == NULL || system_id == NULL) {
        fail(f, SU_E_TYPE, "specialize identity needs expected_world_revision and system_id");
        return SU_E_TYPE;
    }
    rev_string(ww, rev, sizeof rev);
    if (strcmp(expect, rev) != 0) {
        fail(f, SU_E_STALE_REVISION, "expected_world_revision does not match the active world");
        return SU_E_STALE_REVISION;
    }
    j = su_sched_find(&ww->sched, system_id);
    if (j < 0) {
        fail(f, SU_E_INVALID_ARGUMENT, "system_id is not part of this world");
        return SU_E_INVALID_ARGUMENT;
    }
    if (exp_sem != NULL && strcmp(exp_sem, ww->sched.sys[j].semantic_hash) != 0) {
        fail(f, SU_E_STALE_REVISION, "expected_semantic_hash does not match the installed system revision");
        return SU_E_STALE_REVISION;
    }
    su_buf_str(res, "{\"world_id\":");
    su_buf_quoted(res, ww->id, strlen(ww->id));
    su_buf_str(res, ",\"system_id\":");
    su_buf_quoted(res, system_id, strlen(system_id));
    su_buf_str(res, ",\"world_revision\":");
    su_buf_quoted(res, rev, strlen(rev));
    su_buf_str(res, ",\"system_revision\":");
    su_buf_u64(res, ww->sched.sys[j].system_rev);
    su_buf_str(res, ",\"semantic_hash\":");
    su_buf_quoted(res, ww->sched.sys[j].semantic_hash, strlen(ww->sched.sys[j].semantic_hash));
    su_buf_str(res, ",\"source_hash\":");
    su_buf_quoted(res, ww->sched.sys[j].source_hash, strlen(ww->sched.sys[j].source_hash));
    su_buf_str(res, ",\"capsule_hash\":");
    su_buf_quoted(res, ww->sched.sys[j].capsule_hash, strlen(ww->sched.sys[j].capsule_hash));
    su_buf_str(res, ",\"layout_generation\":");
    su_buf_u64(res, ww->sched.layout_generation);
    su_buf_str(res, ",\"tick\":");
    su_buf_u64(res, su_world_tick(ww->inst.w));
    su_buf_str(res, "}");
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* Appends the first mismatch of a failed admission (corpus or own test case). */
static void spec_put_mismatch(SuBuf *res, int own, const char *own_name, const char *own_reason,
                              const CaseMis *cm, const SuNvMismatch *mis) {
    if (own) {
        su_buf_str(res, "{\"source\":\"own_case\",\"case\":");
        su_buf_quoted(res, own_name, strlen(own_name));
        su_buf_str(res, ",\"reason\":");
        su_buf_quoted(res, own_reason, strlen(own_reason));
        if (cm != NULL) {
            su_buf_str(res, ",\"field\":");
            su_buf_quoted(res, cm->field, strlen(cm->field));
            su_buf_str(res, ",\"row\":");
            su_buf_u64(res, cm->row);
            su_buf_str(res, ",\"expected\":");
            su_buf_u64(res, cm->expected);
            su_buf_str(res, ",\"actual\":");
            su_buf_u64(res, cm->actual);
        }
        su_buf_str(res, "}");
        return;
    }
    su_buf_str(res, "{\"source\":\"corpus\",\"case_index\":");
    su_buf_u64(res, mis->case_index);
    su_buf_str(res, ",\"case_rows\":");
    su_buf_u64(res, mis->case_rows);
    su_buf_str(res, ",\"case_offset\":");
    su_buf_u64(res, mis->case_offset);
    su_buf_str(res, ",\"field_id\":");
    su_buf_u64(res, mis->field_id);
    su_buf_str(res, ",\"row\":");
    su_buf_u64(res, mis->row);
    su_buf_str(res, ",\"expected\":");
    su_buf_u64(res, mis->expected);
    su_buf_str(res, ",\"actual\":");
    su_buf_u64(res, mis->actual);
    su_buf_str(res, "}");
}

/* internal.specialize.admit {capsule_path, source_path, test_path?, expected_source_hash,
 * expected_capsule_hash, expected_semantic_hash, expected_layout_generation, backend}. DISPOSABLE
 * worker only. Result status "admitted" (artifact_hash, evidence) or "mismatch" (first_mismatch).
 * Compile or load failures answer ok:false. A crash, hang or timeout is seen by the supervisor. */
static uint32_t h_internal_specialize_admit(WorkerState *s, const SuJsonValue *params, SuBuf *res,
                                            WorkerFail *f) {
    const char *capsule = str_of(params, "capsule_path");
    const char *source = str_of(params, "source_path");
    const char *test_path = str_of(params, "test_path");
    const char *exp_src = str_of(params, "expected_source_hash");
    const char *exp_cap = str_of(params, "expected_capsule_hash");
    const char *exp_sem = str_of(params, "expected_semantic_hash");
    const char *backend = str_of(params, "backend");
    const SuJsonValue *gen_v = params != NULL ? su_json_member(params, "expected_layout_generation") : NULL;
    uint64_t exp_gen = 0;
    SuSchedule sched;
    SuLirModule *mod = NULL;
    uint8_t *tdata = NULL;
    size_t tlen = 0;
    SuJsonLimits limits;
    SuJsonDoc doc;
    SuJsonError jerr;
    const SuJsonValue *cases = NULL;
    SuNvMismatch mis;
    uint32_t sv = 0;
    uint32_t gen = 0;
    uint32_t caps = 0;
    uint32_t cases_n = 0;
    uint64_t rows = 0;
    uint64_t bad = 0;
    uint64_t seq = 0;
    uint64_t own_total = 0;
    uint64_t own_bad = 0;
    char own_name[WORKER_MSG_CAP];
    char own_reason[WORKER_MSG_CAP];
    CaseMis own_mis;
    CaseMis own_mis_saved;
    int own_has_mis = 0;
    char fallback[64];
    char msg[WORKER_MSG_CAP];
    char hash[65];
    char wpath[SU_PATH_BUF];
    uint32_t backend_id = 0;
    int tier_auto = 0;
    int own_first = 1;
    int status_mismatch = 0;
    int have_doc = 0;
    size_t i;
    uint32_t st;

    (void)s;
    fallback[0] = '\0';
    own_name[0] = '\0';
    own_reason[0] = '\0';
    memset(&own_mis, 0, sizeof own_mis);
    memset(&own_mis_saved, 0, sizeof own_mis_saved);
    memset(&mis, 0, sizeof mis);
    memset(&doc, 0, sizeof doc);
    if (capsule == NULL || source == NULL || exp_src == NULL || exp_cap == NULL || exp_sem == NULL ||
        backend == NULL || gen_v == NULL || !su_json_get_uint(gen_v, &exp_gen)) {
        fail(f, SU_E_TYPE, "specialize admission parameters are incomplete");
        return SU_E_TYPE;
    }
    st = spec_caps(backend, f, &caps, &tier_auto);
    if (st != SU_OK) {
        return st;
    }
    SPEC_FAULT_ARM_ADMIT();
    su_sched_init(&sched);
    st = su_reload_load(capsule, source, exp_src, exp_cap, &mod, &sv, &gen, msg, sizeof msg);
    if (st != SU_OK) {
        fail(f, st, msg);
        goto out;
    }
    if ((uint64_t)gen != exp_gen) {
        su_reload_module_free(mod);
        fail(f, SU_E_SCHEMA_MISMATCH, "layout generation differs from the installed system");
        st = SU_E_SCHEMA_MISMATCH;
        goto out;
    }
    st = su_sched_add(&sched, mod, sv, gen, 0, 1, 0);
    if (st != SU_OK) {
        su_reload_module_free(mod);
        fail(f, st, "candidate cannot be scheduled");
        goto out;
    }
    st = su_sched_finalize(&sched);
    if (st != SU_OK) {
        fail(f, st, "candidate field table is inconsistent");
        goto out;
    }
    if (strcmp(sched.sys[0].semantic_hash, exp_sem) != 0) {
        fail(f, SU_E_SCHEMA_MISMATCH, "semantic hash differs from the installed system revision");
        st = SU_E_SCHEMA_MISMATCH;
        goto out;
    }
    st = spec_prepare(sched.sys[0].mod, gen, tier_auto, &caps, &seq, fallback, sizeof fallback, msg,
                      sizeof msg);
    if (st != SU_OK) {
        fail(f, SU_E_CANDIDATE_FAILED, msg);
        st = SU_E_CANDIDATE_FAILED;
        goto out;
    }
    /* Corpus admission (VM is the oracle). */
    st = su_nv_admit(seq, SPEC_ADMIT_SEED, &cases_n, &rows, &bad);
    if (st != SU_OK && st != SU_E_CANDIDATE_FAILED) {
        fail(f, st, "differential admission could not run");
        goto out;
    }
    if (bad != 0u) {
        status_mismatch = 1;
    } else {
        /* Own test cases: the installed kernel runs them in this disposable worker. */
        st = su_nv_install(seq);
        if (st != SU_OK) {
            fail(f, SU_E_INTERNAL, "admitted kernel could not be staged");
            st = SU_E_INTERNAL;
            goto out;
        }
        sched.sys[0].native_seq = seq;
    }
    if (!status_mismatch && test_path != NULL) {
        const char *world_name;
        size_t dl;
        st = su_path_read_file(test_path, WORKER_TEST_MAX, &tdata, &tlen);
        if (st != SU_OK) {
            fail(f, st, "own test file could not be read");
            goto out;
        }
        limits.max_bytes = tlen;
        limits.max_depth = SU_PROTO_MAX_DEPTH;
        st = su_json_parse(tdata, tlen, &limits, &doc, &jerr);
        free(tdata);
        tdata = NULL;
        if (st != SU_OK) {
            fail(f, st, jerr.message);
            goto out;
        }
        have_doc = 1;
        cases = doc.root != NULL && doc.root->type == SU_JSON_OBJECT ? su_json_member(doc.root, "cases") : NULL;
        world_name = str_of(doc.root, "world");
        if (cases == NULL || cases->type != SU_JSON_ARRAY || world_name == NULL || strchr(world_name, '/') != NULL ||
            strchr(world_name, '\\') != NULL || strcmp(world_name, "..") == 0) {
            fail(f, SU_E_TYPE, "own test file needs a cases array and a plain world name");
            st = SU_E_TYPE;
            goto out;
        }
        dl = strlen(test_path);
        while (dl > 0 && test_path[dl - 1] != '/') {
            dl--;
        }
        snprintf(wpath, sizeof wpath, "%.*s%s", (int)dl, test_path, world_name);
        for (i = 0; i < cases->u.array.count; i++) {
            const SuJsonValue *c = cases->u.array.items[i];
            const char *name = str_of(c, "name");
            const SuJsonValue *ticks_v = c != NULL && c->type == SU_JSON_OBJECT ? su_json_member(c, "ticks") : NULL;
            const SuJsonValue *exp = c != NULL && c->type == SU_JSON_OBJECT ? su_json_member(c, "expected_columns") : NULL;
            uint64_t ticks = 0;
            uint64_t done = 0;
            uint32_t failed_index = UINT32_MAX;
            WorldInst in;
            char reason[WORKER_MSG_CAP];
            char r2[WORKER_MSG_CAP];

            if (name == NULL || ticks_v == NULL || !su_json_get_uint(ticks_v, &ticks) ||
                ticks > WORKER_MAX_TICKS || exp == NULL) {
                fail(f, SU_E_TYPE, "each own case needs name, ticks (<= 100000) and expected_columns");
                st = SU_E_TYPE;
                goto out;
            }
            own_total++;
            memset(&own_mis, 0, sizeof own_mis);
            st = world_open(&sched, wpath, NULL, &in, f);
            if (st != SU_OK) {
                goto out;
            }
            st = su_sched_run_ticks(in.w, &sched, ticks, NULL, NULL, &done, &failed_index);
            if (st != SU_OK) {
                snprintf(r2, sizeof r2, "tick_failed:%s", su_status_name(st));
                if (own_first) {
                    own_first = 0;
                    snprintf(own_name, sizeof own_name, "%s", name);
                    snprintf(own_reason, sizeof own_reason, "%s", r2);
                }
                own_bad++;
            } else if (!case_columns_match(&sched, &in, exp, reason, sizeof reason, &own_mis)) {
                if (own_first) {
                    own_first = 0;
                    snprintf(own_name, sizeof own_name, "%s", name);
                    snprintf(own_reason, sizeof own_reason, "%s", reason);
                    own_has_mis = own_mis.field[0] != '\0';
                    own_mis_saved = own_mis;
                }
                own_bad++;
            }
            inst_free(&in);
        }
        if (own_bad != 0u) {
            status_mismatch = 1;
        }
    }
    if (su_nv_artifact_hash(seq, hash) != SU_OK || su_nv_backend_id(seq, &backend_id) != SU_OK) {
        fail(f, SU_E_INTERNAL, "artifact identity unavailable");
        st = SU_E_INTERNAL;
        goto out;
    }
    if (status_mismatch && bad != 0u) {
        (void)su_nv_first_mismatch(seq, &mis);
    }
    su_buf_str(res, "{\"status\":\"");
    su_buf_str(res, status_mismatch ? "mismatch" : "admitted");
    su_buf_str(res, "\",\"backend\":");
    su_buf_quoted(res, caps == SU_NCAP_X86_64_AVX2 ? "avx2" : "scalar", caps == SU_NCAP_X86_64_AVX2 ? 4u : 6u);
    su_buf_str(res, ",\"execution_tier\":");
    su_buf_quoted(res, backend_id == SU_NATIVE_BACKEND_ASMJIT_AVX2 ? "native_avx2" : "native_scalar",
                  backend_id == SU_NATIVE_BACKEND_ASMJIT_AVX2 ? 11u : 13u);
    su_buf_str(res, ",\"artifact_hash\":");
    su_buf_quoted(res, hash, strlen(hash));
    su_buf_str(res, ",\"semantic_hash\":");
    su_buf_quoted(res, sched.sys[0].semantic_hash, strlen(sched.sys[0].semantic_hash));
    su_buf_str(res, ",\"layout_generation\":");
    su_buf_u64(res, gen);
    su_buf_str(res, ",\"fallback\":");
    if (fallback[0] != '\0') {
        su_buf_quoted(res, fallback, strlen(fallback));
    } else {
        su_buf_str(res, "null");
    }
    su_buf_str(res, ",\"evidence\":{\"cases\":");
    su_buf_u64(res, cases_n + own_total);
    su_buf_str(res, ",\"rows_checked\":");
    su_buf_u64(res, rows);
    su_buf_str(res, ",\"mismatches\":");
    su_buf_u64(res, bad + own_bad);
    su_buf_str(res, ",\"corpus_cases\":");
    su_buf_u64(res, cases_n);
    su_buf_str(res, ",\"own_cases\":");
    su_buf_u64(res, own_total);
    su_buf_str(res, "},\"first_mismatch\":");
    if (status_mismatch) {
        spec_put_mismatch(res, bad == 0u, own_name, own_reason, own_has_mis ? &own_mis_saved : NULL, &mis);
    } else {
        su_buf_str(res, "null");
    }
    su_buf_str(res, "}");
    st = res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
out:
    su_nv_test_arm(SU_NV_FAULT_NONE, 0u);
    if (have_doc) {
        su_json_doc_free(&doc);
    }
    if (seq != 0u && (sched.count == 0u || sched.sys[0].native_seq == 0u)) {
        su_nv_release(seq);
    }
    su_sched_free(&sched);
    return st;
}

/* internal.specialize.install {world_id, system_id, expected_world_revision, expected_semantic_hash,
 * expected_layout_generation, backend, artifact_hash, admission_cases, admission_rows_checked}. Active
 * worker. Recompiles (compilation only) and requires the admitted artifact_hash. Nothing is executed
 * here: the kernel is marked admitted by hash, then installed at the schedule's safe point. */
static uint32_t h_internal_specialize_install(WorkerState *s, const SuJsonValue *params, SuBuf *res,
                                              WorkerFail *f) {
    const char *expect = str_of(params, "expected_world_revision");
    const char *system_id = str_of(params, "system_id");
    const char *exp_sem = str_of(params, "expected_semantic_hash");
    const char *backend = str_of(params, "backend");
    const char *admitted = str_of(params, "artifact_hash");
    const SuJsonValue *gen_v = params != NULL ? su_json_member(params, "expected_layout_generation") : NULL;
    const SuJsonValue *ac_v = params != NULL ? su_json_member(params, "admission_cases") : NULL;
    const SuJsonValue *ar_v = params != NULL ? su_json_member(params, "admission_rows_checked") : NULL;
    uint64_t exp_gen = 0;
    uint64_t adm_cases = 0;
    uint64_t adm_rows = 0;
    WorkerWorld *ww;
    char rev[64];
    char msg[WORKER_MSG_CAP];
    char hash2[65];
    char fallback[64];
    uint32_t caps = 0;
    uint32_t backend_id = 0;
    uint64_t seq2 = 0;
    uint64_t old_seq;
    int tier_auto = 0;
    uint32_t st = SU_OK;
    const char *tier;
    int j;

    fallback[0] = '\0';
    ww = world_param(s, params, f, &st);
    if (st != SU_OK) {
        return st;
    }
    if (expect == NULL || system_id == NULL || backend == NULL || admitted == NULL || exp_sem == NULL ||
        gen_v == NULL || !su_json_get_uint(gen_v, &exp_gen)) {
        fail(f, SU_E_TYPE, "specialize install needs its identity and the admitted artifact_hash");
        return SU_E_TYPE;
    }
    if (ac_v != NULL) {
        (void)su_json_get_uint(ac_v, &adm_cases);
    }
    if (ar_v != NULL) {
        (void)su_json_get_uint(ar_v, &adm_rows);
    }
    rev_string(ww, rev, sizeof rev);
    if (strcmp(expect, rev) != 0) {
        fail(f, SU_E_STALE_REVISION, "expected_world_revision does not match the active world");
        return SU_E_STALE_REVISION;
    }
    j = su_sched_find(&ww->sched, system_id);
    if (j < 0) {
        fail(f, SU_E_INVALID_ARGUMENT, "system_id is not part of this world");
        return SU_E_INVALID_ARGUMENT;
    }
    if (strcmp(exp_sem, ww->sched.sys[j].semantic_hash) != 0) {
        fail(f, SU_E_STALE_REVISION, "expected_semantic_hash does not match the installed system revision");
        return SU_E_STALE_REVISION;
    }
    if (exp_gen != (uint64_t)ww->sched.layout_generation) {
        fail(f, SU_E_SCHEMA_MISMATCH, "layout generation differs from the installed system");
        return SU_E_SCHEMA_MISMATCH;
    }
    st = spec_caps(backend, f, &caps, &tier_auto);
    if (st != SU_OK) {
        return st;
    }
    st = spec_prepare(ww->sched.sys[j].mod, ww->sched.layout_generation, tier_auto, &caps, &seq2, fallback,
                      sizeof fallback, msg, sizeof msg);
    if (st != SU_OK) {
        fail(f, SU_E_CANDIDATE_FAILED, msg);
        return SU_E_CANDIDATE_FAILED;
    }
    if (su_nv_artifact_hash(seq2, hash2) != SU_OK) {
        su_nv_release(seq2);
        fail(f, SU_E_CANDIDATE_FAILED, "recompile produced no artifact identity");
        return SU_E_CANDIDATE_FAILED;
    }
#ifdef SU_TEST_FAULT_HOOKS
    if (SPEC_FAULT_PERTURB()) {
        hash2[0] = hash2[0] == '0' ? '1' : '0';
    }
#endif
    if (strcmp(hash2, admitted) != 0) {
        su_nv_release(seq2);
        fail(f, SU_E_CANDIDATE_FAILED,
             "artifact_hash differs from the admitted kernel: not installed (stale or non-deterministic build)");
        return SU_E_CANDIDATE_FAILED;
    }
    {
        const char *trust_arg = admitted;
#ifdef SU_TEST_FAULT_HOOKS
        char trust_hash[65];
        if (spec_fault_perturb_trust()) {
            memcpy(trust_hash, admitted, sizeof trust_hash);
            trust_hash[0] = trust_hash[0] == '0' ? '1' : '0';
            trust_arg = trust_hash;
        }
#endif
        if (su_nv_trust_admitted(seq2, trust_arg) != SU_OK || su_nv_install(seq2) != SU_OK) {
            su_nv_release(seq2);
            fail(f, SU_E_CANDIDATE_FAILED, "recompiled artifact could not be marked admitted");
            return SU_E_CANDIDATE_FAILED;
        }
    }
    /* Publish between requests: the schedule entry switches; the previous kernel is released at once
     * (no tick is in flight in the serial worker). The world revision is not changed. */
    old_seq = ww->sched.sys[j].native_seq;
    ww->sched.sys[j].native_seq = seq2;
    if (old_seq != 0u) {
        su_nv_release(old_seq);
    }
    (void)su_nv_backend_id(seq2, &backend_id);
    tier = backend_id == SU_NATIVE_BACKEND_ASMJIT_AVX2 ? "native_avx2" : "native_scalar";

    su_buf_str(res, "{\"world_id\":");
    su_buf_quoted(res, ww->id, strlen(ww->id));
    su_buf_str(res, ",\"specialization_id\":\"spec-");
    su_buf_u64(res, seq2);
    su_buf_str(res, "\",\"state\":\"installed\",\"system_id\":");
    su_buf_quoted(res, system_id, strlen(system_id));
    su_buf_str(res, ",\"backend\":");
    if (caps == SU_NCAP_X86_64_AVX2) {
        su_buf_quoted(res, "avx2", 4u);
    } else {
        su_buf_quoted(res, "scalar", 6u);
    }
    su_buf_str(res, ",\"requested_backend\":");
    su_buf_quoted(res, backend, strlen(backend));
    su_buf_str(res, ",\"execution_tier\":");
    su_buf_quoted(res, tier, strlen(tier));
    su_buf_str(res, ",\"artifact_hash\":");
    su_buf_quoted(res, hash2, strlen(hash2));
    su_buf_str(res, ",\"semantic_hash\":");
    su_buf_quoted(res, ww->sched.sys[j].semantic_hash, strlen(ww->sched.sys[j].semantic_hash));
    su_buf_str(res, ",\"fallback\":");
    if (fallback[0] != '\0') {
        su_buf_quoted(res, fallback, strlen(fallback));
    } else {
        su_buf_str(res, "null");
    }
    su_buf_str(res, ",\"source\":{\"kind\":\"disposable_worker\",\"admission\":{\"cases\":");
    su_buf_u64(res, adm_cases);
    su_buf_str(res, ",\"rows_checked\":");
    su_buf_u64(res, adm_rows);
    su_buf_str(res, ",\"mismatches\":0},\"recompile_hash_equal\":true}");
    su_buf_str(res, ",\"world_revision\":");
    su_buf_quoted(res, rev, strlen(rev));
    su_buf_str(res, ",\"tick\":");
    su_buf_u64(res, su_world_tick(ww->inst.w));
    su_buf_str(res, ",\"worker_pid\":");
    su_buf_u64(res, (uint64_t)getpid());
    su_buf_str(res, "}");
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}
#endif /* SU_HAVE_NATIVE */

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
    } else if (strcmp(req.method, "world.spawn") == 0) {
        st = h_world_spawn(s, req.params, &res, &f);
    } else if (strcmp(req.method, "world.despawn") == 0) {
        st = h_world_despawn(s, req.params, &res, &f);
    } else if (strcmp(req.method, "events.read") == 0) {
        st = h_events_read(s, req.params, &res, &f);
    } else if (strcmp(req.method, "internal.candidate.evaluate") == 0) {
        st = h_candidate_evaluate(s, req.params, &res, &f);
    } else if (strcmp(req.method, "internal.candidate.install") == 0) {
        st = h_candidate_install(s, req.params, &res, &f);
    } else if (strcmp(req.method, "world.snapshot") == 0) {
        st = h_world_snapshot(s, req.params, &res, &f);
    } else if (strcmp(req.method, "world.restore") == 0) {
        st = h_world_restore(s, req.params, &res, &f);
    } else if (strcmp(req.method, "world.close") == 0) {
        st = h_world_close(s, req.params, &res, &f);
#ifdef SU_HAVE_NATIVE
    } else if (strcmp(req.method, "internal.specialize.identity") == 0) {
        st = h_internal_specialize_identity(s, req.params, &res, &f);
    } else if (strcmp(req.method, "internal.specialize.admit") == 0) {
        st = h_internal_specialize_admit(s, req.params, &res, &f);
    } else if (strcmp(req.method, "internal.specialize.install") == 0) {
        st = h_internal_specialize_install(s, req.params, &res, &f);
#endif
#ifdef SU_TEST_FAULT_HOOKS
    } else if (strcmp(req.method, "internal.test.containment") == 0) {
        st = h_internal_test_containment(&res);
        if (st != SU_OK) {
            fail(&f, st, "containment probe failed");
        }
    } else if (strcmp(req.method, "internal.test.sandbox") == 0) {
        st = h_internal_test_sandbox(&res);
    } else if (strcmp(req.method, "internal.test.fs") == 0) {
        st = h_internal_test_fs(&res);
    } else if (strcmp(req.method, "internal.test.i386_socket") == 0) {
        st = h_internal_test_i386_socket(&res);
    } else if (strcmp(req.method, "internal.test.x32_socket") == 0) {
        st = h_internal_test_x32_socket(&res);
#endif
    } else if (strcmp(req.method, "internal.stats") == 0) {
        st = h_internal_stats(s, &res);
    } else if (strcmp(req.method, "internal.world.seed") == 0) {
        st = h_internal_world_seed(s, req.params, &res, &f);
    } else if (strcmp(req.method, "internal.world.capture") == 0) {
        st = h_internal_world_capture(s, req.params, &res, &f);
    } else if (strcmp(req.method, "internal.world.digest") == 0) {
        st = h_internal_world_digest(s, req.params, &res, &f);
    } else if (strcmp(req.method, "internal.world.restore_bytes") == 0) {
        st = h_internal_world_restore_bytes(s, req.params, &res, &f);
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
    su_sandbox_worker_install(&s->sandbox);
    if (hooks != NULL) {
        s->hooks = *hooks;
    }
#ifdef SU_TEST_FAULT_HOOKS
    test_kill_configure(s);
#ifdef SU_HAVE_NATIVE
    test_native_crash_configure();
#endif
#endif
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
