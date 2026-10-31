/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <string.h>

#include "module_host.h"
#include "mw_service.h"
#include "zzm_runtime.h"

int zzm_module_init(const zzm_services_t *services);
void zzm_module_fini(void);

static const zzm_minigl_registration_v1_t *registered;
static unsigned int doorbells;
static unsigned int resets;
static unsigned int transport_fini;
static uint32_t next_handle = 0x10001U;

static int window_alloc(uint32_t owner, uint32_t bytes, uint32_t alignment,
                        zzm_host_window_t *out)
{
    (void)owner;
    (void)bytes;
    (void)alignment;
    out->arm_address = 0x08400000U;
    out->generation = 1U;
    return 0;
}

static int window_free(uint32_t owner, uint32_t address, uint32_t generation)
{
    (void)owner;
    (void)address;
    (void)generation;
    return 0;
}

static const zzm_host_window_service_t host_window = {
    ZZM_SERVICE_HOST_WINDOW_V3, sizeof(host_window), window_alloc, window_free
};

static int transport_publish(uint32_t module_id, const void *data, uint32_t bytes)
{
    (void)module_id;
    (void)data;
    (void)bytes;
    return 0;
}

static void transport_retract(uint32_t module_id) { (void)module_id; }
static int transport_install(uint32_t module_id)
{
    return module_id == ZZM_MODULE_MINIGL ? 0 : -1;
}
static void transport_remove(uint32_t module_id) { (void)module_id; }
static void transport_caps(uint32_t module_id, uint32_t caps)
{
    (void)module_id;
    (void)caps;
}

static const zzm_transport_service_t transport = {
    ZZM_SERVICE_TRANSPORT_V3, sizeof(transport), transport_publish,
    transport_retract, transport_install, transport_remove, transport_caps
};

static void app_enter(void) { }
static void app_leave(void) { }
static int app_bound(uint32_t *width, uint32_t *height, uint32_t *pitch,
                     uint32_t *format, uint32_t *front, uint32_t *back)
{
    (void)width;
    (void)height;
    (void)pitch;
    (void)format;
    (void)front;
    (void)back;
    return -1;
}
static int app_flip(uint32_t generation)
{
    (void)generation;
    return 0;
}
static uint32_t app_generation(void) { return 1U; }

static const zzm_app_gl_host_service_t app_host = {
    ZZM_SERVICE_APP_GL_HOST_V3, sizeof(app_host), app_enter, app_leave,
    app_bound, app_flip, app_generation
};

static uint32_t palette_generation(void) { return 1U; }
static int palette_snapshot(uint32_t entries[256], uint32_t *generation)
{
    memset(entries, 0, 256U * sizeof(*entries));
    *generation = 1U;
    return 1;
}
static uint32_t palette_lookup(uint32_t index) { return index; }

static const zzm_palette_service_t palette = {
    ZZM_SERVICE_PALETTE_V3, sizeof(palette), palette_generation,
    palette_snapshot, palette_lookup
};

static int graphics_caps(zzm_graphics_hw_caps_t *caps)
{
    memset(caps, 0, sizeof(*caps));
    return 1;
}

static int graphics_read(uint32_t offset, uint32_t *value)
{
    (void)offset;
    *value = 0U;
    return 1;
}

static int graphics_write(uint32_t offset, uint32_t value)
{
    (void)offset;
    (void)value;
    return 1;
}

static int graphics_words(const uint32_t *words, uint32_t count)
{
    (void)words;
    (void)count;
    return 1;
}

static void graphics_barrier(void) { }

static const zzm_graphics_hw_service_t graphics = {
    ZZM_SERVICE_GRAPHICS_HW_V3, sizeof(graphics), graphics_caps,
    graphics_read, graphics_write, graphics_read, graphics_write,
    graphics_words, 0, 0, 0, 0, graphics_barrier
};

int zzm_rt_bind(const zzm_services_t *services, uint32_t module_id,
                uint32_t memory_class, uint32_t quota)
{
    (void)services;
    (void)module_id;
    (void)memory_class;
    (void)quota;
    return 1;
}

const void *zzm_rt_get_service(uint32_t id, uint32_t version)
{
    (void)version;
    switch (id) {
    case ZZM_SERVICE_HOST_WINDOW:
        return &host_window;
    case ZZM_SERVICE_TRANSPORT:
        return &transport;
    case ZZM_SERVICE_APP_GL_HOST:
        return &app_host;
    case ZZM_SERVICE_PALETTE:
        return &palette;
    case ZZM_SERVICE_GRAPHICS_HW:
        return &graphics;
    default:
        return 0;
    }
}

void zzm_rt_dcache_clean(uint32_t address, uint32_t length)
{
    (void)address;
    (void)length;
}

void zzm_rt_dcache_invalidate(uint32_t address, uint32_t length)
{
    (void)address;
    (void)length;
}

void zzm_rt_barrier(void) { }
uint64_t zzm_rt_time_us(void) { return 0U; }
void zzm_rt_log(const char *message) { (void)message; }
int minigl_transport_set_module_services(const zzm_host_window_service_t *window,
                                         const zzm_transport_service_t *link,
                                         uint32_t owner)
{
    return window == &host_window && link == &transport && owner ? 0 : -1;
}
int minigl_transport_init(void) { return 0; }
void minigl_transport_fini(void) { ++transport_fini; }
void mw_service_init(void) { }
void mw_service_doorbell(void) { ++doorbells; }
int mw_service_has_work(void) { return 0; }
int mw_service_poll(void) { return 0; }
int mw_service_open(uint32_t width, uint32_t height, uint32_t flags,
                    uint32_t *handle)
{
    (void)width;
    (void)height;
    (void)flags;
    *handle = next_handle;
    return MW_STATUS_OK;
}
int mw_service_close(uint32_t handle)
{
    return handle == next_handle ? MW_STATUS_OK : MW_STATUS_UNAVAILABLE;
}
void mw_service_reset(void) { ++resets; }
void mw_service_reset_local(void) { ++resets; }
void mw_service_on_core1_fault(void) { ++resets; }
int mw_service_get_transport_descriptor(ZZ9KMiniGLTransportDescriptor *out)
{
    out->generation = 7U;
    return 0;
}

static int register_ops(uint32_t module_id, const void *ops, uint32_t bytes)
{
    if (module_id != ZZM_MODULE_MINIGL || bytes != sizeof(*registered))
        return -1;
    registered = ops;
    return 1;
}

int minigl_module_host_init(void)
{
    const zzm_services_t services = {
        ZZM_V3_ABI_MAJOR, sizeof(services), 0, register_ops, 0
    };

    registered = 0;
    doorbells = 0;
    resets = 0;
    transport_fini = 0;
    return zzm_module_init(&services);
}

void minigl_module_host_fini(void)
{
    zzm_module_fini();
}

const zzm_minigl_registration_v1_t *minigl_module_host_registration(void)
{
    return registered;
}

unsigned int minigl_module_host_doorbell_count(void) { return doorbells; }
unsigned int minigl_module_host_reset_count(void) { return resets; }
unsigned int minigl_module_host_transport_fini_count(void)
{
    return transport_fini;
}
