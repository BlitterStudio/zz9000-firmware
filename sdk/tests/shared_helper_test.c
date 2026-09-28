/*
 * Header-only checks for shared-buffer access helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/shared.h"

#include <stdint.h>
#include <string.h>

static int test_shared_range_validation_accepts_bounded_ranges(void)
{
  uint8_t bytes[16];
  ZZ9KSharedBuffer buffer;

  memset(&buffer, 0, sizeof(buffer));
  buffer.handle = 0x40000010UL;
  buffer.data = bytes;
  buffer.length = sizeof(bytes);

  if (!zz9k_shared_range_fits(&buffer, 0U, 16U)) return 1;
  if (!zz9k_shared_range_fits(&buffer, 16U, 0U)) return 2;
  if (zz9k_shared_range_fits(&buffer, 15U, 2U)) return 3;
  if (zz9k_shared_range_fits(&buffer, 17U, 0U)) return 4;

  buffer.handle = ZZ9K_INVALID_HANDLE;
  if (zz9k_shared_range_fits(&buffer, 0U, 1U)) return 5;

  buffer.handle = 0x40000010UL;
  buffer.data = 0;
  if (zz9k_shared_range_fits(&buffer, 0U, 1U)) return 6;

  return 0;
}

static int test_shared_copy_to_and_from_respect_offsets(void)
{
  uint8_t bytes[16];
  uint8_t input[4] = { 0x10U, 0x20U, 0x30U, 0x40U };
  uint8_t output[4];
  ZZ9KSharedBuffer buffer;

  memset(bytes, 0, sizeof(bytes));
  memset(output, 0, sizeof(output));
  memset(&buffer, 0, sizeof(buffer));
  buffer.handle = 0x40000020UL;
  buffer.data = bytes;
  buffer.length = sizeof(bytes);

  if (!zz9k_shared_copy_to(&buffer, 5U, input, sizeof(input))) return 1;
  if (bytes[4] != 0U || bytes[5] != 0x10U || bytes[8] != 0x40U ||
      bytes[9] != 0U) {
    return 2;
  }
  if (!zz9k_shared_copy_from(output, &buffer, 5U, sizeof(output))) return 3;
  if (memcmp(output, input, sizeof(input)) != 0) return 4;

  if (zz9k_shared_copy_to(&buffer, 14U, input, sizeof(input))) return 5;
  if (zz9k_shared_copy_from(output, &buffer, 14U, sizeof(output))) return 6;
  if (zz9k_shared_copy_to(&buffer, 0U, 0, sizeof(input))) return 7;
  if (zz9k_shared_copy_from(0, &buffer, 0U, sizeof(output))) return 8;

  return 0;
}

static int test_shared_set_writes_volatile_bytes(void)
{
  uint8_t bytes[8];
  ZZ9KSharedBuffer buffer;

  memset(bytes, 0, sizeof(bytes));
  memset(&buffer, 0, sizeof(buffer));
  buffer.handle = 0x40000030UL;
  buffer.data = bytes;
  buffer.length = sizeof(bytes);

  if (!zz9k_shared_set(&buffer, 2U, 0xa5U, 4U)) return 1;
  if (bytes[1] != 0U || bytes[2] != 0xa5U || bytes[5] != 0xa5U ||
      bytes[6] != 0U) {
    return 2;
  }
  if (zz9k_shared_set(&buffer, 7U, 0x55U, 2U)) return 3;

  return 0;
}

static int test_shared_move_handles_overlapping_ranges(void)
{
  uint8_t bytes[10];
  ZZ9KSharedBuffer buffer;
  uint32_t i;

  for (i = 0U; i < sizeof(bytes); i++) {
    bytes[i] = (uint8_t)i;
  }
  memset(&buffer, 0, sizeof(buffer));
  buffer.handle = 0x40000040UL;
  buffer.data = bytes;
  buffer.length = sizeof(bytes);

  if (!zz9k_shared_move(&buffer, 0U, 2U, 6U)) return 1;
  if (bytes[0] != 2U || bytes[1] != 3U || bytes[5] != 7U ||
      bytes[6] != 6U) {
    return 2;
  }

  for (i = 0U; i < sizeof(bytes); i++) {
    bytes[i] = (uint8_t)i;
  }
  if (!zz9k_shared_move(&buffer, 3U, 0U, 6U)) return 3;
  if (bytes[3] != 0U || bytes[4] != 1U || bytes[8] != 5U ||
      bytes[2] != 2U) {
    return 4;
  }

  if (zz9k_shared_move(&buffer, 8U, 0U, 4U)) return 5;
  if (zz9k_shared_move(&buffer, 0U, 8U, 4U)) return 6;

  return 0;
}

int main(void)
{
  int result;

  result = test_shared_range_validation_accepts_bounded_ranges();
  if (result) return 10 + result;

  result = test_shared_copy_to_and_from_respect_offsets();
  if (result) return 30 + result;

  result = test_shared_set_writes_volatile_bytes();
  if (result) return 50 + result;

  result = test_shared_move_handles_overlapping_ranges();
  if (result) return 70 + result;

  return 0;
}
