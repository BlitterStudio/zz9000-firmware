/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "zzapp.h"
#include "zzapp_runtime.h"
#include "zzapp_proto.h"
#include "zzm_format.h"
#include "zzm_minigl_host.h"

static zzapp_gl g_gl;
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
    return ZZ_OK;
}

static int mock_app_swap(void *context)
{
    (void)context;
    assert(zzapp_test_in_api() == 1u);
    g_present_pixel = g_render_pixel;
    g_flips++;
    g_swaps++;
    return ZZ_OK;
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
    return ZZ_OK;
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
}

int main(void)
{
    const zzapp_api *api;
    const zzapp_gl *gl;
    uint32_t card;

    zzapp_boot_init();
    reset_module();
    api = zzapp_api_table();

    assert(zzapp_minigl_bind(7u, &gl) == ZZ_ENOSYS);
    assert(gl == NULL);
    assert(api->gl_bind(7u) == NULL);
    assert(g_acquires == 2);
    assert(g_releases == 0);

    g_available = 1;
    g_ops.version++;
    assert(api->gl_bind(7u) == NULL);
    assert(g_acquires == 3);
    assert(g_releases == 1);
    g_ops.version = ZZM_MINIGL_OPS_V1;

    g_available = 1;
    gl = api->gl_bind(41u);
    assert(gl == &g_gl);
    assert(g_binds == 1);
    assert(g_surface_id == 41u);
    gl->Begin(0x42u);
    assert(g_render_pixel == 0xa55a0042u);
    api->gl_swap();
    assert(g_swaps == 1);
    assert(g_flips == 1);
    assert(g_present_pixel == 0xa55a0042u);
    api->gl_unbind();
    assert(g_unbinds == 1);
    assert(g_releases == 2);

    card = 0;
    assert(zzapp_loader_begin() == ZZ_OK);
    card = zzapp_test_arena_card();
    assert(card != 0u);
    zzapp_test_mark_running();
    assert(api->gl_bind(99u) == &g_gl);
    assert(g_ops.open_68k(&g_context, NULL, &card) == ZZ_OK);
    assert(g_68k_open);
    zzapp_take_app_fault(ZZAPP_FAULT_DATA, card + 4u, 0u, 0u, 0u, 0u, 0u);
    assert(g_reclaims == 1);
    assert(g_reclaim_generation != 0u);
    assert(g_68k_closes == 0);
    assert(g_68k_open);
    assert(g_releases == 3);

    zzapp_boot_init();
    reset_module();
    g_available = 1;
    zzapp_test_mark_running();
    assert(api->gl_bind(100u) == &g_gl);
    zzapp_on_amiga_reset();
    assert(g_reclaims == 1);
    assert(g_68k_closes == 0);
    assert(g_releases == 1);
    return 0;
}
