/* Worker child processes (POSIX only: fork, execv, pipes, poll, waitpid, kill).
 * Windows is NOT implemented: this file and su_agent.c compile only where those
 * APIs exist. Workers are exec'd directly from a validated argv, never through a
 * shell. Pipes carry the JSON-lines protocol on the child's stdin and stdout. */
#ifndef SU_AGENT_PROC_H
#define SU_AGENT_PROC_H

#include <stdint.h>
#include <sys/types.h>

#include "agent/su_proto.h"

#define SU_PROC_MAX_ARGS 8u

typedef struct SuProcLimits {
    uint64_t address_space_bytes; /* RLIMIT_AS; 0 leaves it unchanged */
    uint32_t cpu_seconds;         /* RLIMIT_CPU; 0 leaves it unchanged */
    uint64_t file_bytes;          /* RLIMIT_FSIZE; 0 leaves it unchanged. SIGXFSZ is ignored, so an
                                     oversized write fails with EFBIG instead of killing the child. */
    uint32_t nofile;              /* RLIMIT_NOFILE; 0 leaves it unchanged */
    int deny_network;             /* nonzero: socket creation and connection are denied in the child
                                     (seccomp on x86-64 Linux, else a network namespace when privileged) */
    int deny_fs_writes;           /* nonzero: the child cannot create, remove, rename or truncate any file
                                     (Landlock, applied before exec and verified by the child's restrict call).
                                     Reads are not restricted. */
    const char *writable_dir;     /* TEST SEAM ONLY, NULL in production: with deny_fs_writes, this one directory
                                     keeps write access so the containment probes can write a bounded file. */
} SuProcLimits;

/* Network denial actually enforced in the child, verified in the child before exec. */
enum {
    SU_PROC_NET_NONE = 0,
    SU_PROC_NET_SECCOMP = 1, /* seccomp-bpf filter; socket(2) verified to fail with EPERM */
    SU_PROC_NET_NETNS = 2    /* unshare(CLONE_NEWNET) succeeded in the child */
};

/* Containment facts the child reports before exec (read back with getrlimit after setrlimit).
 * reported is 0 when the child never reached exec (no status was received) or no limits were given.
 * Limit values are soft limits; SU_PROC_UNLIMITED means RLIM_INFINITY. */
#define SU_PROC_UNLIMITED UINT64_MAX
/* Filesystem write denial enforced in the child (Landlock), verified by the child before exec. */
enum {
    SU_PROC_FS_NONE = 0,     /* not requested, or not enforced (reason says why) */
    SU_PROC_FS_LANDLOCK = 1  /* every write-class access is handled and no rule grants it (except writable_dir) */
};

typedef struct SuProcContainment {
    int reported;
    uint64_t rlimit_as;
    uint64_t rlimit_cpu;
    uint64_t rlimit_fsize;
    uint64_t rlimit_nofile;
    uint32_t net_mode;       /* SU_PROC_NET_* */
    uint32_t fs_mode;        /* SU_PROC_FS_* */
    int32_t fs_abi;          /* Landlock ABI version the kernel reports (0 when unavailable) */
    char reason[200];        /* why net_mode is what it is, in plain words */
    char fs_reason[160];     /* why fs_mode is what it is, in plain words */
} SuProcContainment;

/* How the last su_proc_request that failed ended the child (set only on failure). */
enum {
    SU_PROC_END_NONE = 0,
    SU_PROC_END_EXITED = 1,      /* exit_code holds the status */
    SU_PROC_END_SIGNALED = 2,    /* term_signal holds the signal (crash, OOM kill, RLIMIT_CPU) */
    SU_PROC_END_SUPERVISOR = 3   /* the supervisor killed it: deadline, overflow or I/O failure */
};

typedef struct SuProc {
    pid_t pid;      /* 0 when no child */
    int in_fd;      /* write end to child's stdin */
    int out_fd;     /* read end of child's stdout */
    int alive;
    int end_kind;   /* SU_PROC_END_* */
    int exit_code;
    int term_signal;
    SuProcContainment containment; /* set by su_proc_spawn */
    SuLineReader reader;
} SuProc;

/* argv must be NULL-terminated with 1..SU_PROC_MAX_ARGS entries; argv[0] must
 * contain '/' and be an executable regular file. Returns SU_OK or SU_E_WORKER_FAILED. */
uint32_t su_proc_spawn(SuProc *p, const char *const *argv, const SuProcLimits *limits);

/* Sends one request line and reads one response line before deadline_ms (absolute,
 * su_now_ms clock). On SU_OK, *resp and *resp_len are valid until the next call. On
 * SU_E_WORKER_FAILED (EOF, I/O error, deadline) or SU_E_RESOURCE_LIMIT (response
 * over 1 MiB) the child has been killed and reaped and p->alive is 0. */
uint32_t su_proc_request(SuProc *p, const uint8_t *req, size_t req_len, int64_t deadline_ms,
                         const uint8_t **resp, size_t *resp_len);

/* Closes stdin so a well-behaved child exits after its last reply, waits up to
 * grace_ms for it, then kills and reaps it. */
void su_proc_finish(SuProc *p, int64_t grace_ms);

/* 1 when the child has exited (it is then reaped and p->alive is 0); 0 while it runs. */
int su_proc_exited(SuProc *p);

/* SIGKILL, close pipes, reap. Idempotent. */
void su_proc_kill(SuProc *p);

#endif
