/*
 * Finding and judging the Resident (romtag) of the software mpega.library,
 * see mpega_romtag.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mpega_romtag.h"

const struct Resident *mpega_romtag_scan(const uint8_t *code, uint32_t bytes)
{
  uint32_t i;

  for (i = 0U; i + sizeof(struct Resident) <= bytes; i += 2U) {
    const struct Resident *candidate = (const struct Resident *)(code + i);

    if (candidate->rt_MatchWord == RTC_MATCHWORD &&
        candidate->rt_MatchTag == candidate) {
      return candidate;
    }
  }

  return 0;
}

static int mpega_text_has_zz9000(const char *text)
{
  static const char needle[] = "ZZ9000";
  uint32_t i;
  uint32_t j;

  if (!text) {
    return 0;
  }

  for (i = 0U; text[i] != '\0'; i++) {
    for (j = 0U; needle[j] != '\0'; j++) {
      char c = text[i + j];

      if (c == '\0') {
        return 0;
      }
      if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
      }
      if (c != needle[j]) {
        break;
      }
    }
    if (needle[j] == '\0') {
      return 1;
    }
  }

  return 0;
}

int mpega_romtag_is_zz9000(const struct Resident *resident)
{
  return resident &&
         (mpega_text_has_zz9000((const char *)resident->rt_IdString) ||
          mpega_text_has_zz9000((const char *)resident->rt_Name));
}
