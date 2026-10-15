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

/* Display modes smaller than 640x480 are pixel-doubled by ZZ9000.card, and
 * the hardware overlay refuses doubled modes. */
#define ZZPLAY_SCREEN_FLOOR_W 640U
#define ZZPLAY_SCREEN_FLOOR_H 480U
#define ZZPLAY_SCREEN_CAP_W 1920U
#define ZZPLAY_SCREEN_CAP_H 1200U
#define ZZPLAY_INVALID_MODE_ID 0xFFFFFFFFUL

typedef enum ZZPlayFullscreenAction {
  ZZPLAY_FS_REUSE_MODE,     /* Workbench mode is usable directly: same ModeID and depth */
  ZZPLAY_FS_MATCH_WB_RES,   /* 8-bit or unsupported depth: ask p96BestModeID for WB res at 16-bit */
  ZZPLAY_FS_FALLBACK_LADDER /* Pixel-doubled (<640x480) or invalid WB: use request ladder */
} ZZPlayFullscreenAction;

typedef struct ZZPlayFullscreenPlan {
  ZZPlayFullscreenAction action;
  uint32_t mode_id;      /* DisplayID to open when action == ZZPLAY_FS_REUSE_MODE */
  uint16_t width;        /* Width to request when action == ZZPLAY_FS_MATCH_WB_RES */
  uint16_t height;       /* Height to request when action == ZZPLAY_FS_MATCH_WB_RES */
  uint16_t target_depth; /* Depth to open screen at (15, 16, 24 or 32) */
} ZZPlayFullscreenPlan;

/* True when the overlay can show a PIP on a screen of this depth: 15/16-bit
 * hicolor or truecolor. Picasso96 reports a 32-bit truecolor screen as
 * depth 24 or 32. */
int zzplay_geometry_depth_is_supported(uint16_t depth);

/* True when screen width and height are pixel-doubled by ZZ9000.card (<640x480). */
int zzplay_geometry_is_doubled(uint16_t width, uint16_t height);

/* Decide how fullscreen should open based on the Workbench display mode and depth.
 * - Usable hicolor or truecolor mode: reuse wb_mode_id at the depth the
 *   Workbench reports (no monitor resync).
 * - 8-bit or other depth: match Workbench width and height at 16-bit depth.
 * - Pixel-doubled (<640x480) or invalid mode: fallback to the request ladder. */
void zzplay_geometry_fullscreen_plan(uint32_t wb_mode_id,
                                     uint16_t wb_width,
                                     uint16_t wb_height,
                                     uint16_t wb_depth,
                                     ZZPlayFullscreenPlan *plan);
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
