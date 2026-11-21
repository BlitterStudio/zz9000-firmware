/*
 * Finding and judging the Resident (romtag) of the software mpega.library
 * the ZZ9000 library falls back to. No AmigaOS calls, so the host tests
 * compile this file as it is.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MPEGA_ROMTAG_H
#define MPEGA_ROMTAG_H

#include <stdint.h>

#include <exec/resident.h>

/*
 * The first struct Resident in the code bytes: a word RTC_MATCHWORD whose
 * rt_MatchTag points at the word itself. `code` is one loaded segment's
 * contents (after its next-segment pointer). Returns NULL if there is none.
 */
const struct Resident *mpega_romtag_scan(const uint8_t *code, uint32_t bytes);

/* Whether the module's name or id string says ZZ9000, that is whether it is
 * the ZZ9000 mpega.library (or a copy of it) and not a software decoder. The
 * search ignores case. */
int mpega_romtag_is_zz9000(const struct Resident *resident);

#endif /* MPEGA_ROMTAG_H */
