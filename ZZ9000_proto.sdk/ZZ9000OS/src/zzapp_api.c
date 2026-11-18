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
#if defined(ZZAPP_HOST_TEST) && ZZAPP_PERF_COUNTERS
void __attribute__((weak)) zzapp_svc_gl_telemetry_reset(uint32_t slot)
{ (void)slot; }
int __attribute__((weak)) zzapp_svc_gl_telemetry_swap(
    uint32_t slot, uint64_t now_us, zzapp_gl_telemetry_snapshot_t *snapshot)
{
    (void)slot;
    (void)now_us;
    if (snapshot)
        memset(snapshot, 0, sizeof(*snapshot));
    return 0;
}
void __attribute__((weak)) zzapp_svc_gl_telemetry_report(
    uint32_t slot, const zzapp_gl_telemetry_snapshot_t *snapshot)
{ (void)slot; (void)snapshot; }
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
WEAK uint8_t *zzapp_services_window(void) { return NULL; }
WEAK int zzapp_services_start_entered(uint32_t generation)
{
    (void)generation;
    return AAR_OK;
}
WEAK void zzapp_services_loader_aborted(void) {}
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
WEAK int zzapp_svc_display_refresh(aar_surface *o) { (void)o; return AAR_E_NOSYS; }
WEAK int zzapp_svc_display_set_palette(uint32_t id,uint32_t first,uint32_t count,const uint32_t *rgb) { (void)id;(void)first;(void)count;(void)rgb; return AAR_E_NOSYS; }
WEAK uint32_t zzapp_svc_display_latest_dropped(void) { return 0U; }
WEAK int zzapp_svc_display_vsync(void) { return AAR_E_NOSYS; }
WEAK int zzapp_svc_display_close(uint32_t id) { (void)id; return AAR_E_NOSYS; }
WEAK int zzapp_svc_event_poll(aar_event *o,uint32_t n,uint32_t *out) { (void)o;(void)n; if(out)*out=0; return AAR_E_NOSYS; }
WEAK int zzapp_svc_audio_open(uint32_t r,uint32_t c,aar_audio *o) { (void)r;(void)c;(void)o; return AAR_E_NOSYS; }
WEAK int zzapp_svc_audio_close(void) { return AAR_E_NOSYS; }
WEAK void zzapp_svc_gl_release(uint32_t slot, uint32_t generation)
{ (void)slot; (void)generation; }

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
WEAK uint32_t sdk_compression_app_decode_owner(uint32_t slot,
                                                uint32_t generation)
{ return sdk_compression_app_owner_bits(slot, generation); }
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
static uint32_t api_slot(void);
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
    uint32_t owner = api_slot();
    uint32_t generation = zzapp_current_generation();
    int status;
    void *context = NULL;

    if (out)
        *out = NULL;
    if (!surface_id)
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
    uint32_t owner = api_slot();
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
    uint32_t owner = api_slot();
    uint32_t generation = zzapp_current_generation();

    if (!minigl_binding_matches(owner, generation))
        return;
    g_minigl.ops->app_unbind(g_minigl.context);
    minigl_drop();
}

void zzapp_minigl_reclaim(uint32_t generation)
{
    uint32_t owner = api_slot();

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

static uint32_t api_slot(void)
{
    return zzapp_services_current_slot();
}

static uint32_t app_decode_owner(void)
{
    return sdk_compression_app_decode_owner(api_slot(),
                                            zzapp_current_generation());
}

struct zzapp_image_session {
    uint32_t id;
    uint32_t tile_address;
    uint32_t tile_bytes;
};

static struct zzapp_image_session
    g_image_sessions[ZZAPP_AS_SLOT_COUNT][ZZAPP_DECODE_SESSIONS];

struct zzapp_audio_session {
    uint32_t id;
    uint32_t codec;
    union {
        mp3dec_t mp3;
        struct sdk_flac_state flac;
        struct sdk_vorbis_state vorbis;
    } state;
};

static struct zzapp_audio_session
    g_audio_sessions[ZZAPP_AS_SLOT_COUNT][ZZAPP_DECODE_SESSIONS];
static uint32_t g_next_audio_session[ZZAPP_AS_SLOT_COUNT];

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
    uint32_t owner = api_slot();
    uint32_t i;
    int rc;

    if (!open || !session || !out || !open->tile || !open->stride ||
        !open->rows || !open->tile_bytes ||
        open->output_format != AAR_FMT_BGRA8888)
        return AAR_E_INVAL;
    for (i = 0; i < ZZAPP_DECODE_SESSIONS &&
                g_image_sessions[owner][i].id; ++i) {}
    if (i == ZZAPP_DECODE_SESSIONS ||
        zzapp_kernel_session_open(zzapp_runtime_kernel(), owner, 0) != 0)
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
        g_image_sessions[owner][i].id = result.session;
        g_image_sessions[owner][i].tile_address = (uint32_t)(uintptr_t)open->tile;
        g_image_sessions[owner][i].tile_bytes = open->tile_bytes;
        zzapp_proc_slot(owner)->image_sessions++;
    } else
        zzapp_kernel_session_close(zzapp_runtime_kernel(), owner, 0);
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
    uint32_t owner = api_slot();
    uint32_t i;
    int rc;

    if (!src || !out)
        return AAR_E_INVAL;
    for (i = 0; i < ZZAPP_DECODE_SESSIONS &&
                g_image_sessions[owner][i].id != session; ++i) {}
    if (i == ZZAPP_DECODE_SESSIONS)
        return AAR_E_INVAL;
    if (zzapp_svc_check_user(&zzapp_proc_slot(owner)->as,
                             g_image_sessions[owner][i].tile_address,
                             g_image_sessions[owner][i].tile_bytes, 1) != AAR_OK)
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
    uint32_t owner = api_slot();
    uint32_t i;
    int rc;

    for (i = 0; i < ZZAPP_DECODE_SESSIONS &&
                g_image_sessions[owner][i].id != session; ++i) {}
    if (i == ZZAPP_DECODE_SESSIONS)
        return AAR_E_INVAL;
    zzapp_api_enter();
    rc = sdk_to_zz(sdk_image_stream_close(session));
    zzapp_api_leave();
    if (rc == AAR_OK) {
        memset(&g_image_sessions[owner][i], 0,
               sizeof(g_image_sessions[owner][i]));
        zzapp_kernel_session_close(zzapp_runtime_kernel(), owner, 0);
        if (zzapp_proc_slot(owner)->image_sessions)
            zzapp_proc_slot(owner)->image_sessions--;
    }
    return rc;
}

static struct zzapp_audio_session *audio_session_find(uint32_t session)
{
    struct zzapp_audio_session *sessions = g_audio_sessions[api_slot()];
    uint32_t i;

    for (i = 0; i < ZZAPP_DECODE_SESSIONS; ++i)
        if (sessions[i].id == session)
            return &sessions[i];
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

static int api_audio_open_decode(uint32_t codec, uint32_t *session)
{
    struct zzapp_audio_session *slot = NULL;
    uint32_t owner = api_slot();
    uint32_t i;

    if (!session || (codec != AAR_AUDIO_MP3 && codec != AAR_AUDIO_FLAC &&
                     codec != AAR_AUDIO_VORBIS))
        return AAR_E_INVAL;
    for (i = 0; i < ZZAPP_DECODE_SESSIONS; ++i)
        if (g_audio_sessions[owner][i].id == 0u) {
            slot = &g_audio_sessions[owner][i];
            break;
        }
    if (!slot ||
        zzapp_kernel_session_open(zzapp_runtime_kernel(), owner, 1) != 0)
        return AAR_E_NOMEM;
    memset(slot, 0, sizeof(*slot));
    slot->id = 0x80000000u | ++g_next_audio_session[owner];
    if (g_next_audio_session[owner] == 0u)
        g_next_audio_session[owner] = 1u;
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
    zzapp_proc_slot(owner)->audio_sessions++;
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
    uint32_t owner = api_slot();
    struct zzapp_audio_session *slot = audio_session_find(session);

    if (!slot)
        return AAR_E_INVAL;
    zzapp_api_enter();
    audio_session_release(slot);
    zzapp_api_leave();
    zzapp_kernel_session_close(zzapp_runtime_kernel(), owner, 1);
    if (zzapp_proc_slot(owner)->audio_sessions)
        zzapp_proc_slot(owner)->audio_sessions--;
    return AAR_OK;
}

void zzapp_api_reclaim_resources(void)
{
    uint32_t owner = api_slot();
    struct zzapp_audio_session *audio_sessions = g_audio_sessions[owner];
    struct zzapp_image_session *image_sessions = g_image_sessions[owner];
    uint32_t i;

    for (i = 0; i < ZZAPP_DECODE_SESSIONS; ++i)
        if (image_sessions[i].id) {
            (void)sdk_image_stream_close(image_sessions[i].id);
            memset(&image_sessions[i], 0, sizeof(image_sessions[i]));
        }
    for (i = 0; i < ZZAPP_DECODE_SESSIONS; ++i)
        if (audio_sessions[i].id) {
            audio_session_release(&audio_sessions[i]);
            zzapp_kernel_session_close(zzapp_runtime_kernel(), owner, 1);
            if (zzapp_proc_slot(owner)->audio_sessions)
                zzapp_proc_slot(owner)->audio_sessions--;
        }
}

void zzapp_api_poison_resources(void)
{
    uint32_t owner = api_slot();
    struct zzapp_audio_session *audio_sessions = g_audio_sessions[owner];
    struct zzapp_image_session *image_sessions = g_image_sessions[owner];
    uint32_t i;

    for (i = 0; i < ZZAPP_DECODE_SESSIONS; ++i)
        if (image_sessions[i].id) {
            sdk_image_stream_poison_session(image_sessions[i].id);
            (void)sdk_image_stream_release_poisoned(image_sessions[i].id);
            memset(&image_sessions[i], 0, sizeof(image_sessions[i]));
        }
    for (i = 0; i < ZZAPP_DECODE_SESSIONS; ++i)
        if (audio_sessions[i].id) {
            if (audio_sessions[i].codec == AAR_AUDIO_FLAC)
                sdk_flac_forget(&audio_sessions[i].state.flac);
            else if (audio_sessions[i].codec == AAR_AUDIO_VORBIS)
                sdk_vorbis_forget(&audio_sessions[i].state.vorbis);
            memset(&audio_sessions[i], 0, sizeof(audio_sessions[i]));
            zzapp_kernel_session_close(zzapp_runtime_kernel(), owner, 1);
            if (zzapp_proc_slot(owner)->audio_sessions)
                zzapp_proc_slot(owner)->audio_sessions--;
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
static const aar_gl *api_gl_bind(uint32_t id){const aar_gl*out=NULL;zzapp_api_enter();if(zzapp_minigl_bind(id,&out)!=AAR_OK)out=NULL;zzapp_api_leave();return out;}
static const aar_gl *api_gl_bind_flags(uint32_t id,uint32_t flags){const aar_gl*out=NULL;zzapp_api_enter();if(zzapp_minigl_bind_flags(id,flags,&out)!=AAR_OK)out=NULL;zzapp_api_leave();return out;}
#if ZZAPP_PERF_COUNTERS
static void api_gl_swap(void)
{
    zzapp_gl_telemetry_snapshot_t snapshot;
    uint32_t slot = api_slot();
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
 * slot. `groups` follows the runtime's group mask. */
static aar_api g_api = {
    .size = sizeof(g_api), .version = AAR_API_VERSION,
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
    /* optional */
    .ext = api_ext, .map_file = api_map_file, .unmap_file = api_unmap_file,
    /* display */
    .display_open = api_display_open, .display_flip = api_display_flip,
    .display_flip_async = api_display_flip_async,
    .display_flip_idle = api_display_flip_idle,
    .display_refresh = api_display_refresh, .display_vsync = api_vsync,
    .display_set_palette = api_display_set_palette,
    .display_close = api_display_close,
    .display_latest_dropped = api_display_latest_dropped,
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

const aar_api *zzapp_api_table(void)
{
    uint32_t groups = zzapp_runtime_groups();

    if (g_api.groups != groups)
        g_api.groups = groups;
    return &g_api;
}
