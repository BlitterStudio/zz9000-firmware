/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-path.h"

#include <string.h>

#include "zz9k/abi.h"

static int zzplay_present_is_clipped(const ZZPlayPresentInfo *info)
{
  int32_t right;
  int32_t bottom;

  /* A screen size of zero means firmware had no validated mode snapshot to
   * report; without it there is nothing to clip against, so do not invent a
   * transition. */
  if (info->screen_w == 0U || info->screen_h == 0U) {
    return 0;
  }
  right = (int32_t)info->dst_x + (int32_t)info->dst_w;
  bottom = (int32_t)info->dst_y + (int32_t)info->dst_h;
  if (info->dst_x < 0 || info->dst_y < 0) {
    return 1;
  }
  if (right > (int32_t)info->screen_w || bottom > (int32_t)info->screen_h) {
    return 1;
  }
  return 0;
}

void zzplay_present_classify(uint32_t flags, const uint64_t *value,
                             ZZPlayPresentInfo *out)
{
  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  if (!value) {
    out->path = ZZPLAY_PATH_UNKNOWN;
    return;
  }
  out->src_w = ZZ9K_MEDIA_PAIR_HI(value[0]);
  out->src_h = ZZ9K_MEDIA_PAIR_LO(value[0]);
  out->dst_w = ZZ9K_MEDIA_PAIR_HI(value[1]);
  out->dst_h = ZZ9K_MEDIA_PAIR_LO(value[1]);
  out->dst_x = ZZ9K_MEDIA_PAIR_HI_S(value[2]);
  out->dst_y = ZZ9K_MEDIA_PAIR_LO_S(value[2]);
  out->screen_w = ZZ9K_MEDIA_PAIR_HI(value[3]);
  out->screen_h = ZZ9K_MEDIA_PAIR_LO(value[3]);
  out->owned = (flags & ZZ9K_MEDIA_PRESENT_OWNED) != 0U ? 1 : 0;
  out->clipped = zzplay_present_is_clipped(out);

  if ((flags & ZZ9K_MEDIA_PRESENT_CONFIGURED) == 0U ||
      (flags & ZZ9K_MEDIA_PRESENT_ACTIVE) == 0U) {
    out->path = ZZPLAY_PATH_INACTIVE;
    return;
  }
  if ((flags & ZZ9K_MEDIA_PRESENT_NATIVE) == 0U) {
    out->path = ZZPLAY_PATH_SOFTWARE;
    return;
  }
  /* R10 defines 1:1 as exact size AND fully visible; a clipped exact-size
   * window is still going through the scaler/clipper path. */
  if (out->dst_w == out->src_w && out->dst_h == out->src_h &&
      !out->clipped) {
    out->path = ZZPLAY_PATH_NATIVE_1_1;
    return;
  }
  out->path = ZZPLAY_PATH_NATIVE_SCALED;
}

void zzplay_present_from_status(int status, uint32_t flags,
                                const uint64_t *value,
                                ZZPlayPresentInfo *out)
{
  if (!out) {
    return;
  }
  if (status != ZZ9K_STATUS_OK) {
    memset(out, 0, sizeof(*out));
    out->path = ZZPLAY_PATH_UNKNOWN;
    return;
  }
  zzplay_present_classify(flags, value, out);
}

const char *zzplay_present_path_name(ZZPlayPresentPath path)
{
  switch (path) {
  case ZZPLAY_PATH_INACTIVE:
    return "inactive";
  case ZZPLAY_PATH_NATIVE_1_1:
    return "native 1:1";
  case ZZPLAY_PATH_NATIVE_SCALED:
    return "native scaled";
  case ZZPLAY_PATH_SOFTWARE:
    return "card-local compositor";
  case ZZPLAY_PATH_UNKNOWN:
  default:
    return "unknown";
  }
}

int zzplay_present_changed(const ZZPlayPresentInfo *previous,
                           const ZZPlayPresentInfo *current)
{
  if (!previous || !current) {
    return 0;
  }
  /* Only the path itself and the scale it runs at are worth reporting.
   * Dragging the window changes dst_x/dst_y every frame and must stay
   * silent. */
  if (previous->path != current->path) {
    return 1;
  }
  if (previous->dst_w != current->dst_w ||
      previous->dst_h != current->dst_h) {
    return 1;
  }
  return 0;
}
