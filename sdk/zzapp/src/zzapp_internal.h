/*
 * Symbols crt0 and the GL wrappers share. Not an app header.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ZZAPP_INTERNAL_H
#define ZZAPP_INTERNAL_H

#include "zzapp.h"

void zzapp_crt0_bind(const zzapp_api *api);
const zzapp_gl *zzapp_gl_bound(void);

#endif
