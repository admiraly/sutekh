/* Effect analysis and schedule graph (M1-04). See su_effects.h. */
#define _POSIX_C_SOURCE 200809L

#include "runtime/jobs/su_effects.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_ops03.h"
#include "sutekh/ir.h"

struct SuEffectGraph {
    uint32_t n;
    char (*ids)[SU_DIAG_SYSTEM_BYTES];
    uint32_t *rank;        /* sorted phase rank */
    uint32_t *priority;
    uint8_t *parallel;
    uint32_t *nf;          /* field count per system */
    uint32_t *fid;         /* n * SU_LIMIT_FIELDS: field IDs */
    uint8_t *rd;           /* n * SU_LIMIT_FIELDS: derived reads */
    uint8_t *wr;           /* n * SU_LIMIT_FIELDS: derived writes */
    uint8_t *adj;          /* n * n: adj[a*n+b] = a must run before b */
    uint8_t *reach;        /* n * n: transitive closure of adj */
    uint8_t *conf;         /* n * n: effects conflict on some field (within one table) */
    uint32_t *tab;         /* n: table of each system (issue 140) */
    uint32_t *order;       /* canonical serial order */
};

static void set_err(char *err, size_t cap, const char *fmt, ...) {
    va_list ap;
    if (err == NULL || cap == 0u) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

uint32_t su_effects_derive(const SuLirModule *m, uint8_t *reads, uint8_t *writes) {
    uint32_t i;
    if (m == NULL || reads == NULL || writes == NULL || m->field_count > SU_LIMIT_FIELDS) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(reads, 0, m->field_count > 0u ? m->field_count : 1u);
    memset(writes, 0, m->field_count > 0u ? m->field_count : 1u);
    for (i = 0; i < m->insn_count; i++) {
        const SuInsn *ins = &m->insns[i];
        if (ins->op == SU_OP_LOAD_U32) {
            if (ins->a >= m->field_count) {
                return SU_E_INTERNAL;
            }
            reads[ins->a] = 1u;
        } else if (ins->op == SU_OP_STORE_U32) {
            if (ins->a >= m->field_count) {
                return SU_E_INTERNAL;
            }
            writes[ins->a] = 1u;
        } else {
            /* IR 0.3 (2026-10-18): field operands from the operation table; a 64-bit field operand names lane 0
             * and covers lanes idx and idx + 1. Opcodes of 0.1 and 0.2 are not in the table. */
            const SuOp03 *o = su_op03_by_code(ins->op);
            uint32_t k;
            for (k = 0; o != NULL && k < o->n; k++) {
                uint32_t kind = o->o[k].kind, idx, lanes, l;
                if (kind != SU_O_FREAD && kind != SU_O_FWRITE && kind != SU_O_FGATHER) {
                    continue;
                }
                idx = o->o[k].slot == SU_S_A ? ins->a : (o->o[k].slot == SU_S_B ? ins->b : ins->c);
                lanes = su_type_lanes(o->o[k].type);
                if (lanes == 0u || idx >= m->field_count || lanes > m->field_count - idx) {
                    return SU_E_INTERNAL;
                }
                for (l = 0; l < lanes; l++) {
                    if (kind != SU_O_FWRITE) { /* load, or an IR 0.4 cross-row read (gather_*, find_eq_u32) */
                        reads[idx + l] = 1u;
                    } else {
                        writes[idx + l] = 1u;
                    }
                }
            }
        }
    }
    if (m->has_row_filter) {
        /* IR 0.4: the row filter reads its field for every row, and a filtered-out row copies every writable
         * lane from its input (so a filtered system reads each field it writes). */
        if (m->row_filter_field >= m->field_count) {
            return SU_E_INTERNAL;
        }
        reads[m->row_filter_field] = 1u;
        for (i = 0; i < m->field_count; i++) {
            if ((m->fields[i].access & SU_ACCESS_WRITE) != 0u) {
                reads[i] = 1u;
            }
        }
    }
    return SU_OK;
}

uint32_t su_effects_check_declared(const SuLirModule *m, char *err, size_t err_cap) {
    uint8_t rd[SU_LIMIT_FIELDS];
    uint8_t wr[SU_LIMIT_FIELDS];
    uint32_t i;
    uint32_t st;

    st = su_effects_derive(m, rd, wr);
    if (st != SU_OK) {
        return st;
    }
    for (i = 0; i < m->field_count; i++) {
        uint32_t acc = m->fields[i].access;
        int dr = (acc & SU_ACCESS_READ) != 0u;
        int dw = (acc & SU_ACCESS_WRITE) != 0u;
        if (dr != (rd[i] != 0u) || dw != (wr[i] != 0u)) {
            set_err(err, err_cap,
                    "system %s: field %s declared read=%d write=%d but IR reads=%d writes=%d (the capsule's "
                    "read/write flags must equal what the IR actually loads and stores: %s)",
                    m->system_id, m->fields[i].name, (int)dr, (int)dw, (int)rd[i], (int)wr[i],
                    dr && !rd[i]   ? "set read:false or load the field"
                    : !dr && rd[i] ? "set read:true"
                    : dw && !wr[i] ? "set write:false or store the field"
                                   : "set write:true");
            return SU_E_SCHEMA_MISMATCH;
        }
    }
    return SU_OK;
}

static int find_index(const SuEffectGraph *g, const char *id) {
    uint32_t j;
    for (j = 0; j < g->n; j++) {
        if (strcmp(g->ids[j], id) == 0) {
            return (int)j;
        }
    }
    return -1;
}

/* Builds the shortest cycle through node s (s reaches s) and writes it to err. */
static void report_cycle(const SuEffectGraph *g, uint32_t s, char *err, size_t cap) {
    uint32_t n = g->n;
    uint32_t *parent = malloc(n * sizeof *parent);
    uint32_t *queue = malloc(n * sizeof *queue);
    uint8_t *seen = calloc(n, 1u);
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t cur;
    uint32_t path[SU_EFFECTS_MAX_SYSTEMS + 1u];
    uint32_t len = 0;
    uint32_t k;
    size_t pos = 0;

    if (parent == NULL || queue == NULL || seen == NULL) {
        set_err(err, cap, "ordering cycle detected");
        free(parent);
        free(queue);
        free(seen);
        return;
    }
    cur = UINT32_MAX;
    if (g->adj[s * n + s] != 0u) {
        cur = s;
    }
    /* BFS from the successors of s (not revisiting s) until a node has an edge back to s. */
    for (k = 0; k < n && cur == UINT32_MAX; k++) {
        if (k != s && g->adj[s * n + k] != 0u) {
            parent[k] = s;
            seen[k] = 1u;
            queue[tail++] = k;
        }
    }
    while (head < tail && cur == UINT32_MAX) {
        uint32_t u = queue[head++];
        uint32_t v;
        for (v = 0; v < n; v++) {
            if (g->adj[u * n + v] == 0u) {
                continue;
            }
            if (v == s) {
                cur = u;
                break;
            }
            if (seen[v] == 0u) {
                seen[v] = 1u;
                parent[v] = u;
                queue[tail++] = v;
            }
        }
    }
    if (cur == UINT32_MAX) {
        set_err(err, cap, "ordering cycle through %s", g->ids[s]);
    } else if (cur == s) {
        set_err(err, cap, "ordering cycle: %s -> %s", g->ids[s], g->ids[s]);
    } else {
        /* rev = [s, cur, parent(cur), ..., first successor]; each rev[i+1] -> rev[i]. */
        uint32_t node = cur;
        uint32_t rev[SU_EFFECTS_MAX_SYSTEMS + 2u];
        uint32_t rl = 0;
        rev[rl++] = s;
        while (node != s) {
            rev[rl++] = node;
            node = parent[node];
        }
        path[len++] = s;
        for (k = rl - 1u; k >= 1u; k--) {
            path[len++] = rev[k];
        }
        path[len++] = s;
        pos = (size_t)snprintf(err, cap, "ordering cycle: ");
        for (k = 0; k < len && pos < cap; k++) {
            int w = snprintf(err + pos, cap - pos, "%s%s", k == 0u ? "" : " -> ", g->ids[path[k]]);
            if (w < 0) {
                break;
            }
            pos += (size_t)w;
        }
    }
    free(parent);
    free(queue);
    free(seen);
}

/* Fills g->conf by merging ID-sorted field tables. Returns 1 when done, 0 when a system has duplicate field IDs or
 * memory runs out (the caller then compares exhaustively). */
static int merge_conflicts(SuEffectGraph *g) {
    uint32_t n = g->n;
    uint32_t *ord = malloc((size_t)(n > 0u ? n : 1u) * SU_LIMIT_FIELDS * sizeof *ord);
    uint32_t i, j, k;
    if (ord == NULL) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        const uint32_t *fid = &g->fid[(size_t)i * SU_LIMIT_FIELDS];
        uint32_t *o = &ord[(size_t)i * SU_LIMIT_FIELDS];
        /* Insertion sort: linear on the ascending tables the verifier produces. */
        for (k = 0; k < g->nf[i]; k++) {
            uint32_t at = k;
            while (at > 0u && fid[o[at - 1u]] > fid[k]) {
                o[at] = o[at - 1u];
                at--;
            }
            o[at] = k;
        }
        for (k = 1; k < g->nf[i]; k++) {
            if (fid[o[k - 1u]] == fid[o[k]]) {
                free(ord);
                return 0;
            }
        }
    }
    for (i = 0; i < n; i++) {
        for (j = i + 1u; j < n; j++) {
            const uint32_t *oi = &ord[(size_t)i * SU_LIMIT_FIELDS];
            if (g->tab[i] != g->tab[j]) {
                continue; /* different tables never share columns */
            }
            const uint32_t *oj = &ord[(size_t)j * SU_LIMIT_FIELDS];
            uint32_t a = 0, b = 0;
            while (a < g->nf[i] && b < g->nf[j]) {
                size_t xa = (size_t)i * SU_LIMIT_FIELDS + oi[a];
                size_t xb = (size_t)j * SU_LIMIT_FIELDS + oj[b];
                if (g->fid[xa] < g->fid[xb]) {
                    a++;
                } else if (g->fid[xa] > g->fid[xb]) {
                    b++;
                } else {
                    uint32_t ra = g->rd[xa], wa = g->wr[xa], rb = g->rd[xb], wb = g->wr[xb];
                    if ((wa && wb) || (wa && rb) || (ra && wb)) {
                        g->conf[i * n + j] = 1u;
                        g->conf[j * n + i] = 1u;
                        break;
                    }
                    a++;
                    b++;
                }
            }
        }
    }
    free(ord);
    return 1;
}

uint32_t su_effects_build(const SuEffectInput *in, uint32_t n, SuEffectGraph **out, char *err,
                          size_t err_cap) {
    SuEffectGraph *g;
    uint32_t i;
    uint32_t j;
    uint32_t k;
    uint32_t st = SU_OK;

    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (err != NULL && err_cap > 0u) {
        err[0] = '\0';
    }
    if ((n > 0u && in == NULL) || n > SU_EFFECTS_MAX_SYSTEMS) {
        return n > SU_EFFECTS_MAX_SYSTEMS ? SU_E_RESOURCE_LIMIT : SU_E_INVALID_ARGUMENT;
    }

    g = calloc(1, sizeof *g);
    if (g == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    g->n = n;
    g->ids = calloc(n > 0u ? n : 1u, sizeof *g->ids);
    g->rank = calloc(n > 0u ? n : 1u, sizeof *g->rank);
    g->priority = calloc(n > 0u ? n : 1u, sizeof *g->priority);
    g->parallel = calloc(n > 0u ? n : 1u, sizeof *g->parallel);
    g->nf = calloc(n > 0u ? n : 1u, sizeof *g->nf);
    g->fid = calloc((size_t)(n > 0u ? n : 1u) * SU_LIMIT_FIELDS, sizeof *g->fid);
    g->rd = calloc((size_t)(n > 0u ? n : 1u) * SU_LIMIT_FIELDS, 1u);
    g->wr = calloc((size_t)(n > 0u ? n : 1u) * SU_LIMIT_FIELDS, 1u);
    g->adj = calloc((size_t)(n > 0u ? n * n : 1u), 1u);
    g->reach = calloc((size_t)(n > 0u ? n * n : 1u), 1u);
    g->conf = calloc((size_t)(n > 0u ? n * n : 1u), 1u);
    g->order = calloc(n > 0u ? n : 1u, sizeof *g->order);
    g->tab = calloc(n > 0u ? n : 1u, sizeof *g->tab);
    if (g->ids == NULL || g->rank == NULL || g->priority == NULL || g->parallel == NULL ||
        g->nf == NULL || g->fid == NULL || g->rd == NULL || g->wr == NULL || g->adj == NULL ||
        g->reach == NULL || g->conf == NULL || g->order == NULL || g->tab == NULL) {
        su_effects_free(g);
        return SU_E_RESOURCE_LIMIT;
    }

    /* 1. identity, derived effects, declared-vs-IR check. */
    for (i = 0; i < n && st == SU_OK; i++) {
        const SuEffectInput *e = &in[i];
        size_t len;
        if (e->system_id == NULL || e->phase == NULL || e->mod == NULL) {
            st = SU_E_INVALID_ARGUMENT;
            break;
        }
        len = strlen(e->system_id);
        if (len == 0u || len >= SU_DIAG_SYSTEM_BYTES) {
            st = SU_E_INVALID_ARGUMENT;
            break;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(g->ids[j], e->system_id) == 0) {
                set_err(err, err_cap, "duplicate system_id %s", e->system_id);
                st = SU_E_INVALID_ARGUMENT;
                break;
            }
        }
        if (st != SU_OK) {
            break;
        }
        memcpy(g->ids[i], e->system_id, len + 1u);
        g->priority[i] = e->priority;
        g->parallel[i] = e->declared_parallel != 0u ? 1u : 0u;
        g->tab[i] = e->table;
        if (e->mod->field_count > SU_LIMIT_FIELDS) {
            st = SU_E_RESOURCE_LIMIT;
            break;
        }
        if (e->host_declared == 0u) {
            st = su_effects_check_declared(e->mod, err, err_cap);
            if (st != SU_OK) {
                break;
            }
        }
        g->nf[i] = e->mod->field_count;
        for (k = 0; k < e->mod->field_count; k++) {
            g->fid[(size_t)i * SU_LIMIT_FIELDS + k] = e->mod->fields[k].id;
        }
        if (e->host_declared != 0u) {
            /* Host entry: the declared access is the effect (no IR to derive it from). */
            for (k = 0; k < e->mod->field_count; k++) {
                uint32_t acc = e->mod->fields[k].access;
                g->rd[(size_t)i * SU_LIMIT_FIELDS + k] = (acc & SU_ACCESS_READ) != 0u ? 1u : 0u;
                g->wr[(size_t)i * SU_LIMIT_FIELDS + k] = (acc & SU_ACCESS_WRITE) != 0u ? 1u : 0u;
            }
        } else {
            st = su_effects_derive(e->mod, &g->rd[(size_t)i * SU_LIMIT_FIELDS], &g->wr[(size_t)i * SU_LIMIT_FIELDS]);
            if (st != SU_OK) {
                break;
            }
        }
    }

    /* Phase ranks: the number of distinct phases that sort before this one. */
    for (i = 0; st == SU_OK && i < n; i++) {
        uint32_t r = 0;
        for (j = 0; j < n; j++) {
            int less = strcmp(in[j].phase, in[i].phase) < 0;
            int dup_earlier = 0;
            if (!less) {
                continue;
            }
            /* Count each smaller phase name once: only the first system with it. */
            for (k = 0; k < j; k++) {
                if (strcmp(in[k].phase, in[j].phase) == 0) {
                    dup_earlier = 1;
                    break;
                }
            }
            if (!dup_earlier) {
                r++;
            }
        }
        g->rank[i] = r;
    }

    /* 2. explicit ordering edges. */
    for (i = 0; st == SU_OK && i < n; i++) {
        const SuEffectInput *e = &in[i];
        for (k = 0; st == SU_OK && k < e->after_count; k++) {
            int t = find_index(g, e->after[k]);
            if (t < 0) {
                set_err(err, err_cap, "system %s: after names unknown system %s", g->ids[i], e->after[k]);
                st = SU_E_INVALID_ARGUMENT;
            } else if (strcmp(in[t].phase, e->phase) != 0 &&
                       !(in[t].host_declared != 0u && e->host_declared == 0u && g->rank[t] < g->rank[i])) {
                /* Post-query ordering (2026-10-18): the one admitted cross-phase edge is an IR system's `after`
                 * edge to a host operator of an earlier phase (strcmp order), so the IR system can consume the
                 * operator's results in the same tick. */
                set_err(err, err_cap,
                        "system %s: after edge to %s crosses phases (%s -> %s); only an IR system's after edge to a "
                        "host operator of an earlier phase may cross phases",
                        g->ids[i], e->after[k], in[t].phase, e->phase);
                st = SU_E_INVALID_ARGUMENT;
            } else {
                g->adj[(uint32_t)t * n + i] = 1u;
            }
        }
        for (k = 0; st == SU_OK && k < e->before_count; k++) {
            int t = find_index(g, e->before[k]);
            if (t < 0) {
                set_err(err, err_cap, "system %s: before names unknown system %s", g->ids[i], e->before[k]);
                st = SU_E_INVALID_ARGUMENT;
            } else if (strcmp(in[t].phase, e->phase) != 0) {
                set_err(err, err_cap,
                        "system %s: before edge to %s crosses phases (%s -> %s); use an after edge from the later "
                        "IR system to the host operator instead",
                        g->ids[i], e->before[k], e->phase, in[t].phase);
                st = SU_E_INVALID_ARGUMENT;
            } else {
                g->adj[i * n + (uint32_t)t] = 1u;
            }
        }
    }
    if (st != SU_OK) {
        su_effects_free(g);
        return st;
    }

    /* 3. transitive closure; a self-reaching node is an ordering cycle. */
    memcpy(g->reach, g->adj, (size_t)(n * n));
    for (k = 0; k < n; k++) {
        for (i = 0; i < n; i++) {
            if (g->reach[i * n + k] == 0u) {
                continue;
            }
            for (j = 0; j < n; j++) {
                if (g->reach[k * n + j] != 0u) {
                    g->reach[i * n + j] = 1u;
                }
            }
        }
    }
    for (i = 0; i < n; i++) {
        if (g->reach[i * n + i] != 0u) {
            report_cycle(g, i, err, err_cap);
            su_effects_free(g);
            return SU_E_INVALID_ARGUMENT;
        }
    }

    /* 4. effect conflicts (derived), then rejection for parallel unordered pairs. conf[i][j] is "some field ID
     * both systems bind conflicts", so it does not depend on the order fields are compared in: with unique IDs per
     * system (verified modules and host declarations), a merge over ID-sorted tables gives the same result in
     * O(lanes) per pair instead of O(lanes^2) (issue 144: up to SU_SCHED_MAX_SYSTEMS systems). */
    if (merge_conflicts(g) == 0) {
        /* duplicate IDs in a table, or no memory for the merge: compare every pair of fields */
        for (i = 0; i < n; i++) {
            for (j = i + 1u; j < n; j++) {
                uint32_t a;
                if (g->tab[i] != g->tab[j]) {
                    continue; /* different tables never share columns */
                }
                for (a = 0; a < g->nf[i]; a++) {
                    uint32_t b;
                    uint32_t id = g->fid[(size_t)i * SU_LIMIT_FIELDS + a];
                    for (b = 0; b < g->nf[j]; b++) {
                        uint32_t ra;
                        uint32_t wa;
                        uint32_t rb;
                        uint32_t wb;
                        if (g->fid[(size_t)j * SU_LIMIT_FIELDS + b] != id) {
                            continue;
                        }
                        ra = g->rd[(size_t)i * SU_LIMIT_FIELDS + a];
                        wa = g->wr[(size_t)i * SU_LIMIT_FIELDS + a];
                        rb = g->rd[(size_t)j * SU_LIMIT_FIELDS + b];
                        wb = g->wr[(size_t)j * SU_LIMIT_FIELDS + b];
                        if ((wa && wb) || (wa && rb) || (ra && wb)) {
                            g->conf[i * n + j] = 1u;
                            g->conf[j * n + i] = 1u;
                        }
                    }
                }
            }
        }
    }
    for (i = 0; st == SU_OK && i < n; i++) {
        for (j = i + 1u; st == SU_OK && j < n; j++) {
            uint32_t a;
            if (g->conf[i * n + j] == 0u || g->rank[i] != g->rank[j] || g->parallel[i] == 0u ||
                g->parallel[j] == 0u || g->reach[i * n + j] != 0u || g->reach[j * n + i] != 0u) {
                continue;
            }
            /* Name the first conflicting field for the message. */
            for (a = 0; a < g->nf[i] && st == SU_OK; a++) {
                uint32_t b;
                uint32_t id = g->fid[(size_t)i * SU_LIMIT_FIELDS + a];
                for (b = 0; b < g->nf[j]; b++) {
                    uint32_t ra;
                    uint32_t wa;
                    uint32_t rb;
                    uint32_t wb;
                    const char *kind;
                    if (g->fid[(size_t)j * SU_LIMIT_FIELDS + b] != id) {
                        continue;
                    }
                    ra = g->rd[(size_t)i * SU_LIMIT_FIELDS + a];
                    wa = g->wr[(size_t)i * SU_LIMIT_FIELDS + a];
                    rb = g->rd[(size_t)j * SU_LIMIT_FIELDS + b];
                    wb = g->wr[(size_t)j * SU_LIMIT_FIELDS + b];
                    if (!((wa && wb) || (wa && rb) || (ra && wb))) {
                        continue;
                    }
                    kind = (wa && wb) ? "write/write" : "read/write";
                    set_err(err, err_cap,
                            "systems %s and %s are declared parallel without an ordering edge and conflict on field %u (%s)",
                            g->ids[i], g->ids[j], (unsigned)id, kind);
                    st = SU_E_SCHEMA_MISMATCH;
                    break;
                }
            }
        }
    }
    if (st != SU_OK) {
        su_effects_free(g);
        return st;
    }

    /* 5. canonical serial order: rank, then Kahn by (priority, system_id). */
    {
        uint32_t *indeg = calloc(n > 0u ? n : 1u, sizeof *indeg);
        uint8_t *placed = calloc(n > 0u ? n : 1u, 1u);
        uint32_t pos = 0;
        uint32_t r;
        uint32_t max_rank = 0;
        if (indeg == NULL || placed == NULL) {
            free(indeg);
            free(placed);
            su_effects_free(g);
            return SU_E_RESOURCE_LIMIT;
        }
        for (i = 0; i < n; i++) {
            for (j = 0; j < n; j++) {
                indeg[i] += g->adj[j * n + i];
            }
            if (g->rank[i] > max_rank) {
                max_rank = g->rank[i];
            }
        }
        for (r = 0; r <= max_rank && n > 0u; r++) {
            uint32_t emitted = 0;
            uint32_t members = 0;
            for (i = 0; i < n; i++) {
                if (g->rank[i] == r) {
                    members++;
                }
            }
            while (emitted < members) {
                int best = -1;
                for (i = 0; i < n; i++) {
                    if (placed[i] != 0u || g->rank[i] != r || indeg[i] != 0u) {
                        continue;
                    }
                    if (best < 0 || g->priority[i] < g->priority[(uint32_t)best] ||
                        (g->priority[i] == g->priority[(uint32_t)best] &&
                         strcmp(g->ids[i], g->ids[(uint32_t)best]) < 0)) {
                        best = (int)i;
                    }
                }
                if (best < 0) {
                    /* Unreachable: the closure check rejected every cycle. */
                    free(indeg);
                    free(placed);
                    su_effects_free(g);
                    return SU_E_INTERNAL;
                }
                placed[(uint32_t)best] = 1u;
                g->order[pos++] = (uint32_t)best;
                for (j = 0; j < n; j++) {
                    if (g->adj[(uint32_t)best * n + j] != 0u) {
                        indeg[j]--;
                    }
                }
                emitted++;
            }
        }
        free(indeg);
        free(placed);
    }

    *out = g;
    return SU_OK;
}

void su_effects_free(SuEffectGraph *g) {
    if (g == NULL) {
        return;
    }
    free(g->ids);
    free(g->rank);
    free(g->priority);
    free(g->parallel);
    free(g->nf);
    free(g->fid);
    free(g->rd);
    free(g->wr);
    free(g->adj);
    free(g->reach);
    free(g->conf);
    free(g->tab);
    free(g->order);
    free(g);
}

uint32_t su_effects_count(const SuEffectGraph *g) {
    return g != NULL ? g->n : 0u;
}

uint32_t su_effects_serial_order(const SuEffectGraph *g, uint32_t *out, uint32_t cap) {
    if (g == NULL || out == NULL || cap < g->n) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (g->n > 0u) {
        memcpy(out, g->order, (size_t)g->n * sizeof *out);
    }
    return SU_OK;
}

int su_effects_ordered(const SuEffectGraph *g, uint32_t a, uint32_t b) {
    if (g == NULL || a >= g->n || b >= g->n) {
        return 0;
    }
    return g->reach[a * g->n + b] != 0u || g->reach[b * g->n + a] != 0u;
}

int su_effects_conflict(const SuEffectGraph *g, uint32_t a, uint32_t b) {
    if (g == NULL || a >= g->n || b >= g->n) {
        return 0;
    }
    return g->conf[a * g->n + b] != 0u;
}
