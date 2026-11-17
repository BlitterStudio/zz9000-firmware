/*
 * Echo every 32-byte client message until the launcher stops the app. The app
 * parks in zz_msg_wait between messages. Firmware older than API 0.4 has no
 * wait, so the app falls back to polling with zz_yield.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"
#include "zzapp_note.h"

ZZAPP_NOTE(0, 0)

int zz_main(int argc, char **argv)
{
    uint8_t message[32];

    (void)argc;
    (void)argv;
    while (!zz_should_stop()) {
        int rc = zz_msg_wait(500000u);

        if (rc == ZZ_ETIMEDOUT)
            continue;
        if (rc != ZZ_OK && rc != ZZ_ENOSYS)
            break;              /* stop, or a corrupt ring */
        rc = zz_msg_poll(message);
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
