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

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
typedef int ssize_t_pipe;
#define pipe_read(fd, buf, n) _read((fd), (buf), (int)(n))
typedef ssize_t_pipe read_len_t;
#else
#include <unistd.h>
typedef ssize_t read_len_t;
#define pipe_read(fd, buf, n) read((fd), (buf), (size_t)(n))
#endif

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
    read_len_t n;

    if (!buf || !out_n)
        return ZZ_EINVAL;
    if (len == 0u) {
        *out_n = 0;
        return ZZ_OK;
    }
    if (host_stop_requested())
        return ZZ_EIO;
    /*
     * The card returns as soon as any input is available. fread would
     * block until the full length or EOF; a raw read returns what the
     * pipe holds now.
     */
    do {
        n = pipe_read(0, buf, len);
    } while (n < 0 && errno == EINTR);
    if (n == 0) {
        if (host_stop_requested())
            return ZZ_EIO;
        *out_n = 0;
        return ZZ_OK; /* EOF */
    }
    if (n < 0)
        return ZZ_EIO;
    *out_n = (uint32_t)n;
    return ZZ_OK;
}

void host_console_bind(zzapp_api *api)
{
    api->console_write = api_console_write;
    api->console_read = api_console_read;
}
