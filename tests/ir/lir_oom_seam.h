/* Allocation-failure seam for test_lir_oom where the linker has no --wrap (lld-link, Windows).
 * Force-included only into the su_json.c and su_lir.c objects that test links on that platform, so
 * their malloc/calloc/realloc calls reach the test's __wrap_* functions, as --wrap does elsewhere. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_TESTS_IR_LIR_OOM_SEAM_H
#define SUTEKH_TESTS_IR_LIR_OOM_SEAM_H

#include <stddef.h>
#include <stdlib.h>

void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t n, size_t m);
void *__wrap_realloc(void *p, size_t n);

#define malloc(n) __wrap_malloc(n)
#define calloc(n, m) __wrap_calloc((n), (m))
#define realloc(p, n) __wrap_realloc((p), (n))

#endif
