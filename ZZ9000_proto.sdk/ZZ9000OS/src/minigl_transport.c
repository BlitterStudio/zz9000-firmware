/*
 * MiniGL wire-v9 transport placement glue (2.9.0 layout).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "minigl_transport.h"
#include "mw_service.h"
#include "memorymap.h"
#include "surface_allocator.h"
#include "sdk_aperture_layout.h"
#include <stdio.h>
#include <string.h>

#if defined(__arm__) && !defined(MINIGL_HOST_TEST)
#include "xil_cache.h"
#endif

#if defined(MINIGL_MODULE)

static const zzm_host_window_service_t *g_host_window;
static const zzm_transport_service_t *g_transport;
static uint32_t g_owner;
static uint32_t g_window_base;
static uint32_t g_window_generation;

int minigl_transport_set_module_services(
    const zzm_host_window_service_t *host_window,
    const zzm_transport_service_t *transport, uint32_t owner)
{
    if (!host_window || !transport || !owner || !host_window->alloc ||
        !host_window->free || !transport->publish_descriptor ||
        !transport->retract_descriptor)
        return -1;
    g_host_window = host_window;
    g_transport = transport;
    g_owner = owner;
    return 0;
}

uint32_t minigl_transport_window_base(void)
{
    return g_window_base;
}

void minigl_transport_publish_descriptor(void)
{
    ZZ9KMiniGLTransportDescriptor snapshot;
    ZZ9KMiniGLTransportDescriptorWire wire;

    if (!g_transport)
        return;
    if (mw_service_get_transport_descriptor(&snapshot) != 0) {
        g_transport->retract_descriptor(ZZM_MODULE_MINIGL);
        return;
    }
    mw_service_serialize_transport_descriptor(&snapshot, &wire);
    (void)g_transport->publish_descriptor(ZZM_MODULE_MINIGL, &wire,
                                           sizeof(wire));
}

int minigl_transport_init(void)
{
    zzm_host_window_t window;

    if (!g_host_window || !g_transport || g_window_base)
        return g_window_base ? 0 : -1;
    if (g_host_window->alloc(g_owner, MW_WINDOW_SIZE_Z3,
                             MW_TRANSPORT_WINDOW_ALIGNMENT, &window) != 0)
        return -1;
    if (!mw_service_bind_region((volatile void *)(uintptr_t)window.arm_address,
                                window.bytes)) {
        (void)g_host_window->free(g_owner, window.arm_address,
                                  window.generation);
        return -1;
    }
    mw_service_set_vram_limit_arm(window.arm_address);
    g_window_base = window.arm_address;
    g_window_generation = window.generation;
    minigl_transport_publish_descriptor();
    return 0;
}

void minigl_transport_fini(void)
{
    if (g_transport)
        g_transport->retract_descriptor(ZZM_MODULE_MINIGL);
    mw_service_bind_region(NULL, 0);
    if (g_window_base && g_host_window)
        (void)g_host_window->free(g_owner, g_window_base,
                                  g_window_generation);
    g_window_base = 0;
    g_window_generation = 0;
}

#elif !defined(MINIGL_HOST_TEST)

/* The transport window is not host-testable here: host tests bind explicit
 * regions through mw_service_bind_region() instead. */
/* Non-module firmware retains the resident transport implementation. */

static uint32_t g_window_base;

uint32_t minigl_transport_window_base(void)
{
	return g_window_base;
}

/* Publish (or retract) the versioned descriptor inside the SDK mailbox
 * reservation. The reservation is Normal non-cacheable on Zorro III, so no
 * cache maintenance is needed after the write. */
void minigl_transport_publish_descriptor(void)
{
	volatile ZZ9KMiniGLTransportDescriptorWire *desc =
		(volatile ZZ9KMiniGLTransportDescriptor *)
		(SDK_MAILBOX_Z3_ADDRESS + SDK_MAILBOX_MINIGL_DESC_OFFSET);
	ZZ9KMiniGLTransportDescriptor snapshot;
	ZZ9KMiniGLTransportDescriptorWire wire;
	/* The window shares the mailbox MMU section: plain stores are
	 * visible to the 68k without cache maintenance. */
	if (mw_service_get_transport_descriptor(&snapshot) == 0) {
		mw_service_serialize_transport_descriptor(&snapshot, &wire);
		*desc = wire;
	}
	__asm__ __volatile__("dsb" ::: "memory");
}

int minigl_transport_init(void)
{
	uint32_t base;

	g_window_base = 0;

	/* Zorro II decision: the Z2 host window budget is 64 KiB total
	 * (16 KiB for clients on generation-2 bitstreams; see
	 * learnings/z2-host-window-client-budgets.md) while even the
	 * single-slot transport needs 68 KiB, and the Z2 path has no SDK
	 * mailbox reservation to publish through. Report the service
	 * unavailable; the library falls back as on firmware without it. */
	if (!sdk_aperture_runtime_is_zorro3()) {
		mw_service_bind_region(NULL, 0);
		minigl_transport_publish_descriptor();
		printf("[MGL9] unavailable: Zorro II host window cannot hold the transport\r\n");
		return -1;
	}

	/* One allocation from the host-visible heap, constrained to the gap
	 * between the SDK mailbox and direct-ring reservations inside the
	 * already non-cacheable 1 MiB MMU section. */
	base = surface_allocator_alloc_range(
		MW_WINDOW_SIZE_Z3, MW_TRANSPORT_WINDOW_ALIGNMENT,
		MW_TRANSPORT_PLACEMENT_LO, MW_TRANSPORT_PLACEMENT_HI);
	if (base == 0) {
		mw_service_bind_region(NULL, 0);
		minigl_transport_publish_descriptor();
		printf("[MGL9] unavailable: no host-visible heap room for the transport\r\n");
		return -1;
	}

	if (!mw_service_bind_region((volatile void *)(uintptr_t)base,
	                            MW_WINDOW_SIZE_Z3)) {
		mw_service_bind_region(NULL, 0);
		minigl_transport_publish_descriptor();
		printf("[MGL9] unavailable: window bind failed\r\n");
		return -1;
	}
	mw_service_set_vram_limit_arm(base);
	g_window_base = base;
	minigl_transport_publish_descriptor();
	printf("[MGL9] transport window 0x%08lx (host-visible heap)\r\n",
	       (unsigned long)base);
	return 0;
}

#else /* MINIGL_HOST_TEST */

uint32_t minigl_transport_window_base(void)
{
	return 0;
}

void minigl_transport_publish_descriptor(void)
{
}

int minigl_transport_init(void)
{
	return -1;
}

#endif /* MINIGL_HOST_TEST */
