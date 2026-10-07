/* Behavioural tests for the format registry's requester pattern.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <string.h>

#include "zzplay-formats.h"

static int failures;

static void check_pattern(int include_playlists, const char *want)
{
  char pattern[64];

  if (!zzplay_formats_pattern(pattern, sizeof(pattern), include_playlists)) {
    printf("pattern construction failed\n");
    failures++;
  } else if (strcmp(pattern, want) != 0) {
    printf("pattern: got %s, want %s\n", pattern, want);
    failures++;
  }
  if (zzplay_formats_pattern(pattern, strlen(want), include_playlists)) {
    printf("unterminated-capacity pattern unexpectedly succeeded\n");
    failures++;
  }
}

int main(void)
{
  check_pattern(0, "#?.(mpg|mpeg|mp3)");
  check_pattern(1, "#?.(mpg|mpeg|mp3|m3u|m3u8)");
  if (zzplay_formats_pattern(0, 0U, 0)) {
    printf("null output unexpectedly succeeded\n");
    failures++;
  }
  if (failures != 0) {
    printf("zzplay_formats_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("zzplay_formats_test: all checks passed\n");
  return 0;
}
