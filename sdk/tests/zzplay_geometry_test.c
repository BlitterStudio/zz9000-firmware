/*
 * Aspect-correct fit, fullscreen letterboxing and geometry memory.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>

#include "../tools/zzplay/zzplay-controls.h"
#include "../tools/zzplay/zzplay-geometry.h"

static int test_exact_fit(void)
{
  ZZPlayRect r = zzplay_geometry_fit(320U, 240U, 320U, 240U);

  if (r.width != 320U || r.height != 240U) return 1;
  if (r.x != 0 || r.y != 0) return 2;
  if (!zzplay_geometry_is_exact(&r, 320U, 240U)) return 3;
  return 0;
}

/* 4:3 into a 16:9-ish area: height binds, pillarboxed left and right. */
static int test_pillarbox(void)
{
  ZZPlayRect r = zzplay_geometry_fit(320U, 240U, 800U, 480U);

  if (r.height != 480U) return 1;
  if (r.width != 640U) return 2;
  if (r.x != 80) return 3;
  if (r.y != 0) return 4;
  if (zzplay_geometry_is_exact(&r, 320U, 240U)) return 5;
  return 0;
}

/* A wide source into a squarer area: width binds, letterboxed top/bottom. */
static int test_letterbox(void)
{
  ZZPlayRect r = zzplay_geometry_fit(640U, 272U, 640U, 480U);

  if (r.width != 640U) return 1;
  if (r.height != 272U) return 2;
  if (r.x != 0) return 3;
  if (r.y != 104) return 4;
  return 0;
}

static int test_downscale_keeps_aspect(void)
{
  ZZPlayRect r = zzplay_geometry_fit(640U, 480U, 320U, 320U);

  if (r.width != 320U || r.height != 240U) return 1;
  if (r.y != 40 || r.x != 0) return 2;
  return 0;
}

/* Never exceed the available area, and never collapse to zero. */
static int test_bounds(void)
{
  ZZPlayRect r;

  r = zzplay_geometry_fit(1000U, 1U, 100U, 100U);
  if (r.width > 100U || r.height > 100U) return 1;
  if (r.width == 0U || r.height == 0U) return 2;
  r = zzplay_geometry_fit(1U, 1000U, 100U, 100U);
  if (r.width == 0U || r.height == 0U) return 3;
  if (r.width > 100U || r.height > 100U) return 4;
  /* Degenerate inputs produce an empty rect rather than dividing by zero. */
  r = zzplay_geometry_fit(0U, 240U, 320U, 240U);
  if (r.width != 0U || r.height != 0U) return 5;
  r = zzplay_geometry_fit(320U, 240U, 0U, 240U);
  if (r.width != 0U || r.height != 0U) return 6;
  return 0;
}

/* The windowed player refits after every resize, and each refit that
 * changes the size causes another resize (A4000: a 1080p window on a
 * 1280x720 screen shrank by a pixel per frame, 1194x672, 1194x671,
 * 1192x671...). The size a fit produces must be left alone. */
static int refit_until_stable(uint16_t src_w, uint16_t src_h,
                              uint16_t avail_w, uint16_t avail_h,
                              ZZPlayRect *window)
{
  unsigned i;

  *window = zzplay_geometry_fit(src_w, src_h, avail_w, avail_h);
  for (i = 0U; i < 20U; i++) {
    ZZPlayRect again;

    if (zzplay_geometry_is_fitted(window->width, window->height,
                                  src_w, src_h))
      return 0;
    again = zzplay_geometry_fit(src_w, src_h, window->width, window->height);
    window->width = again.width;
    window->height = again.height;
  }
  return 1;
}

static int test_refit_is_stable(void)
{
  ZZPlayRect w;

  if (refit_until_stable(1920U, 1080U, 1248U, 672U, &w)) return 1;
  if (w.width != 1194U || w.height != 672U) return 2;
  if (refit_until_stable(720U, 1280U, 1248U, 672U, &w)) return 3;
  if (w.width != 378U || w.height != 672U) return 4;
  /* A window the user dragged off the aspect is still corrected. */
  if (zzplay_geometry_is_fitted(1194U, 600U, 1920U, 1080U)) return 5;
  if (!zzplay_geometry_is_fitted(640U, 480U, 640U, 480U)) return 6;
  if (zzplay_geometry_is_fitted(640U, 482U, 640U, 480U)) return 7;
  return 0;
}


static int test_geometry_memory(void)
{
  ZZPlayWindowGeometry saved;
  ZZPlayRect window;
  ZZPlayRect restored;

  saved.valid = 0;
  restored.width = 1U;
  /* Nothing remembered yet: restore must refuse rather than hand back
   * uninitialised geometry. */
  if (zzplay_geometry_restore(&saved, &restored)) return 1;

  window.x = 30;
  window.y = 40;
  window.width = 512U;
  window.height = 384U;
  zzplay_geometry_remember(&saved, &window);
  if (!zzplay_geometry_restore(&saved, &restored)) return 2;
  if (restored.x != 30 || restored.y != 40 ||
      restored.width != 512U || restored.height != 384U)
    return 3;
  return 0;
}

static int test_controls(void)
{
  ZZPlayControlInput input;

  if (zzplay_control_action_from_key(' ') != ZZPLAY_CONTROL_TOGGLE_PAUSE)
    return 1;
  if (zzplay_control_action_from_key(0x1bU) != ZZPLAY_CONTROL_STOP_KEY)
    return 2;
  if (zzplay_control_action_from_key('q') != ZZPLAY_CONTROL_STOP_KEY)
    return 3;
  if (zzplay_control_action_from_key('Q') != ZZPLAY_CONTROL_STOP_KEY)
    return 4;
  if (zzplay_control_action_from_key('f') !=
      ZZPLAY_CONTROL_TOGGLE_FULLSCREEN)
    return 5;
  if (zzplay_control_action_from_key('F') !=
      ZZPLAY_CONTROL_TOGGLE_FULLSCREEN)
    return 6;
  if (zzplay_control_action_from_key('l') != ZZPLAY_CONTROL_CYCLE_REPEAT)
    return 7;
  if (zzplay_control_action_from_key('x') != ZZPLAY_CONTROL_NONE)
    return 8;

  /* Stops outrank other input seen in the same poll. */
  input.ctrl_c = 0;
  input.window_close = 0;
  input.key = 'f';
  if (zzplay_control_resolve(&input) != ZZPLAY_CONTROL_TOGGLE_FULLSCREEN)
    return 9;
  input.window_close = 1;
  if (zzplay_control_resolve(&input) != ZZPLAY_CONTROL_STOP_WINDOW)
    return 10;
  input.ctrl_c = 1;
  if (zzplay_control_resolve(&input) != ZZPLAY_CONTROL_STOP_CTRL_C)
    return 11;

  /* Every stop must map to a real stop reason so cleanup runs. */
  if (!zzplay_control_is_stop(ZZPLAY_CONTROL_STOP_KEY)) return 12;
  if (zzplay_control_is_stop(ZZPLAY_CONTROL_TOGGLE_PAUSE)) return 13;
  if (zzplay_control_stop_reason_from_action(ZZPLAY_CONTROL_STOP_KEY) ==
      ZZPLAY_STOP_NONE)
    return 14;
  if (zzplay_control_stop_reason_from_action(ZZPLAY_CONTROL_STOP_CTRL_C) !=
      ZZPLAY_STOP_CTRL_C)
    return 15;
  /* The pre-existing three-argument form must keep behaving. */
  if (zzplay_control_action(0, 0, 1) != ZZPLAY_CONTROL_TOGGLE_PAUSE)
    return 16;
  if (zzplay_control_action(1, 1, 1) != ZZPLAY_CONTROL_STOP_CTRL_C)
    return 17;
  return 0;
}

static int test_portrait_phone(void)
{
  ZZPlayRect a = zzplay_geometry_fit(720U, 1280U, 640U, 480U);
  ZZPlayRect b = zzplay_geometry_fit(720U, 1280U, 800U, 600U);
  ZZPlayRect c = zzplay_geometry_fit(720U, 1280U, 1920U, 1080U);

  /* Height binds on every landscape screen: pillarbox, not stretch. */
  if (a.height != 480U || a.width != 270U || a.x != 185 || a.y != 0) {
    return 1;
  }
  if (b.height != 600U || b.width != 337U || b.x != 231 || b.y != 0) {
    return 2;
  }
  if (c.height != 1080U || c.width != 607U || c.x != 656 || c.y != 0) {
    return 3;
  }
  if ((uint32_t)a.width * 1280U / 720U > 480U) {
    return 4;
  }
  return 0;
}

/* The fullscreen screen must be asked at the video's display size and then
 * in growing steps, because the card's overlay plane only shows a window
 * at or above the frame's size correctly (A4000: a 720x576 MPEG-2 frame
 * letterboxed into a 720x405 window on a 720x576 screen showed just its
 * top rows, while the same clip windowed at 1024x576 was correct). */
static int test_fullscreen_screen_request(void)
{
  uint16_t w;
  uint16_t h;

  /* PAL 16:9: the first ask is the display size, not the coded 720x576. */
  if (!zzplay_geometry_screen_request(0U, 1024U, 576U, 720U, 576U, &w, &h)) {
    return 1;
  }
  if (w != 1024U || h != 576U) return 2;
  /* The ladder grows, so a screen mode too small for the display can be
   * left behind without ever asking for a downscale. */
  if (!zzplay_geometry_screen_request(1U, 1024U, 576U, 720U, 576U, &w, &h)) {
    return 3;
  }
  if (w <= 1024U || h <= 576U) return 4;
  if (zzplay_geometry_screen_request(4U, 1024U, 576U, 720U, 576U, &w, &h)) {
    return 5;
  }
  /* No display aspect (MPEG-1, WebP): the coded frame is the ask, with
   * the historical 640x480 pixel-doubling floor. */
  if (!zzplay_geometry_screen_request(0U, 0U, 0U, 320U, 288U, &w, &h)) {
    return 6;
  }
  if (w != 640U || h != 480U) return 7;
  return 0;
}

static int test_fullscreen_plan_reuse_modes(void)
{
  ZZPlayFullscreenPlan plan;

  /* 15-bit Workbench */
  zzplay_geometry_fullscreen_plan(0x50001000UL, 800U, 600U, 15U, &plan);
  if (plan.action != ZZPLAY_FS_REUSE_MODE) return 1;
  if (plan.mode_id != 0x50001000UL) return 2;
  if (plan.target_depth != 15U) return 3;

  /* 16-bit Workbench */
  zzplay_geometry_fullscreen_plan(0x50002000UL, 1024U, 768U, 16U, &plan);
  if (plan.action != ZZPLAY_FS_REUSE_MODE) return 4;
  if (plan.mode_id != 0x50002000UL) return 5;
  if (plan.target_depth != 16U) return 6;

  /* 32-bit Workbench */
  zzplay_geometry_fullscreen_plan(0x50003000UL, 1920U, 1080U, 32U, &plan);
  if (plan.action != ZZPLAY_FS_REUSE_MODE) return 7;
  if (plan.mode_id != 0x50003000UL) return 8;
  if (plan.target_depth != 32U) return 9;

  /* Truecolor Workbench reported as depth 24 (Picasso96 reports a
   * 32-bit screen this way): reuse it at the depth it reports. */
  zzplay_geometry_fullscreen_plan(0x50003001UL, 1280U, 720U, 24U, &plan);
  if (plan.action != ZZPLAY_FS_REUSE_MODE) return 10;
  if (plan.mode_id != 0x50003001UL) return 11;
  if (plan.target_depth != 24U) return 12;

  return 0;
}

static int test_fullscreen_plan_8bit_and_unsupported(void)
{
  ZZPlayFullscreenPlan plan;

  /* 8-bit CLUT Workbench: matches resolution, forces 16-bit depth */
  zzplay_geometry_fullscreen_plan(0x50004000UL, 1280U, 720U, 8U, &plan);
  if (plan.action != ZZPLAY_FS_MATCH_WB_RES) return 1;
  if (plan.width != 1280U || plan.height != 720U) return 2;
  if (plan.target_depth != 16U) return 3;

  /* 4-bit Workbench */
  zzplay_geometry_fullscreen_plan(0x50004001UL, 800U, 600U, 4U, &plan);
  if (plan.action != ZZPLAY_FS_MATCH_WB_RES) return 4;
  if (plan.width != 800U || plan.height != 600U) return 5;
  if (plan.target_depth != 16U) return 6;

  /* 12-bit depth: not an overlay format */
  zzplay_geometry_fullscreen_plan(0x50004002UL, 1024U, 768U, 12U, &plan);
  if (plan.action != ZZPLAY_FS_MATCH_WB_RES) return 7;
  if (plan.width != 1024U || plan.height != 768U) return 8;
  if (plan.target_depth != 16U) return 9;

  return 0;
}

static int test_fullscreen_plan_doubled_and_fallback(void)
{
  ZZPlayFullscreenPlan plan;

  /* Width < 640: pixel-doubled, falls back to ladder */
  zzplay_geometry_fullscreen_plan(0x50005000UL, 320U, 240U, 16U, &plan);
  if (plan.action != ZZPLAY_FS_FALLBACK_LADDER) return 1;
  if (plan.target_depth != 16U) return 2;

  /* Width < 640 on 32-bit: preserves 32-bit depth in ladder */
  zzplay_geometry_fullscreen_plan(0x50005001UL, 320U, 240U, 32U, &plan);
  if (plan.action != ZZPLAY_FS_FALLBACK_LADDER) return 3;
  if (plan.target_depth != 32U) return 4;

  /* Height < 480: pixel-doubled */
  zzplay_geometry_fullscreen_plan(0x50005002UL, 640U, 400U, 16U, &plan);
  if (plan.action != ZZPLAY_FS_FALLBACK_LADDER) return 5;
  if (plan.target_depth != 16U) return 6;

  /* Width < 640 on 8-bit: falls back to 16-bit depth in ladder */
  zzplay_geometry_fullscreen_plan(0x50005003UL, 320U, 256U, 8U, &plan);
  if (plan.action != ZZPLAY_FS_FALLBACK_LADDER) return 7;
  if (plan.target_depth != 16U) return 8;

  /* Invalid mode ID: falls back to ladder */
  zzplay_geometry_fullscreen_plan(ZZPLAY_INVALID_MODE_ID, 1920U, 1080U, 32U, &plan);
  if (plan.action != ZZPLAY_FS_FALLBACK_LADDER) return 9;
  if (plan.target_depth != 32U) return 10;

  /* Invalid mode ID on 8-bit: falls back to 16-bit ladder */
  zzplay_geometry_fullscreen_plan(ZZPLAY_INVALID_MODE_ID, 1024U, 768U, 8U, &plan);
  if (plan.action != ZZPLAY_FS_FALLBACK_LADDER) return 11;
  if (plan.target_depth != 16U) return 12;

  return 0;
}

static int test_aspect_fit_up_and_down(void)
{
  ZZPlayRect r;

  /* Fit UP (4:3 into 16:9 area: pillarboxed, centered) */
  r = zzplay_geometry_fit(320U, 240U, 1920U, 1080U);
  if (r.width != 1440U || r.height != 1080U) return 1;
  if (r.x != 240 || r.y != 0) return 2;

  /* Fit UP (16:9 into 16:10 area: letterboxed, centered) */
  r = zzplay_geometry_fit(640U, 360U, 1920U, 1200U);
  if (r.width != 1920U || r.height != 1080U) return 3;
  if (r.x != 0 || r.y != 60) return 4;

  /* Fit DOWN (16:9 into 4:3 area: letterboxed, centered) */
  r = zzplay_geometry_fit(1920U, 1080U, 800U, 600U);
  if (r.width != 800U || r.height != 450U) return 5;
  if (r.x != 0 || r.y != 75) return 6;

  /* Fit DOWN (16:9 into 640x480 area: letterboxed, centered) */
  r = zzplay_geometry_fit(1920U, 1080U, 640U, 480U);
  if (r.width != 640U || r.height != 360U) return 7;
  if (r.x != 0 || r.y != 60) return 8;

  /* Fit DOWN (4:3 into 16:9 area: pillarboxed, centered) */
  r = zzplay_geometry_fit(1280U, 960U, 800U, 450U);
  if (r.width != 600U || r.height != 450U) return 9;
  if (r.x != 100 || r.y != 0) return 10;

  /* Anamorphic fit DOWN (1024x576 aspect into 720x576 screen) */
  r = zzplay_geometry_fit(1024U, 576U, 720U, 576U);
  if (r.width != 720U || r.height != 405U) return 11;
  if (r.x != 0 || r.y != 85) return 12;

  /* Anamorphic fit UP (1024x576 aspect into 1920x1080 screen) */
  r = zzplay_geometry_fit(1024U, 576U, 1920U, 1080U);
  if (r.width != 1920U || r.height != 1080U) return 13;
  if (r.x != 0 || r.y != 0) return 14;

  return 0;
}

int main(void)
{
  int rc;
  rc = test_exact_fit();
  if (rc != 0) { printf("exact %d\n", rc); return 10 + rc; }
  rc = test_pillarbox();
  if (rc != 0) { printf("pillarbox %d\n", rc); return 30 + rc; }
  rc = test_letterbox();
  if (rc != 0) { printf("letterbox %d\n", rc); return 50 + rc; }
  rc = test_downscale_keeps_aspect();
  if (rc != 0) { printf("downscale %d\n", rc); return 70 + rc; }
  rc = test_bounds();
  if (rc != 0) { printf("bounds %d\n", rc); return 90 + rc; }
  rc = test_geometry_memory();
  if (rc != 0) { printf("memory %d\n", rc); return 110 + rc; }
  rc = test_controls();
  if (rc != 0) { printf("controls %d\n", rc); return 130 + rc; }
  rc = test_refit_is_stable();
  if (rc != 0) { printf("refit %d\n", rc); return 150 + rc; }
  rc = test_portrait_phone();
  if (rc != 0) { printf("portrait %d\n", rc); return 170 + rc; }
  rc = test_fullscreen_screen_request();
  if (rc != 0) { printf("fsrequest %d\n", rc); return 190 + rc; }
  rc = test_fullscreen_plan_reuse_modes();
  if (rc != 0) { printf("fsreuse %d\n", rc); return 210 + rc; }
  rc = test_fullscreen_plan_8bit_and_unsupported();
  if (rc != 0) { printf("fs8bit %d\n", rc); return 230 + rc; }
  rc = test_fullscreen_plan_doubled_and_fallback();
  if (rc != 0) { printf("fsdoubled %d\n", rc); return 250 + rc; }
  rc = test_aspect_fit_up_and_down();
  if (rc != 0) { printf("fsfit %d\n", rc); return 270 + rc; }
  printf("zzplay_geometry_test: all checks passed\n");
  return 0;
}
