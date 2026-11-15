/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_FEED_RETRY_H
#define ZZPLAY_FEED_RETRY_H

#include "zz9k/host.h"

/* A rejected FEED clears its result on the host side. Keep the last accepted
 * result so a retry retains the stream session it must address. */
static inline void zzplay_card_feed_result(int status,
                                           const ZZ9KAudioStreamResult *attempt,
                                           ZZ9KAudioStreamResult *accepted)
{
  if (status == ZZ9K_STATUS_OK && attempt && accepted) {
    *accepted = *attempt;
  }
}

#endif
