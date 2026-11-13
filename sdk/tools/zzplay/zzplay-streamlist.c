/* Stream playlists (.pls and .m3u) fetched from a URL.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-streamlist.h"

#include <string.h>

static int lower(int c)
{
  return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int ends_with_nocase(const char *text, size_t len, const char *suffix)
{
  size_t n = strlen(suffix);
  size_t i;

  if (len < n) {
    return 0;
  }
  for (i = 0U; i < n; i++) {
    if (lower((unsigned char)text[len - n + i]) != suffix[i]) {
      return 0;
    }
  }
  return 1;
}

int zzplay_streamlist_is_playlist(const char *url)
{
  size_t len = 0U;

  if (!zzplay_http_is_url(url)) {
    return 0;
  }
  while (url[len] != '\0' && url[len] != '?' && url[len] != '#') {
    len++;
  }
  return ends_with_nocase(url, len, ".pls") ||
         ends_with_nocase(url, len, ".m3u") ||
         ends_with_nocase(url, len, ".m3u8");
}

/* Store `entry` (trimmed, not NUL-terminated) in `slot`, resolved against
 * the playlist's own address. Returns 1 when it is a usable http(s) URL. */
static int resolve_entry(const ZZPlayUrl *base, int have_base,
                         const char *entry, size_t len, char *slot)
{
  char text[ZZPLAY_HTTP_URL_MAX];

  if (len == 0U || len >= sizeof(text)) {
    return 0;
  }
  memcpy(text, entry, len);
  text[len] = '\0';
  if (zzplay_http_is_url(text)) {
    memcpy(slot, text, len + 1U);
    return 1;
  }
  {
    /* "Work:a.mp3" or "ftp://..." is not a path on this server. */
    const char *colon = strchr(text, ':');
    const char *slash = strchr(text, '/');

    if (colon != NULL && (slash == NULL || colon < slash)) {
      return 0;
    }
  }
  return have_base &&
         zzplay_http_resolve(base, text, slot, ZZPLAY_HTTP_URL_MAX);
}

static void trim(const char **text, size_t *len)
{
  while (*len != 0U && (**text == ' ' || **text == '\t')) {
    (*text)++;
    (*len)--;
  }
  while (*len != 0U &&
         ((*text)[*len - 1U] == ' ' || (*text)[*len - 1U] == '\t')) {
    (*len)--;
  }
}

/* "FileN=" with N in 1..99: returns N and the value, or 0. */
static uint32_t pls_file_entry(const char *line, size_t len,
                               const char **value, size_t *value_len)
{
  uint32_t n = 0U;
  size_t i = 4U;

  if (len < 6U || lower((unsigned char)line[0]) != 'f' ||
      lower((unsigned char)line[1]) != 'i' ||
      lower((unsigned char)line[2]) != 'l' ||
      lower((unsigned char)line[3]) != 'e') {
    return 0U;
  }
  while (i < len && i < 6U && line[i] >= '0' && line[i] <= '9') {
    n = n * 10U + (uint32_t)(line[i] - '0');
    i++;
  }
  if (i == 4U || i >= len || line[i] != '=' || n == 0U) {
    return 0U;
  }
  *value = line + i + 1U;
  *value_len = len - i - 1U;
  return n;
}

uint32_t zzplay_streamlist_parse(const char *text, size_t len,
                                 const char *base_url, ZZPlayStreamList *out)
{
  ZZPlayUrl base;
  int have_base;
  int pls;
  size_t pos = 0U;
  /* PLS entries land by number, then close ranks. */
  uint8_t used[ZZPLAY_STREAMLIST_MAX];

  if (out == NULL) {
    return 0U;
  }
  memset(out, 0, sizeof(*out));
  memset(used, 0, sizeof(used));
  if (text == NULL) {
    return 0U;
  }
  have_base = base_url != NULL && zzplay_http_parse_url(base_url, &base);
  if (len >= 3U && (unsigned char)text[0] == 0xEFU &&
      (unsigned char)text[1] == 0xBBU && (unsigned char)text[2] == 0xBFU) {
    pos = 3U;
  }
  {
    size_t p = pos;

    while (p < len && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' ||
                       text[p] == '\n')) {
      p++;
    }
    pls = len - p >= 10U && lower((unsigned char)text[p]) == '[' &&
          ends_with_nocase(text + p, 10U, "[playlist]");
  }

  while (pos < len) {
    size_t end = pos;
    const char *line = text + pos;
    size_t line_len;

    while (end < len && text[end] != '\n' && text[end] != '\r') {
      end++;
    }
    line_len = end - pos;
    trim(&line, &line_len);
    if (pls) {
      const char *value;
      size_t value_len;
      uint32_t n = pls_file_entry(line, line_len, &value, &value_len);

      if (n != 0U && n <= ZZPLAY_STREAMLIST_MAX && !used[n - 1U]) {
        trim(&value, &value_len);
        if (resolve_entry(&base, have_base, value, value_len,
                          out->url[n - 1U])) {
          used[n - 1U] = 1U;
        }
      }
    } else if (line_len != 0U && line[0] != '#' &&
               out->count < ZZPLAY_STREAMLIST_MAX) {
      if (resolve_entry(&base, have_base, line, line_len,
                        out->url[out->count])) {
        out->count++;
      }
    }
    pos = end + 1U;
  }

  if (pls) {
    uint32_t i;

    for (i = 0U; i < ZZPLAY_STREAMLIST_MAX; i++) {
      if (used[i]) {
        if (out->count != i) {
          memcpy(out->url[out->count], out->url[i], ZZPLAY_HTTP_URL_MAX);
        }
        out->count++;
      }
    }
  }
  return out->count;
}
