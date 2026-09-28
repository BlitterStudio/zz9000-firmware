/*
 * Unit checks for the real-card SDK smoke-test helper logic.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define ZZ9K_SMOKE_NO_MAIN
#include "../tools/zz9k-smoke.c"

#include <stdint.h>
#include <string.h>

int main(void)
{
  ZZ9KSurface src;
  ZZ9KSurface dst;
  ZZ9KScaleImageDesc desc;

  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));
  memset(&desc, 0xff, sizeof(desc));

  src.handle = 0x40000011UL;
  src.width = 64U;
  src.height = 48U;
  dst.handle = 0x40000022UL;
  dst.width = 128U;
  dst.height = 96U;

  if (!zz9k_smoke_build_scale_desc(&desc, &src, &dst,
                                   ZZ9K_SCALE_NEAREST)) {
    return 1;
  }
  if (desc.src_surface != 0x40000011UL) return 2;
  if (desc.dst_surface != 0x40000022UL) return 3;
  if (desc.src_x != 0U || desc.src_y != 0U) return 4;
  if (desc.src_w != 64U || desc.src_h != 48U) return 5;
  if (desc.dst_x != 0U || desc.dst_y != 0U) return 6;
  if (desc.dst_w != 128U || desc.dst_h != 96U) return 7;
  if (desc.filter != ZZ9K_SCALE_NEAREST) return 8;
  if (desc.flags != 0U) return 9;

  src.handle = ZZ9K_INVALID_HANDLE;
  if (zz9k_smoke_build_scale_desc(&desc, &src, &dst,
                                  ZZ9K_SCALE_NEAREST)) {
    return 10;
  }
  src.handle = 0x40000011UL;
  dst.width = 0U;
  if (zz9k_smoke_build_scale_desc(&desc, &src, &dst,
                                  ZZ9K_SCALE_NEAREST)) {
    return 11;
  }

  return 0;
}
