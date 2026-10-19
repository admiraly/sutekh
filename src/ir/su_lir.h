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
 *     register, duplicate dst, operand type, field permission (IR 0.3: then the
 *     field value type, SU_E_TYPE)
 *   7 end of body: writable field never stored -> SU_E_MISSING_STORE
 * Stage 1 also covers the capsule field-count cap (SU_LIMIT_FIELDS, 1024).
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
#define SU_DIAG_HINT_BYTES 192u   /* one concrete correction hint (audit item 6; LIVE_IR section 8) */
#define SU_DIAG_VALUE_BYTES 96u   /* expected / actual, short text */

typedef struct SuDiag {
    uint32_t code;                              /* SU_OK when no error */
    size_t byte_offset;                         /* SIZE_MAX when not a byte-level error */
    char json_pointer[SU_DIAG_POINTER_BYTES];   /* RFC 6901; "" for document root */
    char system_id[SU_DIAG_SYSTEM_BYTES];       /* "" until the IR system_id is known */
    char message[SU_DIAG_MESSAGE_BYTES];
    char hint[SU_DIAG_HINT_BYTES];              /* "" when no hint applies; set from the code by the verifier */
    char expected[SU_DIAG_VALUE_BYTES];         /* "" when the check has no expected value */
    char actual[SU_DIAG_VALUE_BYTES];           /* "" when the check has no actual value */
} SuDiag;

typedef struct SuLirField {
    uint32_t id;
    uint32_t type;   /* SU_TYPE_U32: the storage element type (a u32 lane) */
    uint32_t access; /* SU_ACCESS_READ | SU_ACCESS_WRITE */
    char *name;      /* owned, NUL-terminated; not part of the semantic hash */
    /* IR 0.3: numeric interpretation of the lane. value_type is SU_TYPE_U32, SU_TYPE_I32, SU_TYPE_I64 or
     * SU_TYPE_Q32; 0 (0.1/0.2 modules and host declarations) means SU_TYPE_U32. An i64/q32 field declared
     * with id I and name N is two table entries: lane 0 (id I, name "N.lo", low word) and lane 1 (id I+1,
     * name "N.hi", high word), adjacent in the ascending-id table. */
    uint32_t value_type;
    uint32_t lane;
} SuLirField;

/* value_type with 0 read as SU_TYPE_U32 (0.1/0.2 modules and host declarations leave it 0). */
#define SU_LIR_VALUE_TYPE(f) ((f)->value_type == 0u ? SU_TYPE_U32 : (f)->value_type)

/* Capability bits of SuLirModule.capabilities (audit item 4, C04). A capability names an IR 0.2
 * operation that needs a grant: "despawn_self" and "emit_event". */
#define SU_LIR_CAP_DESPAWN_SELF 0x1u
#define SU_LIR_CAP_EMIT_EVENT 0x2u

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
    uint32_t ir_version_minor;   /* 1 for "0.1", 2 for "0.2" (IR 0.2, M2-00c2), 3 for "0.3" (fixed_q32_v1), 4 for "0.4" */
    uint32_t max_struct_per_row; /* capsule limits.max_struct_per_row (0.2; 0 when absent) */
    uint32_t max_events_per_row; /* capsule limits.max_events_per_row (0.2; 0 when absent) */
    uint32_t uses_entity_ops;    /* 1 when any 0.2 op (slot/gen/tick/despawn_self/emit_event) appears */
    uint32_t has_system_rev;     /* capsule "system_rev" present (recorded and shown in the manifest; not in the hashes) */
    uint32_t system_rev;         /* the declared revision (>= 1) when has_system_rev */
    uint32_t has_capabilities;   /* capsule "capabilities" present: the grants are enforced (else M0-M2 behaviour) */
    uint32_t capabilities;       /* granted SU_LIR_CAP_* bits when has_capabilities */
    SuInsn *insns;               /* field operands are table indices */
    uint8_t source_hash[32];
    char source_hash_hex[65];
    uint8_t capsule_hash[32];
    char capsule_hash_hex[65];
    uint8_t semantic_hash[32];
    char semantic_hash_hex[65];
    /* IR 0.4 (2026-10-19). Row filter: a live row runs the body only when (field & mask) == value, read from the
     * system's entry state; other live rows keep their writable lanes (copied from the inputs) and emit nothing.
     * row_filter_field is the verified-table index of a readable u32 lane. */
    uint32_t has_row_filter;
    uint32_t row_filter_field;
    uint32_t row_filter_mask;
    uint32_t row_filter_value;
    uint32_t uses_gather;        /* 1 when any gather_* or find_eq_u32 appears (needs the whole-table view) */
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
