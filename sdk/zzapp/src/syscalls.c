/*
 * newlib glue for the app heap. The heap is [image_end, stack_base), which
 * the runtime reports through heap_range. This is not the firmware _sbrk.
 *
 * Other stubs exist so -lc links. They do not reach the firmware console.
 * The contract is silent on them; ENOSYS / EIO is the boring failure.
 *
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
    (void)fd;
    (void)buf;
    (void)count;
    errno = EIO;
    return -1;
}

int _read(int fd, void *buf, size_t count)
{
    (void)fd;
    (void)buf;
    (void)count;
    errno = EIO;
    return -1;
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
