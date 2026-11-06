/*
 * Console rings are the process stdin/stdout. A stop during a read returns
 * ZZ_EIO. EOF is ZZ_OK and zero bytes. Writes are not dropped.
 *
 * On the card, newlib routes fd 1 and fd 2 through one ring. The host does
 * not link those syscalls, so printf and fprintf(stderr) stay on the C
 * library. zz_console_write still writes stdout.
 *
 * SPDX-License-Identifier: MIT
 */
#include "host_internal.h"

#include <stdio.h>
#include <string.h>

static int api_console_write(const void *buf, uint32_t len, uint32_t *out_n)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t done = 0;

    if (!buf || !out_n)
        return ZZ_EINVAL;
    while (done < len) {
        size_t n = fwrite(p + done, 1, (size_t)(len - done), stdout);
        if (n == 0) {
            if (ferror(stdout)) {
                *out_n = done;
                return ZZ_EIO;
            }
            break;
        }
        done += (uint32_t)n;
    }
    if (fflush(stdout) != 0) {
        *out_n = done;
        return ZZ_EIO;
    }
    *out_n = done;
    return done == len ? ZZ_OK : ZZ_EIO;
}

static int api_console_read(void *buf, uint32_t len, uint32_t *out_n)
{
    size_t n;

    if (!buf || !out_n)
        return ZZ_EINVAL;
    if (len == 0u) {
        *out_n = 0;
        return ZZ_OK;
    }
    if (host_stop_requested())
        return ZZ_EIO;
    n = fread(buf, 1, (size_t)len, stdin);
    if (n == 0) {
        if (host_stop_requested())
            return ZZ_EIO;
        if (feof(stdin)) {
            *out_n = 0;
            return ZZ_OK;
        }
        return ZZ_EIO;
    }
    *out_n = (uint32_t)n;
    return ZZ_OK;
}

void host_console_bind(zzapp_api *api)
{
    api->console_write = api_console_write;
    api->console_read = api_console_read;
}
