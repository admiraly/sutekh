/* Platform services for the M0 skeleton: monotonic clock and process id.
 * Internal to the native build; not part of the frozen ABI in include/sutekh. */
#ifndef SUTEKH_PLATFORM_H
#define SUTEKH_PLATFORM_H

#include <stdint.h>

/* Monotonic clock in nanoseconds from an unspecified origin. Returns 0 and
 * sets *out_ns to 0 on failure; check the return value. */
int su_platform_monotonic_ns(uint64_t *out_ns);

/* Process identifier of the calling process. */
uint32_t su_platform_pid(void);

#endif
