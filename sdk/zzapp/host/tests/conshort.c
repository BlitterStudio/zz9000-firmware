/*
 * One console read with the input pipe still open. zz_console_read must
 * return the available bytes (card semantics), not block waiting for the
 * full length; a blocked read never prints and this never exits.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"

#include <stdio.h>

int zz_main(int argc, char **argv)
{
    char buf[64];
    uint32_t n = 0;
    int rc;

    (void)argc;
    (void)argv;
    rc = zz_console_read(buf, sizeof buf, &n);
    if (rc != ZZ_OK)
        return 1;
    printf("got %u: %.*s\n", n, (int)n, buf);
    fflush(stdout);
    return n == 6u ? 0 : 2;
}
