/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Stand-in for newlib's <reent.h> on a glibc host.
 *
 * libaar's newlib glue (libaar/src/syscalls.c) includes <reent.h> for the
 * struct _reent parameter of __wrap__getenv_r, which it never dereferences.
 * glibc has no such header, so only the incomplete type is declared here. The
 * directory is on the include path of the syscalls.o rule alone.
 */
#ifndef ZZAPP_HOST_REENT_H
#define ZZAPP_HOST_REENT_H

struct _reent;

#endif
