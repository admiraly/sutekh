/* Sutekh M0 ABI contract, generation 1. Owner: M0-00 contract owner.
 * Status codes are plain uint32_t constants (no enums across the ABI).
 * Values are frozen once published; new codes append only. */
#ifndef SUTEKH_STATUS_H
#define SUTEKH_STATUS_H

#include <stdint.h>

#define SU_OK                       UINT32_C(0)
#define SU_E_PARSE                  UINT32_C(1)
#define SU_E_TYPE                   UINT32_C(2)
#define SU_E_DUPLICATE_KEY          UINT32_C(3)
#define SU_E_UNKNOWN_OPCODE         UINT32_C(4)
#define SU_E_UNDEFINED_REGISTER     UINT32_C(5)
#define SU_E_DUPLICATE_REGISTER     UINT32_C(6)
#define SU_E_UNDECLARED_READ        UINT32_C(7)
#define SU_E_UNDECLARED_WRITE       UINT32_C(8)
#define SU_E_SCHEMA_MISMATCH        UINT32_C(9)
#define SU_E_STALE_REVISION         UINT32_C(10)
#define SU_E_RESOURCE_LIMIT         UINT32_C(11)
#define SU_E_UNSUPPORTED_METHOD     UINT32_C(12)
#define SU_E_UNSUPPORTED_MIGRATION  UINT32_C(13)
#define SU_E_CANDIDATE_FAILED       UINT32_C(14)
#define SU_E_WORKER_FAILED          UINT32_C(15)
#define SU_E_CAPABILITY_DENIED      UINT32_C(16)
#define SU_E_IO                     UINT32_C(17)
/* Additions beyond the spec's initial set (documented in M0_CONTRACT.md). */
#define SU_E_INVALID_ARGUMENT       UINT32_C(18) /* null pointer, bad struct_size/abi_version */
#define SU_E_INTERNAL               UINT32_C(19) /* engine invariant violated; never a client error */
#define SU_E_MISSING_STORE          UINT32_C(20) /* declared-writable field never stored */
#define SU_STATUS_COUNT             UINT32_C(21) /* one past the highest defined code */

/* Returns the stable symbolic name, or "SU_E_UNKNOWN" for undefined values. */
static inline const char *su_status_name(uint32_t status) {
    static const char *const names[SU_STATUS_COUNT] = {
        "SU_OK", "SU_E_PARSE", "SU_E_TYPE", "SU_E_DUPLICATE_KEY",
        "SU_E_UNKNOWN_OPCODE", "SU_E_UNDEFINED_REGISTER",
        "SU_E_DUPLICATE_REGISTER", "SU_E_UNDECLARED_READ",
        "SU_E_UNDECLARED_WRITE", "SU_E_SCHEMA_MISMATCH", "SU_E_STALE_REVISION",
        "SU_E_RESOURCE_LIMIT", "SU_E_UNSUPPORTED_METHOD",
        "SU_E_UNSUPPORTED_MIGRATION", "SU_E_CANDIDATE_FAILED",
        "SU_E_WORKER_FAILED", "SU_E_CAPABILITY_DENIED", "SU_E_IO",
        "SU_E_INVALID_ARGUMENT", "SU_E_INTERNAL", "SU_E_MISSING_STORE"};
    return status < SU_STATUS_COUNT ? names[status] : "SU_E_UNKNOWN";
}

#endif
