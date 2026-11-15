/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <string.h>

#include "zzplay-feed-retry.h"

static int failures;

#define EXPECT(condition, description) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL: %s\n", description); \
    failures++; \
  } \
} while (0)

int main(void)
{
  ZZ9KAudioStreamResult accepted;
  ZZ9KAudioStreamResult attempt;

  memset(&accepted, 0, sizeof(accepted));
  accepted.session = 0x11223344U;
  accepted.bytes_produced = 77U;
  memset(&attempt, 0, sizeof(attempt));
  zzplay_card_feed_result(ZZ9K_STATUS_BUSY, &attempt, &accepted);
  EXPECT(accepted.session == 0x11223344U,
         "busy FEED preserves the accepted session");
  EXPECT(accepted.bytes_produced == 77U,
         "busy FEED preserves accepted progress");

  attempt.session = 0x55667788U;
  attempt.bytes_produced = 99U;
  zzplay_card_feed_result(ZZ9K_STATUS_OK, &attempt, &accepted);
  EXPECT(accepted.session == attempt.session,
         "accepted FEED advances the session result");
  EXPECT(accepted.bytes_produced == attempt.bytes_produced,
         "accepted FEED advances progress");

  if (failures) {
    fprintf(stderr, "zzplay_feed_retry_test: %d failure(s)\n", failures);
    return 1;
  }
  puts("zzplay_feed_retry_test: all passed");
  return 0;
}
