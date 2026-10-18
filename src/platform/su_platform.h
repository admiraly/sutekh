/* Platform services for the M0 skeleton: monotonic clock and process id.
 * Internal to the native build; not part of the frozen ABI in include/sutekh. */
#ifndef SUTEKH_PLATFORM_H
#define SUTEKH_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* Monotonic clock in nanoseconds from an unspecified origin. Returns 0 and
 * sets *out_ns to 0 on failure; check the return value. */
int su_platform_monotonic_ns(uint64_t *out_ns);

/* Process identifier of the calling process. */
uint32_t su_platform_pid(void);

/* Canonical absolute path of an existing file or directory: symbolic links (and, on Windows, junctions)
 * resolved, no "." or ".." components. POSIX uses realpath(3). Windows opens the object and reads the
 * final path from its handle; the result uses '/' separators and a drive-letter prefix ("C:/dir/file"),
 * or "//server/share/..." for UNC paths. path is UTF-8 on both platforms.
 * Returns 0 on success; ENOENT when the object does not exist or cannot be opened, ENAMETOOLONG when the
 * result does not fit cap, EINVAL for bad arguments. */
int su_platform_realpath(const char *path, char *out, size_t cap);

/* Nonzero when path is absolute on this platform: "/x" on POSIX; on Windows a drive prefix ("C:/", "C:\"), a
 * UNC or extended-length prefix, or a leading separator (the root of the current drive). */
int su_platform_path_is_absolute(const char *path);

/* Nonzero when c separates path components on this platform ('/' always, also '\' on Windows). */
int su_platform_is_sep(char c);

/* Directory for private temporary files, without a trailing separator: TMPDIR or "/tmp" on POSIX; on
 * Windows TMPDIR when set, else the per-user GetTempPathW directory (with '/' separators). Returns 0, or ENAMETOOLONG/ENOENT. */
int su_platform_temp_dir(char *out, size_t cap);

/* mkdtemp(3): tmpl ends in "XXXXXX", which is replaced with a unique suffix, and the directory is created
 * private to the user (0700 on POSIX; on Windows it inherits the per-user temp directory's ACL). Returns
 * tmpl, or NULL with errno set. */
char *su_platform_mkdtemp(char *tmpl);

/* Absolute path of the running executable (/proc/self/exe on Linux; GetModuleFileNameW on Windows, with
 * '/' separators). Returns 0, ENOENT or ENAMETOOLONG. */
int su_platform_self_exe(char *out, size_t cap);

/* Online logical processors (at least 1). */
uint32_t su_platform_cpu_count(void);

/* Sleeps at least ms milliseconds (the Windows timer granularity may round it up). */
void su_platform_sleep_ms(uint32_t ms);

/* Process setup for a protocol process (serve, worker): binary stdin/stdout, and on Windows no error or
 * abort dialogs (a crash ends the process with its status). No-op on POSIX. */
void su_platform_process_init(void);

/* Ends the calling process at once, without cleanup: SIGKILL on POSIX; TerminateProcess with exit code
 * SU_PLATFORM_KILLED_EXIT_CODE on Windows, which has no signals (128 + SIGKILL, the shell convention).
 * Test seams only. Never returns. */
#define SU_PLATFORM_KILLED_EXIT_CODE 137u
_Noreturn void su_platform_kill_self(void);

#endif
