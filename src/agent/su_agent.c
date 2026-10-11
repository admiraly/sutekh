#define _POSIX_C_SOURCE 200809L

#include "agent/su_agent.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent/su_module.h"
#include "agent/su_path.h"
#include "agent/su_proc.h"
#include "agent/su_proto.h"
#include "ir/su_json.h"
#include "ir/su_lir.h"
#include "sutekh/abi.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"

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
#define AGENT_MAX_PARAM_STRING 255u
#define AGENT_MAX_FIELD_ARGS 64u

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
} AgentCandidate;

typedef struct SuAgent {
    SuAgentConfig cfg;
    char root[SU_PATH_BUF];
    uint32_t timeout_ms;
    SuProc worker; /* the long-lived active worker; holds the worlds */
    uint64_t next_candidate;
    uint64_t next_eval;
    AgentCandidate cand[AGENT_MAX_CANDIDATES];
} SuAgent;

typedef struct AgentCall {
    const char *id; /* the client request id; always set before routing */
    char msg[AGENT_MSG_CAP];
    int relay; /* res holds one complete worker response line, to be sent as-is */
} AgentCall;

static const char *const k_capabilities[] = {
    "engine.info",   "capsule.submit", "candidate.status", "candidate.evaluate",
    "world.create",  "world.inspect",  "world.step",
};

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

/* ---------- worker round trip ---------- */

static uint32_t ensure_worker(SuAgent *a, AgentCall *c) {
    SuProcLimits lim;
    if (a->worker.alive) {
        return SU_OK;
    }
    if (a->cfg.worker_argv == NULL) {
        return call_fail(c, SU_E_WORKER_FAILED, "no worker executable is configured");
    }
    memset(&lim, 0, sizeof lim);
    lim.address_space_bytes = a->cfg.worker_address_space_bytes;
    if (su_proc_spawn(&a->worker, a->cfg.worker_argv, &lim) != SU_OK) {
        return call_fail(c, SU_E_WORKER_FAILED, "active worker could not be started");
    }
    return SU_OK;
}

/* Sends one request line to the active worker and copies back its response.
 * The response must be one object with protocol "0.1", the same id, and a
 * boolean ok. Any other outcome kills the worker. */
static uint32_t active_roundtrip(SuAgent *a, const char *id, const SuBuf *req, SuBuf *resp,
                                 AgentCall *c) {
    const uint8_t *line = NULL;
    size_t len = 0;
    SuJsonLimits lim;
    SuJsonDoc doc;
    SuJsonError err;
    const SuJsonValue *v;
    uint32_t st;
    int64_t deadline;

    st = ensure_worker(a, c);
    if (st != SU_OK) {
        return st;
    }
    deadline = su_now_ms() + (int64_t)a->timeout_ms;
    st = su_proc_request(&a->worker, req->data, req->len, deadline, &line, &len);
    if (st != SU_OK) {
        return call_fail(c, st == SU_E_RESOURCE_LIMIT ? st : SU_E_WORKER_FAILED,
                         st == SU_E_RESOURCE_LIMIT
                             ? "worker response exceeded 1 MiB; worker killed"
                             : "active worker died or timed out; it was killed and its worlds are lost");
    }
    su_buf_put(resp, line, len);
    if (resp->failed) {
        su_proc_kill(&a->worker);
        return call_fail(c, SU_E_WORKER_FAILED, "worker response was malformed; worker killed");
    }

    memset(&doc, 0, sizeof doc);
    lim.max_bytes = resp->len;
    lim.max_depth = SU_PROTO_MAX_DEPTH + 4u;
    st = su_json_parse(resp->data, resp->len, &lim, &doc, &err);
    v = st == SU_OK && doc.root != NULL && doc.root->type == SU_JSON_OBJECT
            ? su_json_member(doc.root, "id")
            : NULL;
    if (st != SU_OK || v == NULL || !su_json_is_string(v) || v->u.string.len != strlen(id) ||
        memcmp(v->u.string.data, id, strlen(id)) != 0 ||
        su_json_member(doc.root, "ok") == NULL || su_json_member(doc.root, "ok")->type != SU_JSON_BOOL) {
        su_json_doc_free(&doc);
        su_proc_kill(&a->worker);
        return call_fail(c, SU_E_WORKER_FAILED, "worker response was malformed; worker killed");
    }
    su_json_doc_free(&doc);
    return SU_OK;
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
    st = su_path_sibling(a->root, cap_real, source, src_real, SU_PATH_BUF);
    if (st != SU_OK) {
        free(*cap_bytes);
        *cap_bytes = NULL;
        return call_fail(c, st, "capsule source path rejected by the project path policy");
    }
    return SU_OK;
}

static void buf_hash_field(SuBuf *b, const char *key, const char *hex) {
    su_buf_str(b, ",\"");
    su_buf_str(b, key);
    su_buf_str(b, "\":");
    su_buf_quoted(b, hex, strlen(hex));
}

/* ---------- handlers ---------- */

static uint32_t h_engine_info(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    size_t i;
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
    su_buf_str(res, "},\"capabilities\":[");
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
    static const char *const allowed[] = {"capsule_path"};
    const char *rel = NULL;
    char cap_real[SU_PATH_BUF];
    char src_real[SU_PATH_BUF];
    uint8_t *cap = NULL;
    uint8_t *lir = NULL;
    size_t cap_len = 0;
    size_t lir_len = 0;
    SuDiag diag;
    SuLirModule mod;
    AgentCandidate *slot = NULL;
    size_t i;
    uint32_t st;

    if (check_keys(params, allowed, 1, c) != SU_OK || param_str(params, "capsule_path", 1, &rel, c) != SU_OK) {
        return SU_E_TYPE;
    }
    st = locate_capsule(a, rel, cap_real, src_real, &cap, &cap_len, c);
    if (st != SU_OK) {
        return st;
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
    free(cap);
    free(lir);

    for (i = 0; i < AGENT_MAX_CANDIDATES; i++) {
        if (!a->cand[i].used) {
            slot = &a->cand[i];
            break;
        }
    }
    if (slot == NULL) {
        su_lir_module_free(&mod);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "candidate registry is full");
    }
    memset(slot, 0, sizeof *slot);
    slot->used = 1;
    a->next_candidate++;
    snprintf(slot->id, sizeof slot->id, "candidate-%llu", (unsigned long long)a->next_candidate);
    snprintf(slot->system_id, sizeof slot->system_id, "%s", mod.system_id);
    snprintf(slot->capsule_real, sizeof slot->capsule_real, "%s", cap_real);
    snprintf(slot->source_real, sizeof slot->source_real, "%s", src_real);
    snprintf(slot->source_hash, sizeof slot->source_hash, "%s", mod.source_hash_hex);
    snprintf(slot->capsule_hash, sizeof slot->capsule_hash, "%s", mod.capsule_hash_hex);
    snprintf(slot->semantic_hash, sizeof slot->semantic_hash, "%s", mod.semantic_hash_hex);
    snprintf(slot->eval_status, sizeof slot->eval_status, "not_run");

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
    memset(&plim, 0, sizeof plim);
    plim.address_space_bytes = a->cfg.worker_address_space_bytes;
    plim.cpu_seconds = (uint32_t)(timeout / 1000u) + 2u;
    if (su_proc_spawn(&p, a->cfg.worker_argv, &plim) != SU_OK) {
        su_buf_free(&req);
        cd->eval_status[0] = '\0';
        snprintf(cd->eval_status, sizeof cd->eval_status, "worker_failed");
        return call_fail(c, SU_E_WORKER_FAILED, "evaluation worker could not be started");
    }
    {
        const uint8_t *line = NULL;
        size_t len = 0;
        su_buf_init(&resp);
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

/* Forwards a validated world request to the active worker with the client's id
 * and relays the worker's response line unchanged. */
static uint32_t relay_world(SuAgent *a, const SuBuf *req, SuBuf *res, AgentCall *c) {
    SuBuf resp;
    uint32_t st;

    su_buf_init(&resp);
    st = active_roundtrip(a, c->id, req, &resp, c);
    if (st != SU_OK) {
        su_buf_free(&resp);
        return st;
    }
    su_buf_put(res, resp.data, resp.len);
    su_buf_free(&resp);
    c->relay = 1;
    return res->failed ? SU_E_RESOURCE_LIMIT : SU_OK;
}

static void req_begin(SuBuf *req, const char *id, const char *method) {
    su_buf_init(req);
    su_buf_str(req, "{\"protocol\":\"" SU_PROTOCOL_VERSION "\",\"id\":");
    su_buf_quoted(req, id, strlen(id));
    su_buf_str(req, ",\"method\":\"");
    su_buf_str(req, method);
    su_buf_str(req, "\",\"params\":{");
}

static uint32_t h_world_create(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"fixture_path", "capsule_path"};
    const char *fixture = NULL;
    const char *capsule = NULL;
    char fx_real[SU_PATH_BUF];
    char cap_real[SU_PATH_BUF];
    char src_real[SU_PATH_BUF];
    uint8_t *cap = NULL;
    size_t cap_len = 0;
    SuBuf req;
    uint32_t st;

    if (check_keys(params, allowed, 2, c) != SU_OK ||
        param_str(params, "fixture_path", 1, &fixture, c) != SU_OK ||
        param_str(params, "capsule_path", 1, &capsule, c) != SU_OK) {
        return SU_E_TYPE;
    }
    st = su_path_resolve(a->root, fixture, fx_real, sizeof fx_real);
    if (st != SU_OK) {
        return call_fail(c, st, "fixture path rejected by the project path policy");
    }
    st = locate_capsule(a, capsule, cap_real, src_real, &cap, &cap_len, c);
    if (st != SU_OK) {
        return st;
    }
    free(cap);

    req_begin(&req, c->id, "world.create");
    su_buf_str(&req, "\"fixture_path\":");
    su_buf_quoted(&req, fx_real, strlen(fx_real));
    su_buf_str(&req, ",\"capsule_path\":");
    su_buf_quoted(&req, cap_real, strlen(cap_real));
    su_buf_str(&req, ",\"source_path\":");
    su_buf_quoted(&req, src_real, strlen(src_real));
    su_buf_str(&req, "}}");
    if (req.failed) {
        su_buf_free(&req);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "world.create request too large");
    }
    st = relay_world(a, &req, res, c);
    su_buf_free(&req);
    return st;
}

static uint32_t h_world_inspect(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"world_id", "fields", "limit"};
    const char *world_id = NULL;
    uint64_t limit = 16;
    const SuJsonValue *fields = su_json_member(params, "fields");
    SuBuf req;
    size_t i;
    uint32_t st;

    if (check_keys(params, allowed, 3, c) != SU_OK ||
        param_str(params, "world_id", 1, &world_id, c) != SU_OK ||
        param_uint(params, "limit", 0, 1, 256, 16, &limit, c) != SU_OK) {
        return SU_E_TYPE;
    }
    if (fields != NULL && (fields->type != SU_JSON_ARRAY || fields->u.array.count > AGENT_MAX_FIELD_ARGS)) {
        return call_fail(c, SU_E_TYPE, "fields must be an array of at most 64 names");
    }
    if (!a->worker.alive) {
        return call_fail(c, SU_E_INVALID_ARGUMENT, "no active worker holds worlds; call world.create");
    }
    req_begin(&req, c->id, "world.inspect");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, world_id, strlen(world_id));
    su_buf_str(&req, ",\"limit\":");
    su_buf_u64(&req, limit);
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
    su_buf_str(&req, "}}");
    if (req.failed) {
        su_buf_free(&req);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "world.inspect request too large");
    }
    st = relay_world(a, &req, res, c);
    su_buf_free(&req);
    return st;
}

static uint32_t h_world_step(SuAgent *a, const SuJsonValue *params, SuBuf *res, AgentCall *c) {
    static const char *const allowed[] = {"world_id", "expected_world_revision", "ticks"};
    const char *world_id = NULL;
    const char *expect = NULL;
    uint64_t ticks = 0;
    SuBuf req;
    uint32_t st;

    if (check_keys(params, allowed, 3, c) != SU_OK ||
        param_str(params, "world_id", 1, &world_id, c) != SU_OK ||
        param_str(params, "expected_world_revision", 1, &expect, c) != SU_OK ||
        param_uint(params, "ticks", 1, 0, AGENT_MAX_TICKS, 0, &ticks, c) != SU_OK) {
        return SU_E_TYPE;
    }
    if (!a->worker.alive) {
        return call_fail(c, SU_E_INVALID_ARGUMENT, "no active worker holds worlds; call world.create");
    }
    req_begin(&req, c->id, "world.step");
    su_buf_str(&req, "\"world_id\":");
    su_buf_quoted(&req, world_id, strlen(world_id));
    su_buf_str(&req, ",\"expected_world_revision\":");
    su_buf_quoted(&req, expect, strlen(expect));
    su_buf_str(&req, ",\"ticks\":");
    su_buf_u64(&req, ticks);
    su_buf_str(&req, "}}");
    if (req.failed) {
        su_buf_free(&req);
        return call_fail(c, SU_E_RESOURCE_LIMIT, "world.step request too large");
    }
    st = relay_world(a, &req, res, c);
    su_buf_free(&req);
    return st;
}

/* ---------- dispatch ---------- */

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
    return call_fail(c, SU_E_UNSUPPORTED_METHOD, "method is unknown or not implemented in M0");
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
    if (st != SU_OK) {
        su_proto_error(out, req.has_id, req.id, st, c.msg[0] != '\0' ? c.msg : su_status_name(st));
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
    su_buf_free(&res);
    su_json_doc_free(&doc);
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
    if (su_path_canonical_root(cfg->project_root, a->root, sizeof a->root) != SU_OK) {
        free(a);
        return -1;
    }
    a->worker.pid = 0;
    a->worker.alive = 0;
    a->worker.in_fd = -1;
    a->worker.out_fd = -1;

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
    }
    su_proc_finish(&a->worker, 1000);
    su_line_reader_free(&r);
    free(a);
    sigaction(SIGPIPE, &old, NULL);
    return ret;
}
