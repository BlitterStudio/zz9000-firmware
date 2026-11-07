/*
 * zz_* wrappers. Every firmware call goes through zzapp_api. A field is
 * read only when size covers it. A missing optional call returns ZZ_ENOSYS.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"
#include <stdlib.h>

const zzapp_api *zzapp_api_ptr;
static const zzapp_gl *bound_gl;
/* Host-visible allocations are few and application-owned.  Track only these
 * so zz_free can keep newlib allocations on libc's allocator. */
#define ZZAPP_HOST_ALLOC_SLOTS 64u
static void *host_allocs[ZZAPP_HOST_ALLOC_SLOTS];

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
    uintptr_t aligned;
    void *raw;
    uint32_t i;

    if ((flags & ~(ZZ_ALLOC_UNCACHED | ZZ_ALLOC_HOST_VISIBLE)) != 0u)
        return 0;
    if ((flags & ZZ_ALLOC_HOST_VISIBLE) != 0u) {
        if (!ZZ_API_COVERS(zzapp_api_ptr, alloc) || !zzapp_api_ptr->alloc)
            return 0;
        raw = zzapp_api_ptr->alloc(bytes, align, flags);
        if (!raw)
            return 0;
        for (i = 0; i < ZZAPP_HOST_ALLOC_SLOTS; ++i) {
            if (!host_allocs[i]) {
                host_allocs[i] = raw;
                return raw;
            }
        }
        zzapp_api_ptr->free(raw);
        return 0;
    }
    if (align < sizeof(void *) || (align & (align - 1u)) != 0u)
        return 0;
    if (bytes > UINT32_MAX - align - (uint32_t)sizeof(void *))
        return 0;
    raw = malloc((size_t)bytes + align - 1u + sizeof(void *));
    if (!raw)
        return 0;
    aligned = ((uintptr_t)raw + sizeof(void *) + align - 1u) &
              ~(uintptr_t)(align - 1u);
    ((void **)aligned)[-1] = raw;
    return (void *)aligned;
}

void zz_free(void *p)
{
    uint32_t i;

    if (!p)
        return;
    for (i = 0; i < ZZAPP_HOST_ALLOC_SLOTS; ++i) {
        if (host_allocs[i] == p) {
            host_allocs[i] = 0;
            if (ZZ_API_COVERS(zzapp_api_ptr, free) && zzapp_api_ptr->free)
                zzapp_api_ptr->free(p);
            return;
        }
    }
    free(((void **)p)[-1]);
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

int zz_host_address(const void *p, uint32_t *host_offset)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, host_address) &&
        zzapp_api_ptr->host_address)
        return zzapp_api_ptr->host_address(p, host_offset);
    return ZZ_ENOSYS;
}

int zz_mem_info(zzapp_mem_info *out)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, mem_info) && zzapp_api_ptr->mem_info)
        return zzapp_api_ptr->mem_info(out);
    return ZZ_ENOSYS;
}

int zz_map_file(const char *path, const void **data, uint32_t *bytes)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, map_file) && zzapp_api_ptr->map_file)
        return zzapp_api_ptr->map_file(path, data, bytes);
    return ZZ_ENOSYS;
}

void zz_unmap_file(const void *data)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, unmap_file) && zzapp_api_ptr->unmap_file)
        zzapp_api_ptr->unmap_file(data);
}

int zz_sha256(const void *data, uint32_t bytes, uint8_t digest[32])
{
    if (ZZ_API_COVERS(zzapp_api_ptr, sha256) && zzapp_api_ptr->sha256)
        return zzapp_api_ptr->sha256(data, bytes, digest);
    return ZZ_ENOSYS;
}

int zz_decompress(uint32_t algorithm, uint32_t flags, const void *src,
                  uint32_t src_bytes, void *dst, uint32_t dst_bytes,
                  zzapp_service_result *out)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, decompress) &&
        zzapp_api_ptr->decompress)
        return zzapp_api_ptr->decompress(algorithm, flags, src, src_bytes,
                                         dst, dst_bytes, out);
    return ZZ_ENOSYS;
}
int zz_image_open(const zzapp_image_open *open, uint32_t *session,
                  zzapp_image_result *out)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, image_open) && zzapp_api_ptr->image_open)
        return zzapp_api_ptr->image_open(open, session, out);
    return ZZ_ENOSYS;
}

int zz_image_decode(uint32_t session, const void *src, uint32_t src_bytes,
                    uint32_t flags, zzapp_image_result *out)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, image_decode) &&
        zzapp_api_ptr->image_decode)
        return zzapp_api_ptr->image_decode(session, src, src_bytes, flags, out);
    return ZZ_ENOSYS;
}

int zz_image_close(uint32_t session)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, image_close) &&
        zzapp_api_ptr->image_close)
        return zzapp_api_ptr->image_close(session);
    return ZZ_ENOSYS;
}

int zz_audio_decode_open(uint32_t codec, uint32_t *session)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, audio_decode_open) &&
        zzapp_api_ptr->audio_decode_open)
        return zzapp_api_ptr->audio_decode_open(codec, session);
    return ZZ_ENOSYS;
}

int zz_audio_decode(uint32_t session, const void *src, uint32_t src_bytes,
                    uint32_t flags, int16_t *pcm, uint32_t pcm_frames,
                    zzapp_audio_decode_result *out)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, audio_decode) &&
        zzapp_api_ptr->audio_decode)
        return zzapp_api_ptr->audio_decode(session, src, src_bytes, flags, pcm,
                                           pcm_frames, out);
    return ZZ_ENOSYS;
}

int zz_audio_decode_close(uint32_t session)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, audio_decode_close) &&
        zzapp_api_ptr->audio_decode_close)
        return zzapp_api_ptr->audio_decode_close(session);
    return ZZ_ENOSYS;
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

int zz_display_flip_async(uint32_t surface_id)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, display_flip_async) &&
        zzapp_api_ptr->display_flip_async)
        return zzapp_api_ptr->display_flip_async(surface_id);
    return ZZ_ENOSYS;
}

int zz_display_flip_idle(void)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, display_flip_idle) &&
        zzapp_api_ptr->display_flip_idle)
        return zzapp_api_ptr->display_flip_idle();
    return ZZ_ENOSYS;
}

int zz_display_refresh(zzapp_surface *surface)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, display_refresh) &&
        zzapp_api_ptr->display_refresh)
        return zzapp_api_ptr->display_refresh(surface);
    return ZZ_ENOSYS;
}

uint32_t zz_display_mailbox_replaced(void)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, display_mailbox_replaced) &&
        zzapp_api_ptr->display_mailbox_replaced)
        return zzapp_api_ptr->display_mailbox_replaced();
    return 0U;
}

int zz_display_set_palette(uint32_t surface_id, uint32_t first,
                           uint32_t count, const uint32_t *rgb)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, display_set_palette) &&
        zzapp_api_ptr->display_set_palette)
        return zzapp_api_ptr->display_set_palette(surface_id, first, count, rgb);
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

int zz_console_write(const void *buf, uint32_t len, uint32_t *out_n)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, console_write) &&
        zzapp_api_ptr->console_write)
        return zzapp_api_ptr->console_write(buf, len, out_n);
    return ZZ_ENOSYS;
}

int zz_console_read(void *buf, uint32_t len, uint32_t *out_n)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, console_read) &&
        zzapp_api_ptr->console_read)
        return zzapp_api_ptr->console_read(buf, len, out_n);
    return ZZ_ENOSYS;
}

int zz_msg_send(const void *msg)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, msg_send) && zzapp_api_ptr->msg_send)
        return zzapp_api_ptr->msg_send(msg);
    return ZZ_ENOSYS;
}

int zz_msg_poll(void *msg)
{
    if (ZZ_API_COVERS(zzapp_api_ptr, msg_poll) && zzapp_api_ptr->msg_poll)
        return zzapp_api_ptr->msg_poll(msg);
    return ZZ_ENOSYS;
}

const zzapp_gl *zz_gl_bind_flags(uint32_t surface_id, uint32_t flags)
{
    const zzapp_gl *gl;

    bound_gl = 0;
    if (ZZ_API_COVERS(zzapp_api_ptr, gl_bind_flags) &&
        zzapp_api_ptr->gl_bind_flags)
        gl = zzapp_api_ptr->gl_bind_flags(surface_id, flags);
    else if (flags == 0U && ZZ_API_COVERS(zzapp_api_ptr, gl_bind) &&
             zzapp_api_ptr->gl_bind)
        gl = zzapp_api_ptr->gl_bind(surface_id);
    else
        return 0;
    bound_gl = gl;
    return gl;
}

const zzapp_gl *zz_gl_bind(uint32_t surface_id)
{
    return zz_gl_bind_flags(surface_id, 0U);
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
