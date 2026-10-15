/* MPEG decode-request compatibility policy for zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZPLAY_DECODE_POLICY_H
#define ZZPLAY_DECODE_POLICY_H

#include <stdint.h>

#include <zz9k/request.h>

static inline int zzplay_decode_retry_legacy(uint32_t flags, int status,
                                             uint8_t *intra_skip_unsupported,
                                             uint32_t *retry_flags)
{
  if (status != ZZ9K_STATUS_BAD_REQUEST ||
      (flags & ZZ9K_MEDIA_DECODE_SKIP_TO_I_PICTURE) == 0U) {
    return 0;
  }
  *intra_skip_unsupported = 1U;
  *retry_flags = flags & ~ZZ9K_MEDIA_DECODE_SKIP_TO_I_PICTURE;
  return 1;
}

#endif
