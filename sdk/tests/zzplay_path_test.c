/*
 * R10 presentation-path classification.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "zz9k/abi.h"
#include "../tools/zzplay-path.h"

static void build(uint64_t *value, uint16_t src_w, uint16_t src_h,
                  uint16_t dst_w, uint16_t dst_h, int16_t dst_x,
                  int16_t dst_y, uint16_t scr_w, uint16_t scr_h)
{
  value[0] = ZZ9K_MEDIA_PACK_PAIR(src_w, src_h);
  value[1] = ZZ9K_MEDIA_PACK_PAIR(dst_w, dst_h);
  value[2] = ZZ9K_MEDIA_PACK_PAIR(dst_x, dst_y);
  value[3] = ZZ9K_MEDIA_PACK_PAIR(scr_w, scr_h);
}

static const uint32_t live =
    ZZ9K_MEDIA_PRESENT_CONFIGURED | ZZ9K_MEDIA_PRESENT_ACTIVE |
    ZZ9K_MEDIA_PRESENT_OWNED;

static int test_unknown_when_page_unsupported(void)
{
  ZZPlayPresentInfo info;
  uint64_t value[4];

  build(value, 320U, 240U, 320U, 240U, 0, 0, 640U, 480U);
  /* Firmware without page 4 rejects the request. That must read as
   * "unknown", never as a decoded path and never as a failure. */
  zzplay_present_from_status(ZZ9K_STATUS_BAD_REQUEST, live, value, &info);
  if (info.path != ZZPLAY_PATH_UNKNOWN) return 1;
  if (info.src_w != 0U || info.dst_w != 0U) return 2;
  zzplay_present_from_status(ZZ9K_STATUS_OK, live | ZZ9K_MEDIA_PRESENT_NATIVE,
                             value, &info);
  if (info.path != ZZPLAY_PATH_NATIVE_1_1) return 3;
  return 0;
}

static int test_native_one_to_one(void)
{
  ZZPlayPresentInfo info;
  uint64_t value[4];

  build(value, 320U, 240U, 320U, 240U, 40, 20, 640U, 480U);
  zzplay_present_classify(live | ZZ9K_MEDIA_PRESENT_NATIVE, value, &info);
  if (info.path != ZZPLAY_PATH_NATIVE_1_1) return 1;
  if (info.clipped != 0 || info.owned != 1) return 2;
  if (info.src_w != 320U || info.src_h != 240U ||
      info.dst_x != 40 || info.dst_y != 20 ||
      info.screen_w != 640U || info.screen_h != 480U)
    return 3;
  return 0;
}

static int test_native_scaled(void)
{
  ZZPlayPresentInfo info;
  uint64_t value[4];

  build(value, 320U, 240U, 640U, 480U, 0, 0, 640U, 480U);
  zzplay_present_classify(live | ZZ9K_MEDIA_PRESENT_NATIVE, value, &info);
  if (info.path != ZZPLAY_PATH_NATIVE_SCALED) return 1;
  if (info.clipped != 0) return 2;

  /* Downscale is equally "scaled". */
  build(value, 640U, 480U, 320U, 240U, 0, 0, 640U, 480U);
  zzplay_present_classify(live | ZZ9K_MEDIA_PRESENT_NATIVE, value, &info);
  if (info.path != ZZPLAY_PATH_NATIVE_SCALED) return 3;
  return 0;
}

/* AE4: exact size but partly off-screen is no longer "native 1:1" — the
 * requirement defines 1:1 as fully visible. Negative origins arrive as
 * two's-complement halves and must sign-extend. */
static int test_clipped_exact_size_is_not_one_to_one(void)
{
  ZZPlayPresentInfo info;
  uint64_t value[4];

  build(value, 320U, 240U, 320U, 240U, -16, -9, 640U, 480U);
  zzplay_present_classify(live | ZZ9K_MEDIA_PRESENT_NATIVE, value, &info);
  if (info.dst_x != -16 || info.dst_y != -9) return 1;
  if (info.clipped != 1) return 2;
  if (info.path != ZZPLAY_PATH_NATIVE_SCALED) return 3;

  /* Clipped off the right/bottom edge counts too. */
  build(value, 320U, 240U, 320U, 240U, 400, 300, 640U, 480U);
  zzplay_present_classify(live | ZZ9K_MEDIA_PRESENT_NATIVE, value, &info);
  if (info.clipped != 1 || info.path != ZZPLAY_PATH_NATIVE_SCALED) return 4;

  /* Exactly flush against both far edges is still fully visible. */
  build(value, 320U, 240U, 320U, 240U, 320, 240, 640U, 480U);
  zzplay_present_classify(live | ZZ9K_MEDIA_PRESENT_NATIVE, value, &info);
  if (info.clipped != 0 || info.path != ZZPLAY_PATH_NATIVE_1_1) return 5;
  return 0;
}

static int test_software_fallback(void)
{
  ZZPlayPresentInfo info;
  uint64_t value[4];

  /* Active but not native: the ARM shadow compositor is running. */
  build(value, 320U, 240U, 320U, 240U, 0, 0, 640U, 480U);
  zzplay_present_classify(live, value, &info);
  if (info.path != ZZPLAY_PATH_SOFTWARE) return 1;
  return 0;
}

static int test_inactive(void)
{
  ZZPlayPresentInfo info;
  uint64_t value[4];

  build(value, 320U, 240U, 320U, 240U, 0, 0, 640U, 480U);
  zzplay_present_classify(0U, value, &info);
  if (info.path != ZZPLAY_PATH_INACTIVE) return 1;
  /* Configured but not yet active is still inactive, even if native armed. */
  zzplay_present_classify(
      ZZ9K_MEDIA_PRESENT_CONFIGURED | ZZ9K_MEDIA_PRESENT_NATIVE, value,
      &info);
  if (info.path != ZZPLAY_PATH_INACTIVE) return 2;
  return 0;
}

static int test_change_detection_and_names(void)
{
  ZZPlayPresentInfo a;
  ZZPlayPresentInfo b;
  uint64_t value[4];

  build(value, 320U, 240U, 320U, 240U, 0, 0, 640U, 480U);
  zzplay_present_classify(live | ZZ9K_MEDIA_PRESENT_NATIVE, value, &a);
  build(value, 320U, 240U, 640U, 480U, 0, 0, 640U, 480U);
  zzplay_present_classify(live | ZZ9K_MEDIA_PRESENT_NATIVE, value, &b);
  if (!zzplay_present_changed(&a, &b)) return 1;
  if (zzplay_present_changed(&b, &b)) return 2;

  /* A pure window move at the same size is not a path change and must not
   * spam the user with a report on every drag. */
  build(value, 320U, 240U, 640U, 480U, 4, 4, 640U, 480U);
  zzplay_present_classify(live | ZZ9K_MEDIA_PRESENT_NATIVE, value, &a);
  if (zzplay_present_changed(&b, &a)) return 3;

  if (strcmp(zzplay_present_path_name(ZZPLAY_PATH_NATIVE_1_1),
             "native 1:1") != 0)
    return 4;
  if (strcmp(zzplay_present_path_name(ZZPLAY_PATH_NATIVE_SCALED),
             "native scaled") != 0)
    return 5;
  if (strcmp(zzplay_present_path_name(ZZPLAY_PATH_SOFTWARE),
             "card-local compositor") != 0)
    return 6;
  if (strcmp(zzplay_present_path_name(ZZPLAY_PATH_UNKNOWN),
             "unknown") != 0)
    return 7;
  return 0;
}

int main(void)
{
  int rc;

  rc = test_unknown_when_page_unsupported();
  if (rc != 0) { printf("unknown-page %d\n", rc); return 10 + rc; }
  rc = test_native_one_to_one();
  if (rc != 0) { printf("native-1:1 %d\n", rc); return 30 + rc; }
  rc = test_native_scaled();
  if (rc != 0) { printf("native-scaled %d\n", rc); return 50 + rc; }
  rc = test_clipped_exact_size_is_not_one_to_one();
  if (rc != 0) { printf("clipped %d\n", rc); return 70 + rc; }
  rc = test_software_fallback();
  if (rc != 0) { printf("software %d\n", rc); return 90 + rc; }
  rc = test_inactive();
  if (rc != 0) { printf("inactive %d\n", rc); return 110 + rc; }
  rc = test_change_detection_and_names();
  if (rc != 0) { printf("change %d\n", rc); return 130 + rc; }
  printf("zzplay_path_test: all checks passed\n");
  return 0;
}
