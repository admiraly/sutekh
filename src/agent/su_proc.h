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
} SuProcLimits;

typedef struct SuProc {
    pid_t pid;      /* 0 when no child */
    int in_fd;      /* write end to child's stdin */
    int out_fd;     /* read end of child's stdout */
    int alive;
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

/* SIGKILL, close pipes, reap. Idempotent. */
void su_proc_kill(SuProc *p);

#endif
