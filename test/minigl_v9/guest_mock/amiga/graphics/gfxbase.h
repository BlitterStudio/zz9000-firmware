/*
 * Mock AmigaOS graphics/gfxbase.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GRAPHICS_GFXBASE_H
#define GRAPHICS_GFXBASE_H

#include <exec/types.h>
#include <exec/libraries.h>

struct GfxBase {
    struct Library LibNode;
};

#endif /* GRAPHICS_GFXBASE_H */
