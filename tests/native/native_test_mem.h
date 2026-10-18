/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Memory and process helpers of the native backend test drivers (tests/native/scalar, tests/native/avx2), for
 * POSIX and Windows. Header-only; include after the system headers. On POSIX each helper is what the drivers did
 * before (mmap/mprotect, /proc/self/maps, fork, /proc/cpuinfo, clock_gettime). On Windows:
 *   - guarded columns: VirtualAlloc reserved PAGE_NOACCESS, the data pages switched to PAGE_READWRITE;
 *   - mapping count and permissions: a VirtualQuery walk of the address space; a region's protection is shown in
 *     /proc/self/maps style ("r-xp", "rw-p", "rwxp", ...);
 *   - "a store into the code page faults": the store runs in a non-inlined function under SEH (__try/__except
 *     on EXCEPTION_ACCESS_VIOLATION) instead of in a forked child;
 *   - the independent AVX2 oracle: IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE), the OS's own view
 *     (CPU support and OS-enabled AVX state), instead of /proc/cpuinfo;
 *   - clock_gettime(CLOCK_MONOTONIC): QueryPerformanceCounter. */
#ifndef SUTEKH_TESTS_NATIVE_NATIVE_TEST_MEM_H
#define SUTEKH_TESTS_NATIVE_NATIVE_TEST_MEM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static inline size_t su_nt_page_size(void) {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize > 0u ? (size_t)si.dwPageSize : 4096u;
#else
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? (size_t)p : 4096u;
#endif
}

/* total bytes reserved, no access; pages [first, first + count) readable and writable. Returns the base or NULL. */
static inline uint8_t *su_nt_guarded_map(size_t total, size_t first, size_t count) {
    size_t pg = su_nt_page_size();
#if defined(_WIN32)
    DWORD old = 0;
    uint8_t *base = VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (base == NULL) {
        return NULL;
    }
    if (!VirtualProtect(base + first * pg, count * pg, PAGE_READWRITE, &old)) {
        VirtualFree(base, 0, MEM_RELEASE);
        return NULL;
    }
    return base;
#else
    uint8_t *base = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        return NULL;
    }
    if (mprotect(base + first * pg, count * pg, PROT_READ | PROT_WRITE) != 0) {
        munmap(base, total);
        return NULL;
    }
    return base;
#endif
}

static inline void su_nt_unmap(uint8_t *base, size_t total) {
    if (base == NULL) {
        return;
    }
#if defined(_WIN32)
    (void)total;
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, total);
#endif
}

#if defined(_WIN32)
static inline void su_nt_protect_text(DWORD prot, char p[8]) {
    DWORD base = prot & 0xFFu;
    strcpy(p, "---p");
    switch (base) {
    case PAGE_READONLY: strcpy(p, "r--p"); break;
    case PAGE_READWRITE: strcpy(p, "rw-p"); break;
    case PAGE_WRITECOPY: strcpy(p, "rw-p"); break;
    case PAGE_EXECUTE: strcpy(p, "--xp"); break;
    case PAGE_EXECUTE_READ: strcpy(p, "r-xp"); break;
    case PAGE_EXECUTE_READWRITE: strcpy(p, "rwxp"); break;
    case PAGE_EXECUTE_WRITECOPY: strcpy(p, "rwxp"); break;
    default: break;
    }
}
#endif

/* Number of mappings of the process (POSIX: /proc/self/maps lines; Windows: committed regions), or -1. */
static inline int su_nt_count_maps(void) {
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *a = NULL;
    int n = 0;
    while (VirtualQuery(a, &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State == MEM_COMMIT) {
            n++;
        }
        a = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
    }
    return n;
#else
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int n = 0;
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        n++;
    }
    fclose(f);
    return n;
#endif
}

/* Permissions ("r-xp" style) of the mapping containing addr; *any_rwx (may be NULL) is set when any mapping is
 * writable and executable. Returns 1 when addr is mapped. */
static inline int su_nt_perms_at(uintptr_t addr, char perms[8], int *any_rwx) {
    int found = 0;
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *a = NULL;
    if (any_rwx != NULL) {
        *any_rwx = 0;
    }
    while (VirtualQuery(a, &mbi, sizeof mbi) == sizeof mbi) {
        uintptr_t lo = (uintptr_t)mbi.BaseAddress, hi = lo + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT) {
            char p[8];
            su_nt_protect_text(mbi.Protect, p);
            if (any_rwx != NULL && p[0] == 'r' && p[1] == 'w' && p[2] == 'x') {
                *any_rwx = 1;
            }
            if (!found && addr >= lo && addr < hi) {
                memcpy(perms, p, 8);
                found = 1;
            }
        }
        a = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
    }
#else
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    if (any_rwx != NULL) {
        *any_rwx = 0;
    }
    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        unsigned long lo = 0, hi = 0;
        char p[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, p) == 3) {
            if (any_rwx != NULL && p[0] == 'r' && p[1] == 'w' && p[2] == 'x') {
                *any_rwx = 1;
            }
            if (!found && addr >= (uintptr_t)lo && addr < (uintptr_t)hi) {
                memcpy(perms, p, sizeof p);
                found = 1;
            }
        }
    }
    fclose(f);
#endif
    return found;
}

/* Executable mappings (protection with execute) and writable-and-executable ones (POSIX: /proc/self/maps lines;
 * Windows: committed regions). Both 0 when the address space cannot be read. */
static inline void su_nt_exec_counts(size_t *exec_n, size_t *rwx_n) {
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *a = NULL;
    *exec_n = 0;
    *rwx_n = 0;
    while (VirtualQuery(a, &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State == MEM_COMMIT) {
            char p[8];
            su_nt_protect_text(mbi.Protect, p);
            if (p[2] == 'x') {
                (*exec_n)++;
            }
            if (p[0] == 'r' && p[1] == 'w' && p[2] == 'x') {
                (*rwx_n)++;
            }
        }
        a = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
    }
#else
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    *exec_n = 0;
    *rwx_n = 0;
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        char *sp = strchr(line, ' ');
        if (sp != NULL && sp[1] != '\0') {
            char *perm = sp + 1;
            if (perm[0] == 'r' && perm[1] == 'w' && perm[2] == 'x') {
                (*rwx_n)++;
            }
            if (perm[2] == 'x') {
                (*exec_n)++;
            }
        }
    }
    fclose(f);
#endif
}

#if defined(_WIN32)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#endif
/* Not inlined: an access violation inside a callee is what SEH reliably catches without -fasync-exceptions. */
static inline __declspec(noinline) void su_nt_poke(volatile uint8_t *p, uint8_t v) {
    *p = v;
}
static inline int su_nt_store_faults_win(const void *code, uint8_t v) {
    int faulted = 0;
    __try {
        su_nt_poke((volatile uint8_t *)(uintptr_t)code, v);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        faulted = 1;
    }
    return faulted;
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

/* 1 when a one-byte store of v into code faults (the code is not modified when it does), 0 when it succeeded,
 * -1 when the check could not run. */
static inline int su_nt_store_faults(const void *code, uint8_t v) {
#if defined(_WIN32)
    return su_nt_store_faults_win(code, v);
#else
    int status = 0;
    pid_t pid;
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid == 0) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)code;
        *p = v;
        _exit(0);
    }
    if (pid < 0) {
        return -1;
    }
    waitpid(pid, &status, 0);
    /* Killed by the fault, or nonzero under a sanitizer runtime; a clean exit means the store succeeded. */
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
#endif
}

/* Independent AVX2 oracle: 1 yes, 0 no, -1 unknown. */
static inline int su_nt_os_avx2(void) {
#if defined(_WIN32)
#ifndef PF_AVX2_INSTRUCTIONS_AVAILABLE
#define PF_AVX2_INSTRUCTIONS_AVAILABLE 40
#endif
    return IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) ? 1 : 0;
#else
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[8192];
    int found = -1;
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "flags", 5) == 0) {
            found = strstr(line, " avx2") != NULL ? 1 : 0;
            break;
        }
    }
    fclose(f);
    return found;
#endif
}

#if defined(_WIN32) && !defined(CLOCK_MONOTONIC)
#define CLOCK_MONOTONIC 1
static inline int su_nt_clock_gettime(int clk, struct timespec *ts) {
    LARGE_INTEGER f, c;
    (void)clk;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    ts->tv_sec = (time_t)(c.QuadPart / f.QuadPart);
    ts->tv_nsec = (long)((c.QuadPart % f.QuadPart) * 1000000000LL / f.QuadPart);
    return 0;
}
#define clock_gettime su_nt_clock_gettime
#endif

#endif
