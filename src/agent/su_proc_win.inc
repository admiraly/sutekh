/* Windows half of su_proc.c (included from there inside #if defined(_WIN32); not a translation unit).
 * See su_proc.h for the contract and the containment differences from POSIX. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */

static void close_fd(int *fd) {
    if (*fd >= 0) {
        _close(*fd);
        *fd = -1;
    }
}

static void close_handle(void **h) {
    if (*h != NULL) {
        CloseHandle((HANDLE)*h);
        *h = NULL;
    }
}

/* POSIX signal numbers for the exception statuses a crashed child ends with. 0 for any other status. */
static int exception_signal(DWORD code) {
    switch (code) {
    case 0xC0000005u: /* STATUS_ACCESS_VIOLATION */
    case 0xC00000FDu: /* STATUS_STACK_OVERFLOW */
    case 0xC0000006u: /* STATUS_IN_PAGE_ERROR */
        return 11;    /* SIGSEGV */
    case 0xC000001Du: /* STATUS_ILLEGAL_INSTRUCTION */
    case 0xC0000096u: /* STATUS_PRIVILEGED_INSTRUCTION */
        return 4;     /* SIGILL */
    case 0xC0000094u: /* STATUS_INTEGER_DIVIDE_BY_ZERO */
    case 0xC0000095u: /* STATUS_INTEGER_OVERFLOW */
    case 0xC000008Eu: /* STATUS_FLOAT_DIVIDE_BY_ZERO */
        return 8;     /* SIGFPE */
    case 0xC0000409u: /* STATUS_STACK_BUFFER_OVERRUN: __fastfail, which abort() uses under a job */
    case 0x80000003u: /* STATUS_BREAKPOINT */
        return 6;     /* SIGABRT */
    default:
        return 0;
    }
}

/* Records the end of a process that has exited (its exit status is final). */
static void record_exit(SuProc *p) {
    DWORD code = 0;
    int sig;
    if (!GetExitCodeProcess((HANDLE)p->process, &code)) {
        return;
    }
    sig = exception_signal(code);
    if (sig != 0) {
        p->end_kind = SU_PROC_END_SIGNALED;
        p->term_signal = sig;
    } else {
        p->end_kind = SU_PROC_END_EXITED;
        p->exit_code = (int)code;
    }
}

/* Releases the process: closes the job (which kills anything still running in it), the process
 * handle and the pipes. */
static void release(SuProc *p) {
    if (p->process != NULL) {
        (void)TerminateProcess((HANDLE)p->process, 1u);
        (void)WaitForSingleObject((HANDLE)p->process, 5000u);
    }
    close_handle(&p->job);
    close_handle(&p->process);
    close_fd(&p->in_fd);
    close_fd(&p->out_fd);
    su_line_reader_free(&p->reader);
    p->pid = 0;
    p->alive = 0;
}

int su_proc_exited(SuProc *p) {
    if (!p->alive || p->process == NULL) {
        return 1;
    }
    if (WaitForSingleObject((HANDLE)p->process, 0) != WAIT_OBJECT_0) {
        return 0;
    }
    release(p);
    return 1;
}

static void end_record(SuProc *p, int64_t grace_until_ms) {
    int64_t rem;
    p->end_kind = SU_PROC_END_SUPERVISOR;
    p->exit_code = 0;
    p->term_signal = 0;
    if (p->process == NULL) {
        return;
    }
    rem = grace_until_ms - su_now_ms();
    if (WaitForSingleObject((HANDLE)p->process, rem > 0 ? (DWORD)rem : 0u) == WAIT_OBJECT_0) {
        record_exit(p);
    }
}

static void end_fail(SuProc *p, int64_t deadline_ms) {
    int64_t now = su_now_ms();
    end_record(p, now < deadline_ms ? now + 1000 : now);
    su_proc_kill(p);
}

void su_proc_kill(SuProc *p) {
    release(p);
}

void su_proc_finish(SuProc *p, int64_t grace_ms) {
    if (p->process == NULL) {
        return;
    }
    close_fd(&p->in_fd);
    (void)WaitForSingleObject((HANDLE)p->process, grace_ms > 0 ? (DWORD)grace_ms : 0u);
    su_proc_kill(p);
}

/* Children get a scrubbed environment: test seams (SU_TEST_*), engine settings (SUTEKH_*), sanitizer
 * options, and the few system variables a Windows process needs to load system DLLs and find its
 * temporary directory (SystemRoot, SystemDrive, windir, TEMP, TMP). Names compare case-insensitively,
 * as Windows does. Everything else is dropped (constitution C07). */
static int env_allowed_w(const wchar_t *kv) {
    static const wchar_t *const prefixes[] = {L"SU_TEST_", L"SUTEKH_", L"ASAN_OPTIONS=", L"UBSAN_OPTIONS=",
                                              L"LSAN_OPTIONS=", L"SystemRoot=", L"SystemDrive=", L"windir=",
                                              L"TEMP=", L"TMP=", NULL};
    size_t i;
    for (i = 0; prefixes[i] != NULL; i++) {
        if (_wcsnicmp(kv, prefixes[i], wcslen(prefixes[i])) == 0) {
            return 1;
        }
    }
    return 0;
}

static int env_cmp(const void *a, const void *b) {
    return _wcsicmp(*(const wchar_t *const *)a, *(const wchar_t *const *)b);
}

/* Builds a sorted, double-NUL-terminated UTF-16 environment block. NULL on allocation failure. */
static wchar_t *scrubbed_env_block(void) {
    wchar_t *src = GetEnvironmentStringsW();
    const wchar_t **keep = NULL;
    wchar_t *block = NULL;
    const wchar_t *s;
    size_t n = 0;
    size_t k = 0;
    size_t chars = 1u;
    size_t i;

    if (src == NULL) {
        return NULL;
    }
    for (s = src; *s != L'\0'; s += wcslen(s) + 1u) {
        n++;
    }
    keep = calloc(n > 0u ? n : 1u, sizeof *keep);
    if (keep == NULL) {
        FreeEnvironmentStringsW(src);
        return NULL;
    }
    for (s = src; *s != L'\0'; s += wcslen(s) + 1u) {
        if (s[0] != L'=' && env_allowed_w(s)) {
            keep[k++] = s;
            chars += wcslen(s) + 1u;
        }
    }
    qsort(keep, k, sizeof *keep, env_cmp);
    block = malloc((chars + 1u) * sizeof *block);
    if (block != NULL) {
        wchar_t *d = block;
        for (i = 0; i < k; i++) {
            size_t len = wcslen(keep[i]) + 1u;
            memcpy(d, keep[i], len * sizeof *d);
            d += len;
        }
        *d++ = L'\0';
        if (k == 0u) {
            *d = L'\0';
        }
    }
    free(keep);
    FreeEnvironmentStringsW(src);
    return block;
}

/* UTF-8 to a malloc'd UTF-16 string, NULL on failure. */
static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) {
        return NULL;
    }
    w = malloc((size_t)n * sizeof *w);
    if (w != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) <= 0) {
        free(w);
        w = NULL;
    }
    return w;
}

/* Appends one argument quoted by the rules CommandLineToArgvW and the CRT use: quotes around every
 * argument, backslashes doubled only before a quote, embedded quotes escaped. */
static void append_arg(wchar_t *dst, size_t *pos, const wchar_t *arg) {
    const wchar_t *c;
    dst[(*pos)++] = L'"';
    for (c = arg;; c++) {
        size_t bs = 0;
        while (*c == L'\\') {
            bs++;
            c++;
        }
        if (*c == L'\0') {
            while (bs-- > 0u) { /* double before the closing quote */
                dst[(*pos)++] = L'\\';
                dst[(*pos)++] = L'\\';
            }
            break;
        }
        if (*c == L'"') {
            while (bs-- > 0u) {
                dst[(*pos)++] = L'\\';
                dst[(*pos)++] = L'\\';
            }
            dst[(*pos)++] = L'\\';
            dst[(*pos)++] = L'"';
        } else {
            while (bs-- > 0u) {
                dst[(*pos)++] = L'\\';
            }
            dst[(*pos)++] = *c;
        }
    }
    dst[(*pos)++] = L'"';
}

/* Command line for argv, NULL on failure. */
static wchar_t *command_line(const char *const *argv, size_t argc) {
    wchar_t *wargs[SU_PROC_MAX_ARGS];
    wchar_t *cmd = NULL;
    size_t need = 1u;
    size_t pos = 0;
    size_t i;
    memset(wargs, 0, sizeof wargs);
    for (i = 0; i < argc; i++) {
        wargs[i] = widen(argv[i]);
        if (wargs[i] == NULL) {
            goto out;
        }
        need += 2u * wcslen(wargs[i]) + 4u; /* worst case: every char escaped, two quotes, a space */
    }
    cmd = malloc(need * sizeof *cmd);
    if (cmd == NULL) {
        goto out;
    }
    for (i = 0; i < argc; i++) {
        if (i > 0u) {
            cmd[pos++] = L' ';
        }
        append_arg(cmd, &pos, wargs[i]);
    }
    cmd[pos] = L'\0';
out:
    for (i = 0; i < argc; i++) {
        free(wargs[i]);
    }
    return cmd;
}

/* One pipe: the supervisor end is a named pipe server opened for overlapped I/O (so su_proto can wait
 * with a deadline and cancel); the child end is a synchronous, inheritable client handle. The pipe
 * accepts one instance, refuses remote clients and is connected before this returns, so no other
 * process can attach to it. outbound: the supervisor writes (child stdin). */
static int make_pipe(int outbound, HANDLE *server, HANDLE *client) {
    static volatile LONG seq;
    wchar_t name[96];
    SECURITY_ATTRIBUTES sa;
    DWORD mode = (outbound ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) | FILE_FLAG_OVERLAPPED |
                 FILE_FLAG_FIRST_PIPE_INSTANCE;
    _snwprintf(name, sizeof name / sizeof name[0], L"\\\\.\\pipe\\sutekh-%lu-%ld-%llu",
               (unsigned long)GetCurrentProcessId(), (long)InterlockedIncrement(&seq),
               (unsigned long long)GetTickCount64());
    name[sizeof name / sizeof name[0] - 1u] = L'\0';
    *server = CreateNamedPipeW(name, mode, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                               1u, 65536u, 65536u, 0u, NULL);
    if (*server == INVALID_HANDLE_VALUE) {
        *server = NULL;
        return 0;
    }
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    *client = CreateFileW(name, outbound ? GENERIC_READ : GENERIC_WRITE, 0u, &sa, OPEN_EXISTING, 0u, NULL);
    if (*client == INVALID_HANDLE_VALUE) {
        CloseHandle(*server);
        *server = NULL;
        *client = NULL;
        return 0;
    }
    /* The client opened synchronously, so the server end is already connected: ConnectNamedPipe on an
     * overlapped handle reports ERROR_PIPE_CONNECTED. Anything else means the pipe is not ours. */
    {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        if (!ConnectNamedPipe(*server, &ov) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(*client);
            CloseHandle(*server);
            *server = NULL;
            *client = NULL;
            return 0;
        }
    }
    return 1;
}

/* A primary token like ours at low mandatory integrity. Under "no write up" the child cannot create,
 * write, rename or delete files and directories that carry the default (medium) label, which is every
 * ordinary file of the user; reads are allowed. NULL on failure. */
static HANDLE low_integrity_token(DWORD *err) {
    HANDLE self = NULL;
    HANDLE tok = NULL;
    SID_IDENTIFIER_AUTHORITY auth = SECURITY_MANDATORY_LABEL_AUTHORITY;
    PSID low = NULL;
    TOKEN_MANDATORY_LABEL tml;
    *err = 0;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY, &self) ||
        !DuplicateTokenEx(self, TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE, NULL,
                          SecurityImpersonation, TokenPrimary, &tok) ||
        !AllocateAndInitializeSid(&auth, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0, &low)) {
        *err = GetLastError();
        goto fail;
    }
    memset(&tml, 0, sizeof tml);
    tml.Label.Attributes = SE_GROUP_INTEGRITY;
    tml.Label.Sid = low;
    if (!SetTokenInformation(tok, TokenIntegrityLevel, &tml, (DWORD)(sizeof tml + GetLengthSid(low)))) {
        *err = GetLastError();
        goto fail;
    }
    FreeSid(low);
    CloseHandle(self);
    return tok;
fail:
    if (low != NULL) {
        FreeSid(low);
    }
    if (tok != NULL) {
        CloseHandle(tok);
    }
    if (self != NULL) {
        CloseHandle(self);
    }
    return NULL;
}

/* Integrity RID of a running process's token, or 0 when it cannot be read. */
static DWORD process_integrity(HANDLE process) {
    HANDLE tok = NULL;
    uint8_t buf[128];
    DWORD len = 0;
    DWORD rid = 0;
    if (!OpenProcessToken(process, TOKEN_QUERY, &tok)) {
        return 0;
    }
    if (GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof buf, &len)) {
        const TOKEN_MANDATORY_LABEL *l = (const TOKEN_MANDATORY_LABEL *)(const void *)buf;
        PUCHAR cnt = GetSidSubAuthorityCount(l->Label.Sid);
        if (cnt != NULL && *cnt > 0u) {
            rid = *GetSidSubAuthority(l->Label.Sid, (DWORD)(*cnt - 1u));
        }
    }
    CloseHandle(tok);
    return rid;
}

/* Reads back the limits the job enforces and fills the containment record. */
static void job_containment(SuProc *p, const SuProcLimits *limits, int fs_low, DWORD fs_err) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    SuProcContainment *c = &p->containment;
    memset(c, 0, sizeof *c);
    c->net_mode = SU_PROC_NET_NONE;
    if (limits == NULL) {
        snprintf(c->reason, sizeof c->reason, "no containment requested");
        return;
    }
    memset(&li, 0, sizeof li);
    if (!QueryInformationJobObject((HANDLE)p->job, JobObjectExtendedLimitInformation, &li, sizeof li, NULL)) {
        snprintf(c->reason, sizeof c->reason, "job limits could not be read back (Windows error %lu)",
                 (unsigned long)GetLastError());
        return;
    }
    c->reported = 1;
    c->rlimit_as = (li.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) != 0u
                       ? (uint64_t)li.ProcessMemoryLimit
                       : SU_PROC_UNLIMITED;
    c->rlimit_cpu = (li.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_TIME) != 0u
                        ? (uint64_t)li.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart / 10000000u
                        : SU_PROC_UNLIMITED;
    c->rlimit_fsize = SU_PROC_UNLIMITED; /* no per-process file-size limit on Windows */
    c->rlimit_nofile = SU_PROC_UNLIMITED; /* no per-process handle-count limit on Windows */
    c->fs_mode = fs_low ? SU_PROC_FS_LOW_INTEGRITY : SU_PROC_FS_NONE;
    if (!limits->deny_network) {
        snprintf(c->reason, sizeof c->reason, "network denial not requested");
    } else {
        snprintf(c->reason, sizeof c->reason,
                 "no network denial enforced: Windows has no seccomp or network namespace; an AppContainer "
                 "is not used");
    }
    if (!limits->deny_fs_writes) {
        snprintf(c->fs_reason, sizeof c->fs_reason, "file writes not restricted (not requested)");
    } else if (fs_low) {
        snprintf(c->fs_reason, sizeof c->fs_reason,
                 "low integrity token (verified on the child): writes to medium-labelled files and directories "
                 "denied; reads not restricted%s",
                 limits->writable_dir != NULL ? " (test seam directory not granted on Windows)" : "");
    } else {
        snprintf(c->fs_reason, sizeof c->fs_reason, "file writes not restricted: low integrity token failed "
                 "(Windows error %lu)", (unsigned long)fs_err);
    }
}

uint32_t su_proc_spawn(SuProc *p, const char *const *argv, const SuProcLimits *limits) {
    HANDLE in_srv = NULL;
    HANDLE in_cli = NULL;
    HANDLE out_srv = NULL;
    HANDLE out_cli = NULL;
    HANDLE err_h = NULL;
    HANDLE token = NULL;
    HANDLE job = NULL;
    HANDLE inherit[3];
    DWORD n_inherit = 0;
    wchar_t *exe = NULL;
    wchar_t *cmd = NULL;
    wchar_t *env = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    SIZE_T attr_size = 0;
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    DWORD attr;
    DWORD fs_err = 0;
    int fs_low = 0;
    size_t argc = 0;
    uint32_t st = SU_E_WORKER_FAILED;
    BOOL ok;

    p->pid = 0;
    p->process = NULL;
    p->job = NULL;
    p->in_fd = -1;
    p->out_fd = -1;
    p->alive = 0;
    p->end_kind = SU_PROC_END_NONE;
    p->exit_code = 0;
    p->term_signal = 0;
    memset(&p->reader, 0, sizeof p->reader);
    memset(&p->containment, 0, sizeof p->containment);
    memset(&pi, 0, sizeof pi);

    if (argv == NULL || argv[0] == NULL || (strchr(argv[0], '/') == NULL && strchr(argv[0], '\\') == NULL)) {
        return SU_E_WORKER_FAILED;
    }
    while (argv[argc] != NULL) {
        argc++;
        if (argc > SU_PROC_MAX_ARGS) {
            return SU_E_WORKER_FAILED;
        }
    }
    exe = widen(argv[0]);
    if (exe == NULL) {
        return SU_E_WORKER_FAILED;
    }
    attr = GetFileAttributesW(exe);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
        free(exe);
        return SU_E_WORKER_FAILED;
    }
    cmd = command_line(argv, argc);
    env = scrubbed_env_block();
    if (cmd == NULL || env == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        goto out;
    }
    if (!make_pipe(1, &in_srv, &in_cli) || !make_pipe(0, &out_srv, &out_cli)) {
        goto out;
    }
    /* stderr is shared with the supervisor, as on POSIX; a duplicate is made inheritable for the child. */
    {
        HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
        if (e != NULL && e != INVALID_HANDLE_VALUE &&
            !DuplicateHandle(GetCurrentProcess(), e, GetCurrentProcess(), &err_h, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            err_h = NULL;
        }
    }
    inherit[n_inherit++] = in_cli;
    inherit[n_inherit++] = out_cli;
    if (err_h != NULL) {
        inherit[n_inherit++] = err_h;
    }
    /* Only these handles reach the child, whatever else is inheritable in this process. */
    (void)InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    attrs = malloc(attr_size);
    if (attrs == NULL || !InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
        free(attrs);
        attrs = NULL;
        goto out;
    }
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, n_inherit * sizeof inherit[0],
                                   NULL, NULL)) {
        goto out;
    }
    job = CreateJobObjectW(NULL, NULL);
    if (job == NULL) {
        goto out;
    }
    memset(&li, 0, sizeof li);
    li.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    li.BasicLimitInformation.ActiveProcessLimit = 1u;
    if (limits != NULL && limits->address_space_bytes > 0u) {
        li.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        li.ProcessMemoryLimit = (SIZE_T)limits->address_space_bytes;
    }
    if (limits != NULL && limits->cpu_seconds > 0u) {
        li.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_TIME;
        li.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart = (LONGLONG)limits->cpu_seconds * 10000000LL;
    }
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li)) {
        goto out;
    }
    if (limits != NULL && limits->deny_fs_writes) {
        token = low_integrity_token(&fs_err);
    }

    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_cli;
    si.StartupInfo.hStdOutput = out_cli;
    si.StartupInfo.hStdError = err_h;
    si.lpAttributeList = attrs;
    /* Suspended until it is in the job, so it never runs a single instruction outside its limits. */
    if (token != NULL) {
        ok = CreateProcessAsUserW(token, exe, cmd, NULL, NULL, TRUE,
                                  CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, env,
                                  NULL, &si.StartupInfo, &pi);
    } else {
        ok = CreateProcessW(exe, cmd, NULL, NULL, TRUE,
                            CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, env, NULL,
                            &si.StartupInfo, &pi);
    }
    if (!ok) {
        goto out;
    }
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        (void)TerminateProcess(pi.hProcess, 1u);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        goto out;
    }
    fs_low = token != NULL && process_integrity(pi.hProcess) == SECURITY_MANDATORY_LOW_RID;
    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        (void)TerminateProcess(pi.hProcess, 1u);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        goto out;
    }
    CloseHandle(pi.hThread);
    p->process = pi.hProcess;
    p->job = job;
    job = NULL;
    p->pid = (pid_t)pi.dwProcessId;
    p->in_fd = _open_osfhandle((intptr_t)in_srv, _O_BINARY | _O_WRONLY);
    if (p->in_fd < 0) {
        goto out_proc;
    }
    in_srv = NULL;
    p->out_fd = _open_osfhandle((intptr_t)out_srv, _O_BINARY | _O_RDONLY);
    if (p->out_fd < 0) {
        goto out_proc;
    }
    out_srv = NULL;
    job_containment(p, limits, fs_low, fs_err);
    p->alive = 1;
    su_line_reader_init(&p->reader, p->out_fd, -1);
    st = SU_OK;
    goto out;
out_proc:
    release(p);
out:
    if (attrs != NULL) {
        DeleteProcThreadAttributeList(attrs);
        free(attrs);
    }
    /* The child holds its own copies; the supervisor keeps only the server ends. */
    if (in_cli != NULL) {
        CloseHandle(in_cli);
    }
    if (out_cli != NULL) {
        CloseHandle(out_cli);
    }
    if (err_h != NULL) {
        CloseHandle(err_h);
    }
    if (in_srv != NULL) {
        CloseHandle(in_srv);
    }
    if (out_srv != NULL) {
        CloseHandle(out_srv);
    }
    if (token != NULL) {
        CloseHandle(token);
    }
    if (job != NULL) {
        CloseHandle(job);
    }
    free(exe);
    free(cmd);
    free(env);
    return st;
}
