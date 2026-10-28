/*
 * Mock AmigaOS proto/Picasso96.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PROTO_PICASSO96_H
#define PROTO_PICASSO96_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <libraries/Picasso96.h>
#include <utility/tagitem.h>

extern struct Library *P96Base;

ULONG p96BestModeIDTags(Tag tag1, ...);
LONG  p96GetBitMapAttr(struct BitMap *bitmap, ULONG attr);
ULONG p96GetModeIDAttr(ULONG modeid, ULONG attr);
LONG  p96LockBitMap(struct BitMap *bitmap, void *lockinfo, ULONG flags);
void  p96UnlockBitMap(struct BitMap *bitmap, LONG lock);
void  p96ReadPixelArray(struct RenderInfo *ri, ULONG srcx, ULONG srcy,
                        struct RastPort *rp, ULONG destx, ULONG desty,
                        ULONG width, ULONG height);
void  p96WritePixelArray(struct RenderInfo *ri, ULONG srcx, ULONG srcy,
                         struct RastPort *rp, ULONG destx, ULONG desty,
                         ULONG width, ULONG height);
struct BitMap *p96AllocBitMap(ULONG width, ULONG height, ULONG depth,
                              ULONG flags, struct BitMap *friend,
                              ULONG rgbformat);
void p96FreeBitMap(struct BitMap *bitmap);

#endif /* PROTO_PICASSO96_H */
