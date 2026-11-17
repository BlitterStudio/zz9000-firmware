/*
 * MiniGL getter output sizes shared by the frontend and SVC boundary.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZAPP_GL_CONTRACT_H
#define ZZAPP_GL_CONTRACT_H

#include <stdint.h>

static inline uint32_t zzapp_gl_get_values(uint32_t pname)
{
    switch (pname) {
    case 0x0b02U:
        return 3U;
    case 0x0b00U: case 0x0b03U: case 0x0b04U: case 0x0b06U:
    case 0x0b07U: case 0x0b53U: case 0x0b66U: case 0x0ba2U:
    case 0x0c10U: case 0x0c22U: case 0x0c23U: case 0x8005U:
        return 4U;
    case 0x0b40U: case 0x0b70U: case 0x0d3aU:
    case 0x846dU: case 0x846eU: case 0x846fU:
        return 2U;
    case 0x0ba6U: case 0x0ba7U: case 0x0ba8U:
        return 16U;
    default:
        return 1U;
    }
}

#endif
