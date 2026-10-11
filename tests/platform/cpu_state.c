/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Prints one JSON line with the x86-64 features the native backend needs: CPUID leaf 1 OSXSAVE and
 * AVX, leaf 7 AVX2 and AVX512F, and XCR0 (XGETBV, valid only with OSXSAVE). Built and run by
 * tests/platform/capabilities.py with the host C compiler. GCC and Clang only (cpuid.h). */
#include <cpuid.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    unsigned a = 0;
    unsigned b = 0;
    unsigned c = 0;
    unsigned d = 0;
    unsigned osxsave = 0;
    unsigned avx = 0;
    unsigned avx2 = 0;
    unsigned avx512f = 0;
    uint64_t xcr0 = 0;
    unsigned max_leaf = __get_cpuid_max(0, NULL);

    if (__get_cpuid(1, &a, &b, &c, &d) != 0) {
        osxsave = (c >> 27) & 1u;
        avx = (c >> 28) & 1u;
    }
    if (max_leaf >= 7u) {
        __cpuid_count(7, 0, a, b, c, d);
        avx2 = (b >> 5) & 1u;
        avx512f = (b >> 16) & 1u;
    }
    if (osxsave != 0u) {
        uint32_t lo;
        uint32_t hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0u));
        xcr0 = ((uint64_t)hi << 32) | lo;
    }
    printf("{\"osxsave\":%u,\"avx\":%u,\"avx2\":%u,\"avx512f\":%u,\"xcr0\":\"0x%llx\","
           "\"ymm_state_enabled\":%s,\"max_cpuid_leaf\":%u}\n",
           osxsave, avx, avx2, avx512f, (unsigned long long)xcr0,
           (xcr0 & 6u) == 6u ? "true" : "false", max_leaf);
    return 0;
}
