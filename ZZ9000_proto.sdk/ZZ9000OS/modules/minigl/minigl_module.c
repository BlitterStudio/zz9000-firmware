/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stddef.h>
#include <stdint.h>

#include "minigl_transport.h"
#include "mw_service.h"
#include "zzapp_services.h"
#include "zzm_minigl_host.h"
#include "zzm_runtime.h"

#define ZZM_MINIGL_MODULE_QUOTA (4U * 1024U * 1024U)
#define ZZM_MINIGL_CAP_RASTERIX 1U

struct zzapp_gl;

int zzapp_gl_set_module_host(const zzm_app_gl_host_service_t *host);
int zzapp_gl_bind(uint32_t surface_id, const struct zzapp_gl **out);
void zzapp_gl_swap(void);
void zzapp_gl_unbind(void);
void zzapp_gl_reclaim(void);

typedef struct minigl_module_context {
    const zzm_host_window_service_t *host_window;
    const zzm_transport_service_t *transport;
    const zzm_app_gl_host_service_t *app_host;
    const zzm_graphics_hw_service_t *graphics;
    uint32_t owner;
    int initialized;
} minigl_module_context_t;

static minigl_module_context_t g_module;

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
    return mw_service_has_work();
}

static int minigl_poll(void *context, uint32_t budget)
{
    (void)context;
    if (budget == 0U)
        return 0;
    return mw_service_poll();
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

static int minigl_transport_acquire(void *context, uint32_t *generation)
{
    ZZ9KMiniGLTransportDescriptor descriptor;
    minigl_module_context_t *module = context;

    if (!module || minigl_transport_init() != 0 ||
        mw_service_get_transport_descriptor(&descriptor) != 0)
        return 0;
    if (generation)
        *generation = descriptor.generation;
    return 1;
}

static int minigl_transport_release(void *context)
{
    (void)context;
    minigl_transport_fini();
    return 1;
}

static int minigl_begin_reset(void *context)
{
    (void)context;
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
    return mw_service_has_work() ? 0 : 1;
}

static void minigl_fini(void *context)
{
    minigl_module_context_t *module = context;

    minigl_transport_fini();
    if (module && module->transport && module->transport->remove_doorbell)
        module->transport->remove_doorbell(ZZM_MODULE_MINIGL);
    mw_service_reset_local();
    if (module)
        module->initialized = 0;
}

static int minigl_app_bind(void *context, uint32_t surface_id,
                           const void **out_gl)
{
    const struct zzapp_gl *gl = 0;
    int status;

    (void)context;
    status = zzapp_gl_bind(surface_id, &gl);
    if (out_gl)
        *out_gl = gl;
    return status;
}

static int minigl_app_swap(void *context)
{
    (void)context;
    zzapp_gl_swap();
    return 0;
}

static void minigl_app_unbind(void *context)
{
    (void)context;
    zzapp_gl_unbind();
}

static void minigl_app_reclaim(void *context, uint32_t generation)
{
    (void)context;
    (void)generation;
    zzapp_gl_reclaim();
}

static const zzm_minigl_ops_v1_t g_ops = {
    ZZM_MINIGL_OPS_V1, sizeof(g_ops),
    minigl_doorbell, minigl_has_work, minigl_poll,
    minigl_open_68k, minigl_close_68k,
    minigl_transport_acquire, minigl_transport_release,
    minigl_begin_reset, minigl_reset_poll, minigl_abort_reset,
    minigl_begin_unload, minigl_fini,
    minigl_app_bind, minigl_app_swap, minigl_app_unbind, minigl_app_reclaim
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
    app_host = zzm_rt_get_service(ZZM_SERVICE_APP_GL_HOST,
                                  ZZM_SERVICE_APP_GL_HOST_V3);
    palette = zzm_rt_get_service(ZZM_SERVICE_PALETTE, ZZM_SERVICE_PALETTE_V3);
    graphics = zzm_rt_get_service(ZZM_SERVICE_GRAPHICS_HW,
                                  ZZM_SERVICE_GRAPHICS_HW_V3);
    if (!host_window || !transport || !app_host || !palette || !graphics ||
        !minigl_table_compatible(host_window->version, host_window->size,
                                 ZZM_SERVICE_HOST_WINDOW_V3,
                                 sizeof(*host_window)) ||
        !minigl_table_compatible(transport->version, transport->size,
                                 ZZM_SERVICE_TRANSPORT_V3,
                                 sizeof(*transport)) ||
        !minigl_table_compatible(app_host->version, app_host->size,
                                 ZZM_SERVICE_APP_GL_HOST_V3,
                                 sizeof(*app_host)) ||
        !minigl_table_compatible(palette->version, palette->size,
                                 ZZM_SERVICE_PALETTE_V3,
                                 sizeof(*palette)) ||
        !minigl_table_compatible(graphics->version, graphics->size,
                                 ZZM_SERVICE_GRAPHICS_HW_V3,
                                 sizeof(*graphics)) || !graphics->caps ||
        graphics->caps(&graphics_caps) != 0)
        return -1;

    g_module.host_window = host_window;
    g_module.transport = transport;
    g_module.app_host = app_host;
    g_module.graphics = graphics;
    g_module.owner = ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL);
    if (zzapp_gl_set_module_host(app_host) != 0 ||
        minigl_transport_set_module_services(host_window, transport,
                                             g_module.owner) != 0 ||
        !transport->install_doorbell ||
        transport->install_doorbell(ZZM_MODULE_MINIGL) != 0)
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
    return 0;
}

void zzm_module_fini(void)
{
    minigl_fini(&g_module);
}

const uint32_t zzm_module_descriptor = ZZM_MODULE_MINIGL;
