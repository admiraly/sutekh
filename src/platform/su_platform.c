/* Feature macros must precede every system header (stdint.h included). */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#  define _XOPEN_SOURCE 700 /* realpath(3) in glibc */
#endif

#include "su_platform.h"
#include "su_thread.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <windows.h>
#  include <process.h>
#else
#  include <limits.h>
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

/* ---------- threads (su_thread.h) ---------- */

#if defined(_WIN32)
typedef struct SuThreadStart {
    SuThreadFn fn;
    void *arg;
} SuThreadStart;

/* SRWLOCK and CONDITION_VARIABLE are one pointer each and are zero when initialized. */
_Static_assert(sizeof(SRWLOCK) == sizeof(void *), "SRWLOCK is pointer-sized");
_Static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void *), "CONDITION_VARIABLE is pointer-sized");

int su_mutex_init(SuMutex *m) {
    InitializeSRWLock((PSRWLOCK)&m->opaque);
    return 0;
}
void su_mutex_destroy(SuMutex *m) {
    (void)m; /* an SRWLOCK holds no resources */
}
void su_mutex_lock(SuMutex *m) {
    AcquireSRWLockExclusive((PSRWLOCK)&m->opaque);
}
void su_mutex_unlock(SuMutex *m) {
    ReleaseSRWLockExclusive((PSRWLOCK)&m->opaque);
}
int su_cond_init(SuCond *c) {
    InitializeConditionVariable((PCONDITION_VARIABLE)&c->opaque);
    return 0;
}
void su_cond_destroy(SuCond *c) {
    (void)c;
}
void su_cond_wait(SuCond *c, SuMutex *m) {
    (void)SleepConditionVariableSRW((PCONDITION_VARIABLE)&c->opaque, (PSRWLOCK)&m->opaque, INFINITE, 0);
}
void su_cond_signal(SuCond *c) {
    WakeConditionVariable((PCONDITION_VARIABLE)&c->opaque);
}
void su_cond_broadcast(SuCond *c) {
    WakeAllConditionVariable((PCONDITION_VARIABLE)&c->opaque);
}

static unsigned __stdcall thread_start(void *p) {
    SuThreadStart s = *(SuThreadStart *)p;
    free(p);
    (void)s.fn(s.arg);
    return 0;
}

int su_thread_create(SuThread *t, SuThreadFn fn, void *arg) {
    SuThreadStart *s = malloc(sizeof *s);
    uintptr_t h;
    if (s == NULL) {
        return 1;
    }
    s->fn = fn;
    s->arg = arg;
    h = _beginthreadex(NULL, 0, thread_start, s, 0, NULL);
    if (h == 0) {
        free(s);
        return 1;
    }
    t->handle = (void *)h;
    return 0;
}

int su_thread_join(SuThread t) {
    DWORD rc = WaitForSingleObject((HANDLE)t.handle, INFINITE);
    CloseHandle((HANDLE)t.handle);
    return rc == WAIT_OBJECT_0 ? 0 : 1;
}
#else
int su_mutex_init(SuMutex *m) {
    return pthread_mutex_init(&m->m, NULL);
}
void su_mutex_destroy(SuMutex *m) {
    (void)pthread_mutex_destroy(&m->m);
}
void su_mutex_lock(SuMutex *m) {
    (void)pthread_mutex_lock(&m->m);
}
void su_mutex_unlock(SuMutex *m) {
    (void)pthread_mutex_unlock(&m->m);
}
int su_cond_init(SuCond *c) {
    return pthread_cond_init(&c->c, NULL);
}
void su_cond_destroy(SuCond *c) {
    (void)pthread_cond_destroy(&c->c);
}
void su_cond_wait(SuCond *c, SuMutex *m) {
    (void)pthread_cond_wait(&c->c, &m->m);
}
void su_cond_signal(SuCond *c) {
    (void)pthread_cond_signal(&c->c);
}
void su_cond_broadcast(SuCond *c) {
    (void)pthread_cond_broadcast(&c->c);
}
int su_thread_create(SuThread *t, SuThreadFn fn, void *arg) {
    return pthread_create(&t->t, NULL, fn, arg);
}
int su_thread_join(SuThread t) {
    return pthread_join(t.t, NULL);
}
#endif

/* ---------- paths ---------- */

int su_platform_is_sep(char c) {
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

int su_platform_path_is_absolute(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
#if defined(_WIN32)
    if (su_platform_is_sep(path[0])) {
        return 1; /* UNC, an extended-length prefix, or the root of the current drive ("/etc/x") */
    }
    return ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' &&
           su_platform_is_sep(path[2]);
#else
    return path[0] == '/';
#endif
}

#if defined(_WIN32)
int su_platform_realpath(const char *path, char *out, size_t cap) {
    wchar_t *wpath = NULL;
    wchar_t *final = NULL;
    HANDLE h;
    DWORD n;
    DWORD need;
    const wchar_t *p;
    int wlen;
    int u8;
    int rc = ENOENT;
    char *s;

    if (path == NULL || out == NULL || cap == 0u || path[0] == '\0') {
        return EINVAL;
    }
    wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (wlen <= 0) {
        return EINVAL;
    }
    wpath = malloc((size_t)wlen * sizeof *wpath);
    if (wpath == NULL) {
        return ENOMEM;
    }
    (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, wlen);
    /* No access rights: opening for attributes only works for files held open elsewhere and for
     * directories (backup semantics), and never blocks on a pipe. */
    h = CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
        return ENOENT;
    }
    need = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (need == 0u) {
        CloseHandle(h);
        return ENOENT;
    }
    final = malloc(((size_t)need + 1u) * sizeof *final);
    if (final == NULL) {
        CloseHandle(h);
        return ENOMEM;
    }
    n = GetFinalPathNameByHandleW(h, final, need + 1u, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0u || n > need) {
        free(final);
        return ENOENT;
    }
    /* Strip the extended-length prefix: \\?\C:\x -> C:\x, \\?\UNC\srv\share -> \\srv\share. */
    p = final;
    if (wcsncmp(p, L"\\\\?\\UNC\\", 8) == 0) {
        p += 6;
        final[6] = L'\\';
    } else if (wcsncmp(p, L"\\\\?\\", 4) == 0) {
        p += 4;
    }
    u8 = WideCharToMultiByte(CP_UTF8, 0, p, -1, NULL, 0, NULL, NULL);
    if (u8 <= 0) {
        rc = EINVAL;
    } else if ((size_t)u8 > cap) {
        rc = ENAMETOOLONG;
    } else {
        (void)WideCharToMultiByte(CP_UTF8, 0, p, -1, out, u8, NULL, NULL);
        for (s = out; *s != '\0'; s++) {
            if (*s == '\\') {
                *s = '/';
            }
        }
        /* "C:/" keeps its separator; any other trailing separator is dropped. */
        n = (DWORD)strlen(out);
        if (n > 3u && out[n - 1u] == '/') {
            out[n - 1u] = '\0';
        }
        rc = 0;
    }
    free(final);
    return rc;
}
#else
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
int su_platform_realpath(const char *path, char *out, size_t cap) {
    char real[PATH_MAX];
    size_t n;
    if (path == NULL || out == NULL || cap == 0u) {
        return EINVAL;
    }
    if (realpath(path, real) == NULL) {
        return errno == ENAMETOOLONG ? ENAMETOOLONG : ENOENT;
    }
    n = strlen(real);
    if (n >= cap) {
        return ENAMETOOLONG;
    }
    memcpy(out, real, n + 1u);
    return 0;
}
#endif

#if defined(_WIN32)
int su_platform_temp_dir(char *out, size_t cap) {
    wchar_t w[MAX_PATH + 2];
    const char *tmpdir = getenv("TMPDIR");
    DWORD n;
    int u8;
    char *s;
    size_t len;
    if (out == NULL || cap == 0u) {
        return ENOENT;
    }
    /* TMPDIR, when set, wins on Windows too, so one test harness can place the spool on both platforms. */
    if (tmpdir != NULL && tmpdir[0] != '\0') {
        len = strlen(tmpdir);
        if (len >= cap) {
            return ENAMETOOLONG;
        }
        memcpy(out, tmpdir, len + 1u);
        return 0;
    }
    n = GetTempPathW((DWORD)(sizeof w / sizeof w[0]), w);
    if (n == 0u || n >= sizeof w / sizeof w[0]) {
        return ENOENT;
    }
    u8 = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (u8 <= 0 || (size_t)u8 > cap) {
        return ENAMETOOLONG;
    }
    (void)WideCharToMultiByte(CP_UTF8, 0, w, -1, out, u8, NULL, NULL);
    for (s = out; *s != '\0'; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
    len = strlen(out);
    while (len > 3u && out[len - 1u] == '/') {
        out[--len] = '\0';
    }
    return 0;
}

char *su_platform_mkdtemp(char *tmpl) {
    static volatile LONG counter;
    size_t n = tmpl != NULL ? strlen(tmpl) : 0u;
    unsigned attempt;
    if (n < 6u || strcmp(tmpl + n - 6u, "XXXXXX") != 0) {
        errno = EINVAL;
        return NULL;
    }
    for (attempt = 0; attempt < 1000u; attempt++) {
        static const char digits[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        LARGE_INTEGER qpc;
        uint64_t v;
        unsigned k;
        wchar_t wpath[4096];
        (void)QueryPerformanceCounter(&qpc);
        v = (uint64_t)qpc.QuadPart * UINT64_C(0x9E3779B97F4A7C15) ^ ((uint64_t)GetCurrentProcessId() << 32) ^
            (uint64_t)(uint32_t)InterlockedIncrement(&counter) * UINT64_C(0xBF58476D1CE4E5B9);
        for (k = 0; k < 6u; k++) {
            tmpl[n - 6u + k] = digits[v % 36u];
            v /= 36u;
        }
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, tmpl, -1, wpath, 4096) <= 0) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        if (CreateDirectoryW(wpath, NULL)) {
            return tmpl;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            errno = ENOENT;
            return NULL;
        }
    }
    errno = EEXIST;
    return NULL;
}
#else
int su_platform_temp_dir(char *out, size_t cap) {
    const char *tmp = getenv("TMPDIR");
    size_t n;
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = "/tmp";
    }
    n = strlen(tmp);
    if (out == NULL || n >= cap) {
        return ENAMETOOLONG;
    }
    memcpy(out, tmp, n + 1u);
    return 0;
}

char *su_platform_mkdtemp(char *tmpl) {
    return mkdtemp(tmpl);
}
#endif

/* ---------- process ---------- */

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>

void su_platform_process_init(void) {
    /* Protocol streams carry exact bytes: no CRLF or ^Z translation. */
    (void)_setmode(0, _O_BINARY);
    (void)_setmode(1, _O_BINARY);
    /* A crash or abort ends the process with a status; it never waits on a dialog. */
    (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

_Noreturn void su_platform_kill_self(void) {
    (void)TerminateProcess(GetCurrentProcess(), SU_PLATFORM_KILLED_EXIT_CODE);
    for (;;) {
        Sleep(INFINITE);
    }
}
#else
#include <signal.h>

void su_platform_process_init(void) {
}

_Noreturn void su_platform_kill_self(void) {
    (void)kill(getpid(), SIGKILL);
    for (;;) {
        pause();
    }
}
#endif

#if defined(_WIN32)
int su_platform_self_exe(char *out, size_t cap) {
    wchar_t w[32768];
    DWORD n = GetModuleFileNameW(NULL, w, (DWORD)(sizeof w / sizeof w[0]));
    int u8;
    char *s;
    if (out == NULL || cap == 0u || n == 0u || n >= sizeof w / sizeof w[0]) {
        return ENOENT;
    }
    u8 = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (u8 <= 0 || (size_t)u8 > cap) {
        return ENAMETOOLONG;
    }
    (void)WideCharToMultiByte(CP_UTF8, 0, w, -1, out, u8, NULL, NULL);
    for (s = out; *s != '\0'; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
    return 0;
}
#else
int su_platform_self_exe(char *out, size_t cap) {
    ssize_t n;
    if (out == NULL || cap < 2u) {
        return EINVAL;
    }
    n = readlink("/proc/self/exe", out, cap - 1u);
    if (n <= 0) {
        return ENOENT;
    }
    if ((size_t)n >= cap - 1u) {
        return ENAMETOOLONG;
    }
    out[n] = '\0';
    return 0;
}
#endif

#if defined(_WIN32)
uint32_t su_platform_cpu_count(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0u ? (uint32_t)n : 1u;
}

void su_platform_sleep_ms(uint32_t ms) {
    Sleep(ms);
}
#else
uint32_t su_platform_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (uint32_t)n : 1u;
}

void su_platform_sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}
#endif
