/*
 * Sends more app-to-host messages than the a2h ring holds. With
 * --msg-script every send must succeed and be recorded; a wedged ring
 * returns ZZ_EBUSY and this exits non-zero.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"

#include <string.h>

int zz_main(int argc, char **argv)
{
    uint8_t msg[32];
    uint32_t ok = 0;
    uint32_t i;

    (void)argc;
    (void)argv;
    memset(msg, 0xAA, sizeof msg);
    for (i = 0; i < 100u; i++) {
        if (zz_msg_send(msg) == ZZ_OK)
            ok++;
    }
    return ok == 100u ? 0 : 1;
}
