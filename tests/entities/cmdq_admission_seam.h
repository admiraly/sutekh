/* Test-only allocation recorder, force-included into su_cmdq.c for admission checks. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SU_TEST_CMDQ_ADMISSION_SEAM_H
#define SU_TEST_CMDQ_ADMISSION_SEAM_H

#include <stddef.h>
#include <stdlib.h>

void *su_admission_malloc(size_t n);
void *su_admission_calloc(size_t n, size_t size);

#define malloc(n) su_admission_malloc(n)
#define calloc(n, size) su_admission_calloc((n), (size))

#endif
