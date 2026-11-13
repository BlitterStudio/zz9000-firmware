/* Stream playlists (.pls and .m3u) fetched from a URL: the addresses a
 * radio station publishes for its stream, in order of preference.
 *
 * Host-tested. SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_STREAMLIST_H
#define ZZPLAY_STREAMLIST_H

#include <stddef.h>
#include <stdint.h>

#include "zzplay-http.h"

#define ZZPLAY_STREAMLIST_MAX 8U
#define ZZPLAY_STREAMLIST_TEXT_MAX (64U * 1024U)

typedef struct ZZPlayStreamList {
  uint32_t count;
  char url[ZZPLAY_STREAMLIST_MAX][ZZPLAY_HTTP_URL_MAX];
} ZZPlayStreamList;

/* True for an http(s) URL whose path ends in .pls, .m3u or .m3u8 (any
 * case, ignoring a query string). */
int zzplay_streamlist_is_playlist(const char *url);

/* Parse a downloaded playlist: "[playlist]" text as PLS (FileN= entries in
 * N order), anything else as M3U (non-comment lines). Relative entries are
 * resolved against `base_url`; only http(s) entries are kept, at most
 * ZZPLAY_STREAMLIST_MAX. Returns the number of addresses. */
uint32_t zzplay_streamlist_parse(const char *text, size_t len,
                                 const char *base_url, ZZPlayStreamList *out);

#endif /* ZZPLAY_STREAMLIST_H */
