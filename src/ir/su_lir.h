/* SU-LIR 0.1 decoder and semantic verifier (M0-02).
 *
 * su_lir_verify() takes the exact bytes of a capsule JSON and a .lir.json and
 * either produces a verified module or the first error in the stage order of
 * planning/contracts/M0_CONTRACT.md section 1 (r3):
 *   1 size/depth/instruction-count limits   -> SU_E_RESOURCE_LIMIT
 *   2 JSON syntax / UTF-8                   -> SU_E_PARSE        (capsule first)
 *   3 duplicate object keys                 -> SU_E_DUPLICATE_KEY (capsule first)
 *   4 top-level document shape              -> SU_E_TYPE         (capsule first)
 *   5 capsule/schema semantics              -> SU_E_SCHEMA_MISMATCH, and
 *     capsule limits.max_instructions (SU_E_RESOURCE_LIMIT)
 *   6 instructions in ascending order; within one instruction: op name
 *     (SU_E_UNKNOWN_OPCODE), keys/types/constant range (SU_E_TYPE), undefined
 *     register, duplicate dst, operand type, field permission
 *   7 end of body: writable field never stored -> SU_E_MISSING_STORE
 * Stage 1 also covers the capsule field-count cap (256).
 */
#ifndef SU_LIR_H
#define SU_LIR_H

#include <stddef.h>
#include <stdint.h>

#include "sutekh/abi.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"

#define SU_DIAG_POINTER_BYTES 256u
#define SU_DIAG_SYSTEM_BYTES 128u
#define SU_DIAG_MESSAGE_BYTES 256u

typedef struct SuDiag {
    uint32_t code;                              /* SU_OK when no error */
    size_t byte_offset;                         /* SIZE_MAX when not a byte-level error */
    char json_pointer[SU_DIAG_POINTER_BYTES];   /* RFC 6901; "" for document root */
    char system_id[SU_DIAG_SYSTEM_BYTES];       /* "" until the IR system_id is known */
    char message[SU_DIAG_MESSAGE_BYTES];
} SuDiag;

typedef struct SuLirField {
    uint32_t id;
    uint32_t type;   /* SU_TYPE_U32 */
    uint32_t access; /* SU_ACCESS_READ | SU_ACCESS_WRITE */
    char *name;      /* owned, NUL-terminated; not part of the semantic hash */
} SuLirField;

typedef struct SuLirModule {
    uint32_t artifact_format;    /* SU_ARTIFACT_FORMAT_VERSION */
    char *system_id;             /* owned */
    char *query;                 /* owned; equals schema.id */
    char *numeric_profile;       /* owned */
    uint32_t field_count;
    SuLirField *fields;          /* verified table: ascending field id */
    uint32_t register_count;
    uint32_t insn_count;
    uint64_t max_rows;           /* capsule limits.max_rows; enforced by the VM wrapper */
    SuInsn *insns;               /* field operands are table indices */
    uint8_t source_hash[32];
    char source_hash_hex[65];
    uint8_t capsule_hash[32];
    char capsule_hash_hex[65];
    uint8_t semantic_hash[32];
    char semantic_hash_hex[65];
} SuLirModule;

void su_diag_init(SuDiag *d);

/* Returns SU_OK and fills *out (caller frees with su_lir_module_free), or a
 * non-zero SU_E_* code with *diag describing the first error. On failure *out
 * is left zeroed. */
uint32_t su_lir_verify(const uint8_t *capsule, size_t capsule_len,
                       const uint8_t *lir, size_t lir_len,
                       SuLirModule *out, SuDiag *diag);

void su_lir_module_free(SuLirModule *m);

#endif
