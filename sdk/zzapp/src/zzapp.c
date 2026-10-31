/*
 * zz_* wrappers. Every firmware call goes through zzapp_api. A field is
 * read only when size covers it. A missing optional call returns ZZ_ENOSYS.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"

const zzapp_api *zzapp_api_ptr;
static const zzapp_gl *bound_gl;

void zzapp_crt0_bind(const zzapp_api *api)
{
    zzapp_api_ptr = api;
    bound_gl = 0;
}

void zz_exit(int code)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, exit) && zzapp_api_ptr->exit)
        zzapp_api_ptr->exit(code);
    for (;;)
        ;
}

void zz_yield(void)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, yield) && zzapp_api_ptr->yield)
        zzapp_api_ptr->yield();
}

int zz_should_stop(void)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, should_stop) && zzapp_api_ptr->should_stop)
        return zzapp_api_ptr->should_stop();
    return ZZ_ENOSYS;
}

uint64_t zz_time_us(void)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, time_us) && zzapp_api_ptr->time_us)
        return zzapp_api_ptr->time_us();
    return 0;
}

void zz_sleep_us(uint32_t us)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, sleep_us) && zzapp_api_ptr->sleep_us)
        zzapp_api_ptr->sleep_us(us);
}

void *zz_alloc(uint32_t bytes, uint32_t align, uint32_t flags)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, alloc) && zzapp_api_ptr->alloc)
        return zzapp_api_ptr->alloc(bytes, align, flags);
    return 0;
}

void zz_free(void *p)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, free) && zzapp_api_ptr->free)
        zzapp_api_ptr->free(p);
}

void zz_heap_range(void **base, uint32_t *bytes)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, heap_range) && zzapp_api_ptr->heap_range)
        zzapp_api_ptr->heap_range(base, bytes);
    else {
        if (base)
            *base = 0;
        if (bytes)
            *bytes = 0;
    }
}

void zz_log(const char *msg, uint32_t len)
{
    if (!msg)
        return;
    if (ZZ_API_COVERS(zzapp_api_ptr, log) && zzapp_api_ptr->log)
        zzapp_api_ptr->log(msg, len);
}

int zz_file_open(const char *path, uint32_t flags)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, file_open) && zzapp_api_ptr->file_open)
        return zzapp_api_ptr->file_open(path, flags);
    return ZZ_ENOSYS;
}

int zz_file_close(int fd)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, file_close) && zzapp_api_ptr->file_close)
        return zzapp_api_ptr->file_close(fd);
    return ZZ_ENOSYS;
}

int zz_file_read(int fd, void *buf, uint32_t len, uint32_t *out_n)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, file_read) && zzapp_api_ptr->file_read)
        return zzapp_api_ptr->file_read(fd, buf, len, out_n);
    return ZZ_ENOSYS;
}

int zz_file_write(int fd, const void *buf, uint32_t len, uint32_t *out_n)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, file_write) && zzapp_api_ptr->file_write)
        return zzapp_api_ptr->file_write(fd, buf, len, out_n);
    return ZZ_ENOSYS;
}

int zz_file_seek(int fd, int64_t off, int whence, int64_t *out_pos)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, file_seek) && zzapp_api_ptr->file_seek)
        return zzapp_api_ptr->file_seek(fd, off, whence, out_pos);
    return ZZ_ENOSYS;
}

int zz_file_stat(const char *path, zzapp_stat *out)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, file_stat) && zzapp_api_ptr->file_stat)
        return zzapp_api_ptr->file_stat(path, out);
    return ZZ_ENOSYS;
}

int zz_file_list_open(const char *path)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, file_list_open) && zzapp_api_ptr->file_list_open)
        return zzapp_api_ptr->file_list_open(path);
    return ZZ_ENOSYS;
}

int zz_file_list_next(int list, zzapp_dirent *out)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, file_list_next) && zzapp_api_ptr->file_list_next)
        return zzapp_api_ptr->file_list_next(list, out);
    return ZZ_ENOSYS;
}

int zz_file_list_close(int list)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, file_list_close) && zzapp_api_ptr->file_list_close)
        return zzapp_api_ptr->file_list_close(list);
    return ZZ_ENOSYS;
}

int zz_display_open(uint32_t w, uint32_t h, uint32_t format,
                    uint32_t flags, zzapp_surface *out)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, display_open) && zzapp_api_ptr->display_open)
        return zzapp_api_ptr->display_open(w, h, format, flags, out);
    return ZZ_ENOSYS;
}

int zz_display_flip(uint32_t surface_id)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, display_flip) && zzapp_api_ptr->display_flip)
        return zzapp_api_ptr->display_flip(surface_id);
    return ZZ_ENOSYS;
}

int zz_display_vsync(void)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, display_vsync) && zzapp_api_ptr->display_vsync)
        return zzapp_api_ptr->display_vsync();
    return ZZ_ENOSYS;
}

int zz_display_close(uint32_t surface_id)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, display_close) && zzapp_api_ptr->display_close)
        return zzapp_api_ptr->display_close(surface_id);
    return ZZ_ENOSYS;
}

int zz_input_poll(zzapp_event *out, uint32_t max, uint32_t *out_n)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, input_poll) && zzapp_api_ptr->input_poll)
        return zzapp_api_ptr->input_poll(out, max, out_n);
    return ZZ_ENOSYS;
}

int zz_audio_open(uint32_t rate, uint32_t channels, zzapp_audio *out)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, audio_open) && zzapp_api_ptr->audio_open)
        return zzapp_api_ptr->audio_open(rate, channels, out);
    return ZZ_ENOSYS;
}

int zz_audio_close(void)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, audio_close) && zzapp_api_ptr->audio_close)
        return zzapp_api_ptr->audio_close();
    return ZZ_ENOSYS;
}

const zzapp_gl *zz_gl_bind(uint32_t surface_id)
{
    const zzapp_gl *gl;

    bound_gl = 0;
    if (!ZZ_API_COVERS(zzapp_api_ptr, gl_bind) || !zzapp_api_ptr->gl_bind)
        return 0;
    gl = zzapp_api_ptr->gl_bind(surface_id);
    bound_gl = gl;
    return gl;
}

void zz_gl_swap(void)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, gl_swap) && zzapp_api_ptr->gl_swap)
        zzapp_api_ptr->gl_swap();
}

void zz_gl_unbind(void)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, gl_unbind) && zzapp_api_ptr->gl_unbind)
        zzapp_api_ptr->gl_unbind();
    bound_gl = 0;
}

const zzapp_gl *zzapp_gl_bound(void)
{
    return bound_gl;
}
