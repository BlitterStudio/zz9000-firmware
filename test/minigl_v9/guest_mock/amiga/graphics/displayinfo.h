/*
 * Mock AmigaOS graphics/displayinfo.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GRAPHICS_DISPLAYINFO_H
#define GRAPHICS_DISPLAYINFO_H

#include <exec/types.h>
#include <utility/tagitem.h>

#define INVALID_ID (~(ULONG)0)

#define BIDTAG_NominalWidth  (TAG_USER + 16)
#define BIDTAG_NominalHeight (TAG_USER + 17)
#define BIDTAG_Depth         (TAG_USER + 18)

struct ViewPort {
    ULONG pad;
};

#define DTAG_DIMS 0x80000000

struct DimensionInfo {
    ULONG Header;
    struct {
        UWORD MinX;
        UWORD MinY;
        UWORD MaxX;
        UWORD MaxY;
    } Nominal;
};

ULONG NextDisplayInfo(ULONG last_id);
LONG  GetDisplayInfoData(void *handle, UBYTE *buffer, LONG size, ULONG tag, ULONG id);
ULONG GetVPModeID(struct ViewPort *vp);
ULONG BestModeID(Tag tag1, ...);

#endif /* GRAPHICS_DISPLAYINFO_H */
