/* Sutekh fixed-step host clock (generation 1): how many simulation ticks a presentation host runs per frame,
 * and the interpolation factor between the last two ticks. Part of the host library (su_host / sutekh_host);
 * independent of any world. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_CLOCK_H
#define SUTEKH_CLOCK_H

#include <stdint.h>

#include "sutekh/host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Deterministic accumulator in integer time: every elapsed nanosecond counts exactly; there is no floating
 * point in the tick decision. The accumulator is kept in units of ns * hz, and a tick is due per 10^9 units,
 * so rates that do not divide a second (120 Hz) do not drift. When more than max_ticks would be due in one
 * advance (a stall), max_ticks are returned and the excess whole ticks are dropped (dropped_ticks counts
 * them); the fractional remainder is kept. */
typedef struct SuHostClock {
    uint32_t hz;
    uint32_t max_ticks;        /* per advance, >= 1 */
    uint64_t accumulator;      /* ns * hz, always < 10^9 after an advance */
    uint64_t total_ticks;      /* ticks returned so far */
    uint64_t dropped_ticks;    /* whole ticks discarded by the clamp */
} SuHostClock;

SU_HOST_API uint32_t su_host_clock_init(SuHostClock *c, uint32_t hz, uint32_t max_ticks);
/* Adds elapsed_ns of real time; returns the ticks to run now. alpha (optional) receives the interpolation
 * factor in [0, 1): the fraction of a tick accumulated after them. */
SU_HOST_API uint32_t su_host_clock_advance(SuHostClock *c, uint64_t elapsed_ns, double *alpha);
/* Interpolation factor of the current accumulator, in [0, 1). */
SU_HOST_API double su_host_clock_alpha(const SuHostClock *c);

#ifdef __cplusplus
}
#endif

#endif
