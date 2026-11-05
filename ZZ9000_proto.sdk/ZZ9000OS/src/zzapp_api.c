/*
 * Trusted app API table. File/display/input/audio bodies are Slice B seams.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stddef.h>
#include <stdint.h>
#ifdef ZZAPP_HOST_TEST
#include <stdlib.h>
#endif
#include "zzapp.h"
#include "zzapp_runtime.h"
#include "zzm_format.h"
#include "zzm_loader.h"
#include "zzm_minigl_host.h"
#include "zzm_registry.h"
#include "sdk_compression.h"
#include "sdk_crypto.h"

extern int zzapp_host_address(const void *p, uint32_t *host_offset);
extern int zzapp_host_mem_info(zzapp_mem_info *out);
extern void *zzapp_host_alloc(uint32_t bytes, uint32_t align, uint32_t flags);


#ifndef ZZ9000_BUILD_GIT
#define ZZ9000_BUILD_GIT "unknown"
#endif
#define WEAK __attribute__((weak))

#ifdef ZZAPP_HOST_TEST
extern int zzm_loader_acquire(uint32_t module_id) WEAK;
extern void zzm_loader_release(uint32_t module_id) WEAK;
extern const void *zzm_registry_ops(uint32_t module_id, uint32_t min_size) WEAK;
extern const zzm_minigl_ops_v1_t *zzm_minigl_ops(void **context) WEAK;
#endif

WEAK void zzapp_services_poll(void) {}
WEAK int zzapp_services_take_start(uint32_t *argc) { (void)argc; return 0; }
WEAK uint8_t *zzapp_services_window(void) { return NULL; }
WEAK void zzapp_services_start_entered(int status) { (void)status; }
WEAK void zzapp_services_on_reset(void) {}
WEAK void zzapp_services_on_app_end(void) {}
WEAK void zzapp_services_cancel_start(int status) { (void)status; }
WEAK void zzapp_services_on_launch(void) {}
WEAK void zzapp_svc_log(const char *msg, uint32_t len) { (void)msg; (void)len; }
WEAK int zzapp_svc_should_stop(void) { return 0; }
WEAK int zzapp_svc_file_open(const char *p, uint32_t f) { (void)p; (void)f; return ZZ_ENOSYS; }
WEAK int zzapp_svc_file_close(int fd) { (void)fd; return ZZ_ENOSYS; }
WEAK int zzapp_svc_file_read(int fd, void *p, uint32_t n, uint32_t *o) { (void)fd; (void)p; (void)n; if (o) *o = 0; return ZZ_ENOSYS; }
WEAK int zzapp_svc_file_write(int fd, const void *p, uint32_t n, uint32_t *o) { (void)fd; (void)p; (void)n; if (o) *o = 0; return ZZ_ENOSYS; }
WEAK int zzapp_svc_file_seek(int fd, int64_t o, int w, int64_t *out) { (void)fd; (void)o; (void)w; if (out) *out = 0; return ZZ_ENOSYS; }
WEAK int zzapp_svc_file_stat(const char *p, zzapp_stat *o) { (void)p; (void)o; return ZZ_ENOSYS; }
WEAK int zzapp_svc_file_list_open(const char *p) { (void)p; return ZZ_ENOSYS; }
WEAK int zzapp_svc_file_list_next(int l, zzapp_dirent *o) { (void)l; (void)o; return ZZ_ENOSYS; }
WEAK int zzapp_svc_file_list_close(int l) { (void)l; return ZZ_ENOSYS; }
WEAK int zzapp_svc_display_open(uint32_t w,uint32_t h,uint32_t f,uint32_t fl,zzapp_surface *o) { (void)w;(void)h;(void)f;(void)fl;(void)o; return ZZ_ENOSYS; }
WEAK int zzapp_svc_display_flip(uint32_t id) { (void)id; return ZZ_ENOSYS; }
WEAK int zzapp_svc_display_flip_async(uint32_t id) { (void)id; return ZZ_ENOSYS; }
WEAK int zzapp_display_flip_idle(void) { return 0; }
WEAK int zzapp_svc_display_refresh(zzapp_surface *o) { (void)o; return ZZ_ENOSYS; }
WEAK int zzapp_svc_display_set_palette(uint32_t id,uint32_t first,uint32_t count,const uint32_t *rgb) { (void)id;(void)first;(void)count;(void)rgb; return ZZ_ENOSYS; }
WEAK uint32_t zzapp_svc_display_mailbox_replaced(void) { return 0U; }
WEAK int zzapp_svc_display_vsync(void) { return ZZ_ENOSYS; }
WEAK int zzapp_svc_display_close(uint32_t id) { (void)id; return ZZ_ENOSYS; }
WEAK int zzapp_svc_input_poll(zzapp_event *o,uint32_t n,uint32_t *out) { (void)o;(void)n; if(out)*out=0; return ZZ_ENOSYS; }
WEAK int zzapp_svc_audio_open(uint32_t r,uint32_t c,zzapp_audio *o) { (void)r;(void)c;(void)o; return ZZ_ENOSYS; }
WEAK int zzapp_svc_audio_close(void) { return ZZ_ENOSYS; }

#ifdef ZZAPP_HOST_TEST
WEAK void sdk_sha256(const uint8_t *data, uint32_t length,
                     uint8_t digest[SDK_SHA256_DIGEST_SIZE])
{
    uint32_t i;

    (void)data;
    (void)length;
    for (i = 0; i < SDK_SHA256_DIGEST_SIZE; ++i)
        digest[i] = 0;
}
WEAK uint16_t sdk_decompress_buffer(uint32_t algorithm, uint32_t flags,
                                    const uint8_t *src, uint32_t src_length,
                                    uint8_t *dst, uint32_t dst_capacity,
                                    struct SDKDecompressResult *result)
{
    (void)algorithm;
    (void)flags;
    (void)src;
    (void)src_length;
    (void)dst;
    (void)dst_capacity;
    (void)result;
    return SDK_STATUS_UNSUPPORTED;
}
#endif
struct zzapp_minigl_binding {
    const zzm_minigl_ops_v1_t *ops;
    void *context;
};

static struct zzapp_minigl_binding g_minigl;

static int minigl_app_ops_ready(const zzm_minigl_ops_v1_t *ops)
{
    return ops && ops->version == ZZM_MINIGL_OPS_V1 &&
           ops->size >= offsetof(zzm_minigl_ops_v1_t, app_reclaim) +
                            sizeof(ops->app_reclaim) &&
           ops->app_bind && ops->app_swap && ops->app_unbind &&
           ops->app_reclaim;
}

static int minigl_registration_ready(const zzm_minigl_registration_v1_t *registration)
{
    return registration &&
           registration->version == ZZM_MINIGL_REGISTRATION_V1 &&
           registration->size >= offsetof(zzm_minigl_registration_v1_t, worker_ops) +
                                 sizeof(registration->worker_ops) &&
           registration->ops && registration->worker_ops;
}

int zzapp_minigl_bind_flags(uint32_t surface_id, uint32_t flags,
                            const zzapp_gl **out)
{
    const zzm_minigl_registration_v1_t *registration;
    const zzm_minigl_ops_v1_t *ops;
    const void *module_gl = NULL;
    int status;
    void *context = NULL;

    if (out)
        *out = NULL;
    if (!surface_id)
        return ZZ_EINVAL;
    if (g_minigl.ops)
        return ZZ_EBUSY;
#ifdef ZZAPP_HOST_TEST
    if (!zzm_loader_acquire || !zzm_loader_release || !zzm_registry_ops ||
        !zzm_minigl_ops)
        return ZZ_ENOSYS;
#endif
    if (!zzm_loader_acquire(ZZM_MODULE_MINIGL))
        return ZZ_ENOSYS;
    registration = (const zzm_minigl_registration_v1_t *)zzm_registry_ops(
        ZZM_MODULE_MINIGL,
        offsetof(zzm_minigl_registration_v1_t, worker_ops) +
            sizeof(registration->worker_ops));
    if (!minigl_registration_ready(registration) ||
        !minigl_app_ops_ready(registration->ops) ||
        zzm_minigl_ops(&context) != registration->ops ||
        context != registration->context)
        goto unavailable;
    ops = registration->ops;
    if (flags != 0U &&
        (ops->size < offsetof(zzm_minigl_ops_v1_t, app_bind_flags) +
                         sizeof(ops->app_bind_flags) ||
         !ops->app_bind_flags))
        goto unavailable;
    status = flags != 0U ? ops->app_bind_flags(context, surface_id, flags, &module_gl) :
                            ops->app_bind(context, surface_id, &module_gl);
    if (status != ZZ_OK)
        goto failed;
    if (!module_gl)
        goto unavailable;
    g_minigl.ops = ops;
    g_minigl.context = context;
    if (out)
        *out = (const zzapp_gl *)module_gl;
    return ZZ_OK;

failed:
    zzm_loader_release(ZZM_MODULE_MINIGL);
    return status;

unavailable:
    zzm_loader_release(ZZM_MODULE_MINIGL);
    return ZZ_ENOSYS;
}

int zzapp_minigl_bind(uint32_t surface_id, const zzapp_gl **out)
{
    return zzapp_minigl_bind_flags(surface_id, 0U, out);
}

void zzapp_minigl_swap(void)
{
    if (g_minigl.ops)
        (void)g_minigl.ops->app_swap(g_minigl.context);
}

void zzapp_minigl_unbind(void)
{
    if (!g_minigl.ops)
        return;
    g_minigl.ops->app_unbind(g_minigl.context);
    g_minigl.ops = NULL;
    g_minigl.context = NULL;
    zzm_loader_release(ZZM_MODULE_MINIGL);
}

void zzapp_minigl_reclaim(uint32_t generation)
{
    if (!g_minigl.ops)
        return;
    g_minigl.ops->app_reclaim(g_minigl.context, generation);
    g_minigl.ops = NULL;
    g_minigl.context = NULL;
    zzm_loader_release(ZZM_MODULE_MINIGL);
}

static void api_exit(int code)
{
    extern void zzapp_host_exit(int);
    zzapp_host_exit(code);
}
static void api_yield(void) { zzapp_api_enter(); zzapp_host_yield(); zzapp_api_leave(); }
static int api_stop(void) { return zzapp_svc_should_stop(); }
static uint64_t api_time(void) { return zzapp_host_time_us(); }
static void api_sleep(uint32_t us)
{
    uint64_t end;

    zzapp_api_enter();
    end = zzapp_host_time_us() + us;
    do {
        /* Sleep has no MiniGL lock; it may service one queued client packet. */
        zzapp_host_yield_wait();
#ifdef ZZAPP_HOST_TEST
        break;
#endif
    } while (!api_stop() && zzapp_host_time_us() < end);
    zzapp_api_leave();
}
/* Firmware allocation is bounded by the app heap. The app's _sbrk uses this
 * same entry, avoiding a second bump pointer in the arena. */
static void *api_alloc(uint32_t bytes, uint32_t align, uint32_t flags)
{
    extern void *zzapp_host_alloc(uint32_t,uint32_t,uint32_t);
    return zzapp_host_alloc(bytes, align, flags);
}
static void api_free(void *p) { extern void zzapp_host_free(void *); zzapp_host_free(p); }
static void api_heap(void **b, uint32_t *n) { extern void zzapp_host_heap(void **,uint32_t *); zzapp_host_heap(b,n); }

static int sdk_to_zz(uint16_t status)
{
    switch (status) {
    case SDK_STATUS_OK: return ZZ_OK;
    case SDK_STATUS_NO_MEMORY: return ZZ_ENOMEM;
    case SDK_STATUS_UNSUPPORTED: return ZZ_ENOSYS;
    case SDK_STATUS_BAD_REQUEST: return ZZ_EINVAL;
    case SDK_STATUS_NOT_FOUND: return ZZ_ENOENT;
    case SDK_STATUS_BUSY: return ZZ_EBUSY;
    default: return ZZ_EIO;
    }
}

static int api_host_address(const void *p, uint32_t *host_offset)
{
    int rc;

    zzapp_api_enter();
    rc = zzapp_host_address(p, host_offset);
    zzapp_api_leave();
    return rc;
}

static int api_mem_info(zzapp_mem_info *out)
{
    int rc;

    zzapp_api_enter();
    rc = zzapp_host_mem_info(out);
    zzapp_api_leave();
    return rc;
}

static int api_map_file(const char *path, const void **data, uint32_t *bytes)
{
    zzapp_stat st;
    uint8_t *dst;
    uint32_t done = 0;
    int fd;
    int rc;

    if (!path || !data || !bytes)
        return ZZ_EINVAL;
    *data = NULL;
    *bytes = 0;
    zzapp_api_enter();
    rc = zzapp_svc_file_stat(path, &st);
    if (rc != ZZ_OK || st.size > (16u << 20) || st.size > UINT32_MAX)
        goto out;
    dst = (uint8_t *)zzapp_host_alloc((uint32_t)st.size, 8u, 0u);
    if (!dst) {
        rc = ZZ_ENOMEM;
        goto out;
    }
    fd = zzapp_svc_file_open(path, ZZ_FILE_READ);
    if (fd < 0) {
        rc = fd;
        goto out;
    }
    rc = ZZ_OK;
    while (done < (uint32_t)st.size) {
        uint32_t got = 0;
        uint32_t want = (uint32_t)st.size - done;

        if (want > 65536u)
            want = 65536u;
        rc = zzapp_svc_file_read(fd, dst + done, want, &got);
        if (rc != ZZ_OK || got == 0u) {
            if (rc == ZZ_OK)
                rc = ZZ_EIO;
            break;
        }
        done += got;
    }
    (void)zzapp_svc_file_close(fd);
    if (rc == ZZ_OK) {
        *data = dst;
        *bytes = done;
    }
out:
    zzapp_api_leave();
    return rc;
}

static void api_unmap_file(const void *data)
{
    /* Arena allocations are bump-only and reclaimed with the app. */
    (void)data;
}

static int api_sha256(const void *data, uint32_t bytes, uint8_t digest[32])
{
    if (!data || !digest)
        return ZZ_EINVAL;
    zzapp_api_enter();
    sdk_sha256(data, bytes, digest);
    zzapp_api_leave();
    return ZZ_OK;
}

static int api_decompress(uint32_t algorithm, uint32_t flags, const void *src,
                          uint32_t src_bytes, void *dst, uint32_t dst_bytes,
                          zzapp_service_result *out)
{
    struct SDKDecompressResult result;
    int rc;

    if (!src || !dst || !out)
        return ZZ_EINVAL;
    zzapp_api_enter();
    rc = sdk_to_zz(sdk_decompress_buffer(algorithm, flags, src, src_bytes,
                                         dst, dst_bytes, &result));
    if (rc == ZZ_OK) {
        out->bytes_consumed = result.bytes_consumed;
        out->bytes_written = result.bytes_written;
        out->flags = result.flags;
    }
    zzapp_api_leave();
    return rc;
}
#define WRAP0(name, call) static int name(void) { zzapp_api_enter(); int r=(call); zzapp_api_leave(); return r; }
#define WRAP1(name, type, call) static int name(type a) { zzapp_api_enter(); int r=(call); zzapp_api_leave(); return r; }
WRAP1(api_file_close,int,zzapp_svc_file_close(a))
WRAP1(api_list_open,const char *,zzapp_svc_file_list_open(a))
WRAP1(api_list_close,int,zzapp_svc_file_list_close(a))
WRAP1(api_display_flip,uint32_t,zzapp_svc_display_flip(a))
WRAP1(api_display_flip_async,uint32_t,zzapp_svc_display_flip_async(a))
WRAP0(api_display_flip_idle,zzapp_display_flip_idle())
static int api_display_refresh(zzapp_surface *o) { zzapp_api_enter(); int r=zzapp_svc_display_refresh(o); zzapp_api_leave(); return r; }
static int api_display_set_palette(uint32_t id,uint32_t first,uint32_t count,const uint32_t *rgb){zzapp_api_enter();int r=zzapp_svc_display_set_palette(id,first,count,rgb);zzapp_api_leave();return r;}
static uint32_t api_display_mailbox_replaced(void){zzapp_api_enter();uint32_t r=zzapp_svc_display_mailbox_replaced();zzapp_api_leave();return r;}
WRAP0(api_vsync,zzapp_svc_display_vsync())
WRAP1(api_display_close,uint32_t,zzapp_svc_display_close(a))
WRAP0(api_audio_close,zzapp_svc_audio_close())
static int api_file_open(const char *p,uint32_t f){zzapp_api_enter();int r=zzapp_svc_file_open(p,f);zzapp_api_leave();return r;}
static int api_file_read(int f,void*p,uint32_t n,uint32_t*o){zzapp_api_enter();int r=zzapp_svc_file_read(f,p,n,o);zzapp_api_leave();return r;}
static int api_file_write(int f,const void*p,uint32_t n,uint32_t*o){zzapp_api_enter();int r=zzapp_svc_file_write(f,p,n,o);zzapp_api_leave();return r;}
static int api_file_seek(int f,int64_t o,int w,int64_t*out){zzapp_api_enter();int r=zzapp_svc_file_seek(f,o,w,out);zzapp_api_leave();return r;}
static int api_file_stat(const char*p,zzapp_stat*out){zzapp_api_enter();int r=zzapp_svc_file_stat(p,out);zzapp_api_leave();return r;}
static int api_list_next(int l,zzapp_dirent*out){zzapp_api_enter();int r=zzapp_svc_file_list_next(l,out);zzapp_api_leave();return r;}
static int api_display_open(uint32_t w,uint32_t h,uint32_t f,uint32_t fl,zzapp_surface*out){zzapp_api_enter();int r=zzapp_svc_display_open(w,h,f,fl,out);zzapp_api_leave();return r;}
static int api_input(zzapp_event*out,uint32_t n,uint32_t*got){zzapp_api_enter();int r=zzapp_svc_input_poll(out,n,got);zzapp_api_leave();return r;}
static int api_audio_open(uint32_t r,uint32_t c,zzapp_audio*out){zzapp_api_enter();int x=zzapp_svc_audio_open(r,c,out);zzapp_api_leave();return x;}
static const zzapp_gl *api_gl_bind(uint32_t id){const zzapp_gl*out=NULL;zzapp_api_enter();if(zzapp_minigl_bind(id,&out)!=ZZ_OK)out=NULL;zzapp_api_leave();return out;}
static const zzapp_gl *api_gl_bind_flags(uint32_t id,uint32_t flags){const zzapp_gl*out=NULL;zzapp_api_enter();if(zzapp_minigl_bind_flags(id,flags,&out)!=ZZ_OK)out=NULL;zzapp_api_leave();return out;}
static void api_gl_swap(void){zzapp_api_enter();zzapp_minigl_swap();zzapp_api_leave();}
static void api_gl_unbind(void){zzapp_api_enter();zzapp_minigl_unbind();zzapp_api_leave();}

static const zzapp_api g_api = {
    .size=sizeof(g_api), .version=ZZAPP_API_VERSION,
    .exit=api_exit,.yield=api_yield,.should_stop=api_stop,.time_us=api_time,.sleep_us=api_sleep,
    .alloc=api_alloc,.free=api_free,.heap_range=api_heap,.log=zzapp_svc_log,
    .file_open=api_file_open,.file_close=api_file_close,.file_read=api_file_read,.file_write=api_file_write,.file_seek=api_file_seek,.file_stat=api_file_stat,.file_list_open=api_list_open,.file_list_next=api_list_next,.file_list_close=api_list_close,
    .display_open=api_display_open,.display_flip=api_display_flip,.display_vsync=api_vsync,.display_close=api_display_close,.input_poll=api_input,.audio_open=api_audio_open,.audio_close=api_audio_close,
    .gl_bind=api_gl_bind,.gl_swap=api_gl_swap,.gl_unbind=api_gl_unbind,
    .display_flip_async=api_display_flip_async,.display_flip_idle=api_display_flip_idle,
    .display_refresh=api_display_refresh,.gl_bind_flags=api_gl_bind_flags,
    .display_set_palette=api_display_set_palette,
    .display_mailbox_replaced=api_display_mailbox_replaced,
    .build_id=ZZ9000_BUILD_GIT,
    .host_address=api_host_address,.mem_info=api_mem_info,
    .map_file=api_map_file,.unmap_file=api_unmap_file,
    .sha256=api_sha256,.decompress=api_decompress
};
const zzapp_api *zzapp_api_table(void) { return &g_api; }
