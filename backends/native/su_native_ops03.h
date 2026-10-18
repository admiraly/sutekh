/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * C helpers of the IR 0.3 native lowering (see su_native_ops03.c). */
#ifndef SU_NATIVE_OPS03_H
#define SU_NATIVE_OPS03_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*SuNativeHelper03)(int64_t *d, const int64_t *a, const int64_t *b, const int64_t *c);

/* The helper of an IR 0.3 opcode, or NULL when the lowering emits the operation inline. */
SuNativeHelper03 su_native_helper03(uint32_t op);

#ifdef __cplusplus
}
#endif

#endif
