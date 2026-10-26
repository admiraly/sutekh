/* Prints every operation of the IR 0.3/0.4 table (src/ir/su_ops03.c) as "<name> <min minor>", one per line, for
 * check_op_coverage.py (docs/DETERMINISM_MATRIX.md section 2). The table itself is the source, so an operation
 * added to it without a matrix workload fails that test. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdio.h>

#include "ir/su_ops03.h"

int main(void) {
    size_t i, n = su_op03_count();
    for (i = 0; i < n; i++) {
        const SuOp03 *o = su_op03_at(i);
        printf("%s %u\n", o->name, (unsigned)su_op_min_minor(o));
    }
    return n > 0u ? 0 : 1;
}
