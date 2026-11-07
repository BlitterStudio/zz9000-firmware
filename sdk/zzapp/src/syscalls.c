/*
 * newlib glue for the app heap. The heap is [image_end, stack_base), which
 * the runtime reports through heap_range. This is not the firmware _sbrk.
 *
 * File descriptors 0, 1, and 2 are the launcher console. They are separate
 * from zz_log: writes use a lossless backpressured ring and reads return zero
 * at the launcher-published EOF. Other descriptor stubs remain deliberately
 * unsupported here.
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"

#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

struct _reent;

static uint8_t *heap_brk;
static uint8_t *heap_base;
static uint8_t *heap_end;
static int heap_ready;

static int heap_init(void)
{
    void *base = 0;
    uint32_t bytes = 0;

    if (heap_ready)
        return 1;
    if (!ZZ_API_COVERS(zzapp_api_ptr, heap_range) || !zzapp_api_ptr->heap_range)
        return 0;
    zzapp_api_ptr->heap_range(&base, &bytes);
    if (!base && bytes != 0)
        return 0;
    heap_base = base;
    heap_end = (uint8_t *)base + bytes;
    heap_brk = heap_base;
    heap_ready = 1;
    return 1;
}

void *zzapp_sbrk(int increment)
{
    uint8_t *previous;
    uint32_t step;

    if (!heap_init()) {
        errno = ENOMEM;
        return (void *)-1;
    }
    previous = heap_brk;
    if (increment >= 0) {
        step = (uint32_t)increment;
        if (step > (uint32_t)(heap_end - heap_brk)) {
            errno = ENOMEM;
            return (void *)-1;
        }
    } else {
        step = (uint32_t)(-(increment + 1)) + 1u;
        if (step > (uint32_t)(heap_brk - heap_base)) {
            errno = ENOMEM;
            return (void *)-1;
        }
    }
    heap_brk = previous + increment;
    return previous;
}

#if !defined(ZZAPP_HOST_TEST)
caddr_t _sbrk(int increment)
{
    return zzapp_sbrk(increment);
}

void _exit(int code)
{
    zz_exit(code);
}

int _write(int fd, const void *buf, size_t count)
{
    const uint8_t *bytes = (const uint8_t *)buf;
    size_t done = 0;

    if ((fd != 1 && fd != 2) || (count != 0 && !bytes)) {
        errno = EIO;
        return -1;
    }
    while (done < count) {
        uint32_t wrote = 0;
        uint32_t chunk = count - done > UINT32_MAX ? UINT32_MAX :
                         (uint32_t)(count - done);
        int rc = zz_console_write(bytes + done, chunk, &wrote);

        if (rc != ZZ_OK || wrote > chunk) {
            errno = EIO;
            return done ? (int)done : -1;
        }
        done += wrote;
        if (wrote != chunk) {
            errno = EIO;
            return (int)done;
        }
    }
    return (int)done;
}

int _read(int fd, void *buf, size_t count)
{
    uint32_t got = 0;
    uint32_t chunk;
    int rc;

    if (fd != 0 || (count != 0 && !buf)) {
        errno = EIO;
        return -1;
    }
    if (count == 0)
        return 0;
    chunk = count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
    rc = zz_console_read(buf, chunk, &got);
    if (rc != ZZ_OK || got > chunk) {
        errno = EIO;
        return -1;
    }
    return (int)got;
}

int _close(int fd)
{
    (void)fd;
    errno = ENOSYS;
    return -1;
}

int _lseek(int fd, int offset, int whence)
{
    (void)fd;
    (void)offset;
    (void)whence;
    errno = ENOSYS;
    return -1;
}

int _fstat(int fd, struct stat *st)
{
    (void)fd;
    (void)st;
    errno = ENOSYS;
    return -1;
}

int _isatty(int fd)
{
    (void)fd;
    return 0;
}

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = ENOSYS;
    return -1;
}

int _getpid(void)
{
    return 1;
}

#endif
