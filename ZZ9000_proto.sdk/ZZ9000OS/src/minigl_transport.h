/*
 * MiniGL wire-v9 transport placement glue (2.9.0 layout).
 *
 * Requests one window from the host-visible heap, binds the wire-v9 service
 * to it and publishes the versioned transport descriptor inside the SDK
 * mailbox reservation. Runs on core 0 at boot and after every Amiga reset
 * (the surface allocator drops the previous window then). Returns a clean
 * "unavailable" -- unbound service, zeroed descriptor -- whenever the
 * placement cannot be satisfied (Zorro II, missing mailbox section, or a
 * fragmented heap), so the Amiga library falls back exactly as on firmware
 * built without the service.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MINIGL_TRANSPORT_H
#define MINIGL_TRANSPORT_H

#include <stdint.h>

#if defined(MINIGL_MODULE)
#include "zzm_runtime.h"

/* Set once by the module entry before the transport is acquired. */
int minigl_transport_set_module_services(
    const zzm_host_window_service_t *host_window,
    const zzm_transport_service_t *transport, uint32_t owner);
void minigl_transport_fini(void);
#endif

/* Allocate + bind + publish. 0 on success, nonzero when unavailable. */
int minigl_transport_init(void);

/* ARM base of the bound transport window; 0 when unavailable. */
uint32_t minigl_transport_window_base(void);

/* Rewrite the published descriptor (zeroed when unavailable). Exposed for
 * tests; firmware calls it from minigl_transport_init(). */
void minigl_transport_publish_descriptor(void);

#endif /* MINIGL_TRANSPORT_H */
