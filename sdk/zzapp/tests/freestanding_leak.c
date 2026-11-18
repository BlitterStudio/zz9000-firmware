/*
 * Negative fixture for `make check`: linked into the freestanding template
 * together with -lc, it makes the link map hold a newlib member. The build
 * must fail in zzapp-check --no-libc. Not an app template.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>

size_t strlen(const char *s);

size_t zz_leak(const char *s)
{
    return strlen(s);
}
