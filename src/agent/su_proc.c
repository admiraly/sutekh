#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "agent/su_proc.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sched.h>
#  include <signal.h>
#  include <sys/prctl.h>
#  include <sys/resource.h>
#  include <sys/syscall.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <time.h>
#  include <unistd.h>
#endif
#if defined(__linux__) && defined(__x86_64__)
#  include <linux/audit.h>
#  include <linux/filter.h>
#  include <linux/seccomp.h>
#  define SU_HAVE_SECCOMP_X64 1
#endif

#include "sutekh/status.h"

#if defined(_WIN32)
#include "agent/su_proc_win.inc"
#else

static void close_fd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void reap(pid_t pid) {
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

/* Non-blocking check for a child that has exited (killed, crashed or returned). An exited child
 * is reaped and its pipes are closed, as su_proc_kill does. Returns 1 when the child is gone. */
int su_proc_exited(SuProc *p) {
    int status;
    pid_t r;
    if (!p->alive || p->pid <= 0) {
        return 1;
    }
    do {
        r = waitpid(p->pid, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == 0) {
        return 0;
    }
    /* Already reaped by waitpid: close the pipes only, never signal a pid that may be reused. */
    close_fd(&p->in_fd);
    close_fd(&p->out_fd);
    su_line_reader_free(&p->reader);
    p->pid = 0;
    p->alive = 0;
    return 1;
}


/* Children (workers, disposable admission workers) get a scrubbed environment: only test seams
 * (SU_TEST_*), engine settings (SUTEKH_*) and sanitizer options are passed on. Everything else,
 * including credentials or tokens in the supervisor's environment, is dropped (constitution C07). */
extern char **environ;
static int env_allowed(const char *kv) {
    static const char *const prefixes[] = {"SU_TEST_", "SUTEKH_", "ASAN_OPTIONS=", "UBSAN_OPTIONS=",
                                           "LSAN_OPTIONS=", "MSAN_OPTIONS=", "TSAN_OPTIONS=", NULL};
    for (size_t i = 0; prefixes[i] != NULL; i++) {
        if (strncmp(kv, prefixes[i], strlen(prefixes[i])) == 0) {
            return 1;
        }
    }
    return 0;
}
static char **scrubbed_env(void) {
    size_t n = 0;
    char **out;
    size_t k = 0;
    if (environ != NULL) {
        while (environ[n] != NULL) {
            n++;
        }
    }
    out = calloc(n + 1u, sizeof *out);
    if (out == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if (env_allowed(environ[i])) {
            out[k++] = environ[i];
        }
    }
    out[k] = NULL;
    return out;
}

/* Records how a failed child ended. A child that is already gone (crash, exit) is reaped here and
 * its status kept; a child still running at grace_until_ms is left for su_proc_kill, and is
 * recorded as killed by the supervisor. */
static void end_record(SuProc *p, int64_t grace_until_ms) {
    p->end_kind = SU_PROC_END_SUPERVISOR;
    p->exit_code = 0;
    p->term_signal = 0;
    while (p->pid > 0) {
        int status = 0;
        pid_t r = waitpid(p->pid, &status, WNOHANG);
        if (r == p->pid) {
            if (WIFSIGNALED(status)) {
                p->end_kind = SU_PROC_END_SIGNALED;
                p->term_signal = WTERMSIG(status);
            } else if (WIFEXITED(status)) {
                p->end_kind = SU_PROC_END_EXITED;
                p->exit_code = WEXITSTATUS(status);
            }
            p->pid = 0;
            return;
        }
        if (r < 0 && errno != EINTR) {
            return;
        }
        if (su_now_ms() >= grace_until_ms) {
            return;
        }
        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 2000000L;
            nanosleep(&ts, NULL);
        }
    }
}

static void end_fail(SuProc *p, int64_t deadline_ms) {
    int64_t now = su_now_ms();
    /* A pipe that closed before the deadline means the child is exiting: give it a second to be
     * reaped so that its status is known. At the deadline there is no grace. */
    end_record(p, now < deadline_ms ? now + 1000 : now);
    su_proc_kill(p);
}

void su_proc_kill(SuProc *p) {
    if (p->pid > 0) {
        kill(p->pid, SIGKILL);
        reap(p->pid);
    }
    close_fd(&p->in_fd);
    close_fd(&p->out_fd);
    su_line_reader_free(&p->reader);
    p->pid = 0;
    p->alive = 0;
}

/* ---- containment (constitution C07) -----------------------------------------------------------
 * The child applies its limits between fork and exec. Only async-signal-safe calls run there
 * (setrlimit, prctl, signal, socket, unshare, write; no malloc, no stdio). It verifies what it
 * enforced and writes one fixed-size record to a close-on-exec status pipe. The supervisor reads
 * that record and gets EOF exactly when the child has exec'd (or exited without exec). */
#define SU_PROC_WIRE_MAGIC 0x53554350u /* "SUCP" */

typedef struct ContainWire {
    uint32_t magic;
    uint32_t net_mode;      /* SU_PROC_NET_* as verified in the child */
    uint32_t seccomp_tried; /* 1 when the seccomp route was attempted */
    int32_t seccomp_errno;  /* 0 when the filter is installed and verified */
    uint32_t netns_tried;   /* 1 when unshare(CLONE_NEWNET) was attempted */
    int32_t netns_errno;    /* 0 when the namespace was created */
    uint64_t rl_as;         /* getrlimit read-back, soft limits (RLIM_INFINITY = UINT64_MAX) */
    uint64_t rl_cpu;
    uint64_t rl_fsize;
    uint64_t rl_nofile;
    uint32_t fs_tried;      /* 1 when the Landlock route was attempted */
    uint32_t fs_mode;       /* SU_PROC_FS_* */
    int32_t fs_abi;         /* Landlock ABI reported by the kernel, 0 when unavailable */
    int32_t fs_errno;       /* errno of the step that failed, 0 when applied */
} ContainWire;

static uint64_t rl_soft(int resource) {
    struct rlimit rl;
    if (getrlimit(resource, &rl) != 0) {
        return SU_PROC_UNLIMITED - 1u; /* distinct from unlimited: the read-back failed */
    }
    return rl.rlim_cur == RLIM_INFINITY ? SU_PROC_UNLIMITED : (uint64_t)rl.rlim_cur;
}

#ifdef SU_HAVE_SECCOMP_X64
/* x86-64 syscall numbers (checked against asm/unistd_64.h). The network filter returns EPERM for:
 * every call that creates, binds, connects or accepts a socket (socket 41, connect 42, accept 43, bind 49,
 * listen 50, socketpair 53, accept4 288), and every io_uring entry point (io_uring_setup 425,
 * io_uring_enter 426, io_uring_register 427), because io_uring opens sockets (IORING_OP_SOCKET) without
 * calling socket(2). It also returns EPERM for kernel interfaces a sandboxed child has no use for:
 * process inspection and injection (ptrace 101, process_vm_readv 310, process_vm_writev 311, kcmp 312),
 * kernel keys (keyctl 250, add_key 248, request_key 249), BPF and perf (bpf 321, perf_event_open 298),
 * userfaultfd 323, namespaces, mounts and roots (mount 165, umount2 166, pivot_root 155, chroot 161,
 * setns 308, unshare 272, open_by_handle_at 304, name_to_handle_at 303), kernel images and modules
 * (kexec_load 246, init_module 175, finit_module 313, delete_module 176), reboot 169, swap 167/168 and
 * acct 163. personality is not denied: glibc and the sanitizers depend on it.
 * Any architecture other than x86-64 is killed, and so is any x32 syscall (nr >= 0x40000000). */
static const uint32_t g_denied_nr[] = {
    41u,  42u,  43u,  49u,  50u,  53u,  288u,            /* socket family */
    425u, 426u, 427u,                                     /* io_uring */
    101u, 310u, 311u, 312u,                               /* process inspection and injection */
    248u, 249u, 250u, 321u, 298u, 323u,                   /* keys, bpf, perf, userfaultfd */
    155u, 161u, 165u, 166u, 272u, 303u, 304u, 308u,       /* roots, mounts, namespaces, handles */
    163u, 167u, 168u, 169u, 175u, 176u, 246u, 313u};      /* accounting, swap, reboot, modules, kexec */
#define SU_DENIED_NR_COUNT (sizeof g_denied_nr / sizeof g_denied_nr[0])

/* Builds the filter into prog (cap instructions). Returns its length, 0 when cap is too small.
 * Layout: 0 load arch; 1 arch == x86-64 ? skip to 3 : 2; 2 kill (wrong arch); 3 load nr;
 * 4 nr >= 0x40000000 ? kill (x32); 5..5+n-1 nr == denied ? errno; 5+n allow; 5+n+1 errno(EPERM);
 * 5+n+2 kill. */
static size_t build_socket_filter(struct sock_filter *prog, size_t cap) {
    const size_t n = SU_DENIED_NR_COUNT;
    const size_t allow_at = 5u + n;
    const size_t errno_at = allow_at + 1u;
    const size_t kill_at = errno_at + 1u;
    size_t i = 0;
    size_t k;
    if (cap < kill_at + 1u) {
        return 0;
    }
    prog[i++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 4u); /* seccomp_data.arch */
    prog[i++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1u, 0u);
    prog[i++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS); /* wrong arch */
    prog[i++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0u); /* seccomp_data.nr */
    prog[i] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u,
                                           (unsigned char)(kill_at - i - 1u), 0u); /* x32 */
    i++;
    for (k = 0; k < n; k++) {
        prog[i] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, g_denied_nr[k],
                                               (unsigned char)(errno_at - i - 1u), 0u);
        i++;
    }
    prog[i++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    prog[i++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    prog[i++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    return i;
}

/* Installs the filter and verifies that socket(2) now fails with EPERM. Returns 0 when enforced,
 * otherwise an errno (1 when the probe socket was created anyway, which means not enforced). */
static int install_socket_filter(void) {
    struct sock_filter prog[8u + SU_DENIED_NR_COUNT];
    struct sock_fprog fp;
    size_t len = build_socket_filter(prog, sizeof prog / sizeof prog[0]);
    int fd;
    if (len == 0u) {
        return ENOSYS;
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0) {
        return errno;
    }
    fp.len = (unsigned short)len;
    fp.filter = prog;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fp) != 0) {
        return errno;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        close(fd);
        return 1;
    }
    return errno == EPERM ? 0 : errno;
}
#endif

/* ---- Landlock write denial (Linux, raw syscalls; no library) ----------------------------------
 * A ruleset that handles every write-class access and grants none (except one optional test
 * directory) denies those accesses everywhere. Reads are not handled, so they stay allowed. The
 * ruleset is applied in the child before exec and survives exec. The child reports the outcome of
 * landlock_restrict_self; the containment tests then check the denials from inside the worker. */
#if defined(__linux__)
#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#endif
#ifndef SYS_landlock_add_rule
#define SYS_landlock_add_rule 445
#endif
#ifndef SYS_landlock_restrict_self
#define SYS_landlock_restrict_self 446
#endif
#define SU_LL_CREATE_VERSION_FLAG (1u << 0)
#define SU_LL_RULE_PATH_BENEATH 1
#define SU_LL_FS_WRITE_FILE (1ull << 1)
#define SU_LL_FS_REMOVE_DIR (1ull << 4)
#define SU_LL_FS_REMOVE_FILE (1ull << 5)
#define SU_LL_FS_MAKE_CHAR (1ull << 6)
#define SU_LL_FS_MAKE_DIR (1ull << 7)
#define SU_LL_FS_MAKE_REG (1ull << 8)
#define SU_LL_FS_MAKE_SOCK (1ull << 9)
#define SU_LL_FS_MAKE_FIFO (1ull << 10)
#define SU_LL_FS_MAKE_BLOCK (1ull << 11)
#define SU_LL_FS_MAKE_SYM (1ull << 12)
#define SU_LL_FS_REFER (1ull << 13)    /* ABI 2 */
#define SU_LL_FS_TRUNCATE (1ull << 14) /* ABI 3 */

struct su_ll_ruleset_attr {
    uint64_t handled_access_fs;
};
struct __attribute__((packed)) su_ll_path_beneath {
    uint64_t allowed_access;
    int32_t parent_fd;
};

/* ABI version the kernel reports, or -errno when Landlock is unavailable. Child only. */
static long child_landlock_abi(void) {
    long r = syscall(SYS_landlock_create_ruleset, NULL, 0, SU_LL_CREATE_VERSION_FLAG);
    return r < 0 ? -(long)errno : r;
}

/* Child only, between fork and exec. Returns 1 when the ruleset is applied, else 0 with *err set. */
static int child_landlock(const char *writable_dir, int32_t *abi_out, int *err_out) {
    struct su_ll_ruleset_attr attr;
    uint64_t handled;
    long abi = child_landlock_abi();
    long rs;
    *abi_out = 0;
    *err_out = 0;
    if (abi <= 0) {
        *err_out = abi < 0 ? (int)-abi : ENOSYS;
        return 0;
    }
    *abi_out = (int32_t)abi;
    handled = SU_LL_FS_WRITE_FILE | SU_LL_FS_REMOVE_DIR | SU_LL_FS_REMOVE_FILE | SU_LL_FS_MAKE_CHAR |
              SU_LL_FS_MAKE_DIR | SU_LL_FS_MAKE_REG | SU_LL_FS_MAKE_SOCK | SU_LL_FS_MAKE_FIFO |
              SU_LL_FS_MAKE_BLOCK | SU_LL_FS_MAKE_SYM;
    if (abi >= 2) {
        handled |= SU_LL_FS_REFER;
    }
    if (abi >= 3) {
        handled |= SU_LL_FS_TRUNCATE;
    }
    attr.handled_access_fs = handled;
    rs = syscall(SYS_landlock_create_ruleset, &attr, sizeof attr, 0);
    if (rs < 0) {
        *err_out = errno;
        return 0;
    }
    if (writable_dir != NULL) {
        struct su_ll_path_beneath pb;
        int dfd = open(writable_dir, O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (dfd < 0) {
            *err_out = errno;
            close((int)rs);
            return 0;
        }
        pb.allowed_access = handled;
        pb.parent_fd = dfd;
        if (syscall(SYS_landlock_add_rule, (int)rs, SU_LL_RULE_PATH_BENEATH, &pb, 0) != 0) {
            *err_out = errno;
            close(dfd);
            close((int)rs);
            return 0;
        }
        close(dfd);
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0) {
        *err_out = errno;
        close((int)rs);
        return 0;
    }
    if (syscall(SYS_landlock_restrict_self, (int)rs, 0u) != 0) {
        *err_out = errno;
        close((int)rs);
        return 0;
    }
    close((int)rs);
    return 1;
}
#else
static int child_landlock(const char *writable_dir, int32_t *abi_out, int *err_out) {
    (void)writable_dir;
    *abi_out = 0;
    *err_out = ENOSYS;
    return 0;
}
#endif

static void write_wire(int fd, const ContainWire *w) {
    const uint8_t *p = (const uint8_t *)w;
    size_t left = sizeof *w;
    while (left > 0u) {
        ssize_t r = write(fd, p, left);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        p += r;
        left -= (size_t)r;
    }
}

/* Child only, between fork and exec. */
static void child_contain(const SuProcLimits *limits, int status_fd) {
    ContainWire w;
    memset(&w, 0, sizeof w);
    w.magic = SU_PROC_WIRE_MAGIC;
    w.net_mode = SU_PROC_NET_NONE;
    if (limits != NULL && limits->address_space_bytes > 0) {
        struct rlimit rl;
        rl.rlim_cur = (rlim_t)limits->address_space_bytes;
        rl.rlim_max = (rlim_t)limits->address_space_bytes;
        setrlimit(RLIMIT_AS, &rl);
    }
    if (limits != NULL && limits->cpu_seconds > 0) {
        struct rlimit rl;
        rl.rlim_cur = (rlim_t)limits->cpu_seconds;
        rl.rlim_max = (rlim_t)limits->cpu_seconds + 1u;
        setrlimit(RLIMIT_CPU, &rl);
    }
    if (limits != NULL && limits->file_bytes > 0) {
        struct rlimit rl;
        rl.rlim_cur = (rlim_t)limits->file_bytes;
        rl.rlim_max = (rlim_t)limits->file_bytes;
        setrlimit(RLIMIT_FSIZE, &rl);
        /* Over-limit writes fail with EFBIG; SIGXFSZ (default: terminate) is ignored. The
         * disposition survives exec, so the worker sees the error, not a signal. */
        signal(SIGXFSZ, SIG_IGN);
    }
    if (limits != NULL && limits->nofile > 0) {
        struct rlimit rl;
        rl.rlim_cur = (rlim_t)limits->nofile;
        rl.rlim_max = (rlim_t)limits->nofile;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    {
        struct rlimit core;
        core.rlim_cur = 0;
        core.rlim_max = 0;
        setrlimit(RLIMIT_CORE, &core);
    }
    if (limits != NULL && limits->deny_network) {
#ifdef SU_HAVE_SECCOMP_X64
        int e;
        w.seccomp_tried = 1u;
        e = install_socket_filter();
        w.seccomp_errno = e;
        if (e == 0) {
            w.net_mode = SU_PROC_NET_SECCOMP;
        }
#else
        w.seccomp_tried = 0u;
        w.seccomp_errno = ENOSYS;
#endif
        if (w.net_mode != SU_PROC_NET_SECCOMP) {
            w.netns_tried = 1u;
            if (unshare(CLONE_NEWNET) == 0) {
                w.net_mode = SU_PROC_NET_NETNS;
                w.netns_errno = 0;
            } else {
                w.netns_errno = errno;
            }
        }
    }
    if (limits != NULL && limits->deny_fs_writes) {
        int32_t abi = 0;
        int e = 0;
        w.fs_tried = 1u;
        if (child_landlock(limits->writable_dir, &abi, &e)) {
            w.fs_mode = SU_PROC_FS_LANDLOCK;
        }
        w.fs_abi = abi;
        w.fs_errno = e;
    }
    w.rl_as = rl_soft(RLIMIT_AS);
    w.rl_cpu = rl_soft(RLIMIT_CPU);
    w.rl_fsize = rl_soft(RLIMIT_FSIZE);
    w.rl_nofile = rl_soft(RLIMIT_NOFILE);
    write_wire(status_fd, &w);
}

/* Parent: reads the child's status record up to EOF and closes the status pipe. */
static void read_containment(SuProc *p, int status_fd, const SuProcLimits *limits) {
    ContainWire w;
    uint8_t *dst = (uint8_t *)&w;
    size_t got = 0;
    memset(&w, 0, sizeof w);
    memset(&p->containment, 0, sizeof p->containment);
    p->containment.net_mode = SU_PROC_NET_NONE;
    /* The child always writes its record (even with no limits), so this side always drains the pipe.
     * Closing it early would give the child SIGPIPE before exec. */
    while (got < sizeof w) {
        ssize_t r = read(status_fd, dst + got, sizeof w - got);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    close(status_fd);
    if (limits == NULL) {
        snprintf(p->containment.reason, sizeof p->containment.reason, "no containment requested");
        return;
    }
    if (got != sizeof w || w.magic != SU_PROC_WIRE_MAGIC) {
        snprintf(p->containment.reason, sizeof p->containment.reason,
                 "child status not received: the child did not reach exec");
        return;
    }
    p->containment.reported = 1;
    p->containment.rlimit_as = w.rl_as;
    p->containment.rlimit_cpu = w.rl_cpu;
    p->containment.rlimit_fsize = w.rl_fsize;
    p->containment.rlimit_nofile = w.rl_nofile;
    p->containment.net_mode = w.net_mode;
    p->containment.fs_mode = w.fs_mode;
    p->containment.fs_abi = w.fs_abi;
    if (!limits->deny_fs_writes) {
        snprintf(p->containment.fs_reason, sizeof p->containment.fs_reason, "file writes not restricted (not requested)");
    } else if (w.fs_mode == SU_PROC_FS_LANDLOCK) {
        snprintf(p->containment.fs_reason, sizeof p->containment.fs_reason,
                 "landlock ABI %d applied: file creation, writes, removal and truncation denied; reads not restricted%s",
                 (int)w.fs_abi, limits->writable_dir != NULL ? " (test seam: one directory writable)" : "");
    } else {
        snprintf(p->containment.fs_reason, sizeof p->containment.fs_reason,
                 "file writes not restricted: landlock %s", strerror(w.fs_errno));
    }
    if (!limits->deny_network) {
        snprintf(p->containment.reason, sizeof p->containment.reason, "network denial not requested");
    } else if (w.net_mode == SU_PROC_NET_SECCOMP) {
        snprintf(p->containment.reason, sizeof p->containment.reason,
                 "seccomp filter installed; socket creation denied with EPERM (verified in the child)");
    } else if (w.net_mode == SU_PROC_NET_NETNS) {
        snprintf(p->containment.reason, sizeof p->containment.reason,
                 "seccomp not enforced (%s); network namespace created",
                 w.seccomp_tried ? strerror(w.seccomp_errno) : "not x86-64 Linux");
    } else {
        snprintf(p->containment.reason, sizeof p->containment.reason,
                 "no network denial enforced: seccomp %s; unshare(CLONE_NEWNET) %s",
                 w.seccomp_tried ? strerror(w.seccomp_errno) : "not available on this platform",
                 strerror(w.netns_errno));
    }
}

uint32_t su_proc_spawn(SuProc *p, const char *const *argv, const SuProcLimits *limits) {
    int pin[2] = {-1, -1};
    int pout[2] = {-1, -1};
    int pst[2] = {-1, -1};
    size_t argc = 0;
    pid_t pid;

    p->pid = 0;
    p->in_fd = -1;
    p->out_fd = -1;
    p->alive = 0;
    p->end_kind = SU_PROC_END_NONE;
    p->exit_code = 0;
    p->term_signal = 0;
    memset(&p->reader, 0, sizeof p->reader);
    memset(&p->containment, 0, sizeof p->containment);

    if (argv == NULL || argv[0] == NULL || strchr(argv[0], '/') == NULL) {
        return SU_E_WORKER_FAILED;
    }
    while (argv[argc] != NULL) {
        argc++;
        if (argc > SU_PROC_MAX_ARGS) {
            return SU_E_WORKER_FAILED;
        }
    }
    if (argc == 0 || access(argv[0], X_OK) != 0) {
        return SU_E_WORKER_FAILED;
    }
    if (pipe(pin) != 0) {
        return SU_E_WORKER_FAILED;
    }
    if (pipe(pout) != 0) {
        close(pin[0]);
        close(pin[1]);
        return SU_E_WORKER_FAILED;
    }
    if (pipe(pst) != 0) {
        close(pin[0]);
        close(pin[1]);
        close(pout[0]);
        close(pout[1]);
        return SU_E_WORKER_FAILED;
    }
    /* Parent-side ends and every pipe end are close-on-exec, so a later child does not inherit
     * another worker's pipes. dup2 in the child clears it. The status pipe's write end stays open
     * in the child until its exec, so the parent sees EOF exactly then. */
    fcntl(pin[0], F_SETFD, FD_CLOEXEC);
    fcntl(pin[1], F_SETFD, FD_CLOEXEC);
    fcntl(pout[0], F_SETFD, FD_CLOEXEC);
    fcntl(pout[1], F_SETFD, FD_CLOEXEC);
    fcntl(pst[0], F_SETFD, FD_CLOEXEC);
    fcntl(pst[1], F_SETFD, FD_CLOEXEC);

    char **child_env = scrubbed_env();
    if (child_env == NULL) {
        close(pin[0]);
        close(pin[1]);
        close(pout[0]);
        close(pout[1]);
        close(pst[0]);
        close(pst[1]);
        return SU_E_RESOURCE_LIMIT;
    }
    pid = fork();
    if (pid < 0) {
        free(child_env);
        close(pin[0]);
        close(pin[1]);
        close(pout[0]);
        close(pout[1]);
        close(pst[0]);
        close(pst[1]);
        return SU_E_WORKER_FAILED;
    }
    if (pid == 0) {
        /* Child: only async-signal-safe calls until exec. */
        int a = fcntl(pin[0], F_DUPFD, 10);
        int b = fcntl(pout[1], F_DUPFD, 10);
        if (a < 0 || b < 0) {
            _exit(127);
        }
        if (dup2(a, 0) < 0 || dup2(b, 1) < 0) {
            _exit(127);
        }
        /* The worker must hold only stdin/stdout: close every other pipe end so it never keeps
         * its own stdin writer or the supervisor's read end alive (EOF would never arrive). */
        close(a);
        close(b);
        if (pin[0] > 2) close(pin[0]);
        if (pin[1] > 2) close(pin[1]);
        if (pout[0] > 2) close(pout[0]);
        if (pout[1] > 2) close(pout[1]);
        if (pst[0] > 2) close(pst[0]);
        child_contain(limits, pst[1]);
        execve(argv[0], (char *const *)argv, child_env);
        _exit(127);
    }
    free(child_env);

    close(pin[0]);
    close(pout[1]);
    close(pst[1]);
    read_containment(p, pst[0], limits);
    p->pid = pid;
    p->in_fd = pin[1];
    p->out_fd = pout[0];
    p->alive = 1;
    su_line_reader_init(&p->reader, p->out_fd, -1);
    return SU_OK;
}
#endif /* !_WIN32 */

uint32_t su_proc_request(SuProc *p, const uint8_t *req, size_t req_len, int64_t deadline_ms,
                         const uint8_t **resp, size_t *resp_len) {
    const uint8_t *line = NULL;
    size_t len = 0;
    int overflow = 0;
    int rc;
    uint8_t *framed;

    *resp = NULL;
    *resp_len = 0;
    if (!p->alive) {
        return SU_E_WORKER_FAILED;
    }
    if (req_len == SIZE_MAX) {
        su_proc_kill(p);
        return SU_E_RESOURCE_LIMIT;
    }
    framed = malloc(req_len + 1u);
    if (framed == NULL) {
        su_proc_kill(p);
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(framed, req, req_len);
    framed[req_len] = '\n';
    rc = su_write_all(p->in_fd, framed, req_len + 1u, deadline_ms);
    free(framed);
    if (rc != 0) {
        end_fail(p, deadline_ms);
        return SU_E_WORKER_FAILED;
    }

    p->reader.deadline_ms = deadline_ms;
    rc = su_line_next(&p->reader, &line, &len, &overflow);
    if (rc != 1) {
        end_fail(p, deadline_ms);
        return SU_E_WORKER_FAILED;
    }
    if (overflow) {
        p->end_kind = SU_PROC_END_SUPERVISOR;
        su_proc_kill(p);
        return SU_E_RESOURCE_LIMIT;
    }
    *resp = line;
    *resp_len = len;
    return SU_OK;
}

#if !defined(_WIN32)
void su_proc_finish(SuProc *p, int64_t grace_ms) {
    int64_t until;
    if (p->pid <= 0) {
        return;
    }
    close_fd(&p->in_fd);
    until = su_now_ms() + grace_ms;
    for (;;) {
        int status;
        pid_t r = waitpid(p->pid, &status, WNOHANG);
        if (r == p->pid) {
            p->pid = 0;
            break;
        }
        if (r < 0 && errno != EINTR) {
            p->pid = 0;
            break;
        }
        if (su_now_ms() >= until) {
            break;
        }
        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 5000000L;
            nanosleep(&ts, NULL);
        }
    }
    su_proc_kill(p);
}
#endif
