/* Sutekh M0 fixed-row SoA u32 world. See su_world.h for the protocol. */
#include "world/su_world.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SU_WORLD_PHASE_IDLE UINT32_C(0)
#define SU_WORLD_PHASE_TICK UINT32_C(1)
#define SU_WORLD_ACCESS_MASK (SU_ACCESS_READ | SU_ACCESS_WRITE)
#define SU_WORLD_ELEM_BYTES UINT32_C(4)
#define SU_WORLD_STATE_VERSION UINT32_C(1)
#define SU_WORLD_MAGIC "SUWSTATE"
#define SU_WORLD_MAGIC_BYTES 9u /* 8 ASCII bytes + 0x00 */

typedef struct SuWorldField {
    uint32_t id;
    uint32_t type;
    uint32_t access;
    char *name;
} SuWorldField;

struct SuWorld {
    char *schema_id;
    uint32_t schema_version;
    uint32_t layout_generation;
    uint64_t row_count;
    uint64_t tick;
    uint32_t phase;
    uint32_t field_count;
    uint32_t readable_count;
    uint32_t writable_count;
    uint32_t *entity_ids;
    SuWorldField *fields;     /* verified table: sorted ascending by id */
    uint32_t **committed;     /* per table index */
    uint32_t **working;       /* per table index */
    uint32_t **staging;       /* per table index; NULL for read-only fields */
    SuColumnBinding *inv_bindings; /* readable fields, ascending id */
    SuColumnBinding *out_bindings; /* writable fields, ascending id */
};

/* ---- helpers ---------------------------------------------------------- */

/* Checked a*b into *out (size_t). */
static uint32_t su_mul_size(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) {
        return SU_E_RESOURCE_LIMIT;
    }
    *out = a * b;
    return SU_OK;
}

/* Checked a+b into *out (size_t). */
static uint32_t su_add_size(size_t a, size_t b, size_t *out) {
    if (a > SIZE_MAX - b) {
        return SU_E_RESOURCE_LIMIT;
    }
    *out = a + b;
    return SU_OK;
}

/* Validates a bounded NUL-terminated string. Reads at most SU_WORLD_NAME_MAX+1
 * bytes. */
static uint32_t su_check_name(const char *s, size_t *len_out) {
    size_t n = 0;
    if (s == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    while (n <= SU_WORLD_NAME_MAX && s[n] != '\0') {
        n++;
    }
    if (n == 0) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (n > SU_WORLD_NAME_MAX) {
        return SU_E_RESOURCE_LIMIT;
    }
    *len_out = n;
    return SU_OK;
}

static char *su_copy_name(const char *s, size_t len) {
    char *p = malloc(len + 1u);
    if (p != NULL) {
        memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}

/* Allocates one zeroed column of rows u32 values. At least one element is
 * allocated so every column pointer is non-NULL, including for zero rows. */
static uint32_t su_alloc_column(uint64_t rows, uint32_t **out) {
    size_t n = (size_t)rows;
    size_t bytes = 0;
    uint32_t st;
    if (n == 0) {
        n = 1;
    }
    st = su_mul_size(n, SU_WORLD_ELEM_BYTES, &bytes);
    if (st != SU_OK) {
        return st;
    }
    *out = calloc(1, bytes);
    return *out != NULL ? SU_OK : SU_E_RESOURCE_LIMIT;
}

static int su_cmp_u32(const void *pa, const void *pb) {
    uint32_t a = *(const uint32_t *)pa;
    uint32_t b = *(const uint32_t *)pb;
    return (a > b) - (a < b);
}

static int su_cmp_field(const void *pa, const void *pb) {
    const SuWorldField *a = pa;
    const SuWorldField *b = pb;
    return su_cmp_u32(&a->id, &b->id);
}

static void su_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void su_put_u64(uint8_t *p, uint64_t v) {
    su_put_u32(p, (uint32_t)(v & 0xFFFFFFFFu));
    su_put_u32(p + 4, (uint32_t)(v >> 32));
}

/* Index of a field by ID in the verified table, or -1. */
static long su_find_id(const SuWorld *w, uint32_t field_id) {
    uint32_t lo = 0;
    uint32_t hi = w->field_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (w->fields[mid].id == field_id) {
            return (long)mid;
        }
        if (w->fields[mid].id < field_id) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return -1;
}

/* ---- lifetime --------------------------------------------------------- */

void su_world_destroy(SuWorld *w) {
    uint32_t i;
    if (w == NULL) {
        return;
    }
    for (i = 0; i < w->field_count; i++) {
        if (w->committed != NULL) {
            free(w->committed[i]);
        }
        if (w->working != NULL) {
            free(w->working[i]);
        }
        if (w->staging != NULL) {
            free(w->staging[i]);
        }
        if (w->fields != NULL) {
            free(w->fields[i].name);
        }
    }
    free(w->committed);
    free(w->working);
    free(w->staging);
    free(w->fields);
    free(w->entity_ids);
    free(w->schema_id);
    free(w->inv_bindings);
    free(w->out_bindings);
    free(w);
}

uint32_t su_world_create(const SuWorldDesc *desc, SuWorld **out) {
    SuWorld *w = NULL;
    size_t id_len = 0;
    size_t n_rows = 0;
    size_t entity_bytes = 0;
    uint32_t i;
    uint32_t j;
    uint32_t st;
    uint32_t readable = 0;
    uint32_t writable = 0;
    uint32_t *sorted_ids = NULL;

    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (desc == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (desc->row_count > SU_LIMIT_ROWS) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (desc->field_count > SU_LIMIT_FIELDS) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (desc->field_count > 0 && desc->fields == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (desc->row_count > 0 && desc->entity_ids == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = su_check_name(desc->schema_id, &id_len);
    if (st != SU_OK) {
        return st;
    }

    /* Field validation before any allocation. */
    for (i = 0; i < desc->field_count; i++) {
        const SuWorldFieldDesc *f = &desc->fields[i];
        size_t name_len = 0;
        if (f->type != SU_TYPE_U32) {
            return SU_E_TYPE;
        }
        if (f->access == 0u || (f->access & ~SU_WORLD_ACCESS_MASK) != 0u) {
            return SU_E_INVALID_ARGUMENT;
        }
        st = su_check_name(f->name, &name_len);
        if (st != SU_OK) {
            return st;
        }
        for (j = 0; j < i; j++) {
            if (desc->fields[j].id == f->id || strcmp(desc->fields[j].name, f->name) == 0) {
                return SU_E_SCHEMA_MISMATCH;
            }
        }
        if ((f->access & SU_ACCESS_READ) != 0u) {
            readable++;
        }
        if ((f->access & SU_ACCESS_WRITE) != 0u) {
            writable++;
        }
    }

    /* Entity IDs: nonzero and unique. */
    n_rows = (size_t)desc->row_count;
    st = su_mul_size(n_rows, SU_WORLD_ELEM_BYTES, &entity_bytes);
    if (st != SU_OK) {
        return st;
    }
    if (n_rows > 0) {
        for (size_t r = 0; r < n_rows; r++) {
            if (desc->entity_ids[r] == 0u) {
                return SU_E_INVALID_ARGUMENT;
            }
        }
        sorted_ids = malloc(entity_bytes);
        if (sorted_ids == NULL) {
            return SU_E_RESOURCE_LIMIT;
        }
        memcpy(sorted_ids, desc->entity_ids, entity_bytes);
        qsort(sorted_ids, n_rows, SU_WORLD_ELEM_BYTES, su_cmp_u32);
        for (size_t r = 1; r < n_rows; r++) {
            if (sorted_ids[r] == sorted_ids[r - 1u]) {
                free(sorted_ids);
                return SU_E_INVALID_ARGUMENT;
            }
        }
        free(sorted_ids);
    }

    w = calloc(1, sizeof *w);
    if (w == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    w->schema_version = desc->schema_version;
    w->layout_generation = desc->layout_generation;
    w->row_count = desc->row_count;
    w->readable_count = readable;
    w->writable_count = writable;

    w->schema_id = su_copy_name(desc->schema_id, id_len);
    if (w->schema_id == NULL) {
        goto fail_alloc;
    }

    w->entity_ids = malloc(entity_bytes > 0 ? entity_bytes : SU_WORLD_ELEM_BYTES);
    if (w->entity_ids == NULL) {
        goto fail_alloc;
    }
    if (n_rows > 0) {
        memcpy(w->entity_ids, desc->entity_ids, entity_bytes);
    } else {
        w->entity_ids[0] = 0u;
    }

    /* Fields: copy, then sort into the verified table. */
    w->fields = calloc(desc->field_count > 0 ? desc->field_count : 1u, sizeof *w->fields);
    if (w->fields == NULL) {
        goto fail_alloc;
    }
    w->field_count = desc->field_count;
    for (i = 0; i < desc->field_count; i++) {
        size_t name_len = strlen(desc->fields[i].name);
        w->fields[i].id = desc->fields[i].id;
        w->fields[i].type = desc->fields[i].type;
        w->fields[i].access = desc->fields[i].access;
        w->fields[i].name = su_copy_name(desc->fields[i].name, name_len);
        if (w->fields[i].name == NULL) {
            goto fail_alloc;
        }
    }
    if (desc->field_count > 1u) {
        qsort(w->fields, desc->field_count, sizeof *w->fields, su_cmp_field);
    }

    w->committed = calloc(desc->field_count > 0 ? desc->field_count : 1u, sizeof *w->committed);
    w->working = calloc(desc->field_count > 0 ? desc->field_count : 1u, sizeof *w->working);
    w->staging = calloc(desc->field_count > 0 ? desc->field_count : 1u, sizeof *w->staging);
    if (w->committed == NULL || w->working == NULL || w->staging == NULL) {
        goto fail_alloc;
    }
    for (i = 0; i < w->field_count; i++) {
        st = su_alloc_column(w->row_count, &w->committed[i]);
        if (st != SU_OK) {
            goto fail_status;
        }
        st = su_alloc_column(w->row_count, &w->working[i]);
        if (st != SU_OK) {
            goto fail_status;
        }
        if ((w->fields[i].access & SU_ACCESS_WRITE) != 0u) {
            st = su_alloc_column(w->row_count, &w->staging[i]);
            if (st != SU_OK) {
                goto fail_status;
            }
        }
    }

    w->inv_bindings = calloc(readable > 0 ? readable : 1u, sizeof *w->inv_bindings);
    w->out_bindings = calloc(writable > 0 ? writable : 1u, sizeof *w->out_bindings);
    if (w->inv_bindings == NULL || w->out_bindings == NULL) {
        goto fail_alloc;
    }

    *out = w;
    return SU_OK;

fail_alloc:
    su_world_destroy(w);
    return SU_E_RESOURCE_LIMIT;

fail_status:
    su_world_destroy(w);
    return st;
}

/* ---- accessors -------------------------------------------------------- */

uint64_t su_world_row_count(const SuWorld *w) {
    return w != NULL ? w->row_count : 0u;
}

uint32_t su_world_field_count(const SuWorld *w) {
    return w != NULL ? w->field_count : 0u;
}

uint32_t su_world_layout_generation(const SuWorld *w) {
    return w != NULL ? w->layout_generation : 0u;
}

uint64_t su_world_tick(const SuWorld *w) {
    return w != NULL ? w->tick : 0u;
}

int su_world_in_tick(const SuWorld *w) {
    return w != NULL && w->phase == SU_WORLD_PHASE_TICK;
}

/* ---- tick control ----------------------------------------------------- */

uint32_t su_world_begin_tick(SuWorld *w) {
    uint32_t i;
    size_t bytes = 0;
    if (w == NULL || w->phase != SU_WORLD_PHASE_IDLE) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (su_mul_size((size_t)w->row_count, SU_WORLD_ELEM_BYTES, &bytes) != SU_OK) {
        return SU_E_INTERNAL;
    }
    for (i = 0; i < w->field_count; i++) {
        if (bytes > 0) {
            memcpy(w->working[i], w->committed[i], bytes);
        }
    }
    w->phase = SU_WORLD_PHASE_TICK;
    return SU_OK;
}

uint32_t su_world_commit_staging(SuWorld *w, const uint32_t *field_ids, uint32_t count) {
    uint32_t k;
    uint32_t m;
    size_t bytes = 0;
    long idx;
    if (w == NULL || w->phase != SU_WORLD_PHASE_TICK) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (count == 0u) {
        return SU_OK;
    }
    if (field_ids == NULL || count > w->field_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    /* Validate the whole list before copying anything. */
    for (k = 0; k < count; k++) {
        idx = su_find_id(w, field_ids[k]);
        if (idx < 0) {
            return SU_E_SCHEMA_MISMATCH;
        }
        if ((w->fields[idx].access & SU_ACCESS_WRITE) == 0u) {
            return SU_E_UNDECLARED_WRITE;
        }
        for (m = 0; m < k; m++) {
            if (field_ids[m] == field_ids[k]) {
                return SU_E_INVALID_ARGUMENT;
            }
        }
    }
    if (su_mul_size((size_t)w->row_count, SU_WORLD_ELEM_BYTES, &bytes) != SU_OK) {
        return SU_E_INTERNAL;
    }
    for (k = 0; k < count; k++) {
        idx = su_find_id(w, field_ids[k]);
        if (bytes > 0) {
            memcpy(w->working[idx], w->staging[idx], bytes);
        }
    }
    return SU_OK;
}

uint32_t su_world_publish(SuWorld *w) {
    uint32_t i;
    size_t bytes = 0;
    if (w == NULL || w->phase != SU_WORLD_PHASE_TICK) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (su_mul_size((size_t)w->row_count, SU_WORLD_ELEM_BYTES, &bytes) != SU_OK) {
        return SU_E_INTERNAL;
    }
    for (i = 0; i < w->field_count; i++) {
        if (bytes > 0) {
            memcpy(w->committed[i], w->working[i], bytes);
        }
    }
    w->tick++;
    w->phase = SU_WORLD_PHASE_IDLE;
    return SU_OK;
}

uint32_t su_world_abort(SuWorld *w) {
    if (w == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    /* Committed is never written during a tick, so discarding the open tick
     * leaves it bit-identical. Working and staging are rebuilt by the next
     * begin_tick. */
    w->phase = SU_WORLD_PHASE_IDLE;
    return SU_OK;
}

/* ---- views ------------------------------------------------------------ */

uint32_t su_world_build_invocation(SuWorld *w, SuInvocation *out) {
    uint32_t i;
    uint32_t k = 0;
    if (w == NULL || out == NULL || w->phase != SU_WORLD_PHASE_TICK) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < w->field_count; i++) {
        SuColumnBinding *b;
        if ((w->fields[i].access & SU_ACCESS_READ) == 0u) {
            continue;
        }
        b = &w->inv_bindings[k++];
        b->struct_size = (uint32_t)sizeof(SuColumnBinding);
        b->field_id = w->fields[i].id;
        b->elem_type = SU_TYPE_U32;
        b->access = SU_ACCESS_READ;
        b->layout_generation = w->layout_generation;
        b->stride_bytes = SU_WORLD_ELEM_BYTES;
        b->row_count = w->row_count;
        b->data = w->working[i];
    }
    out->struct_size = (uint32_t)sizeof(SuInvocation);
    out->abi_version = SU_ABI_VERSION;
    out->layout_generation = w->layout_generation;
    out->binding_count = k;
    out->row_count = w->row_count;
    out->bindings = w->inv_bindings;
    return SU_OK;
}

uint32_t su_world_build_outputs(SuWorld *w, SuOutputs *out) {
    uint32_t i;
    uint32_t k = 0;
    if (w == NULL || out == NULL || w->phase != SU_WORLD_PHASE_TICK) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < w->field_count; i++) {
        SuColumnBinding *b;
        if ((w->fields[i].access & SU_ACCESS_WRITE) == 0u) {
            continue;
        }
        b = &w->out_bindings[k++];
        b->struct_size = (uint32_t)sizeof(SuColumnBinding);
        b->field_id = w->fields[i].id;
        b->elem_type = SU_TYPE_U32;
        b->access = SU_ACCESS_WRITE;
        b->layout_generation = w->layout_generation;
        b->stride_bytes = SU_WORLD_ELEM_BYTES;
        b->row_count = w->row_count;
        b->data = w->staging[i];
    }
    out->struct_size = (uint32_t)sizeof(SuOutputs);
    out->binding_count = k;
    out->failed_insn = UINT32_MAX;
    out->reserved = 0u;
    out->bindings = w->out_bindings;
    return SU_OK;
}

/* ---- fixture init and inspection -------------------------------------- */

uint32_t su_world_init_committed(SuWorld *w, uint32_t field_id, const uint32_t *values, uint64_t count) {
    long idx;
    size_t bytes = 0;
    if (w == NULL || (values == NULL && count > 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (w->phase != SU_WORLD_PHASE_IDLE || w->tick != 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (count != w->row_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    idx = su_find_id(w, field_id);
    if (idx < 0) {
        return SU_E_SCHEMA_MISMATCH;
    }
    if (su_mul_size((size_t)count, SU_WORLD_ELEM_BYTES, &bytes) != SU_OK) {
        return SU_E_INTERNAL;
    }
    if (bytes > 0) {
        memcpy(w->committed[idx], values, bytes);
    }
    return SU_OK;
}

uint32_t su_world_read_committed(const SuWorld *w, uint32_t field_id, const uint32_t **data, uint64_t *row_count) {
    long idx;
    if (w == NULL || data == NULL || row_count == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    idx = su_find_id(w, field_id);
    if (idx < 0) {
        return SU_E_SCHEMA_MISMATCH;
    }
    *data = w->committed[idx];
    *row_count = w->row_count;
    return SU_OK;
}

uint32_t su_world_read_committed_by_name(const SuWorld *w, const char *name, const uint32_t **data, uint64_t *row_count) {
    uint32_t i;
    if (w == NULL || name == NULL || data == NULL || row_count == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < w->field_count; i++) {
        if (strcmp(w->fields[i].name, name) == 0) {
            *data = w->committed[i];
            *row_count = w->row_count;
            return SU_OK;
        }
    }
    return SU_E_SCHEMA_MISMATCH;
}

/* ---- canonical state -------------------------------------------------- */

static uint32_t su_canonical_size_internal(const SuWorld *w, size_t *size) {
    size_t rows = (size_t)w->row_count;
    size_t total = SU_WORLD_MAGIC_BYTES + 4u + 8u + 8u + 4u;
    size_t entity_bytes = 0;
    uint32_t st;
    uint32_t i;

    st = su_mul_size(rows, SU_WORLD_ELEM_BYTES, &entity_bytes);
    if (st != SU_OK) {
        return st;
    }
    st = su_add_size(total, entity_bytes, &total);
    if (st != SU_OK) {
        return st;
    }
    /* Per field: u32 id + u32 elem_type + values. */
    for (i = 0; i < w->field_count; i++) {
        size_t per_field = 0;
        st = su_mul_size(rows, SU_WORLD_ELEM_BYTES, &per_field);
        if (st != SU_OK) {
            return st;
        }
        st = su_add_size(per_field, 8u, &per_field);
        if (st != SU_OK) {
            return st;
        }
        st = su_add_size(total, per_field, &total);
        if (st != SU_OK) {
            return st;
        }
    }
    *size = total;
    return SU_OK;
}

uint32_t su_world_canonical_size(const SuWorld *w, size_t *size) {
    if (w == NULL || size == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    return su_canonical_size_internal(w, size);
}

uint32_t su_world_canonical_write(const SuWorld *w, uint8_t *buf, size_t cap, size_t *written) {
    size_t need = 0;
    size_t off = 0;
    size_t rows;
    uint32_t st;
    uint32_t i;
    uint64_t r;

    if (w == NULL || written == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = su_canonical_size_internal(w, &need);
    if (st != SU_OK) {
        return st;
    }
    *written = need;
    if (buf == NULL || cap < need) {
        return SU_E_INVALID_ARGUMENT;
    }
    rows = (size_t)w->row_count;

    memcpy(buf, SU_WORLD_MAGIC, SU_WORLD_MAGIC_BYTES);
    off += SU_WORLD_MAGIC_BYTES;
    su_put_u32(buf + off, SU_WORLD_STATE_VERSION);
    off += 4u;
    su_put_u64(buf + off, w->tick);
    off += 8u;
    su_put_u64(buf + off, w->row_count);
    off += 8u;
    su_put_u32(buf + off, w->field_count);
    off += 4u;
    for (r = 0; r < (uint64_t)rows; r++) {
        su_put_u32(buf + off, w->entity_ids[r]);
        off += 4u;
    }
    for (i = 0; i < w->field_count; i++) {
        su_put_u32(buf + off, w->fields[i].id);
        off += 4u;
        su_put_u32(buf + off, w->fields[i].type);
        off += 4u;
        for (r = 0; r < (uint64_t)rows; r++) {
            su_put_u32(buf + off, w->committed[i][r]);
            off += 4u;
        }
    }
    return SU_OK;
}

uint32_t su_world_state_equal(const SuWorld *a, const SuWorld *b, int *equal) {
    size_t na = 0;
    size_t nb = 0;
    uint8_t *ba = NULL;
    uint8_t *bb = NULL;
    size_t wa = 0;
    size_t wb = 0;
    uint32_t st;

    if (a == NULL || b == NULL || equal == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *equal = 0;
    st = su_canonical_size_internal(a, &na);
    if (st != SU_OK) {
        return st;
    }
    st = su_canonical_size_internal(b, &nb);
    if (st != SU_OK) {
        return st;
    }
    if (na != nb) {
        return SU_OK;
    }
    ba = malloc(na);
    bb = malloc(nb);
    if (ba == NULL || bb == NULL) {
        free(ba);
        free(bb);
        return SU_E_RESOURCE_LIMIT;
    }
    st = su_world_canonical_write(a, ba, na, &wa);
    if (st == SU_OK) {
        st = su_world_canonical_write(b, bb, nb, &wb);
    }
    if (st == SU_OK) {
        *equal = memcmp(ba, bb, na) == 0;
    }
    free(ba);
    free(bb);
    return st;
}
