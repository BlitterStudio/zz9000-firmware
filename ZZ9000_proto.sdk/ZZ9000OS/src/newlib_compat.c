/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <errno.h>

char *__ctype_ptr__ __attribute__((weak)) = (char *)_ctype_;

int _unlink(const char *path)
{
	(void)path;
	errno = ENOSYS;
	return -1;
}
