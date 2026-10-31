/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MINIGL_MODULE_TEST_HOST_H
#define MINIGL_MODULE_TEST_HOST_H

#include <stdint.h>

#include "zzm_minigl_host.h"

/* Links the module entry point to deterministic v3 host-service fakes. */
int minigl_module_host_init(void);
void minigl_module_host_fini(void);
const zzm_minigl_registration_v1_t *minigl_module_host_registration(void);
unsigned int minigl_module_host_doorbell_count(void);
unsigned int minigl_module_host_reset_count(void);
unsigned int minigl_module_host_transport_fini_count(void);

#endif /* MINIGL_MODULE_TEST_HOST_H */
