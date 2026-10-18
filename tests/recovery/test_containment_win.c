/* Windows containment (constitution C07) observed from inside the child, the counterpart of test_containment.c
 * (which is Linux-only: rlimits, seccomp, Landlock, /proc). Every child is started by su_proc_spawn, so it runs
 * in the supervisor's job object; each probe is one request line, answered by one line, or the child's end.
 *
 *   spawn   CreateProcessW of this executable: refused inside the job (active-process limit 1)
 *   write   create a file in a directory with the default (medium) label: refused at low integrity
 *   alloc   commit 64 MiB: refused under a 32 MiB process memory limit
 *   crash   write through a null pointer: the child ends with SU_PROC_END_SIGNALED, signal 11
 *   sandbox su_sandbox_worker_install in the child: state "on" (the job limit is verified from inside)
 *
 * Every contained case has a control without the restriction (spawn and write from this process or an
 * unrestricted child, alloc without a memory limit) that must succeed, so a passing contained case cannot come
 * from a probe that always fails.
 *
 * Usage: test_containment_win <scratch_dir>      (the child mode is `test_containment_win --child`) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/su_proc.h"
#include "agent/su_proto.h"
#include "agent/su_sandbox.h"
#include "platform/su_platform.h"
#include "sutekh/status.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>

static long g_checks = 0;
static long g_fail = 0;
static char g_exe[4096];

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            g_fail++;                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

/* ---- probes (run in the child, or in this process for a control) ---- */

/* 1 when CreateProcessW of this executable (with --noop) succeeded. */
static int probe_spawn(void) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe[4096];
    wchar_t cmd[4200];
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    if (GetModuleFileNameW(NULL, exe, 4096) == 0u) {
        return 0;
    }
    _snwprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\" --noop", exe);
    cmd[sizeof cmd / sizeof cmd[0] - 1u] = L'\0';
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        return 0;
    }
    (void)WaitForSingleObject(pi.hProcess, 10000u);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* 1 when a new file could be created in dir (it is removed again). */
static int probe_write(const char *dir) {
    char path[4200];
    FILE *f;
    snprintf(path, sizeof path, "%s/probe-%lu.tmp", dir, (unsigned long)GetCurrentProcessId());
    f = fopen(path, "wb");
    if (f == NULL) {
        return 0;
    }
    fputs("x", f);
    fclose(f);
    (void)remove(path);
    return 1;
}

/* 1 when 64 MiB could be committed. */
static int probe_alloc(void) {
    void *p = VirtualAlloc(NULL, (SIZE_T)64u << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (p == NULL) {
        return 0;
    }
    VirtualFree(p, 0, MEM_RELEASE);
    return 1;
}

static int child_main(void) {
    char line[4400];
    su_platform_process_init();
    while (fgets(line, sizeof line, stdin) != NULL) {
        size_t n = strlen(line);
        while (n > 0u && (line[n - 1u] == '\n' || line[n - 1u] == '\r')) {
            line[--n] = '\0';
        }
        if (strcmp(line, "spawn") == 0) {
            printf("{\"done\":%d}\n", probe_spawn());
        } else if (strncmp(line, "write ", 6) == 0) {
            printf("{\"done\":%d}\n", probe_write(line + 6));
        } else if (strcmp(line, "alloc") == 0) {
            printf("{\"done\":%d}\n", probe_alloc());
        } else if (strcmp(line, "crash") == 0) {
            volatile int *volatile np = NULL;
            *np = 1;
            printf("{\"done\":1}\n");
        } else if (strcmp(line, "sandbox") == 0) {
            SuSandboxState st;
            su_sandbox_worker_install(&st);
            printf("{\"done\":%d}\n", st.state == SU_SANDBOX_ON ? 1 : 0);
        } else {
            printf("{\"done\":-1}\n");
        }
        fflush(stdout);
    }
    return 0;
}

/* ---- parent ---- */

/* Sends one probe to a fresh child. Returns the child's "done" value (0 or 1), or -2 when it gave no answer. */
static int run_probe(const SuProcLimits *lim, const char *probe, SuProc *out_proc) {
    const char *argv[3];
    SuProc p;
    const uint8_t *resp = NULL;
    size_t len = 0;
    int done = -2;
    argv[0] = g_exe;
    argv[1] = "--child";
    argv[2] = NULL;
    if (su_proc_spawn(&p, argv, lim) != SU_OK) {
        fprintf(stderr, "spawn failed for probe %s\n", probe);
        CHECK(0);
        return -2;
    }
    if (su_proc_request(&p, (const uint8_t *)probe, strlen(probe), su_now_ms() + 20000, &resp, &len) == SU_OK) {
        if (len >= 9u && memcmp(resp, "{\"done\":", 8) == 0) {
            done = resp[8] == '1' ? 1 : resp[8] == '0' ? 0 : -1;
        }
    }
    if (out_proc != NULL) {
        *out_proc = p;
    }
    su_proc_finish(&p, 2000);
    return done;
}

int main(int argc, char **argv) {
    SuProcLimits none;
    SuProcLimits ro;
    SuProcLimits mem;
    SuProc ended;
    char probe[4300];
    char dir[4096];
    int r;

    if (argc == 2 && strcmp(argv[1], "--child") == 0) {
        return child_main();
    }
    if (argc == 2 && strcmp(argv[1], "--noop") == 0) {
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scratch_dir>\n", argv[0]);
        return 2;
    }
    if (su_platform_self_exe(g_exe, sizeof g_exe) != 0 || su_platform_realpath(argv[1], dir, sizeof dir) != 0) {
        fprintf(stderr, "cannot resolve the executable or the scratch directory\n");
        return 2;
    }
    memset(&none, 0, sizeof none);
    memset(&ro, 0, sizeof ro);
    ro.deny_fs_writes = 1;
    memset(&mem, 0, sizeof mem);
    mem.address_space_bytes = (uint64_t)32u << 20;

    /* Process creation: allowed here (control), refused in every job child. */
    CHECK(probe_spawn() == 1);
    r = run_probe(&none, "spawn", NULL);
    CHECK(r == 0);

    /* File writes: allowed in an unrestricted child (control), refused at low integrity. */
    snprintf(probe, sizeof probe, "write %s", dir);
    CHECK(run_probe(&none, probe, NULL) == 1);
    {
        SuProc p;
        r = run_probe(&ro, probe, &p);
        CHECK(r == 0);
        CHECK(p.containment.reported == 1);
        CHECK(p.containment.fs_mode == SU_PROC_FS_LOW_INTEGRITY);
    }

    /* Committed memory: 64 MiB succeeds without a limit (control), fails under 32 MiB. */
    CHECK(run_probe(&none, "alloc", NULL) == 1);
    {
        SuProc p;
        r = run_probe(&mem, "alloc", &p);
        CHECK(r == 0);
        CHECK(p.containment.reported == 1 && p.containment.rlimit_as == (uint64_t)32u << 20);
    }

    /* A crash is reported as a signal, not as a supervisor kill or an exit code. */
    memset(&ended, 0, sizeof ended);
    r = run_probe(&none, "crash", &ended);
    CHECK(r == -2);
    CHECK(ended.end_kind == SU_PROC_END_SIGNALED);
    CHECK(ended.term_signal == 11);

    /* The worker self-check sees the job's active-process limit from inside. */
    CHECK(run_probe(&none, "sandbox", NULL) == 1);

    printf("test_containment_win: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
#else
int main(void) {
    printf("UNSUPPORTED on this platform: Windows job-object containment (see test_containment.c)\n");
    return 0;
}
#endif
