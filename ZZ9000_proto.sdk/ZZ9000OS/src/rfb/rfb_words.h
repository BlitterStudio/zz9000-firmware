/* Remote-framebuffer control words; must match src/tools/web/client/console/.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ZZ9K_RFB_WORDS_H
#define ZZ9K_RFB_WORDS_H

#include "rfb/rfb_encode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The longest `geom` there is.  A `pal` is 4 + 6 * (1 << depth) and the caller
 * sizes that one from the depth it has. */
#define RFB_WORD_GEOM_MAX   51
#define RFB_WORD_PAL_MAX    (5u + 6u * (1u << RFB_MAX_DEPTH))

/* What a pointer sprite may be, which is what sizes the `ptr` word.  An image
 * past any of these is REFUSED rather than truncated. */
#define RFB_PTR_MAX_W       64u
#define RFB_PTR_MAX_H       64u
#define RFB_PTR_MAX_DEPTH   4u
#define RFB_PTR_ROW_BYTES(w) ((((w) + 15u) / 16u) * 2u)
#define RFB_PTR_MAX_BITS    (RFB_PTR_MAX_DEPTH * \
                             RFB_PTR_ROW_BYTES(RFB_PTR_MAX_W) * RFB_PTR_MAX_H)
#define RFB_PTR_MAX_COLOURS ((1u << RFB_PTR_MAX_DEPTH) - 1u)

/* "ptr " and seven numbers with their spaces, then two hex digits a byte. */
#define RFB_WORD_PTR_MAX    (4u + 7u * 7u + \
                             2u * (3u * RFB_PTR_MAX_COLOURS + RFB_PTR_MAX_BITS))

enum {
    RFB_IN_NONE = 0,
    RFB_IN_REFRESH,
    RFB_IN_POINTER,     /* a = x, b = y, c = button bitmask               */
    RFB_IN_WHEEL,       /* a = dx, b = dy, notches, either sign           */
    RFB_IN_KEYDOWN,     /* a = rawkey, b = qualifier bits                 */
    RFB_IN_KEYUP,
    RFB_IN_RESET        /* reboot the machine                             */
};

/* The browser's own bitmask, which is also the order IEQUALIFIER_ puts them
 * in. */
#define RFB_BUTTON_LEFT     0x01u
#define RFB_BUTTON_RIGHT    0x02u
#define RFB_BUTTON_MIDDLE   0x04u

typedef struct {
    rfb_u8  kind;
    rfb_s32 a;
    rfb_s32 b;
    rfb_s32 c;
} rfb_input;

/* Characters written, not counting the terminator, or 0 when it would not
 * fit.  Both write a NUL. */
rfb_u32 rfb_word_geom(char *out, rfb_u32 cap, const rfb_geom *g);
rfb_u32 rfb_word_pal(char *out, rfb_u32 cap, const rfb_u8 *rgb, rfb_u32 colours);

/*
 * The pointer image.  `rgb` is 3 * ((1 << depth) - 1) bytes and `bits` is
 * depth * RFB_PTR_ROW_BYTES(width) * height, plane-major.  0 when the shape is
 * one this refuses or the buffer is too small.
 */
typedef struct {
    rfb_u16 width;
    rfb_u16 height;
    rfb_u16 depth;
    rfb_u16 x_scale;    /* screen pixels one sprite pixel covers across */
    rfb_u16 y_scale;    /* screen rows one sprite row covers            */
    rfb_s16 hot_x;      /* sprite pixels from the left                  */
    rfb_s16 hot_y;
} rfb_pointer;

rfb_u32 rfb_word_ptr(char *out, rfb_u32 cap, const rfb_pointer *p,
                     const rfb_u8 *rgb, const rfb_u8 *bits);

/* Fills `ev` and returns 1 when `w` is a word this knows, 0 when it is not.
 * `w` need not be terminated; `len` is what arrived in the text frame.  A
 * malformed word returns 0 rather than acting on half of it. */
int rfb_word_parse(const char *w, rfb_u32 len, rfb_input *ev);

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_RFB_WORDS_H */
