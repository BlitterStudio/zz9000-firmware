/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stddef.h>
#include <stdint.h>

#include "minigl_transport.h"
#include "minigl_platform.h"
#include "minigl_raster.h"
#include "minigl_profile.h"
#include "mw_service.h"
#include "zzapp_services.h"
#include "zzm_minigl_host.h"
#include "zzm_runtime.h"

#define ZZM_MINIGL_MODULE_QUOTA (4U * 1024U * 1024U)
#define ZZM_MINIGL_TRANSPORT_QUOTA (4U * 1024U * 1024U)
#define ZZM_MINIGL_CONTEXT_QUOTA (384U * 1024U * 1024U)
#ifndef ZZM_MINIGL_BUILD_ID
#define ZZM_MINIGL_BUILD_ID "unknown"
#endif


struct aar_gl;

int zzapp_gl_set_module_host(const zzm_app_gl_host_service_t *host);
int zzapp_gl_bind(uint32_t surface_id, uint32_t flags,
                  const struct aar_gl **out);
void zzapp_gl_swap(void);
void zzapp_gl_unbind(void);
void zzapp_gl_reclaim(void);

typedef struct minigl_module_context {
    const zzm_host_window_service_t *host_window;
    const zzm_transport_service_t *transport;
    const zzm_reclaim_service_t *reclaim;
    const zzm_app_gl_host_service_t *app_host;
    const zzm_graphics_hw_service_t *graphics;
    uint32_t owner;
    uint32_t transport_owner;
    uint32_t app_owner;
    int initialized;
} minigl_module_context_t;

static minigl_module_context_t g_module;

static void minigl_release_app(minigl_module_context_t *module);

static int minigl_table_compatible(uint32_t version, uint32_t size,
                                   uint32_t required_version,
                                   size_t required_size)
{
    return ZZM_SERVICE_VERSION_COMPATIBLE(version, required_version) &&
           size >= required_size;
}

static void minigl_doorbell(void *context)
{
    (void)context;
    mw_service_doorbell();
}

static int minigl_has_work(void *context)
{
    (void)context;
    return mw_service_has_work() || minigl_raster_feed_pending();
}

extern void zzm_module_set_worker_context(int active);
extern int zzm_module_api_active(void);
static int g_wire_dispatch_deferred;

int zzm_minigl_take_deferred_dispatch(void)
{
    if (!g_wire_dispatch_deferred)
        return 0;
    g_wire_dispatch_deferred = 0;
    return 1;
}

void zzm_minigl_clear_deferred_dispatch(void)
{
    g_wire_dispatch_deferred = 0;
}


static int minigl_poll(void *context, uint32_t budget)
{
    int worked = 0;

    (void)context;
    if (budget == 0U)
        return 0;
    zzm_module_set_worker_context(1);
    if (zzm_module_api_active()) {
        /* An ARM GL call is parked on this ring. Feed and retire so it
         * can resume, but do not dispatch wire commands: they share this
         * backend and would change it before the call returns. */
        if (mw_service_has_work())
            g_wire_dispatch_deferred = 1;
        if (minigl_raster_work_outstanding() || minigl_raster_feed_pending()) {
            int feed = minigl_raster_try_feed(RIX_DRV_FEED_SOURCE_PARKED);

            worked = feed > RIX_DRV_FEED_BLOCKED ||
                     minigl_raster_work_outstanding() ||
                     minigl_raster_feed_pending();
        }
    } else if (mw_service_has_work()) {
        worked = mw_service_poll();
    }
    if (!worked && minigl_raster_feed_pending()) {
        minigl_profile_feed_deadline_wake();
        /* A full FIFO or a DMA still in flight is no progress: the worker
         * may then give the core up instead of retrying at once.  An app
         * parked inside a GL call waits on this ring, so keep feeding it. */
        worked = minigl_raster_try_feed(RIX_DRV_FEED_SOURCE_SCHEDULER) >
                     RIX_DRV_FEED_BLOCKED ||
                 zzm_module_api_active();
    }
    zzm_module_set_worker_context(0);
    return worked;
}

static int minigl_open_68k(void *context, const zzm_minigl_wire_open_t *request,
                           uint32_t *handle)
{
    (void)context;
    if (!request || !handle)
        return 0;
    return mw_service_open(request->width, request->height, request->flags,
                           handle) == MW_STATUS_OK;
}

static void minigl_close_68k(void *context, uint32_t handle)
{
    (void)context;
    (void)mw_service_close(handle);
}

static int minigl_create_transport_owner(minigl_module_context_t *module)
{
    uint32_t owner = 0U;

    if (!module || !module->reclaim || !module->reclaim->create_owner)
        return 0;
    if (module->transport_owner != 0U)
        return 1;
    if (!module->reclaim->create_owner(
            ZZM_MODULE_MINIGL, ZZM_RUNTIME_OWNER_CLASS_MINIGL_TRANSPORT,
            0U, ZZM_MINIGL_TRANSPORT_QUOTA, &owner))
        return 0;
    module->transport_owner = owner;
    return 1;
}

static void minigl_release_transport(minigl_module_context_t *module)
{
    if (!module || module->transport_owner == 0U)
        return;
    minigl_transport_fini();
    if (module->reclaim && module->reclaim->close_owner)
        (void)module->reclaim->close_owner(module->transport_owner);
    module->transport_owner = 0U;
}

static int minigl_transport_acquire(void *context, uint32_t *generation)
{
    ZZ9KMiniGLTransportDescriptor descriptor;
    minigl_module_context_t *module = context;

    if (!module || !minigl_create_transport_owner(module) ||
        minigl_transport_set_module_services(module->host_window,
                                             module->transport,
                                             module->transport_owner) != 0 ||
        minigl_transport_init() != 0 ||
        mw_service_get_transport_descriptor(&descriptor) != 0)
        return 0;
    if (generation)
        *generation = descriptor.generation;
    return 1;
}

static int minigl_transport_release(void *context)
{
    minigl_release_transport(context);
    return 1;
}

static int minigl_begin_reset(void *context)
{
    minigl_module_context_t *module = context;

    /* What the 68k last asked for and how long ago, before the reset tears
     * the context down: an Amiga-side crash leaves no other ARM trace. */
    mw_service_report_amiga_reset();
    /* Retrying a stop that failed cannot succeed later: report it, and the
     * firmware stops the engines and drops the module with core 1 halted. */
    if (!mw_rix_abort_engines())
        return ZZM_MINIGL_RESET_FAILED;
    minigl_release_app(module);
    minigl_release_transport(module);
    mw_service_reset();
    return ZZM_MINIGL_RESET_QUIESCENT;
}

static int minigl_reset_poll(void *context)
{
    (void)context;
    return ZZM_MINIGL_RESET_QUIESCENT;
}

static void minigl_abort_reset(void *context)
{
    (void)context;
    mw_service_on_core1_fault();
}

static int minigl_begin_unload(void *context)
{
    (void)context;
    if (mw_service_has_work())
        return 0;
    return 1;
}

static void minigl_fini(void *context)
{
    minigl_module_context_t *module = context;

    if (minigl_raster_work_outstanding()) {
        uint32_t bound = 100000U;
        /* Unload waits for the buffers back: a PARKED step polls fences. */
        while (minigl_raster_work_outstanding() && bound--) {
            if (minigl_raster_try_feed(RIX_DRV_FEED_SOURCE_PARKED) ==
                RIX_DRV_FEED_ERROR)
                break;
        }
        if (minigl_raster_work_outstanding()) {
            (void)mw_rix_abort_engines();
        } else {
            (void)minigl_raster_drain();
        }
    }
    minigl_release_app(module);
    minigl_release_transport(module);
    if (module && module->transport && module->transport->remove_doorbell)
        module->transport->remove_doorbell(ZZM_MODULE_MINIGL);
    mw_service_reset_local();
    if (module)
        module->initialized = 0;
}

static void minigl_release_app(minigl_module_context_t *module)
{
    if (!module || module->app_owner == 0U)
        return;
    zzm_minigl_clear_deferred_dispatch();
    zzapp_gl_reclaim();
    if (module->reclaim && module->reclaim->close_owner)
        (void)module->reclaim->close_owner(module->app_owner);
    module->app_owner = 0U;
    zzm_rt_set_context(module->owner, ZZM_MEMORY_CLASS_MINIGL_MODULE,
                       ZZM_MINIGL_MODULE_QUOTA);
}

static int minigl_app_bind_flags(void *context, uint32_t surface_id,
                                 uint32_t flags, const void **out_gl)
{
    const struct aar_gl *gl = 0;
    minigl_module_context_t *module = context;
    int status;

    if (!module || !module->reclaim || !module->reclaim->create_owner ||
        module->app_owner != 0U ||
        !module->reclaim->create_owner(
            ZZM_MODULE_MINIGL, ZZM_RUNTIME_OWNER_CLASS_MINIGL_APP_CONTEXT,
            0U, ZZM_MINIGL_CONTEXT_QUOTA, &module->app_owner))
        return -1;
    zzm_rt_set_context(module->app_owner, ZZM_MEMORY_CLASS_MINIGL_APP_CONTEXT,
                       ZZM_MINIGL_CONTEXT_QUOTA);
    status = zzapp_gl_bind(surface_id, flags, &gl);
    if (status != 0) {
        minigl_release_app(module);
        return status;
    }
    if (out_gl)
        *out_gl = gl;
    return 0;
}

static int minigl_app_bind(void *context, uint32_t surface_id,
                           const void **out_gl)
{
    return minigl_app_bind_flags(context, surface_id, 0U, out_gl);
}

static int minigl_app_swap(void *context)
{
    (void)context;
    zzapp_gl_swap();
    return 0;
}

static void minigl_app_unbind(void *context)
{
    minigl_release_app(context);
}

static void minigl_app_reclaim(void *context, uint32_t generation)
{
    (void)generation;
    if (mw_rix_abort_engines())
        minigl_release_app(context);
}

static const zzm_minigl_ops_v1_t g_ops = {
    ZZM_MINIGL_OPS_V1, sizeof(g_ops),
    minigl_doorbell, minigl_has_work, minigl_poll,
    minigl_open_68k, minigl_close_68k,
    minigl_transport_acquire, minigl_transport_release,
    minigl_begin_reset, minigl_reset_poll, minigl_abort_reset,
    minigl_begin_unload, minigl_fini,
    minigl_app_bind, minigl_app_swap, minigl_app_unbind, minigl_app_reclaim,
    minigl_app_bind_flags, ZZM_MINIGL_RESET_PROTOCOL_V2
};

static const zzm_minigl_worker_ops_t g_worker_ops = {
    ZZM_MINIGL_WORKER_OPS_V1, sizeof(g_worker_ops), minigl_has_work, minigl_poll
};

static const zzm_minigl_registration_v1_t g_registration = {
    ZZM_MINIGL_REGISTRATION_V1, sizeof(g_registration), &g_ops, &g_module,
    &g_worker_ops
};

int zzm_module_init(const zzm_services_t *services)
{
    const zzm_host_window_service_t *host_window;
    const zzm_transport_service_t *transport;
    const zzm_reclaim_service_t *reclaim;
    const zzm_app_gl_host_service_t *app_host;
    const zzm_palette_service_t *palette;
    const zzm_graphics_hw_service_t *graphics;
    zzm_graphics_hw_caps_t graphics_caps;

    if (g_module.initialized ||
        !zzm_rt_bind(services, ZZM_MODULE_MINIGL,
                     ZZM_MEMORY_CLASS_MINIGL_MODULE,
                     ZZM_MINIGL_MODULE_QUOTA))
        return -1;
    host_window = zzm_rt_get_service(ZZM_SERVICE_HOST_WINDOW,
                                     ZZM_SERVICE_HOST_WINDOW_V3);
    transport = zzm_rt_get_service(ZZM_SERVICE_TRANSPORT,
                                   ZZM_SERVICE_TRANSPORT_V3);
    reclaim = zzm_rt_get_service(ZZM_SERVICE_RECLAIM,
                                 ZZM_SERVICE_RECLAIM_V3);
    app_host = zzm_rt_get_service(ZZM_SERVICE_APP_GL_HOST,
                                  ZZM_SERVICE_APP_GL_HOST_V4);
    palette = zzm_rt_get_service(ZZM_SERVICE_PALETTE, ZZM_SERVICE_PALETTE_V3);
    graphics = zzm_rt_get_service(ZZM_SERVICE_GRAPHICS_HW,
                                  ZZM_SERVICE_GRAPHICS_HW_V3);
    if (!host_window || !transport || !reclaim || !app_host || !palette ||
        !graphics ||
        !minigl_table_compatible(host_window->version, host_window->size,
                                 ZZM_SERVICE_HOST_WINDOW_V3,
                                 sizeof(*host_window)) ||
        !minigl_table_compatible(transport->version, transport->size,
                                 ZZM_SERVICE_TRANSPORT_V3,
                                 sizeof(*transport)) ||
        !minigl_table_compatible(reclaim->version, reclaim->size,
                                 ZZM_SERVICE_RECLAIM_V3,
                                 sizeof(*reclaim)) ||
        !reclaim->create_owner || !reclaim->close_owner ||
        !reclaim->quarantine_owner ||
        !minigl_table_compatible(app_host->version, app_host->size,
                                 ZZM_SERVICE_APP_GL_HOST_V4,
                                 offsetof(zzm_app_gl_host_service_t, mailbox_buffers)) ||
        !minigl_table_compatible(palette->version, palette->size,
                                 ZZM_SERVICE_PALETTE_V3,
                                 sizeof(*palette)) ||
        !minigl_table_compatible(graphics->version, graphics->size,
                                 ZZM_SERVICE_GRAPHICS_HW_V3,
                                 offsetof(zzm_graphics_hw_service_t, barrier) +
                                 sizeof(graphics->barrier)) || !graphics->caps ||
        graphics->caps(&graphics_caps) <= 0)
        return -1;

    g_module.host_window = host_window;
    g_module.transport = transport;
    g_module.reclaim = reclaim;
    g_module.app_host = app_host;
    g_module.graphics = graphics;
    g_module.owner = ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL);
    if (!minigl_platform_set_graphics(graphics, &graphics_caps, g_module.owner) ||
        zzapp_gl_set_module_host(app_host) != 0 ||
        !transport->install_doorbell ||
        !transport->install_doorbell(ZZM_MODULE_MINIGL))
        return -1;
    if (transport->publish_capabilities)
        transport->publish_capabilities(ZZM_MODULE_MINIGL,
                                        graphics_caps.has_rasterix_ef ?
                                        ZZM_MINIGL_CAP_RASTERIX : 0U);
    mw_service_init();
    if (!services->register_ops ||
        !services->register_ops(ZZM_MODULE_MINIGL, &g_registration,
                                sizeof(g_registration)))
        return -1;
    g_module.initialized = 1;
    minigl_platform_logf("MINIGL loaded v1 build %s", ZZM_MINIGL_BUILD_ID);
    return 0;
}

void zzm_module_fini(void)
{
    minigl_fini(&g_module);
}

const uint32_t zzm_module_descriptor = ZZM_MODULE_MINIGL;
