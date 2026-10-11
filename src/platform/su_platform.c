/* Feature macros must precede every system header (stdint.h included). */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "su_platform.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <windows.h>
#else
#  include <time.h>
#  include <unistd.h>
#endif

int su_platform_monotonic_ns(uint64_t *out_ns) {
    if (out_ns == NULL) {
        return 0;
    }
    *out_ns = 0;
#if defined(_WIN32)
    LARGE_INTEGER freq;
    LARGE_INTEGER count;
    if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&count) ||
        freq.QuadPart <= 0) {
        return 0;
    }
    /* Split whole seconds from the remainder to avoid overflow in count * 1e9. */
    uint64_t f = (uint64_t)freq.QuadPart;
    uint64_t c = (uint64_t)count.QuadPart;
    *out_ns = (c / f) * UINT64_C(1000000000) + ((c % f) * UINT64_C(1000000000)) / f;
    return 1;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    *out_ns = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    return 1;
#endif
}

uint32_t su_platform_pid(void) {
#if defined(_WIN32)
    return (uint32_t)GetCurrentProcessId();
#else
    return (uint32_t)getpid();
#endif
}
