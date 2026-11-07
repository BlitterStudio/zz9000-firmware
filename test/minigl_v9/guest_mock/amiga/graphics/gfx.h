/*
 * Mock AmigaOS graphics/gfx.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GRAPHICS_GFX_H
#define GRAPHICS_GFX_H

#include <exec/types.h>

#define BMA_WIDTH  9
#define BMA_HEIGHT 10
#define BMA_DEPTH 8

struct BitMap {
    UWORD       BytesPerRow;
    UWORD       Rows;
    UWORD       Depth;
    UWORD       Flags;
    UBYTE      *Planes[8];
    /* Host-mock extension: P96-style linear RGBA backing. */
    UBYTE      *mock_memory;
    ULONG       mock_arm_base;
    ULONG       mock_format;
    ULONG       mock_pitch;
    ULONG       mock_width;
    ULONG       mock_height;
    struct BitMap *mock_next;
};

struct RastPort {
    struct BitMap  *BitMap;
    void           *Layer;
    UBYTE            pen;
};

void InitRastPort(struct RastPort *rp);
LONG GetBitMapAttr(struct BitMap *bitmap, LONG attr);

#endif /* GRAPHICS_GFX_H */
