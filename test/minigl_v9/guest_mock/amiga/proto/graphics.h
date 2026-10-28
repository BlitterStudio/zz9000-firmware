/*
 * Mock AmigaOS proto/graphics.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PROTO_GRAPHICS_H
#define PROTO_GRAPHICS_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/layers.h>

extern struct GfxBase *GfxBase;

void WaitTOF(void);
void SetAPen(struct RastPort *rp, ULONG pen);
void RectFill(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1);

#endif /* PROTO_GRAPHICS_H */
