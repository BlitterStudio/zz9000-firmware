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
#include <windows.h>
#define pipe_read(fd, buf, n) _read((fd), (buf), (int)(n))
typedef int read_len_t;
#else
#include <poll.h>
#include <unistd.h>
typedef ssize_t read_len_t;
#define pipe_read(fd, buf, n) read((fd), (buf), (size_t)(n))
#endif

/* 1 = stdin is readable (or at EOF, so the read decides), 0 = timed out
 * and the stop flag must be re-checked, -1 = error. */
static int wait_stdin(uint32_t ms)
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD type = GetFileType(h);
    DWORD avail = 0;
    DWORD waited;

    if (type == FILE_TYPE_PIPE) {
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
            return 1; /* writer closed: let the read report EOF */
        return avail != 0 ? 1 : 0;
    }
    if (type == FILE_TYPE_DISK)
        return 1;
    waited = WaitForSingleObject(h, ms);
    if (waited == WAIT_OBJECT_0)
        return 1;
    if (waited == WAIT_TIMEOUT)
        return 0;
    return -1;
#else
    struct pollfd pfd;
    int pr;

    pfd.fd = 0;
    pfd.events = POLLIN;
    pfd.revents = 0;
    do {
        pr = poll(&pfd, 1, (int)ms);
    } while (pr < 0 && errno == EINTR);
    if (pr < 0)
        return -1;
    return pr != 0 ? 1 : 0;
#endif
}

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
     * block until the full length or EOF; wait in slices so a stop
     * request (Ctrl-C, SIGTERM) interrupts a blocked read like the card.
     */
    for (;;) {
        int ready;

        if (host_stop_requested())
            return ZZ_EIO;
        ready = wait_stdin(200u);
        if (ready < 0)
            return ZZ_EIO;
        if (ready == 0)
            continue; /* timeout: re-check the stop flag */
        do {
            n = pipe_read(0, buf, len);
        } while (n < 0 && errno == EINTR);
        if (n == 0) {
            if (host_stop_requested())
                return ZZ_EIO;
            *out_n = 0;
            return ZZ_OK; /* EOF */
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue; /* spurious readiness */
            return ZZ_EIO;
        }
        *out_n = (uint32_t)n;
        return ZZ_OK;
    }
}

void host_console_bind(zzapp_api *api)
{
    api->console_write = api_console_write;
    api->console_read = api_console_read;
}
