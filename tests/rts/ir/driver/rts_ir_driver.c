/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * M3-03 test driver (test code only, not a product entry point). It runs the engine's own
 * verifier (su_lir_verify, src/ir/su_lir.c) and the checked reference VM (su_vm_invoke_v1,
 * src/vm/su_vm.c) on rows supplied by the Python differential in tests/rts/ir.
 *
 *   rts_ir_driver verify <capsule.json> <lir.json>
 *       prints one line: "VERIFY OK insn=N regs=N fields=N ..." or "VERIFY ERR <code> <name> ..."
 *   rts_ir_driver run <capsule.json> <lir.json>  < calls
 *       reads calls from stdin, one block per invocation:
 *           CALL <rows>
 *           IN <field_id> <v0> ... <v(rows-1)>      one line per READABLE field of the module
 *           END
 *       and writes, per call, to stdout:
 *           RES <status> <failed_insn>
 *           OUT <field_id> <v0> ... <v(rows-1)>     one line per WRITABLE field, ascending id (SU_OK only)
 *           DESPAWN [<slot> ...]                    slots emitted by despawn_self, in row order
 *           END
 *       Every row is alive (the Python side compacts the rows it wants to run), generation 1.
 *   Exit status: 0 when the driver ran to completion (verification refusals are reported in the
 *   output, not as an exit status), 2 on malformed input or an I/O error.
 */
#define _POSIX_C_SOURCE 200809L

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_lir.h"
#include "sutekh/abi.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "vm/su_vm.h"

#define DRIVER_LAYOUT_GENERATION UINT32_C(1)
#define DRIVER_MAX_FIELDS 256u

typedef struct U32Vec {
    uint32_t *items;
    size_t n;
    size_t cap;
} U32Vec;

static int u32vec_push(U32Vec *v, uint32_t x) {
    if (v->n == v->cap) {
        size_t nc = v->cap == 0 ? 64u : v->cap * 2u;
        uint32_t *grown = realloc(v->items, nc * sizeof *grown);
        if (grown == NULL) {
            return 1;
        }
        v->items = grown;
        v->cap = nc;
    }
    v->items[v->n++] = x;
    return 0;
}

static char *read_file(const char *path, size_t *len) {
    FILE *fh = fopen(path, "rb");
    long size;
    char *buf;
    if (fh == NULL) {
        return NULL;
    }
    if (fseek(fh, 0, SEEK_END) != 0 || (size = ftell(fh)) < 0 || fseek(fh, 0, SEEK_SET) != 0) {
        fclose(fh);
        return NULL;
    }
    buf = malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(fh);
        return NULL;
    }
    if (fread(buf, 1u, (size_t)size, fh) != (size_t)size) {
        free(buf);
        fclose(fh);
        return NULL;
    }
    buf[size] = '\0';
    fclose(fh);
    *len = (size_t)size;
    return buf;
}

static uint32_t verify_pair(const char *cap_path, const char *lir_path, SuLirModule *mod, SuDiag *diag,
                            char **cap_out, size_t *cap_len_out, char **lir_out, size_t *lir_len_out) {
    size_t cap_len = 0;
    size_t lir_len = 0;
    char *cap = read_file(cap_path, &cap_len);
    char *lir = read_file(lir_path, &lir_len);
    uint32_t st;
    if (cap == NULL || lir == NULL) {
        free(cap);
        free(lir);
        return SU_E_INVALID_ARGUMENT;
    }
    su_diag_init(diag);
    memset(mod, 0, sizeof *mod);
    st = su_lir_verify((const uint8_t *)cap, cap_len, (const uint8_t *)lir, lir_len, mod, diag);
    if (cap_out != NULL) {
        *cap_out = cap;
        *cap_len_out = cap_len;
        *lir_out = lir;
        *lir_len_out = lir_len;
    } else {
        free(cap);
        free(lir);
    }
    return st;
}

static int cmd_verify(const char *cap_path, const char *lir_path) {
    SuLirModule mod;
    SuDiag diag;
    uint32_t st = verify_pair(cap_path, lir_path, &mod, &diag, NULL, NULL, NULL, NULL);
    if (st != SU_OK) {
        printf("VERIFY ERR %u %s pointer=%s message=%s\n", (unsigned)st, su_status_name(st),
               diag.json_pointer, diag.message);
        return 0;
    }
    printf("VERIFY OK insn=%u regs=%u fields=%u ir_minor=%u format=%u max_struct=%u max_events=%u "
           "uses_entity_ops=%u semantic=%s capsule=%s source=%s\n",
           (unsigned)mod.insn_count, (unsigned)mod.register_count, (unsigned)mod.field_count,
           (unsigned)mod.ir_version_minor, (unsigned)mod.artifact_format,
           (unsigned)mod.max_struct_per_row, (unsigned)mod.max_events_per_row,
           (unsigned)mod.uses_entity_ops, mod.semantic_hash_hex, mod.capsule_hash_hex, mod.source_hash_hex);
    su_lir_module_free(&mod);
    return 0;
}

typedef struct InputSet {
    uint32_t id;
    uint32_t *vals;
    size_t n;
    int present;
} InputSet;

static int emit_despawn(void *user, uint32_t slot, uint32_t seq, uint64_t expected_handle) {
    (void)seq;
    (void)expected_handle;
    return u32vec_push((U32Vec *)user, slot) != 0 ? 1 : 0;
}

static int emit_event(void *user, uint32_t slot, uint32_t seq, uint32_t kind, uint32_t a, uint32_t b) {
    (void)user;
    (void)slot;
    (void)seq;
    (void)kind;
    (void)a;
    (void)b;
    /* RTS modules emit no events; a call that does is refused by the wrapper (buffer overflow path). */
    return 1;
}

static int parse_u32(const char *s, uint32_t *out) {
    char *end = NULL;
    unsigned long long v;
    if (*s < '0' || *s > '9') {
        return 0;
    }
    v = strtoull(s, &end, 10);
    if (end == s || v > 0xFFFFFFFFull) {
        return 0;
    }
    *out = (uint32_t)v;
    return 1;
}

static int parse_size(const char *s, size_t *out) {
    char *end = NULL;
    unsigned long long v;
    if (*s < '0' || *s > '9') {
        return 0;
    }
    v = strtoull(s, &end, 10);
    if (end == s || v > 1000000ull) {
        return 0;
    }
    *out = (size_t)v;
    return 1;
}

/* Splits one line in place into tokens; returns the token count (at most max). */
static size_t tokenize(char *line, char **tok, size_t max) {
    size_t n = 0;
    char *p = line;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (n == max) {
            return max + 1u;
        }
        tok[n++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }
    return n;
}

static void free_inputs(InputSet *in, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        free(in[i].vals);
        in[i].vals = NULL;
        in[i].present = 0;
    }
}

static int run_call(SuExecContext *ctx, const SuLirModule *mod, size_t rows, InputSet *in, size_t nin) {
    SuColumnBinding *bin = NULL;
    SuColumnBinding *bout = NULL;
    uint32_t **stage = NULL;
    size_t nread = 0;
    size_t nwrite = 0;
    size_t i;
    size_t k = 0;
    size_t w = 0;
    uint32_t st;
    SuInvocation inv;
    SuOutputs outs;
    SuVmEmitter em;
    U32Vec despawns = {NULL, 0, 0};
    int rc = 0;

    for (i = 0; i < mod->field_count; i++) {
        if (mod->fields[i].access & SU_ACCESS_READ) {
            nread++;
        }
        if (mod->fields[i].access & SU_ACCESS_WRITE) {
            nwrite++;
        }
    }
    bin = calloc(nread + 1u, sizeof *bin);
    bout = calloc(nwrite + 1u, sizeof *bout);
    stage = calloc(nwrite + 1u, sizeof *stage);
    if (bin == NULL || bout == NULL || stage == NULL) {
        rc = 2;
        goto done;
    }
    for (i = 0; i < mod->field_count; i++) {
        uint32_t fid = mod->fields[i].id;
        if (mod->fields[i].access & SU_ACCESS_READ) {
            size_t j;
            InputSet *found = NULL;
            for (j = 0; j < nin; j++) {
                if (in[j].present && in[j].id == fid) {
                    found = &in[j];
                }
            }
            if (found == NULL || found->n != rows) {
                /* Missing or mis-sized input: reported as an invalid argument, nothing executed. */
                printf("RES %u %u\nDESPAWN\n", (unsigned)SU_E_INVALID_ARGUMENT, (unsigned)UINT32_MAX);
                goto done;
            }
            memset(&bin[k], 0, sizeof bin[k]);
            bin[k].struct_size = sizeof bin[k];
            bin[k].field_id = fid;
            bin[k].elem_type = SU_TYPE_U32;
            bin[k].access = SU_ACCESS_READ;
            bin[k].layout_generation = DRIVER_LAYOUT_GENERATION;
            bin[k].stride_bytes = 4u;
            bin[k].row_count = rows;
            bin[k].data = found->vals;
            k++;
        }
        if (mod->fields[i].access & SU_ACCESS_WRITE) {
            stage[w] = calloc(rows + 1u, sizeof **stage);
            if (stage[w] == NULL) {
                rc = 2;
                goto done;
            }
            memset(&bout[w], 0, sizeof bout[w]);
            bout[w].struct_size = sizeof bout[w];
            bout[w].field_id = fid;
            bout[w].elem_type = SU_TYPE_U32;
            bout[w].access = SU_ACCESS_WRITE;
            bout[w].layout_generation = DRIVER_LAYOUT_GENERATION;
            bout[w].stride_bytes = 4u;
            bout[w].row_count = rows;
            bout[w].data = stage[w];
            w++;
        }
    }

    memset(&em, 0, sizeof em);
    em.user = &despawns;
    em.emit_despawn = emit_despawn;
    em.emit_event = emit_event;
    st = su_vm_context_set_entity_view(ctx, NULL, NULL, 0u, 1u, &em);
    if (st != SU_OK) {
        printf("RES %u %u\nDESPAWN\n", (unsigned)st, (unsigned)UINT32_MAX);
        goto done;
    }

    memset(&inv, 0, sizeof inv);
    inv.struct_size = sizeof inv;
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = DRIVER_LAYOUT_GENERATION;
    inv.binding_count = (uint32_t)nread;
    inv.row_count = rows;
    inv.bindings = bin;

    memset(&outs, 0, sizeof outs);
    outs.struct_size = sizeof outs;
    outs.binding_count = (uint32_t)nwrite;
    outs.failed_insn = UINT32_MAX;
    outs.reserved = 0;
    outs.bindings = bout;

    st = su_vm_invoke_v1(ctx, &inv, &outs);
    printf("RES %u %u\n", (unsigned)st, (unsigned)outs.failed_insn);
    if (st == SU_OK) {
        for (w = 0; w < nwrite; w++) {
            size_t r;
            printf("OUT %u", (unsigned)bout[w].field_id);
            for (r = 0; r < rows; r++) {
                printf(" %u", (unsigned)stage[w][r]);
            }
            printf("\n");
        }
    }
    printf("DESPAWN");
    for (i = 0; i < despawns.n; i++) {
        printf(" %u", (unsigned)despawns.items[i]);
    }
    printf("\n");

done:
    for (w = 0; stage != NULL && w < nwrite; w++) {
        free(stage[w]);
    }
    free(stage);
    free(bin);
    free(bout);
    free(despawns.items);
    return rc;
}

static int cmd_run(const char *cap_path, const char *lir_path) {
    SuLirModule mod;
    SuDiag diag;
    char *cap = NULL;
    char *lir = NULL;
    size_t cap_len = 0;
    size_t lir_len = 0;
    SuExecContext *ctx = NULL;
    uint32_t st = verify_pair(cap_path, lir_path, &mod, &diag, &cap, &cap_len, &lir, &lir_len);
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t got;
    int rc = 0;
    size_t rows = 0;
    int in_call = 0;
    InputSet in[DRIVER_MAX_FIELDS];
    size_t nin = 0;
    char **tok = NULL;
    size_t tok_cap = 0;

    if (st != SU_OK) {
        printf("RUN ERR verify %u %s pointer=%s message=%s\n", (unsigned)st, su_status_name(st),
               diag.json_pointer, diag.message);
        free(cap);
        free(lir);
        return 0;
    }
    st = su_vm_context_create(&mod, DRIVER_LAYOUT_GENERATION, &ctx);
    if (st != SU_OK) {
        printf("RUN ERR context %u %s\n", (unsigned)st, su_status_name(st));
        su_lir_module_free(&mod);
        free(cap);
        free(lir);
        return 0;
    }
    printf("RUN READY insn=%u\n", (unsigned)mod.insn_count);

    while ((got = getline(&line, &line_cap, stdin)) != -1) {
        size_t need = strlen(line) / 2u + 2u;
        size_t nt;
        if (need > tok_cap) {
            char **grown = realloc(tok, need * sizeof *grown);
            if (grown == NULL) {
                rc = 2;
                break;
            }
            tok = grown;
            tok_cap = need;
        }
        nt = tokenize(line, tok, tok_cap);
        if (nt == 0) {
            continue;
        }
        if (nt > tok_cap) {
            rc = 2;
            break;
        }
        if (strcmp(tok[0], "CALL") == 0) {
            if (in_call || nt != 2 || !parse_size(tok[1], &rows)) {
                rc = 2;
                break;
            }
            in_call = 1;
            nin = 0;
            memset(in, 0, sizeof in);
        } else if (strcmp(tok[0], "IN") == 0) {
            uint32_t fid;
            size_t j;
            if (!in_call || nt < 2 || !parse_u32(tok[1], &fid) || nin == DRIVER_MAX_FIELDS) {
                rc = 2;
                break;
            }
            in[nin].id = fid;
            in[nin].present = 1;
            in[nin].n = nt - 2u;
            in[nin].vals = calloc(in[nin].n + 1u, sizeof(uint32_t));
            nin++;
            if (in[nin - 1u].vals == NULL) {
                rc = 2;
                break;
            }
            for (j = 2; j < nt; j++) {
                if (!parse_u32(tok[j], &in[nin - 1u].vals[j - 2u])) {
                    rc = 2;
                    break;
                }
            }
            if (rc != 0) {
                break;
            }
        } else if (strcmp(tok[0], "END") == 0) {
            if (!in_call) {
                rc = 2;
                break;
            }
            rc = run_call(ctx, &mod, rows, in, nin);
            free_inputs(in, nin);
            nin = 0;
            in_call = 0;
            if (rc != 0) {
                break;
            }
            printf("END\n");
            fflush(stdout);
        } else {
            rc = 2;
            break;
        }
    }
    if (rc != 0 || in_call) {
        fprintf(stderr, "rts_ir_driver: malformed input\n");
        rc = 2;
    }
    free_inputs(in, nin);
    free(line);
    free(tok);
    su_vm_context_destroy(ctx);
    su_lir_module_free(&mod);
    free(cap);
    free(lir);
    return rc;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "verify") == 0) {
        return cmd_verify(argv[2], argv[3]);
    }
    if (argc == 4 && strcmp(argv[1], "run") == 0) {
        return cmd_run(argv[2], argv[3]);
    }
    fprintf(stderr, "usage: rts_ir_driver verify|run <capsule.json> <lir.json>\n");
    return 2;
}
