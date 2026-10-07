/* AHI preferences parser (ENV:Sys/ahi.prefs) for zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-ahiprefs.h"

#include <string.h>

int zzplay_ahiprefs_parse(const uint8_t *data, size_t length,
                          ZZPlayAHIUnitPref units[ZZPLAY_AHIPREFS_UNITS])
{
  uint32_t form_size;
  size_t limit;
  size_t offset = 12U;
  int count = 0;
  uint32_t u;

  if (!data || !units || length < 12U) {
    return -1;
  }
  if (data[0] != 'F' || data[1] != 'O' || data[2] != 'R' || data[3] != 'M') {
    return -1;
  }
  if (data[8] != 'P' || data[9] != 'R' || data[10] != 'E' || data[11] != 'F') {
    return -1;
  }

  form_size = ((uint32_t)data[4] << 24) |
              ((uint32_t)data[5] << 16) |
              ((uint32_t)data[6] << 8) |
              (uint32_t)data[7];

  limit = length;
  if ((size_t)form_size + 8U < limit && (size_t)form_size + 8U >= 12U) {
    limit = (size_t)form_size + 8U;
  }

  memset(units, 0, ZZPLAY_AHIPREFS_UNITS * sizeof(ZZPlayAHIUnitPref));

  while (offset + 8U <= limit) {
    uint32_t chunk_sz = ((uint32_t)data[offset + 4U] << 24) |
                        ((uint32_t)data[offset + 5U] << 16) |
                        ((uint32_t)data[offset + 6U] << 8) |
                        (uint32_t)data[offset + 7U];
    size_t padded_sz = (chunk_sz + 1U) & ~1U;

    /* Subtract, never add: on m68k size_t is 32 bits, so offset + 8 +
     * chunk_sz wraps for a corrupt size near 2^32. offset + 8 <= limit <=
     * length holds here. */
    if (chunk_sz > length - offset - 8U) {
      /* Malformed or truncated chunk: end scan without reading past length. */
      break;
    }

    if (data[offset] == 'A' && data[offset + 1U] == 'H' &&
        data[offset + 2U] == 'I' && data[offset + 3U] == 'U') {
      if (chunk_sz >= 12U) {
        const uint8_t *cd = data + offset + 8U;
        uint8_t unit = cd[0];
        if (unit < ZZPLAY_AHIPREFS_UNITS) {
          units[unit].present = 1U;
          units[unit].channels = ((uint16_t)cd[2] << 8) | (uint16_t)cd[3];
          units[unit].audio_mode = ((uint32_t)cd[4] << 24) |
                                   ((uint32_t)cd[5] << 16) |
                                   ((uint32_t)cd[6] << 8) |
                                   (uint32_t)cd[7];
          units[unit].frequency = ((uint32_t)cd[8] << 24) |
                                  ((uint32_t)cd[9] << 16) |
                                  ((uint32_t)cd[10] << 8) |
                                  (uint32_t)cd[11];
        }
      }
    }

    /* An odd final chunk may lack its pad byte; it was read above. */
    if (padded_sz > length - offset - 8U) {
      break;
    }
    offset += 8U + padded_sz;
  }

  for (u = 0U; u < ZZPLAY_AHIPREFS_UNITS; u++) {
    if (units[u].present) {
      count++;
    }
  }

  return count;
}
