#define _POSIX_C_SOURCE 200809L

#include "agent/su_agent.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef SU_HAVE_NATIVE
#include "backends/native/su_native.h"
#endif
#include "agent/su_module.h"
#include "agent/su_path.h"
#include "agent/su_proc.h"
#include "agent/su_proto.h"
#include "agent/su_spool.h"
#include "ir/su_json.h"
#include "ir/su_lir.h"
#include "ir/su_sha256.h"
#include "sutekh/abi.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "world/entities/su_cmdq.h"

#ifndef SUTEKH_VERSION_STRING
#define SUTEKH_VERSION_STRING "0.1.0"
#endif

#define AGENT_MAX_CANDIDATES 16u
#define AGENT_DEFAULT_TIMEOUT_MS 10000u
#define AGENT_MIN_TIMEOUT_MS 50u
#define AGENT_MAX_TIMEOUT_MS 60000u
#define AGENT_CAPSULE_MAX (64u * 1024u)
#define AGENT_TEST_MAX (256u * 1024u)
#define AGENT_MAX_TICKS 100000u
#define AGENT_MSG_CAP 640u
#define AGENT_DETAIL_CAP 2048u
#define AGENT_MAX_PARAM_STRING 255u
#define AGENT_MAX_FIELD_ARGS 64u
#define AGENT_MAX_SYSTEMS 8u
#define AGENT_MAX_OFFSET (UINT64_C(1) << 40)
/* Canonical values object of one world.spawn kept in the replay log (bytes, NUL-terminated). */
#define AGENT_SPAWN_VALUES_CAP 1024u
/* Limits of the entity-world configuration accepted by world.create (contract r2 section 5). */
#define AGENT_MAX_ENTITIES 1000000u
#define AGENT_MAX_TICK_CAP 1048576u
#define AGENT_MAX_LOG_CAP 1048576u
#define AGENT_EVENTS_MAX 256u
#define AGENT_MAX_WORLDS 4u
#define AGENT_WORLD_ID_CAP 16u
#define AGENT_REV_CAP 48u
#define AGENT_REASON_CAP 200u
#define AGENT_DEFAULT_INTERVAL 64u
#define AGENT_DEFAULT_RETAIN 4u
#define AGENT_MAX_RETAIN 8u
#define AGENT_DEFAULT_LOG 256u
#define AGENT_MAX_LOG 1024u
#define AGENT_DEFAULT_QUARANTINE 2u
#define AGENT_DEFAULT_SPOOL (UINT64_C(32) << 20)
#define AGENT_DEFAULT_CHUNK_MIN_ROWS UINT64_C(4096)
#define AGENT_MAX_TICK_WORKERS 64u
#define AGENT_FIXTURE_MAX (UINT64_C(64) << 20)
#define AGENT_RECOVERY_ATTEMPTS 3u
/* M1-02c-2 limits (docs/AGENT_RPC.md section 10). */
#define AGENT_MAX_QUARANTINE 16u /* quarantined artifacts; a full table refuses native specialization */
#define AGENT_MAX_RESPEC 16u     /* pending respecialization requests */
#define AGENT_MAX_PLANS 4u       /* M2-01b: migration plans kept per world (each names spooled documents) */
#define AGENT_MIG_MEMORY_BUDGET (UINT64_C(1) << 30) /* memory admission for world.migrate_plan (estimate) */
#define AGENT_RESPEC_RETRIES 1u  /* extra attempts after SU_E_WORKER_FAILED (refusals are not retried) */
#define AGENT_INTERNAL_ID "internal"

/* ---------- supervisor records (M1-02) ----------
 *
 * Everything recovery needs is held here, never in the worker: the creation parameters of a
 * world, the revision of each system (by content hash), canonical checkpoints (their bytes live in
 * the spool, keyed by sha256), and the ordered log of accepted operations since the newest
 * checkpoint. Revisions are the worker's world revisions; they are replayed exactly. */

/* Project paths of one capsule and its source (candidate and creation revisions). */
typedef struct PathPair {
    char capsule[SU_PATH_BUF];
    char source[SU_PATH_BUF];
} PathPair;

/* One system's installed revision. Hashes name spooled bytes. */
typedef struct SysRef {
    char system_id[SU_DIAG_SYSTEM_BYTES];
    char capsule_sha[65];
    char source_sha[65];
    char semantic_hash[65];
    uint64_t system_rev;
} SysRef;

/* A canonical checkpoint: the world's state at (rev, tick), its digest, and the schedule it was
 * captured with. The snapshot bytes are the spool entry <sha>.snap. */
typedef struct Ckpt {
    uint64_t rev;
    uint64_t tick;
    char sha[65];
    uint64_t bytes;
    uint32_t count;
    int client;            /* taken by world.snapshot (counted by world.close as a snapshot) */
    int migrated;          /* the world had a migration or rollback when this was taken: recovery uses the snapshot */
    SysRef sys[AGENT_MAX_SYSTEMS];
} Ckpt;

enum { LOG_NONE = 0, LOG_STEP = 1, LOG_INSTALL = 2, LOG_SPAWN = 3, LOG_DESPAWN = 4, LOG_MIGRATE = 5,
       LOG_ROLLBACK = 6 };

/* M2-01b: a schema migration (LOG_MIGRATE) or a rollback (LOG_ROLLBACK). Documents and replacement
 * capsules are spooled by content hash; a rollback names the pinned pre-migration checkpoint it restored. */
#define AGENT_MIG_ID_CAP 24u
typedef struct MigOp {
    char mig_sha[65];          /* migration document (spool ext "migration") */
    char tr_sha[65];           /* transform document (ext "transform"), when has_tr */
    char lir_sha[65];          /* transform IR (ext "lir"), when has_lir */
    char plan_hash[65];
    uint32_t has_tr;
    uint32_t has_lir;
    uint32_t to_gen;
    uint64_t spool_bytes;      /* admission budget the plan was made with */
    uint64_t memory_bytes;
    uint32_t n;                /* replacement systems, in schedule order (sys[] of the committed world) */
    SysRef sys[AGENT_MAX_SYSTEMS];
    uint32_t nd;
    char drop[AGENT_MAX_SYSTEMS][SU_DIAG_SYSTEM_BYTES];
    Ckpt pin;                  /* LOG_ROLLBACK: the restored pinned checkpoint */
} MigOp;

/* One accepted mutating operation. hash_after is the canonical digest after it. */
typedef struct LogOp {
    int kind;
    uint64_t ticks;        /* step: ticks the operation committed (partial steps included) */
    uint64_t rev_after;
    uint64_t tick_after;
    char hash_after[65];
    char cand_id[32];      /* install */
    SysRef sys;            /* install: the revision it made live */
    uint64_t effective_tick;
    /* spawn: the canonical values object text (u32 field values) and the optional external id;
     * despawn: the entity handle. Replayed verbatim (M2-00c3b). */
    char spawn_values[AGENT_SPAWN_VALUES_CAP];
    uint32_t has_ext;
    uint32_t ext_id;
    uint64_t handle;
    MigOp mig;             /* migration and rollback */
} LogOp;

/* A migration plan returned by world.migrate_plan. The plan is bound to the world revision it was made at;
 * the documents are in the spool under their hashes. */
typedef struct PlanRec {
    int used;
    char id[AGENT_MIG_ID_CAP];
    char plan_hash[65];
    uint64_t rev;
    uint64_t tick;
    uint32_t valid;
    uint32_t from_gen;
    uint32_t to_gen;
    uint64_t plan_ns;          /* time the worker spent on world.migrate_plan (reported with the commit) */
    uint64_t bytes_new_columns;
    uint64_t bytes_peak;
    MigOp doc;             /* documents and budget (sys, pin and log fields unused) */
} PlanRec;

/* A native kernel installed in the active worker for one system (not canonical state). */
typedef struct NatRec {
    int used;
    char system_id[SU_DIAG_SYSTEM_BYTES];
    char backend[8];          /* requested: scalar | avx2 | auto */
    char artifact_hash[65];
    char tier[16];            /* native_scalar | native_avx2 */
} NatRec;

typedef struct AgentWorld {
    int used;
    int pending;           /* being created: not yet visible to recovery */
    int unrecoverable;
    char reason[AGENT_REASON_CAP];
    char id[AGENT_WORLD_ID_CAP];
    uint64_t seq;          /* N of world-N */
    uint64_t rev;          /* acknowledged worker revision */
    uint64_t tick;         /* acknowledged tick */
    char hash[65];         /* acknowledged canonical digest */
    char fixture_path[SU_PATH_BUF];
    char fixture_sha[65];
    uint32_t creation_count;
    /* Entity configuration as given to world.create (replayed on recreation): max_entities,
     * max_struct_per_tick, max_events_per_tick, event_log_max. */
    uint64_t cfg[4];
    SysRef creation[AGENT_MAX_SYSTEMS];
    PathPair creation_paths[AGENT_MAX_SYSTEMS];
    uint32_t count;
    SysRef sys[AGENT_MAX_SYSTEMS];
    uint32_t ck_count;
    Ckpt ck[AGENT_MAX_RETAIN]; /* ck[0] oldest, ck[ck_count-1] newest */
    uint64_t reserved;     /* spool bytes reserved for this world's checkpoints */
    /* M2-01b: the pinned pre-migration checkpoint (rollback target), sticky "migrated" flag, plans. */
    int pin_has;
    Ckpt pin;
    char pin_mig[65];      /* migration hash of the commit that pinned pin (reported by rollback) */
    int migrated;
    uint32_t next_plan;
    PlanRec plan[AGENT_MAX_PLANS];
    LogOp *log;
    uint32_t log_count;
    uint32_t log_cap;
    uint64_t ticks_since_ck;
    char crash_key[160];   /* the operation the last crashes were attributed to */
    uint32_t crash_repeat;
    NatRec nat[AGENT_MAX_SYSTEMS];   /* native kernels installed in the active worker */
    char respec_state[16];           /* last outcome: none | pending | respecialized | failed */
    char respec_reason[AGENT_REASON_CAP];
} AgentWorld;

/* A quarantined native artifact, keyed by artifact_hash. */
typedef struct QRec {
    int used;
    char artifact_hash[65];
    char world_id[AGENT_WORLD_ID_CAP];
    char system_id[SU_DIAG_SYSTEM_BYTES];
    char backend[8];
    char tier[16];
    char reason[AGENT_REASON_CAP];
    uint64_t crashes;      /* worker deaths attributed to this artifact */
    uint64_t replaced_pid; /* pid of the last worker that died running it */
    uint64_t tick;         /* tick being computed by the last attributed crash */
} QRec;

/* A pending respecialization (a non-quarantined kernel that died with its worker). */
typedef struct RespecRec {
    int used;
    char world_id[AGENT_WORLD_ID_CAP];
    char system_id[SU_DIAG_SYSTEM_BYTES];
    char backend[8];
    uint32_t attempts;
} RespecRec;

typedef struct AgentCandidate {
    int used;
    char id[32];
    char system_id[SU_DIAG_SYSTEM_BYTES];
    char capsule_real[SU_PATH_BUF];
    char source_real[SU_PATH_BUF];
    char source_hash[65];
    char capsule_hash[65];
    char semantic_hash[65];
    char eval_status[16]; /* not_run | passed | failed | worker_failed */
    uint64_t eval_total;
    uint64_t eval_passed;
    uint64_t worker_pid;      /* pid of the last worker that evaluated or installed it; 0 = none */
    SuProcContainment eval_contain; /* containment of the last disposable evaluation worker */
    char install_state[16];   /* not_attempted | installed | rejected | quarantined */
    uint64_t install_txid;
    uint64_t install_tick;    /* effective tick of the last installation */
    uint64_t install_code;    /* SU_E_* status of the last rejection */
    char quarantine_reason[AGENT_REASON_CAP];
    uint64_t seq;             /* creation order; the oldest record is evicted when the table is full */
} AgentCandidate;

typedef struct AgentSystemPaths {
    char capsule[SU_PATH_BUF];
    char source[SU_PATH_BUF];
} AgentSystemPaths;

typedef struct SuAgent {
    SuAgentConfig cfg;
    char root[SU_PATH_BUF];
    uint32_t timeout_ms;
    SuProc worker; /* the active worker; holds the worlds */
    uint64_t last_pid;      /* pid of the most recent active worker, for recovery reports */
    uint64_t next_candidate;
    uint64_t next_eval;
    AgentCandidate cand[AGENT_MAX_CANDIDATES];
    AgentWorld world[AGENT_MAX_WORLDS];
    uint64_t next_world_seq; /* highest world number ever issued: seeds replacement workers */
    uint64_t generation;     /* worker_generation: active worker processes started */
    uint64_t recoveries;     /* completed worker replacements */
    SuSpool *spool;
    uint64_t spool_max;
    uint32_t tick_workers;
    uint64_t tick_chunk_min_rows;
    uint32_t interval;
    uint32_t retain;
    uint32_t log_cap;
    uint32_t quarantine_after;
    int install_ck;
    SuBuf rec;       /* recovery object of the request being answered */
    SuBuf event;     /* recovery object not yet reported on a response */
    int event_pending;
    QRec quar[AGENT_MAX_QUARANTINE];
    int quar_overflow;      /* a crash could not be recorded: native specialization is refused */
    RespecRec respec[AGENT_MAX_RESPEC];
    uint32_t respec_count;  /* FIFO: respec[0] is served first */
    SuBuf nat_rec;          /* native member of the recovery object being built (nat_has = 1) */
    int nat_has;
    int service_respec;     /* serve loop: run the respecialization queue after this reply */
} SuAgent;

typedef struct AgentCall {
    const char *id; /* the client request id; always set before routing */
    char msg[AGENT_MSG_CAP];
    char detail[AGENT_DETAIL_CAP]; /* optional error.detail: a complete JSON object, or empty */
    int relay; /* res holds one complete worker response line, to be sent as-is */
    int has_rec; /* the response reports a recovery (top-level "recovery" member) */
} AgentCall;

/* What the worker was doing when it died, for crash bookkeeping and quarantine. */
enum { OPK_READ = 0, OPK_STEP, OPK_INSTALL, OPK_CREATE, OPK_RESTORE, OPK_CLOSE, OPK_SPAWN, OPK_DESPAWN,
       OPK_MIGRATE, OPK_ROLLBACK };
typedef struct InFlight {
    int kind;
    AgentWorld *w;     /* NULL for reads and creation */
    uint64_t tick;     /* acknowledged tick when the operation started */
    char cand[32];     /* install: candidate id */
} InFlight;

/* Exactly the methods routed below. The other AGENT_RPC methods are not implemented and
 * must not be advertised. */
static const char *const k_capabilities[] = {
    "engine.info",   "capsule.submit", "candidate.status", "candidate.evaluate",
    "candidate.install", "world.create", "world.inspect", "world.step",
    "world.snapshot", "world.restore", "world.close", "world.spawn", "world.despawn", "events.read",
    "world.migrate_plan", "world.migrate_commit", "world.migrate_rollback",
#ifdef SU_HAVE_NATIVE
    "world.specialize",
#endif
};

/* Tick options appended to every world.step request the supervisor sends to a worker. */
static void append_tick_opts(SuBuf *b, const SuAgent *a) {
    su_buf_str(b, ",\"tick_workers\":");
    su_buf_u64(b, a->tick_workers);
    su_buf_str(b, ",\"chunk_min_rows\":");
    su_buf_u64(b, a->tick_chunk_min_rows);
}

static uint32_t call_fail(AgentCall *c, uint32_t code, const char *msg) {
    snprintf(c->msg, sizeof c->msg, "%s", msg);
    return code;
}

/* ---------- parameter helpers ---------- */

static int key_in(const SuJsonMember *m, const char *const *allowed, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        size_t l = strlen(allowed[i]);
        if (m->key_len == l && memcmp(m->key, allowed[i], l) == 0) {
            return 1;
        }
    }
    return 0;
}

static uint32_t check_keys(const SuJsonValue *params, const char *const *allowed, size_t n,
                           AgentCall *c) {
    size_t i;
    for (i = 0; i < params->u.object.count; i++) {
        if (!key_in(&params->u.object.members[i], allowed, n)) {
            return call_fail(c, SU_E_TYPE, "unexpected parameter for this method");
        }
    }
    return SU_OK;
}

static uint32_t param_str(const SuJsonValue *p, const char *key, int required, const char **out,
                          AgentCall *c) {
    const SuJsonValue *v = su_json_member(p, key);
    *out = NULL;
    if (v == NULL) {
        return required ? call_fail(c, SU_E_TYPE, "missing required string parameter")
                        : SU_OK;
    }
    if (!su_json_is_string(v) || v->u.string.len == 0 || v->u.string.len > AGENT_MAX_PARAM_STRING) {
        return call_fail(c, SU_E_TYPE, "string parameter must be 1..255 bytes");
    }
    *out = v->u.string.data;
    return SU_OK;
}

static uint32_t param_uint(const SuJsonValue *p, const char *key, int required, uint64_t lo,
                           uint64_t hi, uint64_t def, uint64_t *out, AgentCall *c) {
    const SuJsonValue *v = su_json_member(p, key);
    *out = def;
    if (v == NULL) {
        return required ? call_fail(c, SU_E_TYPE, "missing required integer parameter") : SU_OK;
    }
    if (!su_json_get_uint(v, out) || *out < lo || *out > hi) {
        return call_fail(c, SU_E_TYPE, "integer parameter out of range");
    }
    return SU_OK;
}

/* ---------- reply helpers (worker responses) ---------- */

typedef struct Reply {
    SuJsonDoc doc;
    int parsed;
    int has_ok;
    int ok;
    const SuJsonValue *result;
    uint64_t code;
    char msg[AGENT_MSG_CAP];
} Reply;

static const SuJsonValue *jf(const SuJsonValue *o, const char *k) {
    return o != NULL && o->type == SU_JSON_OBJECT ? su_json_member(o, k) : NULL;
}

static int jstr(const SuJsonValue *o, const char *k, char *dst, size_t cap) {
    const SuJsonValue *v = jf(o, k);
    if (v == NULL || su_json_copy_string(v, dst, cap) != 0) {
        if (cap > 0) {
            dst[0] = '\0';
        }
        return 0;
    }
    return 1;
}

static int ju64(const SuJsonValue *o, const char *k, uint64_t *out) {
    const SuJsonValue *v = jf(o, k);
    return v != NULL && su_json_get_uint(v, out);
}

static void reply_parse(Reply *r, const uint8_t *data, size_t len) {
    SuJsonLimits lim;
    SuJsonError err;
    const SuJsonValue *okv;
    uint32_t st;

    memset(r, 0, sizeof *r);
    lim.max_bytes = len;
    lim.max_depth = SU_PROTO_MAX_DEPTH + 4u;
    st = su_json_parse(data, len, &lim, &r->doc, &err);
    if (st != SU_OK || r->doc.root == NULL || r->doc.root->type != SU_JSON_OBJECT) {
        return;
    }
    r->parsed = 1;
    okv = su_json_member(r->doc.root, "ok");
    r->has_ok = okv != NULL && okv->type == SU_JSON_BOOL;
    r->ok = r->has_ok && okv->u.boolean;
    if (r->ok) {
        r->result = su_json_member(r->doc.root, "result");
    } else {
        const SuJsonValue *e = jf(r->doc.root, "error");
        r->code = SU_E_INTERNAL;
        ju64(e, "status", &r->code);
        if (!jstr(e, "message", r->msg, sizeof r->msg)) {
            snprintf(r->msg, sizeof r->msg, "worker reported an error");
        }
    }
}

static void reply_free(Reply *r) {
    su_json_doc_free(&r->doc);
    memset(r, 0, sizeof *r);
}

/* ---------- revisions ---------- */

/* Revision number of "<world-id>.r<N>", 0 when malformed. */
static uint64_t rev_num(const char *rev) {
    const char *p = rev != NULL ? strrchr(rev, 'r') : NULL;
    uint64_t v = 0;
    if (p == NULL || p == rev || p[-1] != '.' || p[1] == '\0') {
        return 0;
    }
    for (p++; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        v = v * 10u + (uint64_t)(*p - '0');
    }
    return v;
}

static void wrev(const AgentWorld *w, uint64_t rev, char *out, size_t cap) {
    snprintf(out, cap, "%s.r%llu", w->id, (unsigned long long)rev);
}

/* Parses "world-N" into N; 0 if malformed. */
static uint64_t wid_num(const char *id) {
    uint64_t v = 0;
    size_t i;
    if (strncmp(id, "world-", 6) != 0 || id[6] == '\0' || id[6] == '0' || strlen(id + 6) > 9) {
        return 0;
    }
    for (i = 6; id[i] != '\0'; i++) {
        if (id[i] < '0' || id[i] > '9') {
            return 0;
        }
        v = v * 10u + (uint64_t)(id[i] - '0');
    }
    return v;
}

/* ---------- worker process and round trips ---------- */

/* Containment defaults (constitution C07, docs/AGENT_RPC.md section 12). 0 in SuAgentConfig selects them. */
#define AGENT_DEFAULT_WORKER_FILE_BYTES (UINT64_C(256) * 1024u * 1024u)
#define AGENT_DEFAULT_WORKER_NOFILE 1024u
#define AGENT_DEFAULT_DISPOSABLE_NOFILE 64u

/* Limits for a child of the supervisor. Every child gets the address-space bound, the file-size
 * bound and network denial. The active worker has no CPU bound (it runs long simulations) and 1024
 * open files; a disposable worker has a CPU bound (the caller sets it) and 64 open files. */
static void child_limits(const SuAgent *a, SuProcLimits *lim, int disposable) {
    memset(lim, 0, sizeof *lim);
    lim->address_space_bytes = a->cfg.worker_address_space_bytes;
    lim->file_bytes = a->cfg.worker_file_bytes == 0 ? AGENT_DEFAULT_WORKER_FILE_BYTES : a->cfg.worker_file_bytes;
    if (disposable) {
        lim->nofile = a->cfg.disposable_nofile == 0 ? AGENT_DEFAULT_DISPOSABLE_NOFILE : a->cfg.disposable_nofile;
    } else {
        lim->nofile = a->cfg.worker_nofile == 0 ? AGENT_DEFAULT_WORKER_NOFILE : a->cfg.worker_nofile;
    }
    lim->deny_network = 1;
    /* Only disposable children are read-only (they compute and return; they keep no state on disk). The
     * active worker keeps its ordinary write access. */
    lim->deny_fs_writes = disposable ? 1 : 0;
}

static void put_limit(SuBuf *b, int reported, uint64_t v) {
    if (!reported) {
        su_buf_str(b, "null");
    } else if (v == SU_PROC_UNLIMITED) {
        su_buf_str(b, "\"unlimited\"");
    } else {
        su_buf_u64(b, v);
    }
}

/* JSON object for a child's containment record: the enforced soft limits and the network mode. Limits
 * the child did not report are null, never a guessed value. */
static void put_containment(SuBuf *b, const SuProcContainment *c) {
    static const char *const net_names[] = {"none", "seccomp", "netns"};
    uint32_t mode = c->reported && c->net_mode <= SU_PROC_NET_NETNS ? c->net_mode : SU_PROC_NET_NONE;
    su_buf_str(b, "{\"reported\":");
    su_buf_str(b, c->reported ? "true" : "false");
    su_buf_str(b, ",\"rlimit_as\":");
    put_limit(b, c->reported, c->rlimit_as);
    su_buf_str(b, ",\"rlimit_cpu\":");
    put_limit(b, c->reported, c->rlimit_cpu);
    su_buf_str(b, ",\"rlimit_fsize\":");
    put_limit(b, c->reported, c->rlimit_fsize);
    su_buf_str(b, ",\"rlimit_nofile\":");
    put_limit(b, c->reported, c->rlimit_nofile);
    su_buf_str(b, ",\"no_network\":\"");
    su_buf_str(b, net_names[mode]);
    su_buf_str(b, "\",\"fs_write_denied\":\"");
    su_buf_str(b, c->reported && c->fs_mode == SU_PROC_FS_LANDLOCK ? "landlock" : "none");
    su_buf_str(b, "\",\"fs_abi\":");
    su_buf_str(b, c->reported && c->fs_mode == SU_PROC_FS_LANDLOCK ? "" : "null");
    if (c->reported && c->fs_mode == SU_PROC_FS_LANDLOCK) {
        char abi[16];
        snprintf(abi, sizeof abi, "%d", (int)c->fs_abi);
        su_buf_str(b, abi);
    }
    su_buf_str(b, ",\"fs_reason\":");
    su_buf_quoted(b, c->fs_reason, strlen(c->fs_reason));
    su_buf_str(b, ",\"reason\":");
    su_buf_quoted(b, c->reason, strlen(c->reason));
    su_buf_str(b, "}");
}

static uint32_t ensure_worker(SuAgent *a) {
    SuProcLimits lim;
    if (a->worker.alive) {
        return SU_OK;
    }
    if (a->cfg.worker_argv == NULL) {
        return SU_E_WORKER_FAILED;
    }
    child_limits(a, &lim, 0);
    if (su_proc_spawn(&a->worker, a->cfg.worker_argv, &lim) != SU_OK) {
        return SU_E_WORKER_FAILED;
    }
    a->last_pid = (uint64_t)a->worker.pid;
    a->generation++;
    return SU_OK;
}

/* One request to the active worker. The response must be an envelope with the same id and a
 * boolean ok. Any transport failure, timeout or malformed response kills the worker and returns
 * SU_E_WORKER_FAILED (or SU_E_RESOURCE_LIMIT for an oversized response); the caller then recovers. */
static uint32_t roundtrip(SuAgent *a, const char *id, const SuBuf *req, SuBuf *resp) {
    const uint8_t *line = NULL;
    size_t len = 0;
    int64_t deadline;
    uint32_t st;
    Reply r;
    int idok;

    st = ensure_worker(a);
    if (st != SU_OK) {
        return st;
    }
    deadline = su_now_ms() + (int64_t)a->timeout_ms;
    st = su_proc_request(&a->worker, req->data, req->len, deadline, &line, &len);
    if (st != SU_OK) {
        return st == SU_E_RESOURCE_LIMIT ? st : SU_E_WORKER_FAILED;
    }
    su_buf_put(resp, line, len);
    if (resp->failed) {
        su_proc_kill(&a->worker);
        return SU_E_WORKER_FAILED;
    }
    reply_parse(&r, resp->data, resp->len);
    idok = r.parsed && su_json_is_string(jf(r.doc.root, "id")) &&
           jf(r.doc.root, "id")->u.string.len == strlen(id) &&
           memcmp(jf(r.doc.root, "id")->u.string.data, id, strlen(id)) == 0;
    if (!idok || !r.has_ok) {
        reply_free(&r);
        su_proc_kill(&a->worker);
        return SU_E_WORKER_FAILED;
    }
    reply_free(&r);
    return SU_OK;
}

static void req_begin(SuBuf *req, const char *id, const char *method) {
    su_buf_init(req);
    su_buf_str(req, "{\"protocol\":\"" SU_PROTOCOL_VERSION "\",\"id\":");
    su_buf_quoted(req, id, strlen(id));
    su_buf_str(req, ",\"method\":\"");
    su_buf_str(req, method);
    su_buf_str(req, "\",\"params\":{");
}

static void req_end(SuBuf *req) {
    su_buf_str(req, "}}");
}

static void buf_hash_field(SuBuf *b, const char *key, const char *hex) {
    su_buf_str(b, ",\"");
    su_buf_str(b, key);
    su_buf_str(b, "\":");
    su_buf_quoted(b, hex, strlen(hex));
}

static AgentCandidate *candidate_by_capsule(SuAgent *a, const char *sha);

/* ---------- spool: admission, references, reclamation ---------- */

/* M2-01b: 1 when the pinned checkpoint p names the spooled file (sha, ext). */
static int migop_refs_pin(const Ckpt *p, const char *sha, const char *ext) {
    uint32_t j;
    if (strcmp(ext, "snap") == 0) {
        return strcmp(p->sha, sha) == 0;
    }
    for (j = 0; j < p->count; j++) {
        if (strcmp(strcmp(ext, "capsule") == 0 ? p->sys[j].capsule_sha : p->sys[j].source_sha, sha) == 0) {
            return 1;
        }
    }
    return 0;
}

/* M2-01b: 1 when the migration record m names the spooled file (sha, ext). */
static int migop_refs(const MigOp *m, const char *sha, const char *ext) {
    uint32_t j;
    if (strcmp(ext, "migration") == 0) {
        return strcmp(m->mig_sha, sha) == 0;
    }
    if (strcmp(ext, "transform") == 0) {
        return m->has_tr && strcmp(m->tr_sha, sha) == 0;
    }
    if (strcmp(ext, "lir") == 0) {
        return m->has_lir && strcmp(m->lir_sha, sha) == 0;
    }
    if (strcmp(ext, "snap") == 0) {
        return m->pin.sha[0] != '\0' && strcmp(m->pin.sha, sha) == 0;
    }
    for (j = 0; j < m->n; j++) {
        if (strcmp(strcmp(ext, "capsule") == 0 ? m->sys[j].capsule_sha : m->sys[j].source_sha, sha) == 0) {
            return 1;
        }
    }
    for (j = 0; j < m->pin.count; j++) {
        if (strcmp(strcmp(ext, "capsule") == 0 ? m->pin.sys[j].capsule_sha : m->pin.sys[j].source_sha, sha) == 0) {
            return 1;
        }
    }
    return 0;
}

static int spool_referenced(const SuAgent *a, const char *sha, const char *ext) {
    uint32_t i;
    uint32_t k;
    uint32_t j;
    int is_snap = strcmp(ext, "snap") == 0;
    int is_cap = strcmp(ext, "capsule") == 0;

    for (i = 0; i < AGENT_MAX_WORLDS; i++) {
        const AgentWorld *w = &a->world[i];
        if (!w->used) {
            continue;
        }
        if (w->pin_has && migop_refs_pin(&w->pin, sha, ext)) {
            return 1;
        }
        for (k = 0; k < AGENT_MAX_PLANS; k++) {
            if (w->plan[k].used && migop_refs(&w->plan[k].doc, sha, ext)) {
                return 1;
            }
        }
        for (k = 0; k < w->log_count; k++) {
            if (migop_refs(&w->log[k].mig, sha, ext)) {
                return 1;
            }
        }
        for (k = 0; k < w->ck_count; k++) {
            if (is_snap) {
                if (strcmp(w->ck[k].sha, sha) == 0) {
                    return 1;
                }
                continue;
            }
            for (j = 0; j < w->ck[k].count; j++) {
                if (strcmp(is_cap ? w->ck[k].sys[j].capsule_sha : w->ck[k].sys[j].source_sha, sha) == 0) {
                    return 1;
                }
            }
        }
        if (is_snap) {
            continue;
        }
        for (k = 0; k < w->creation_count; k++) {
            if (strcmp(is_cap ? w->creation[k].capsule_sha : w->creation[k].source_sha, sha) == 0) {
                return 1;
            }
        }
        for (k = 0; k < w->count; k++) {
            if (strcmp(is_cap ? w->sys[k].capsule_sha : w->sys[k].source_sha, sha) == 0) {
                return 1;
            }
        }
        for (k = 0; k < w->log_count; k++) {
            if (w->log[k].kind == LOG_INSTALL &&
                strcmp(is_cap ? w->log[k].sys.capsule_sha : w->log[k].sys.source_sha, sha) == 0) {
                return 1;
            }
        }
    }
    if (!is_snap) {
        for (i = 0; i < AGENT_MAX_CANDIDATES; i++) {
            const AgentCandidate *c = &a->cand[i];
            if (c->used && strcmp(is_cap ? c->capsule_hash : c->source_hash, sha) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

/* Drops every spool entry that no record references. */
static void gc_spool(SuAgent *a) {
    int again = 1;
    while (again) {
        uint32_t i;
        uint32_t n = su_spool_count(a->spool);
        again = 0;
        for (i = 0; i < n; i++) {
            const char *ext = NULL;
            uint64_t bytes = 0;
            const char *sha = su_spool_entry(a->spool, i, &ext, &bytes);
            char s[65];
            char e[SU_SPOOL_EXT_MAX + 1u];
            if (sha == NULL) {
                continue;
            }
            snprintf(s, sizeof s, "%s", sha);
            snprintf(e, sizeof e, "%s", ext);
            if (!spool_referenced(a, s, e)) {
                su_spool_drop(a->spool, s, e);
                again = 1;
                break;
            }
        }
    }
}

static uint64_t checkpoint_bytes(const AgentWorld *w) {
    uint64_t t = 0;
    uint32_t k;
    for (k = 0; k < w->ck_count; k++) {
        t += w->ck[k].bytes;
    }
    return t;
}

/* Bytes the spool is committed to: what it holds, plus checkpoint reservations not yet used. */
static uint64_t committed_bytes(const SuAgent *a) {
    uint64_t total = su_spool_bytes(a->spool);
    uint32_t i;
    for (i = 0; i < AGENT_MAX_WORLDS; i++) {
        const AgentWorld *w = &a->world[i];
        if (w->used && w->reserved > checkpoint_bytes(w)) {
            total += w->reserved - checkpoint_bytes(w);
        }
    }
    return total;
}

static int budget_admits(const SuAgent *a, uint64_t add) {
    uint64_t c = committed_bytes(a);
    return c <= a->spool_max && add <= a->spool_max - c;
}

/* Stores bytes in the spool, reclaiming unreferenced entries first when the budget is tight. */
static uint32_t spool_put_admit(SuAgent *a, const char *ext, const uint8_t *data, size_t len, char sha[65]) {
    if (!budget_admits(a, (uint64_t)len)) {
        gc_spool(a);
        if (!budget_admits(a, (uint64_t)len)) {
            return SU_E_RESOURCE_LIMIT;
        }
    }
    return su_spool_put(a->spool, ext, data, len, sha);
}

/* Spool path of a referenced entry, or SU_E_INTERNAL when the record names bytes not present. */
static uint32_t spool_file(const SuAgent *a, const char *sha, const char *ext, char *out, size_t cap) {
    return su_spool_path(a->spool, sha, ext, out, cap) == SU_OK ? SU_OK : SU_E_INTERNAL;
}

/* Writes a JSON array of {system_id, capsule_path, source_path, capsule_sha256, source_sha256,
 * semantic_hash} objects whose paths are spool files. */
static uint32_t write_capsules(const SuAgent *a, SuBuf *req, const char *key, const SysRef *sys, uint32_t n) {
    uint32_t i;
    char cap[SU_PATH_BUF];
    char src[SU_PATH_BUF];
    su_buf_str(req, ",\"");
    su_buf_str(req, key);
    su_buf_str(req, "\":[");
    for (i = 0; i < n; i++) {
        if (spool_file(a, sys[i].capsule_sha, "capsule", cap, sizeof cap) != SU_OK ||
            spool_file(a, sys[i].source_sha, "source", src, sizeof src) != SU_OK) {
            return SU_E_INTERNAL;
        }
        if (i > 0) {
            su_buf_str(req, ",");
        }
        su_buf_str(req, "{\"system_id\":");
        su_buf_quoted(req, sys[i].system_id, strlen(sys[i].system_id));
        su_buf_str(req, ",\"capsule_path\":");
        su_buf_quoted(req, cap, strlen(cap));
        su_buf_str(req, ",\"source_path\":");
        su_buf_quoted(req, src, strlen(src));
        buf_hash_field(req, "capsule_sha256", sys[i].capsule_sha);
        buf_hash_field(req, "source_sha256", sys[i].source_sha);
        buf_hash_field(req, "semantic_hash", sys[i].semantic_hash);
        su_buf_str(req, "}");
    }
    su_buf_str(req, "]");
    return req->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* Like write_capsules, but a revision whose project files are still known to the supervisor is
 * named by those paths, so the worker re-checks the files against the recorded hashes (an explicit
 * world.restore or candidate.install of changed bytes is refused, as in M0). Other revisions use the
 * spool. Recovery never uses this function. */
static uint32_t write_capsules_disk(SuAgent *a, const AgentWorld *w, SuBuf *req, const char *key,
                                    const SysRef *sys, uint32_t n) {
    uint32_t i;
    uint32_t k;
    su_buf_str(req, ",\"");
    su_buf_str(req, key);
    su_buf_str(req, "\":[");
    for (i = 0; i < n; i++) {
        const char *cap = NULL;
        const char *src = NULL;
        char cap_sp[SU_PATH_BUF];
        char src_sp[SU_PATH_BUF];
        AgentCandidate *cd;
        for (k = 0; k < w->creation_count && cap == NULL; k++) {
            if (strcmp(w->creation[k].capsule_sha, sys[i].capsule_sha) == 0 &&
                strcmp(w->creation[k].source_sha, sys[i].source_sha) == 0) {
                cap = w->creation_paths[k].capsule;
                src = w->creation_paths[k].source;
            }
        }
        cd = cap == NULL ? candidate_by_capsule(a, sys[i].capsule_sha) : NULL;
        if (cd != NULL && strcmp(cd->source_hash, sys[i].source_sha) == 0) {
            cap = cd->capsule_real;
            src = cd->source_real;
        }
        if (cap == NULL) {
            if (spool_file(a, sys[i].capsule_sha, "capsule", cap_sp, sizeof cap_sp) != SU_OK ||
                spool_file(a, sys[i].source_sha, "source", src_sp, sizeof src_sp) != SU_OK) {
                return SU_E_INTERNAL;
            }
            cap = cap_sp;
            src = src_sp;
        }
        if (i > 0) {
            su_buf_str(req, ",");
        }
        su_buf_str(req, "{\"system_id\":");
        su_buf_quoted(req, sys[i].system_id, strlen(sys[i].system_id));
        su_buf_str(req, ",\"capsule_path\":");
        su_buf_quoted(req, cap, strlen(cap));
        su_buf_str(req, ",\"source_path\":");
        su_buf_quoted(req, src, strlen(src));
        buf_hash_field(req, "capsule_sha256", sys[i].capsule_sha);
        buf_hash_field(req, "source_sha256", sys[i].source_sha);
        buf_hash_field(req, "semantic_hash", sys[i].semantic_hash);
        su_buf_str(req, "}");
    }
    su_buf_str(req, "]");
    return req->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* ---------- world records ---------- */

static AgentWorld *find_world(SuAgent *a, const char *id) {
    uint32_t i;
    for (i = 0; i < AGENT_MAX_WORLDS; i++) {
        AgentWorld *w = &a->world[i];
        if (w->used && !w->pending && strcmp(w->id, id) == 0) {
            return w;
        }
    }
    return NULL;
}

static AgentWorld *free_world_slot(SuAgent *a) {
    uint32_t i;
    for (i = 0; i < AGENT_MAX_WORLDS; i++) {
        if (!a->world[i].used) {
            return &a->world[i];
        }
    }
    return NULL;
}

static uint32_t live_worlds(const SuAgent *a) {
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < AGENT_MAX_WORLDS; i++) {
        if (a->world[i].used && !a->world[i].pending) {
            n++;
        }
    }
    return n;
}

static void respec_drop_world(SuAgent *a, AgentWorld *w);

static void world_release(SuAgent *a, AgentWorld *w) {
    respec_drop_world(a, w);
    free(w->log);
    w->log = NULL;
    memset(w, 0, sizeof *w);
    gc_spool(a);
}

static void mark_unrecoverable(AgentWorld *w, const char *why) {
    if (!w->unrecoverable) {
        w->unrecoverable = 1;
        snprintf(w->reason, sizeof w->reason, "%s", why);
    }
}

/* Snapshot size bound from the schema shape: 11 bytes per u32 value and per entity id, plus
 * fixed overhead for the header, field names and the manifest. */
static uint64_t snapshot_bound(uint64_t rows, uint64_t fields) {
    uint64_t per_row = (fields + 1u) * 11u;
    uint64_t lim = UINT64_MAX / 4u;
    if (per_row != 0 && rows > (lim - 16384u - fields * 512u - AGENT_MAX_SYSTEMS * 512u) / per_row) {
        return lim;
    }
    return 16384u + fields * 512u + AGENT_MAX_SYSTEMS * 512u + rows * per_row;
}

static void ck_drop_oldest(SuAgent *a, AgentWorld *w) {
    char sha[65];
    snprintf(sha, sizeof sha, "%s", w->ck[0].sha);
    memmove(&w->ck[0], &w->ck[1], (w->ck_count - 1u) * sizeof w->ck[0]);
    w->ck_count--;
    if (!spool_referenced(a, sha, "snap")) {
        su_spool_drop(a->spool, sha, "snap");
    }
}

/* Records a checkpoint at the acknowledged state and clears the log it supersedes. */
static void ck_push(SuAgent *a, AgentWorld *w, uint64_t rev, uint64_t tick, const char *sha, uint64_t bytes) {
    Ckpt *c;
    while (w->ck_count >= a->retain) {
        ck_drop_oldest(a, w);
    }
    c = &w->ck[w->ck_count];
    memset(c, 0, sizeof *c);
    c->rev = rev;
    c->tick = tick;
    snprintf(c->sha, sizeof c->sha, "%s", sha);
    c->bytes = bytes;
    c->count = w->count;
    c->migrated = w->migrated;
    memcpy(c->sys, w->sys, sizeof c->sys);
    w->ck_count++;
    w->log_count = 0;
    w->ticks_since_ck = 0;
}

/* Result codes of the checkpoint and sync helpers. */
enum { CK_OK = 0, CK_DIED = 1, CK_NOROOM = 2, CK_BAD = 3 };

/* Captures the canonical state of w into the spool and records it as a checkpoint. initial is
 * the creation capture: it sets the acknowledged revision, tick and digest and reserves the
 * world's checkpoint bytes. Otherwise the capture must match the acknowledged state. */
static int checkpoint_world(SuAgent *a, AgentWorld *w, int initial) {
    char scratch[SU_PATH_BUF + 64];
    char rev[AGENT_REV_CAP];
    char sha[65];
    char rev_got[AGENT_REV_CAP];
    SuBuf req;
    SuBuf resp;
    Reply r;
    uint64_t tick = 0;
    uint64_t bytes = 0;
    uint64_t rows = 0;
    uint64_t fields = 0;
    uint64_t rv;
    uint32_t st;
    int rc = CK_OK;

    if (su_spool_scratch_path(a->spool, w->id, scratch, sizeof scratch) != SU_OK) {
        return CK_NOROOM;
    }
    unlink(scratch);
    wrev(w, w->rev, rev, sizeof rev);
    req_begin(&req, AGENT_INTERNAL_ID, "internal.world.capture");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, w->id, strlen(w->id));
    su_buf_str(&req, ",\"expected_world_revision\":");
    su_buf_quoted(&req, rev, strlen(rev));
    su_buf_str(&req, ",\"spool_path\":");
    su_buf_quoted(&req, scratch, strlen(scratch));
    req_end(&req);
    su_buf_init(&resp);
    st = roundtrip(a, AGENT_INTERNAL_ID, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        unlink(scratch);
        return CK_DIED;
    }
    reply_parse(&r, resp.data, resp.len);
    if (!r.ok || !jstr(r.result, "world_revision", rev_got, sizeof rev_got) || !ju64(r.result, "tick", &tick) ||
        !ju64(r.result, "bytes", &bytes) || !ju64(r.result, "row_count", &rows) ||
        !ju64(r.result, "field_count", &fields) || !jstr(r.result, "sha256", sha, sizeof sha)) {
        reply_free(&r);
        su_buf_free(&resp);
        unlink(scratch);
        mark_unrecoverable(w, "checkpoint capture was rejected by the worker");
        return CK_BAD;
    }
    reply_free(&r);
    su_buf_free(&resp);
    rv = rev_num(rev_got);
    if (initial) {
        uint64_t est = snapshot_bound(rows, fields);
        if (est > UINT64_MAX / (a->retain + 1u)) {
            unlink(scratch);
            return CK_NOROOM;
        }
        est *= a->retain + 1u;
        if (!budget_admits(a, est)) {
            unlink(scratch);
            return CK_NOROOM;
        }
        w->reserved = est;
        w->rev = rv;
        w->tick = tick;
        snprintf(w->hash, sizeof w->hash, "%s", sha);
    } else if (rv != w->rev || tick != w->tick || strcmp(sha, w->hash) != 0) {
        unlink(scratch);
        mark_unrecoverable(w, "checkpoint capture disagrees with the acknowledged state");
        return CK_BAD;
    }
    if (su_spool_adopt(a->spool, "snap", scratch, sha, bytes) != SU_OK) {
        unlink(scratch);
        return CK_NOROOM;
    }
    ck_push(a, w, rv, tick, sha, bytes);
    return rc;
}

/* Reads the worker's canonical digest of w: its revision, tick and sha256. Returns 0, 1 when
 * the worker died, 2 when the answer is rejected or malformed. */
static int read_digest(SuAgent *a, const AgentWorld *w, char rev_out[AGENT_REV_CAP], uint64_t *tick,
                       char sha_out[65]) {
    SuBuf req;
    SuBuf resp;
    Reply r;
    uint32_t st;
    int rc = 0;

    req_begin(&req, AGENT_INTERNAL_ID, "internal.world.digest");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, w->id, strlen(w->id));
    req_end(&req);
    su_buf_init(&resp);
    st = roundtrip(a, AGENT_INTERNAL_ID, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return 1;
    }
    reply_parse(&r, resp.data, resp.len);
    if (!r.ok || !jstr(r.result, "world_revision", rev_out, AGENT_REV_CAP) || !ju64(r.result, "tick", tick) ||
        !jstr(r.result, "sha256", sha_out, 65)) {
        rc = 2;
    }
    reply_free(&r);
    su_buf_free(&resp);
    return rc;
}

int su_agent_digest_fields_match(const char *rev_got, const char *rev_want, uint64_t tick_got, uint64_t tick_want,
                                 const char *sha_got, const char *sha_want) {
    return strcmp(rev_got, rev_want) == 0 && tick_got == tick_want &&
           (sha_want == NULL || strcmp(sha_got, sha_want) == 0);
}

/* Digest of w compared with the expected revision, tick and sha256 (sha NULL: not compared).
 * Returns 0 when they match, 1 when the worker died, 2 on any mismatch or rejection. */
static int digest_matches(SuAgent *a, const AgentWorld *w, uint64_t rev, uint64_t tick, const char *sha,
                          char *got_sha, size_t got_cap, uint64_t *got_rev, uint64_t *got_tick) {
    char rev_got[AGENT_REV_CAP];
    char got[65];
    char want[AGENT_REV_CAP];
    uint64_t t = 0;
    int rc;

    rc = read_digest(a, w, rev_got, &t, got);
    if (rc != 0) {
        return rc == 1 ? 1 : 2;
    }
    if (got_sha != NULL) {
        snprintf(got_sha, got_cap, "%s", got);
    }
    if (got_rev != NULL) {
        *got_rev = rev_num(rev_got);
    }
    if (got_tick != NULL) {
        *got_tick = t;
    }
    wrev(w, rev, want, sizeof want);
    return su_agent_digest_fields_match(rev_got, want, t, tick, got, sha) ? 0 : 2;
}

/* After an operation the worker acknowledged, or partially applied: digests the committed state
 * and records the change. log_kind LOG_STEP or LOG_INSTALL appends tmpl (its rev/tick/digest are
 * filled in here); force_ck takes a checkpoint now (restore). Checkpoints follow the interval, the
 * install policy and the forced case. ok_flag clears the crash bookkeeping of the world. */
static int sync_after(SuAgent *a, AgentWorld *w, int log_kind, const LogOp *tmpl, int force_ck, int ok_flag) {
    char got[65];
    char rv[AGENT_REV_CAP];
    uint64_t rev_new = 0;
    uint64_t tick_new = 0;
    uint64_t delta = 0;
    int rc;
    LogOp e;

    rc = read_digest(a, w, rv, &tick_new, got);
    if (rc == 1) {
        return CK_DIED;
    }
    if (rc == 2) {
        mark_unrecoverable(w, "digest was rejected by the worker after an accepted operation");
        return CK_BAD;
    }
    rev_new = rev_num(rv);
    if (rev_new == w->rev && tick_new == w->tick && strcmp(got, w->hash) == 0 && !force_ck) {
        if (ok_flag) {
            w->crash_repeat = 0;
            w->crash_key[0] = '\0';
        }
        return CK_OK;
    }
    if (log_kind != LOG_NONE) {
        if (log_kind == LOG_SPAWN || log_kind == LOG_DESPAWN || log_kind == LOG_MIGRATE ||
            log_kind == LOG_ROLLBACK) {
            /* A control-plane mutation: one new revision, the tick unchanged. A rollback is a restore: one new
             * revision, and the tick becomes the pinned checkpoint's (it may be earlier). */
            if ((log_kind != LOG_ROLLBACK && tick_new != w->tick) || rev_new != w->rev + 1u) {
                mark_unrecoverable(w, "entity mutation changed the revision or tick inconsistently");
                return CK_BAD;
            }
        }
        if (log_kind == LOG_STEP) {
            if (tick_new < w->tick || rev_new < w->rev || rev_new - w->rev != tick_new - w->tick) {
                mark_unrecoverable(w, "step changed revision and tick inconsistently");
                return CK_BAD;
            }
            delta = tick_new - w->tick;
        }
        if (w->log_count >= w->log_cap) {
            mark_unrecoverable(w, "replay log is full at an acknowledged operation");
            return CK_BAD;
        }
        e = *tmpl;
        e.kind = log_kind;
        if (log_kind != LOG_MIGRATE && log_kind != LOG_ROLLBACK) {
            memset(&e.mig, 0, sizeof e.mig);
        }
        e.ticks = delta;
        e.rev_after = rev_new;
        e.tick_after = tick_new;
        snprintf(e.hash_after, sizeof e.hash_after, "%s", got);
        w->log[w->log_count++] = e;
    }
    w->rev = rev_new;
    w->tick = tick_new;
    snprintf(w->hash, sizeof w->hash, "%s", got);
    w->ticks_since_ck += delta;
    if (ok_flag) {
        w->crash_repeat = 0;
        w->crash_key[0] = '\0';
    }
    if (force_ck || w->ticks_since_ck >= a->interval || (log_kind == LOG_INSTALL && a->install_ck)) {
        rc = checkpoint_world(a, w, 0);
        if (rc == CK_NOROOM) {
            /* The operation is applied and logged; the log keeps it until a later checkpoint. */
            rc = CK_OK;
        }
        return rc;
    }
    return CK_OK;
}

/* ---------- recovery (M1-02) ----------
 *
 * A replacement worker is started. Each live world is rebuilt from its creation parameters, then
 * its newest checkpoint is restored at the recorded revision (the spool bytes are checked against
 * their sha256 by the worker), then the log is replayed. After every replayed operation the digest
 * must equal the digest recorded when it was acknowledged. Any mismatch marks the world
 * unrecoverable; it is never continued silently. */

typedef struct RecStats {
    uint64_t worlds;
    uint64_t ops;
    int have_base;
    uint64_t min_base_tick;
} RecStats;

static uint32_t seed_worker(SuAgent *a) {
    SuBuf req;
    SuBuf resp;
    Reply r;
    uint32_t st;

    req_begin(&req, AGENT_INTERNAL_ID, "internal.world.seed");
    su_buf_str(&req, "\"next_id\":");
    su_buf_u64(&req, a->next_world_seq);
    req_end(&req);
    su_buf_init(&resp);
    st = roundtrip(a, AGENT_INTERNAL_ID, &req, &resp);
    su_buf_free(&req);
    if (st == SU_OK) {
        reply_parse(&r, resp.data, resp.len);
        if (!r.ok) {
            st = SU_E_WORKER_FAILED;
        }
        reply_free(&r);
    }
    su_buf_free(&resp);
    return st;
}

/* Returns 0 when w is rebuilt and verified, 1 when the replacement worker died meanwhile, 2 when
 * w is now unrecoverable (its world is marked and never retried by recovery). */
/* M2-01b replay of one logged operation. LOG_MIGRATE re-prepares the migration from the spool (documents and
 * replacement capsules) and re-swaps it; LOG_ROLLBACK re-restores the pinned checkpoint. The caller verifies
 * the digest the supervisor acknowledged. Returns 0, 1 (the replacement died) or 2 (unrecoverable). */
static int replay_mig_op(SuAgent *a, AgentWorld *w, const LogOp *op, char *cur) {
    SuBuf req;
    SuBuf resp;
    Reply r;
    char pa[SU_PATH_BUF];
    char ta[SU_PATH_BUF];
    char la[SU_PATH_BUF];
    uint32_t i;
    uint32_t st;

    if (op->kind == LOG_ROLLBACK) {
        char snap[SU_PATH_BUF];
        if (spool_file(a, op->mig.pin.sha, "snap", snap, sizeof snap) != SU_OK) {
            mark_unrecoverable(w, "the pinned checkpoint is missing from the spool");
            return 2;
        }
        req_begin(&req, AGENT_INTERNAL_ID, "internal.world.restore_schema");
        su_buf_str(&req, "\"world_id\":");
        su_buf_quoted(&req, w->id, strlen(w->id));
        su_buf_str(&req, ",\"expected_world_revision\":");
        su_buf_quoted(&req, cur, strlen(cur));
        su_buf_str(&req, ",\"snapshot_path\":");
        su_buf_quoted(&req, snap, strlen(snap));
        su_buf_str(&req, ",\"snapshot_sha256\":");
        su_buf_quoted(&req, op->mig.pin.sha, strlen(op->mig.pin.sha));
        su_buf_str(&req, ",\"mode\":\"exact\",\"world_revision\":");
        su_buf_u64(&req, op->rev_after);
        st = write_capsules(a, &req, "capsules", op->mig.pin.sys, op->mig.pin.count);
        if (st != SU_OK) {
            su_buf_free(&req);
            mark_unrecoverable(w, "a pinned capsule is missing from the spool");
            return 2;
        }
        req_end(&req);
        su_buf_init(&resp);
        st = roundtrip(a, AGENT_INTERNAL_ID, &req, &resp);
        su_buf_free(&req);
        if (st != SU_OK) {
            su_buf_free(&resp);
            return 1;
        }
        reply_parse(&r, resp.data, resp.len);
        if (!r.ok) {
            mark_unrecoverable(w, "replay of a logged rollback was rejected");
            reply_free(&r);
            su_buf_free(&resp);
            return 2;
        }
        reply_free(&r);
        su_buf_free(&resp);
        memcpy(w->sys, op->mig.pin.sys, sizeof w->sys);
        w->count = op->mig.pin.count;
        return 0;
    }
    if (spool_file(a, op->mig.mig_sha, "migration", pa, sizeof pa) != SU_OK ||
        (op->mig.has_tr && spool_file(a, op->mig.tr_sha, "transform", ta, sizeof ta) != SU_OK) ||
        (op->mig.has_lir && spool_file(a, op->mig.lir_sha, "lir", la, sizeof la) != SU_OK)) {
        mark_unrecoverable(w, "a logged migration document is missing from the spool");
        return 2;
    }
    req_begin(&req, AGENT_INTERNAL_ID, "internal.world.migrate_prepare");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, w->id, strlen(w->id));
    su_buf_str(&req, ",\"expected_world_revision\":");
    su_buf_quoted(&req, cur, strlen(cur));
    su_buf_str(&req, ",\"to_layout_generation\":");
    su_buf_u64(&req, op->mig.to_gen);
    su_buf_str(&req, ",\"migration_path\":");
    su_buf_quoted(&req, pa, strlen(pa));
    su_buf_str(&req, ",\"migration_sha256\":");
    su_buf_quoted(&req, op->mig.mig_sha, strlen(op->mig.mig_sha));
    if (op->mig.has_tr) {
        su_buf_str(&req, ",\"transform_path\":");
        su_buf_quoted(&req, ta, strlen(ta));
        su_buf_str(&req, ",\"transform_sha256\":");
        su_buf_quoted(&req, op->mig.tr_sha, strlen(op->mig.tr_sha));
    }
    if (op->mig.has_lir) {
        su_buf_str(&req, ",\"lir_path\":");
        su_buf_quoted(&req, la, strlen(la));
        su_buf_str(&req, ",\"lir_sha256\":");
        su_buf_quoted(&req, op->mig.lir_sha, strlen(op->mig.lir_sha));
    }
    su_buf_str(&req, ",\"drop_systems\":[");
    for (i = 0; i < op->mig.nd; i++) {
        if (i > 0) {
            su_buf_str(&req, ",");
        }
        su_buf_quoted(&req, op->mig.drop[i], strlen(op->mig.drop[i]));
    }
    su_buf_str(&req, "],\"spool_bytes\":");
    su_buf_u64(&req, op->mig.spool_bytes);
    su_buf_str(&req, ",\"memory_bytes\":");
    su_buf_u64(&req, op->mig.memory_bytes);
    st = write_capsules(a, &req, "resubmit", op->mig.sys, op->mig.n);
    if (st != SU_OK) {
        su_buf_free(&req);
        mark_unrecoverable(w, "a logged replacement capsule is missing from the spool");
        return 2;
    }
    req_end(&req);
    su_buf_init(&resp);
    st = roundtrip(a, AGENT_INTERNAL_ID, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return 1;
    }
    reply_parse(&r, resp.data, resp.len);
    if (!r.ok) {
        mark_unrecoverable(w, "replay of a logged migration was rejected at prepare");
        reply_free(&r);
        su_buf_free(&resp);
        return 2;
    }
    reply_free(&r);
    su_buf_free(&resp);

    req_begin(&req, AGENT_INTERNAL_ID, "internal.world.migrate_swap");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, w->id, strlen(w->id));
    su_buf_str(&req, ",\"expected_world_revision\":");
    su_buf_quoted(&req, cur, strlen(cur));
    su_buf_str(&req, ",\"plan_hash\":");
    su_buf_quoted(&req, op->mig.plan_hash, strlen(op->mig.plan_hash));
    req_end(&req);
    su_buf_init(&resp);
    st = roundtrip(a, AGENT_INTERNAL_ID, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return 1;
    }
    reply_parse(&r, resp.data, resp.len);
    if (!r.ok) {
        mark_unrecoverable(w, "replay of a logged migration was rejected at swap");
        reply_free(&r);
        su_buf_free(&resp);
        return 2;
    }
    reply_free(&r);
    su_buf_free(&resp);
    memcpy(w->sys, op->mig.sys, sizeof w->sys);
    w->count = op->mig.n;
    return 0;
}

/* Replays the logged operations after base (verifying the digest after each) and checks the last
 * acknowledged state. Returns 0 (rebuilt), 1 (the replacement died), 2 (unrecoverable). */
static int replay_log(SuAgent *a, AgentWorld *w, RecStats *s, const Ckpt *base) {
    char cur[AGENT_REV_CAP];
    SuBuf req;
    SuBuf resp;
    Reply r;
    uint32_t i;
    uint32_t k;
    uint32_t st;
    int d;
    uint64_t last_rev;
    uint64_t last_tick;
    char last_sha[65];
    char got[65];

    wrev(w, base->rev, cur, sizeof cur);
    /* 3: replay the log, verifying the digest after every operation. */
    for (k = 0; k < w->log_count; k++) {
        const LogOp *op = &w->log[k];
        if (op->kind == LOG_MIGRATE || op->kind == LOG_ROLLBACK) {
            char got_m[65];
            d = replay_mig_op(a, w, op, cur);
            if (d != 0) {
                return d;
            }
            wrev(w, op->rev_after, cur, sizeof cur);
            d = digest_matches(a, w, op->rev_after, op->tick_after, op->hash_after, got_m, sizeof got_m, NULL, NULL);
            if (d == 1) {
                return 1;
            }
            if (d == 2) {
                mark_unrecoverable(w, "replay of a logged migration produced a state that differs from the acknowledged one");
                return 2;
            }
            s->ops++;
            continue;
        }
        if (op->kind == LOG_STEP) {
            req_begin(&req, AGENT_INTERNAL_ID, "world.step");
            su_buf_str(&req, "\"world_id\":");
            su_buf_quoted(&req, w->id, strlen(w->id));
            su_buf_str(&req, ",\"expected_world_revision\":");
            su_buf_quoted(&req, cur, strlen(cur));
            su_buf_str(&req, ",\"ticks\":");
            su_buf_u64(&req, op->ticks);
            append_tick_opts(&req, a);
            req_end(&req);
        } else if (op->kind == LOG_SPAWN) {
            req_begin(&req, AGENT_INTERNAL_ID, "world.spawn");
            su_buf_str(&req, "\"world_id\":");
            su_buf_quoted(&req, w->id, strlen(w->id));
            su_buf_str(&req, ",\"expected_world_revision\":");
            su_buf_quoted(&req, cur, strlen(cur));
            su_buf_str(&req, ",\"values\":");
            su_buf_str(&req, op->spawn_values);
            if (op->has_ext) {
                su_buf_str(&req, ",\"external_id\":");
                su_buf_u64(&req, op->ext_id);
            }
            req_end(&req);
        } else if (op->kind == LOG_DESPAWN) {
            req_begin(&req, AGENT_INTERNAL_ID, "world.despawn");
            su_buf_str(&req, "\"world_id\":");
            su_buf_quoted(&req, w->id, strlen(w->id));
            su_buf_str(&req, ",\"expected_world_revision\":");
            su_buf_quoted(&req, cur, strlen(cur));
            su_buf_str(&req, ",\"entity\":");
            su_buf_u64(&req, op->handle);
            req_end(&req);
        } else {
            char cap[SU_PATH_BUF];
            char src[SU_PATH_BUF];
            if (spool_file(a, op->sys.capsule_sha, "capsule", cap, sizeof cap) != SU_OK ||
                spool_file(a, op->sys.source_sha, "source", src, sizeof src) != SU_OK) {
                mark_unrecoverable(w, "an installed capsule is missing from the spool");
                return 2;
            }
            req_begin(&req, AGENT_INTERNAL_ID, "internal.candidate.install");
            su_buf_str(&req, "\"world_id\":");
            su_buf_quoted(&req, w->id, strlen(w->id));
            su_buf_str(&req, ",\"candidate_id\":");
            su_buf_quoted(&req, op->cand_id, strlen(op->cand_id));
            su_buf_str(&req, ",\"capsule_path\":");
            su_buf_quoted(&req, cap, strlen(cap));
            su_buf_str(&req, ",\"source_path\":");
            su_buf_quoted(&req, src, strlen(src));
            buf_hash_field(&req, "expected_source_hash", op->sys.source_sha);
            buf_hash_field(&req, "expected_capsule_hash", op->sys.capsule_sha);
            su_buf_str(&req, ",\"evidence\":\"passed\",\"expected_world_revision\":");
            su_buf_quoted(&req, cur, strlen(cur));
            su_buf_str(&req, ",\"allowed_effective_tick\":");
            su_buf_u64(&req, op->effective_tick);
            req_end(&req);
        }
        su_buf_init(&resp);
        st = roundtrip(a, AGENT_INTERNAL_ID, &req, &resp);
        su_buf_free(&req);
        if (st != SU_OK) {
            su_buf_free(&resp);
            return 1;
        }
        reply_parse(&r, resp.data, resp.len);
        if (!r.ok) {
            char why[AGENT_REASON_CAP];
            snprintf(why, sizeof why, "replay of logged operation %u was rejected: %.120s", (unsigned)k, r.msg);
            reply_free(&r);
            su_buf_free(&resp);
            mark_unrecoverable(w, why);
            return 2;
        }
        if (op->kind == LOG_INSTALL) {
            for (i = 0; i < w->count; i++) {
                if (strcmp(w->sys[i].system_id, op->sys.system_id) == 0) {
                    w->sys[i] = op->sys;
                }
            }
        }
        reply_free(&r);
        su_buf_free(&resp);
        wrev(w, op->rev_after, cur, sizeof cur);
        d = digest_matches(a, w, op->rev_after, op->tick_after, op->hash_after, got, sizeof got, NULL, NULL);
        if (d == 1) {
            return 1;
        }
        if (d == 2) {
            char why[AGENT_REASON_CAP];
            snprintf(why, sizeof why, "replay of logged operation %u produced a state that differs from the acknowledged one",
                     (unsigned)k);
            mark_unrecoverable(w, why);
            return 2;
        }
        s->ops++;
    }

    /* 4: the recovered state must be the last acknowledged state. */
    if (w->log_count > 0) {
        last_rev = w->log[w->log_count - 1].rev_after;
        last_tick = w->log[w->log_count - 1].tick_after;
        snprintf(last_sha, sizeof last_sha, "%s", w->log[w->log_count - 1].hash_after);
    } else {
        last_rev = base->rev;
        last_tick = base->tick;
        snprintf(last_sha, sizeof last_sha, "%s", base->sha);
    }
    if (last_rev != w->rev || last_tick != w->tick || strcmp(last_sha, w->hash) != 0) {
        mark_unrecoverable(w, "recovered state does not match the last acknowledged state");
        return 2;
    }
    s->worlds++;
    return 0;
    s->worlds++;
    return 0;
}

/* Recovery of a migrated world (M2-01b): the newest checkpoint is taken after a migration or rollback, and
 * its schema is not the fixture's. The world is created from the snapshot header and the checkpointed
 * capsules, verified against the checkpoint digest, and the log after it is replayed. */
static int rebuild_migrated(SuAgent *a, AgentWorld *w, RecStats *s) {
    const Ckpt *base = &w->ck[w->ck_count - 1];
    char snap[SU_PATH_BUF];
    char cur[AGENT_REV_CAP];
    char got[65];
    SuBuf req;
    SuBuf resp;
    Reply r;
    uint32_t st;
    int d;

    if (spool_file(a, base->sha, "snap", snap, sizeof snap) != SU_OK) {
        mark_unrecoverable(w, "the newest checkpoint is missing from the spool");
        return 2;
    }
    req_begin(&req, AGENT_INTERNAL_ID, "internal.world.create_from_snapshot");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, w->id, strlen(w->id));
    su_buf_str(&req, ",\"world_revision\":");
    su_buf_u64(&req, base->rev);
    su_buf_str(&req, ",\"snapshot_path\":");
    su_buf_quoted(&req, snap, strlen(snap));
    su_buf_str(&req, ",\"snapshot_sha256\":");
    su_buf_quoted(&req, base->sha, strlen(base->sha));
    su_buf_str(&req, ",\"max_struct_per_tick\":");
    su_buf_u64(&req, w->cfg[1]);
    su_buf_str(&req, ",\"max_events_per_tick\":");
    su_buf_u64(&req, w->cfg[2]);
    su_buf_str(&req, ",\"event_log_max\":");
    su_buf_u64(&req, w->cfg[3]);
    st = write_capsules(a, &req, "capsules", base->sys, base->count);
    if (st != SU_OK) {
        su_buf_free(&req);
        mark_unrecoverable(w, "a checkpointed capsule is missing from the spool");
        return 2;
    }
    req_end(&req);
    su_buf_init(&resp);
    st = roundtrip(a, AGENT_INTERNAL_ID, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return 1;
    }
    reply_parse(&r, resp.data, resp.len);
    if (!r.ok) {
        char why[AGENT_REASON_CAP];
        snprintf(why, sizeof why, "recreation from the post-migration checkpoint was rejected: %.100s", r.msg);
        mark_unrecoverable(w, why);
        reply_free(&r);
        su_buf_free(&resp);
        return 2;
    }
    reply_free(&r);
    su_buf_free(&resp);
    memcpy(w->sys, base->sys, sizeof w->sys);
    w->count = base->count;
    wrev(w, base->rev, cur, sizeof cur);
    d = digest_matches(a, w, base->rev, base->tick, base->sha, got, sizeof got, NULL, NULL);
    if (d == 1) {
        return 1;
    }
    if (d == 2) {
        mark_unrecoverable(w, "restored post-migration checkpoint does not match its recorded digest");
        return 2;
    }
    if (!s->have_base || base->tick < s->min_base_tick) {
        s->min_base_tick = base->tick;
        s->have_base = 1;
    }
    return replay_log(a, w, s, base);
}

static int rebuild_world(SuAgent *a, AgentWorld *w, RecStats *s) {
    char cur[AGENT_REV_CAP];
    SuBuf req;
    SuBuf resp;
    Reply r;
    const Ckpt *base;
    char snap[SU_PATH_BUF];
    char got[65];
    char rv[AGENT_REV_CAP];
    uint32_t st;
    int d;

    if (w->ck_count > 0 && w->ck[w->ck_count - 1].migrated) {
        return rebuild_migrated(a, w, s);
    }

    /* 1: recreate from the creation parameters (fixture hash and capsule hashes are checked). */
    req_begin(&req, AGENT_INTERNAL_ID, "world.create");
    su_buf_str(&req, "\"fixture_path\":");
    su_buf_quoted(&req, w->fixture_path, strlen(w->fixture_path));
    su_buf_str(&req, ",\"fixture_sha256\":");
    su_buf_quoted(&req, w->fixture_sha, strlen(w->fixture_sha));
    su_buf_str(&req, ",\"world_id\":");
    su_buf_quoted(&req, w->id, strlen(w->id));
    su_buf_str(&req, ",\"max_entities\":");
    su_buf_u64(&req, w->cfg[0]);
    su_buf_str(&req, ",\"max_struct_per_tick\":");
    su_buf_u64(&req, w->cfg[1]);
    su_buf_str(&req, ",\"max_events_per_tick\":");
    su_buf_u64(&req, w->cfg[2]);
    su_buf_str(&req, ",\"event_log_max\":");
    su_buf_u64(&req, w->cfg[3]);
    st = write_capsules(a, &req, "capsules", w->creation, w->creation_count);
    if (st != SU_OK) {
        su_buf_free(&req);
        mark_unrecoverable(w, "a creation capsule is missing from the spool");
        return 2;
    }
    req_end(&req);
    su_buf_init(&resp);
    st = roundtrip(a, AGENT_INTERNAL_ID, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return 1;
    }
    reply_parse(&r, resp.data, resp.len);
    if (!r.ok || !jstr(r.result, "world_id", rv, sizeof rv) || strcmp(rv, w->id) != 0) {
        mark_unrecoverable(w, "recreation from the creation parameters was rejected");
        reply_free(&r);
        su_buf_free(&resp);
        return 2;
    }
    reply_free(&r);
    su_buf_free(&resp);

    /* 2: newest checkpoint, at its recorded revision. */
    if (w->ck_count == 0) {
        mark_unrecoverable(w, "no checkpoint to recover from");
        return 2;
    }
    base = &w->ck[w->ck_count - 1];
    if (spool_file(a, base->sha, "snap", snap, sizeof snap) != SU_OK) {
        mark_unrecoverable(w, "the newest checkpoint is missing from the spool");
        return 2;
    }
    wrev(w, 1, cur, sizeof cur);
    req_begin(&req, AGENT_INTERNAL_ID, "internal.world.restore_bytes");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, w->id, strlen(w->id));
    su_buf_str(&req, ",\"expected_world_revision\":");
    su_buf_quoted(&req, cur, strlen(cur));
    su_buf_str(&req, ",\"snapshot_path\":");
    su_buf_quoted(&req, snap, strlen(snap));
    su_buf_str(&req, ",\"snapshot_sha256\":");
    su_buf_quoted(&req, base->sha, strlen(base->sha));
    su_buf_str(&req, ",\"mode\":\"exact\",\"world_revision\":");
    su_buf_u64(&req, base->rev);
    st = write_capsules(a, &req, "capsules", base->sys, base->count);
    if (st != SU_OK) {
        su_buf_free(&req);
        mark_unrecoverable(w, "a checkpointed capsule is missing from the spool");
        return 2;
    }
    req_end(&req);
    su_buf_init(&resp);
    st = roundtrip(a, AGENT_INTERNAL_ID, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return 1;
    }
    reply_parse(&r, resp.data, resp.len);
    if (!r.ok) {
        mark_unrecoverable(w, "newest checkpoint was rejected on restore (corrupt or missing)");
        reply_free(&r);
        su_buf_free(&resp);
        return 2;
    }
    reply_free(&r);
    su_buf_free(&resp);
    wrev(w, base->rev, cur, sizeof cur);
    memcpy(w->sys, base->sys, sizeof w->sys);
    w->count = base->count;
    d = digest_matches(a, w, base->rev, base->tick, base->sha, got, sizeof got, NULL, NULL);
    if (d == 1) {
        return 1;
    }
    if (d == 2) {
        mark_unrecoverable(w, "restored checkpoint does not match its recorded digest");
        return 2;
    }
    if (!s->have_base || base->tick < s->min_base_tick) {
        s->min_base_tick = base->tick;
        s->have_base = 1;
    }

    return replay_log(a, w, s, base);
}

/* Appends one quarantine entry to the recovery object's list. */
static void quarantine_note(SuBuf *q, int *nq, const char *cand_id, const char *world_id, const char *reason,
                            int rolled_back, uint64_t to_tick, uint64_t discarded) {
    if (*nq > 0) {
        su_buf_str(q, ",");
    }
    su_buf_str(q, "{\"candidate_id\":");
    su_buf_quoted(q, cand_id, strlen(cand_id));
    su_buf_str(q, ",\"world_id\":");
    su_buf_quoted(q, world_id, strlen(world_id));
    su_buf_str(q, ",\"state\":\"quarantined\",\"reason\":");
    su_buf_quoted(q, reason, strlen(reason));
    if (rolled_back) {
        su_buf_str(q, ",\"rolled_back_to_tick\":");
        su_buf_u64(q, to_tick);
        su_buf_str(q, ",\"discarded_ticks\":");
        su_buf_u64(q, discarded);
    }
    su_buf_str(q, "}");
    (*nq)++;
}

static AgentCandidate *find_candidate(SuAgent *a, const char *id);

static AgentCandidate *candidate_by_capsule(SuAgent *a, const char *sha) {
    uint32_t i;
    for (i = 0; i < AGENT_MAX_CANDIDATES; i++) {
        if (a->cand[i].used && strcmp(a->cand[i].capsule_hash, sha) == 0) {
            return &a->cand[i];
        }
    }
    return NULL;
}

static void candidate_quarantine(AgentCandidate *cd, const char *reason) {
    snprintf(cd->install_state, sizeof cd->install_state, "quarantined");
    snprintf(cd->quarantine_reason, sizeof cd->quarantine_reason, "%s", reason);
}

/* Quarantines the revision a world keeps crashing on (ISSUE: repeated identical failures).
 * An install that crashes the worker is refused from then on, and the world stays on its previous
 * revision. Any other operation crashing on a world's installed revision reverts the world to the
 * newest checkpoint that predates that revision, recorded as a new transaction (its revision
 * advances); operations accepted after that checkpoint are discarded and reported. The worker must
 * be alive. */
static void quarantine_apply(SuAgent *a, const InFlight *op, SuBuf *q, int *nq) {
    AgentWorld *w = op->w;
    char reason[AGENT_REASON_CAP];
    char cand_id[32] = "";
    AgentCandidate *cd;
    uint32_t i;
    int idx = -1;
    uint64_t best = 0;
    char rev[AGENT_REV_CAP];
    uint32_t k;
    int found = -1;

    if (op->kind == OPK_INSTALL) {
        cd = find_candidate(a, op->cand);
        snprintf(reason, sizeof reason, "worker died on %u consecutive recoveries while installing this revision at tick %llu",
                 (unsigned)w->crash_repeat, (unsigned long long)op->tick);
        if (cd != NULL) {
            candidate_quarantine(cd, reason);
            snprintf(cand_id, sizeof cand_id, "%s", cd->id);
        }
        quarantine_note(q, nq, cd != NULL ? cand_id : op->cand, w->id, reason, 0, 0, 0);
        w->crash_repeat = 0;
        w->crash_key[0] = '\0';
        return;
    }
    for (i = 0; i < w->count; i++) {
        int creation = 0;
        uint32_t c;
        for (c = 0; c < w->creation_count; c++) {
            if (strcmp(w->creation[c].capsule_sha, w->sys[i].capsule_sha) == 0) {
                creation = 1;
            }
        }
        if (!creation && (idx < 0 || w->sys[i].system_rev >= best)) {
            idx = (int)i;
            best = w->sys[i].system_rev;
        }
    }
    if (idx < 0) {
        mark_unrecoverable(w, "repeated worker failure with no installed revision to quarantine");
        return;
    }
    cd = candidate_by_capsule(a, w->sys[idx].capsule_sha);
    for (k = w->ck_count; k > 0 && found < 0; k--) {
        uint32_t j;
        int has = 0;
        for (j = 0; j < w->ck[k - 1].count; j++) {
            if (strcmp(w->ck[k - 1].sys[j].capsule_sha, w->sys[idx].capsule_sha) == 0) {
                has = 1;
            }
        }
        if (!has) {
            found = (int)(k - 1);
        }
    }
    if (found < 0) {
        mark_unrecoverable(w, "repeated worker failure and no checkpoint predates the installed revision");
        return;
    }
    {
        const Ckpt *target = &w->ck[found];
        SuBuf req;
        SuBuf resp;
        Reply r;
        uint32_t st;
        uint64_t discarded;
        char got[65];
        char rv[AGENT_REV_CAP];
        uint64_t tick_now = w->tick;

        snprintf(reason, sizeof reason, "worker died on %u consecutive recoveries running this revision (tick %llu)",
                 (unsigned)w->crash_repeat, (unsigned long long)op->tick);
        wrev(w, w->rev, rev, sizeof rev);
        {
            char snap[SU_PATH_BUF];
            if (spool_file(a, target->sha, "snap", snap, sizeof snap) != SU_OK) {
                mark_unrecoverable(w, "quarantine rollback checkpoint is missing from the spool");
                return;
            }
            req_begin(&req, AGENT_INTERNAL_ID, "internal.world.restore_bytes");
            su_buf_str(&req, "\"world_id\":");
            su_buf_quoted(&req, w->id, strlen(w->id));
            su_buf_str(&req, ",\"expected_world_revision\":");
            su_buf_quoted(&req, rev, strlen(rev));
            su_buf_str(&req, ",\"snapshot_path\":");
            su_buf_quoted(&req, snap, strlen(snap));
            su_buf_str(&req, ",\"snapshot_sha256\":");
            su_buf_quoted(&req, target->sha, strlen(target->sha));
            su_buf_str(&req, ",\"mode\":\"advance\"");
            if (write_capsules(a, &req, "capsules", target->sys, target->count) != SU_OK) {
                su_buf_free(&req);
                mark_unrecoverable(w, "quarantine rollback capsule is missing from the spool");
                return;
            }
            req_end(&req);
        }
        su_buf_init(&resp);
        st = roundtrip(a, AGENT_INTERNAL_ID, &req, &resp);
        su_buf_free(&req);
        if (st != SU_OK) {
            su_buf_free(&resp);
            return; /* the worker died again: the next request recovers and repeats this decision */
        }
        reply_parse(&r, resp.data, resp.len);
        if (!r.ok || !jstr(r.result, "world_revision", rv, sizeof rv)) {
            reply_free(&r);
            su_buf_free(&resp);
            mark_unrecoverable(w, "quarantine rollback was rejected by the worker");
            return;
        }
        reply_free(&r);
        su_buf_free(&resp);
        discarded = tick_now >= target->tick ? tick_now - target->tick : 0;
        w->rev = rev_num(rv);
        w->count = target->count;
        memcpy(w->sys, target->sys, sizeof w->sys);
        if (digest_matches(a, w, w->rev, target->tick, NULL, got, sizeof got, NULL, NULL) == 1) {
            return;
        }
        w->tick = target->tick;
        snprintf(w->hash, sizeof w->hash, "%s", got);
        if (checkpoint_world(a, w, 0) == CK_DIED) {
            return;
        }
        if (cd != NULL) {
            candidate_quarantine(cd, reason);
            snprintf(cand_id, sizeof cand_id, "%s", cd->id);
        } else {
            snprintf(cand_id, sizeof cand_id, "unknown");
        }
        quarantine_note(q, nq, cand_id, w->id, reason, 1, target->tick, discarded);
    }
    w->crash_repeat = 0;
    w->crash_key[0] = '\0';
}

/* ---- M1-02c-2: native kernels, quarantine and respecialization (docs/AGENT_RPC.md section 10) ----
 * Native kernels are not canonical state: nothing about them is logged or replayed, and a replacement
 * worker always starts on the VM tier. The supervisor records which artifact is installed per system
 * (NatRec). A worker death during a world.step of a world that has installed kernels is attributed to
 * those kernels: each is quarantined by artifact_hash (QRec) and refused from then on. Kernels in the
 * same worker that were not attributed are re-requested after recovery (RespecRec), serviced between
 * requests, never during recovery and never by world.step. */
static void nat_clear(AgentWorld *w) {
    memset(w->nat, 0, sizeof w->nat);
}

static QRec *quar_find(SuAgent *a, const char *hash) {
    uint32_t i;
    for (i = 0; i < AGENT_MAX_QUARANTINE; i++) {
        if (a->quar[i].used && strcmp(a->quar[i].artifact_hash, hash) == 0) {
            return &a->quar[i];
        }
    }
    return NULL;
}

#ifdef SU_TEST_FAULT_HOOKS
/* Test-only (M1 closure C): 1 when a table holding one record for record_hash finds query_hash. Two
 * distinct artifact hashes must never match, however long their common prefix. */
int su_agent_test_quar_hit(const char *record_hash, const char *query_hash) {
    static SuAgent a;
    memset(&a, 0, sizeof a);
    a.quar[0].used = 1;
    snprintf(a.quar[0].artifact_hash, sizeof a.quar[0].artifact_hash, "%s", record_hash);
    return quar_find(&a, query_hash) != NULL;
}
#endif

/* Records one attributed crash of n's artifact. 0 when the table is full: then the artifact cannot be
 * recorded, and quar_overflow refuses every native specialization (no crash loop is possible). */
static int quar_note(SuAgent *a, const AgentWorld *w, const NatRec *n, uint64_t old_pid, uint64_t tick,
                     const char *reason) {
    QRec *q = quar_find(a, n->artifact_hash);
    uint32_t i;
    if (q == NULL) {
        for (i = 0; i < AGENT_MAX_QUARANTINE && q == NULL; i++) {
            if (!a->quar[i].used) {
                q = &a->quar[i];
            }
        }
        if (q == NULL) {
            a->quar_overflow = 1;
            return 0;
        }
        memset(q, 0, sizeof *q);
        q->used = 1;
        snprintf(q->artifact_hash, sizeof q->artifact_hash, "%s", n->artifact_hash);
        snprintf(q->world_id, sizeof q->world_id, "%s", w->id);
        snprintf(q->system_id, sizeof q->system_id, "%s", n->system_id);
        snprintf(q->backend, sizeof q->backend, "%s", n->backend);
        snprintf(q->tier, sizeof q->tier, "%s", n->tier);
    }
    q->crashes++;
    q->replaced_pid = old_pid;
    q->tick = tick;
    snprintf(q->reason, sizeof q->reason, "%s", reason);
    return 1;
}

static void respec_enqueue(SuAgent *a, AgentWorld *w, const char *system_id, const char *backend) {
    RespecRec *r;
    if (a->respec_count >= AGENT_MAX_RESPEC) {
        snprintf(w->respec_state, sizeof w->respec_state, "failed");
        snprintf(w->respec_reason, sizeof w->respec_reason, "respecialization queue is full");
        return;
    }
    r = &a->respec[a->respec_count++];
    memset(r, 0, sizeof *r);
    r->used = 1;
    snprintf(r->world_id, sizeof r->world_id, "%s", w->id);
    snprintf(r->system_id, sizeof r->system_id, "%s", system_id);
    snprintf(r->backend, sizeof r->backend, "%s", backend);
    snprintf(w->respec_state, sizeof w->respec_state, "pending");
    w->respec_reason[0] = '\0';
}

static void respec_drop_world(SuAgent *a, AgentWorld *w) {
    uint32_t i;
    uint32_t k = 0;
    for (i = 0; i < a->respec_count; i++) {
        if (strcmp(a->respec[i].world_id, w->id) != 0) {
            a->respec[k++] = a->respec[i];
        }
    }
    if (k < a->respec_count) {
        memset(&a->respec[k], 0, sizeof a->respec[0] * (a->respec_count - k));
    }
    a->respec_count = k;
    w->respec_state[0] = '\0';
    w->respec_reason[0] = '\0';
}

/* The native object of the recovery answer: the artifact and whether it is quarantined. */
static void nat_object(SuAgent *a, const AgentWorld *w, const NatRec *n, int quarantined, uint64_t old_pid,
                       const QRec *q) {
    su_buf_free(&a->nat_rec);
    su_buf_init(&a->nat_rec);
    su_buf_str(&a->nat_rec, "{\"artifact_hash\":");
    su_buf_quoted(&a->nat_rec, n->artifact_hash, strlen(n->artifact_hash));
    su_buf_str(&a->nat_rec, ",\"world_id\":");
    su_buf_quoted(&a->nat_rec, w->id, strlen(w->id));
    su_buf_str(&a->nat_rec, ",\"system_id\":");
    su_buf_quoted(&a->nat_rec, n->system_id, strlen(n->system_id));
    su_buf_str(&a->nat_rec, ",\"backend\":");
    su_buf_quoted(&a->nat_rec, n->backend, strlen(n->backend));
    su_buf_str(&a->nat_rec, ",\"tier\":");
    su_buf_quoted(&a->nat_rec, n->tier, strlen(n->tier));
    su_buf_str(&a->nat_rec, ",\"quarantined\":");
    su_buf_str(&a->nat_rec, quarantined ? "true" : "false");
    if (q != NULL) {
        su_buf_str(&a->nat_rec, ",\"crash_count\":");
        su_buf_u64(&a->nat_rec, q->crashes);
        su_buf_str(&a->nat_rec, ",\"replaced_pid\":");
        su_buf_u64(&a->nat_rec, old_pid);
    }
    su_buf_str(&a->nat_rec, "}");
    a->nat_has = 1;
}

/* Runs first in recovery, before the old worker is killed. Kernels of a world.step that died are
 * quarantined (every kernel of that world: the crashed one is not identified per tick). The others
 * are queued for respecialization. Every record is dropped: the replacement worker has no native
 * kernel. */
static void native_on_death(SuAgent *a, const InFlight *op, uint64_t old_pid) {
    uint32_t i;
    uint32_t s;
    for (i = 0; i < AGENT_MAX_WORLDS; i++) {
        AgentWorld *w = &a->world[i];
        int attributed;
        if (!w->used || w->pending) {
            continue;
        }
        attributed = op != NULL && op->kind == OPK_STEP && op->w == w;
        for (s = 0; s < AGENT_MAX_SYSTEMS; s++) {
            NatRec *n = &w->nat[s];
            int noted;
            if (!n->used) {
                continue;
            }
            if (attributed) {
                noted = quar_note(a, w, n, old_pid, op->tick, "worker died while running this native kernel");
                if (!a->nat_has) {
                    /* the recovery answer names the first attributed kernel */
                    nat_object(a, w, n, noted, old_pid, noted ? quar_find(a, n->artifact_hash) : NULL);
                }
                if (!noted) {
                    snprintf(w->respec_state, sizeof w->respec_state, "failed");
                    snprintf(w->respec_reason, sizeof w->respec_reason, "quarantine table is full");
                }
                /* The death is attributed to the kernel, not to a candidate revision: the M1-02a
                 * repeated-crash rule (candidate quarantine) must not count these deaths. */
                w->crash_repeat = 0;
                w->crash_key[0] = '\0';
            } else {
                respec_enqueue(a, w, n->system_id, n->backend);
            }
            n->used = 0;
        }
    }
}

/* Replaces the active worker and rebuilds every live world. The recovery object is kept in a->rec
 * (for the response being answered) and in a->event (reported once on the next response). */
static uint32_t count_unrecoverable(const SuAgent *a) {
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < AGENT_MAX_WORLDS; i++) {
        if (a->world[i].used && a->world[i].unrecoverable) {
            n++;
        }
    }
    return n;
}

static void recover(SuAgent *a, AgentCall *c, const InFlight *op) {
    int64_t t0 = su_now_ms();
    uint64_t old_pid = a->worker.alive ? (uint64_t)a->worker.pid : a->last_pid;
    uint32_t unrec_before = count_unrecoverable(a);
    RecStats st;
    int ok = 0;
    uint32_t attempt;
    uint32_t i;
    SuBuf q;
    int nq = 0;
    SuBuf unrec;
    int nu = 0;

    su_buf_init(&q);
    su_buf_init(&unrec);
    native_on_death(a, op, old_pid);
    su_proc_kill(&a->worker);
    memset(&st, 0, sizeof st);
    for (attempt = 0; attempt < AGENT_RECOVERY_ATTEMPTS && !ok; attempt++) {
        RecStats s;
        int died = 0;
        memset(&s, 0, sizeof s);
        if (ensure_worker(a) != SU_OK) {
            continue;
        }
        if (seed_worker(a) != SU_OK) {
            su_proc_kill(&a->worker);
            continue;
        }
        for (i = 0; i < AGENT_MAX_WORLDS && !died; i++) {
            AgentWorld *w = &a->world[i];
            int r;
            if (!w->used || w->pending || w->unrecoverable) {
                continue;
            }
            r = rebuild_world(a, w, &s);
            if (r == 1) {
                died = 1;
                if (attempt + 1 == AGENT_RECOVERY_ATTEMPTS) {
                    mark_unrecoverable(w, "replacement workers kept dying while replaying accepted operations");
                }
            }
        }
        if (!died) {
            ok = 1;
            st = s;
        } else {
            su_proc_kill(&a->worker);
        }
    }
    if (ok) {
        a->recoveries++;
        if (op != NULL && op->w != NULL && op->w->used && op->kind != OPK_READ && op->w->crash_repeat > 0 &&
            op->w->crash_repeat >= a->quarantine_after && strcmp(op->w->crash_key, "") != 0) {
            quarantine_apply(a, op, &q, &nq);
        }
    }
    for (i = 0; i < AGENT_MAX_WORLDS; i++) {
        const AgentWorld *w = &a->world[i];
        if (w->used && w->unrecoverable) {
            if (nu > 0) {
                su_buf_str(&unrec, ",");
            }
            su_buf_str(&unrec, "{\"world_id\":");
            su_buf_quoted(&unrec, w->id, strlen(w->id));
            su_buf_str(&unrec, ",\"reason\":");
            su_buf_quoted(&unrec, w->reason, strlen(w->reason));
            su_buf_str(&unrec, "}");
            nu++;
        }
    }
    su_buf_free(&a->rec);
    su_buf_init(&a->rec);
    /* recovered is true only when every live world was rebuilt and verified in this recovery. */
    ok = ok && count_unrecoverable(a) == unrec_before;
    su_buf_str(&a->rec, "{\"recovered\":");
    su_buf_str(&a->rec, ok ? "true" : "false");
    su_buf_str(&a->rec, ",\"replaced_pid_old\":");
    su_buf_u64(&a->rec, old_pid);
    su_buf_str(&a->rec, ",\"replaced_pid_new\":");
    su_buf_u64(&a->rec, a->worker.alive ? (uint64_t)a->worker.pid : 0u);
    su_buf_str(&a->rec, ",\"worlds_recovered\":");
    su_buf_u64(&a->rec, st.worlds);
    su_buf_str(&a->rec, ",\"ops_replayed\":");
    su_buf_u64(&a->rec, st.ops);
    su_buf_str(&a->rec, ",\"duration_ms\":");
    su_buf_u64(&a->rec, (uint64_t)(su_now_ms() - t0));
    su_buf_str(&a->rec, ",\"from_checkpoint_tick\":");
    if (st.have_base) {
        su_buf_u64(&a->rec, st.min_base_tick);
    } else {
        su_buf_str(&a->rec, "null");
    }
    su_buf_str(&a->rec, ",\"worker_generation\":");
    su_buf_u64(&a->rec, a->generation);
    su_buf_str(&a->rec, ",\"unrecoverable\":[");
    su_buf_put(&a->rec, unrec.data, unrec.len);
    su_buf_str(&a->rec, "],\"quarantined\":[");
    su_buf_put(&a->rec, q.data, q.len);
    su_buf_str(&a->rec, "]");
    if (a->nat_has) {
        su_buf_str(&a->rec, ",\"native\":");
        su_buf_put(&a->rec, a->nat_rec.data, a->nat_rec.len);
        a->nat_has = 0;
    }
    su_buf_str(&a->rec, "}");
    su_buf_free(&q);
    su_buf_free(&unrec);

    su_buf_free(&a->event);
    su_buf_init(&a->event);
    su_buf_put(&a->event, a->rec.data, a->rec.len);
    a->event_pending = 1;
    c->has_rec = 1;
}

/* Crash bookkeeping: identical failures of one operation in a row, across recoveries. */
static void note_crash(const InFlight *op) {
    AgentWorld *w = op->w;
    char key[160];
    if (op->kind == OPK_READ || w == NULL) {
        return;
    }
    snprintf(key, sizeof key, "%d|%s|%llu|%s", op->kind, w->id, (unsigned long long)op->tick, op->cand);
    if (strcmp(key, w->crash_key) == 0) {
        w->crash_repeat++;
    } else {
        snprintf(w->crash_key, sizeof w->crash_key, "%s", key);
        w->crash_repeat = 1;
    }
}

static const char *const k_recovery_msg =
    "active worker died or timed out; a replacement worker was started and live worlds were rebuilt from supervisor records; retry the request";

/* The worker died while answering op. Records the crash, recovers, and returns the failure for the
 * request. op may be NULL (no world involved). */
static uint32_t worker_died(SuAgent *a, AgentCall *c, const InFlight *op) {
    InFlight none;
    if (op == NULL) {
        memset(&none, 0, sizeof none);
        none.kind = OPK_READ;
        op = &none;
    } else {
        note_crash(op);
    }
    recover(a, c, op);
    call_fail(c, SU_E_WORKER_FAILED, k_recovery_msg);
    return SU_E_WORKER_FAILED;
}

/* Before a request is sent: a worker that exited while idle is recovered first. The request is
 * then answered with SU_E_WORKER_FAILED and not executed. */
static uint32_t preflight(SuAgent *a, AgentCall *c) {
    int need = 0;
    InFlight none;
    if (a->worker.alive && su_proc_exited(&a->worker)) {
        need = 1;
    }
    if (!a->worker.alive && live_worlds(a) > 0) {
        need = 1;
    }
    if (!need) {
        return SU_OK;
    }
    memset(&none, 0, sizeof none);
    recover(a, c, &none);
    call_fail(c, SU_E_WORKER_FAILED, "active worker had exited; a replacement worker was started and live worlds were rebuilt; retry the request");
    return SU_E_WORKER_FAILED;
}

/* ---------- capsule location ---------- */

/* Resolves a capsule path under the root, reads it (captured bytes) and derives
 * the source path from the capsule's "source" field. Caller frees *cap_bytes. */
static uint32_t locate_capsule(SuAgent *a, const char *rel, char *cap_real, char *src_real,
                               uint8_t **cap_bytes, size_t *cap_len, AgentCall *c) {
    char source[SU_MODULE_NAME_MAX + 1];
    uint32_t sv = 0;
    uint32_t gen = 0;
    uint32_t st;

    st = su_path_resolve(a->root, rel, cap_real, SU_PATH_BUF);
    if (st != SU_OK) {
        return call_fail(c, st, "capsule path rejected by the project path policy");
    }
    st = su_path_read_file(cap_real, AGENT_CAPSULE_MAX, cap_bytes, cap_len);
    if (st != SU_OK) {
        return call_fail(c, st, "capsule could not be read");
    }
    st = su_module_capsule_meta(*cap_bytes, *cap_len, source, sizeof source, &sv, &gen);
    if (st != SU_OK) {
        free(*cap_bytes);
        *cap_bytes = NULL;
        return call_fail(c, st, "capsule metadata is malformed");
    }
    if (src_real == NULL) {
        return SU_OK; /* caller supplies the source module itself (capsule.submit source_path) */
    }
    st = su_path_sibling(a->root, cap_real, source, src_real, SU_PATH_BUF);
    if (st != SU_OK) {
        free(*cap_bytes);
        *cap_bytes = NULL;
        return call_fail(c, st, "capsule source path rejected by the project path policy");
    }
    return SU_OK;
}

/* ---------- handlers ---------- */

static uint32_t h_engine_info(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    size_t i;
    uint32_t j;
    if (check_keys(params, NULL, 0, c) != SU_OK) {
        return SU_E_TYPE;
    }
    su_buf_str(res, "{\"engine_version\":\"" SUTEKH_VERSION_STRING "\",\"protocol\":\"" SU_PROTOCOL_VERSION
                    "\",\"ir_version\":\"" SU_IR_VERSION_STRING "\",\"abi_version\":");
    su_buf_u64(res, SU_ABI_VERSION);
    su_buf_str(res, ",\"artifact_format\":");
    su_buf_u64(res, SU_ARTIFACT_FORMAT_VERSION);
    su_buf_str(res, ",\"numeric_profile\":\"" SU_NUMERIC_PROFILE_U32_MOD_V1
                    "\",\"runtime_mode\":\"headless-m0-posix\",\"transport\":\"stdio\""
                    ",\"process\":{\"role\":\"supervisor\",\"pid\":");
    su_buf_u64(res, (uint64_t)getpid());
    su_buf_str(res, "},\"active_worker\":{\"pid\":");
    su_buf_u64(res, a->worker.alive ? (uint64_t)a->worker.pid : 0u);
    su_buf_str(res, "},\"worker_generation\":");
    su_buf_u64(res, a->generation);
    su_buf_str(res, ",\"recoveries\":");
    su_buf_u64(res, a->recoveries);
    su_buf_str(res, ",\"tick_workers\":{\"workers\":");
    su_buf_u64(res, a->tick_workers);
    su_buf_str(res, ",\"chunk_min_rows\":");
    su_buf_u64(res, a->tick_chunk_min_rows);
    su_buf_str(res, "}");
    su_buf_str(res, ",\"recovery\":{\"limits\":{\"checkpoint_interval_ticks\":");
    su_buf_u64(res, a->interval);
    su_buf_str(res, ",\"checkpoint_retain\":");
    su_buf_u64(res, a->retain);
    su_buf_str(res, ",\"log_max_entries\":");
    su_buf_u64(res, a->log_cap);
    su_buf_str(res, ",\"quarantine_after\":");
    su_buf_u64(res, a->quarantine_after);
    su_buf_str(res, ",\"spool_bytes_max\":");
    su_buf_u64(res, a->spool_max);
    su_buf_str(res, "},\"spool_bytes\":");
    su_buf_u64(res, su_spool_bytes(a->spool));
    su_buf_str(res, ",\"committed_bytes\":");
    su_buf_u64(res, committed_bytes(a));
    su_buf_str(res, ",\"worlds\":[");
    for (j = 0, i = 0; j < AGENT_MAX_WORLDS; j++) {
        const AgentWorld *w = &a->world[j];
        if (!w->used || w->pending) {
            continue;
        }
        if (i > 0) {
            su_buf_str(res, ",");
        }
        i++;
        su_buf_str(res, "{\"world_id\":");
        su_buf_quoted(res, w->id, strlen(w->id));
        su_buf_str(res, ",\"tick\":");
        su_buf_u64(res, w->tick);
        su_buf_str(res, ",\"revision\":");
        su_buf_u64(res, w->rev);
        su_buf_str(res, ",\"log_entries\":");
        su_buf_u64(res, w->log_count);
        su_buf_str(res, ",\"checkpoints\":");
        su_buf_u64(res, w->ck_count);
        su_buf_str(res, ",\"checkpoint_bytes\":");
        su_buf_u64(res, checkpoint_bytes(w));
        su_buf_str(res, ",\"reserved_bytes\":");
        su_buf_u64(res, w->reserved);
        su_buf_str(res, ",\"unrecoverable\":");
        su_buf_str(res, w->unrecoverable ? "true" : "false");
        su_buf_str(res, "}");
    }
    su_buf_str(res, "]}");
#ifdef SU_HAVE_NATIVE
    /* Only what the build implements: scalar always; avx2 when the host reports it. */
    su_buf_str(res, ",\"native\":{\"specialization\":{\"method\":\"world.specialize\",\"backends\":[\"scalar\",\"avx2\",\"auto\"],\"tiers\":[\"vm\",\"native_scalar\",\"native_avx2\"],\"host_caps\":{\"scalar\":true,\"avx2\":");
    su_buf_str(res, (su_native_host_caps() & SU_NCAP_X86_64_AVX2) != 0u ? "true" : "false");
    su_buf_str(res, "},\"asmjit_commit\":");
    su_buf_quoted(res, su_native_asmjit_commit(), strlen(su_native_asmjit_commit()));
    su_buf_str(res, ",\"replay\":\"not_replayed_yet_M1-02c\"}}");
#endif
    su_buf_str(res, ",\"capabilities\":[");
    for (i = 0; i < sizeof k_capabilities / sizeof k_capabilities[0]; i++) {
        if (i > 0) {
            su_buf_str(res, ",");
        }
        su_buf_quoted(res, k_capabilities[i], strlen(k_capabilities[i]));
    }
    su_buf_str(res, "]}");
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

static uint32_t h_capsule_submit(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"capsule_path", "source_path"};
    const char *rel = NULL;
    const char *src_override = NULL;
    char cap_real[SU_PATH_BUF];
    char src_real[SU_PATH_BUF];
    uint8_t *cap = NULL;
    uint8_t *lir = NULL;
    size_t cap_len = 0;
    size_t lir_len = 0;
    SuDiag diag;
    SuLirModule mod;
    AgentCandidate *slot = NULL;
    char cap_sha[65];
    char src_sha[65];
    size_t i;
    uint32_t st;

    if (check_keys(params, allowed, 2, c) != SU_OK || param_str(params, "capsule_path", 1, &rel, c) != SU_OK) {
        return SU_E_TYPE;
    }
    if (su_json_member(params, "source_path") != NULL &&
        param_str(params, "source_path", 1, &src_override, c) != SU_OK) {
        return SU_E_TYPE;
    }
    st = locate_capsule(a, rel, cap_real, src_override != NULL ? NULL : src_real, &cap, &cap_len, c);
    if (st != SU_OK) {
        return st;
    }
    if (src_override != NULL) {
        /* Optional explicit source module (same project-root path policy as every other path). It
         * replaces the capsule's own `source` binding for this candidate only. */
        st = su_path_resolve(a->root, src_override, src_real, SU_PATH_BUF);
        if (st != SU_OK) {
            free(cap);
            return call_fail(c, st, "source path rejected by the project path policy");
        }
    }
    st = su_path_read_file(src_real, SU_LIMIT_IR_SOURCE_BYTES, &lir, &lir_len);
    if (st != SU_OK) {
        free(cap);
        return call_fail(c, st, "source module could not be read");
    }
    su_diag_init(&diag);
    memset(&mod, 0, sizeof mod);
    st = su_lir_verify(cap, cap_len, lir, lir_len, &mod, &diag);
    if (st != SU_OK) {
        char msg[AGENT_MSG_CAP];
        snprintf(msg, sizeof msg, "verification failed: %.200s at %.100s", diag.message,
                 diag.json_pointer[0] != '\0' ? diag.json_pointer : "/");
        free(cap);
        free(lir);
        return call_fail(c, st, msg);
    }
    /* The verified bytes are the candidate: keep them in the spool so that installs and replays
     * never depend on the files on disk again. */
    st = spool_put_admit(a, "capsule", cap, cap_len, cap_sha);
    if (st == SU_OK) {
        st = spool_put_admit(a, "source", lir, lir_len, src_sha);
    }
    free(cap);
    free(lir);
    if (st != SU_OK) {
        su_lir_module_free(&mod);
        return call_fail(c, st, st == SU_E_RESOURCE_LIMIT ? "spool budget exhausted; capsule not accepted"
                                                          : "candidate bytes could not be spooled");
    }
    if (strcmp(cap_sha, mod.capsule_hash_hex) != 0 || strcmp(src_sha, mod.source_hash_hex) != 0) {
        su_lir_module_free(&mod);
        return call_fail(c, SU_E_INTERNAL, "spooled bytes disagree with the verified hashes");
    }

    for (i = 0; i < AGENT_MAX_CANDIDATES; i++) {
        if (!a->cand[i].used) {
            slot = &a->cand[i];
            break;
        }
    }
    if (slot == NULL) {
        /* Bounded retention: evict the oldest candidate record (candidate work is synchronous, so
         * nothing in flight references it; a later lookup of the evicted id reports it unknown). */
        for (i = 0; i < AGENT_MAX_CANDIDATES; i++) {
            if (slot == NULL || a->cand[i].seq < slot->seq) {
                slot = &a->cand[i];
            }
        }
    }
    memset(slot, 0, sizeof *slot);
    slot->used = 1;
    a->next_candidate++;
    slot->seq = a->next_candidate;
    snprintf(slot->id, sizeof slot->id, "candidate-%llu", (unsigned long long)a->next_candidate);
    snprintf(slot->system_id, sizeof slot->system_id, "%s", mod.system_id);
    snprintf(slot->capsule_real, sizeof slot->capsule_real, "%s", cap_real);
    snprintf(slot->source_real, sizeof slot->source_real, "%s", src_real);
    snprintf(slot->source_hash, sizeof slot->source_hash, "%s", src_sha);
    snprintf(slot->capsule_hash, sizeof slot->capsule_hash, "%s", cap_sha);
    snprintf(slot->semantic_hash, sizeof slot->semantic_hash, "%s", mod.semantic_hash_hex);
    snprintf(slot->eval_status, sizeof slot->eval_status, "not_run");
    snprintf(slot->install_state, sizeof slot->install_state, "not_attempted");

    su_buf_str(res, "{\"candidate_id\":");
    su_buf_quoted(res, slot->id, strlen(slot->id));
    su_buf_str(res, ",\"status\":\"effects_verified\",\"system_id\":");
    su_buf_quoted(res, slot->system_id, strlen(slot->system_id));
    buf_hash_field(res, "source_hash", slot->source_hash);
    buf_hash_field(res, "capsule_hash", slot->capsule_hash);
    buf_hash_field(res, "semantic_hash", slot->semantic_hash);
    su_buf_str(res, ",\"artifact_format\":");
    su_buf_u64(res, mod.artifact_format);
    su_buf_str(res, "}");
    su_lir_module_free(&mod);
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

static AgentCandidate *find_candidate(SuAgent *a, const char *id) {
    size_t i;
    for (i = 0; i < AGENT_MAX_CANDIDATES; i++) {
        if (a->cand[i].used && strcmp(a->cand[i].id, id) == 0) {
            return &a->cand[i];
        }
    }
    return NULL;
}

static uint32_t h_candidate_status(SuAgent *a, const SuJsonValue *params, SuBuf *res,
                                   AgentCall *c) {
    static const char *const allowed[] = {"candidate_id"};
    const char *id = NULL;
    AgentCandidate *cd;

    if (check_keys(params, allowed, 1, c) != SU_OK || param_str(params, "candidate_id", 1, &id, c) != SU_OK) {
        return SU_E_TYPE;
    }
    cd = find_candidate(a, id);
    if (cd == NULL) {
        return call_fail(c, SU_E_INVALID_ARGUMENT, "unknown candidate_id");
    }
    su_buf_str(res, "{\"candidate_id\":");
    su_buf_quoted(res, cd->id, strlen(cd->id));
    su_buf_str(res, ",\"status\":\"effects_verified\",\"system_id\":");
    su_buf_quoted(res, cd->system_id, strlen(cd->system_id));
    buf_hash_field(res, "source_hash", cd->source_hash);
    buf_hash_field(res, "capsule_hash", cd->capsule_hash);
    buf_hash_field(res, "semantic_hash", cd->semantic_hash);
    su_buf_str(res, ",\"evaluation\":{\"status\":\"");
    su_buf_str(res, cd->eval_status);
    su_buf_str(res, "\",\"cases_total\":");
    su_buf_u64(res, cd->eval_total);
    su_buf_str(res, ",\"cases_passed\":");
    su_buf_u64(res, cd->eval_passed);
    su_buf_str(res, "},\"worker_pid\":");
    su_buf_u64(res, cd->worker_pid);
    su_buf_str(res, ",\"install\":{\"state\":\"");
    su_buf_str(res, cd->install_state);
    su_buf_str(res, "\",\"txid\":");
    su_buf_u64(res, cd->install_txid);
    su_buf_str(res, ",\"effective_tick\":");
    su_buf_u64(res, cd->install_tick);
    su_buf_str(res, ",\"error_status\":");
    su_buf_u64(res, cd->install_code);
    su_buf_str(res, ",\"reason\":");
    if (cd->quarantine_reason[0] != '\0') {
        su_buf_quoted(res, cd->quarantine_reason, strlen(cd->quarantine_reason));
    } else {
        su_buf_str(res, "null");
    }
    su_buf_str(res, "}}");
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* Copies the worker's failures array into the result as {name, reason} objects. */
static void copy_failures(const SuJsonValue *arr, SuBuf *res) {
    size_t i;
    su_buf_str(res, "[");
    if (arr != NULL && arr->type == SU_JSON_ARRAY) {
        for (i = 0; i < arr->u.array.count && i < 32u; i++) {
            const SuJsonValue *f = arr->u.array.items[i];
            const SuJsonValue *n = f != NULL && f->type == SU_JSON_OBJECT ? su_json_member(f, "name") : NULL;
            const SuJsonValue *r = f != NULL && f->type == SU_JSON_OBJECT ? su_json_member(f, "reason") : NULL;
            if (i > 0) {
                su_buf_str(res, ",");
            }
            su_buf_str(res, "{\"name\":");
            if (su_json_is_string(n)) {
                su_buf_quoted(res, n->u.string.data, n->u.string.len);
            } else {
                su_buf_str(res, "null");
            }
            su_buf_str(res, ",\"reason\":");
            if (su_json_is_string(r)) {
                su_buf_quoted(res, r->u.string.data, r->u.string.len);
            } else {
                su_buf_str(res, "null");
            }
            su_buf_str(res, "}");
        }
    }
    su_buf_str(res, "]");
}

static uint32_t h_candidate_evaluate(SuAgent *a, const SuJsonValue *params, SuBuf *res,
                                     AgentCall *c) {
    static const char *const allowed[] = {"candidate_id", "test_path", "timeout_ms"};
    const char *id = NULL;
    const char *test_rel = NULL;
    uint64_t timeout = a->timeout_ms;
    AgentCandidate *cd;
    char test_real[SU_PATH_BUF];
    char world_real[SU_PATH_BUF];
    char cap_check[SU_PATH_BUF];
    char eval_id[48];
    uint8_t *tdata = NULL;
    size_t tlen = 0;
    SuJsonLimits lim;
    SuJsonDoc doc;
    SuJsonError err;
    const char *cap_name;
    const char *world_name;
    SuBuf req;
    SuBuf resp;
    SuProc p;
    SuProcLimits plim;
    const SuJsonValue *root;
    const SuJsonValue *ok_v;
    uint32_t st;

    if (check_keys(params, allowed, 3, c) != SU_OK ||
        param_str(params, "candidate_id", 1, &id, c) != SU_OK ||
        param_str(params, "test_path", 1, &test_rel, c) != SU_OK ||
        param_uint(params, "timeout_ms", 0, AGENT_MIN_TIMEOUT_MS, AGENT_MAX_TIMEOUT_MS,
                   a->timeout_ms, &timeout, c) != SU_OK) {
        return SU_E_TYPE;
    }
    cd = find_candidate(a, id);
    if (cd == NULL) {
        return call_fail(c, SU_E_INVALID_ARGUMENT, "unknown candidate_id");
    }
    st = su_path_resolve(a->root, test_rel, test_real, SU_PATH_BUF);
    if (st != SU_OK) {
        return call_fail(c, st, "test path rejected by the project path policy");
    }
    st = su_path_read_file(test_real, AGENT_TEST_MAX, &tdata, &tlen);
    if (st != SU_OK) {
        return call_fail(c, st, "test file could not be read");
    }
    memset(&doc, 0, sizeof doc);
    lim.max_bytes = tlen;
    lim.max_depth = SU_PROTO_MAX_DEPTH;
    st = su_json_parse(tdata, tlen, &lim, &doc, &err);
    free(tdata);
    root = doc.root;
    if (st != SU_OK || root == NULL || root->type != SU_JSON_OBJECT) {
        su_json_doc_free(&doc);
        return call_fail(c, st != SU_OK ? st : SU_E_TYPE, "test file is malformed");
    }
    if (!su_json_is_string(su_json_member(root, "capsule")) ||
        !su_json_is_string(su_json_member(root, "world"))) {
        su_json_doc_free(&doc);
        return call_fail(c, SU_E_TYPE, "test file needs string fields capsule and world");
    }
    cap_name = su_json_member(root, "capsule")->u.string.data;
    world_name = su_json_member(root, "world")->u.string.data;
    st = su_path_sibling(a->root, test_real, cap_name, cap_check, sizeof cap_check);
    if (st != SU_OK || strcmp(cap_check, cd->capsule_real) != 0) {
        su_json_doc_free(&doc);
        return call_fail(c, SU_E_SCHEMA_MISMATCH, "test file does not name this candidate's capsule");
    }
    st = su_path_sibling(a->root, cd->capsule_real, world_name, world_real, sizeof world_real);
    su_json_doc_free(&doc);
    if (st != SU_OK) {
        return call_fail(c, st, "world fixture path rejected by the project path policy");
    }

    a->next_eval++;
    snprintf(eval_id, sizeof eval_id, "eval-%llu", (unsigned long long)a->next_eval);
    su_buf_init(&req);
    su_buf_str(&req, "{\"protocol\":\"" SU_PROTOCOL_VERSION "\",\"id\":");
    su_buf_quoted(&req, eval_id, strlen(eval_id));
    su_buf_str(&req, ",\"method\":\"internal.candidate.evaluate\",\"params\":{\"capsule_path\":");
    su_buf_quoted(&req, cd->capsule_real, strlen(cd->capsule_real));
    su_buf_str(&req, ",\"source_path\":");
    su_buf_quoted(&req, cd->source_real, strlen(cd->source_real));
    su_buf_str(&req, ",\"test_path\":");
    su_buf_quoted(&req, test_real, strlen(test_real));
    su_buf_str(&req, ",\"world_path\":");
    su_buf_quoted(&req, world_real, strlen(world_real));
    su_buf_str(&req, ",\"expected_source_hash\":");
    su_buf_quoted(&req, cd->source_hash, strlen(cd->source_hash));
    su_buf_str(&req, ",\"expected_capsule_hash\":");
    su_buf_quoted(&req, cd->capsule_hash, strlen(cd->capsule_hash));
    su_buf_str(&req, "}}");
    if (req.failed) {
        su_buf_free(&req);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "evaluation request too large");
    }

    /* Disposable worker: a fresh child per evaluation, never the active worker,
     * never this process. */
    if (a->cfg.worker_argv == NULL) {
        su_buf_free(&req);
        return call_fail(c, SU_E_WORKER_FAILED, "no worker executable is configured");
    }
    child_limits(a, &plim, 1);
    plim.cpu_seconds = (uint32_t)(timeout / 1000u) + 2u;
    if (su_proc_spawn(&p, a->cfg.worker_argv, &plim) != SU_OK) {
        su_buf_free(&req);
        cd->eval_status[0] = '\0';
        snprintf(cd->eval_status, sizeof cd->eval_status, "worker_failed");
        return call_fail(c, SU_E_WORKER_FAILED, "evaluation worker could not be started");
    }
    cd->eval_contain = p.containment;
    {
        const uint8_t *line = NULL;
        size_t len = 0;
        su_buf_init(&resp);
        cd->worker_pid = (uint64_t)p.pid;
        st = su_proc_request(&p, req.data, req.len, su_now_ms() + (int64_t)timeout, &line, &len);
        su_buf_free(&req);
        if (st != SU_OK) {
            /* su_proc_request has already killed and reaped the child. */
            snprintf(cd->eval_status, sizeof cd->eval_status, "worker_failed");
            su_buf_free(&resp);
            return call_fail(c, SU_E_WORKER_FAILED,
                             "evaluation worker was killed, crashed or exceeded its time limit");
        }
        su_buf_put(&resp, line, len);
    }
    su_proc_finish(&p, 1000);
    if (resp.failed) {
        su_buf_free(&resp);
        snprintf(cd->eval_status, sizeof cd->eval_status, "worker_failed");
        return call_fail(c, SU_E_WORKER_FAILED, "evaluation worker response was malformed");
    }

    memset(&doc, 0, sizeof doc);
    lim.max_bytes = resp.len;
    lim.max_depth = SU_PROTO_MAX_DEPTH + 4u;
    st = su_json_parse(resp.data, resp.len, &lim, &doc, &err);
    root = st == SU_OK && doc.root != NULL && doc.root->type == SU_JSON_OBJECT ? doc.root : NULL;
    ok_v = root != NULL ? su_json_member(root, "ok") : NULL;
    if (root == NULL || ok_v == NULL || ok_v->type != SU_JSON_BOOL) {
        su_json_doc_free(&doc);
        su_buf_free(&resp);
        snprintf(cd->eval_status, sizeof cd->eval_status, "worker_failed");
        return call_fail(c, SU_E_WORKER_FAILED, "evaluation worker response was malformed");
    }

    su_buf_str(res, "{\"candidate_id\":");
    su_buf_quoted(res, cd->id, strlen(cd->id));
    su_buf_str(res, ",\"evaluation_id\":");
    su_buf_quoted(res, eval_id, strlen(eval_id));
    su_buf_str(res, ",\"containment\":");
    put_containment(res, &cd->eval_contain);
    if (!ok_v->u.boolean) {
        const SuJsonValue *e = su_json_member(root, "error");
        const SuJsonValue *m = e != NULL && e->type == SU_JSON_OBJECT ? su_json_member(e, "message") : NULL;
        const SuJsonValue *cd_v = e != NULL && e->type == SU_JSON_OBJECT ? su_json_member(e, "code") : NULL;
        su_buf_str(res, ",\"status\":\"failed\",\"cases_total\":0,\"cases_passed\":0,\"diagnostic\":{\"code\":");
        if (su_json_is_string(cd_v)) {
            su_buf_quoted(res, cd_v->u.string.data, cd_v->u.string.len);
        } else {
            su_buf_str(res, "null");
        }
        su_buf_str(res, ",\"message\":");
        if (su_json_is_string(m)) {
            su_buf_quoted(res, m->u.string.data, m->u.string.len);
        } else {
            su_buf_str(res, "null");
        }
        su_buf_str(res, "}}");
        snprintf(cd->eval_status, sizeof cd->eval_status, "failed");
        cd->eval_total = 0;
        cd->eval_passed = 0;
    } else {
        const SuJsonValue *r = su_json_member(root, "result");
        const SuJsonValue *s_v = r != NULL && r->type == SU_JSON_OBJECT ? su_json_member(r, "status") : NULL;
        uint64_t total = 0;
        uint64_t passed = 0;
        if (r == NULL || !su_json_is_string(s_v) || !su_json_get_uint(su_json_member(r, "cases_total"), &total) ||
            !su_json_get_uint(su_json_member(r, "cases_passed"), &passed)) {
            su_json_doc_free(&doc);
            su_buf_free(&resp);
            snprintf(cd->eval_status, sizeof cd->eval_status, "worker_failed");
            return call_fail(c, SU_E_WORKER_FAILED, "evaluation worker result was malformed");
        }
        su_buf_str(res, ",\"status\":");
        su_buf_quoted(res, s_v->u.string.data, s_v->u.string.len);
        su_buf_str(res, ",\"cases_total\":");
        su_buf_u64(res, total);
        su_buf_str(res, ",\"cases_passed\":");
        su_buf_u64(res, passed);
        su_buf_str(res, ",\"failures\":");
        copy_failures(su_json_member(r, "failures"), res);
        su_buf_str(res, "}");
        snprintf(cd->eval_status, sizeof cd->eval_status, "%s",
                 s_v->u.string.len == 6 && memcmp(s_v->u.string.data, "passed", 6) == 0 ? "passed" : "failed");
        cd->eval_total = total;
        cd->eval_passed = passed;
    }
    su_json_doc_free(&doc);
    su_buf_free(&resp);
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* Relays a worker response line unchanged. */
static void relay(SuBuf *res, const SuBuf *resp, AgentCall *c) {
    su_buf_put(res, resp->data, resp->len);
    c->relay = 1;
}

/* Relays a successful worker envelope {"protocol":..,"id":..,"ok":true,"result":{..}} with a
 * "containment" member added to its result object. The result object is the envelope's last member,
 * so the envelope ends in "}}"; those two closing braces are replaced. Other answers relay unchanged. */
#ifdef SU_HAVE_NATIVE
static void relay_with_containment(SuBuf *res, const SuBuf *resp, int ok, const SuProcContainment *ct,
                                   AgentCall *c) {
    if (!ok || resp->len < 3u || resp->data[resp->len - 1u] != '}' || resp->data[resp->len - 2u] != '}') {
        relay(res, resp, c);
        return;
    }
    su_buf_put(res, resp->data, resp->len - 2u);
    su_buf_str(res, ",\"containment\":");
    put_containment(res, ct);
    su_buf_str(res, "}}");
    c->relay = 1;
}
#endif

/* A call that must find its world: unknown ids and unrecoverable worlds are answered here. */
static uint32_t world_for_call(SuAgent *a, const char *id, AgentWorld **out, AgentCall *c) {
    AgentWorld *w = find_world(a, id);
    if (w == NULL) {
        return call_fail(c, SU_E_INVALID_ARGUMENT, "unknown world_id");
    }
    if (w->unrecoverable) {
        char msg[AGENT_MSG_CAP];
        snprintf(msg, sizeof msg, "world is unrecoverable: %.300s", w->reason);
        return call_fail(c, SU_E_INTERNAL, msg);
    }
    *out = w;
    return SU_OK;
}

/* Forced checkpoint before an operation that would overflow the log. */
static uint32_t log_room(SuAgent *a, AgentCall *c, AgentWorld *w) {
    InFlight none;
    int rc;
    if (w->log_count < w->log_cap) {
        return SU_OK;
    }
    rc = checkpoint_world(a, w, 0);
    if (rc == CK_DIED) {
        memset(&none, 0, sizeof none);
        return worker_died(a, c, &none);
    }
    if (rc == CK_NOROOM) {
        return call_fail(c, SU_E_RESOURCE_LIMIT, "checkpoint budget exhausted; the replay log is full");
    }
    if (rc == CK_BAD) {
        return call_fail(c, SU_E_INTERNAL, "checkpoint disagreed with the acknowledged state; world is unrecoverable");
    }
    return SU_OK;
}

/* world.create takes exactly one of capsule_path (one system) or capsule_paths (an ordered array of
 * 1..8 systems, run in that order every tick). */
static uint32_t h_world_create(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"fixture_path", "capsule_path", "capsule_paths", "max_entities",
                                          "max_struct_per_tick", "max_events_per_tick", "event_log_max"};
    const char *fixture = NULL;
    const char *one = NULL;
    const SuJsonValue *many = su_json_member(params, "capsule_paths");
    uint64_t cfgv[4] = {0, SU_CMDQ_DEFAULT_PER_TICK_CAP, SU_CMDQ_DEFAULT_PER_TICK_CAP,
                        SU_CMDQ_DEFAULT_EVENT_LOG_MAX};
    char fx_real[SU_PATH_BUF];
    char fx_sha[65];
    AgentSystemPaths *sys = NULL;
    SysRef *refs = NULL;
    uint8_t *fx = NULL;
    size_t fx_len = 0;
    size_t n = 0;
    size_t i;
    SuBuf req;
    SuBuf resp;
    AgentWorld *slot;
    Reply r;
    uint32_t st;
    int rc;
    char rv[AGENT_REV_CAP];
    char wid[AGENT_WORLD_ID_CAP];

    if (check_keys(params, allowed, 7, c) != SU_OK ||
        param_str(params, "fixture_path", 1, &fixture, c) != SU_OK ||
        param_str(params, "capsule_path", 0, &one, c) != SU_OK ||
        /* Integer type only here; the worker owns the range checks (SU_E_RESOURCE_LIMIT). */
        param_uint(params, "max_entities", 0, 0, UINT64_MAX, 0, &cfgv[0], c) != SU_OK ||
        param_uint(params, "max_struct_per_tick", 0, 0, UINT64_MAX, SU_CMDQ_DEFAULT_PER_TICK_CAP, &cfgv[1],
                   c) != SU_OK ||
        param_uint(params, "max_events_per_tick", 0, 0, UINT64_MAX, SU_CMDQ_DEFAULT_PER_TICK_CAP, &cfgv[2],
                   c) != SU_OK ||
        param_uint(params, "event_log_max", 0, 0, UINT64_MAX, SU_CMDQ_DEFAULT_EVENT_LOG_MAX, &cfgv[3], c) !=
            SU_OK) {
        return SU_E_TYPE;
    }
    if ((one != NULL) == (many != NULL)) {
        return call_fail(c, SU_E_TYPE, "give exactly one of capsule_path or capsule_paths");
    }
    if (many != NULL) {
        if (many->type != SU_JSON_ARRAY || many->u.array.count == 0 || many->u.array.count > AGENT_MAX_SYSTEMS) {
            return call_fail(c, SU_E_TYPE, "capsule_paths must be an array of 1..8 path strings");
        }
        n = many->u.array.count;
    } else {
        n = 1;
    }
    slot = free_world_slot(a);
    if (slot == NULL) {
        return call_fail(c, SU_E_RESOURCE_LIMIT, "worker already holds the maximum number of worlds");
    }
    sys = calloc(n, sizeof *sys);
    refs = calloc(n, sizeof *refs);
    if (sys == NULL || refs == NULL) {
        free(sys);
        free(refs);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "allocation failed");
    }
    st = su_path_resolve(a->root, fixture, fx_real, sizeof fx_real);
    if (st != SU_OK) {
        free(sys);
        free(refs);
        return call_fail(c, st, "fixture path rejected by the project path policy");
    }
    st = su_path_read_file(fx_real, AGENT_FIXTURE_MAX, &fx, &fx_len);
    if (st != SU_OK) {
        free(sys);
        free(refs);
        return call_fail(c, st, "world fixture could not be read");
    }
    {
        SuSha256 ctx;
        uint8_t dig[32];
        su_sha256_init(&ctx);
        su_sha256_update(&ctx, fx, fx_len);
        su_sha256_final(&ctx, dig);
        su_sha256_hex(dig, fx_sha);
    }
    free(fx);
    for (i = 0; i < n; i++) {
        char rel_buf[AGENT_MAX_PARAM_STRING + 1];
        const char *rel = one;
        uint8_t *cap = NULL;
        size_t cap_len = 0;
        uint8_t *src = NULL;
        size_t src_len = 0;
        if (many != NULL) {
            const SuJsonValue *item = many->u.array.items[i];
            if (!su_json_is_string(item) || item->u.string.len == 0 || item->u.string.len > AGENT_MAX_PARAM_STRING) {
                st = SU_E_TYPE;
                call_fail(c, st, "capsule_paths entries must be 1..255 byte strings");
                goto fail_early;
            }
            memcpy(rel_buf, item->u.string.data, item->u.string.len);
            rel_buf[item->u.string.len] = '\0';
            rel = rel_buf;
        }
        st = locate_capsule(a, rel, sys[i].capsule, sys[i].source, &cap, &cap_len, c);
        if (st != SU_OK) {
            goto fail_early;
        }
        st = su_path_read_file(sys[i].source, SU_LIMIT_IR_SOURCE_BYTES, &src, &src_len);
        if (st != SU_OK) {
            free(cap);
            call_fail(c, st, "source module could not be read");
            goto fail_early;
        }
        st = spool_put_admit(a, "capsule", cap, cap_len, refs[i].capsule_sha);
        if (st == SU_OK) {
            st = spool_put_admit(a, "source", src, src_len, refs[i].source_sha);
        }
        free(cap);
        free(src);
        if (st != SU_OK) {
            call_fail(c, st, "spool budget exhausted; world not created");
            goto fail_early;
        }
    }

    /* The record is pending until the worker has confirmed it. Recovery skips pending records. */
    memset(slot, 0, sizeof *slot);
    slot->used = 1;
    slot->pending = 1;
    snprintf(slot->fixture_path, sizeof slot->fixture_path, "%s", fx_real);
    snprintf(slot->fixture_sha, sizeof slot->fixture_sha, "%s", fx_sha);
    slot->creation_count = (uint32_t)n;
    memcpy(slot->cfg, cfgv, sizeof cfgv);
    memcpy(slot->creation, refs, n * sizeof refs[0]);
    for (i = 0; i < n; i++) {
        snprintf(slot->creation_paths[i].capsule, sizeof slot->creation_paths[i].capsule, "%s", sys[i].capsule);
        snprintf(slot->creation_paths[i].source, sizeof slot->creation_paths[i].source, "%s", sys[i].source);
    }
    slot->log_cap = a->log_cap;
    slot->log = calloc(a->log_cap, sizeof *slot->log);
    if (slot->log == NULL) {
        memset(slot, 0, sizeof *slot);
        call_fail(c, SU_E_RESOURCE_LIMIT, "allocation failed");
        goto fail_early;
    }

    su_buf_init(&req);
    req_begin(&req, c->id, "world.create");
    su_buf_str(&req, "\"fixture_path\":");
    su_buf_quoted(&req, fx_real, strlen(fx_real));
    su_buf_str(&req, ",\"fixture_sha256\":");
    su_buf_quoted(&req, fx_sha, strlen(fx_sha));
    su_buf_str(&req, ",\"max_entities\":");
    su_buf_u64(&req, cfgv[0]);
    su_buf_str(&req, ",\"max_struct_per_tick\":");
    su_buf_u64(&req, cfgv[1]);
    su_buf_str(&req, ",\"max_events_per_tick\":");
    su_buf_u64(&req, cfgv[2]);
    su_buf_str(&req, ",\"event_log_max\":");
    su_buf_u64(&req, cfgv[3]);
    st = write_capsules(a, &req, "capsules", refs, (uint32_t)n);
    req_end(&req);
    if (st != SU_OK || req.failed) {
        su_buf_free(&req);
        world_release(a, slot);
        free(sys);
        free(refs);
        return call_fail(c, st != SU_OK ? st : SU_E_RESOURCE_LIMIT, "world.create request too large");
    }
    su_buf_init(&resp);
    st = roundtrip(a, c->id, &req, &resp);
    su_buf_free(&req);
    free(sys);
    free(refs);
    if (st != SU_OK) {
        su_buf_free(&resp);
        world_release(a, slot);
        return worker_died(a, c, NULL);
    }
    reply_parse(&r, resp.data, resp.len);
    if (!r.ok) {
        /* The worker's own refusal (for example a capsule that does not fit the schema) is relayed. */
        reply_free(&r);
        world_release(a, slot);
        relay(res, &resp, c);
        su_buf_free(&resp);
        return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
    }
    if (!jstr(r.result, "world_id", wid, sizeof wid) || !jstr(r.result, "world_revision", rv, sizeof rv) ||
        wid_num(wid) == 0) {
        reply_free(&r);
        su_buf_free(&resp);
        world_release(a, slot);
        return call_fail(c, SU_E_INTERNAL, "worker returned an unusable world identity");
    }
    {
        const SuJsonValue *man = jf(r.result, "semantic_manifest");
        uint32_t m;
        if (man == NULL || man->type != SU_JSON_ARRAY || man->u.array.count != n) {
            reply_free(&r);
            su_buf_free(&resp);
            world_release(a, slot);
            return call_fail(c, SU_E_INTERNAL, "worker manifest does not list the requested systems");
        }
        snprintf(slot->id, sizeof slot->id, "%s", wid);
        slot->seq = wid_num(wid);
        slot->rev = rev_num(rv);
        slot->tick = 0;
        slot->count = (uint32_t)n;
        for (m = 0; m < n; m++) {
            const SuJsonValue *it = man->u.array.items[m];
            SysRef *s = &slot->sys[m];
            if (!jstr(it, "system_id", s->system_id, sizeof s->system_id) ||
                !jstr(it, "semantic_hash", s->semantic_hash, sizeof s->semantic_hash) ||
                !jstr(it, "source_hash", s->source_sha, sizeof s->source_sha) ||
                !jstr(it, "capsule_hash", s->capsule_sha, sizeof s->capsule_sha) ||
                !ju64(it, "system_revision", &s->system_rev) ||
                strcmp(s->source_sha, slot->creation[m].source_sha) != 0 ||
                strcmp(s->capsule_sha, slot->creation[m].capsule_sha) != 0) {
                reply_free(&r);
                su_buf_free(&resp);
                world_release(a, slot);
                return call_fail(c, SU_E_INTERNAL, "worker manifest disagrees with the creation parameters");
            }
            snprintf(slot->creation[m].system_id, sizeof slot->creation[m].system_id, "%s", s->system_id);
        }
    }
    reply_free(&r);

    /* The creation state is the first checkpoint. The worker world is closed again if the
     * supervisor cannot record it. */
    rc = checkpoint_world(a, slot, 1);
    if (rc != CK_OK) {
        su_buf_free(&resp);
        if (rc != CK_DIED) {
            SuBuf cl;
            SuBuf cresp;
            su_buf_init(&cl);
            req_begin(&cl, c->id, "world.close");
            su_buf_str(&cl, "\"world_id\":");
            su_buf_quoted(&cl, slot->id, strlen(slot->id));
            su_buf_str(&cl, ",\"expected_world_revision\":");
            wrev(slot, slot->rev, rv, sizeof rv);
            su_buf_quoted(&cl, rv, strlen(rv));
            req_end(&cl);
            su_buf_init(&cresp);
            (void)roundtrip(a, c->id, &cl, &cresp);
            su_buf_free(&cl);
            su_buf_free(&cresp);
        }
        world_release(a, slot);
        if (rc == CK_DIED) {
            return worker_died(a, c, NULL);
        }
        return call_fail(c, rc == CK_NOROOM ? SU_E_RESOURCE_LIMIT : SU_E_INTERNAL,
                         rc == CK_NOROOM ? "spool budget exhausted; world not created"
                                         : "creation checkpoint disagreed with the worker");
    }
    slot->pending = 0;
    if (slot->seq > a->next_world_seq) {
        a->next_world_seq = slot->seq;
    }
    relay(res, &resp, c);
    su_buf_free(&resp);
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;

fail_early:
    free(sys);
    free(refs);
    return st != SU_OK ? st : SU_E_INTERNAL;
}

static void attach_member(SuBuf *out, const char *key, const SuBuf *val);

/* Adds "specialization" to a world.inspect answer: whether respecialization is pending for the world,
 * its last outcome, and the quarantine table (M1-02c-2). */
static void attach_spec_status(SuAgent *a, const AgentWorld *w, SuBuf *res) {
    SuBuf v;
    uint32_t i;
    int pending = 0;
    int n = 0;
    for (i = 0; i < a->respec_count; i++) {
        if (strcmp(a->respec[i].world_id, w->id) == 0) {
            pending = 1;
        }
    }
    su_buf_init(&v);
    su_buf_str(&v, "{\"pending_respecialization\":");
    su_buf_str(&v, pending ? "true" : "false");
    su_buf_str(&v, ",\"respecialization\":");
    su_buf_quoted(&v, w->respec_state[0] != '\0' ? w->respec_state : "none",
                  w->respec_state[0] != '\0' ? strlen(w->respec_state) : 4u);
    su_buf_str(&v, ",\"reason\":");
    su_buf_quoted(&v, w->respec_reason, strlen(w->respec_reason));
    su_buf_str(&v, ",\"quarantined\":[");
    for (i = 0; i < AGENT_MAX_QUARANTINE; i++) {
        const QRec *q = &a->quar[i];
        if (!q->used) {
            continue;
        }
        if (n > 0) {
            su_buf_str(&v, ",");
        }
        su_buf_str(&v, "{\"artifact_hash\":");
        su_buf_quoted(&v, q->artifact_hash, strlen(q->artifact_hash));
        su_buf_str(&v, ",\"world_id\":");
        su_buf_quoted(&v, q->world_id, strlen(q->world_id));
        su_buf_str(&v, ",\"system_id\":");
        su_buf_quoted(&v, q->system_id, strlen(q->system_id));
        su_buf_str(&v, ",\"backend\":");
        su_buf_quoted(&v, q->backend, strlen(q->backend));
        su_buf_str(&v, ",\"tier\":");
        su_buf_quoted(&v, q->tier, strlen(q->tier));
        su_buf_str(&v, ",\"state\":\"quarantined\",\"reason\":");
        su_buf_quoted(&v, q->reason, strlen(q->reason));
        su_buf_str(&v, ",\"crash_count\":");
        su_buf_u64(&v, q->crashes);
        su_buf_str(&v, ",\"replaced_pid\":");
        su_buf_u64(&v, q->replaced_pid);
        su_buf_str(&v, ",\"tick\":");
        su_buf_u64(&v, q->tick);
        su_buf_str(&v, "}");
        n++;
    }
    su_buf_str(&v, "],\"quarantine_table_full\":");
    su_buf_str(&v, a->quar_overflow ? "true" : "false");
    su_buf_str(&v, "}");
    attach_member(res, "specialization", &v);
    su_buf_free(&v);
}

static uint32_t h_world_inspect(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"world_id", "fields", "limit", "offset", "entity"};
    const char *world_id = NULL;
    uint64_t limit = 16;
    uint64_t offset = 0;
    uint64_t entity = 0;
    const SuJsonValue *fields = su_json_member(params, "fields");
    const SuJsonValue *ent_h = su_json_member(params, "entity");
    SuBuf req;
    SuBuf resp;
    AgentWorld *w = NULL;
    InFlight op;
    size_t i;
    uint32_t st;

    if (check_keys(params, allowed, 5, c) != SU_OK ||
        param_str(params, "world_id", 1, &world_id, c) != SU_OK ||
        param_uint(params, "limit", 0, 1, 256, 16, &limit, c) != SU_OK ||
        param_uint(params, "offset", 0, 0, AGENT_MAX_OFFSET, 0, &offset, c) != SU_OK) {
        return SU_E_TYPE;
    }
    if (ent_h != NULL && !su_json_get_uint(ent_h, &entity)) {
        return call_fail(c, SU_E_TYPE, "entity must be an integer handle");
    }
    if (fields != NULL && (fields->type != SU_JSON_ARRAY || fields->u.array.count > AGENT_MAX_FIELD_ARGS)) {
        return call_fail(c, SU_E_TYPE, "fields must be an array of at most 64 names");
    }
    st = world_for_call(a, world_id, &w, c);
    if (st != SU_OK) {
        return st;
    }
    st = preflight(a, c);
    if (st != SU_OK) {
        return st;
    }
    req_begin(&req, c->id, "world.inspect");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, world_id, strlen(world_id));
    su_buf_str(&req, ",\"limit\":");
    su_buf_u64(&req, limit);
    su_buf_str(&req, ",\"offset\":");
    su_buf_u64(&req, offset);
    if (ent_h != NULL) {
        su_buf_str(&req, ",\"entity\":");
        su_buf_u64(&req, entity);
    }
    if (fields != NULL) {
        su_buf_str(&req, ",\"fields\":[");
        for (i = 0; i < fields->u.array.count; i++) {
            const SuJsonValue *f = fields->u.array.items[i];
            if (!su_json_is_string(f) || f->u.string.len == 0 || f->u.string.len > AGENT_MAX_PARAM_STRING) {
                su_buf_free(&req);
                return call_fail(c, SU_E_TYPE, "fields must be strings of 1..255 bytes");
            }
            if (i > 0) {
                su_buf_str(&req, ",");
            }
            su_buf_quoted(&req, f->u.string.data, f->u.string.len);
        }
        su_buf_str(&req, "]");
    }
    req_end(&req);
    if (req.failed) {
        su_buf_free(&req);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "world.inspect request too large");
    }
    su_buf_init(&resp);
    memset(&op, 0, sizeof op);
    op.kind = OPK_READ;
    st = roundtrip(a, c->id, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return worker_died(a, c, &op);
    }
    relay(res, &resp, c);
    su_buf_free(&resp);
    attach_spec_status(a, w, res);
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* world.step {world_id, expected_world_revision, ticks}. The worker's answer is relayed unchanged,
 * including an error. Whatever the worker committed (a failed step may have committed some ticks)
 * is digested and logged before the answer is sent. */
static uint32_t h_world_step(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"world_id", "expected_world_revision", "ticks"};
    const char *world_id = NULL;
    const char *expect = NULL;
    uint64_t ticks = 0;
    AgentWorld *w = NULL;
    InFlight op;
    SuBuf req;
    SuBuf resp;
    Reply r;
    LogOp none;
    uint32_t st;
    int rc;

    if (check_keys(params, allowed, 3, c) != SU_OK ||
        param_str(params, "world_id", 1, &world_id, c) != SU_OK ||
        param_str(params, "expected_world_revision", 1, &expect, c) != SU_OK ||
        param_uint(params, "ticks", 1, 0, AGENT_MAX_TICKS, 0, &ticks, c) != SU_OK) {
        return SU_E_TYPE;
    }
    st = world_for_call(a, world_id, &w, c);
    if (st != SU_OK) {
        return st;
    }
    st = preflight(a, c);
    if (st != SU_OK) {
        return st;
    }
    memset(&op, 0, sizeof op);
    op.kind = OPK_STEP;
    op.w = w;
    op.tick = w->tick;
    st = log_room(a, c, w);
    if (st != SU_OK) {
        return st;
    }
    req_begin(&req, c->id, "world.step");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, world_id, strlen(world_id));
    su_buf_str(&req, ",\"expected_world_revision\":");
    su_buf_quoted(&req, expect, strlen(expect));
    su_buf_str(&req, ",\"ticks\":");
    su_buf_u64(&req, ticks);
    append_tick_opts(&req, a);
    req_end(&req);
    if (req.failed) {
        su_buf_free(&req);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "world.step request too large");
    }
    su_buf_init(&resp);
    st = roundtrip(a, c->id, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return worker_died(a, c, &op);
    }
    reply_parse(&r, resp.data, resp.len);
    memset(&none, 0, sizeof none);
    rc = sync_after(a, w, LOG_STEP, &none, 0, r.ok);
    reply_free(&r);
    if (rc == CK_DIED) {
        su_buf_free(&resp);
        return worker_died(a, c, &op);
    }
    if (rc == CK_BAD) {
        su_buf_free(&resp);
        return call_fail(c, SU_E_INTERNAL, "world state diverged from the worker; world is unrecoverable");
    }
    relay(res, &resp, c);
    su_buf_free(&resp);
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* world.snapshot {world_id, expected_world_revision}. The canonical state becomes a checkpoint:
 * the snapshot id is its sha256, and the replay log is cleared because the checkpoint supersedes it. */
static uint32_t h_world_snapshot(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"world_id", "expected_world_revision"};
    const char *world_id = NULL;
    const char *expect = NULL;
    AgentWorld *w = NULL;
    InFlight op;
    char rev[AGENT_REV_CAP];
    uint32_t st;
    int rc;

    if (check_keys(params, allowed, 2, c) != SU_OK ||
        param_str(params, "world_id", 1, &world_id, c) != SU_OK ||
        param_str(params, "expected_world_revision", 1, &expect, c) != SU_OK) {
        return SU_E_TYPE;
    }
    st = world_for_call(a, world_id, &w, c);
    if (st != SU_OK) {
        return st;
    }
    st = preflight(a, c);
    if (st != SU_OK) {
        return st;
    }
    wrev(w, w->rev, rev, sizeof rev);
    if (strcmp(expect, rev) != 0) {
        return call_fail(c, SU_E_STALE_REVISION, "expected_world_revision does not match the active world");
    }
    rc = checkpoint_world(a, w, 0);
    if (rc == CK_DIED) {
        memset(&op, 0, sizeof op);
        op.kind = OPK_READ;
        return worker_died(a, c, &op);
    }
    if (rc != CK_OK) {
        return call_fail(c, rc == CK_NOROOM ? SU_E_RESOURCE_LIMIT : SU_E_INTERNAL,
                         rc == CK_NOROOM ? "snapshot does not fit the spool budget" : "snapshot disagreed with the acknowledged state");
    }
    w->ck[w->ck_count - 1].client = 1;
    {
        const Ckpt *ck = &w->ck[w->ck_count - 1];
        su_buf_str(res, "{\"snapshot_id\":");
        su_buf_quoted(res, ck->sha, strlen(ck->sha));
        su_buf_str(res, ",\"world_revision\":");
        wrev(w, ck->rev, rev, sizeof rev);
        su_buf_quoted(res, rev, strlen(rev));
        su_buf_str(res, ",\"tick\":");
        su_buf_u64(res, ck->tick);
        su_buf_str(res, ",\"bytes\":");
        su_buf_u64(res, ck->bytes);
        su_buf_str(res, ",\"sha256\":");
        su_buf_quoted(res, ck->sha, strlen(ck->sha));
        su_buf_str(res, ",\"capture_ns\":null}");
    }
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* world.restore {world_id, snapshot_id, expected_world_revision}. Only checkpoints the supervisor
 * retains can be restored (an older or evicted snapshot id is SU_E_INVALID_ARGUMENT). The restored
 * state is checkpointed at once, so the log restarts from it; the worker advances the revision as
 * before. */
static uint32_t h_world_restore(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"world_id", "snapshot_id", "expected_world_revision"};
    const char *world_id = NULL;
    const char *snap = NULL;
    const char *expect = NULL;
    AgentWorld *w = NULL;
    const Ckpt *ck = NULL;
    uint32_t k;
    InFlight op;
    char snap_path[SU_PATH_BUF];
    SuBuf req;
    SuBuf resp;
    Reply r;
    LogOp none;
    uint32_t st;
    int rc;
    char rv[AGENT_REV_CAP];

    if (check_keys(params, allowed, 3, c) != SU_OK ||
        param_str(params, "world_id", 1, &world_id, c) != SU_OK ||
        param_str(params, "snapshot_id", 1, &snap, c) != SU_OK ||
        param_str(params, "expected_world_revision", 1, &expect, c) != SU_OK) {
        return SU_E_TYPE;
    }
    st = world_for_call(a, world_id, &w, c);
    if (st != SU_OK) {
        return st;
    }
    st = preflight(a, c);
    if (st != SU_OK) {
        return st;
    }
    wrev(w, w->rev, rv, sizeof rv);
    if (strcmp(expect, rv) != 0) {
        return call_fail(c, SU_E_STALE_REVISION, "expected_world_revision does not match the active world");
    }
    for (k = w->ck_count; k > 0 && ck == NULL; k--) {
        if (strcmp(w->ck[k - 1].sha, snap) == 0) {
            ck = &w->ck[k - 1];
        }
    }
    if (ck == NULL) {
        return call_fail(c, SU_E_INVALID_ARGUMENT, "unknown or evicted snapshot_id for this world");
    }
    if (spool_file(a, ck->sha, "snap", snap_path, sizeof snap_path) != SU_OK) {
        return call_fail(c, SU_E_INTERNAL, "snapshot bytes are missing from the spool");
    }
    req_begin(&req, c->id, "internal.world.restore_bytes");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, world_id, strlen(world_id));
    su_buf_str(&req, ",\"expected_world_revision\":");
    su_buf_quoted(&req, expect, strlen(expect));
    su_buf_str(&req, ",\"snapshot_path\":");
    su_buf_quoted(&req, snap_path, strlen(snap_path));
    su_buf_str(&req, ",\"snapshot_sha256\":");
    su_buf_quoted(&req, ck->sha, strlen(ck->sha));
    su_buf_str(&req, ",\"mode\":\"advance\"");
    if (write_capsules_disk(a, w, &req, "capsules", ck->sys, ck->count) != SU_OK) {
        su_buf_free(&req);
        return call_fail(c, SU_E_INTERNAL, "snapshot capsules are missing from the spool");
    }
    req_end(&req);
    if (req.failed) {
        su_buf_free(&req);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "world.restore request too large");
    }
    su_buf_init(&resp);
    st = roundtrip(a, c->id, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        memset(&op, 0, sizeof op);
        op.kind = OPK_RESTORE;
        op.w = w;
        op.tick = w->tick;
        return worker_died(a, c, &op);
    }
    reply_parse(&r, resp.data, resp.len);
    if (!r.ok) {
        reply_free(&r);
        relay(res, &resp, c);
        su_buf_free(&resp);
        return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
    }
    /* The restored schedule is the snapshot's; the revision is the worker's. */
    if (!jstr(r.result, "world_revision", rv, sizeof rv)) {
        reply_free(&r);
        su_buf_free(&resp);
        mark_unrecoverable(w, "restore answer carried no revision");
        return call_fail(c, SU_E_INTERNAL, "world is unrecoverable: restore answer carried no revision");
    }
    reply_free(&r);
    w->rev = rev_num(rv);
    w->count = ck->count;
    memcpy(w->sys, ck->sys, sizeof w->sys);
    memset(&none, 0, sizeof none);
    rc = sync_after(a, w, LOG_NONE, &none, 1, 1);
    if (rc == CK_DIED) {
        su_buf_free(&resp);
        memset(&op, 0, sizeof op);
        op.kind = OPK_RESTORE;
        op.w = w;
        op.tick = w->tick;
        return worker_died(a, c, &op);
    }
    if (rc == CK_BAD) {
        su_buf_free(&resp);
        return call_fail(c, SU_E_INTERNAL, "restored state diverged from the snapshot; world is unrecoverable");
    }
    relay(res, &resp, c);
    su_buf_free(&resp);
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* world.close {world_id, expected_world_revision}. The worker checks first; on success the world's
 * records are released and the slot is free. */
static uint32_t h_world_close(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"world_id", "expected_world_revision"};
    const char *world_id = NULL;
    const char *expect = NULL;
    AgentWorld *w = NULL;
    InFlight op;
    SuBuf req;
    SuBuf resp;
    Reply r;
    uint32_t st;

    if (check_keys(params, allowed, 2, c) != SU_OK ||
        param_str(params, "world_id", 1, &world_id, c) != SU_OK ||
        param_str(params, "expected_world_revision", 1, &expect, c) != SU_OK) {
        return SU_E_TYPE;
    }
    w = find_world(a, world_id);
    if (w == NULL) {
        return call_fail(c, SU_E_INVALID_ARGUMENT, "unknown world_id");
    }
    st = preflight(a, c);
    if (st != SU_OK) {
        return st;
    }
    req_begin(&req, c->id, "world.close");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, world_id, strlen(world_id));
    su_buf_str(&req, ",\"expected_world_revision\":");
    su_buf_quoted(&req, expect, strlen(expect));
    req_end(&req);
    if (req.failed) {
        su_buf_free(&req);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "world.close request too large");
    }
    su_buf_init(&resp);
    st = roundtrip(a, c->id, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        memset(&op, 0, sizeof op);
        op.kind = OPK_CLOSE;
        op.w = w;
        op.tick = w->tick;
        return worker_died(a, c, &op);
    }
    reply_parse(&r, resp.data, resp.len);
    if (r.ok) {
        uint64_t systems = 0;
        uint32_t snapshots = 0;
        uint32_t q;
        for (q = 0; q < w->ck_count; q++) {
            snapshots += w->ck[q].client ? 1u : 0u;
        }
        ju64(jf(r.result, "released"), "systems", &systems);
        world_release(a, w);
        su_buf_str(res, "{\"world_id\":");
        su_buf_quoted(res, world_id, strlen(world_id));
        su_buf_str(res, ",\"closed\":true,\"released\":{\"snapshots\":");
        su_buf_u64(res, snapshots);
        su_buf_str(res, ",\"systems\":");
        su_buf_u64(res, systems);
        su_buf_str(res, "}}");
        reply_free(&r);
        su_buf_free(&resp);
        return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
    }
    reply_free(&r);
    nat_clear(w);
    respec_drop_world(a, w);
    relay(res, &resp, c);
    su_buf_free(&resp);
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* candidate.install {candidate_id, world_id, expected_world_revision, allowed_effective_tick}.
 * The evidence the worker gates on is the supervisor's evaluation record of this candidate
 * (eval_status), never a client-supplied value. The worker's answer is relayed unchanged. The
 * installed revision is logged, and the world is checkpointed after it (see install_ck). */
static uint32_t h_candidate_install(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"candidate_id", "world_id", "expected_world_revision",
                                          "allowed_effective_tick"};
    const char *id = NULL;
    const char *world_id = NULL;
    const char *expect = NULL;
    uint64_t tick = 0;
    AgentCandidate *cd;
    AgentWorld *w = NULL;
    InFlight op;
    LogOp lop;
    SuBuf req;
    SuBuf resp;
    Reply r;
    char cap[SU_PATH_BUF];
    char src[SU_PATH_BUF];
    char txt[65];
    char sysid[SU_DIAG_SYSTEM_BYTES];
    uint32_t i;
    uint32_t st;
    int rc;

    if (check_keys(params, allowed, 4, c) != SU_OK ||
        param_str(params, "candidate_id", 1, &id, c) != SU_OK ||
        param_str(params, "world_id", 1, &world_id, c) != SU_OK ||
        param_str(params, "expected_world_revision", 1, &expect, c) != SU_OK ||
        param_uint(params, "allowed_effective_tick", 1, 0, AGENT_MAX_OFFSET, 0, &tick, c) != SU_OK) {
        return SU_E_TYPE;
    }
    cd = find_candidate(a, id);
    if (cd == NULL) {
        return call_fail(c, SU_E_INVALID_ARGUMENT, "unknown candidate_id");
    }
    if (strcmp(cd->install_state, "quarantined") == 0) {
        char msg[AGENT_MSG_CAP];
        snprintf(msg, sizeof msg, "candidate is quarantined: %.300s", cd->quarantine_reason);
        return call_fail(c, SU_E_CANDIDATE_FAILED, msg);
    }
    st = world_for_call(a, world_id, &w, c);
    if (st != SU_OK) {
        return st;
    }
    st = preflight(a, c);
    if (st != SU_OK) {
        return st;
    }
    st = log_room(a, c, w);
    if (st != SU_OK) {
        return st;
    }
    snprintf(cap, sizeof cap, "%s", cd->capsule_real);
    snprintf(src, sizeof src, "%s", cd->source_real);
    memset(&op, 0, sizeof op);
    op.kind = OPK_INSTALL;
    op.w = w;
    op.tick = w->tick;
    snprintf(op.cand, sizeof op.cand, "%s", cd->id);

    req_begin(&req, c->id, "internal.candidate.install");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, world_id, strlen(world_id));
    su_buf_str(&req, ",\"candidate_id\":");
    su_buf_quoted(&req, cd->id, strlen(cd->id));
    su_buf_str(&req, ",\"capsule_path\":");
    su_buf_quoted(&req, cap, strlen(cap));
    su_buf_str(&req, ",\"source_path\":");
    su_buf_quoted(&req, src, strlen(src));
    buf_hash_field(&req, "expected_source_hash", cd->source_hash);
    buf_hash_field(&req, "expected_capsule_hash", cd->capsule_hash);
    su_buf_str(&req, ",\"evidence\":");
    su_buf_quoted(&req, cd->eval_status, strlen(cd->eval_status));
    su_buf_str(&req, ",\"expected_world_revision\":");
    su_buf_quoted(&req, expect, strlen(expect));
    su_buf_str(&req, ",\"allowed_effective_tick\":");
    su_buf_u64(&req, tick);
    req_end(&req);
    if (req.failed) {
        su_buf_free(&req);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "candidate.install request too large");
    }
    su_buf_init(&resp);
    st = roundtrip(a, c->id, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return worker_died(a, c, &op);
    }
    reply_parse(&r, resp.data, resp.len);
    cd->worker_pid = (uint64_t)a->worker.pid;
    if (!r.ok) {
        uint64_t code = r.code;
        snprintf(cd->install_state, sizeof cd->install_state, "rejected");
        cd->install_txid = 0;
        cd->install_tick = 0;
        cd->install_code = code;
        reply_free(&r);
        relay(res, &resp, c);
        su_buf_free(&resp);
        return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
    }
    snprintf(cd->install_state, sizeof cd->install_state, "installed");
    nat_clear(w);
    respec_drop_world(a, w);
    cd->install_code = 0;
    ju64(r.result, "txid", &cd->install_txid);
    ju64(r.result, "effective_tick", &cd->install_tick);
    memset(&lop, 0, sizeof lop);
    lop.effective_tick = cd->install_tick;
    snprintf(lop.cand_id, sizeof lop.cand_id, "%s", cd->id);
    if (!jstr(r.result, "system_id", sysid, sizeof sysid) || !jstr(r.result, "semantic_hash", txt, sizeof txt) ||
        !jstr(r.result, "semantic_hash", lop.sys.semantic_hash, sizeof lop.sys.semantic_hash) ||
        !jstr(r.result, "source_hash", lop.sys.source_sha, sizeof lop.sys.source_sha) ||
        !jstr(r.result, "capsule_hash", lop.sys.capsule_sha, sizeof lop.sys.capsule_sha) ||
        !ju64(r.result, "system_revision", &lop.sys.system_rev)) {
        reply_free(&r);
        su_buf_free(&resp);
        mark_unrecoverable(w, "install answer was incomplete");
        return call_fail(c, SU_E_INTERNAL, "world is unrecoverable: install answer was incomplete");
    }
    snprintf(lop.sys.system_id, sizeof lop.sys.system_id, "%s", sysid);
    reply_free(&r);
    for (i = 0; i < w->count; i++) {
        if (strcmp(w->sys[i].system_id, sysid) == 0) {
            w->sys[i] = lop.sys;
            break;
        }
    }
    if (i == w->count) {
        mark_unrecoverable(w, "installed system is not in the world schedule");
        su_buf_free(&resp);
        return call_fail(c, SU_E_INTERNAL, "world is unrecoverable: installed system is not in the schedule");
    }
    rc = sync_after(a, w, LOG_INSTALL, &lop, 0, 1);
    if (rc == CK_DIED) {
        su_buf_free(&resp);
        return worker_died(a, c, &op);
    }
    if (rc == CK_BAD) {
        su_buf_free(&resp);
        return call_fail(c, SU_E_INTERNAL, "installed state diverged from the worker; world is unrecoverable");
    }
    relay(res, &resp, c);
    su_buf_free(&resp);
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* ---------- dispatch ---------- */

#ifdef SU_HAVE_NATIVE
/* Project paths of an installed system: its creation or candidate files, else the spool. */
static uint32_t system_paths(SuAgent *a, const AgentWorld *w, const SysRef *sys, char *cap, char *src) {
    const char *cp = NULL;
    const char *sp = NULL;
    char cap_sp[SU_PATH_BUF];
    char src_sp[SU_PATH_BUF];
    AgentCandidate *cd;
    uint32_t k;

    for (k = 0; k < w->creation_count && cp == NULL; k++) {
        if (strcmp(w->creation[k].capsule_sha, sys->capsule_sha) == 0 &&
            strcmp(w->creation[k].source_sha, sys->source_sha) == 0) {
            cp = w->creation_paths[k].capsule;
            sp = w->creation_paths[k].source;
        }
    }
    cd = cp == NULL ? candidate_by_capsule(a, sys->capsule_sha) : NULL;
    if (cd != NULL && strcmp(cd->source_hash, sys->source_sha) == 0) {
        cp = cd->capsule_real;
        sp = cd->source_real;
    }
    if (cp == NULL) {
        if (spool_file(a, sys->capsule_sha, "capsule", cap_sp, sizeof cap_sp) != SU_OK ||
            spool_file(a, sys->source_sha, "source", src_sp, sizeof src_sp) != SU_OK) {
            return SU_E_INTERNAL;
        }
        cp = cap_sp;
        sp = src_sp;
    }
    snprintf(cap, SU_PATH_BUF, "%s", cp);
    snprintf(src, SU_PATH_BUF, "%s", sp);
    return SU_OK;
}

/* The system's own test file: <stem>.test.json beside <stem>.capsule.json. Returns 1 and the real
 * path when it exists; 0 when the capsule has no such file (its own cases are then not run). */
static int own_test_path(SuAgent *a, const char *cap_real, char *out) {
    const char *base = strrchr(cap_real, '/');
    char test_name[SU_PATH_BUF];
    size_t n;

    base = base != NULL ? base + 1 : cap_real;
    n = strlen(base);
    if (n > 13u && strcmp(base + n - 13u, ".capsule.json") == 0) {
        snprintf(test_name, sizeof test_name, "%.*s.test.json", (int)(n - 13u), base);
        if (su_path_sibling(a->root, cap_real, test_name, out, SU_PATH_BUF) == SU_OK && access(out, R_OK) == 0) {
            return 1;
        }
    }
    /* M1-02c-3: the t03 numeric-edge fixtures (capsule.json) keep their cases in test.json beside it. */
    if (strcmp(base, "capsule.json") != 0 && (n <= 13u || strcmp(base + n - 13u, ".capsule.json") != 0)) {
        return 0;
    }
    if (su_path_sibling(a->root, cap_real, "test.json", out, SU_PATH_BUF) != SU_OK) {
        return 0;
    }
    return access(out, R_OK) == 0;
}

/* Detail object for a disposable worker that did not answer: how it ended. */
static void put_end_detail(SuBuf *d, const SuProc *p, uint64_t timeout_ms) {
    su_buf_str(d, "{\"phase\":\"admission\",\"end\":");
    if (p->end_kind == SU_PROC_END_EXITED) {
        su_buf_str(d, "\"exited\",\"exit_code\":");
        su_buf_u64(d, (uint64_t)p->exit_code);
    } else if (p->end_kind == SU_PROC_END_SIGNALED) {
        su_buf_str(d, "\"signaled\",\"signal\":");
        su_buf_u64(d, (uint64_t)p->term_signal);
    } else {
        su_buf_str(d, "\"supervisor_killed\",\"timeout_ms\":");
        su_buf_u64(d, timeout_ms);
    }
    su_buf_str(d, "}");
}

/* Copies one member of the first mismatch into the detail (absent members are omitted). */
static int put_mismatch_member(SuBuf *d, const SuJsonValue *fm, const char *key, int written) {
    const SuJsonValue *v = su_json_member(fm, key);
    uint64_t x = 0;
    if (v == NULL) {
        return 0;
    }
    su_buf_str(d, written > 0 ? ",\"" : "\"");
    su_buf_str(d, key);
    su_buf_str(d, "\":");
    if (su_json_is_string(v)) {
        su_buf_quoted(d, v->u.string.data, v->u.string.len);
    } else if (su_json_get_uint(v, &x)) {
        su_buf_u64(d, x);
    } else {
        su_buf_str(d, "null");
    }
    return 1;
}

/* Records a failure detail for the error envelope; the caller frees d. */
static uint32_t call_detail(AgentCall *c, uint32_t code, const char *msg, const SuBuf *d) {
    if (!d->failed && d->len < sizeof c->detail) {
        memcpy(c->detail, d->data, d->len);
        c->detail[d->len] = '\0';
    }
    return call_fail(c, code, msg);
}

/* world.specialize {world_id, expected_world_revision, system_id, backend, expected_semantic_hash?}
 * (M1-02c-1). Three steps, docs/AGENT_RPC.md section 9:
 *  1. identity: the active worker reports the installed system's identity. Read-only.
 *  2. admission: a DISPOSABLE worker (a fresh child per request, RLIMIT_AS and CPU, wall timeout,
 *     killed on timeout) compiles and runs differential admission against the reference VM, over the
 *     seeded corpus and the system's own test cases. The active worker never runs a candidate kernel.
 *     A disposable that is killed, times out or crashes answers SU_E_WORKER_FAILED with
 *     detail {phase:"admission", ...}; a mismatch answers SU_E_CANDIDATE_FAILED with the first mismatch.
 *  3. install: only after an admitted result, the active worker recompiles (compilation only), requires
 *     the admitted artifact_hash, rechecks the revision and semantic hash, and publishes. A worker death
 *     here recovers the world as before; the kernel is not canonical state, so nothing is logged. */
/* Refusal of a native specialization by the quarantine table (M1-02c-2). reason is
 * artifact_quarantined (hash != NULL) or quarantine_table_full. */
static uint32_t quar_refuse(AgentCall *c, const char *reason, const char *hash, uint64_t crashes) {
    SuBuf d;
    uint32_t st;
    su_buf_init(&d);
    su_buf_str(&d, "{\"phase\":\"quarantine\",\"reason\":");
    su_buf_quoted(&d, reason, strlen(reason));
    if (hash != NULL) {
        su_buf_str(&d, ",\"artifact_hash\":");
        su_buf_quoted(&d, hash, strlen(hash));
        su_buf_str(&d, ",\"crash_count\":");
        su_buf_u64(&d, crashes);
    }
    su_buf_str(&d, "}");
    st = call_detail(c, SU_E_CANDIDATE_FAILED,
                     hash != NULL ? "artifact is quarantined: a worker died while running it"
                                  : "quarantine table is full: native specialization is refused",
                     &d);
    su_buf_free(&d);
    return st;
}

/* Records an installed native kernel of w for system_id (replaces an earlier record for it). */
static void nat_note_install(AgentWorld *w, const char *system_id, const char *backend, const char *hash,
                             const char *tier) {
    uint32_t s;
    NatRec *n = NULL;
    for (s = 0; s < AGENT_MAX_SYSTEMS && n == NULL; s++) {
        if (w->nat[s].used && strcmp(w->nat[s].system_id, system_id) == 0) {
            n = &w->nat[s];
        }
    }
    for (s = 0; s < AGENT_MAX_SYSTEMS && n == NULL; s++) {
        if (!w->nat[s].used) {
            n = &w->nat[s];
        }
    }
    if (n == NULL) {
        return;
    }
    memset(n, 0, sizeof *n);
    n->used = 1;
    snprintf(n->system_id, sizeof n->system_id, "%s", system_id);
    snprintf(n->backend, sizeof n->backend, "%s", backend);
    snprintf(n->artifact_hash, sizeof n->artifact_hash, "%s", hash);
    snprintf(n->tier, sizeof n->tier, "%s", tier);
}

static uint32_t specialize_pipeline(SuAgent *a, const char *world_id, const char *expect,
                                    const char *system_id, const char *backend, const char *sem, SuBuf *res,
                                    AgentCall *c);

#ifdef SU_TEST_FAULT_HOOKS
static uint32_t route(SuAgent *a, const SuRequest *req, SuBuf *res, AgentCall *c);

/* M1-02c-3 test seam (SU_TEST_FAULT_HOOKS builds only). SU_TEST_INTERLEAVE selects one request the
 * supervisor issues itself, through route() as a client would, between admission and install:
 *   candidate  candidate.install of SU_TEST_INTERLEAVE_CANDIDATE at the world's current revision (a new
 *              semantic revision lands after admission);
 *   close      world.close of the world at its current revision.
 * The nested answer is discarded; the outer call's answer and the world state are what the test checks. */
static void test_interleave(SuAgent *a, const char *world_id) {
    const char *mode = getenv("SU_TEST_INTERLEAVE");
    const char *cid = getenv("SU_TEST_INTERLEAVE_CANDIDATE");
    AgentWorld *w;
    uint32_t k;
    AgentCall ic;
    SuBuf params_txt;
    SuBuf res;
    SuJsonDoc doc;
    SuJsonLimits lim;
    SuJsonError err;
    SuRequest rq;
    char rev[AGENT_REV_CAP];
    uint32_t st;

    static int fired; /* one-shot per process: the first specialization admitted after the arm */

    if (mode == NULL || mode[0] == '\0' || fired) {
        return;
    }
    w = find_world(a, world_id);
    if (w == NULL) {
        return;
    }
    fired = 1;
    wrev(w, w->rev, rev, sizeof rev);
    su_buf_init(&params_txt);
    su_buf_str(&params_txt, "{\"world_id\":");
    su_buf_quoted(&params_txt, world_id, strlen(world_id));
    su_buf_str(&params_txt, ",\"expected_world_revision\":");
    su_buf_quoted(&params_txt, rev, strlen(rev));
    if (strcmp(mode, "candidate") == 0 && cid == NULL) {
        /* No id given: the most recent candidate that passed its evaluation and was never installed. */
        for (k = 0; k < AGENT_MAX_CANDIDATES; k++) {
            if (a->cand[k].used && strcmp(a->cand[k].eval_status, "passed") == 0 &&
                strcmp(a->cand[k].install_state, "not_attempted") == 0) {
                cid = a->cand[k].id;
            }
        }
    }
    if (strcmp(mode, "candidate") == 0 && cid != NULL) {
        su_buf_str(&params_txt, ",\"candidate_id\":");
        su_buf_quoted(&params_txt, cid, strlen(cid));
        su_buf_str(&params_txt, ",\"allowed_effective_tick\":0");
        su_buf_str(&params_txt, "}");
        memset(&rq, 0, sizeof rq);
        snprintf(rq.method, sizeof rq.method, "candidate.install");
    } else if (strcmp(mode, "close") == 0) {
        su_buf_str(&params_txt, "}");
        memset(&rq, 0, sizeof rq);
        snprintf(rq.method, sizeof rq.method, "world.close");
    } else {
        su_buf_free(&params_txt);
        return;
    }
    if (params_txt.failed) {
        su_buf_free(&params_txt);
        return;
    }
    lim.max_bytes = params_txt.len;
    lim.max_depth = SU_PROTO_MAX_DEPTH;
    if (su_json_parse(params_txt.data, params_txt.len, &lim, &doc, &err) != SU_OK) {
        su_buf_free(&params_txt);
        su_json_doc_free(&doc);
        return;
    }
    rq.has_id = 1;
    snprintf(rq.id, sizeof rq.id, "interleave");
    rq.params = doc.root;
    memset(&ic, 0, sizeof ic);
    ic.id = "interleave";
    su_buf_init(&res);
    st = route(a, &rq, &res, &ic);
    (void)st;
    su_buf_free(&res);
    su_json_doc_free(&doc);
    su_buf_free(&params_txt);
}
#endif

static uint32_t h_world_specialize(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"world_id", "expected_world_revision", "system_id", "backend",
                                          "expected_semantic_hash"};
    const char *world_id = NULL;
    const char *expect = NULL;
    const char *system_id = NULL;
    const char *backend = NULL;
    const char *sem = NULL;

    if (check_keys(params, allowed, 5, c) != SU_OK ||
        param_str(params, "world_id", 1, &world_id, c) != SU_OK ||
        param_str(params, "expected_world_revision", 1, &expect, c) != SU_OK ||
        param_str(params, "system_id", 1, &system_id, c) != SU_OK ||
        param_str(params, "backend", 1, &backend, c) != SU_OK ||
        param_str(params, "expected_semantic_hash", 0, &sem, c) != SU_OK) {
        return SU_E_TYPE;
    }
    if (strcmp(backend, "scalar") != 0 && strcmp(backend, "avx2") != 0 && strcmp(backend, "auto") != 0) {
        return call_fail(c, SU_E_INVALID_ARGUMENT, "backend must be scalar, avx2 or auto");
    }
    return specialize_pipeline(a, world_id, expect, system_id, backend, sem, res, c);
}

static uint32_t specialize_pipeline(SuAgent *a, const char *world_id, const char *expect,
                                       const char *system_id, const char *backend, const char *sem,
                                       SuBuf *res, AgentCall *c) {
    AgentWorld *w = NULL;
    SuBuf req;
    SuBuf resp;
    SuBuf adm;
    SuBuf det;
    SuProc p;
    SuProcLimits plim;
    SuProcContainment adm_contain; /* the disposable admission worker's containment */
    Reply r;
    char ident_sem[65];
    char ident_src[65];
    char ident_cap[65];
    char cap_real[SU_PATH_BUF];
    char src_real[SU_PATH_BUF];
    char test_real[SU_PATH_BUF];
    char hash[65];
    char status[16];
    uint64_t ident_gen = 0;
    uint64_t cases = 0;
    uint64_t rows = 0;
    uint64_t mis = 0;
    uint64_t timeout = a->timeout_ms;
    int own = 0;
    uint32_t i;
    uint32_t st;


    w = find_world(a, world_id);
    if (w == NULL) {
        return call_fail(c, SU_E_INVALID_ARGUMENT, "unknown world_id");
    }
    st = preflight(a, c);
    if (st != SU_OK) {
        return st;
    }
    if (a->quar_overflow) {
        return quar_refuse(c, "quarantine_table_full", NULL, 0);
    }

    /* 1. identity (active worker, read-only). */
    req_begin(&req, c->id, "internal.specialize.identity");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, world_id, strlen(world_id));
    su_buf_str(&req, ",\"expected_world_revision\":");
    su_buf_quoted(&req, expect, strlen(expect));
    su_buf_str(&req, ",\"system_id\":");
    su_buf_quoted(&req, system_id, strlen(system_id));
    if (sem != NULL) {
        su_buf_str(&req, ",\"expected_semantic_hash\":");
        su_buf_quoted(&req, sem, strlen(sem));
    }
    req_end(&req);
    if (req.failed) {
        su_buf_free(&req);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "world.specialize request too large");
    }
    su_buf_init(&resp);
    st = roundtrip(a, c->id, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return worker_died(a, c, NULL);
    }
    reply_parse(&r, resp.data, resp.len);
    if (!r.ok) {
        reply_free(&r);
        relay(res, &resp, c);
        su_buf_free(&resp);
        return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
    }
    if (!jstr(r.result, "semantic_hash", ident_sem, sizeof ident_sem) ||
        !jstr(r.result, "source_hash", ident_src, sizeof ident_src) ||
        !jstr(r.result, "capsule_hash", ident_cap, sizeof ident_cap) ||
        !ju64(r.result, "layout_generation", &ident_gen)) {
        reply_free(&r);
        su_buf_free(&resp);
        return call_fail(c, SU_E_INTERNAL, "specialize identity answer was incomplete");
    }
    reply_free(&r);
    su_buf_free(&resp);
    for (i = 0; i < w->count; i++) {
        if (strcmp(w->sys[i].system_id, system_id) == 0) {
            break;
        }
    }
    if (i == w->count || strcmp(w->sys[i].source_sha, ident_src) != 0 ||
        strcmp(w->sys[i].capsule_sha, ident_cap) != 0) {
        return call_fail(c, SU_E_SCHEMA_MISMATCH, "installed system differs between the supervisor and the worker");
    }
    if (system_paths(a, w, &w->sys[i], cap_real, src_real) != SU_OK) {
        return call_fail(c, SU_E_INTERNAL, "system project files are unavailable");
    }
    own = own_test_path(a, cap_real, test_real);

    /* 2. admission in a disposable worker. */
    if (a->cfg.worker_argv == NULL) {
        return call_fail(c, SU_E_WORKER_FAILED, "no worker executable is configured");
    }
    req_begin(&req, c->id, "internal.specialize.admit");
    su_buf_str(&req, "\"capsule_path\":");
    su_buf_quoted(&req, cap_real, strlen(cap_real));
    su_buf_str(&req, ",\"source_path\":");
    su_buf_quoted(&req, src_real, strlen(src_real));
    if (own) {
        su_buf_str(&req, ",\"test_path\":");
        su_buf_quoted(&req, test_real, strlen(test_real));
    }
    buf_hash_field(&req, "expected_source_hash", ident_src);
    buf_hash_field(&req, "expected_capsule_hash", ident_cap);
    buf_hash_field(&req, "expected_semantic_hash", ident_sem);
    su_buf_str(&req, ",\"expected_layout_generation\":");
    su_buf_u64(&req, ident_gen);
    su_buf_str(&req, ",\"backend\":");
    su_buf_quoted(&req, backend, strlen(backend));
    req_end(&req);
    if (req.failed) {
        su_buf_free(&req);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "specialization request too large");
    }
    su_buf_init(&det);
    child_limits(a, &plim, 1);
    plim.cpu_seconds = (uint32_t)(timeout / 1000u) + 2u;
    if (su_proc_spawn(&p, a->cfg.worker_argv, &plim) != SU_OK) {
        su_buf_free(&req);
        su_buf_free(&det);
        return call_fail(c, SU_E_WORKER_FAILED, "admission worker could not be started");
    }
    adm_contain = p.containment;
    su_buf_init(&adm);
    {
        const uint8_t *line = NULL;
        size_t len = 0;
        st = su_proc_request(&p, req.data, req.len, su_now_ms() + (int64_t)timeout, &line, &len);
        su_buf_free(&req);
        if (st != SU_OK) {
            /* su_proc_request has killed and reaped the child; p holds how it ended. */
            su_buf_free(&adm);
            su_buf_free(&det);
            su_buf_init(&det);
            put_end_detail(&det, &p, timeout);
            su_proc_kill(&p);
            (void)call_detail(c, SU_E_WORKER_FAILED,
                              "admission worker was killed, crashed or exceeded its time limit", &det);
            su_buf_free(&det);
            return SU_E_WORKER_FAILED;
        }
        su_buf_put(&adm, line, len);
    }
    su_proc_finish(&p, 1000);
    if (adm.failed) {
        su_buf_free(&adm);
        su_buf_free(&det);
        return call_fail(c, SU_E_WORKER_FAILED, "admission worker response was too large");
    }
    reply_parse(&r, adm.data, adm.len);
    if (!r.parsed) {
        reply_free(&r);
        su_buf_free(&adm);
        su_buf_free(&det);
        return call_fail(c, SU_E_WORKER_FAILED, "admission worker response was malformed");
    }
    if (!r.ok) {
        /* The worker rejected the candidate itself (load, compile or schedule failure). A capability refusal
         * (for example ir_0_2_not_lowered) keeps its code. */
        char msg[AGENT_MSG_CAP];
        uint32_t wst = r.code == SU_E_CAPABILITY_DENIED ? SU_E_CAPABILITY_DENIED : SU_E_CANDIDATE_FAILED;
        snprintf(msg, sizeof msg, "%.500s", r.msg);
        reply_free(&r);
        su_buf_free(&adm);
        su_buf_free(&det);
        return call_fail(c, wst, msg);
    }
    if (!jstr(r.result, "status", status, sizeof status)) {
        reply_free(&r);
        su_buf_free(&adm);
        su_buf_free(&det);
        return call_fail(c, SU_E_WORKER_FAILED, "admission worker answer was incomplete");
    }
    if (strcmp(status, "mismatch") == 0) {
        const SuJsonValue *fm = su_json_member(r.result, "first_mismatch");
        static const char *const keys[] = {"source", "case", "reason", "case_index", "case_rows",
                                           "case_offset", "field_id", "field", "row", "expected", "actual"};
        size_t k;
        int n = 0;
        su_buf_free(&det);
        su_buf_init(&det);
        su_buf_str(&det, "{\"phase\":\"admission\",\"first_mismatch\":{");
        if (fm != NULL && fm->type == SU_JSON_OBJECT) {
            for (k = 0; k < sizeof keys / sizeof keys[0]; k++) {
                n += put_mismatch_member(&det, fm, keys[k], n);
            }
        }
        su_buf_str(&det, "}}");
        reply_free(&r);
        su_buf_free(&adm);
        (void)call_detail(c, SU_E_CANDIDATE_FAILED,
                          "differential admission found a mismatch against the reference VM", &det);
        su_buf_free(&det);
        return SU_E_CANDIDATE_FAILED;
    }
    if (strcmp(status, "admitted") != 0 || !jstr(r.result, "artifact_hash", hash, sizeof hash)) {
        reply_free(&r);
        su_buf_free(&adm);
        su_buf_free(&det);
        return call_fail(c, SU_E_WORKER_FAILED, "admission worker answer was incomplete");
    }
    {
        const SuJsonValue *ev = su_json_member(r.result, "evidence");
        if (ev == NULL || ev->type != SU_JSON_OBJECT || !ju64(ev, "cases", &cases) ||
            !ju64(ev, "rows_checked", &rows) || !ju64(ev, "mismatches", &mis) || mis != 0u) {
            reply_free(&r);
            su_buf_free(&adm);
            su_buf_free(&det);
            return call_fail(c, SU_E_CANDIDATE_FAILED, "admission evidence reports mismatches");
        }
    }
    reply_free(&r);
    su_buf_free(&adm);
    su_buf_free(&det);

    {
        QRec *q = quar_find(a, hash);
        if (q != NULL) {
            return quar_refuse(c, "artifact_quarantined", hash, q->crashes);
        }
    }

#ifdef SU_TEST_FAULT_HOOKS
    /* M1-02c-3 test seam: the supervisor issues one request of its own between admission and install.
     * Only in the test supervisor (su_agent_test); the production binary has no such string. */
    test_interleave(a, world_id);
#endif

    /* 3. install: the active worker recompiles and requires this artifact_hash. */
    req_begin(&req, c->id, "internal.specialize.install");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, world_id, strlen(world_id));
    su_buf_str(&req, ",\"system_id\":");
    su_buf_quoted(&req, system_id, strlen(system_id));
    su_buf_str(&req, ",\"expected_world_revision\":");
    su_buf_quoted(&req, expect, strlen(expect));
    buf_hash_field(&req, "expected_semantic_hash", ident_sem);
    su_buf_str(&req, ",\"expected_layout_generation\":");
    su_buf_u64(&req, ident_gen);
    su_buf_str(&req, ",\"backend\":");
    su_buf_quoted(&req, backend, strlen(backend));
    buf_hash_field(&req, "artifact_hash", hash);
    su_buf_str(&req, ",\"admission_cases\":");
    su_buf_u64(&req, cases);
    su_buf_str(&req, ",\"admission_rows_checked\":");
    su_buf_u64(&req, rows);
    req_end(&req);
    if (req.failed) {
        su_buf_free(&req);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "world.specialize install request too large");
    }
    su_buf_init(&resp);
    st = roundtrip(a, c->id, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return worker_died(a, c, NULL);
    }
    int inst_ok = 0;
    {
        Reply ir;
        char tier[16];
        reply_parse(&ir, resp.data, resp.len);
        /* w may have been closed by the test seam between admission and install: look it up again. */
        w = find_world(a, world_id);
        if (w != NULL && ir.ok && jstr(ir.result, "execution_tier", tier, sizeof tier)) {
            nat_note_install(w, system_id, backend, hash, tier);
        }
        inst_ok = ir.ok;
        reply_free(&ir);
    }
    relay_with_containment(res, &resp, inst_ok, &adm_contain, c);
    su_buf_free(&resp);
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}


/* Serves one pending respecialization. Called by the serve loop after the reply to a request other
 * than world.step, so no tick ever waits for it. A refusal is final; SU_E_WORKER_FAILED is retried up
 * to AGENT_RESPEC_RETRIES times. The outcome is kept per world (respec_state, respec_reason). */
static void respec_service(SuAgent *a) {
    RespecRec rr;
    AgentWorld *w;
    AgentCall c;
    SuBuf res;
    char rev[AGENT_REV_CAP];
    uint32_t st;

    if (a->respec_count == 0) {
        return;
    }
    rr = a->respec[0];
    memmove(&a->respec[0], &a->respec[1], sizeof a->respec[0] * (a->respec_count - 1));
    a->respec_count--;
    memset(&a->respec[a->respec_count], 0, sizeof a->respec[0]);
    w = find_world(a, rr.world_id);
    if (w == NULL || w->unrecoverable) {
        return;
    }
    if (a->quar_overflow) {
        snprintf(w->respec_state, sizeof w->respec_state, "failed");
        snprintf(w->respec_reason, sizeof w->respec_reason, "quarantine table is full");
        return;
    }
    memset(&c, 0, sizeof c);
    c.id = "respec";
    su_buf_init(&res);
    wrev(w, w->rev, rev, sizeof rev);
    st = specialize_pipeline(a, w->id, rev, rr.system_id, rr.backend, NULL, &res, &c);
    su_buf_free(&res);
    if (st == SU_OK) {
        snprintf(w->respec_state, sizeof w->respec_state, "respecialized");
        w->respec_reason[0] = '\0';
        return;
    }
    if (st == SU_E_WORKER_FAILED && rr.attempts < AGENT_RESPEC_RETRIES && a->respec_count < AGENT_MAX_RESPEC) {
        rr.attempts++;
        a->respec[a->respec_count++] = rr;
        return;
    }
    snprintf(w->respec_state, sizeof w->respec_state, "failed");
    snprintf(w->respec_reason, sizeof w->respec_reason, "%.*s", (int)(sizeof w->respec_reason - 1u),
             c.msg[0] != '\0' ? c.msg : "respecialization was refused");
}
#else
static void respec_service(SuAgent *a) {
    (void)a;
}
#endif /* SU_HAVE_NATIVE */

/* world.spawn {world_id, expected_world_revision, values:{<field>: u32}, external_id?: u32} (M2-00c3b).
 * The values object is kept as canonical text (ordered as received, u32 values) so recovery replays the
 * same spawn; the entity slot is the worker's lowest free slot, so the replay yields the same handle. */
static uint32_t h_world_spawn(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"world_id", "expected_world_revision", "values", "external_id"};
    const char *world_id = NULL;
    const char *expect = NULL;
    const SuJsonValue *vals = su_json_member(params, "values");
    const SuJsonValue *xid = su_json_member(params, "external_id");
    uint64_t ext = 0;
    AgentWorld *w = NULL;
    InFlight op;
    LogOp lop;
    SuBuf vb;
    SuBuf req;
    SuBuf resp;
    Reply r;
    uint32_t st;
    size_t i;
    int rc;

    if (check_keys(params, allowed, 4, c) != SU_OK ||
        param_str(params, "world_id", 1, &world_id, c) != SU_OK ||
        param_str(params, "expected_world_revision", 1, &expect, c) != SU_OK) {
        return SU_E_TYPE;
    }
    if (vals == NULL || vals->type != SU_JSON_OBJECT) {
        return call_fail(c, SU_E_TYPE, "values must be an object of field names to u32");
    }
    if (xid != NULL && (!su_json_get_uint(xid, &ext) || ext > UINT32_MAX)) {
        return call_fail(c, SU_E_TYPE, "external_id must be a u32 integer");
    }
    su_buf_init(&vb);
    su_buf_str(&vb, "{");
    for (i = 0; i < vals->u.object.count; i++) {
        const SuJsonMember *m = &vals->u.object.members[i];
        uint64_t v = 0;
        if (!su_json_get_uint(m->value, &v) || v > UINT32_MAX) {
            su_buf_free(&vb);
            return call_fail(c, SU_E_TYPE, "values must be u32 integers");
        }
        if (i > 0) {
            su_buf_str(&vb, ",");
        }
        su_buf_quoted(&vb, m->key, m->key_len);
        su_buf_str(&vb, ":");
        su_buf_u64(&vb, v);
    }
    su_buf_str(&vb, "}");
    if (vb.failed || vb.len >= AGENT_SPAWN_VALUES_CAP) {
        su_buf_free(&vb);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "values object is too large for the replay log");
    }
    st = world_for_call(a, world_id, &w, c);
    if (st != SU_OK) {
        su_buf_free(&vb);
        return st;
    }
    st = preflight(a, c);
    if (st != SU_OK) {
        su_buf_free(&vb);
        return st;
    }
    memset(&op, 0, sizeof op);
    op.kind = OPK_SPAWN;
    op.w = w;
    op.tick = w->tick;
    st = log_room(a, c, w);
    if (st != SU_OK) {
        su_buf_free(&vb);
        return st;
    }
    su_buf_init(&req);
    req_begin(&req, c->id, "world.spawn");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, world_id, strlen(world_id));
    su_buf_str(&req, ",\"expected_world_revision\":");
    su_buf_quoted(&req, expect, strlen(expect));
    su_buf_str(&req, ",\"values\":");
    su_buf_put(&req, vb.data, vb.len);
    if (xid != NULL) {
        su_buf_str(&req, ",\"external_id\":");
        su_buf_u64(&req, ext);
    }
    req_end(&req);
    if (req.failed) {
        su_buf_free(&req);
        su_buf_free(&vb);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "world.spawn request too large");
    }
    su_buf_init(&resp);
    st = roundtrip(a, c->id, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        su_buf_free(&vb);
        return worker_died(a, c, &op);
    }
    reply_parse(&r, resp.data, resp.len);
    memset(&lop, 0, sizeof lop);
    memcpy(lop.spawn_values, vb.data, vb.len);
    lop.spawn_values[vb.len] = '\0';
    lop.has_ext = xid != NULL;
    lop.ext_id = (uint32_t)ext;
    rc = sync_after(a, w, LOG_SPAWN, &lop, 0, r.ok);
    reply_free(&r);
    su_buf_free(&vb);
    if (rc == CK_DIED) {
        su_buf_free(&resp);
        return worker_died(a, c, &op);
    }
    if (rc == CK_BAD) {
        su_buf_free(&resp);
        return call_fail(c, SU_E_INTERNAL, "world state diverged from the worker; world is unrecoverable");
    }
    relay(res, &resp, c);
    su_buf_free(&resp);
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* world.despawn {world_id, expected_world_revision, entity} (M2-00c3b). */
static uint32_t h_world_despawn(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"world_id", "expected_world_revision", "entity"};
    const char *world_id = NULL;
    const char *expect = NULL;
    const SuJsonValue *eh = su_json_member(params, "entity");
    uint64_t handle = 0;
    AgentWorld *w = NULL;
    InFlight op;
    LogOp lop;
    SuBuf req;
    SuBuf resp;
    Reply r;
    uint32_t st;
    int rc;

    if (check_keys(params, allowed, 3, c) != SU_OK ||
        param_str(params, "world_id", 1, &world_id, c) != SU_OK ||
        param_str(params, "expected_world_revision", 1, &expect, c) != SU_OK) {
        return SU_E_TYPE;
    }
    if (eh == NULL || !su_json_get_uint(eh, &handle)) {
        return call_fail(c, SU_E_TYPE, "entity must be an integer handle");
    }
    st = world_for_call(a, world_id, &w, c);
    if (st != SU_OK) {
        return st;
    }
    st = preflight(a, c);
    if (st != SU_OK) {
        return st;
    }
    memset(&op, 0, sizeof op);
    op.kind = OPK_DESPAWN;
    op.w = w;
    op.tick = w->tick;
    st = log_room(a, c, w);
    if (st != SU_OK) {
        return st;
    }
    su_buf_init(&req);
    req_begin(&req, c->id, "world.despawn");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, world_id, strlen(world_id));
    su_buf_str(&req, ",\"expected_world_revision\":");
    su_buf_quoted(&req, expect, strlen(expect));
    su_buf_str(&req, ",\"entity\":");
    su_buf_u64(&req, handle);
    req_end(&req);
    if (req.failed) {
        su_buf_free(&req);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "world.despawn request too large");
    }
    su_buf_init(&resp);
    st = roundtrip(a, c->id, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return worker_died(a, c, &op);
    }
    reply_parse(&r, resp.data, resp.len);
    memset(&lop, 0, sizeof lop);
    lop.handle = handle;
    rc = sync_after(a, w, LOG_DESPAWN, &lop, 0, r.ok);
    reply_free(&r);
    if (rc == CK_DIED) {
        su_buf_free(&resp);
        return worker_died(a, c, &op);
    }
    if (rc == CK_BAD) {
        su_buf_free(&resp);
        return call_fail(c, SU_E_INTERNAL, "world state diverged from the worker; world is unrecoverable");
    }
    relay(res, &resp, c);
    su_buf_free(&resp);
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* events.read {world_id, cursor, max<=256}: read-only (M2-00c3b). */
static uint32_t h_events_read(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"world_id", "cursor", "max"};
    const char *world_id = NULL;
    uint64_t cursor = 0;
    uint64_t max = 0;
    AgentWorld *w = NULL;
    InFlight op;
    SuBuf req;
    SuBuf resp;
    uint32_t st;

    if (check_keys(params, allowed, 3, c) != SU_OK ||
        param_str(params, "world_id", 1, &world_id, c) != SU_OK ||
        param_uint(params, "cursor", 1, 0, UINT64_MAX, 0, &cursor, c) != SU_OK ||
        param_uint(params, "max", 1, 1, AGENT_EVENTS_MAX, 0, &max, c) != SU_OK) {
        return SU_E_TYPE;
    }
    st = world_for_call(a, world_id, &w, c);
    if (st != SU_OK) {
        return st;
    }
    su_buf_init(&req);
    req_begin(&req, c->id, "events.read");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, world_id, strlen(world_id));
    su_buf_str(&req, ",\"cursor\":");
    su_buf_u64(&req, cursor);
    su_buf_str(&req, ",\"max\":");
    su_buf_u64(&req, max);
    req_end(&req);
    su_buf_init(&resp);
    memset(&op, 0, sizeof op);
    op.kind = OPK_READ;
    st = roundtrip(a, c->id, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return worker_died(a, c, &op);
    }
    relay(res, &resp, c);
    su_buf_free(&resp);
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* ---------- M2-01b: schema migrations (contract sections 3-5) ----------
 *
 * world.migrate_plan returns estimates and stores a plan bound to the world revision. world.migrate_commit
 * pins the pre-migration checkpoint, prepares the successor aside, swaps it, appends LOG_MIGRATE, and takes the
 * post-commit checkpoint, which must be durable before the reply. A failed post-commit checkpoint for lack of
 * room rolls back to the pin (LOG_ROLLBACK) and answers SU_E_RESOURCE_LIMIT; a worker death during the post
 * checkpoint recovers (the log replays the migration) and the checkpoint is taken again before the reply.
 * world.migrate_rollback restores the pin as a new revision; changes made after the pin are lost. */

#define AGENT_MIG_DOC_MAX (256u * 1024u)

/* Spools one project document (relative to the project root) under its sha256 and returns the hash. */
static uint32_t spool_doc(SuAgent *a, const char *rel, const char *ext, uint32_t max, char sha[65], AgentCall *c) {
    char real[SU_PATH_BUF];
    uint8_t *bytes = NULL;
    size_t len = 0;
    uint32_t st = su_path_resolve(a->root, rel, real, SU_PATH_BUF);
    if (st == SU_E_IO) {
        return call_fail(c, st, "migration document does not exist or cannot be opened");
    }
    if (st != SU_OK) {
        return call_fail(c, st, "migration document path rejected by the project path policy");
    }
    st = su_path_read_file(real, max, &bytes, &len);
    if (st != SU_OK) {
        return call_fail(c, st, "migration document could not be read");
    }
    st = spool_put_admit(a, ext, bytes, len, sha);
    free(bytes);
    if (st != SU_OK) {
        return call_fail(c, SU_E_RESOURCE_LIMIT, "migration document does not fit the spool budget");
    }
    return SU_OK;
}

/* Spool budget a migration may use: the spool limit minus what the spool is already committed to. */
static uint64_t migration_spool_budget(const SuAgent *a) {
    uint64_t used = committed_bytes(a);
    return a->spool_max > used ? a->spool_max - used : 0u;
}

/* Finds a plan of w by id, or NULL. */
static PlanRec *plan_find(AgentWorld *w, const char *id) {
    uint32_t k;
    for (k = 0; k < AGENT_MAX_PLANS; k++) {
        if (w->plan[k].used && strcmp(w->plan[k].id, id) == 0) {
            return &w->plan[k];
        }
    }
    return NULL;
}

/* Restores the pinned pre-migration checkpoint as the next revision (LOG_ROLLBACK). Changes made after the pin are
 * lost. Returns SU_OK, or the failure status with the client message set (the worker died: recovered). */
static uint32_t rollback_pinned(SuAgent *a, AgentWorld *w, AgentCall *c, const InFlight *op, int *retained) {
    char snap[SU_PATH_BUF];
    char cur[AGENT_REV_CAP];
    SuBuf req;
    SuBuf resp;
    Reply r;
    LogOp lop;
    uint32_t st;
    int rc;

    if (!w->pin_has) {
        return call_fail(c, SU_E_STALE_REVISION, "rollback_unavailable: no pinned pre-migration checkpoint is retained");
    }
    if (spool_file(a, w->pin.sha, "snap", snap, sizeof snap) != SU_OK) {
        mark_unrecoverable(w, "the pinned checkpoint is missing from the spool");
        return call_fail(c, SU_E_INTERNAL, "world is unrecoverable: the pinned checkpoint is missing");
    }
    wrev(w, w->rev, cur, sizeof cur);
    req_begin(&req, AGENT_INTERNAL_ID, "internal.world.restore_schema");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, w->id, strlen(w->id));
    su_buf_str(&req, ",\"expected_world_revision\":");
    su_buf_quoted(&req, cur, strlen(cur));
    su_buf_str(&req, ",\"snapshot_path\":");
    su_buf_quoted(&req, snap, strlen(snap));
    su_buf_str(&req, ",\"snapshot_sha256\":");
    su_buf_quoted(&req, w->pin.sha, strlen(w->pin.sha));
    su_buf_str(&req, ",\"mode\":\"exact\",\"world_revision\":");
    su_buf_u64(&req, w->rev + 1u);
    st = write_capsules(a, &req, "capsules", w->pin.sys, w->pin.count);
    if (st != SU_OK) {
        su_buf_free(&req);
        mark_unrecoverable(w, "a pinned capsule is missing from the spool");
        return call_fail(c, SU_E_INTERNAL, "world is unrecoverable: a pinned capsule is missing");
    }
    req_end(&req);
    su_buf_init(&resp);
    st = roundtrip(a, AGENT_INTERNAL_ID, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return worker_died(a, c, op);
    }
    reply_parse(&r, resp.data, resp.len);
    if (!r.ok) {
        char msg[AGENT_MSG_CAP];
        uint32_t code = (uint32_t)r.code;
        snprintf(msg, sizeof msg, "%s", r.msg);
        reply_free(&r);
        su_buf_free(&resp);
        return call_fail(c, code != 0 ? code : SU_E_INTERNAL, msg);
    }
    *retained = 0;
    {
        const SuJsonValue *rv = jf(r.result, "retired_field_ids_retained");
        *retained = rv != NULL && rv->type == SU_JSON_BOOL && rv->u.boolean != 0;
    }
    reply_free(&r);
    su_buf_free(&resp);
    if (!*retained) {
        mark_unrecoverable(w, "the rolled-back lineage did not keep its retired field IDs");
        return call_fail(c, SU_E_INTERNAL, "world is unrecoverable: rollback did not retain the retired field IDs");
    }
    memset(&lop, 0, sizeof lop);
    lop.mig.pin = w->pin;
    snprintf(lop.mig.plan_hash, sizeof lop.mig.plan_hash, "%s", w->pin_mig);
    memcpy(w->sys, w->pin.sys, sizeof w->sys);
    w->count = w->pin.count;
    w->migrated = 1;
    rc = sync_after(a, w, LOG_ROLLBACK, &lop, 0, 1);
    if (rc == CK_DIED) {
        return worker_died(a, c, op);
    }
    if (rc == CK_BAD) {
        return call_fail(c, SU_E_INTERNAL, "rolled-back state diverged from the worker; world is unrecoverable");
    }
    return SU_OK;
}

/* Supervisor records of a world taken before a migration commit (contract section 4 (5), B9). */
typedef struct PreMig {
    uint64_t rev;
    uint64_t tick;
    char hash[65];
    uint64_t ticks_since_ck;
    uint32_t log_count;
    uint32_t ck_count;
    uint32_t count;
    int migrated;
    SysRef sys[AGENT_MAX_SYSTEMS];
} PreMig;

#ifdef SU_TEST_FAULT_HOOKS
/* Test-only: when SUTEKH_TEST_POST_CK_NOROOM_TOKEN names an existing file, the file is unlinked and the next
 * post-commit checkpoint reports no room (the migration is then undone). Compiled only into test supervisors. */
static int test_post_ck_noroom(void) {
    const char *tok = getenv("SUTEKH_TEST_POST_CK_NOROOM_TOKEN");
    return tok != NULL && tok[0] != '\0' && unlink(tok) == 0;
}
#define POST_CK_NOROOM() test_post_ck_noroom()
#else
#define POST_CK_NOROOM() 0
#endif

/* Undoes a migration commit whose post-commit checkpoint could not be made durable. The LOG_MIGRATE entry that
 * sync_after appended is removed and the supervisor records return to the pre-migration values, so the replacement
 * worker rebuilds the pinned pre-migration checkpoint and the replay log ends before the migration. Nothing is
 * acknowledged: the revision is the one before the commit and the world is the old world (contract section 1).
 * The pin no longer names a committed migration, so rollback is unavailable until the next commit. */
static uint32_t undo_migration(SuAgent *a, AgentWorld *w, AgentCall *c, const InFlight *op, const PreMig *pre,
                               uint32_t code, const char *msg, const char *reason) {
    const LogOp *last;
    if (w->log_count == 0u || w->ck_count != pre->ck_count) {
        mark_unrecoverable(w, "a migration could not be undone: the log or checkpoints changed during the commit");
        return call_fail(c, SU_E_INTERNAL, "world is unrecoverable: the migration could not be undone");
    }
    last = &w->log[w->log_count - 1u];
    if (last->kind != LOG_MIGRATE) {
        mark_unrecoverable(w, "a migration could not be undone: the logged entry is not the migration");
        return call_fail(c, SU_E_INTERNAL, "world is unrecoverable: the migration could not be undone");
    }
    w->log_count--;
    w->rev = pre->rev;
    w->tick = pre->tick;
    snprintf(w->hash, sizeof w->hash, "%s", pre->hash);
    w->ticks_since_ck = pre->ticks_since_ck;
    w->count = pre->count;
    memcpy(w->sys, pre->sys, sizeof w->sys);
    w->migrated = pre->migrated;
    w->pin_has = 0;
    w->pin_mig[0] = '\0';
    recover(a, c, op);
    if (w->unrecoverable) {
        return call_fail(c, SU_E_INTERNAL, "world is unrecoverable: the pre-migration state could not be rebuilt");
    }
    snprintf(c->detail, sizeof c->detail, "{\"reason\":\"%s\",\"rolled_back_to\":\"pinned_pre_migration_checkpoint\"}",
             reason);
    return call_fail(c, code, msg);
}

/* Adds the replacement systems, drops, documents and budget of op to a prepare request (the same request the
 * replay of LOG_MIGRATE sends). */
static uint32_t migrate_prepare_request(SuAgent *a, const AgentWorld *w, const MigOp *op, uint64_t to_gen,
                                        const char *expect, SuBuf *req) {
    char pa[SU_PATH_BUF];
    char ta[SU_PATH_BUF];
    char la[SU_PATH_BUF];
    uint32_t i;
    uint32_t st;

    if (spool_file(a, op->mig_sha, "migration", pa, sizeof pa) != SU_OK ||
        (op->has_tr && spool_file(a, op->tr_sha, "transform", ta, sizeof ta) != SU_OK) ||
        (op->has_lir && spool_file(a, op->lir_sha, "lir", la, sizeof la) != SU_OK)) {
        return SU_E_INTERNAL;
    }
    req_begin(req, AGENT_INTERNAL_ID, "internal.world.migrate_prepare");
    su_buf_str(req, "\"world_id\":");
    su_buf_quoted(req, w->id, strlen(w->id));
    su_buf_str(req, ",\"expected_world_revision\":");
    su_buf_quoted(req, expect, strlen(expect));
    su_buf_str(req, ",\"to_layout_generation\":");
    su_buf_u64(req, to_gen);
    su_buf_str(req, ",\"migration_path\":");
    su_buf_quoted(req, pa, strlen(pa));
    su_buf_str(req, ",\"migration_sha256\":");
    su_buf_quoted(req, op->mig_sha, strlen(op->mig_sha));
    if (op->has_tr) {
        su_buf_str(req, ",\"transform_path\":");
        su_buf_quoted(req, ta, strlen(ta));
        su_buf_str(req, ",\"transform_sha256\":");
        su_buf_quoted(req, op->tr_sha, strlen(op->tr_sha));
    }
    if (op->has_lir) {
        su_buf_str(req, ",\"lir_path\":");
        su_buf_quoted(req, la, strlen(la));
        su_buf_str(req, ",\"lir_sha256\":");
        su_buf_quoted(req, op->lir_sha, strlen(op->lir_sha));
    }
    su_buf_str(req, ",\"drop_systems\":[");
    for (i = 0; i < op->nd; i++) {
        if (i > 0) {
            su_buf_str(req, ",");
        }
        su_buf_quoted(req, op->drop[i], strlen(op->drop[i]));
    }
    su_buf_str(req, "],\"spool_bytes\":");
    su_buf_u64(req, op->spool_bytes);
    su_buf_str(req, ",\"memory_bytes\":");
    su_buf_u64(req, op->memory_bytes);
    st = write_capsules(a, req, "resubmit", op->sys, op->n);
    if (st != SU_OK) {
        return st;
    }
    req_end(req);
    return req->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* world.migrate_plan {world_id, expected_world_revision, migration_path, transform_path?, lir_path?, drop_systems?}.
 * Estimates and validity; stores the plan (plan_id) bound to the current revision. Changes no state. */
static uint32_t h_world_migrate_plan(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"world_id", "expected_world_revision", "migration_path", "transform_path",
                                          "lir_path", "drop_systems"};
    uint64_t t_plan = 0;
    const char *world_id = NULL;
    const char *expect = NULL;
    const char *mp = NULL;
    const char *tp = NULL;
    const char *lp = NULL;
    const SuJsonValue *drops_v = su_json_member(params, "drop_systems");
    const char *dn[AGENT_MAX_SYSTEMS];
    AgentWorld *w = NULL;
    PlanRec *pr;
    MigOp doc;
    char pid[AGENT_MIG_ID_CAP];
    char cur[AGENT_REV_CAP];
    char path[SU_PATH_BUF];
    SuBuf req;
    SuBuf resp;
    Reply r;
    const SuJsonValue *plan;
    uint32_t nd = 0;
    uint32_t slot;
    uint32_t st;
    size_t i;

    if (check_keys(params, allowed, 6, c) != SU_OK || param_str(params, "world_id", 1, &world_id, c) != SU_OK ||
        param_str(params, "expected_world_revision", 1, &expect, c) != SU_OK ||
        param_str(params, "migration_path", 1, &mp, c) != SU_OK || param_str(params, "transform_path", 0, &tp, c) != SU_OK ||
        param_str(params, "lir_path", 0, &lp, c) != SU_OK) {
        return SU_E_TYPE;
    }
    if (drops_v != NULL) {
        if (drops_v->type != SU_JSON_ARRAY || drops_v->u.array.count > AGENT_MAX_SYSTEMS) {
            return call_fail(c, SU_E_TYPE, "drop_systems must be an array of at most 8 system ids");
        }
        for (i = 0; i < drops_v->u.array.count; i++) {
            const SuJsonValue *e = drops_v->u.array.items[i];
            if (e->type != SU_JSON_STRING) {
                return call_fail(c, SU_E_TYPE, "drop_systems must be an array of strings");
            }
            dn[nd++] = e->u.string.data;
        }
    }
    st = world_for_call(a, world_id, &w, c);
    if (st != SU_OK) {
        return st;
    }
    st = preflight(a, c);
    if (st != SU_OK) {
        return st;
    }
    wrev(w, w->rev, cur, sizeof cur);
    if (strcmp(expect, cur) != 0) {
        return call_fail(c, SU_E_STALE_REVISION, "expected_world_revision does not match the active world");
    }
    memset(&doc, 0, sizeof doc);
    st = spool_doc(a, mp, "migration", AGENT_MIG_DOC_MAX, doc.mig_sha, c);
    if (st == SU_OK && tp != NULL) {
        st = spool_doc(a, tp, "transform", AGENT_CAPSULE_MAX, doc.tr_sha, c);
        doc.has_tr = 1;
    }
    if (st == SU_OK && lp != NULL) {
        st = spool_doc(a, lp, "lir", AGENT_CAPSULE_MAX, doc.lir_sha, c);
        doc.has_lir = 1;
    }
    if (st != SU_OK) {
        return st;
    }
    doc.spool_bytes = migration_spool_budget(a);
    doc.memory_bytes = AGENT_MIG_MEMORY_BUDGET;
    doc.nd = nd;
    for (i = 0; i < nd; i++) {
        snprintf(doc.drop[i], sizeof doc.drop[i], "%s", dn[i]);
    }
    slot = w->next_plan % AGENT_MAX_PLANS;
    w->next_plan++;
    snprintf(pid, sizeof pid, "plan-%u", (unsigned)w->next_plan);

    su_buf_init(&req);
    req_begin(&req, c->id, "internal.world.migrate_plan");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, w->id, strlen(w->id));
    su_buf_str(&req, ",\"expected_world_revision\":");
    su_buf_quoted(&req, cur, strlen(cur));
    su_buf_str(&req, ",\"plan_id\":");
    su_buf_quoted(&req, pid, strlen(pid));
    if (spool_file(a, doc.mig_sha, "migration", path, sizeof path) != SU_OK) {
        su_buf_free(&req);
        return call_fail(c, SU_E_INTERNAL, "migration document is missing from the spool");
    }
    su_buf_str(&req, ",\"migration_path\":");
    su_buf_quoted(&req, path, strlen(path));
    su_buf_str(&req, ",\"migration_sha256\":");
    su_buf_quoted(&req, doc.mig_sha, 64);
    if (doc.has_tr && spool_file(a, doc.tr_sha, "transform", path, sizeof path) == SU_OK) {
        su_buf_str(&req, ",\"transform_path\":");
        su_buf_quoted(&req, path, strlen(path));
        su_buf_str(&req, ",\"transform_sha256\":");
        su_buf_quoted(&req, doc.tr_sha, 64);
    }
    if (doc.has_lir && spool_file(a, doc.lir_sha, "lir", path, sizeof path) == SU_OK) {
        su_buf_str(&req, ",\"lir_path\":");
        su_buf_quoted(&req, path, strlen(path));
        su_buf_str(&req, ",\"lir_sha256\":");
        su_buf_quoted(&req, doc.lir_sha, 64);
    }
    su_buf_str(&req, ",\"drop_systems\":[");
    for (i = 0; i < nd; i++) {
        if (i > 0) {
            su_buf_str(&req, ",");
        }
        su_buf_quoted(&req, dn[i], strlen(dn[i]));
    }
    su_buf_str(&req, "],\"spool_bytes\":");
    su_buf_u64(&req, doc.spool_bytes);
    su_buf_str(&req, ",\"memory_bytes\":");
    su_buf_u64(&req, doc.memory_bytes);
    req_end(&req);
    if (req.failed) {
        su_buf_free(&req);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "world.migrate_plan request too large");
    }
    su_buf_init(&resp);
    t_plan = mono_ns();
    st = roundtrip(a, c->id, &req, &resp);
    t_plan = mono_ns() - t_plan;
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return worker_died(a, c, NULL);
    }
    reply_parse(&r, resp.data, resp.len);
    if (!r.ok) {
        reply_free(&r);
        relay(res, &resp, c);
        su_buf_free(&resp);
        return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
    }
    /* Record the plan: bound to this revision; the documents stay in the spool while it is held. */
    pr = &w->plan[slot];
    memset(pr, 0, sizeof *pr);
    pr->used = 1;
    snprintf(pr->id, sizeof pr->id, "%s", pid);
    pr->rev = w->rev;
    pr->tick = w->tick;
    pr->doc = doc;
    pr->plan_ns = t_plan;
    plan = jf(r.result, "plan");
    if (plan != NULL) {
        uint64_t g = 0;
        const SuJsonValue *rsc = jf(plan, "resource");
        if (rsc != NULL) {
            ju64(rsc, "bytes_new_columns", &pr->bytes_new_columns);
            ju64(rsc, "bytes_peak", &pr->bytes_peak);
        }
        const SuJsonValue *vv = jf(plan, "valid");
        pr->valid = vv != NULL && vv->type == SU_JSON_BOOL && vv->u.boolean != 0;
        jstr(plan, "plan_hash", pr->plan_hash, sizeof pr->plan_hash);
        ju64(plan, "from_layout_generation", &g);
        pr->from_gen = (uint32_t)g;
        g = 0;
        ju64(plan, "to_layout_generation", &g);
        pr->to_gen = (uint32_t)g;
        memcpy(pr->doc.plan_hash, pr->plan_hash, sizeof pr->doc.plan_hash);
    }
    reply_free(&r);
    relay(res, &resp, c);
    su_buf_free(&resp);
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* Turns a worker refusal into the client's error (the worker's status and message). */
static uint32_t reject_from(AgentCall *c, Reply *r, SuBuf *resp) {
    char msg[AGENT_MSG_CAP];
    uint32_t code = (uint32_t)r->code;
    snprintf(msg, sizeof msg, "%s", r->msg);
    reply_free(r);
    su_buf_free(resp);
    return call_fail(c, code != 0 ? code : SU_E_INTERNAL, msg);
}

/* world.migrate_commit {world_id, plan_id, expected_world_revision, resubmit: [{system_id, candidate_id}],
 * drop_systems?}. Every installed system is replaced by an evaluated candidate of the plan's target generation or
 * dropped (contract B2). See the section comment for the stages and the crash behaviour. */
static uint32_t h_world_migrate_commit(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"world_id", "plan_id", "expected_world_revision", "resubmit", "drop_systems"};
    const char *world_id = NULL;
    const char *plan_id = NULL;
    const char *expect = NULL;
    const SuJsonValue *rs_v = su_json_member(params, "resubmit");
    const SuJsonValue *drops_v = su_json_member(params, "drop_systems");
    AgentWorld *w = NULL;
    PlanRec *pr;
    MigOp op;
    SysRef oldsys[AGENT_MAX_SYSTEMS];
    uint32_t oldcount;
    InFlight inop;
    char cur[AGENT_REV_CAP];
    char rv[AGENT_REV_CAP];
    SuBuf req;
    SuBuf resp;
    Reply r;
    uint64_t prep_ns = 0;
    uint64_t trans_ns = 0;
    uint64_t t0 = 0;
    uint64_t ck_ns = 0;
    uint64_t swap_tick = 0;
    uint64_t layout = 0;
    char mig_hash[65];
    SysRef ordered[AGENT_MAX_SYSTEMS];
    LogOp lop;
    PreMig pre;
    uint64_t total_t0 = mono_ns();
    uint64_t swap_ns = 0;
    uint32_t st;
    uint32_t k;
    uint32_t j;
    size_t i;
    int rc;

    if (check_keys(params, allowed, 5, c) != SU_OK || param_str(params, "world_id", 1, &world_id, c) != SU_OK ||
        param_str(params, "plan_id", 1, &plan_id, c) != SU_OK || param_str(params, "expected_world_revision", 1, &expect, c) != SU_OK) {
        return SU_E_TYPE;
    }
    if (rs_v == NULL || rs_v->type != SU_JSON_ARRAY || rs_v->u.array.count == 0u || rs_v->u.array.count > AGENT_MAX_SYSTEMS) {
        return call_fail(c, SU_E_TYPE, "resubmit must be an array of 1..8 {system_id, candidate_id} objects");
    }
    st = world_for_call(a, world_id, &w, c);
    if (st != SU_OK) {
        return st;
    }
    st = preflight(a, c);
    if (st != SU_OK) {
        return st;
    }
    pr = plan_find(w, plan_id);
    if (pr == NULL) {
        return call_fail(c, SU_E_INVALID_ARGUMENT, "unknown plan_id");
    }
    if (!pr->valid) {
        return call_fail(c, SU_E_UNSUPPORTED_MIGRATION, "the plan is not valid; see world.migrate_plan");
    }
    wrev(w, w->rev, cur, sizeof cur);
    if (strcmp(expect, cur) != 0 || pr->rev != w->rev) {
        return call_fail(c, SU_E_STALE_REVISION, "the plan is stale: the world revision changed after it was made");
    }
    memset(&op, 0, sizeof op);
    op = pr->doc;
    op.to_gen = pr->to_gen;
    op.n = (uint32_t)rs_v->u.array.count;
    memset(op.sys, 0, sizeof op.sys);
    for (k = 0; k < op.n; k++) {
        const SuJsonValue *item = rs_v->u.array.items[k];
        char sid[SU_DIAG_SYSTEM_BYTES];
        char cid[32];
        AgentCandidate *cd = NULL;
        if (item->type != SU_JSON_OBJECT || !jstr(item, "system_id", sid, sizeof sid) ||
            !jstr(item, "candidate_id", cid, sizeof cid)) {
            return call_fail(c, SU_E_TYPE, "each resubmit entry needs system_id and candidate_id");
        }
        for (j = 0; j < AGENT_MAX_CANDIDATES; j++) {
            if (a->cand[j].used && strcmp(a->cand[j].id, cid) == 0) {
                cd = &a->cand[j];
            }
        }
        if (cd == NULL || strcmp(cd->system_id, sid) != 0) {
            return call_fail(c, SU_E_INVALID_ARGUMENT, "resubmit names an unknown candidate for that system");
        }
        if (strcmp(cd->eval_status, "passed") != 0) {
            return call_fail(c, SU_E_UNSUPPORTED_MIGRATION, "resubmit candidate has not passed evaluation");
        }
        snprintf(op.sys[k].system_id, sizeof op.sys[k].system_id, "%s", sid);
        snprintf(op.sys[k].capsule_sha, sizeof op.sys[k].capsule_sha, "%s", cd->capsule_hash);
        snprintf(op.sys[k].source_sha, sizeof op.sys[k].source_sha, "%s", cd->source_hash);
        snprintf(op.sys[k].semantic_hash, sizeof op.sys[k].semantic_hash, "%s", cd->semantic_hash);
    }
    op.nd = 0;
    if (drops_v != NULL) {
        if (drops_v->type != SU_JSON_ARRAY || drops_v->u.array.count > AGENT_MAX_SYSTEMS) {
            return call_fail(c, SU_E_TYPE, "drop_systems must be an array of at most 8 system ids");
        }
        for (i = 0; i < drops_v->u.array.count; i++) {
            const SuJsonValue *e = drops_v->u.array.items[i];
            if (e->type != SU_JSON_STRING) {
                return call_fail(c, SU_E_TYPE, "drop_systems must be an array of strings");
            }
            snprintf(op.drop[op.nd], sizeof op.drop[op.nd], "%s", e->u.string.data);
            op.nd++;
        }
    }
    memset(&inop, 0, sizeof inop);
    inop.kind = OPK_MIGRATE;
    inop.w = w;
    inop.tick = w->tick;

    /* Pin the pre-migration state: a durable checkpoint at the acknowledged state. */
    rc = checkpoint_world(a, w, 0);
    if (rc == CK_DIED) {
        return worker_died(a, c, &inop);
    }
    if (rc == CK_NOROOM) {
        return call_fail(c, SU_E_RESOURCE_LIMIT, "checkpoint budget exhausted; the migration was not started");
    }
    if (rc == CK_BAD) {
        return call_fail(c, SU_E_INTERNAL, "checkpoint disagreed with the acknowledged state; world is unrecoverable");
    }
    w->pin = w->ck[w->ck_count - 1];
    w->pin_has = 1;
    memcpy(w->pin_mig, pr->plan_hash, sizeof w->pin_mig);
    gc_spool(a);

    /* Prepare the successor aside: the worker builds and checks it; nothing installed changes. */
    su_buf_init(&req);
    st = migrate_prepare_request(a, w, &op, op.to_gen, cur, &req);
    if (st != SU_OK) {
        su_buf_free(&req);
        return call_fail(c, st, "migration documents or candidates are missing from the spool");
    }
    su_buf_init(&resp);
    st = roundtrip(a, AGENT_INTERNAL_ID, &req, &resp);
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return worker_died(a, c, &inop);
    }
    reply_parse(&r, resp.data, resp.len);
    if (!r.ok) {
        return reject_from(c, &r, &resp);
    }
    ju64(jf(r.result, "timings"), "prepare_ns", &prep_ns);
    ju64(jf(r.result, "timings"), "transform_ns", &trans_ns);
    reply_free(&r);
    su_buf_free(&resp);

    /* Swap: publication in the worker. A refusal aborts the pending candidate. */
    su_buf_init(&req);
    req_begin(&req, AGENT_INTERNAL_ID, "internal.world.migrate_swap");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, w->id, strlen(w->id));
    su_buf_str(&req, ",\"expected_world_revision\":");
    su_buf_quoted(&req, cur, strlen(cur));
    su_buf_str(&req, ",\"plan_hash\":");
    su_buf_quoted(&req, pr->plan_hash, strlen(pr->plan_hash));
    req_end(&req);
    su_buf_init(&resp);
    swap_ns = mono_ns();
    st = roundtrip(a, AGENT_INTERNAL_ID, &req, &resp);
    swap_ns = mono_ns() - swap_ns;
    su_buf_free(&req);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return worker_died(a, c, &inop);
    }
    reply_parse(&r, resp.data, resp.len);
    if (!r.ok) {
        SuBuf ab;
        uint32_t ast;
        reply_free(&r);
        su_buf_free(&resp);
        su_buf_init(&ab);
        req_begin(&ab, AGENT_INTERNAL_ID, "internal.world.migrate_abort");
        su_buf_str(&ab, "\"world_id\":");
        su_buf_quoted(&ab, w->id, strlen(w->id));
        req_end(&ab);
        su_buf_init(&resp);
        ast = roundtrip(a, AGENT_INTERNAL_ID, &ab, &resp);
        su_buf_free(&ab);
        if (ast != SU_OK) {
            su_buf_free(&resp);
            return worker_died(a, c, &inop);
        }
        su_buf_free(&resp);
        return call_fail(c, SU_E_STALE_REVISION, "migration swap refused; the pending candidate was discarded");
    }
    ju64(r.result, "tick", &swap_tick);
    ju64(r.result, "layout_generation", &layout);
    jstr(r.result, "migration_hash", mig_hash, sizeof mig_hash);
    {
        const SuJsonValue *man = jf(r.result, "semantic_manifest");
        uint32_t m;
        int ok = man != NULL && man->type == SU_JSON_ARRAY && man->u.array.count == op.n;
        for (m = 0; ok && m < op.n; m++) {
            char sid[SU_DIAG_SYSTEM_BYTES];
            uint32_t q;
            int found = 0;
            if (!jstr(man->u.array.items[m], "system_id", sid, sizeof sid)) {
                ok = 0;
                break;
            }
            for (q = 0; q < op.n; q++) {
                if (strcmp(op.sys[q].system_id, sid) == 0) {
                    ordered[m] = op.sys[q];
                    found = 1;
                }
            }
            ok = found;
        }
        if (!ok) {
            reply_free(&r);
            su_buf_free(&resp);
            mark_unrecoverable(w, "swap answer does not name the replacement systems");
            return call_fail(c, SU_E_INTERNAL, "world is unrecoverable: swap answer does not name the replacement systems");
        }
    }
    reply_free(&r);
    su_buf_free(&resp);
    memcpy(op.sys, ordered, sizeof ordered);
    memcpy(oldsys, w->sys, sizeof oldsys);
    oldcount = w->count;
    memcpy(w->sys, ordered, sizeof ordered);
    w->count = op.n;
    snprintf(op.plan_hash, sizeof op.plan_hash, "%s", pr->plan_hash);
    pre.rev = w->rev;
    pre.tick = w->tick;
    snprintf(pre.hash, sizeof pre.hash, "%s", w->hash);
    pre.ticks_since_ck = w->ticks_since_ck;
    pre.log_count = w->log_count;
    pre.ck_count = w->ck_count;
    pre.count = oldcount;
    pre.migrated = w->migrated;
    memcpy(pre.sys, oldsys, sizeof pre.sys);

    /* Log the migration: the worker's digest after it must equal the acknowledged state. */
    memset(&lop, 0, sizeof lop);
    lop.mig = op;
    rc = sync_after(a, w, LOG_MIGRATE, &lop, 0, 1);
    if (rc == CK_DIED) {
        memcpy(w->sys, oldsys, sizeof oldsys);
        w->count = oldcount;
        return worker_died(a, c, &inop);
    }
    if (rc == CK_BAD) {
        return call_fail(c, SU_E_INTERNAL, "migrated state diverged from the worker; world is unrecoverable");
    }

    /* Post-commit checkpoint: it must be durable before the reply (contract section 4, B9). It records the
     * migrated flag, so recovery builds the world from it rather than from the fixture. A failure of the
     * checkpoint leaves the migration undone: the world is the pre-commit world (contract section 1). */
    w->migrated = 1;
    t0 = mono_ns();
    rc = POST_CK_NOROOM() ? CK_NOROOM : checkpoint_world(a, w, 0);
    if (rc == CK_DIED) {
        /* The first death: recovery replays LOG_MIGRATE from the pinned checkpoint; the checkpoint is taken again. */
        recover(a, c, &inop);
        if (w->unrecoverable) {
            return call_fail(c, SU_E_INTERNAL, "world is unrecoverable: the post-migration state could not be rebuilt");
        }
        rc = checkpoint_world(a, w, 0);
        if (rc == CK_DIED) {
            /* The second death: the migration is undone, so the log and the world agree on the old state. */
            return undo_migration(a, w, c, &inop, &pre, SU_E_WORKER_FAILED,
                                  "post-migration checkpoint failed twice; the migration was rolled back to the pinned "
                                  "pre-migration checkpoint; retry the commit",
                                  "post_commit_checkpoint_failed");
        }
    }
    if (rc == CK_NOROOM) {
        return undo_migration(a, w, c, &inop, &pre, SU_E_RESOURCE_LIMIT,
                              "post-commit checkpoint has no room; the migration was rolled back",
                              "post_commit_checkpoint");
    }
    if (rc != CK_OK) {
        return call_fail(c, SU_E_INTERNAL, "post-migration checkpoint disagreed with the acknowledged state; world is unrecoverable");
    }
    ck_ns = mono_ns() - t0;
    w->migrated = 1;

    wrev(w, w->rev, rv, sizeof rv);
    su_buf_str(res, "{\"world_id\":");
    su_buf_quoted(res, w->id, strlen(w->id));
    su_buf_str(res, ",\"world_revision\":");
    su_buf_quoted(res, rv, strlen(rv));
    su_buf_str(res, ",\"tick\":");
    su_buf_u64(res, w->tick);
    su_buf_str(res, ",\"layout_generation\":");
    su_buf_u64(res, layout);
    su_buf_str(res, ",\"migration_hash\":");
    su_buf_quoted(res, mig_hash, strlen(mig_hash));
    su_buf_str(res, ",\"plan_id\":");
    su_buf_quoted(res, plan_id, strlen(plan_id));
    su_buf_str(res, ",\"rollback_available\":true,\"timings\":{\"plan_ns\":");
    su_buf_u64(res, pr->plan_ns);
    su_buf_str(res, ",\"prepare_ns\":");
    su_buf_u64(res, prep_ns);
    su_buf_str(res, ",\"transform_ns\":");
    su_buf_u64(res, trans_ns);
    su_buf_str(res, ",\"swap_ns\":");
    su_buf_u64(res, swap_ns);
    su_buf_str(res, ",\"checkpoint_ns\":");
    su_buf_u64(res, ck_ns);
    su_buf_str(res, ",\"total_ns\":");
    su_buf_u64(res, mono_ns() - total_t0);
    su_buf_str(res, "},\"bytes\":{\"new_columns\":");
    su_buf_u64(res, pr->bytes_new_columns);
    su_buf_str(res, ",\"peak\":");
    su_buf_u64(res, pr->bytes_peak);
    su_buf_str(res, "},\"systems\":[");
    for (k = 0; k < op.n; k++) {
        if (k > 0) {
            su_buf_str(res, ",");
        }
        su_buf_str(res, "{\"system_id\":");
        su_buf_quoted(res, op.sys[k].system_id, strlen(op.sys[k].system_id));
        su_buf_str(res, ",\"semantic_hash\":");
        su_buf_quoted(res, op.sys[k].semantic_hash, strlen(op.sys[k].semantic_hash));
        su_buf_str(res, "}");
    }
    su_buf_str(res, "]}");
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

/* world.migrate_rollback {world_id, expected_world_revision}: restores the pinned pre-migration checkpoint as a new
 * revision. Changes made after the pin are lost; the reply says so. */
static uint32_t h_world_migrate_rollback(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"world_id", "expected_world_revision"};
    const char *world_id = NULL;
    const char *expect = NULL;
    AgentWorld *w = NULL;
    InFlight op;
    char cur[AGENT_REV_CAP];
    char rv[AGENT_REV_CAP];
    uint64_t tick_before;
    uint64_t discarded;
    int retained = 0;
    uint32_t st;

    if (check_keys(params, allowed, 2, c) != SU_OK || param_str(params, "world_id", 1, &world_id, c) != SU_OK ||
        param_str(params, "expected_world_revision", 1, &expect, c) != SU_OK) {
        return SU_E_TYPE;
    }
    st = world_for_call(a, world_id, &w, c);
    if (st != SU_OK) {
        return st;
    }
    st = preflight(a, c);
    if (st != SU_OK) {
        return st;
    }
    wrev(w, w->rev, cur, sizeof cur);
    if (strcmp(expect, cur) != 0) {
        return call_fail(c, SU_E_STALE_REVISION, "expected_world_revision does not match the active world");
    }
    if (!w->pin_has) {
        return call_fail(c, SU_E_STALE_REVISION, "rollback_unavailable: no pinned pre-migration checkpoint is retained");
    }
    tick_before = w->tick;
    discarded = tick_before > w->pin.tick ? tick_before - w->pin.tick : 0u;
    memset(&op, 0, sizeof op);
    op.kind = OPK_ROLLBACK;
    op.w = w;
    op.tick = w->tick;
    st = rollback_pinned(a, w, c, &op, &retained);
    if (st != SU_OK) {
        return st;
    }
    wrev(w, w->rev, rv, sizeof rv);
    su_buf_str(res, "{\"world_id\":");
    su_buf_quoted(res, w->id, strlen(w->id));
    su_buf_str(res, ",\"world_revision\":");
    su_buf_quoted(res, rv, strlen(rv));
    su_buf_str(res, ",\"tick\":");
    su_buf_u64(res, w->tick);
    su_buf_str(res, ",\"restored_from\":");
    su_buf_quoted(res, w->pin_mig, strlen(w->pin_mig));
    su_buf_str(res, ",\"changes_lost\":true,\"discarded_ticks\":");
    su_buf_u64(res, discarded);
    su_buf_str(res, retained ? ",\"lineage\":\"retained\"}" : ",\"lineage\":\"lost\"}");
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

static uint32_t route(SuAgent *a, const SuRequest *req, SuBuf *res, AgentCall *c) {
    const SuJsonValue *p = req->params;
    if (strcmp(req->method, "engine.info") == 0) {
        return h_engine_info(a, p, res, c);
    }
    if (strcmp(req->method, "capsule.submit") == 0) {
        return h_capsule_submit(a, p, res, c);
    }
    if (strcmp(req->method, "candidate.status") == 0) {
        return h_candidate_status(a, p, res, c);
    }
    if (strcmp(req->method, "candidate.evaluate") == 0) {
        return h_candidate_evaluate(a, p, res, c);
    }
    if (strcmp(req->method, "world.create") == 0) {
        return h_world_create(a, p, res, c);
    }
    if (strcmp(req->method, "world.inspect") == 0) {
        return h_world_inspect(a, p, res, c);
    }
    if (strcmp(req->method, "world.step") == 0) {
        return h_world_step(a, p, res, c);
    }
    if (strcmp(req->method, "world.spawn") == 0) {
        return h_world_spawn(a, p, res, c);
    }
    if (strcmp(req->method, "world.despawn") == 0) {
        return h_world_despawn(a, p, res, c);
    }
    if (strcmp(req->method, "world.migrate_plan") == 0) {
        return h_world_migrate_plan(a, p, res, c);
    }
    if (strcmp(req->method, "world.migrate_commit") == 0) {
        return h_world_migrate_commit(a, p, res, c);
    }
    if (strcmp(req->method, "world.migrate_rollback") == 0) {
        return h_world_migrate_rollback(a, p, res, c);
    }
    if (strcmp(req->method, "events.read") == 0) {
        return h_events_read(a, p, res, c);
    }
    if (strcmp(req->method, "candidate.install") == 0) {
        return h_candidate_install(a, p, res, c);
    }
    if (strcmp(req->method, "world.snapshot") == 0) {
        return h_world_snapshot(a, p, res, c);
    }
    if (strcmp(req->method, "world.restore") == 0) {
        return h_world_restore(a, p, res, c);
    }
    if (strcmp(req->method, "world.close") == 0) {
        return h_world_close(a, p, res, c);
    }
#ifdef SU_HAVE_NATIVE
    if (strcmp(req->method, "world.specialize") == 0) {
        return h_world_specialize(a, p, res, c);
    }
#endif
    return call_fail(c, SU_E_UNSUPPORTED_METHOD, "method is unknown or not implemented in M0");
}

/* Adds one top-level member to a complete response line (an object) before its closing brace. */
static void attach_member(SuBuf *out, const char *key, const SuBuf *val) {
    SuBuf tmp;
    if (out->failed || out->len == 0 || out->data[out->len - 1] != '}') {
        return;
    }
    su_buf_init(&tmp);
    su_buf_put(&tmp, out->data, out->len - 1);
    su_buf_str(&tmp, ",\"");
    su_buf_str(&tmp, key);
    su_buf_str(&tmp, "\":");
    su_buf_put(&tmp, val->data, val->len);
    su_buf_str(&tmp, "}");
    if (tmp.failed) {
        su_buf_free(&tmp);
        return;
    }
    su_buf_free(out);
    *out = tmp;
}

static void agent_handle(SuAgent *a, const uint8_t *line, size_t len, int overflow, SuBuf *out) {
    SuJsonDoc doc;
    SuRequest req;
    char msg[AGENT_MSG_CAP];
    AgentCall c;
    SuBuf res;
    uint32_t st;

    st = su_proto_parse_request(line, len, overflow, &doc, &req, msg, sizeof msg);
    if (st != SU_OK) {
        su_proto_error(out, req.has_id, req.id, st, msg);
        su_json_doc_free(&doc);
        return;
    }
    su_buf_init(&res);
    memset(&c, 0, sizeof c);
    c.id = req.id;
    st = route(a, &req, &res, &c);
    /* M1-02c-2: deferred respecialization runs after a reply to a non-tick request that reports no
     * recovery, so the answer that reports a recovery shows the VM tier with the request pending. */
    a->service_respec = strcmp(req.method, "world.step") != 0 && !c.has_rec;
    if (st != SU_OK) {
        su_proto_error_detail(out, req.has_id, req.id, st, c.msg[0] != '\0' ? c.msg : su_status_name(st),
                              c.detail[0] != '\0' ? c.detail : NULL);
    } else if (c.relay) {
        su_buf_put(out, res.data, res.len);
    } else {
        su_proto_ok(out, req.has_id, req.id, &res);
    }
    if (out->failed) {
        su_buf_free(out);
        su_buf_init(out);
        su_proto_error(out, req.has_id, req.id, SU_E_RESOURCE_LIMIT,
                       "response exceeds the 1 MiB message limit");
    }
    if (c.has_rec) {
        attach_member(out, "recovery", &a->rec);
    } else if (a->event_pending) {
        attach_member(out, "worker_event", &a->event);
        a->event_pending = 0;
        su_buf_free(&a->event);
        su_buf_init(&a->event);
    }
    su_buf_free(&res);
    su_json_doc_free(&doc);
}

static int config_limits(SuAgent *a, const SuAgentConfig *cfg) {
    a->interval = cfg->checkpoint_interval_ticks == 0 ? AGENT_DEFAULT_INTERVAL : cfg->checkpoint_interval_ticks;
    a->retain = cfg->checkpoint_retain == 0 ? AGENT_DEFAULT_RETAIN : cfg->checkpoint_retain;
    a->log_cap = cfg->log_max_entries == 0 ? AGENT_DEFAULT_LOG : cfg->log_max_entries;
    a->quarantine_after = cfg->quarantine_after == 0 ? AGENT_DEFAULT_QUARANTINE : cfg->quarantine_after;
    a->spool_max = cfg->spool_bytes_max == 0 ? AGENT_DEFAULT_SPOOL : cfg->spool_bytes_max;
    if (a->retain > AGENT_MAX_RETAIN || a->log_cap > AGENT_MAX_LOG) {
        return -1;
    }
    a->install_ck = !cfg->no_install_checkpoint;
    a->tick_workers = cfg->tick_workers == 0 ? 1u : cfg->tick_workers;
    a->tick_chunk_min_rows = cfg->tick_chunk_min_rows == 0 ? AGENT_DEFAULT_CHUNK_MIN_ROWS : cfg->tick_chunk_min_rows;
    if (a->tick_workers > AGENT_MAX_TICK_WORKERS) {
        return -1;
    }
    return 0;
}

static void free_all_worlds(SuAgent *a) {
    uint32_t i;
    for (i = 0; i < AGENT_MAX_WORLDS; i++) {
        free(a->world[i].log);
        a->world[i].log = NULL;
    }
}

int su_agent_serve(int in_fd, int out_fd, const SuAgentConfig *cfg) {
    SuAgent *a;
    SuLineReader r;
    struct sigaction ign;
    struct sigaction old;
    int ret = 0;

    if (cfg == NULL || cfg->project_root == NULL) {
        return -1;
    }
    a = calloc(1, sizeof *a);
    if (a == NULL) {
        return -1;
    }
    a->cfg = *cfg;
    a->timeout_ms = cfg->request_timeout_ms == 0 ? AGENT_DEFAULT_TIMEOUT_MS : cfg->request_timeout_ms;
    if (config_limits(a, cfg) != 0 ||
        su_path_canonical_root(cfg->project_root, a->root, sizeof a->root) != SU_OK ||
        su_spool_open(cfg->spool_dir, a->spool_max, &a->spool) != SU_OK) {
        su_spool_close(a->spool);
        free(a);
        return -1;
    }
    a->worker.pid = 0;
    a->worker.alive = 0;
    a->worker.in_fd = -1;
    a->worker.out_fd = -1;
    su_buf_init(&a->rec);
    su_buf_init(&a->event);
    su_buf_init(&a->nat_rec);

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
            ret = -1;
            break;
        }
        su_buf_init(&out);
        agent_handle(a, line, len, overflow, &out);
        if (su_write_all(out_fd, out.data, out.len, -1) != 0 ||
            su_write_all(out_fd, (const uint8_t *)"\n", 1, -1) != 0) {
            su_buf_free(&out);
            ret = -1;
            break;
        }
        su_buf_free(&out);
        if (a->service_respec) {
            a->service_respec = 0;
            respec_service(a);
        }
    }
    su_proc_finish(&a->worker, 1000);
    su_line_reader_free(&r);
    free_all_worlds(a);
    su_spool_close(a->spool);
    su_buf_free(&a->rec);
    su_buf_free(&a->event);
    su_buf_free(&a->nat_rec);
    free(a);
    sigaction(SIGPIPE, &old, NULL);
    return ret;
}
