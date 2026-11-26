/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * libaar's newlib glue (libaar/src/syscalls.c) on a glibc host.
 *
 * The glue defines newlib's stub names (_sbrk, _exit, _open, ...) and, where
 * newlib has no stub, the POSIX names themselves (rename, mkdir, rmdir,
 * ftruncate, truncate, chmod, utime, utimes, setenv, unsetenv, putenv).
 * Every one of those is a glibc symbol or is called through glibc's: defining
 * it in a test binary would replace the C library's own _exit, setenv and
 * file calls for the harness, the loader and the test code. So the real glue
 * is compiled with each such name renamed to aar_host_<name>, and tests call
 * aar_host_sbrk() and friends while the C library stays untouched. aar_alloc,
 * aar_free and the __wrap_ environment readers keep their names: glibc has
 * none of them.
 *
 * Order matters. glibc declares most of the POSIX names with nonnull
 * attributes, and the glue checks those arguments for NULL as the target's
 * newlib allows. If a rename came before the header, the renamed declaration
 * would still carry the attribute and -Wnonnull-compare (an error under
 * -Werror) would reject the glue's own checks. So every system header the
 * glue includes is loaded here under its native names first; the include
 * guards then make the glue's own #include lines no-ops, and only the
 * definitions are renamed.
 *
 * A new name in the glue that glibc also defines belongs in the list below.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>

#define _sbrk aar_host_sbrk
#define _exit aar_host_exit
#define _isatty aar_host_isatty
#define _kill aar_host_kill
#define _getpid aar_host_getpid
#define _open aar_host_open
#define _close aar_host_close
#define _write aar_host_write
#define _read aar_host_read
#define _lseek aar_host_lseek
#define _fstat aar_host_fstat
#define _stat aar_host_stat
#define _unlink aar_host_unlink
#define _rename aar_host_rename
#define _mkdir aar_host_mkdir
#define _gettimeofday aar_host_gettimeofday
#define _times aar_host_times
#define rename aar_host_posix_rename
#define mkdir aar_host_posix_mkdir
#define rmdir aar_host_rmdir
#define ftruncate aar_host_ftruncate
#define truncate aar_host_truncate
#define chmod aar_host_chmod
#define utime aar_host_utime
#define utimes aar_host_utimes
#define setenv aar_host_setenv
#define unsetenv aar_host_unsetenv
#define putenv aar_host_putenv

#include "syscalls.c"
