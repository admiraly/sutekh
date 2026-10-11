/* M1-03 host capability detection for the native backend (see su_native.h).
 *
 * AVX2 is reported only when all of these hold (never by CPU name):
 *   CPUID.1:ECX.OSXSAVE[27] and CPUID.1:ECX.AVX[28]
 *   XGETBV(0) has XCR0 bits 1 (SSE state) and 2 (YMM state) set
 *   CPUID.(EAX=7,ECX=0):EBX.AVX2[5]
 * Other architectures and compilers report scalar only (MSVC detection is not implemented here). */
#include <stdint.h>

#include "backends/native/su_native.h"

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#define SU_CAPS_X86_64 1
#else
#define SU_CAPS_X86_64 0
#endif

static uint32_t g_disabled_caps = 0u;

void su_native_test_disable_caps(uint32_t mask) {
    g_disabled_caps = mask;
}

uint32_t su_native_host_caps(void) {
    uint32_t caps = SU_NCAP_X86_64_SCALAR;
#if SU_CAPS_X86_64
    unsigned int eax = 0u;
    unsigned int ebx = 0u;
    unsigned int ecx = 0u;
    unsigned int edx = 0u;
    unsigned int xlo = 0u;
    unsigned int xhi = 0u;
    if (__get_cpuid_max(0, NULL) >= 7u && __get_cpuid(1, &eax, &ebx, &ecx, &edx) != 0 &&
        (ecx & (1u << 27)) != 0u && (ecx & (1u << 28)) != 0u) {
        /* XGETBV is legal only when OSXSAVE is set, which was checked above. */
        __asm__ volatile("xgetbv" : "=a"(xlo), "=d"(xhi) : "c"(0u));
        if ((xlo & 0x6u) == 0x6u) {
            __cpuid_count(7, 0, eax, ebx, ecx, edx);
            if ((ebx & (1u << 5)) != 0u) {
                caps |= SU_NCAP_X86_64_AVX2;
            }
        }
    }
#endif
    return caps & ~g_disabled_caps;
}

uint32_t su_native_best_backend(void) {
    return (su_native_host_caps() & SU_NCAP_X86_64_AVX2) != 0u ? SU_NCAP_X86_64_AVX2
                                                                : SU_NCAP_X86_64_SCALAR;
}
