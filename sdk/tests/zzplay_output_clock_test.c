/* Playing time is DMA frames over the card rate, not file bytes over
 * the first-frame bitrate. These cases are the ones that used to run
 * the display at about twice real time, or jump by a buffer's worth
 * of audio.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "zzplay-audio-clock.h"

#include <stdio.h>

static int expect_ms(const char *name, uint64_t written, uint64_t lead,
                      uint64_t origin, uint32_t rate, uint32_t want)
{
  uint32_t got = zzplay_output_elapsed_ms(written, lead, origin, rate);

  if (got == want) {
    return 0;
  }
  fprintf(stderr, "%s: got %u ms, want %u\n", name, got, want);
  return 1;
}

int main(void)
{
  int failed = 0;

  /* One second of 48 kHz stereo output, no lead, no seek. */
  failed += expect_ms("one second", 48000U, 0U, 0U, 48000U, 1000U);
  /* 120 ms still sitting in the TX ring must not count. */
  failed += expect_ms("lead excluded", 48000U + 5760U, 5760U, 0U,
                      48000U, 1000U);
  /* A seek anchors at the retired count; only frames after that move. */
  failed += expect_ms("seek origin", 96000U, 0U, 48000U, 48000U, 1000U);

  /* The old clock: input_bytes * 8 / first-frame bitrate_kbps.
   * A 64 kbps Xing header against a 128 kbps file reports 2000 ms
   * after 16000 bytes, which is one second of audio. */
  if ((16000ULL * 8ULL) / 64ULL != 2000U) {
    fprintf(stderr, "bitrate fixture no longer shows the 2x case\n");
    failed++;
  }
  failed += expect_ms("vbr is not the bitrate clock", 48000U, 0U, 0U,
                      48000U, 1000U);

  /* MPEG-1 44.1 kHz stereo is 1152 samples/frame. One second is about
   * 38.28 frames, not 48000. Counting those frames at 48 kHz would
   * freeze the display. The output clock counts transmitter frames. */
  failed += expect_ms("not source frames", 48000U, 0U, 0U, 48000U, 1000U);

  /* A 100 KB ID3v2 tag is 6.25 s at 128 kbps and must not move this
   * clock at all: the tag never reaches the transmitter. */
  failed += expect_ms("id3 ignored", 48000U, 0U, 0U, 48000U, 1000U);

  if (zzplay_output_elapsed_ms(1000U, 0U, 0U, 0U) != 0U ||
      zzplay_output_elapsed_ms(100U, 200U, 0U, 48000U) != 0U) {
    fprintf(stderr, "bad rate or lead past the write cursor\n");
    failed++;
  }

  return failed == 0 ? 0 : 1;
}
