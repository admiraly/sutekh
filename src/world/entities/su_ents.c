/* Sutekh M2-00a generational entity table (contract r2). See su_ents.h for the model. */
#include "world/entities/su_ents.h"

#include <stdlib.h>
#include <string.h>

#define SU_ENTS_GEN_MAX_LIVE   (SU_GEN_RETIRED - 1u) /* 0xFFFFFFFE */
#define SU_ENTS_MAGIC          "SUENTABL"
#define SU_ENTS_MAGIC_BYTES    9u /* 8 ASCII bytes + 0x00 */
#define SU_ENTS_STATE_VERSION  UINT32_C(1)
#define SU_ENTS_POLICY_LEN     11u /* strlen("lowest_slot") */
#define SU_ENTS_HEADER_BYTES   (SU_ENTS_MAGIC_BYTES + 4u + 4u + SU_ENTS_POLICY_LEN + 8u + 8u + 8u + 8u)
#define SU_ENTS_RECORD_BYTES   9u /* u32 gen | u32 ext | u8 alive */
#define SU_ENTS_CHUNK_WORDS    (SU_CHUNK_SLOTS / 64u)

struct SuEnts {
    uint32_t capacity;
    uint32_t chunks;         /* ceil(capacity / SU_CHUNK_SLOTS) */
    uint32_t high_water;     /* 1 + highest slot ever allocated; 0 if none */
    uint32_t alive_count;
    uint32_t retired_count;
    uint32_t summary_words;  /* ceil(chunks / 64) */
    uint32_t hint_word;      /* every summary word below this index is zero */
    uint32_t *gen;           /* capacity */
    uint32_t *ext;           /* capacity; zero for dead slots */
    uint64_t *alive;         /* ceil(capacity / 64) words; bits past capacity are zero */
    uint64_t *avail;         /* same shape; bit set iff the slot is free */
    uint32_t *chunk_alive;   /* chunks */
    uint32_t *chunk_free;    /* chunks */
    uint64_t *summary;       /* summary_words; bit c set iff chunk c has a free slot */
};

/* ---- helpers ---------------------------------------------------------- */

static uint32_t su_ctz64(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_ctzll(v);
#else
    uint32_t n = 0;
    while ((v & UINT64_C(1)) == 0u) {
        v >>= 1;
        n++;
    }
    return n;
#endif
}

static uint32_t su_bit_get(const uint64_t *words, uint32_t i) {
    return (uint32_t)((words[i / 64u] >> (i % 64u)) & UINT64_C(1));
}

static void su_bit_set(uint64_t *words, uint32_t i) {
    words[i / 64u] |= UINT64_C(1) << (i % 64u);
}

static void su_bit_clear(uint64_t *words, uint32_t i) {
    words[i / 64u] &= ~(UINT64_C(1) << (i % 64u));
}

/* Slots in chunk c: SU_CHUNK_SLOTS except possibly the last chunk. */
static uint32_t su_chunk_len(const SuEnts *e, uint32_t c) {
    uint32_t start = c * SU_CHUNK_SLOTS;
    uint32_t rest = e->capacity - start;
    return rest < SU_CHUNK_SLOTS ? rest : SU_CHUNK_SLOTS;
}

static uint32_t su_words_for(uint32_t slots) {
    return (slots + 63u) / 64u;
}

/* Checked a+b into *out (size_t). */
static uint32_t su_add_size(size_t a, size_t b, size_t *out) {
    if (a > SIZE_MAX - b) {
        return SU_E_RESOURCE_LIMIT;
    }
    *out = a + b;
    return SU_OK;
}

/* Checked a*b into *out (size_t). */
static uint32_t su_mul_size(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) {
        return SU_E_RESOURCE_LIMIT;
    }
    *out = a * b;
    return SU_OK;
}

static void su_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void su_put_u64(uint8_t *p, uint64_t v) {
    su_put_u32(p, (uint32_t)(v & UINT64_C(0xFFFFFFFF)));
    su_put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t su_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t su_get_u64(const uint8_t *p) {
    return (uint64_t)su_get_u32(p) | ((uint64_t)su_get_u32(p + 4) << 32);
}

/* A slot becomes free: set avail, bump chunk_free, set the summary bit, lower the hint. */
static void su_mark_free(SuEnts *e, uint32_t slot) {
    uint32_t c = slot / SU_CHUNK_SLOTS;
    uint32_t w = c / 64u;
    su_bit_set(e->avail, slot);
    if (e->chunk_free[c] == 0u) {
        e->summary[w] |= UINT64_C(1) << (c % 64u);
        if (w < e->hint_word) {
            e->hint_word = w;
        }
    }
    e->chunk_free[c]++;
}

/* A free slot stops being free (allocated). */
static void su_unmark_free(SuEnts *e, uint32_t slot) {
    uint32_t c = slot / SU_CHUNK_SLOTS;
    su_bit_clear(e->avail, slot);
    e->chunk_free[c]--;
    if (e->chunk_free[c] == 0u) {
        e->summary[c / 64u] &= ~(UINT64_C(1) << (c % 64u));
    }
}

/* Allocates zeroed storage for capacity slots. No slot is free yet; callers mark them. */
static uint32_t su_raw_new(uint32_t capacity, SuEnts **out) {
    SuEnts *e;
    *out = NULL;
    e = calloc(1, sizeof *e);
    if (e == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    e->capacity = capacity;
    e->chunks = (capacity + SU_CHUNK_SLOTS - 1u) / SU_CHUNK_SLOTS;
    e->summary_words = su_words_for(e->chunks);
    /* calloc(0) may return NULL: always request at least one element. */
    e->gen = calloc(capacity > 0u ? capacity : 1u, sizeof(uint32_t));
    e->ext = calloc(capacity > 0u ? capacity : 1u, sizeof(uint32_t));
    e->alive = calloc(su_words_for(capacity) > 0u ? su_words_for(capacity) : 1u, sizeof(uint64_t));
    e->avail = calloc(su_words_for(capacity) > 0u ? su_words_for(capacity) : 1u, sizeof(uint64_t));
    e->chunk_alive = calloc(e->chunks > 0u ? e->chunks : 1u, sizeof(uint32_t));
    e->chunk_free = calloc(e->chunks > 0u ? e->chunks : 1u, sizeof(uint32_t));
    e->summary = calloc(e->summary_words > 0u ? e->summary_words : 1u, sizeof(uint64_t));
    if (e->gen == NULL || e->ext == NULL || e->alive == NULL || e->avail == NULL ||
        e->chunk_alive == NULL || e->chunk_free == NULL || e->summary == NULL) {
        su_ents_destroy(e);
        return SU_E_RESOURCE_LIMIT;
    }
    *out = e;
    return SU_OK;
}

/* Marks every slot free with generation 1 (fresh table). */
static void su_init_all_free(SuEnts *e) {
    uint32_t i;
    for (i = 0; i < e->capacity; i++) {
        e->gen[i] = SU_GEN_FIRST;
        su_mark_free(e, i);
    }
}

/* Validates a handle against the live state. */
static uint32_t su_check_handle(const SuEnts *e, SuEntity h, uint32_t *slot) {
    uint32_t s;
    if (SU_ENTITY_GEN(h) == 0u) {
        return SU_E_STALE_HANDLE;
    }
    s = SU_ENTITY_SLOT(h);
    if (s >= e->capacity || su_bit_get(e->alive, s) == 0u) {
        return SU_E_STALE_HANDLE;
    }
    if (e->gen[s] != SU_ENTITY_GEN(h)) {
        return SU_E_STALE_HANDLE;
    }
    *slot = s;
    return SU_OK;
}

/* ---- lifecycle --------------------------------------------------------- */

void su_ents_destroy(SuEnts *e) {
    if (e == NULL) {
        return;
    }
    free(e->gen);
    free(e->ext);
    free(e->alive);
    free(e->avail);
    free(e->chunk_alive);
    free(e->chunk_free);
    free(e->summary);
    free(e);
}

uint32_t su_ents_create(const SuEntsDesc *desc, SuEnts **out) {
    uint64_t want;
    uint32_t capacity;
    uint32_t i;
    uint32_t st;
    SuEnts *e;
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (desc == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    want = desc->max_entities > desc->initial_alive ? desc->max_entities : desc->initial_alive;
    if (want > SU_LIMIT_ROWS) {
        return SU_E_RESOURCE_LIMIT;
    }
    capacity = (uint32_t)want;
    st = su_raw_new(capacity, &e);
    if (st != SU_OK) {
        return st;
    }
    su_init_all_free(e);
    for (i = 0; i < (uint32_t)desc->initial_alive; i++) {
        SuEntity h = SU_ENTITY_INVALID;
        uint32_t ext = desc->external_ids != NULL ? desc->external_ids[i] : 0u;
        st = su_ents_alloc(e, ext, &h);
        if (st != SU_OK || SU_ENTITY_SLOT(h) != i) {
            su_ents_destroy(e);
            return SU_E_INTERNAL;
        }
    }
    *out = e;
    return SU_OK;
}

/* ---- accessors --------------------------------------------------------- */

uint32_t su_ents_capacity(const SuEnts *e) {
    return e != NULL ? e->capacity : 0u;
}

uint32_t su_ents_chunk_count(const SuEnts *e) {
    return e != NULL ? e->chunks : 0u;
}

uint32_t su_ents_high_water(const SuEnts *e) {
    return e != NULL ? e->high_water : 0u;
}

uint32_t su_ents_alive_count(const SuEnts *e) {
    return e != NULL ? e->alive_count : 0u;
}

uint32_t su_ents_retired_count(const SuEnts *e) {
    return e != NULL ? e->retired_count : 0u;
}

uint32_t su_ents_free_count(const SuEnts *e) {
    if (e == NULL) {
        return 0u;
    }
    return e->capacity - e->alive_count - e->retired_count;
}

uint32_t su_ents_chunk_alive_count(const SuEnts *e, uint32_t chunk, uint32_t *count) {
    if (e == NULL || count == NULL || chunk >= e->chunks) {
        return SU_E_INVALID_ARGUMENT;
    }
    *count = e->chunk_alive[chunk];
    return SU_OK;
}

/* ---- allocation and despawn ------------------------------------------- */

uint32_t su_ents_alloc(SuEnts *e, uint32_t ext_id, SuEntity *out) {
    uint32_t w;
    uint32_t slot = 0;
    int found = 0;
    if (e == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = SU_ENTITY_INVALID;
    for (w = e->hint_word; w < e->summary_words && !found; w++) {
        uint32_t c;
        uint32_t k;
        uint32_t nwords;
        if (e->summary[w] == 0u) {
            continue;
        }
        e->hint_word = w;
        c = w * 64u + su_ctz64(e->summary[w]);
        nwords = su_words_for(su_chunk_len(e, c));
        for (k = 0; k < nwords; k++) {
            uint32_t word = c * SU_ENTS_CHUNK_WORDS + k;
            if (e->avail[word] != 0u) {
                slot = c * SU_CHUNK_SLOTS + k * 64u + su_ctz64(e->avail[word]);
                found = 1;
                break;
            }
        }
        if (!found) {
            return SU_E_INTERNAL; /* summary disagrees with the avail bitmap */
        }
    }
    if (!found) {
        return SU_E_RESOURCE_LIMIT;
    }
    su_unmark_free(e, slot);
    su_bit_set(e->alive, slot);
    e->chunk_alive[slot / SU_CHUNK_SLOTS]++;
    e->alive_count++;
    e->ext[slot] = ext_id;
    if (slot + 1u > e->high_water) {
        e->high_water = slot + 1u;
    }
    *out = SU_ENTITY_MAKE(slot, e->gen[slot]);
    return SU_OK;
}

uint32_t su_ents_despawn(SuEnts *e, SuEntity expected) {
    uint32_t slot;
    if (e == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (su_check_handle(e, expected, &slot) != SU_OK) {
        return SU_E_STALE_HANDLE;
    }
    su_bit_clear(e->alive, slot);
    e->chunk_alive[slot / SU_CHUNK_SLOTS]--;
    e->alive_count--;
    e->ext[slot] = 0u;
    if (e->gen[slot] == SU_ENTS_GEN_MAX_LIVE) {
        /* Next generation would be 0xFFFFFFFF: retire. Never free, never reissued. */
        e->gen[slot] = SU_GEN_RETIRED;
        e->retired_count++;
    } else {
        e->gen[slot]++;
        su_mark_free(e, slot);
    }
    return SU_OK;
}

/* ---- handles ----------------------------------------------------------- */

uint32_t su_ents_validate(const SuEnts *e, SuEntity h) {
    uint32_t slot;
    if (e == NULL) {
        return SU_E_STALE_HANDLE;
    }
    return su_check_handle(e, h, &slot);
}

uint32_t su_ents_handle_for_slot(const SuEnts *e, uint32_t slot, SuEntity *out) {
    if (e == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (slot >= e->capacity || su_bit_get(e->alive, slot) == 0u) {
        return SU_E_STALE_HANDLE;
    }
    *out = SU_ENTITY_MAKE(slot, e->gen[slot]);
    return SU_OK;
}

int su_ents_slot_alive(const SuEnts *e, uint32_t slot) {
    if (e == NULL || slot >= e->capacity) {
        return 0;
    }
    return su_bit_get(e->alive, slot) != 0u;
}

int su_ents_slot_retired(const SuEnts *e, uint32_t slot) {
    if (e == NULL || slot >= e->capacity) {
        return 0;
    }
    return su_bit_get(e->alive, slot) == 0u && e->gen[slot] == SU_GEN_RETIRED;
}

uint32_t su_ents_slot_generation(const SuEnts *e, uint32_t slot, uint32_t *gen) {
    if (e == NULL || gen == NULL || slot >= e->capacity) {
        return SU_E_INVALID_ARGUMENT;
    }
    *gen = e->gen[slot];
    return SU_OK;
}

uint32_t su_ents_get_external(const SuEnts *e, SuEntity h, uint32_t *ext_id) {
    uint32_t slot;
    uint32_t st;
    if (e == NULL || ext_id == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = su_check_handle(e, h, &slot);
    if (st != SU_OK) {
        return st;
    }
    *ext_id = e->ext[slot];
    return SU_OK;
}

uint32_t su_ents_set_external(SuEnts *e, SuEntity h, uint32_t ext_id) {
    uint32_t slot;
    uint32_t st;
    if (e == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = su_check_handle(e, h, &slot);
    if (st != SU_OK) {
        return st;
    }
    e->ext[slot] = ext_id;
    return SU_OK;
}

/* ---- iteration --------------------------------------------------------- */

int su_ents_next_alive(const SuEnts *e, uint32_t from, uint32_t *slot) {
    uint32_t s = from;
    if (e == NULL || slot == NULL) {
        return 0;
    }
    while (s < e->capacity) {
        uint32_t c = s / SU_CHUNK_SLOTS;
        uint32_t w;
        uint64_t bits;
        if (e->chunk_alive[c] == 0u) {
            s = (c + 1u) * SU_CHUNK_SLOTS;
            continue;
        }
        w = s / 64u;
        bits = e->alive[w] & (~UINT64_C(0) << (s % 64u));
        if (bits != 0u) {
            *slot = w * 64u + su_ctz64(bits);
            return 1;
        }
        s = (w + 1u) * 64u;
    }
    return 0;
}

uint32_t su_ents_chunk_alive_bits(const SuEnts *e, uint32_t chunk, const uint64_t **words, uint32_t *word_count) {
    if (e == NULL || words == NULL || word_count == NULL || chunk >= e->chunks) {
        return SU_E_INVALID_ARGUMENT;
    }
    *words = &e->alive[(size_t)chunk * SU_ENTS_CHUNK_WORDS];
    *word_count = su_words_for(su_chunk_len(e, chunk));
    return SU_OK;
}

/* ---- copy and clone ---------------------------------------------------- */

uint32_t su_ents_copy(SuEnts *dst, const SuEnts *src) {
    uint32_t words;
    if (dst == NULL || src == NULL || dst->capacity != src->capacity) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (dst == src) {
        return SU_OK;
    }
    words = su_words_for(src->capacity);
    memcpy(dst->gen, src->gen, (size_t)src->capacity * sizeof(uint32_t));
    memcpy(dst->ext, src->ext, (size_t)src->capacity * sizeof(uint32_t));
    memcpy(dst->alive, src->alive, (size_t)words * sizeof(uint64_t));
    memcpy(dst->avail, src->avail, (size_t)words * sizeof(uint64_t));
    memcpy(dst->chunk_alive, src->chunk_alive, (size_t)src->chunks * sizeof(uint32_t));
    memcpy(dst->chunk_free, src->chunk_free, (size_t)src->chunks * sizeof(uint32_t));
    memcpy(dst->summary, src->summary, (size_t)src->summary_words * sizeof(uint64_t));
    dst->high_water = src->high_water;
    dst->alive_count = src->alive_count;
    dst->retired_count = src->retired_count;
    dst->hint_word = src->hint_word;
    return SU_OK;
}

uint32_t su_ents_clone(const SuEnts *src, SuEnts **out) {
    SuEnts *e;
    uint32_t st;
    if (src == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    st = su_raw_new(src->capacity, &e);
    if (st != SU_OK) {
        return st;
    }
    st = su_ents_copy(e, src);
    if (st != SU_OK) {
        su_ents_destroy(e);
        return st;
    }
    *out = e;
    return SU_OK;
}

/* ---- canonical encoding ------------------------------------------------ */

static uint32_t su_canonical_bytes(uint32_t high_water, size_t *out) {
    size_t records = 0;
    uint32_t st = su_mul_size((size_t)high_water, SU_ENTS_RECORD_BYTES, &records);
    if (st != SU_OK) {
        return st;
    }
    return su_add_size(SU_ENTS_HEADER_BYTES, records, out);
}

uint32_t su_ents_canonical_size(const SuEnts *e, size_t *size) {
    if (e == NULL || size == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    return su_canonical_bytes(e->high_water, size);
}

uint32_t su_ents_canonical_write(const SuEnts *e, uint8_t *buf, size_t cap, size_t *written) {
    size_t need = 0;
    size_t off;
    uint32_t i;
    uint32_t st;
    if (e == NULL || written == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = su_canonical_bytes(e->high_water, &need);
    if (st != SU_OK) {
        return st;
    }
    *written = need;
    if (buf == NULL || cap < need) {
        return SU_E_INVALID_ARGUMENT;
    }
    memcpy(buf, SU_ENTS_MAGIC, SU_ENTS_MAGIC_BYTES);
    off = SU_ENTS_MAGIC_BYTES;
    su_put_u32(buf + off, SU_ENTS_STATE_VERSION);
    off += 4u;
    su_put_u32(buf + off, SU_ENTS_POLICY_LEN);
    off += 4u;
    memcpy(buf + off, SU_ENTS_FREE_POLICY, SU_ENTS_POLICY_LEN);
    off += SU_ENTS_POLICY_LEN;
    su_put_u64(buf + off, e->capacity);
    off += 8u;
    su_put_u64(buf + off, e->high_water);
    off += 8u;
    su_put_u64(buf + off, e->alive_count);
    off += 8u;
    su_put_u64(buf + off, e->retired_count);
    off += 8u;
    for (i = 0; i < e->high_water; i++) {
        uint8_t *r = buf + off + (size_t)i * SU_ENTS_RECORD_BYTES;
        su_put_u32(r, e->gen[i]);
        su_put_u32(r + 4, e->ext[i]);
        r[8] = (uint8_t)su_bit_get(e->alive, i);
    }
    return SU_OK;
}

uint32_t su_ents_canonical_parse(const uint8_t *buf, size_t len, SuEnts **out) {
    uint64_t cap64;
    uint64_t hw64;
    uint64_t alive_claim;
    uint64_t retired_claim;
    uint32_t capacity;
    uint32_t hw;
    uint32_t i;
    uint32_t alive_n = 0;
    uint32_t ret_n = 0;
    size_t expected = 0;
    size_t off;
    uint32_t st;
    SuEnts *e;
    if (buf == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (len < SU_ENTS_HEADER_BYTES) {
        return SU_E_PARSE;
    }
    if (memcmp(buf, SU_ENTS_MAGIC, SU_ENTS_MAGIC_BYTES) != 0) {
        return SU_E_PARSE;
    }
    if (su_get_u32(buf + 9) != SU_ENTS_STATE_VERSION) {
        return SU_E_PARSE;
    }
    if (su_get_u32(buf + 13) != SU_ENTS_POLICY_LEN ||
        memcmp(buf + 17, SU_ENTS_FREE_POLICY, SU_ENTS_POLICY_LEN) != 0) {
        return SU_E_PARSE;
    }
    off = 17u + SU_ENTS_POLICY_LEN;
    cap64 = su_get_u64(buf + off);
    hw64 = su_get_u64(buf + off + 8u);
    alive_claim = su_get_u64(buf + off + 16u);
    retired_claim = su_get_u64(buf + off + 24u);
    off += 32u;
    /* Oversize is refused before any size arithmetic or allocation. */
    if (cap64 > SU_LIMIT_ROWS) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (hw64 > cap64 || alive_claim > hw64 || retired_claim > hw64 ||
        alive_claim + retired_claim > hw64) {
        return SU_E_PARSE;
    }
    capacity = (uint32_t)cap64;
    hw = (uint32_t)hw64;
    st = su_canonical_bytes(hw, &expected);
    if (st != SU_OK) {
        return st;
    }
    if (len != expected) {
        return SU_E_PARSE; /* truncated (len < expected) or trailing bytes (len > expected) */
    }
    st = su_raw_new(capacity, &e);
    if (st != SU_OK) {
        return st;
    }
    for (i = 0; i < capacity; i++) {
        if (i < hw) {
            const uint8_t *r = buf + off + (size_t)i * SU_ENTS_RECORD_BYTES;
            uint32_t g = su_get_u32(r);
            uint32_t x = su_get_u32(r + 4);
            uint8_t a = r[8];
            if (a > 1u) {
                goto fail;
            }
            e->gen[i] = g;
            e->ext[i] = x;
            if (a == 1u) {
                if (g == 0u || g == SU_GEN_RETIRED) {
                    goto fail;
                }
                su_bit_set(e->alive, i);
                e->chunk_alive[i / SU_CHUNK_SLOTS]++;
                alive_n++;
                continue;
            }
            if (x != 0u || g == 0u) {
                goto fail;
            }
            if (g == SU_GEN_RETIRED) {
                ret_n++;
                continue;
            }
        } else {
            e->gen[i] = SU_GEN_FIRST;
            e->ext[i] = 0u;
        }
        su_mark_free(e, i);
    }
    /* high_water must be exact: the top slot was really allocated (alive, or despawned at least once). */
    if (hw > 0u && su_bit_get(e->alive, hw - 1u) == 0u && e->gen[hw - 1u] == SU_GEN_FIRST) {
        goto fail;
    }
    if (alive_n != alive_claim || ret_n != retired_claim) {
        goto fail;
    }
    e->high_water = hw;
    e->alive_count = alive_n;
    e->retired_count = ret_n;
    *out = e;
    return SU_OK;
fail:
    su_ents_destroy(e);
    return SU_E_PARSE;
}

uint32_t su_ents_state_equal(const SuEnts *a, const SuEnts *b, int *equal) {
    uint32_t words;
    if (a == NULL || b == NULL || equal == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *equal = 0;
    if (a->capacity != b->capacity || a->high_water != b->high_water ||
        a->alive_count != b->alive_count || a->retired_count != b->retired_count) {
        return SU_OK;
    }
    words = su_words_for(a->capacity);
    if (memcmp(a->gen, b->gen, (size_t)a->capacity * sizeof(uint32_t)) != 0 ||
        memcmp(a->ext, b->ext, (size_t)a->capacity * sizeof(uint32_t)) != 0 ||
        memcmp(a->alive, b->alive, (size_t)words * sizeof(uint64_t)) != 0) {
        return SU_OK;
    }
    *equal = 1;
    return SU_OK;
}

/* ---- test seam --------------------------------------------------------- */

uint32_t su_ents_fault_set_generation(SuEnts *e, uint32_t slot, uint32_t gen) {
    if (e == NULL || slot >= e->high_water || gen == 0u || gen > SU_ENTS_GEN_MAX_LIVE) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (su_bit_get(e->alive, slot) == 0u && e->gen[slot] == SU_GEN_RETIRED) {
        return SU_E_INVALID_ARGUMENT; /* retirement is permanent */
    }
    e->gen[slot] = gen;
    return SU_OK;
}

/* ---- exact memory accounting (M3-08a) ----------------------------------- */

uint64_t su_ents_memory_bytes(const SuEnts *e) {
    uint64_t words;
    uint64_t chunks;
    uint64_t slots;
    uint64_t n;
    if (e == NULL) {
        return 0u;
    }
    /* Same allocation sizes as su_raw_new: every array holds at least one element. */
    slots = e->capacity > 0u ? e->capacity : 1u;
    words = su_words_for(e->capacity);
    words = words > 0u ? words : 1u;
    chunks = e->chunks > 0u ? e->chunks : 1u;
    n = (uint64_t)sizeof *e;
    n += 2u * slots * sizeof(uint32_t);                            /* gen, ext */
    n += 2u * words * sizeof(uint64_t);                            /* alive, avail */
    n += 2u * chunks * sizeof(uint32_t);                           /* chunk_alive, chunk_free */
    n += (e->summary_words > 0u ? e->summary_words : 1u) * sizeof(uint64_t); /* summary */
    return n;
}
