/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * libaar's newlib glue (libaar/src/syscalls.c) on a glibc host.
 *
 * The glue defines the newlib stub names (_sbrk, _exit, _open, ...). _exit is
 * glibc's own symbol, so defining it in a test would replace the C library's
 * exit path. Every stub is renamed to aar_host_<name> before the glue is
 * included, so tests can call aar_host_sbrk() and friends while the real C
 * library stays untouched. aar_alloc and aar_free keep their names.
 */
#define _GNU_SOURCE

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

#include "syscalls.c"
