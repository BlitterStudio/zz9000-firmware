/*
 * Echo every 32-byte client message until the launcher stops the app.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"

int zz_main(int argc, char **argv)
{
    uint8_t message[32];

    (void)argc;
    (void)argv;
    while (!zz_should_stop()) {
        int rc = zz_msg_poll(message);

        if (rc < 0)
            return 20;
        if (rc == 0) {
            zz_yield();
            continue;
        }
        while ((rc = zz_msg_send(message)) == ZZ_EBUSY)
            zz_yield();
        if (rc != ZZ_OK)
            return 20;
    }
    return 0;
}
