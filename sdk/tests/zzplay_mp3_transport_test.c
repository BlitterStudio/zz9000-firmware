/* Behavioural tests for the host-testable MP3 transport calculations the
 * desktop player added: the CBR duration estimate over the audio payload
 * and the proportional, clamped seek offset.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-mp3-transport.h"

#include <stdio.h>

static int failures;

static void check_u32(const char *what, uint32_t got, uint32_t want)
{
  if (got != want) {
    printf("%s: got %lu, want %lu\n", what, (unsigned long)got,
           (unsigned long)want);
    failures++;
  }
}

static void check_u64(const char *what, uint64_t got, uint64_t want)
{
  if (got != want) {
    printf("%s: got %llu, want %llu\n", what,
           (unsigned long long)got, (unsigned long long)want);
    failures++;
  }
}

static void test_duration(void)
{
  /* Unknown bitrate means unknown duration, never a division by zero. */
  check_u32("duration no bitrate", zzplay_mp3_duration_ms(5000000U, 0U),
            0U);
  /* bytes * 8 / kbps is milliseconds directly: a 128 kbps CBR stream of
   * 128000 audio bytes lasts exactly 8 s. */
  check_u32("duration 128k", zzplay_mp3_duration_ms(128000U, 128U),
            8000U);
  check_u32("duration 1s", zzplay_mp3_duration_ms(32000U, 256U), 1000U);
  /* An empty payload is a zero duration, not a stray tick. */
  check_u32("duration empty", zzplay_mp3_duration_ms(0U, 128U), 0U);
  /* Implausibly large inputs saturate instead of wrapping the cast. */
  check_u32("duration saturates",
            zzplay_mp3_duration_ms(0xffffffffULL, 1U), 0xffffffffU);
}

static void test_seek_offset(void)
{
  /* A 10 s, 20000-byte audio range starting after a 2000-byte ID3v2 tag. */
  const uint64_t start = 2000U;
  const uint64_t end = 22000U;
  const uint64_t total = 10000U;

  /* Target 0 restarts the item. */
  check_u64("seek start", zzplay_mp3_seek_offset(start, end, total, 0U),
            start);
  /* A target at or past the end clamps to the last audio byte. */
  check_u64("seek end", zzplay_mp3_seek_offset(start, end, total, total),
            end);
  check_u64("seek past end",
            zzplay_mp3_seek_offset(start, end, total, total + 60000U),
            end);
  /* Halfway in time is halfway in bytes (plus the tag offset). */
  check_u64("seek half",
            zzplay_mp3_seek_offset(start, end, total, total / 2U),
            12000U);
  /* A quarter in, with truncation: 2000 + 5000/4 exact here. */
  check_u64("seek quarter",
            zzplay_mp3_seek_offset(start, end, total, 2500U), 7000U);
  /* Unknown duration cannot be mapped: restart instead of guessing. */
  check_u64("seek no duration",
            zzplay_mp3_seek_offset(start, end, 0U, 4000U), start);
  /* Degenerate range (tag-only file, or trailer swallowing everything):
   * stay at the start of the range. */
  check_u64("seek empty range",
            zzplay_mp3_seek_offset(start, start, total, 4000U), start);
  check_u64("seek inverted range",
            zzplay_mp3_seek_offset(start, start - 1U, total, 4000U),
            start);
  /* Rounding stays inside the range for awkward fractions. */
  check_u64("seek rounding",
            zzplay_mp3_seek_offset(0U, 3U, 10000U, 5000U), 1U);
  check_u64("seek rounding high",
            zzplay_mp3_seek_offset(0U, 3U, 10000U, 9999U), 2U);
}

int main(void)
{
  test_duration();
  test_seek_offset();
  if (failures != 0) {
    printf("zzplay_mp3_transport_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("zzplay_mp3_transport_test: all checks passed\n");
  return 0;
}
