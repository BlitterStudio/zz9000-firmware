/*
 * Mailbox three-buffer rotation checks. The drawing buffer must always be
 * one of the three live buffers, after async-then-sync and sync-then-async
 * flips; a wrong rotation hands the app a NULL back buffer.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"

#include <stdio.h>

static int fail(const char *what)
{
    fprintf(stderr, "mailbox: %s\n", what);
    return 1;
}

int zz_main(int argc, char **argv)
{
    zzapp_surface s = {0};
    const void *p0;
    const void *p1;
    const void *p2;
    const void *last;

    (void)argc;
    (void)argv;
    if (zz_display_open(8, 8, ZZ_FMT_RGB565, ZZ_DISPLAY_MAILBOX, &s) != ZZ_OK)
        return fail("open");
    p0 = s.front;
    p1 = s.back;
    last = p1;

    /* async flip, let the pump show it, then a synchronous flip */
    if (zz_display_flip_async(s.id) != ZZ_OK)
        return fail("async flip");
    if (zz_display_refresh(&s) != ZZ_OK || !s.front || !s.back ||
        s.back == s.front || s.back == p0 || s.back == p1)
        return fail("back after async flip");
    p2 = s.back;
    last = p2;
    zz_yield();
    if (zz_display_flip(s.id) != ZZ_OK)
        return fail("sync flip after async");
    if (zz_display_refresh(&s) != ZZ_OK || !s.front || !s.back ||
        s.back == s.front)
        return fail("rotation broke after sync-after-async");
    if (s.front != p2)
        return fail("sync did not present the drawn buffer");
    last = s.back;

    /* other order: synchronous flip first, then async */
    if (zz_display_flip(s.id) != ZZ_OK)
        return fail("sync flip");
    if (zz_display_refresh(&s) != ZZ_OK || !s.front || !s.back ||
        s.back == s.front)
        return fail("rotation broke after sync");
    if (s.front != last)
        return fail("sync did not present the drawn buffer");
    if (zz_display_flip_async(s.id) != ZZ_OK)
        return fail("async flip after sync");
    if (zz_display_refresh(&s) != ZZ_OK || !s.front || !s.back ||
        s.back == s.front)
        return fail("rotation broke after async-after-sync");
    return 0;
}
