/* POSIX file and process names used by the control plane, for both platforms.
 *
 * POSIX: includes <unistd.h>, <fcntl.h> and <sys/stat.h> and adds O_BINARY (0).
 * Windows: maps the same names onto the UCRT's low-level I/O (<io.h>, <direct.h>, <process.h>), which
 * implements open/read/write/close/unlink/getpid with POSIX meaning. Differences callers must respect:
 *   - every open() of a data file passes O_BINARY, or the UCRT translates CRLF and ^Z;
 *   - O_CLOEXEC maps to _O_NOINHERIT (handle not inherited by children); O_NONBLOCK has no meaning and is 0;
 *   - struct stat / stat / fstat map to the 64-bit-size variants, so st_size holds files over 2 GiB;
 *   - mkdir ignores the mode argument (NTFS ACLs are inherited from the parent).
 * Include this header before any other header that might include <sys/stat.h> after it. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_PLATFORM_SU_POSIX_H
#define SUTEKH_PLATFORM_SU_POSIX_H

#if defined(_WIN32)
#  include <direct.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <io.h>
#  include <process.h>
#  include <stddef.h>
#  include <sys/stat.h>
#  include <sys/types.h>

typedef ptrdiff_t ssize_t;
typedef int pid_t;

#  ifndef O_CLOEXEC
#    define O_CLOEXEC _O_NOINHERIT
#  endif
#  ifndef O_NONBLOCK
#    define O_NONBLOCK 0
#  endif
#  ifndef S_ISREG
#    define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#  endif
#  ifndef S_ISDIR
#    define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#  endif
#  ifndef S_ISFIFO
#    define S_ISFIFO(m) (((m) & _S_IFMT) == _S_IFIFO)
#  endif
#  ifndef R_OK
#    define R_OK 4
#  endif
#  ifndef F_OK
#    define F_OK 0
#  endif
/* 64-bit sizes: `struct stat` becomes `struct _stat64`, stat() _stat64() and fstat() _fstat64(). */
#  define stat _stat64
#  define fstat _fstat64
#  define mkdir(path, mode) _mkdir(path)
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#ifndef O_BINARY
#  define O_BINARY 0
#endif

/* Byte count of one read() or write() call. The UCRT's functions take an unsigned int; callers loop over partial
 * transfers anyway, so a call moves at most SU_IO_CHUNK_MAX bytes (1 GiB) on every platform (2026-10-18, MSVC
 * /W4 C4267). */
#define SU_IO_CHUNK_MAX ((size_t)1 << 30)
#if defined(_WIN32)
#  define SU_IO_COUNT(n) ((unsigned int)((size_t)(n) < SU_IO_CHUNK_MAX ? (size_t)(n) : SU_IO_CHUNK_MAX))
#else
#  define SU_IO_COUNT(n) ((size_t)(n) < SU_IO_CHUNK_MAX ? (size_t)(n) : SU_IO_CHUNK_MAX)
#endif

#endif
