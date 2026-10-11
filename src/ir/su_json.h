/* Strict JSON reader for SU-LIR documents.
 *
 * Rules: RFC 8259 grammar, strict UTF-8, no duplicate object keys, no NUL
 * characters inside strings (\u0000 rejected), nesting depth and byte size
 * capped by SuJsonLimits. Numbers are never converted through floating point:
 * an integer literal is kept as an exact magnitude (u64, with an overflow flag)
 * plus a sign; fractions and exponents mark the number as non-integer.
 * Range rules (u32, non-negative) are applied by callers through
 * su_json_get_uint(); the parser itself accepts any grammatical number.
 *
 * Error precedence within one document (first failing stage wins):
 * size/depth (SU_E_RESOURCE_LIMIT), syntax/UTF-8 (SU_E_PARSE), then duplicate
 * keys (SU_E_DUPLICATE_KEY). For SU_E_DUPLICATE_KEY the tree is still complete
 * in doc->root, so the caller can apply earlier-stage checks of other documents
 * first. For every other non-zero status the document is empty. */
#ifndef SU_JSON_H
#define SU_JSON_H

#include <stddef.h>
#include <stdint.h>

typedef enum SuJsonType {
    SU_JSON_NULL = 0,
    SU_JSON_BOOL,
    SU_JSON_NUMBER,
    SU_JSON_STRING,
    SU_JSON_ARRAY,
    SU_JSON_OBJECT
} SuJsonType;

typedef struct SuJsonValue SuJsonValue;

typedef struct SuJsonMember {
    const char *key; /* decoded UTF-8, NUL-terminated, may contain no NUL */
    size_t key_len;
    SuJsonValue *value;
} SuJsonMember;

struct SuJsonValue {
    SuJsonType type;
    size_t offset; /* byte offset of the value's first byte */
    union {
        int boolean;
        struct {
            int negative;
            int is_integer;  /* no fraction and no exponent */
            int overflow;    /* integer magnitude exceeds UINT64_MAX */
            uint64_t magnitude;
        } number;
        struct {
            const char *data; /* decoded UTF-8, NUL-terminated */
            size_t len;
        } string;
        struct {
            SuJsonValue **items;
            size_t count;
        } array;
        struct {
            SuJsonMember *members; /* source order */
            size_t count;
        } object;
    } u;
};

typedef struct SuJsonArena SuJsonArena;

typedef struct SuJsonDoc {
    SuJsonValue *root;
    SuJsonArena *arena; /* owns every value, string and member array */
} SuJsonDoc;

typedef struct SuJsonLimits {
    size_t max_bytes;
    size_t max_depth; /* a top-level object or array is depth 1 */
} SuJsonLimits;

typedef struct SuJsonError {
    uint32_t code;      /* SU_E_* */
    size_t offset;      /* byte offset of the failure */
    char pointer[256];  /* RFC 6901 pointer to the failing location, "" for root */
    char message[160];
} SuJsonError;

uint32_t su_json_parse(const uint8_t *data, size_t len, const SuJsonLimits *limits,
                       SuJsonDoc *doc, SuJsonError *err);
void su_json_doc_free(SuJsonDoc *doc);

/* First member with this key of an object, or NULL. */
const SuJsonValue *su_json_member(const SuJsonValue *obj, const char *key);

/* Succeeds for a non-negative integer literal that does not overflow uint64.
 * A "-0" literal is accepted as zero. */
int su_json_get_uint(const SuJsonValue *v, uint64_t *out);

#endif
