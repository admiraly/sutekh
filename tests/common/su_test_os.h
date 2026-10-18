/* OS helpers shared by the C test drivers of the control plane, for POSIX and Windows.
 *
 * Header-only (static functions). A driver includes it after its system headers. On POSIX every helper is
 * the call the drivers made before (fork, waitpid, kill, setenv, opendir); on Windows it is the closest
 * native equivalent, and where the two differ the comment says how:
 *   - su_test_serve_*: the supervisor under test runs in a forked child on POSIX and on a thread of the
 *     test process on Windows (no fork). Its workers are real child processes on both.
 *   - su_test_kill_pid: SIGKILL on POSIX; TerminateProcess with exit code SU_PLATFORM_KILLED_EXIT_CODE.
 *   - su_test_symlink: symlink(2); CreateSymbolicLinkW, which needs Developer Mode or the privilege, so it
 *     can fail where POSIX would not. Callers report such checks as not run, never as passed. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_TESTS_COMMON_SU_TEST_OS_H
#define SUTEKH_TESTS_COMMON_SU_TEST_OS_H

#include "platform/su_posix.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/su_agent.h"
#include "agent/su_proc.h"
#include "platform/su_platform.h"
#include "platform/su_thread.h"

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <signal.h>
#include <sys/wait.h>
#endif

/* Exit code a test driver returns when a check cannot run on this platform (ctest SKIP_RETURN_CODE). */
#define SU_TEST_SKIP_EXIT 77

/* ---------- supervisor under test ---------- */

#if defined(_WIN32)
typedef struct SuTestServeThread {
    SuThread thread;
    SuAgentConfig cfg; /* copied: the caller's descriptor may go out of scope while the thread runs */
    int in_fd;
    int out_fd;
    int rc;
} SuTestServeThread;
#endif

/* The struct may be copied (sessions are returned by value); the thread state lives on the heap. */
typedef struct SuTestServe {
#if defined(_WIN32)
    SuTestServeThread *t;
#else
    pid_t pid;
#endif
} SuTestServe;

#if defined(_WIN32)
static inline void *su_test_serve_main(void *arg) {
    SuTestServeThread *t = arg;
    t->rc = su_agent_serve(t->in_fd, t->out_fd, &t->cfg);
    _close(t->in_fd);
    _close(t->out_fd);
    return NULL;
}
#endif

/* Starts su_agent_serve(cfg) over a new pipe pair. *wfd is the request write end, *rfd the response read
 * end. The strings cfg points to must stay valid until su_test_serve_wait (the descriptor itself is
 * copied). close_in_child (POSIX only, may be NULL) lists n descriptors the forked child closes first,
 * so it does not hold another session's pipes. Exits the test (2) when it cannot start. */
static inline void su_test_serve_start_ex(SuTestServe *s, const SuAgentConfig *cfg, int *wfd, int *rfd,
                                   const int *close_in_child, int n) {
    int req[2];
    int rsp[2];
    memset(s, 0, sizeof *s);
#if defined(_WIN32)
    (void)close_in_child;
    (void)n;
    /* Not inheritable: the supervisor's worker children must not hold the test's pipe ends. */
    if (_pipe(req, 65536u, _O_BINARY | _O_NOINHERIT) != 0 || _pipe(rsp, 65536u, _O_BINARY | _O_NOINHERIT) != 0) {
        perror("pipe");
        exit(2);
    }
    s->t = calloc(1, sizeof *s->t);
    if (s->t == NULL) {
        exit(2);
    }
    s->t->cfg = *cfg;
    s->t->in_fd = req[0];
    s->t->out_fd = rsp[1];
    if (su_thread_create(&s->t->thread, su_test_serve_main, s->t) != 0) {
        fprintf(stderr, "cannot start the supervisor thread\n");
        exit(2);
    }
#else
    pid_t pid;
    if (pipe(req) != 0 || pipe(rsp) != 0) {
        perror("pipe");
        exit(2);
    }
    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        int rc;
        int k;
        close(req[1]);
        close(rsp[0]);
        for (k = 0; close_in_child != NULL && k < n; k++) {
            close(close_in_child[k]);
        }
        rc = su_agent_serve(req[0], rsp[1], cfg);
        _exit(rc == 0 ? 0 : 1);
    }
    close(req[0]);
    close(rsp[1]);
    s->pid = pid;
#endif
    *wfd = req[1];
    *rfd = rsp[0];
}

static inline void su_test_serve_start(SuTestServe *s, const SuAgentConfig *cfg, int *wfd, int *rfd) {
    su_test_serve_start_ex(s, cfg, wfd, rfd, NULL, 0);
}

#if defined(_WIN32)
#define SU_TEST_DEFERRED_MAX 8
static const char *su_test_deferred[SU_TEST_DEFERRED_MAX];
static int su_test_deferred_n;
#endif

/* Removes an environment variable that only the supervisor just started (and its workers) must see. POSIX:
 * at once, since the forked supervisor keeps the environment it inherited. Windows: when the supervisor
 * thread ends (su_test_serve_wait), since it and the workers it spawns read this process's environment.
 * name must be a string literal or otherwise outlive the session. */
static inline void su_test_unset_after_start(const char *name) {
#if defined(_WIN32)
    if (su_test_deferred_n < SU_TEST_DEFERRED_MAX) {
        su_test_deferred[su_test_deferred_n++] = name;
    }
#else
    unsetenv(name);
#endif
}

/* Waits for the supervisor to finish (the caller has closed the write end). 1 when it ended cleanly. */
static inline int su_test_serve_wait(SuTestServe *s) {
#if defined(_WIN32)
    int ok;
    if (s->t == NULL) {
        return 0;
    }
    ok = su_thread_join(s->t->thread) == 0 && s->t->rc == 0;
    free(s->t);
    s->t = NULL;
    while (su_test_deferred_n > 0) {
        (void)_putenv_s(su_test_deferred[--su_test_deferred_n], "");
    }
    return ok;
#else
    int status = 0;
    waitpid(s->pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

/* ---------- processes ---------- */

/* Hard kill of a process by id. 0 on success. */
static inline int su_test_kill_pid(uint64_t pid) {
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD)pid);
    int rc;
    if (h == NULL) {
        return -1;
    }
    rc = TerminateProcess(h, SU_PLATFORM_KILLED_EXIT_CODE) ? 0 : -1;
    (void)WaitForSingleObject(h, 5000u);
    CloseHandle(h);
    return rc;
#else
    return kill((pid_t)pid, SIGKILL);
#endif
}

/* 1 when a process with this id is running (kill(pid, 0) on POSIX). */
static inline int su_test_pid_alive(uint64_t pid) {
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    DWORD code = 0;
    int alive;
    if (h == NULL) {
        return 0;
    }
    alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return kill((pid_t)pid, 0) == 0;
#endif
}

/* Resident set of a process in KiB (VmRSS on Linux, the working set on Windows), or -1 when unknown. */
static inline long su_test_rss_kib(uint64_t pid) {
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    PROCESS_MEMORY_COUNTERS pmc;
    long kib = -1;
    if (h == NULL) {
        return -1;
    }
    memset(&pmc, 0, sizeof pmc);
    if (K32GetProcessMemoryInfo(h, &pmc, sizeof pmc)) {
        kib = (long)(pmc.WorkingSetSize / 1024u);
    }
    CloseHandle(h);
    return kib;
#else
    char path[64];
    char line[256];
    long kib = -1;
    FILE *f;
    snprintf(path, sizeof path, "/proc/%ld/status", (long)pid);
    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            kib = strtol(line + 6, NULL, 10);
            break;
        }
    }
    fclose(f);
    return kib;
#endif
}

/* Parent process id of a running process (/proc/<pid>/stat field 4 on Linux; a Toolhelp snapshot on
 * Windows), or -1 when it cannot be read. */
static inline long su_test_parent_pid(uint64_t pid) {
#if defined(_WIN32)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    long ppid = -1;
    if (snap == INVALID_HANDLE_VALUE) {
        return -1;
    }
    memset(&pe, 0, sizeof pe);
    pe.dwSize = sizeof pe;
    if (Process32First(snap, &pe)) {
        do {
            if ((uint64_t)pe.th32ProcessID == pid) {
                ppid = (long)pe.th32ParentProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return ppid;
#else
    char path[128];
    char stat_line[256];
    long ppid = -1;
    FILE *f;
    snprintf(path, sizeof path, "/proc/%llu/stat", (unsigned long long)pid);
    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    /* comm may contain spaces and ')': the state and the ppid follow the last ')'. */
    if (fgets(stat_line, sizeof stat_line, f) != NULL) {
        char *p = strrchr(stat_line, ')');
        if (p != NULL) {
            sscanf(p + 2, "%*c %ld", &ppid);
        }
    }
    fclose(f);
    return ppid;
#endif
}

/* Waits for a worker started with su_proc_spawn to exit by itself (after its stdin was closed) and
 * releases it. 1 when it exited normally with status 0. */
static inline int su_test_proc_clean_exit(SuProc *p) {
    int ok;
#if defined(_WIN32)
    DWORD code = 1;
    ok = WaitForSingleObject((HANDLE)p->process, 60000u) == WAIT_OBJECT_0 &&
         GetExitCodeProcess((HANDLE)p->process, &code) && code == 0u;
    su_proc_kill(p);
#else
    int status = 0;
    ok = waitpid(p->pid, &status, 0) == p->pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    p->pid = 0;
    su_proc_kill(p); /* pid is 0: frees the line reader and the pipes only */
#endif
    return ok;
}

/* ---------- environment and files ---------- */

#if defined(_WIN32)
/* _putenv_s also updates the process environment block, which su_proc_spawn copies for its children. */
static inline int su_test_setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name) != NULL) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
static inline int su_test_unsetenv(const char *name) {
    return _putenv_s(name, "") == 0 ? 0 : -1; /* an empty value removes the variable */
}
#define setenv su_test_setenv
#define unsetenv su_test_unsetenv
#endif

/* Creates a symbolic link path -> target. 0 on success; on Windows it fails without Developer Mode or the
 * symbolic-link privilege (*why then says so). */
static inline int su_test_symlink(const char *target, const char *path, const char **why) {
#if defined(_WIN32)
    wchar_t wt[4096];
    wchar_t wp[4096];
    *why = NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, target, -1, wt, 4096) <= 0 ||
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 4096) <= 0) {
        *why = "path conversion failed";
        return -1;
    }
    if (!CreateSymbolicLinkW(wp, wt, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        *why = "CreateSymbolicLinkW refused (needs Developer Mode or SeCreateSymbolicLinkPrivilege)";
        return -1;
    }
    return 0;
#else
    *why = NULL;
    if (symlink(target, path) != 0) {
        *why = strerror(errno);
        return -1;
    }
    return 0;
#endif
}

/* Directory listing: names of the entries of dir, "." and ".." excluded. */
typedef struct SuTestDir {
#if defined(_WIN32)
    HANDLE h;
    WIN32_FIND_DATAA fd;
    int first;
#else
    DIR *d;
#endif
} SuTestDir;

static inline int su_test_dir_open(SuTestDir *it, const char *dir) {
#if defined(_WIN32)
    char pat[4200];
    snprintf(pat, sizeof pat, "%s/*", dir);
    it->h = FindFirstFileA(pat, &it->fd);
    it->first = 1;
    return it->h != INVALID_HANDLE_VALUE ? 0 : -1;
#else
    it->d = opendir(dir);
    return it->d != NULL ? 0 : -1;
#endif
}

static inline const char *su_test_dir_next(SuTestDir *it) {
    for (;;) {
        const char *name;
#if defined(_WIN32)
        if (!it->first && !FindNextFileA(it->h, &it->fd)) {
            return NULL;
        }
        it->first = 0;
        name = it->fd.cFileName;
#else
        struct dirent *de = readdir(it->d);
        if (de == NULL) {
            return NULL;
        }
        name = de->d_name;
#endif
        if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
            return name;
        }
    }
}

static inline void su_test_dir_close(SuTestDir *it) {
#if defined(_WIN32)
    if (it->h != INVALID_HANDLE_VALUE) {
        FindClose(it->h);
    }
#else
    if (it->d != NULL) {
        closedir(it->d);
    }
#endif
}

/* Scratch base directory: SU_TEST_SCRATCH, then TMPDIR, then the platform temporary directory. */
static inline const char *su_test_scratch_base(void) {
    static char buf[4096];
    const char *tmp = getenv("SU_TEST_SCRATCH");
    if (tmp != NULL && tmp[0] != '\0') {
        return tmp;
    }
    if (su_platform_temp_dir(buf, sizeof buf) != 0) {
        return ".";
    }
    return buf;
}

#endif
