/*
 * Aspect-correct fit, fullscreen letterboxing and geometry memory.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>

#include "../tools/zzplay-controls.h"
#include "../tools/zzplay-geometry.h"

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
  if (zzplay_control_action_from_key('l') != ZZPLAY_CONTROL_TOGGLE_LOOP)
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
  printf("zzplay_geometry_test: all checks passed\n");
  return 0;
}
