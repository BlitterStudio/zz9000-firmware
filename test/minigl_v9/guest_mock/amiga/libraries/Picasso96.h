/*
 * Mock AmigaOS libraries/Picasso96.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef LIBRARIES_PICASSO96_H
#define LIBRARIES_PICASSO96_H

#include <exec/types.h>
#include <graphics/gfx.h>
#include <utility/tagitem.h>

/* RGB format constants (wire RGBFTYPE) */
#define RGBFB_R8G8B8       2UL
#define RGBFB_B8G8R8       3UL
#define RGBFB_R5G6B5PC     4UL
#define RGBFB_R5G5B5PC     5UL
#define RGBFB_A8R8G8B8     6UL
#define RGBFB_A8B8G8R8     7UL
#define RGBFB_R8G8B8A8     8UL
#define RGBFB_B8G8R8A8     9UL
#define RGBFB_R5G6B5      10UL
#define RGBFB_R5G5B5      11UL
#define RGBFB_B5G6R5PC    12UL
#define RGBFB_B5G5R5PC    13UL

#define RGBFB_NONE         0UL
#define RGBFB_CLUT         1UL

/* BitMap attribute tags */
#define P96BMA_MEMORY        1UL
#define P96BMA_BYTESPERROW   3UL
#define P96BMA_BYTESPERPIXEL 5UL
#define P96BMA_RGBFORMAT     8UL
#define P96BMA_WIDTH         9UL
#define P96BMA_HEIGHT       10UL

/* Mode attribute tags */
#define P96IDA_ISP96 4UL
#define P96IDA_DEPTH 7UL

/* BestModeID tags */
#define P96BIDTAG_NominalWidth  (TAG_USER + 16)
#define P96BIDTAG_NominalHeight (TAG_USER + 17)
#define P96BIDTAG_Depth         (TAG_USER + 18)

#define BMF_CLEAR      (1UL << 0)
#define BMF_DISPLAYABLE (1UL << 2)

struct RenderInfo {
    UBYTE *Memory;
    ULONG  BytesPerRow;
    ULONG  pad;
    ULONG  RGBFormat;
};

#endif /* LIBRARIES_PICASSO96_H */
