/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aar/app.h"
#include "aar/ext_zz9000.h"
#include "zzapp_runtime.h"
#include "zzapp_proto.h"
#include "zzm_format.h"
#include "zzm_minigl_host.h"
#include "zzapp_svc.h"

/* A fresh arena with two admitted records (services admits at claim): pid 0
 * is the app under test, pid 1 the other app the arbiter must refuse. */
static void boot_with_records(void)
{
    (zzapp_boot_init)();
    assert(zzapp_test_admit() == 0U);
    assert(zzapp_test_admit() == 1U);
}
#define zzapp_boot_init() boot_with_records()

static aar_gl g_gl;
static zzm_minigl_ops_v1_t g_ops;
static zzm_minigl_worker_ops_t g_worker;
static zzm_minigl_registration_v1_t g_registration;
static uint32_t g_context;
static int g_available;
static int g_acquires;
static int g_releases;
static int g_binds;
static int g_swaps;
static int g_unbinds;
static int g_reclaims;
static int g_68k_closes;
static int g_68k_open;
static uint32_t g_surface_id;
static uint32_t g_reclaim_generation;
static uint32_t g_render_pixel;
static uint32_t g_present_pixel;
static int g_flips;
static uint32_t g_bind_flags;
static uint32_t g_display_id;

/* The id of the app's open display, 0 without one. */
uint32_t zzapp_display_id(void)
{
    return g_display_id;
}

static unsigned g_telemetry_resets;
static unsigned g_telemetry_swaps;
static unsigned g_telemetry_reports;

void zzapp_svc_gl_telemetry_reset(uint32_t pid)
{
    assert(pid == 0U);
    g_telemetry_resets++;
}

int zzapp_svc_gl_telemetry_swap(uint32_t pid, uint64_t now_us,
                                 zzapp_gl_telemetry_snapshot_t *snapshot)
{
    assert(pid == 0U);
    assert(now_us == 0U);
    assert(snapshot != NULL);
    memset(snapshot, 0, sizeof(*snapshot));
    g_telemetry_swaps++;
    return 1;
}

void zzapp_svc_gl_telemetry_report(uint32_t pid,
                                    const zzapp_gl_telemetry_snapshot_t *snapshot)
{
    assert(pid == 0U);
    assert(snapshot != NULL);
    assert(g_telemetry_reports < g_telemetry_swaps);
    g_telemetry_reports++;
}


int zzm_loader_acquire(uint32_t module_id)
{
    assert(module_id == ZZM_MODULE_MINIGL);
    g_acquires++;
    return g_available;
}

void zzm_loader_release(uint32_t module_id)
{
    assert(module_id == ZZM_MODULE_MINIGL);
    g_releases++;
}

const void *zzm_registry_ops(uint32_t module_id, uint32_t min_size)
{
    assert(module_id == ZZM_MODULE_MINIGL);
    return g_available && min_size <= sizeof(g_registration) ?
        &g_registration : NULL;
}


static void mock_begin(uint32_t mode)
{
    zzapp_api_enter();
    assert(zzapp_test_in_api() == 1u);
    g_render_pixel = 0xa55a0000u | mode;
    zzapp_api_leave();
}

static int mock_app_bind(void *context, uint32_t surface_id, const void **out_gl)
{
    (void)context;
    assert(zzapp_test_in_api() == 1u);
    g_binds++;
    g_surface_id = surface_id;
    *out_gl = &g_gl;
    return AAR_OK;
}
static int mock_app_bind_flags(void *context, uint32_t surface_id,
                               uint32_t flags, const void **out_gl)
{
    g_bind_flags = flags;
    return mock_app_bind(context, surface_id, out_gl);
}


static int mock_app_swap(void *context)
{
    (void)context;
    assert(zzapp_test_in_api() == 1u);
    g_present_pixel = g_render_pixel;
    g_flips++;
    g_swaps++;
    return AAR_OK;
}

static void mock_app_unbind(void *context)
{
    (void)context;
    assert(zzapp_test_in_api() == 1u);
    g_unbinds++;
}

static void mock_app_reclaim(void *context, uint32_t generation)
{
    (void)context;
    g_reclaims++;
    g_reclaim_generation = generation;
}

static void mock_close_68k(void *context, uint32_t handle)
{
    (void)context;
    (void)handle;
    g_68k_closes++;
}

static int mock_open_68k(void *context, const zzm_minigl_wire_open_t *request,
                         uint32_t *handle)
{
    (void)context;
    (void)request;
    g_68k_open = 1;
    *handle = 1u;
    return AAR_OK;
}

static int mock_has_work(void *context)
{
    (void)context;
    return 0;
}

static int mock_poll(void *context, uint32_t budget)
{
    (void)context;
    (void)budget;
    return 0;
}

static void reset_module(void)
{
    zzm_minigl_unregister();
    memset(&g_gl, 0, sizeof(g_gl));
    g_gl.size = sizeof(g_gl);
    g_gl.Begin = mock_begin;
    memset(&g_ops, 0, sizeof(g_ops));
    g_ops.version = ZZM_MINIGL_OPS_V1;
    g_ops.size = sizeof(g_ops);
    g_ops.close_68k = mock_close_68k;
    g_ops.open_68k = mock_open_68k;
    g_ops.app_bind = mock_app_bind;
    g_ops.app_swap = mock_app_swap;
    g_ops.app_unbind = mock_app_unbind;
    g_ops.app_reclaim = mock_app_reclaim;
    g_ops.app_bind_flags = mock_app_bind_flags;
    g_ops.reset_protocol = ZZM_MINIGL_RESET_PROTOCOL_V2;
    g_available = 0;
    g_acquires = 0;
    g_releases = 0;
    g_binds = 0;
    memset(&g_worker, 0, sizeof(g_worker));
    g_worker.version = ZZM_MINIGL_WORKER_OPS_V1;
    g_worker.size = sizeof(g_worker);
    g_worker.has_work = mock_has_work;
    g_worker.poll = mock_poll;
    memset(&g_registration, 0, sizeof(g_registration));
    g_registration.version = ZZM_MINIGL_REGISTRATION_V1;
    g_registration.size = sizeof(g_registration);
    g_registration.ops = &g_ops;
    g_registration.context = &g_context;
    g_registration.worker_ops = &g_worker;
    assert(zzm_minigl_register_ops(ZZM_MODULE_MINIGL, &g_registration,
                                   sizeof(g_registration)));
    g_swaps = 0;
    g_unbinds = 0;
    g_reclaims = 0;
    g_68k_closes = 0;
    g_68k_open = 0;
    g_surface_id = 0;
    g_reclaim_generation = 0;
    g_render_pixel = 0;
    g_present_pixel = 0;
    g_flips = 0;
    g_bind_flags = 0;
    g_telemetry_resets = 0U;
    g_telemetry_swaps = 0U;
    g_telemetry_reports = 0U;
}

int main(void)
{
    const aar_api *api;
    const aar_gl *gl;
    const aar_ext_zz9000 *ext;
    uint32_t card;

    zzapp_boot_init();
    reset_module();
    api = zzapp_api_table();

    g_display_id = 7u;

    assert(zzapp_minigl_bind(7u, &gl) == AAR_E_NOSYS);
    assert(gl == NULL);
    g_display_id = 7u;
    assert(api->gl_bind(7u) == NULL);
    assert(g_acquires == 2);
    assert(g_releases == 0);

    g_available = 1;
    g_ops.version++;
    g_display_id = 7u;
    assert(api->gl_bind(7u) == NULL);
    assert(g_acquires == 3);
    assert(g_releases == 1);
    g_ops.version = ZZM_MINIGL_OPS_V1;


    zzapp_kernel_set_current_thread(0);
    g_available = 1;
    g_display_id = 41u;
    gl = api->gl_bind(41u);
    assert(gl == &g_gl);
    assert(g_binds == 1);
    assert(g_surface_id == 41u);
    assert(g_telemetry_resets == 1U);
    gl->Begin(0x42u);
    assert(g_render_pixel == 0xa55a0042u);
    api->gl_swap();
    assert(g_swaps == 1);
    assert(g_flips == 1);
    assert(g_present_pixel == 0xa55a0042u);
    assert(g_telemetry_swaps == 1U);
    assert(g_telemetry_reports == 1U);
    zzapp_kernel_set_current_thread(1);
    assert(zzapp_minigl_swap() == AAR_E_BUSY);
    api->gl_swap();
    assert(g_swaps == 1);
    assert(g_flips == 1);
    assert(g_telemetry_swaps == 1U);
    assert(g_telemetry_reports == 1U);
    zzapp_kernel_set_current_thread(1);
    api->gl_unbind();
    assert(g_unbinds == 0);
    assert(g_releases == 1);
    zzapp_kernel_set_current_thread(0);
    api->gl_unbind();
    assert(g_unbinds == 1);
    assert(g_releases == 2);
    assert(g_telemetry_resets == 2U);
    ext = (const aar_ext_zz9000 *)api->ext(AAR_PLATFORM_ZZ9000,
                                           AAR_EXT_ZZ9000_MAIN);
    assert(ext != NULL && AAR_EXT_ZZ9000_COVERS(ext, gl_bind_flags));
    g_display_id = 42u;
    gl = ext->gl_bind_flags(42u, AAR_ZZ9000_GL_BIND_DIRECT_PRESENT);
    assert(gl == &g_gl);
    assert(g_bind_flags == AAR_ZZ9000_GL_BIND_DIRECT_PRESENT);
    api->gl_unbind();

    /* Only the app's open display can be bound. Another id, an id that names
     * nothing and 0 are refused with no table, no module context and no GL
     * slot taken, so the right id binds next, not BUSY. */
    {
        int binds = g_binds, acquires = g_acquires;
        unsigned bad = 0u;

#define BIND_CHECK(expr) do { if (!(expr)) { bad++; printf("bind: %s\n", #expr); } } while (0)
        g_display_id = 5u;
        gl = &g_gl;
        zzapp_api_enter();
        BIND_CHECK(zzapp_minigl_bind(6u, &gl) == AAR_E_INVAL && gl == NULL);
        gl = &g_gl;
        BIND_CHECK(zzapp_minigl_bind(0xFFFFFFFFu, &gl) == AAR_E_INVAL && gl == NULL);
        BIND_CHECK(zzapp_minigl_bind(0u, &gl) == AAR_E_INVAL && gl == NULL);
        zzapp_api_leave();
        BIND_CHECK(api->gl_bind(6u) == NULL);
        BIND_CHECK(ext->gl_bind_flags(6u, 0u) == NULL);
        BIND_CHECK(g_binds == binds && g_acquires == acquires);
        BIND_CHECK(api->gl_bind(5u) == &g_gl);
        BIND_CHECK(g_binds == binds + 1 && g_surface_id == 5u);
        api->gl_unbind();
#undef BIND_CHECK
        printf("minigl_api: bind: %u failures\n", bad);
        assert(bad == 0u);
    }

    card = 0;
    assert(zzapp_loader_begin() == AAR_OK);
    card = zzapp_test_arena_card();
    assert(card != 0u);
    zzapp_test_mark_running();
    g_display_id = 99u;
    assert(api->gl_bind(99u) == &g_gl);
    assert(g_ops.open_68k(&g_context, NULL, &card) == AAR_OK);
    assert(g_68k_open);
    zzapp_take_app_fault(ZZAPP_FAULT_DATA, card + 4u, 0u, 0u, 0u, 0u, 0u);
    zzapp_worker_hook();
    assert(g_reclaims == 1);
    assert(g_reclaim_generation != 0u);
    assert(g_68k_closes == 0);
    assert(g_68k_open);
    assert(g_releases == 5);

    zzapp_boot_init();
    reset_module();
    g_available = 1;
    zzapp_test_mark_running();
    g_display_id = 100u;
    assert(api->gl_bind(100u) == &g_gl);
    zzapp_on_amiga_reset();
    zzapp_worker_hook();
    assert(g_reclaims == 1);
    assert(g_68k_closes == 0);
    assert(g_releases == 1);

    /* A watchdog cold restart poisons the module image before the app-side
     * reclaim runs. zzapp_minigl_forget must drop the binding without
     * calling into the (poisoned) ops, and the later reclaim must no-op. */
    zzapp_boot_init();
    reset_module();
    g_available = 1;
    zzapp_test_mark_running();
    g_display_id = 101u;
    assert(api->gl_bind(101u) == &g_gl);
    assert(g_reclaims == 0);
    zzapp_minigl_forget();
    zzapp_minigl_reclaim(zzapp_current_generation());
    assert(g_reclaims == 0);
    assert(g_unbinds == 0);
    assert(g_releases == 1);
    return 0;
}
