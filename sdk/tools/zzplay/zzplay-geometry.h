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


/* True when the fit is exactly 1:1, i.e. the native fast path is reachable
 * at this size. */
int zzplay_geometry_is_exact(const ZZPlayRect *rect,
                             uint16_t src_w, uint16_t src_h);

/* Remember/restore the windowed placement across a fullscreen toggle. */
void zzplay_geometry_remember(ZZPlayWindowGeometry *saved,
                              const ZZPlayRect *window);
int zzplay_geometry_restore(const ZZPlayWindowGeometry *saved,
                            ZZPlayRect *window);

#endif /* ZZPLAY_GEOMETRY_H */
