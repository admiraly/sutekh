/* Sutekh fixed-capacity SoA u32 world with the generational entity table. See su_world.h. */
#include "world/su_world.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_sha256.h"

#define SU_WORLD_PHASE_IDLE UINT32_C(0)
#define SU_WORLD_PHASE_TICK UINT32_C(1)
#define SU_WORLD_ACCESS_MASK (SU_ACCESS_READ | SU_ACCESS_WRITE)
#define SU_WORLD_ELEM_BYTES UINT32_C(4)
#define SU_WORLD_STATE_VERSION UINT32_C(2)
#define SU_WORLD_STATE_VERSION_V1 UINT32_C(1)
#define SU_WORLD_MAGIC "SUWSTATE"
#define SU_WORLD_MAGIC_BYTES 9u /* 8 ASCII bytes + 0x00 */
#define SU_WORLD_V2_HEADER_BYTES (SU_WORLD_MAGIC_BYTES + 4u + 8u + 4u)
#define SU_WORLD_ENTS_HEADER_BYTES 60u     /* su_ents canonical header */
#define SU_WORLD_ENTS_HW_OFF 36u           /* u64 high_water inside the su_ents header */
#define SU_WORLD_ENTS_RECORD_BYTES 9u

typedef struct SuWorldField {
    uint32_t id;
    uint32_t type;
    uint32_t access;
    uint32_t def;   /* default written by spawn for fields the request does not name */
    char *name;
} SuWorldField;

struct SuWorld {
    char *schema_id;
    uint32_t schema_version;
    uint32_t layout_generation;
    uint64_t row_count;       /* capacity: column length and entity table capacity */
    uint64_t initial_rows;    /* desc.row_count: slots alive at creation */
    uint64_t tick;
    uint32_t phase;
    uint32_t field_count;
    uint32_t readable_count;
    uint32_t writable_count;
    SuEnts *ents_committed;   /* entity table, committed root */
    SuEnts *ents_working;     /* entity table, working root (copied at begin_tick) */
    SuWorldField *fields;     /* verified table: sorted ascending by id */
    uint32_t **committed;     /* per table index */
    uint32_t **working;       /* per table index */
    uint32_t **staging;       /* per table index; NULL for read-only fields */
    SuColumnBinding *inv_bindings; /* readable fields, ascending id */
    SuColumnBinding *out_bindings; /* writable fields, ascending id */
    uint32_t *retired;             /* retired field IDs, ascending; never shrunk (M2-01a) */
    uint32_t retired_count;
    uint8_t (*lineage)[32];        /* migration hashes, oldest first (M2-01a) */
    uint32_t lineage_count;
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

static uint32_t su_rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t su_rd_u64(const uint8_t *p) {
    return (uint64_t)su_rd_u32(p) | ((uint64_t)su_rd_u32(p + 4) << 32);
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

/* Zeroes slot in every column of root cols. */
static void su_zero_slot(const SuWorld *w, uint32_t **cols, uint32_t slot) {
    uint32_t i;
    for (i = 0; i < w->field_count; i++) {
        cols[i][slot] = 0u;
    }
}

/* Zeroes every column value of every dead slot in cols, using table e. Full chunks are skipped. */
static void su_zero_dead_rows(const SuWorld *w, const SuEnts *e, uint32_t **cols) {
    uint32_t chunks = su_ents_chunk_count(e);
    uint32_t c;
    for (c = 0; c < chunks; c++) {
        uint64_t base = (uint64_t)c * SU_CHUNK_SLOTS;
        uint64_t len = w->row_count - base;
        uint32_t alive = 0;
        const uint64_t *words = NULL;
        uint32_t word_count = 0;
        uint32_t wi;
        if (len > SU_CHUNK_SLOTS) {
            len = SU_CHUNK_SLOTS;
        }
        (void)su_ents_chunk_alive_count(e, c, &alive);
        if ((uint64_t)alive == len) {
            continue;
        }
        (void)su_ents_chunk_alive_bits(e, c, &words, &word_count);
        for (wi = 0; wi < word_count; wi++) {
            uint64_t lo = (uint64_t)wi * 64u;
            uint64_t bits = len - lo;
            uint64_t mask;
            uint64_t dead;
            uint32_t k;
            if (bits > 64u) {
                bits = 64u;
            }
            mask = bits == 64u ? UINT64_C(0xFFFFFFFFFFFFFFFF) : ((UINT64_C(1) << bits) - 1u);
            dead = ~words[wi] & mask;
            for (k = 0; dead != 0u && k < 64u; k++) {
                if ((dead >> k) & 1u) {
                    su_zero_slot(w, cols, (uint32_t)(base + lo + k));
                    dead &= ~(UINT64_C(1) << k);
                }
            }
        }
    }
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
    su_ents_destroy(w->ents_committed);
    su_ents_destroy(w->ents_working);
    free(w->schema_id);
    free(w->inv_bindings);
    free(w->out_bindings);
    free(w->retired);
    free(w->lineage);
    free(w);
}

uint32_t su_world_create(const SuWorldDesc *desc, SuWorld **out) {
    return su_world_create_ex(desc, 0u, out);
}

/* Checks the schema extensions against the descriptor (run before any allocation). */
static uint32_t su_check_ext(const SuWorldDesc *desc, const SuWorldSchemaExt *ext) {
    uint32_t i;
    uint32_t k;
    if (ext == NULL) {
        return SU_OK;
    }
    if (ext->retired_count > SU_WORLD_MAX_HISTORY || ext->lineage_count > SU_WORLD_MAX_HISTORY) {
        return SU_E_RESOURCE_LIMIT;
    }
    if ((ext->retired_count > 0u && ext->retired_ids == NULL) ||
        (ext->lineage_count > 0u && ext->lineage == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    /* ext->defaults may be NULL (all defaults 0); the retired-ID checks below still apply. */
    for (i = 1u; i < ext->retired_count; i++) {
        if (ext->retired_ids[i] <= ext->retired_ids[i - 1u]) {
            return SU_E_SCHEMA_MISMATCH;
        }
    }
    for (i = 0; i < desc->field_count; i++) {
        for (k = 0; k < ext->retired_count; k++) {
            if (ext->retired_ids[k] == desc->fields[i].id) {
                return SU_E_SCHEMA_MISMATCH;
            }
        }
    }
    return SU_OK;
}

uint32_t su_world_create_ex(const SuWorldDesc *desc, uint64_t max_entities, SuWorld **out) {
    return su_world_create_ext(desc, max_entities, NULL, out);
}

uint32_t su_world_create_ext(const SuWorldDesc *desc, uint64_t max_entities, const SuWorldSchemaExt *ext,
                             SuWorld **out) {
    SuWorld *w = NULL;
    size_t id_len = 0;
    size_t n_rows = 0;
    size_t entity_bytes = 0;
    uint64_t capacity;
    uint32_t i;
    uint32_t j;
    uint32_t st;
    uint32_t readable = 0;
    uint32_t writable = 0;
    uint32_t *sorted_ids = NULL;
    SuEntsDesc ed;

    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (desc == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (desc->row_count > SU_LIMIT_ROWS || max_entities > SU_LIMIT_ROWS) {
        return SU_E_RESOURCE_LIMIT;
    }
    capacity = max_entities > desc->row_count ? max_entities : desc->row_count;
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

    st = su_check_ext(desc, ext);
    if (st != SU_OK) {
        return st;
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
    w->row_count = capacity;
    w->initial_rows = desc->row_count;
    w->readable_count = readable;
    w->writable_count = writable;

    w->schema_id = su_copy_name(desc->schema_id, id_len);
    if (w->schema_id == NULL) {
        goto fail_alloc;
    }

    /* Entity table: initial rows alive with generation 1, the rest free. */
    memset(&ed, 0, sizeof ed);
    ed.max_entities = capacity;
    ed.initial_alive = desc->row_count;
    ed.external_ids = desc->row_count > 0u ? desc->entity_ids : NULL;
    st = su_ents_create(&ed, &w->ents_committed);
    if (st != SU_OK) {
        goto fail_status;
    }
    st = su_ents_clone(w->ents_committed, &w->ents_working);
    if (st != SU_OK) {
        goto fail_status;
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
        w->fields[i].def = ext != NULL && ext->defaults != NULL ? ext->defaults[i] : 0u;
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
    if (ext != NULL && ext->retired_count > 0u) {
        w->retired = malloc(ext->retired_count * sizeof *w->retired);
        if (w->retired == NULL) {
            goto fail_alloc;
        }
        memcpy(w->retired, ext->retired_ids, ext->retired_count * sizeof *w->retired);
        w->retired_count = ext->retired_count;
    }
    if (ext != NULL && ext->lineage_count > 0u) {
        w->lineage = malloc(ext->lineage_count * sizeof *w->lineage);
        if (w->lineage == NULL) {
            goto fail_alloc;
        }
        memcpy(w->lineage, ext->lineage, ext->lineage_count * sizeof *w->lineage);
        w->lineage_count = ext->lineage_count;
    }

    /* Initial entity IDs live in the entity table (external ids); columns start zeroed. */
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

const SuEnts *su_world_entities_committed(const SuWorld *w) {
    return w != NULL ? w->ents_committed : NULL;
}

const SuEnts *su_world_entities_working(const SuWorld *w) {
    return w != NULL ? w->ents_working : NULL;
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
    if (su_ents_copy(w->ents_working, w->ents_committed) != SU_OK) {
        return SU_E_INTERNAL;
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

uint32_t su_world_zero_dead(SuWorld *w) {
    if (w == NULL || w->phase != SU_WORLD_PHASE_TICK) {
        return SU_E_INVALID_ARGUMENT;
    }
    su_zero_dead_rows(w, w->ents_working, w->working);
    return SU_OK;
}

uint32_t su_world_publish(SuWorld *w) {
    uint32_t i;
    size_t bytes = 0;
    SuEnts *swap;
    if (w == NULL || w->phase != SU_WORLD_PHASE_TICK) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (su_mul_size((size_t)w->row_count, SU_WORLD_ELEM_BYTES, &bytes) != SU_OK) {
        return SU_E_INTERNAL;
    }
    /* Dead rows are canonically zero in the published root. */
    su_zero_dead_rows(w, w->ents_working, w->working);
    for (i = 0; i < w->field_count; i++) {
        if (bytes > 0) {
            memcpy(w->committed[i], w->working[i], bytes);
        }
    }
    swap = w->ents_committed;
    w->ents_committed = w->ents_working;
    w->ents_working = swap;
    w->tick++;
    w->phase = SU_WORLD_PHASE_IDLE;
    return SU_OK;
}

uint32_t su_world_abort(SuWorld *w) {
    if (w == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    /* Committed columns and the committed table are never written during a tick, so discarding the
     * open tick leaves them bit-identical. Working columns and table are rebuilt by begin_tick. */
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

uint32_t su_world_alive_view(const SuWorld *w, uint32_t base, uint32_t n, uint8_t *alive) {
    const SuEnts *e;
    uint32_t k;
    if (w == NULL || (uint64_t)base + (uint64_t)n > w->row_count || (n > 0u && alive == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    e = w->phase == SU_WORLD_PHASE_TICK ? w->ents_working : w->ents_committed;
    for (k = 0; k < n; k++) {
        alive[k] = su_ents_slot_alive(e, base + k) ? 1u : 0u;
    }
    return SU_OK;
}

uint32_t su_world_generation_view(const SuWorld *w, uint32_t base, uint32_t n, uint32_t *generation) {
    const SuEnts *e;
    uint32_t k;
    if (w == NULL || (uint64_t)base + (uint64_t)n > w->row_count || (n > 0u && generation == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    e = w->phase == SU_WORLD_PHASE_TICK ? w->ents_working : w->ents_committed;
    for (k = 0; k < n; k++) {
        uint32_t st = su_ents_slot_generation(e, base + k, &generation[k]);
        if (st != SU_OK) {
            return st;
        }
    }
    return SU_OK;
}

/* ---- control-plane entity mutations ----------------------------------- */

uint32_t su_world_spawn(SuWorld *w, const SuWorldSpawnValue *values, uint32_t count,
                        uint32_t external_id, SuEntity *out) {
    uint32_t k;
    uint32_t m;
    uint32_t slot;
    SuEntity h;
    uint32_t st;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = SU_ENTITY_INVALID;
    if (w == NULL || w->phase != SU_WORLD_PHASE_IDLE) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (count > 0u && values == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (count > w->field_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (k = 0; k < count; k++) {
        if (su_find_id(w, values[k].field_id) < 0) {
            return SU_E_SCHEMA_MISMATCH;
        }
        for (m = 0; m < k; m++) {
            if (values[m].field_id == values[k].field_id) {
                return SU_E_INVALID_ARGUMENT;
            }
        }
    }
    /* Allocation is the only step that can fail; nothing has changed yet. */
    st = su_ents_alloc(w->ents_committed, external_id, &h);
    if (st != SU_OK) {
        return st;
    }
    slot = SU_ENTITY_SLOT(h);
    su_zero_slot(w, w->committed, slot);
    /* Fields the request does not name take their default (M2-01a; 0 for worlds that never migrated). */
    for (k = 0; k < w->field_count; k++) {
        w->committed[k][slot] = w->fields[k].def;
    }
    for (k = 0; k < count; k++) {
        w->committed[su_find_id(w, values[k].field_id)][slot] = values[k].value;
    }
    *out = h;
    return SU_OK;
}

uint32_t su_world_despawn(SuWorld *w, SuEntity handle) {
    uint32_t st;
    uint32_t slot;
    if (w == NULL || w->phase != SU_WORLD_PHASE_IDLE) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = su_ents_validate(w->ents_committed, handle);
    if (st != SU_OK) {
        return st;
    }
    slot = SU_ENTITY_SLOT(handle);
    su_zero_slot(w, w->committed, slot);
    return su_ents_despawn(w->ents_committed, handle);
}

/* ---- fixture init and inspection -------------------------------------- */

uint32_t su_world_init_committed(SuWorld *w, uint32_t field_id, const uint32_t *values, uint64_t count) {
    long idx;
    size_t bytes = 0;
    uint32_t s;
    if (w == NULL || (values == NULL && count > 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (w->phase != SU_WORLD_PHASE_IDLE || w->tick != 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (count != w->row_count && count != w->initial_rows) {
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
    /* Dead rows stay canonically zero. */
    for (s = 0; s < (uint32_t)w->row_count; s++) {
        if (!su_ents_slot_alive(w->ents_committed, s)) {
            w->committed[idx][s] = 0u;
        }
    }
    return SU_OK;
}

uint32_t su_world_adopt_entities(SuWorld *w, const SuEnts *t) {
    if (w == NULL || t == NULL || w->phase != SU_WORLD_PHASE_IDLE || w->tick != 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (su_ents_capacity(t) != su_ents_capacity(w->ents_committed)) {
        return SU_E_INVALID_ARGUMENT;
    }
    return su_ents_copy(w->ents_committed, t);
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
    size_t hw = su_ents_high_water(w->ents_committed);
    size_t total = SU_WORLD_V2_HEADER_BYTES;
    size_t ents = 0;
    size_t per_field = 0;
    uint32_t st;
    uint32_t i;

    st = su_ents_canonical_size(w->ents_committed, &ents);
    if (st != SU_OK) {
        return st;
    }
    st = su_add_size(total, ents, &total);
    if (st != SU_OK) {
        return st;
    }
    st = su_mul_size(hw, SU_WORLD_ELEM_BYTES, &per_field);
    if (st != SU_OK) {
        return st;
    }
    st = su_add_size(per_field, 8u, &per_field);
    if (st != SU_OK) {
        return st;
    }
    for (i = 0; i < w->field_count; i++) {
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
    size_t ents_len = 0;
    size_t hw;
    uint32_t st;
    uint32_t i;
    uint32_t r;

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
    hw = su_ents_high_water(w->ents_committed);

    memcpy(buf, SU_WORLD_MAGIC, SU_WORLD_MAGIC_BYTES);
    off += SU_WORLD_MAGIC_BYTES;
    su_put_u32(buf + off, SU_WORLD_STATE_VERSION);
    off += 4u;
    su_put_u64(buf + off, w->tick);
    off += 8u;
    su_put_u32(buf + off, w->field_count);
    off += 4u;
    st = su_ents_canonical_write(w->ents_committed, buf + off, cap - off, &ents_len);
    if (st != SU_OK) {
        return st;
    }
    off += ents_len;
    for (i = 0; i < w->field_count; i++) {
        su_put_u32(buf + off, w->fields[i].id);
        off += 4u;
        su_put_u32(buf + off, w->fields[i].type);
        off += 4u;
        for (r = 0; r < (uint32_t)hw; r++) {
            su_put_u32(buf + off, w->committed[i][r]);
            off += 4u;
        }
    }
    return SU_OK;
}

/* Restore from canonical bytes, version 1 (M0 layout, all slots alive) or version 2. */
uint32_t su_world_restore_canonical(const SuWorldDesc *desc, const uint8_t *buf, size_t len, SuWorld **out) {
    SuWorld *w = NULL;
    SuEnts *t = NULL;
    uint32_t version;
    uint32_t i;
    uint32_t st;
    uint64_t tick;
    uint64_t rows;
    uint32_t fc;
    size_t off;
    size_t ents_len = 0;
    size_t hw_size = 0;

    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (desc == NULL || buf == NULL || len < SU_WORLD_MAGIC_BYTES + 4u) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (memcmp(buf, SU_WORLD_MAGIC, SU_WORLD_MAGIC_BYTES) != 0) {
        return SU_E_PARSE;
    }
    version = su_rd_u32(buf + SU_WORLD_MAGIC_BYTES);

    if (version == SU_WORLD_STATE_VERSION_V1) {
        size_t hdr = SU_WORLD_MAGIC_BYTES + 4u + 8u + 8u + 4u;
        size_t vals;
        if (len < hdr) {
            return SU_E_PARSE;
        }
        tick = su_rd_u64(buf + SU_WORLD_MAGIC_BYTES + 4u);
        rows = su_rd_u64(buf + SU_WORLD_MAGIC_BYTES + 12u);
        fc = su_rd_u32(buf + SU_WORLD_MAGIC_BYTES + 20u);
        if (rows != desc->row_count || fc != desc->field_count) {
            return SU_E_SCHEMA_MISMATCH;
        }
        if (rows > SU_LIMIT_ROWS) {
            return SU_E_PARSE;
        }
        vals = (size_t)rows * 4u;
        if (len != hdr + vals + (size_t)fc * (8u + vals)) {
            return SU_E_PARSE;
        }
        off = hdr;
        for (i = 0; i < rows; i++) {
            if (su_rd_u32(buf + off + 4u * i) != desc->entity_ids[i]) {
                return SU_E_SCHEMA_MISMATCH;
            }
        }
        off += vals;
        st = su_world_create(desc, &w);
        if (st != SU_OK) {
            return st;
        }
        if (w->row_count != rows) {
            st = SU_E_SCHEMA_MISMATCH;
            goto fail;
        }
        for (i = 0; i < fc; i++) {
            if (su_rd_u32(buf + off) != w->fields[i].id || su_rd_u32(buf + off + 4u) != w->fields[i].type) {
                st = SU_E_SCHEMA_MISMATCH;
                goto fail;
            }
            off += 8u;
            memcpy(w->committed[i], buf + off, vals);
            off += vals;
        }
        w->tick = tick;
        *out = w;
        return SU_OK;
    }

    if (version != SU_WORLD_STATE_VERSION) {
        return SU_E_PARSE;
    }
    if (len < SU_WORLD_V2_HEADER_BYTES + SU_WORLD_ENTS_HEADER_BYTES) {
        return SU_E_PARSE;
    }
    tick = su_rd_u64(buf + SU_WORLD_MAGIC_BYTES + 4u);
    fc = su_rd_u32(buf + SU_WORLD_MAGIC_BYTES + 12u);
    if (fc != desc->field_count) {
        return SU_E_SCHEMA_MISMATCH;
    }
    {
        uint64_t hw = su_rd_u64(buf + SU_WORLD_V2_HEADER_BYTES + SU_WORLD_ENTS_HW_OFF);
        if (hw > SU_LIMIT_ROWS) {
            return SU_E_PARSE;
        }
        ents_len = SU_WORLD_ENTS_HEADER_BYTES + (size_t)hw * SU_WORLD_ENTS_RECORD_BYTES;
        hw_size = (size_t)hw;
    }
    if (len - SU_WORLD_V2_HEADER_BYTES < ents_len) {
        return SU_E_PARSE;
    }
    st = su_ents_canonical_parse(buf + SU_WORLD_V2_HEADER_BYTES, ents_len, &t);
    if (st != SU_OK) {
        return st == SU_E_RESOURCE_LIMIT ? st : SU_E_PARSE;
    }
    if (su_ents_capacity(t) < desc->row_count) {
        st = SU_E_SCHEMA_MISMATCH;
        goto fail;
    }
    off = SU_WORLD_V2_HEADER_BYTES + ents_len;
    {
        size_t per_field = 8u + 4u * hw_size;
        if (len != off + (size_t)fc * per_field) {
            st = SU_E_PARSE;
            goto fail;
        }
    }

    st = su_world_create_ex(desc, su_ents_capacity(t), &w);
    if (st != SU_OK) {
        goto fail;
    }
    st = su_world_adopt_entities(w, t);
    if (st != SU_OK) {
        goto fail;
    }
    for (i = 0; i < fc; i++) {
        uint32_t r;
        if (su_rd_u32(buf + off) != w->fields[i].id || su_rd_u32(buf + off + 4u) != w->fields[i].type) {
            st = SU_E_SCHEMA_MISMATCH;
            goto fail;
        }
        off += 8u;
        for (r = 0; r < (uint32_t)hw_size; r++) {
            uint32_t v = su_rd_u32(buf + off);
            if (v != 0u && !su_ents_slot_alive(t, r)) {
                st = SU_E_TYPE;
                goto fail;
            }
            w->committed[i][r] = v;
            off += 4u;
        }
    }
    w->tick = tick;
    st = SU_OK;
    *out = w;
    w = NULL;

fail:
    su_ents_destroy(t);
    su_world_destroy(w);
    return st;
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

/* ---- schema extensions (M2-01a) ----------------------------------------- */

uint32_t su_world_field_default(const SuWorld *w, uint32_t field_id, uint32_t *out) {
    long idx;
    if (w == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    idx = su_find_id(w, field_id);
    if (idx < 0) {
        return SU_E_SCHEMA_MISMATCH;
    }
    *out = w->fields[idx].def;
    return SU_OK;
}

uint32_t su_world_field_at(const SuWorld *w, uint32_t index, SuWorldFieldDesc *out, uint32_t *default_value) {
    if (w == NULL || out == NULL || default_value == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (index >= w->field_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    out->id = w->fields[index].id;
    out->type = w->fields[index].type;
    out->access = w->fields[index].access;
    out->name = w->fields[index].name;
    *default_value = w->fields[index].def;
    return SU_OK;
}

uint32_t su_world_retired_ids(const SuWorld *w, const uint32_t **ids, uint32_t *count) {
    if (w == NULL || ids == NULL || count == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *ids = w->retired;
    *count = w->retired_count;
    return SU_OK;
}

uint32_t su_world_lineage(const SuWorld *w, const uint8_t (**hashes)[32], uint32_t *count) {
    if (w == NULL || hashes == NULL || count == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *hashes = (const uint8_t (*)[32])w->lineage;
    *count = w->lineage_count;
    return SU_OK;
}

uint32_t su_world_set_layout_generation(SuWorld *w, uint32_t layout_generation) {
    if (w == NULL || w->phase != SU_WORLD_PHASE_IDLE) {
        return SU_E_INVALID_ARGUMENT;
    }
    w->layout_generation = layout_generation;
    return SU_OK;
}

uint32_t su_world_put_committed(SuWorld *w, uint32_t field_id, const uint32_t *values, uint64_t count) {
    long idx;
    uint64_t s;
    if (w == NULL || (values == NULL && count > 0u) || w->phase != SU_WORLD_PHASE_IDLE) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (count > w->row_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    idx = su_find_id(w, field_id);
    if (idx < 0) {
        return SU_E_SCHEMA_MISMATCH;
    }
    for (s = 0; s < w->row_count; s++) {
        uint32_t v = s < count ? values[s] : 0u;
        if (!su_ents_slot_alive(w->ents_committed, (uint32_t)s)) {
            v = 0u;
        }
        w->committed[idx][s] = v;
        w->working[idx][s] = v;
    }
    return SU_OK;
}

uint32_t su_world_create_successor(const SuWorld *old, const SuWorldDesc *desc, const SuWorldSchemaExt *ext,
                                   SuWorld **out) {
    SuWorld *w = NULL;
    uint32_t st;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (old == NULL || desc == NULL || su_world_in_tick(old)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (desc->row_count != 0u || desc->entity_ids != NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (strcmp(old->schema_id, desc->schema_id) != 0) {
        return SU_E_SCHEMA_MISMATCH;
    }
    st = su_world_create_ext(desc, old->row_count, ext, &w);
    if (st != SU_OK) {
        return st;
    }
    /* Entity table, generations and tick are carried over unchanged. */
    su_ents_destroy(w->ents_committed);
    su_ents_destroy(w->ents_working);
    w->ents_committed = NULL;
    w->ents_working = NULL;
    st = su_ents_clone(old->ents_committed, &w->ents_committed);
    if (st == SU_OK) {
        st = su_ents_clone(old->ents_committed, &w->ents_working);
    }
    if (st != SU_OK) {
        su_world_destroy(w);
        return st;
    }
    w->tick = old->tick;
    *out = w;
    return SU_OK;
}

/* Digest over the field table: the schema descriptor (contract section 1). */
static uint32_t su_schema_digest_internal(const SuWorld *w, uint8_t out[32]) {
    SuSha256 ctx;
    uint8_t hdr[18];
    uint8_t rec[16];
    uint32_t i;
    memcpy(hdr, "SUWSCHEMA", 9u);
    hdr[9] = 0u;
    su_put_u32(hdr + 10, 1u);
    su_put_u32(hdr + 14, w->field_count);
    su_sha256_init(&ctx);
    su_sha256_update(&ctx, hdr, sizeof hdr);
    for (i = 0; i < w->field_count; i++) {
        size_t n = strlen(w->fields[i].name);
        su_put_u32(rec, w->fields[i].id);
        su_put_u32(rec + 4, w->fields[i].type);
        su_put_u32(rec + 8, w->fields[i].def);
        su_put_u32(rec + 12, (uint32_t)n);
        su_sha256_update(&ctx, rec, sizeof rec);
        su_sha256_update(&ctx, w->fields[i].name, n);
    }
    su_sha256_final(&ctx, out);
    return SU_OK;
}

uint32_t su_world_schema_digest(const SuWorld *w, uint8_t out[32]) {
    if (w == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    return su_schema_digest_internal(w, out);
}

uint32_t su_world_recovery_digest(const SuWorld *w, uint8_t out[32]) {
    size_t need = 0;
    uint8_t *buf;
    uint8_t gen[4];
    uint8_t sd[32];
    uint32_t st;
    SuSha256 ctx;
    if (w == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = su_canonical_size_internal(w, &need);
    if (st != SU_OK) {
        return st;
    }
    buf = malloc(need > 0u ? need : 1u);
    if (buf == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    {
        size_t written = 0;
        st = su_world_canonical_write(w, buf, need, &written);
    }
    if (st != SU_OK) {
        free(buf);
        return st;
    }
    su_schema_digest_internal(w, sd);
    su_put_u32(gen, w->layout_generation);
    su_sha256_init(&ctx);
    su_sha256_update(&ctx, buf, need);
    su_sha256_update(&ctx, gen, sizeof gen);
    su_sha256_update(&ctx, sd, sizeof sd);
    su_sha256_final(&ctx, out);
    free(buf);
    return SU_OK;
}

const char *su_world_schema_id(const SuWorld *w) {
    return w != NULL ? w->schema_id : NULL;
}

uint32_t su_world_schema_version(const SuWorld *w) {
    return w != NULL ? w->schema_version : 0u;
}

/* ---- exact memory accounting (M3-08a) ----------------------------------- */

uint32_t su_world_memory(const SuWorld *w, SuWorldMemory *out) {
    uint64_t rows;
    uint64_t col_bytes;
    uint64_t fc;
    uint64_t meta;
    uint64_t rd;
    uint64_t wr;
    uint32_t i;
    if (w == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    /* Column length as su_alloc_column requests it (at least one element). */
    rows = w->row_count > 0u ? w->row_count : 1u;
    col_bytes = rows * SU_WORLD_ELEM_BYTES;
    for (i = 0; i < w->field_count; i++) {
        out->columns_committed += col_bytes;
        out->columns_working += col_bytes;
        if (w->staging[i] != NULL) {
            out->columns_staging += col_bytes;
        }
    }
    out->entity_table = su_ents_memory_bytes(w->ents_committed) + su_ents_memory_bytes(w->ents_working);

    meta = (uint64_t)sizeof *w;
    meta += (uint64_t)strlen(w->schema_id) + 1u;
    fc = w->field_count > 0u ? w->field_count : 1u;
    meta += fc * (sizeof *w->fields + 3u * sizeof *w->committed);
    for (i = 0; i < w->field_count; i++) {
        meta += (uint64_t)strlen(w->fields[i].name) + 1u;
    }
    rd = w->readable_count > 0u ? w->readable_count : 1u;
    wr = w->writable_count > 0u ? w->writable_count : 1u;
    meta += rd * sizeof *w->inv_bindings + wr * sizeof *w->out_bindings;
    meta += (uint64_t)w->retired_count * sizeof *w->retired;
    meta += (uint64_t)w->lineage_count * sizeof *w->lineage;
    out->metadata = meta;

    out->total = out->columns_committed + out->columns_working + out->columns_staging +
                 out->entity_table + out->metadata;
    return SU_OK;
}
