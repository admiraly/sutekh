/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
/* Consumer code reaching for engine internals. Linking Sutekh::host must not put the internal directories on the
 * include path, so every variant fails to compile at the #include (tests/consumer/check_consumer.py). */
#include "sutekh/host.h"
#if defined(CONSUMER_PROBE_SRC)
#include "world/su_world.h" /* src/ */
#elif defined(CONSUMER_PROBE_SRC_PLATFORM)
#include "su_platform.h" /* src/platform/ */
#elif defined(CONSUMER_PROBE_SPACE3D)
#include "space3d_geom.h" /* profiles/space3d/native/ */
#endif

int main(void) {
    return su_host_api_version() > 0u ? 0 : 1;
}
