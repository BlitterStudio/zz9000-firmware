/*
 * Trusted app API table (aar_api, API 0.5). File/display/event/audio bodies
 * are the zzapp_svc_* services; this file adapts them to the table.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef ZZAPP_HOST_TEST
#include <stdlib.h>
#endif
#include "aar/app.h"
#include "zzapp_rec.h"
#include "zzapp_runtime.h"
#include "zzapp_svc.h"
#include "zzm_format.h"
#include "zzm_loader.h"
#include "zzm_minigl_host.h"
#include "zzm_registry.h"
#ifdef ZZAPP_HOST_TEST
#define MINIMP3_IMPLEMENTATION 1
#endif
#include "mp3/minimp3.h"
#include "sdk_audio_codec_io.h"
#include "sdk_audio_flac.h"
#include "sdk_audio_vorbis.h"
#include "sdk_compression.h"
#include "sdk_crypto.h"
#include "sdk_image_stream.h"
#include "zzapp_services.h"
#if defined(ZZAPP_HOST_TEST) && ZZAPP_PERF_COUNTERS
void __attribute__((weak)) zzapp_svc_gl_telemetry_reset(uint32_t pid)
{ (void)pid; }
int __attribute__((weak)) zzapp_svc_gl_telemetry_swap(
    uint32_t pid, uint64_t now_us, zzapp_gl_telemetry_snapshot_t *snapshot)
{
    (void)pid;
    (void)now_us;
    if (snapshot)
        memset(snapshot, 0, sizeof(*snapshot));
    return 0;
}
void __attribute__((weak)) zzapp_svc_gl_telemetry_report(
    uint32_t pid, const zzapp_gl_telemetry_snapshot_t *snapshot)
{ (void)pid; (void)snapshot; }
#endif





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
WEAK int zzapp_services_start_entered(uint32_t generation)
{
    (void)generation;
    return AAR_OK;
}
WEAK void zzapp_services_on_reset(void) {}
WEAK void zzapp_services_on_app_end(void) {}
WEAK void zzapp_services_cancel_start(int status) { (void)status; }
WEAK void zzapp_services_on_launch(void) {}
WEAK void zzapp_svc_log(const char *msg, uint32_t len) { (void)msg; (void)len; }
WEAK int zzapp_svc_should_stop(void) { return 0; }
WEAK int zzapp_svc_file_open(const char *p, uint32_t f) { (void)p; (void)f; return AAR_E_NOSYS; }
WEAK int zzapp_svc_file_close(int fd) { (void)fd; return AAR_E_NOSYS; }
WEAK int zzapp_svc_file_read(int fd, void *p, uint32_t n, uint32_t *o) { (void)fd; (void)p; (void)n; if (o) *o = 0; return AAR_E_NOSYS; }
WEAK int zzapp_svc_file_write(int fd, const void *p, uint32_t n, uint32_t *o) { (void)fd; (void)p; (void)n; if (o) *o = 0; return AAR_E_NOSYS; }
WEAK int zzapp_svc_file_seek(int fd, int64_t o, int w, int64_t *out) { (void)fd; (void)o; (void)w; if (out) *out = 0; return AAR_E_NOSYS; }
WEAK int zzapp_svc_file_stat(const char *p, aar_stat *o) { (void)p; (void)o; return AAR_E_NOSYS; }
WEAK int zzapp_svc_file_list_open(const char *p) { (void)p; return AAR_E_NOSYS; }
WEAK int zzapp_svc_file_list_next(int l, aar_dirent *o) { (void)l; (void)o; return AAR_E_NOSYS; }
WEAK int zzapp_svc_file_list_close(int l) { (void)l; return AAR_E_NOSYS; }
WEAK int zzapp_svc_display_open(uint32_t w,uint32_t h,uint32_t f,uint32_t fl,aar_surface *o) { (void)w;(void)h;(void)f;(void)fl;(void)o; return AAR_E_NOSYS; }
WEAK int zzapp_svc_display_flip(uint32_t id) { (void)id; return AAR_E_NOSYS; }
WEAK int zzapp_svc_display_flip_async(uint32_t id) { (void)id; return AAR_E_NOSYS; }
WEAK int zzapp_display_flip_idle(void) { return 0; }
WEAK uint32_t zzapp_display_id(void) { return 0U; }
WEAK int zzapp_svc_display_refresh(aar_surface *o) { (void)o; return AAR_E_NOSYS; }
WEAK int zzapp_svc_display_set_palette(uint32_t id,uint32_t first,uint32_t count,const uint32_t *rgb) { (void)id;(void)first;(void)count;(void)rgb; return AAR_E_NOSYS; }
WEAK uint32_t zzapp_svc_display_latest_dropped(void) { return 0U; }
WEAK int zzapp_svc_display_vsync(void) { return AAR_E_NOSYS; }
WEAK int zzapp_svc_display_close(uint32_t id) { (void)id; return AAR_E_NOSYS; }
WEAK int zzapp_svc_event_poll(aar_event *o,uint32_t n,uint32_t *out) { (void)o;(void)n; if(out)*out=0; return AAR_E_NOSYS; }
WEAK int zzapp_svc_audio_open(uint32_t r,uint32_t c,aar_audio *o) { (void)r;(void)c;(void)o; return AAR_E_NOSYS; }
WEAK int zzapp_svc_audio_close(void) { return AAR_E_NOSYS; }
#ifdef ZZAPP_HOST_TEST
WEAK int zzapp_svc_file_delete(const char *path)
{ (void)path; return AAR_E_NOSYS; }
WEAK int zzapp_svc_file_rename(const char *from, const char *to, uint32_t flags)
{ (void)from; (void)to; (void)flags; return AAR_E_NOSYS; }
WEAK int zzapp_svc_dir_create(const char *path)
{ (void)path; return AAR_E_NOSYS; }
WEAK int zzapp_svc_file_set_date(const char *path, int64_t mtime)
{ (void)path; (void)mtime; return AAR_E_NOSYS; }
WEAK int zzapp_svc_file_set_protect(const char *path, uint32_t protect)
{ (void)path; (void)protect; return AAR_E_NOSYS; }
WEAK int zzapp_svc_file_truncate(int fd, uint64_t size)
{ (void)fd; (void)size; return AAR_E_NOSYS; }
WEAK int zzapp_svc_volume_info(const char *path, aar_volume_info *out)
{ (void)path; (void)out; return AAR_E_NOSYS; }
WEAK int zzapp_svc_time_unix_us(int64_t *out)
{ (void)out; return AAR_E_NOSYS; }
WEAK int zzapp_svc_locale_info(aar_locale_info *out)
{ (void)out; return AAR_E_NOSYS; }
WEAK int zzapp_svc_env_get(const char *name, char *buf, uint32_t bytes, uint32_t *out)
{ (void)name; (void)buf; (void)bytes; (void)out; return AAR_E_NOSYS; }
WEAK int zzapp_svc_clip_read(uint32_t unit, uint32_t type, void *buf,
                             uint32_t bytes, uint32_t *out)
{ (void)unit; (void)type; (void)buf; (void)bytes; (void)out; return AAR_E_NOSYS; }
WEAK int zzapp_svc_clip_write(uint32_t unit, uint32_t type, const void *data,
                              uint32_t bytes)
{ (void)unit; (void)type; (void)data; (void)bytes; return AAR_E_NOSYS; }
WEAK int zzapp_svc_file_get_comment(const char *path, void *buf, uint32_t bytes,
                                    uint32_t *out)
{ (void)path; (void)buf; (void)bytes; (void)out; return AAR_E_NOSYS; }
WEAK int zzapp_svc_file_set_comment(const char *path, const void *data,
                                    uint32_t bytes)
{ (void)path; (void)data; (void)bytes; return AAR_E_NOSYS; }
WEAK int zzapp_svc_display_info(uint32_t id, aar_display_info *out)
{ (void)id; (void)out; return AAR_E_NOSYS; }
WEAK int zzapp_svc_display_modes(aar_display_mode *out, uint32_t max, uint32_t *count)
{ (void)out; (void)max; (void)count; return AAR_E_NOSYS; }
WEAK int zzapp_svc_display_resize(uint32_t id, uint32_t w, uint32_t h, aar_surface *out)
{ (void)id; (void)w; (void)h; (void)out; return AAR_E_NOSYS; }
WEAK int zzapp_svc_display_set_title(uint32_t id, const char *title)
{ (void)id; (void)title; return AAR_E_NOSYS; }
WEAK int zzapp_svc_display_pointer(uint32_t id, uint32_t mode, const aar_pointer *image)
{ (void)id; (void)mode; (void)image; return AAR_E_NOSYS; }
WEAK int zzapp_svc_display_wait_vblank(uint32_t timeout)
{ (void)timeout; return AAR_E_NOSYS; }
WEAK uint64_t zzapp_svc_display_last_present_us(void)
{ return 0U; }
#endif
WEAK void zzapp_svc_gl_release(uint32_t pid, uint32_t generation)
{ (void)pid; (void)generation; }

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
WEAK uint16_t sdk_decompress_buffer_owned(uint32_t algorithm, uint32_t flags,
                                          const uint8_t *src,
                                          uint32_t src_length,
                                          uint8_t *dst, uint32_t dst_capacity,
                                          struct SDKDecompressResult *result,
                                          uint32_t scratch_owner)
{
    (void)algorithm;
    (void)flags;
    (void)src;
    (void)src_length;
    (void)dst;
    (void)dst_capacity;
    (void)result;
    (void)scratch_owner;
    if (result)
        memset(result, 0, sizeof(*result));
    return SDK_STATUS_UNSUPPORTED;
}
WEAK unsigned sdk_compression_reclaim_core1_decode(void) { return 0; }
WEAK unsigned sdk_compression_reclaim_app_decode(uint32_t owner)
{ (void)owner; return 0; }
WEAK uint16_t sdk_image_stream_begin(const struct SDKImageStreamBegin *b,
                                     struct SDKImageStreamResult *r)
{ (void)b; (void)r; return SDK_STATUS_UNSUPPORTED; }
WEAK uint16_t sdk_image_stream_feed(const struct SDKImageStreamFeed *f,
                                    const uint8_t *s,
                                    struct SDKImageStreamResult *r)
{ (void)f; (void)s; (void)r; return SDK_STATUS_UNSUPPORTED; }
WEAK uint16_t sdk_image_stream_close(uint32_t s)
{ (void)s; return SDK_STATUS_UNSUPPORTED; }
WEAK uint16_t sdk_image_stream_release_poisoned(uint32_t s)
{ (void)s; return SDK_STATUS_UNSUPPORTED; }
WEAK void sdk_image_stream_poison_core1_sessions(void) {}
WEAK void sdk_image_stream_poison_session(uint32_t s) { (void)s; }
WEAK void sdk_flac_init(struct sdk_flac_state *state, uint32_t format)
{ (void)state; (void)format; }
WEAK uint16_t sdk_flac_decode(struct sdk_flac_state *state,
                              struct sdk_audio_codec_io *io)
{ (void)state; (void)io; return SDK_STATUS_UNSUPPORTED; }
WEAK void sdk_flac_release(struct sdk_flac_state *state) { (void)state; }
WEAK void sdk_flac_forget(struct sdk_flac_state *state) { (void)state; }
WEAK void sdk_vorbis_init(struct sdk_vorbis_state *state, uint32_t format)
{ (void)state; (void)format; }
WEAK uint16_t sdk_vorbis_decode(struct sdk_vorbis_state *state,
                                struct sdk_audio_codec_io *io)
{ (void)state; (void)io; return SDK_STATUS_UNSUPPORTED; }
WEAK void sdk_vorbis_release(struct sdk_vorbis_state *state) { (void)state; }
WEAK void sdk_vorbis_forget(struct sdk_vorbis_state *state) { (void)state; }
WEAK int zzapp_svc_check_user(const zzapp_as_t *as, uint32_t address,
                               uint32_t bytes, int write)
{ (void)as; (void)address; (void)bytes; (void)write; return AAR_OK; }
#endif
WEAK int zzapp_svc_console_write(const void *p,uint32_t n,uint32_t *o) { (void)p;(void)n;if(o)*o=0;return AAR_E_NOSYS; }
WEAK int zzapp_svc_console_read(void *p,uint32_t n,uint32_t *o) { (void)p;(void)n;if(o)*o=0;return AAR_E_NOSYS; }
WEAK int zzapp_svc_msg_send(const void *p) { (void)p; return AAR_E_NOSYS; }
WEAK int zzapp_svc_msg_poll(void *p) { (void)p; return AAR_E_NOSYS; }
WEAK int zzapp_svc_msg_wait(uint32_t timeout_us) { (void)timeout_us; return AAR_E_NOSYS; }
struct zzapp_minigl_binding {
    const zzm_minigl_ops_v1_t *ops;
    void *context;
    uint32_t owner;
    uint32_t generation;
};

static struct zzapp_minigl_binding g_minigl;
static uint32_t api_pid(void);
static int minigl_binding_matches(uint32_t owner, uint32_t generation);
static uint32_t app_decode_owner(void);

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
                            const aar_gl **out)
{
    const zzm_minigl_registration_v1_t *registration;
    const zzm_minigl_ops_v1_t *ops;
    const void *module_gl = NULL;
    uint32_t owner = api_pid();
    uint32_t generation = zzapp_current_generation();
    int status;
    void *context = NULL;

    if (out)
        *out = NULL;
    /* Only the app's open display can be bound. */
    if (!surface_id || surface_id != zzapp_display_id())
        return AAR_E_INVAL;
    if (g_minigl.ops ||
        zzapp_kernel_gl_bind(zzapp_runtime_kernel(), owner, generation) != 0)
        return AAR_E_BUSY;
#ifdef ZZAPP_HOST_TEST
    if (!zzm_loader_acquire || !zzm_loader_release || !zzm_registry_ops ||
        !zzm_minigl_ops) {
        zzapp_kernel_gl_unbind(zzapp_runtime_kernel(), owner, generation);
        return AAR_E_NOSYS;
    }
#endif
    for (;;) {
        status = zzm_loader_acquire(ZZM_MODULE_MINIGL);
        if (status != ZZM_LOAD_IN_PROGRESS)
            break;
        zzapp_host_yield_wait();
    }
    if (status != ZZM_LOAD_READY) {
        zzapp_kernel_gl_unbind(zzapp_runtime_kernel(), owner, generation);
        return AAR_E_NOSYS;
    }
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
    if (status != AAR_OK)
        goto failed;
    if (!module_gl)
        goto unavailable;
    g_minigl.ops = ops;
    g_minigl.context = context;
    g_minigl.owner = owner;
    g_minigl.generation = generation;
#if ZZAPP_PERF_COUNTERS
    zzapp_svc_gl_telemetry_reset(owner);
#endif
    if (out)
        *out = (const aar_gl *)module_gl;
    return AAR_OK;

failed:
    zzapp_kernel_gl_unbind(zzapp_runtime_kernel(), owner, generation);
    zzm_loader_release(ZZM_MODULE_MINIGL);
    return status;

unavailable:
    zzapp_kernel_gl_unbind(zzapp_runtime_kernel(), owner, generation);
    zzm_loader_release(ZZM_MODULE_MINIGL);
    return AAR_E_NOSYS;
}

int zzapp_minigl_bind(uint32_t surface_id, const aar_gl **out)
{
    return zzapp_minigl_bind_flags(surface_id, 0U, out);
}

int zzapp_minigl_swap(void)
{
    uint32_t owner = api_pid();
    uint32_t generation = zzapp_current_generation();

    if (!minigl_binding_matches(owner, generation))
        return AAR_E_BUSY;
    (void)g_minigl.ops->app_swap(g_minigl.context);
    return AAR_OK;
}

static int minigl_binding_matches(uint32_t owner, uint32_t generation)
{
    return g_minigl.ops && g_minigl.owner == owner &&
           g_minigl.generation == generation;
}

static void minigl_drop(void)
{
    uint32_t owner = g_minigl.owner;
    uint32_t generation = g_minigl.generation;

    g_minigl.ops = NULL;
    g_minigl.context = NULL;
    g_minigl.owner = 0U;
    g_minigl.generation = 0U;
#if ZZAPP_PERF_COUNTERS
    zzapp_svc_gl_telemetry_reset(owner);
#endif
    zzapp_svc_gl_release(owner, generation);
    zzapp_kernel_gl_unbind(zzapp_runtime_kernel(), owner, generation);
    zzm_loader_release(ZZM_MODULE_MINIGL);
}

void zzapp_minigl_unbind(void)
{
    uint32_t owner = api_pid();
    uint32_t generation = zzapp_current_generation();

    if (!minigl_binding_matches(owner, generation))
        return;
    g_minigl.ops->app_unbind(g_minigl.context);
    minigl_drop();
}

void zzapp_minigl_reclaim(uint32_t generation)
{
    uint32_t owner = api_pid();

    if (!minigl_binding_matches(owner, generation))
        return;
    g_minigl.ops->app_reclaim(g_minigl.context, generation);
    minigl_drop();
}

/* Core 0 only, while core 1 is halted and its module image is about to be
 * poisoned. Drops a live binding without entering module code. */
void zzapp_minigl_forget(void)
{
    if (!minigl_binding_matches(g_minigl.owner, g_minigl.generation))
        return;
    minigl_drop();
}

static void api_exit(int code)
{
    extern void zzapp_host_exit(int);
    zzapp_host_exit(code);
}
static void api_yield(void) { zzapp_host_yield(); }
static int api_stop(void)
{
    zzapp_api_touch();
    return zzapp_host_stop_requested();
}
static uint64_t api_time(void)
{
    zzapp_api_touch();
    return zzapp_host_time_us();
}
static void api_sleep(uint32_t us)
{
    zzapp_api_enter();
    zzapp_host_sleep(us);
    zzapp_api_leave();
}
static void api_heap_range(void **base, uint64_t *bytes)
{
    uint32_t n = 0U;

    zzapp_host_heap(base, &n);
    if (bytes)
        *bytes = n;
}

/* A whole number of granules, never zero; a count beyond the 32-bit address
 * space cannot be satisfied (grow) or exceeds the heap (shrink). The break
 * pointer is optional: the app may not care where the heap ended. */
static int api_heap_grow(uint64_t bytes, void **old_break)
{
    int rc;

    if (bytes == 0U || (bytes % ZZAPP_MEM_FRAME_BYTES) != 0U)
        return AAR_E_INVAL;
    if (bytes > UINT32_MAX)
        return AAR_E_NOMEM;
    zzapp_api_enter();
    rc = zzapp_proc_heap_grow((uint32_t)bytes, old_break);
    zzapp_api_leave();
    return rc;
}

static int api_heap_shrink(uint64_t bytes, void **new_break)
{
    int rc;

    if (bytes == 0U || (bytes % ZZAPP_MEM_FRAME_BYTES) != 0U ||
        bytes > UINT32_MAX)
        return AAR_E_INVAL;
    zzapp_api_enter();
    rc = zzapp_proc_heap_shrink((uint32_t)bytes, new_break);
    zzapp_api_leave();
    return rc;
}

static int api_heap_info(aar_heap_info *out)
{
    uint32_t committed;
    uint32_t limit;
    uint32_t pool_free;
    int rc;

    if (!out)
        return AAR_E_INVAL;
    zzapp_api_enter();
    rc = zzapp_proc_heap_info(&committed, &limit, &pool_free);
    zzapp_api_leave();
    if (rc == AAR_OK) {
        out->committed = committed;
        out->limit = limit;
        out->pool_free = pool_free;
    }
    return rc;
}

static int api_info(aar_info *out)
{
    int rc;

    zzapp_api_enter();
    rc = zzapp_host_info(out);
    zzapp_api_leave();
    return rc;
}


static int sdk_to_zz(uint16_t status)
{
    switch (status) {
    case SDK_STATUS_OK: return AAR_OK;
    case SDK_STATUS_NO_MEMORY: return AAR_E_NOMEM;
    case SDK_STATUS_UNSUPPORTED: return AAR_E_NOSYS;
    case SDK_STATUS_BAD_REQUEST: return AAR_E_INVAL;
    case SDK_STATUS_NOT_FOUND: return AAR_E_NOENT;
    case SDK_STATUS_BUSY: return AAR_E_BUSY;
    default: return AAR_E_IO;
    }
}

static int api_shared_alloc(uint64_t bytes, uint32_t align, uint32_t flags,
                            aar_shared *out)
{
    int rc;

    zzapp_api_enter();
    rc = zzapp_host_shared_alloc(bytes, align, flags, out);
    zzapp_api_leave();
    return rc;
}

static int api_shared_free(void *p)
{
    int rc;

    zzapp_api_enter();
    rc = zzapp_host_free(p);
    zzapp_api_leave();
    return rc;
}

/* The ZZ9000's shared memory is coherent with the ARM caches (the Zorro
 * master goes through the ACP), so publish and acquire only order the
 * bus: a store barrier before the message that announces the data, a load
 * barrier before reading what the host wrote. */
static int api_publish(const void *p, uint64_t bytes)
{
    int rc;

    zzapp_api_enter();
    rc = zzapp_host_shared_range(p, bytes);
    if (rc == AAR_OK)
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    zzapp_api_leave();
    return rc;
}

static int api_acquire(const void *p, uint64_t bytes)
{
    int rc;

    zzapp_api_enter();
    rc = zzapp_host_shared_range(p, bytes);
    if (rc == AAR_OK)
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    zzapp_api_leave();
    return rc;
}

static int api_map_file(const char *path, const void **data, uint64_t *bytes)
{
    aar_stat st;
    uint8_t *dst;
    uint32_t done = 0;
    int fd;
    int rc;

    if (!path || !data || !bytes)
        return AAR_E_INVAL;
    *data = NULL;
    *bytes = 0;
    zzapp_api_enter();
    rc = zzapp_svc_file_stat(path, &st);
    if (zzapp_host_faulted()) {
        rc = AAR_E_FAULT;
        goto out;
    }
    if (rc != AAR_OK)
        goto out;
    if (st.size > (16u << 20) || st.size > UINT32_MAX) {
        rc = AAR_E_NOSPC;
        goto out;
    }
    dst = (uint8_t *)zzapp_host_alloc((uint32_t)st.size, 8u);
    if (!dst) {
        rc = AAR_E_NOMEM;
        goto out;
    }
    if (zzapp_host_faulted()) {
        rc = AAR_E_FAULT;
        goto free_dst;
    }
    fd = zzapp_svc_file_open(path, AAR_FILE_READ);
    if (zzapp_host_faulted()) {
        rc = AAR_E_FAULT;
        goto free_dst;
    }
    if (fd < 0) {
        rc = fd;
        goto free_dst;
    }
    rc = AAR_OK;
    while (done < (uint32_t)st.size) {
        uint32_t got = 0;
        uint32_t want = (uint32_t)st.size - done;

        if (want > 65536u)
            want = 65536u;
        rc = zzapp_svc_file_read(fd, dst + done, want, &got);
        if (zzapp_host_faulted()) {
            rc = AAR_E_FAULT;
            break;
        }
        if (rc != AAR_OK || got == 0u) {
            if (rc == AAR_OK)
                rc = AAR_E_IO;
            break;
        }
        done += got;
    }
    (void)zzapp_svc_file_close(fd);
    if (rc != AAR_OK)
        goto free_dst;
    *data = dst;
    *bytes = done;
    goto out;

free_dst:
    (void)zzapp_host_free(dst);
out:
    zzapp_api_leave();
    return rc;
}

static void api_unmap_file(const void *data)
{
    if (!data)
        return;
    zzapp_api_enter();
    (void)zzapp_host_free((void *)data);
    zzapp_api_leave();
}

/* The firmware's services take 32-bit byte counts: a whole-buffer length
 * above that is AAR_E_INVAL. */
static int api_sha256(const void *data, uint64_t bytes, uint8_t digest[32])
{
    if (!data || !digest || bytes > UINT32_MAX)
        return AAR_E_INVAL;
    zzapp_api_enter();
    sdk_sha256(data, (uint32_t)bytes, digest);
    zzapp_api_leave();
    return AAR_OK;
}

static int api_decompress(uint32_t algorithm, uint32_t flags, const void *src,
                          uint64_t src_bytes, void *dst, uint64_t dst_bytes,
                          aar_service_result *out)
{
    struct SDKDecompressResult result;
    int rc;

    if (!src || !dst || !out || src_bytes > UINT32_MAX || dst_bytes > UINT32_MAX)
        return AAR_E_INVAL;
    zzapp_api_enter();
    /* Scratch under the app owner: a force-stop mid-call skips inflateEnd
     * / LZMA cleanup, and zzapp_api_poison_resources reclaims exactly this
     * owner instead of leaking into the shared decode pool. */
    rc = sdk_to_zz(sdk_decompress_buffer_owned(algorithm, flags, src,
                                               (uint32_t)src_bytes, dst,
                                               (uint32_t)dst_bytes,
                                               &result, app_decode_owner()));
    if (rc == AAR_OK) {
        out->bytes_consumed = result.bytes_consumed;
        out->bytes_written = result.bytes_written;
        out->flags = result.flags;
    }
    zzapp_api_leave();
    return rc;
}

#define ZZAPP_DECODE_SESSIONS 4u

/* The record the call runs for. */
static uint32_t api_pid(void)
{
    return zzapp_services_current_pid();
}

static uint32_t app_decode_owner(void)
{
    return sdk_compression_app_decode_owner(api_pid());
}

struct zzapp_image_session {
    uint32_t id;
    uint32_t tile_address;
    uint32_t tile_bytes;
};

struct zzapp_audio_session {
    uint32_t id;
    uint32_t codec;
    union {
        mp3dec_t mp3;
        struct sdk_flac_state flac;
        struct sdk_vorbis_state vorbis;
    } state;
};

/* An app's audio decoders. The block is large, so it exists only while a
 * session is open: allocated on the first open under the app's decode owner,
 * returned with the last close or at retire. */
struct zzapp_audio_sessions {
    struct zzapp_audio_session session[ZZAPP_DECODE_SESSIONS];
};

/* What this file keeps per app, overlaid on the decode part of its process
 * record. */
struct zzapp_decode_state {
    struct zzapp_image_session image[ZZAPP_DECODE_SESSIONS];
    struct zzapp_audio_sessions *audio;
    uint32_t next_audio_session;
};
_Static_assert(sizeof(struct zzapp_decode_state) <= ZZAPP_REC_DECODE_BYTES,
               "decode state fits the record's decode part");

/* NULL for a pid outside the record table. */
static struct zzapp_decode_state *decode_state(uint32_t pid)
{
    zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

    return rec ? (struct zzapp_decode_state *)(void *)rec->decode.bytes : NULL;
}

static void image_result(const struct SDKImageStreamResult *result,
                         aar_image_result *out)
{
    out->image_width = result->image_width;
    out->image_height = result->image_height;
    out->bytes_consumed = result->bytes_consumed;
    out->bytes_written = result->bytes_written;
    out->flags = result->flags;
}

static int api_image_open(const aar_image_params *open, uint32_t *session,
                          aar_image_result *out)
{
    struct SDKImageStreamBegin begin;
    struct SDKImageStreamResult result;
    uint16_t status;
    uint32_t pid = api_pid();
    struct zzapp_decode_state *ds = decode_state(pid);
    uint32_t i;
    int rc;

    if (!open || !session || !out || !open->tile || !open->stride ||
        !open->rows || !open->tile_bytes ||
        open->output_format != AAR_FMT_BGRA8888)
        return AAR_E_INVAL;
    if (!ds)
        return AAR_E_FAULT;
    for (i = 0; i < ZZAPP_DECODE_SESSIONS && ds->image[i].id; ++i) {}
    if (i == ZZAPP_DECODE_SESSIONS ||
        zzapp_kernel_session_open(zzapp_runtime_kernel(), pid, 0) != 0)
        return AAR_E_NOMEM;
    memset(&begin, 0, sizeof(begin));
    begin.codec = open->codec;
    begin.output_mode = SDK_IMAGE_OUTPUT_TILE_BUFFER;
    begin.output_format = SDK_SURFACE_FORMAT_BGRA8888;
    begin.tile_stride = open->stride;
    begin.tile_rows = open->rows;
    begin.tile_address = (uintptr_t)open->tile;
    begin.tile_length = open->tile_bytes;
    begin.flags = open->flags;
    begin.core1_affine = 1u;
    begin.direct_arm_local = 1u;
    zzapp_api_enter();
    /* begin releases its lock, and that release restores interrupts,
     * before it returns. Hold the same commit guard host-visible
     * allocations use until the ledger records the new session. */
    zzapp_host_commit_begin();
    do {
        status = sdk_image_stream_begin(&begin, &result);
        if (status != SDK_TASK_IN_PROGRESS)
            break;
        zzapp_host_yield_wait();
    } while (!api_stop() && !zzapp_host_faulted());
    if (status == SDK_TASK_IN_PROGRESS)
        status = SDK_STATUS_BUSY;
    rc = sdk_to_zz(status);
    if (rc == AAR_OK) {
        ds->image[i].id = result.session;
        ds->image[i].tile_address = (uint32_t)(uintptr_t)open->tile;
        ds->image[i].tile_bytes = open->tile_bytes;
        zzapp_proc_slot(pid)->image_sessions++;
    } else
        zzapp_kernel_session_close(zzapp_runtime_kernel(), pid, 0);
    zzapp_host_commit_end();
    zzapp_api_leave();
    if (rc == AAR_OK) {
        *session = result.session;
        image_result(&result, out);
    }
    return rc;
}

static int api_image_decode(uint32_t session, const void *src, uint32_t bytes,
                            uint32_t flags, aar_image_result *out)
{
    struct SDKImageStreamFeed feed;
    struct SDKImageStreamResult result;
    uint32_t pid = api_pid();
    struct zzapp_decode_state *ds = decode_state(pid);
    uint32_t i;
    int rc;

    if (!src || !out)
        return AAR_E_INVAL;
    if (!ds)
        return AAR_E_FAULT;
    if (!session)
        return AAR_E_INVAL;
    for (i = 0; i < ZZAPP_DECODE_SESSIONS && ds->image[i].id != session; ++i) {}
    if (i == ZZAPP_DECODE_SESSIONS)
        return AAR_E_INVAL;
    if (zzapp_svc_check_user(&zzapp_proc_slot(pid)->as,
                             ds->image[i].tile_address,
                             ds->image[i].tile_bytes, 1) != AAR_OK)
        return AAR_E_FAULT;
    memset(&feed, 0, sizeof(feed));
    feed.session = session;
    feed.src_length = bytes;
    feed.flags = flags;
    zzapp_api_enter();
    rc = sdk_to_zz(sdk_image_stream_feed(&feed, src, &result));
    zzapp_api_leave();
    if (rc == AAR_OK)
        image_result(&result, out);
    return rc;
}

static int api_image_close(uint32_t session)
{
    uint32_t pid = api_pid();
    struct zzapp_decode_state *ds = decode_state(pid);
    uint32_t i;
    int rc;

    if (!ds)
        return AAR_E_FAULT;
    if (!session)
        return AAR_E_INVAL;
    for (i = 0; i < ZZAPP_DECODE_SESSIONS && ds->image[i].id != session; ++i) {}
    if (i == ZZAPP_DECODE_SESSIONS)
        return AAR_E_INVAL;
    zzapp_api_enter();
    rc = sdk_to_zz(sdk_image_stream_close(session));
    zzapp_api_leave();
    if (rc == AAR_OK) {
        memset(&ds->image[i], 0, sizeof(ds->image[i]));
        zzapp_kernel_session_close(zzapp_runtime_kernel(), pid, 0);
        if (zzapp_proc_slot(pid)->image_sessions)
            zzapp_proc_slot(pid)->image_sessions--;
    }
    return rc;
}

static struct zzapp_audio_session *audio_session_find(uint32_t session)
{
    struct zzapp_decode_state *ds = decode_state(api_pid());
    uint32_t i;

    /* Session 0 is no session: an empty slot has id 0. */
    if (!session || !ds || !ds->audio)
        return NULL;
    for (i = 0; i < ZZAPP_DECODE_SESSIONS; ++i)
        if (ds->audio->session[i].id == session)
            return &ds->audio->session[i];
    return NULL;
}

static void audio_session_release(struct zzapp_audio_session *session)
{
    if (session->codec == AAR_AUDIO_FLAC)
        sdk_flac_release(&session->state.flac);
    else if (session->codec == AAR_AUDIO_VORBIS)
        sdk_vorbis_release(&session->state.vorbis);
    memset(session, 0, sizeof(*session));
}

/* Give the audio block back once no session uses it. The pointer is cleared
 * before the block is freed. */
static void audio_block_release_if_idle(struct zzapp_decode_state *ds)
{
    struct zzapp_audio_sessions *block = ds->audio;
    uint32_t i;

    if (!block)
        return;
    for (i = 0; i < ZZAPP_DECODE_SESSIONS; ++i)
        if (block->session[i].id)
            return;
    ds->audio = NULL;
    sdk_decode_heap_free(block);
}

static int api_audio_open_decode(uint32_t codec, uint32_t *session)
{
    struct zzapp_audio_session *slot = NULL;
    uint32_t pid = api_pid();
    struct zzapp_decode_state *ds = decode_state(pid);
    uint32_t i;

    if (!session || (codec != AAR_AUDIO_MP3 && codec != AAR_AUDIO_FLAC &&
                     codec != AAR_AUDIO_VORBIS))
        return AAR_E_INVAL;
    if (!ds)
        return AAR_E_FAULT;
    if (!ds->audio) {
        /* The decoders are large (a few KiB each), and most apps open
         * none: the block exists only while a session is open. */
        ds->audio = sdk_decode_heap_alloc_owned(sizeof(*ds->audio),
                                                app_decode_owner());
        if (!ds->audio)
            return AAR_E_NOMEM;
        memset(ds->audio, 0, sizeof(*ds->audio));
    }
    for (i = 0; i < ZZAPP_DECODE_SESSIONS; ++i)
        if (ds->audio->session[i].id == 0u) {
            slot = &ds->audio->session[i];
            break;
        }
    if (!slot ||
        zzapp_kernel_session_open(zzapp_runtime_kernel(), pid, 1) != 0) {
        audio_block_release_if_idle(ds);
        return AAR_E_NOMEM;
    }
    memset(slot, 0, sizeof(*slot));
    slot->id = 0x80000000u | ++ds->next_audio_session;
    if (ds->next_audio_session == 0u)
        ds->next_audio_session = 1u;
    slot->codec = codec;
    if (codec == AAR_AUDIO_MP3)
        mp3dec_init(&slot->state.mp3);
    else if (codec == AAR_AUDIO_FLAC) {
        sdk_flac_init(&slot->state.flac, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
        slot->state.flac.pool_owner = app_decode_owner();
    } else {
        sdk_vorbis_init(&slot->state.vorbis, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
        slot->state.vorbis.heap.pool_owner = app_decode_owner();
    }
    zzapp_proc_slot(pid)->audio_sessions++;
    *session = slot->id;
    return AAR_OK;
}

static int api_audio_decode_app(uint32_t session, const void *src,
                                uint32_t src_bytes, uint32_t flags,
                                int16_t *pcm, uint32_t pcm_frames,
                                aar_audio_decode_result *out)
{
    struct zzapp_audio_session *slot;
    struct sdk_audio_codec_io io;
    mp3dec_frame_info_t info;
    int samples;
    uint16_t status;
    uint32_t channels;
    int rc;

    if ((!src && src_bytes) || !pcm || !pcm_frames || !out ||
        (flags & ~(AAR_AUDIO_DECODE_EOF | AAR_AUDIO_DECODE_DRAIN)) != 0u)
        return AAR_E_INVAL;
    slot = audio_session_find(session);
    if (!slot)
        return AAR_E_INVAL;
    memset(out, 0, sizeof(*out));
    zzapp_api_enter();
    if (slot->codec == AAR_AUDIO_MP3) {
        if (!src || !src_bytes || flags != 0u) {
            rc = AAR_E_INVAL;
        } else if (pcm_frames < MINIMP3_MAX_SAMPLES_PER_FRAME / 2u ||
                   src_bytes > INT32_MAX) {
            rc = AAR_E_NOSPC;
        } else {
            memset(&info, 0, sizeof(info));
            samples = mp3dec_decode_frame(&slot->state.mp3, src,
                                          (int)src_bytes, pcm, &info);
            if (samples < 0 || info.frame_bytes <= 0 ||
                (uint32_t)info.frame_bytes > src_bytes) {
                rc = AAR_E_IO;
            } else {
                channels = (uint32_t)info.channels;
                if (!channels || (uint32_t)samples > pcm_frames) {
                    rc = AAR_E_NOSPC;
                } else {
                    out->bytes_consumed = (uint32_t)info.frame_bytes;
                    out->frames_written = (uint32_t)samples;
                    out->rate = (uint32_t)info.hz;
                    out->channels = channels;
                    rc = AAR_OK;
                }
            }
        }
    } else {
        memset(&io, 0, sizeof(io));
        io.input = src;
        io.input_length = src_bytes;
        io.eof = (flags & AAR_AUDIO_DECODE_EOF) != 0u;
        io.drain = (flags & AAR_AUDIO_DECODE_DRAIN) != 0u;
        io.pcm = (uint8_t *)pcm;
        if (pcm_frames > UINT32_MAX / 4u) {
            rc = AAR_E_NOSPC;
        } else {
            io.pcm_capacity = pcm_frames * 4u;
            io.pcm_free = io.pcm_capacity;
            io.pcm_budget = io.pcm_capacity;
            status = slot->codec == AAR_AUDIO_FLAC ?
                sdk_flac_decode(&slot->state.flac, &io) :
                sdk_vorbis_decode(&slot->state.vorbis, &io);
            rc = sdk_to_zz(status);
            channels = slot->codec == AAR_AUDIO_FLAC ?
                slot->state.flac.channels : slot->state.vorbis.channels;
            if (rc == AAR_OK) {
                out->bytes_consumed = io.consumed;
                out->frames_written = channels ?
                    io.produced / (channels * 2u) : 0u;
                out->rate = slot->codec == AAR_AUDIO_FLAC ?
                    slot->state.flac.sample_rate :
                    slot->state.vorbis.sample_rate;
                out->channels = channels;
            }
        }
    }
    zzapp_api_leave();
    return rc;
}

static int api_audio_close_decode(uint32_t session)
{
    uint32_t pid = api_pid();
    struct zzapp_decode_state *ds = decode_state(pid);
    struct zzapp_audio_session *slot = audio_session_find(session);

    if (!ds || !slot)
        return AAR_E_INVAL;
    zzapp_api_enter();
    audio_session_release(slot);
    zzapp_api_leave();
    zzapp_kernel_session_close(zzapp_runtime_kernel(), pid, 1);
    if (zzapp_proc_slot(pid)->audio_sessions)
        zzapp_proc_slot(pid)->audio_sessions--;
    audio_block_release_if_idle(ds);
    return AAR_OK;
}

void zzapp_api_reclaim_resources(void)
{
    uint32_t pid = api_pid();
    struct zzapp_decode_state *ds = decode_state(pid);
    uint32_t i;

    if (!ds)
        return;
    for (i = 0; i < ZZAPP_DECODE_SESSIONS; ++i)
        if (ds->image[i].id) {
            (void)sdk_image_stream_close(ds->image[i].id);
            memset(&ds->image[i], 0, sizeof(ds->image[i]));
        }
    if (!ds->audio)
        return;
    for (i = 0; i < ZZAPP_DECODE_SESSIONS; ++i)
        if (ds->audio->session[i].id) {
            audio_session_release(&ds->audio->session[i]);
            zzapp_kernel_session_close(zzapp_runtime_kernel(), pid, 1);
            if (zzapp_proc_slot(pid)->audio_sessions)
                zzapp_proc_slot(pid)->audio_sessions--;
        }
    audio_block_release_if_idle(ds);
}

void zzapp_api_poison_resources(void)
{
    uint32_t pid = api_pid();
    struct zzapp_decode_state *ds = decode_state(pid);
    uint32_t i;

    if (!ds)
        return;
    for (i = 0; i < ZZAPP_DECODE_SESSIONS; ++i)
        if (ds->image[i].id) {
            sdk_image_stream_poison_session(ds->image[i].id);
            (void)sdk_image_stream_release_poisoned(ds->image[i].id);
            memset(&ds->image[i], 0, sizeof(ds->image[i]));
        }
    if (ds->audio) {
        struct zzapp_audio_sessions *block = ds->audio;

        /* The decoders are forgotten, not released: core 1 may be dead
         * inside them. The block itself is a card-pool block under the
         * app's decode owner, which the reclaim below returns. Detach it
         * first. */
        ds->audio = NULL;
        for (i = 0; i < ZZAPP_DECODE_SESSIONS; ++i)
            if (block->session[i].id) {
                if (block->session[i].codec == AAR_AUDIO_FLAC)
                    sdk_flac_forget(&block->session[i].state.flac);
                else if (block->session[i].codec == AAR_AUDIO_VORBIS)
                    sdk_vorbis_forget(&block->session[i].state.vorbis);
                memset(&block->session[i], 0, sizeof(block->session[i]));
                zzapp_kernel_session_close(zzapp_runtime_kernel(), pid, 1);
                if (zzapp_proc_slot(pid)->audio_sessions)
                    zzapp_proc_slot(pid)->audio_sessions--;
            }
    }
    sdk_compression_reclaim_app_decode(app_decode_owner());
}
#define WRAP0(name, call) static int name(void) { zzapp_api_enter(); int r=(call); zzapp_api_leave(); return r; }
#define WRAP1(name, type, call) static int name(type a) { zzapp_api_enter(); int r=(call); zzapp_api_leave(); return r; }
WRAP1(api_file_close,int,zzapp_svc_file_close(a))
WRAP1(api_list_open,const char *,zzapp_svc_file_list_open(a))
WRAP1(api_list_close,int,zzapp_svc_file_list_close(a))
WRAP1(api_display_flip,uint32_t,zzapp_svc_display_flip(a))
WRAP1(api_display_flip_async,uint32_t,zzapp_svc_display_flip_async(a))
WRAP0(api_display_flip_idle,zzapp_display_flip_idle())
static int api_display_refresh(aar_surface *o) { zzapp_api_enter(); int r=zzapp_svc_display_refresh(o); zzapp_api_leave(); return r; }
static int api_display_set_palette(uint32_t id,uint32_t first,uint32_t count,const uint32_t *rgb){zzapp_api_enter();int r=zzapp_svc_display_set_palette(id,first,count,rgb);zzapp_api_leave();return r;}
static uint32_t api_display_latest_dropped(void){zzapp_api_enter();uint32_t r=zzapp_svc_display_latest_dropped();zzapp_api_leave();return r;}
WRAP0(api_vsync,zzapp_svc_display_vsync())
WRAP1(api_display_close,uint32_t,zzapp_svc_display_close(a))
WRAP0(api_audio_close,zzapp_svc_audio_close())
static int api_file_open(const char *p,uint32_t f){zzapp_api_enter();int r=zzapp_svc_file_open(p,f);zzapp_api_leave();return r;}
static int api_file_read(int f,void*p,uint32_t n,uint32_t*o){zzapp_api_enter();int r=zzapp_svc_file_read(f,p,n,o);zzapp_api_leave();return r;}
static int api_file_write(int f,const void*p,uint32_t n,uint32_t*o){zzapp_api_enter();int r=zzapp_svc_file_write(f,p,n,o);zzapp_api_leave();return r;}
static int api_file_seek(int f,int64_t o,int w,int64_t*out){zzapp_api_enter();int r=zzapp_svc_file_seek(f,o,w,out);zzapp_api_leave();return r;}
static int api_file_stat(const char*p,aar_stat*out){zzapp_api_enter();int r=zzapp_svc_file_stat(p,out);zzapp_api_leave();return r;}
static int api_list_next(int l,aar_dirent*out){zzapp_api_enter();int r=zzapp_svc_file_list_next(l,out);zzapp_api_leave();return r;}
static int api_display_open(uint32_t w,uint32_t h,uint32_t f,uint32_t fl,aar_surface*out){zzapp_api_enter();int r=zzapp_svc_display_open(w,h,f,fl,out);zzapp_api_leave();return r;}
static int api_event_poll(aar_event*out,uint32_t n,uint32_t*got){zzapp_api_enter();int r=zzapp_svc_event_poll(out,n,got);zzapp_api_leave();return r;}
static int api_audio_open(uint32_t r,uint32_t c,aar_audio*out){zzapp_api_enter();int x=zzapp_svc_audio_open(r,c,out);zzapp_api_leave();return x;}
static int api_console_write(const void *p,uint32_t n,uint32_t *out){zzapp_api_enter();int r=zzapp_svc_console_write(p,n,out);zzapp_api_leave();return r;}
static int api_console_read(void *p,uint32_t n,uint32_t *out){zzapp_api_enter();int r=zzapp_svc_console_read(p,n,out);zzapp_api_leave();return r;}
static int api_msg_send(const void *p){zzapp_api_enter();int r=zzapp_svc_msg_send(p);zzapp_api_leave();return r;}
static int api_msg_poll(void *p){zzapp_api_enter();int r=zzapp_svc_msg_poll(p);zzapp_api_leave();return r;}
static int api_msg_wait(uint32_t timeout_us){zzapp_api_enter();int r=zzapp_svc_msg_wait(timeout_us);zzapp_api_leave();return r;}
WRAP1(api_file_delete, const char *, zzapp_svc_file_delete(a))
WRAP1(api_dir_create, const char *, zzapp_svc_dir_create(a))
WRAP1(api_time_unix_us, int64_t *, zzapp_svc_time_unix_us(a))
WRAP1(api_locale_info, aar_locale_info *, zzapp_svc_locale_info(a))
WRAP1(api_display_wait_vblank, uint32_t, zzapp_svc_display_wait_vblank(a))

static int api_file_rename(const char *from, const char *to, uint32_t flags)
{
    int rc;
    zzapp_api_enter();
    rc = zzapp_svc_file_rename(from, to, flags);
    zzapp_api_leave();
    return rc;
}

static int api_file_set_date(const char *path, int64_t mtime)
{
    int rc;
    zzapp_api_enter();
    rc = zzapp_svc_file_set_date(path, mtime);
    zzapp_api_leave();
    return rc;
}

static int api_file_set_protect(const char *path, uint32_t protect)
{
    int rc;
    zzapp_api_enter();
    rc = zzapp_svc_file_set_protect(path, protect);
    zzapp_api_leave();
    return rc;
}

static int api_file_truncate(int fd, uint64_t size)
{
    int rc;
    zzapp_api_enter();
    rc = zzapp_svc_file_truncate(fd, size);
    zzapp_api_leave();
    return rc;
}

static int api_volume_info(const char *path, aar_volume_info *out)
{
    int rc;
    zzapp_api_enter();
    rc = zzapp_svc_volume_info(path, out);
    zzapp_api_leave();
    return rc;
}

static int api_file_get_comment(const char *path, void *buf, uint32_t bytes,
                                uint32_t *out)
{
    int rc;
    zzapp_api_enter();
    rc = zzapp_svc_file_get_comment(path, buf, bytes, out);
    zzapp_api_leave();
    return rc;
}

static int api_file_set_comment(const char *path, const void *data, uint32_t bytes)
{
    int rc;
    zzapp_api_enter();
    rc = zzapp_svc_file_set_comment(path, data, bytes);
    zzapp_api_leave();
    return rc;
}

static int api_env_get(const char *name, char *buf, uint32_t bytes, uint32_t *out)
{
    int rc;
    zzapp_api_enter();
    rc = zzapp_svc_env_get(name, buf, bytes, out);
    zzapp_api_leave();
    return rc;
}

static int api_clip_read(uint32_t unit, uint32_t type, void *buf,
                         uint32_t bytes, uint32_t *out)
{
    int rc;
    zzapp_api_enter();
    rc = zzapp_svc_clip_read(unit, type, buf, bytes, out);
    zzapp_api_leave();
    return rc;
}

static int api_clip_write(uint32_t unit, uint32_t type, const void *data,
                          uint32_t bytes)
{
    int rc;
    zzapp_api_enter();
    rc = zzapp_svc_clip_write(unit, type, data, bytes);
    zzapp_api_leave();
    return rc;
}

static int api_display_info(uint32_t id, aar_display_info *out)
{
    int rc;
    zzapp_api_enter();
    rc = zzapp_svc_display_info(id, out);
    zzapp_api_leave();
    return rc;
}

static int api_display_modes(aar_display_mode *out, uint32_t max, uint32_t *count)
{
    int rc;
    zzapp_api_enter();
    rc = zzapp_svc_display_modes(out, max, count);
    zzapp_api_leave();
    return rc;
}

static int api_display_resize(uint32_t id, uint32_t w, uint32_t h, aar_surface *out)
{
    int rc;
    zzapp_api_enter();
    /* The module retains render targets at bind time. Let the app unbind
     * explicitly before replacing their storage and geometry. */
    if (id && id == zzapp_display_id() &&
        minigl_binding_matches(api_pid(), zzapp_current_generation()))
        rc = AAR_E_BUSY;
    else
        rc = zzapp_svc_display_resize(id, w, h, out);
    zzapp_api_leave();
    return rc;
}

static int api_display_set_title(uint32_t id, const char *title)
{
    int rc;
    zzapp_api_enter();
    rc = zzapp_svc_display_set_title(id, title);
    zzapp_api_leave();
    return rc;
}

static int api_display_pointer(uint32_t id, uint32_t mode, const aar_pointer *image)
{
    int rc;
    zzapp_api_enter();
    rc = zzapp_svc_display_pointer(id, mode, image);
    zzapp_api_leave();
    return rc;
}

static uint64_t api_display_last_present_us(void)
{
    uint64_t time;
    zzapp_api_enter();
    time = zzapp_svc_display_last_present_us();
    zzapp_api_leave();
    return time;
}

static const aar_gl *api_gl_bind(uint32_t id){const aar_gl*out=NULL;zzapp_api_enter();if(zzapp_minigl_bind(id,&out)!=AAR_OK)out=NULL;zzapp_api_leave();return out;}
static const aar_gl *api_gl_bind_flags(uint32_t id,uint32_t flags){const aar_gl*out=NULL;zzapp_api_enter();if(zzapp_minigl_bind_flags(id,flags,&out)!=AAR_OK)out=NULL;zzapp_api_leave();return out;}
#if ZZAPP_PERF_COUNTERS
static void api_gl_swap(void)
{
    zzapp_gl_telemetry_snapshot_t snapshot;
    uint32_t slot = api_pid();
    int report = 0;

    zzapp_api_enter();
    if (minigl_binding_matches(slot, zzapp_current_generation())) {
        report = zzapp_svc_gl_telemetry_swap(slot, zzapp_host_time_us(),
                                             &snapshot);
        if (zzapp_minigl_swap() != AAR_OK)
            report = 0;
    }
    zzapp_api_leave();
    if (report)
        zzapp_svc_gl_telemetry_report(slot, &snapshot);
}
#else
static void api_gl_swap(void)
{
    zzapp_api_enter();
    (void)zzapp_minigl_swap();
    zzapp_api_leave();
}
#endif
static void api_gl_unbind(void){zzapp_api_enter();zzapp_minigl_unbind();zzapp_api_leave();}

/* The ZZ9000 extension table (aar/ext_zz9000.h). The app gets its own copy
 * from zzapp_svc_api_for_slot: gl_bind_flags is a veneer there, and the
 * strings live in the app's read-only table page. */
static const aar_ext_zz9000 g_ext = {
    .size = sizeof(g_ext),
    .gl_bind_flags = api_gl_bind_flags,
    .build_id = ZZ9000_BUILD_GIT,
};

static const void *api_ext(uint32_t platform_id, uint32_t ext_id)
{
    if (platform_id != AAR_PLATFORM_ZZ9000 || ext_id != AAR_EXT_ZZ9000_MAIN)
        return NULL;
    return &g_ext;
}

/* Slot order is aar_api's. audio_stats is NULL (the runtime keeps no
 * underrun count): the standard allows it, and the app then sees a NULL
 * slot. This is the table with every group filled in; zzapp_api_table()
 * hands out a copy in which the slots of the groups the runtime does not
 * declare are NULL (spec 3.3 rule 2). */
static const aar_api g_api_full = {
    .size = sizeof(g_api_full), .version = AAR_API_VERSION,
    .platform_id = AAR_PLATFORM_ZZ9000,
    /* core */
    .exit = api_exit, .yield = api_yield, .should_stop = api_stop,
    .time_us = api_time, .sleep_us = api_sleep, .info = api_info,
    .heap_range = api_heap_range, .heap_grow = api_heap_grow,
    .heap_shrink = api_heap_shrink, .heap_info = api_heap_info,
    .shared_alloc = api_shared_alloc, .shared_free = api_shared_free,
    .publish = api_publish, .acquire = api_acquire,
    .log = zzapp_svc_log,
    .file_open = api_file_open, .file_close = api_file_close,
    .file_read = api_file_read, .file_write = api_file_write,
    .file_seek = api_file_seek, .file_stat = api_file_stat,
    .file_list_open = api_list_open, .file_list_next = api_list_next,
    .file_list_close = api_list_close,
    .console_write = api_console_write, .console_read = api_console_read,
    .event_poll = api_event_poll,
    .msg_send = api_msg_send, .msg_poll = api_msg_poll,
    .msg_wait = api_msg_wait,
    .file_delete = api_file_delete, .file_rename = api_file_rename,
    .dir_create = api_dir_create, .file_set_date = api_file_set_date,
    .file_set_protect = api_file_set_protect, .file_truncate = api_file_truncate,
    .volume_info = api_volume_info,
    .time_unix_us = api_time_unix_us, .locale_info = api_locale_info,
    .env_get = api_env_get, .clip_read = api_clip_read, .clip_write = api_clip_write,
    /* optional */
    .ext = api_ext, .map_file = api_map_file, .unmap_file = api_unmap_file,
    .file_get_comment = api_file_get_comment, .file_set_comment = api_file_set_comment,
    /* display */
    .display_open = api_display_open, .display_flip = api_display_flip,
    .display_flip_async = api_display_flip_async,
    .display_flip_idle = api_display_flip_idle,
    .display_refresh = api_display_refresh, .display_vsync = api_vsync,
    .display_set_palette = api_display_set_palette,
    .display_close = api_display_close,
    .display_latest_dropped = api_display_latest_dropped,
    .display_info = api_display_info, .display_modes = api_display_modes,
    .display_resize = api_display_resize, .display_set_title = api_display_set_title,
    .display_pointer = api_display_pointer, .display_wait_vblank = api_display_wait_vblank,
    .display_last_present_us = api_display_last_present_us,
    /* audio */
    .audio_open = api_audio_open, .audio_close = api_audio_close,
    /* GL */
    .gl_bind = api_gl_bind, .gl_swap = api_gl_swap, .gl_unbind = api_gl_unbind,
    /* services */
    .sha256 = api_sha256, .decompress = api_decompress,
    .image_open = api_image_open, .image_decode = api_image_decode,
    .image_close = api_image_close,
    .audio_decode_open = api_audio_open_decode,
    .audio_decode = api_audio_decode_app,
    .audio_decode_close = api_audio_close_decode
};

/* The table the apps see: `groups` follows the runtime's group mask, and
 * every slot of a group whose bit is clear is NULL, so a clear bit is as
 * reliable as a set one. The copy is rebuilt only when the mask changes (it
 * never does on a card; the host tests change it). */
static aar_api g_api_out;
static int g_api_out_valid;

#define CLEAR_SLOTS(first, last) \
    memset(&g_api_out.first, 0, \
           offsetof(aar_api, last) + sizeof(g_api_out.last) - \
           offsetof(aar_api, first))

static void api_build_table(uint32_t groups)
{
    memcpy(&g_api_out, &g_api_full, sizeof(g_api_out));
    g_api_out.groups = groups;
    if ((groups & AAR_GROUP_DISPLAY) == 0u)
        CLEAR_SLOTS(display_open, display_last_present_us);
    if ((groups & AAR_GROUP_AUDIO) == 0u)
        CLEAR_SLOTS(audio_open, audio_stats);
    if ((groups & AAR_GROUP_GL) == 0u)
        CLEAR_SLOTS(gl_bind, gl_unbind);
    if ((groups & AAR_GROUP_SERVICES) == 0u)
        CLEAR_SLOTS(sha256, audio_decode_close);
    g_api_out_valid = 1;
}

const aar_api *zzapp_api_table(void)
{
    uint32_t groups = zzapp_runtime_groups();

    if (!g_api_out_valid || g_api_out.groups != groups)
        api_build_table(groups);
    return &g_api_out;
}
