/* Fixed-step host clock (include/sutekh/clock.h). Integer accumulator in units of ns * hz: one tick is due per
 * 10^9 units, so a 120 Hz clock fed whole nanoseconds never drifts and gives the same tick sequence for the
 * same elapsed-time sequence on every machine. Only the interpolation factor is floating point. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "sutekh/clock.h"

#include <string.h>

#define CLOCK_UNIT UINT64_C(1000000000)

uint32_t su_host_clock_init(SuHostClock *c, uint32_t hz, uint32_t max_ticks) {
    if (c == NULL || hz == 0u || hz > SU_HOST_TICK_RATE_MAX || max_ticks == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(c, 0, sizeof *c);
    c->hz = hz;
    c->max_ticks = max_ticks;
    return SU_OK;
}

uint32_t su_host_clock_advance(SuHostClock *c, uint64_t elapsed_ns, double *alpha) {
    uint64_t secs;
    uint64_t rem;
    uint64_t due;
    uint64_t acc;
    uint32_t run;

    if (c == NULL || c->hz == 0u) {
        if (alpha != NULL) {
            *alpha = 0.0;
        }
        return 0u;
    }
    /* elapsed * hz without overflow: whole seconds contribute exactly hz ticks each; the remainder's
     * units (< 10^9 * hz <= 10^13) join the accumulator (< 10^9). */
    secs = elapsed_ns / CLOCK_UNIT;
    rem = elapsed_ns % CLOCK_UNIT;
    acc = c->accumulator + rem * c->hz;
    due = acc / CLOCK_UNIT;
    acc %= CLOCK_UNIT;
    if (secs > (UINT64_MAX - due) / c->hz) {
        due = UINT64_MAX; /* saturates; the clamp below drops it */
    } else {
        due += secs * c->hz;
    }
    if (due > c->max_ticks) {
        uint64_t drop = due - c->max_ticks;
        c->dropped_ticks = c->dropped_ticks > UINT64_MAX - drop ? UINT64_MAX : c->dropped_ticks + drop;
        due = c->max_ticks;
    }
    run = (uint32_t)due;
    c->accumulator = acc;
    c->total_ticks += run;
    if (alpha != NULL) {
        *alpha = su_host_clock_alpha(c);
    }
    return run;
}

double su_host_clock_alpha(const SuHostClock *c) {
    if (c == NULL) {
        return 0.0;
    }
    return (double)c->accumulator / (double)CLOCK_UNIT;
}
