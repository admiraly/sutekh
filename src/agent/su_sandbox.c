#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "agent/su_sandbox.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/prctl.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#endif

#if defined(__linux__) && defined(__x86_64__)
#  include <linux/audit.h>
#  include <linux/filter.h>
#  include <linux/seccomp.h>
#  include <sched.h>
#  define SU_SANDBOX_X64 1
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define SU_SANDBOX_SANITIZED 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define SU_SANDBOX_SANITIZED 1
#  endif
#endif

const char *su_sandbox_state_name(uint32_t state) {
    switch (state) {
    case SU_SANDBOX_ON:
        return "on";
    case SU_SANDBOX_OPTED_OUT:
        return "opted_out";
    default:
        return "unavailable";
    }
}

#ifdef SU_SANDBOX_X64
/* Denied with EPERM: process creation and program execution (x86-64 numbers, checked against
 * asm/unistd_64.h: fork 57, vfork 58, execve 59, execveat 322). clone 56 is decided by its flags,
 * clone3 435 returns ENOSYS. */
static const uint32_t g_exec_denied_nr[] = {57u, 58u, 59u, 322u};
#define SU_SANDBOX_NR_EXEC (sizeof g_exec_denied_nr / sizeof g_exec_denied_nr[0])
#define SU_SANDBOX_NR_CLONE 56u
#define SU_SANDBOX_NR_CLONE3 435u
#define SU_SANDBOX_CLONE_THREAD 0x00010000u
#define SU_SANDBOX_INSNS (16u + SU_SANDBOX_NR_EXEC)

/* Layout (indices): 0 load arch; 1 arch == x86-64 ? 3 : (fall to 2); 2 kill; 3 load nr; 4 nr >= 0x40000000
 * ? kill; 5..4+E exec-denied nr ? EPERM; then clone ? CLONE_CHECK; clone3 ? ENOSYS; default ALLOW;
 * CLONE_CHECK: load arg0; JSET CLONE_THREAD ? ALLOW : EPERM; ALLOW; EPERM; ENOSYS; KILL. */
static size_t build_sandbox_filter(struct sock_filter *prog, size_t cap) {
    const size_t e = SU_SANDBOX_NR_EXEC;
    size_t i = 0;
    size_t k;
    size_t allow_at, clone_check_at, jset_at, ret_allow_at, eperm_at, enosys_at, kill_at;
    /* Fixed tail, in order after the default-allow instruction:
     *   clone_check_at: LD arg0 low word, jset_at: JSET CLONE_THREAD, ret_allow_at: ALLOW,
     *   eperm_at: ERRNO(EPERM), enosys_at: ERRNO(ENOSYS), kill_at: KILL. */
    allow_at = 5u + e + 2u;      /* after the clone and clone3 compares */
    clone_check_at = allow_at + 1u;
    jset_at = clone_check_at + 1u;
    ret_allow_at = jset_at + 1u;
    eperm_at = ret_allow_at + 1u;
    enosys_at = eperm_at + 1u;
    kill_at = enosys_at + 1u;
    if (cap < kill_at + 1u) {
        return 0;
    }
    prog[i++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 4u); /* seccomp_data.arch */
    prog[i++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1u, 0u);
    prog[i++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS); /* wrong arch */
    prog[i++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0u);              /* seccomp_data.nr */
    prog[i] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u,
                                           (unsigned char)(kill_at - i - 1u), 0u); /* x32 */
    i++;
    for (k = 0; k < e; k++) {
        prog[i] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, g_exec_denied_nr[k],
                                               (unsigned char)(eperm_at - i - 1u), 0u);
        i++;
    }
    /* i == 5 + e here. clone: jump to the flag check, which sits two instructions after allow_at. */
    prog[i] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SU_SANDBOX_NR_CLONE,
                                           (unsigned char)(clone_check_at - i - 1u), 0u);
    i++;
    prog[i] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SU_SANDBOX_NR_CLONE3,
                                           (unsigned char)(enosys_at - i - 1u), 0u);
    i++;
    /* default allow: i == allow_at */
    prog[i++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    /* clone_check_at */
    prog[i++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 16u); /* args[0], low 32 bits */
    /* jset_at: flags & CLONE_THREAD ? allow : EPERM */
    prog[i++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, SU_SANDBOX_CLONE_THREAD,
                                             (unsigned char)(ret_allow_at - jset_at - 1u),
                                             (unsigned char)(eperm_at - jset_at - 1u));
    prog[i++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    prog[i++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    prog[i++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS);
    prog[i++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    return i;
}
#endif

/* Plain-language state and verification. Each state is set once, here, in the worker process. */
void su_sandbox_worker_install(SuSandboxState *st) {
    memset(st, 0, sizeof *st);
    st->state = SU_SANDBOX_UNAVAILABLE;
    snprintf(st->reason, sizeof st->reason, "not installed: x86-64 Linux only");
#if defined(_WIN32)
    /* Windows has no exec and no self-installed syscall filter. The equivalent denial of process creation
     * is the supervisor's job object with an active-process limit of 1 (su_proc_spawn): CreateProcess in
     * the worker then fails. The worker verifies that it runs inside such a job; it cannot install it. */
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        BOOL in_job = FALSE;
        memset(&li, 0, sizeof li);
        if (!IsProcessInJob(GetCurrentProcess(), NULL, &in_job) || !in_job) {
            snprintf(st->reason, sizeof st->reason, "not installed: worker is not in a job object (Windows)");
            return;
        }
        if (!QueryInformationJobObject(NULL, JobObjectExtendedLimitInformation, &li, sizeof li, NULL)) {
            st->errno_value = (int32_t)GetLastError();
            snprintf(st->reason, sizeof st->reason, "not verified: job limits cannot be read (Windows error %lu)",
                     (unsigned long)GetLastError());
            return;
        }
        if ((li.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS) == 0u ||
            li.BasicLimitInformation.ActiveProcessLimit != 1u) {
            snprintf(st->reason, sizeof st->reason, "not installed: the job has no active-process limit of 1");
            return;
        }
        st->state = SU_SANDBOX_ON;
        st->filters = 1u;
        snprintf(st->reason, sizeof st->reason,
                 "job object active-process limit 1; CreateProcess fails (Windows; no syscall filter)");
    }
#endif
#ifdef SU_SANDBOX_X64
    {
        const char *env = getenv("SUTEKH_WORKER_SANDBOX");
        struct sock_filter prog[SU_SANDBOX_INSNS];
        struct sock_fprog fp;
        size_t len;
        int rc;
        if (env != NULL && strcmp(env, "0") == 0) {
#ifdef SU_SANDBOX_SANITIZED
            st->state = SU_SANDBOX_OPTED_OUT;
            snprintf(st->reason, sizeof st->reason,
                     "opted out by SUTEKH_WORKER_SANDBOX=0 (sanitizer build only)");
            return;
#endif
        }
        len = build_sandbox_filter(prog, sizeof prog / sizeof prog[0]);
        if (len == 0u) {
            st->errno_value = ENOSYS;
            snprintf(st->reason, sizeof st->reason, "not installed: filter table too small");
            return;
        }
        if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0) {
            st->errno_value = errno;
            snprintf(st->reason, sizeof st->reason, "not installed: no_new_privs refused: %s",
                     strerror(errno));
            return;
        }
        fp.len = (unsigned short)len;
        fp.filter = prog;
        rc = prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fp);
        if (rc != 0) {
            st->errno_value = errno;
            snprintf(st->reason, sizeof st->reason, "not installed: seccomp refused: %s", strerror(errno));
            return;
        }
        st->filters = 1u;
        /* Verify without side effects: execve of a missing path returns EPERM under the filter (the
         * filter decides before the path is looked up) and ENOENT when nothing denies it. */
        {
            char *argv[] = {NULL};
            char *envp[] = {NULL};
            (void)execve("/nonexistent/sutekh-sandbox-probe", argv, envp);
            if (errno == EPERM) {
                st->state = SU_SANDBOX_ON;
                snprintf(st->reason, sizeof st->reason,
                         "filter installed; execve, execveat, fork and vfork return EPERM; clone only with CLONE_THREAD");
            } else {
                st->errno_value = errno;
                st->filters = 0u;
                snprintf(st->reason, sizeof st->reason, "not enforced: execve probe returned %s", strerror(errno));
            }
        }
    }
#endif
}
