/*
 * Mock AmigaOS graphics/clip.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GRAPHICS_CLIP_H
#define GRAPHICS_CLIP_H

#include <exec/types.h>

struct Rectangle {
    WORD MinX;
    WORD MinY;
    WORD MaxX;
    WORD MaxY;
};

struct ClipRect {
    struct ClipRect *Next;
    struct Rectangle bounds;
    LONG             obscured;
};

struct Layer {
    struct ClipRect *ClipRect;
    LONG             bounds_pad;
};

#endif /* GRAPHICS_CLIP_H */
