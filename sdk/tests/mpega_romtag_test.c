/*
 * Host tests of the romtag search the ZZ9000 mpega.library uses to start the
 * software library (amiga/mpega/mpega_romtag.c, compiled as built for the
 * Amiga).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../amiga/mpega/mpega_romtag.h"

static int failures;

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      printf("%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);  \
      failures++;                                                           \
    }                                                                       \
  } while (0)

typedef union Segment {
  struct Resident align;
  uint8_t bytes[512];
} Segment;

static struct Resident *place(Segment *segment, size_t offset)
{
  struct Resident *resident = (struct Resident *)(segment->bytes + offset);

  memset(resident, 0, sizeof(*resident));
  resident->rt_MatchWord = RTC_MATCHWORD;
  resident->rt_MatchTag = resident;
  return resident;
}

static void test_scan(void)
{
  Segment segment;
  struct Resident *resident;

  memset(&segment, 0, sizeof(segment));
  CHECK(mpega_romtag_scan(segment.bytes, sizeof(segment.bytes)) == 0);

  /* Found at an offset that is not a multiple of the struct's alignment. */
  resident = place(&segment, 64);
  CHECK(mpega_romtag_scan(segment.bytes, sizeof(segment.bytes)) == resident);

  /* The tag word without the self pointer is data that looks like a tag. */
  memset(&segment, 0, sizeof(segment));
  resident = place(&segment, 128);
  segment.bytes[32] = 0x4a;
  segment.bytes[33] = 0xfc;
  CHECK(mpega_romtag_scan(segment.bytes, sizeof(segment.bytes)) == resident);

  /* A tag the segment cuts short is not read. */
  memset(&segment, 0, sizeof(segment));
  resident = place(&segment, 256);
  CHECK(mpega_romtag_scan(segment.bytes, 256 + sizeof(struct Resident) - 2) ==
        0);
  CHECK(mpega_romtag_scan(segment.bytes, 256 + sizeof(struct Resident)) ==
        resident);
  CHECK(mpega_romtag_scan(segment.bytes, 0) == 0);
}

static void test_zz9000_id(void)
{
  struct Resident resident;

  memset(&resident, 0, sizeof(resident));
  CHECK(!mpega_romtag_is_zz9000(0));
  CHECK(!mpega_romtag_is_zz9000(&resident));

  resident.rt_Name = (char *)"mpega.library";
  resident.rt_IdString = (char *)"$VER: mpega.library 2.127 (21.11.2026) ZZ9000 SDK";
  CHECK(mpega_romtag_is_zz9000(&resident));

  resident.rt_IdString = (char *)"$VER: mpega.library 2.2 (13.1.1997) zz9000 copy";
  CHECK(mpega_romtag_is_zz9000(&resident));

  resident.rt_IdString = (char *)"mpega.library 1.0 ZZ900";
  CHECK(!mpega_romtag_is_zz9000(&resident));

  resident.rt_IdString = (char *)"$VER: mpega.library 2.2 (13.1.1997)";
  CHECK(!mpega_romtag_is_zz9000(&resident));

  resident.rt_IdString = 0;
  resident.rt_Name = (char *)"zz9000-mpega.library";
  CHECK(mpega_romtag_is_zz9000(&resident));
}

int main(void)
{
  test_scan();
  test_zz9000_id();

  if (failures != 0) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
