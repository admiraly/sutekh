/* XXH64 (seed parameter, 64-bit result), the non-cryptographic hash of the xxHash family, written here
 * from the published algorithm description (https://github.com/Cyan4973/xxHash/blob/dev/doc/xxhash_spec.md).
 * Used by su_host_digest_fast. Not installed; not part of the ABI. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SU_XXH64_H
#define SU_XXH64_H

#include <stddef.h>
#include <stdint.h>

uint64_t su_xxh64(const void *data, size_t len, uint64_t seed);

#endif
