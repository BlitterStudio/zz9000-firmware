/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "module_host.h"
#include "zzm_runtime.h"

struct zzapp_gl;
int zzapp_gl_set_module_host(const zzm_app_gl_host_service_t *host)
{
    return host != 0 ? 0 : -1;
}
int zzapp_gl_bind(uint32_t surface_id, const struct zzapp_gl **out)
{
    (void)surface_id;
    *out = 0;
    return 0;
}
void zzapp_gl_swap(void) { }
void zzapp_gl_unbind(void) { }
void zzapp_gl_reclaim(void) { }

int main(void)
{
    const zzm_minigl_registration_v1_t *registered;
    zzm_minigl_wire_open_t request = { 320U, 200U, 0U };
    const void *gl = 0;
    uint32_t handle = 0;

    assert(minigl_module_host_init() == 0);
    registered = minigl_module_host_registration();
    assert(registered && registered->version == ZZM_MINIGL_REGISTRATION_V1);
    assert(registered->ops->open_68k(registered->context, &request, &handle));
    assert(handle == 0x10001U);
    registered->ops->doorbell(registered->context);
    assert(minigl_module_host_doorbell_count() == 1U);
    assert(registered->ops->app_bind(registered->context, 1U, &gl) == 0);
    assert(registered->ops->app_swap(registered->context) == 0);
    registered->ops->app_unbind(registered->context);
    registered->ops->app_reclaim(registered->context, 1U);
    assert(registered->ops->begin_reset(registered->context) == ZZM_MINIGL_RESET_QUIESCENT);
    assert(minigl_module_host_reset_count() == 1U);
    minigl_module_host_fini();
    assert(minigl_module_host_transport_fini_count() == 1U);
    assert(minigl_module_host_reset_count() == 2U);
    puts("MiniGL module registration and lifecycle passed");
    return 0;
}
