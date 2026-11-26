/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * libaar's newlib glue (libaar/src/syscalls.c) on a glibc host.
 *
 * The glue defines the newlib stub names (_sbrk, _exit, _open, ...) and the
 * POSIX calls newlib leaves to the platform (rmdir, rename, mkdir, truncate,
 * chmod, setenv, ...). Several are glibc's own symbols: defining _exit in a
 * test would replace the C library's exit path, and defining setenv or mkdir
 * would replace the host calls the other test code makes. Those names are
 * renamed to aar_host_<name> before the glue is included, so tests can call
 * aar_host_sbrk() and friends while the real C library stays untouched.
 * aar_alloc and aar_free keep their names.
 *
 * The C library headers the glue includes are read first, under the real
 * names. glibc declares most of these calls with __nonnull, and the glue
 * checks those arguments for NULL; a prototype renamed along with the
 * definition would carry that attribute onto it and -Werror=nonnull-compare
 * would reject the glue.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

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
#define rmdir aar_host_rmdir
#define rename aar_host_rename
#define mkdir aar_host_mkdir
#define ftruncate aar_host_ftruncate
#define truncate aar_host_truncate
#define chmod aar_host_chmod
#define utime aar_host_utime
#define utimes aar_host_utimes
#define setenv aar_host_setenv
#define unsetenv aar_host_unsetenv
#define putenv aar_host_putenv

#include "syscalls.c"
