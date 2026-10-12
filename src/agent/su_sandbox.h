/* Worker self-sandbox (constitution C07, M1 closure D).
 *
 * A worker installs this filter itself, after it has started and before it reads its first request.
 * It denies process creation and program execution for the rest of the worker's life:
 *   execve, execveat, fork and vfork return EPERM;
 *   clone returns EPERM unless CLONE_THREAD is set (threads only: the tick-worker pool uses them);
 *   clone3 returns ENOSYS, so glibc falls back to clone for threads and the check above applies;
 *   wrong architecture and x32 syscalls kill the process.
 * The supervisor spawns workers with execve before this filter exists, so the supervisor is not affected.
 *
 * SUTEKH_WORKER_SANDBOX=0 turns the filter off, but only in a sanitizer build (AddressSanitizer or
 * ThreadSanitizer, whose runtimes fork or ptrace). In every other build the variable is ignored. The state
 * is reported in the worker's internal.stats answer ("self_sandbox") and checked by harness TX6. */
#ifndef SU_AGENT_SANDBOX_H
#define SU_AGENT_SANDBOX_H

#include <stdint.h>

enum {
    SU_SANDBOX_ON = 0,          /* filter installed and verified (execve returned EPERM) */
    SU_SANDBOX_UNAVAILABLE = 1, /* not installed: not x86-64 Linux, or the kernel refused it */
    SU_SANDBOX_OPTED_OUT = 2    /* SUTEKH_WORKER_SANDBOX=0 in a sanitizer build */
};

typedef struct SuSandboxState {
    uint32_t state;      /* SU_SANDBOX_* */
    uint32_t filters;    /* filters this worker installed (1 when on) */
    int32_t errno_value; /* errno of the failing step, 0 when on */
    char reason[160];    /* why the state is what it is, in plain words */
} SuSandboxState;

/* Installs the worker self-sandbox and fills st. Call once, in the worker process, before serving. */
void su_sandbox_worker_install(SuSandboxState *st);

/* Lower-case state name for reports: "on", "unavailable" or "opted_out". */
const char *su_sandbox_state_name(uint32_t state);

#endif
