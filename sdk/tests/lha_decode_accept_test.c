/*
 * Unit tests for the LHA decode accept/reject decision.
 *
 * Regression guard for the "crc matches but decode reported as failed" bug:
 * the LH5 decoder can leave a couple of packed bytes unconsumed on a perfectly
 * valid decode, so read_size != packed_size must NOT fail a decode whose CRC-16
 * confirms the output.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k_lha_unix.h"

#include <stdio.h>

static int failures;

static void check(int cond, const char *msg)
{
  if (cond) {
    printf("ok   %s\n", msg);
  } else {
    printf("FAIL %s\n", msg);
    failures++;
  }
}

int main(void)
{
  /* The reported bug: a correct decode (CRC matches) where the decoder left a
   * couple of packed bytes unconsumed. Must be ACCEPTED. */
  check(zz9k_lha_unix_decode_accept(0, 98u, 100u, 1, 0xfba7u, 0xfba7u) == 1,
        "crc match with short read_size is accepted");

  /* CRC present and matched with full consumption: accept. */
  check(zz9k_lha_unix_decode_accept(0, 100u, 100u, 1, 0xfba7u, 0xfba7u) == 1,
        "crc match with full read_size is accepted");

  /* CRC present and mismatched: reject even if every packed byte was read. */
  check(zz9k_lha_unix_decode_accept(0, 100u, 100u, 1, 0x1234u, 0xfba7u) == 0,
        "crc mismatch is rejected");

  /* Decoder flagged an error: reject regardless of a matching CRC. */
  check(zz9k_lha_unix_decode_accept(1, 100u, 100u, 1, 0xfba7u, 0xfba7u) == 0,
        "decoder error is rejected even when crc matches");

  /* No CRC available: read_size must equal packed_size. */
  check(zz9k_lha_unix_decode_accept(0, 100u, 100u, 0, 0u, 0u) == 1,
        "no-crc full read_size is accepted");
  check(zz9k_lha_unix_decode_accept(0, 98u, 100u, 0, 0u, 0u) == 0,
        "no-crc short read_size is rejected");

  /* Bug #39: when no CRC is in play, an "actual" value that happens to equal
   * "expected" (e.g. both zero, or a coincidental match) must NOT be treated
   * as a CRC pass -- the read-size mismatch alone must reject the decode.
   * This pins the accept() semantics that the caller's failure message
   * (zz9k_archive_lha_decode_method_to_file) relies on to avoid printing a
   * misleading "crc matched" style line when CRC checking was never in
   * effect. */
  check(zz9k_lha_unix_decode_accept(0, 10u, 20u, 0, 0x1234u, 0x1234u) == 0,
        "equal crc values with check_crc off and short read is rejected "
        "(#39)");

  if (failures) {
    printf("lha_decode_accept_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("lha_decode_accept_test: all passed\n");
  return 0;
}
