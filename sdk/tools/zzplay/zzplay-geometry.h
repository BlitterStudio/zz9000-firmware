/* Aspect-correct placement for zzplay's video window (R7, R10).
 *
 * Pure integer geometry so window/fullscreen/resize behaviour is testable
 * without an Amiga. The FPGA overlay scales whatever rectangle it is given,
 * so preserving the source aspect is entirely the player's job.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_GEOMETRY_H
#define ZZPLAY_GEOMETRY_H

#include <stdint.h>

typedef struct ZZPlayRect {
  int16_t x;
  int16_t y;
  uint16_t width;
  uint16_t height;
} ZZPlayRect;

/* Remembered window placement, so returning from fullscreen restores what
 * the user had rather than snapping back to the source size. */
typedef struct ZZPlayWindowGeometry {
  ZZPlayRect window;
  int valid;
} ZZPlayWindowGeometry;

/* Fit `src_w` x `src_h` inside `avail_w` x `avail_h`, preserving aspect and
 * centring the result. Never enlarges beyond the available area and never
 * returns a zero dimension for a non-empty source. */
ZZPlayRect zzplay_geometry_fit(uint16_t src_w, uint16_t src_h,
                               uint16_t avail_w, uint16_t avail_h);


/* True when a `width` x `height` area already shows `src_w` x `src_h` at
 * its aspect to within the one pixel integer fitting rounds away. Every
 * zzplay_geometry_fit() result passes, so a window resized to a fit must
 * be left alone: fitting it again rounds down once more, and each
 * resulting resize would shrink it by another pixel. */
int zzplay_geometry_is_fitted(uint16_t width, uint16_t height,
                              uint16_t src_w, uint16_t src_h);

/* True when the fit is exactly 1:1, i.e. the native fast path is reachable
 * at this size. */
int zzplay_geometry_is_exact(const ZZPlayRect *rect,
                             uint16_t src_w, uint16_t src_h);

/* True when fitting the display size into a screen of `screen_w` x
 * `screen_h` never shrinks the coded frame: the card's overlay plane is
 * only proven with the window at or above the frame's size, so a
 * fullscreen screen that would letterbox the coded frame smaller must not
 * be chosen while a larger mode exists. `display_w/h` 0 means the file
 * names no aspect (MPEG-1, WebP) and the coded frame is the fit source. */
int zzplay_geometry_screen_upscales(uint16_t screen_w, uint16_t screen_h,
                                    uint16_t display_w, uint16_t display_h,
                                    uint16_t src_w, uint16_t src_h);

/* Nominal size to ask P96 for when opening the fullscreen screen.
 * Attempt 0 is the display size (coded frame without an aspect) with the
 * 640x480 floor the card's pixel-doubling refusal requires; later attempts
 * grow it so a mode too small to hold the display can be left behind.
 * Returns 0 once the ladder is exhausted; the sizes stay capped at modes
 * every ZZ9000 offers. */
int zzplay_geometry_screen_request(unsigned attempt,
                                   uint16_t display_w, uint16_t display_h,
                                   uint16_t src_w, uint16_t src_h,
                                   uint16_t *req_w, uint16_t *req_h);

/* Remember/restore the windowed placement across a fullscreen toggle. */
void zzplay_geometry_remember(ZZPlayWindowGeometry *saved,
                              const ZZPlayRect *window);
int zzplay_geometry_restore(const ZZPlayWindowGeometry *saved,
                            ZZPlayRect *window);

#endif /* ZZPLAY_GEOMETRY_H */
