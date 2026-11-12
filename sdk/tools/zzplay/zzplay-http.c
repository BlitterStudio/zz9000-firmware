/* HTTP/1.1 and Shoutcast protocol parsing for ZZPlay's network sources.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-http.h"

#include <stdio.h>
#include <string.h>

static int lower(int c)
{
  return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int prefix_nocase(const char *text, const char *prefix)
{
  size_t i;

  for (i = 0U; prefix[i] != '\0'; i++) {
    if (lower((unsigned char)text[i]) != prefix[i]) {
      return 0;
    }
  }
  return 1;
}

int zzplay_http_is_url(const char *text)
{
  return text != NULL &&
         (prefix_nocase(text, "http://") || prefix_nocase(text, "https://"));
}

static int host_char(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
}

/* Append path bytes, percent-encoding spaces and non-ASCII the way
 * browsers do. Control characters are refused. */
static int copy_path(char *out, size_t size, const char *in, size_t len)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t used = strlen(out);
  size_t i;

  for (i = 0U; i < len; i++) {
    unsigned char c = (unsigned char)in[i];

    if (c < 0x20U || c == 0x7fU) {
      return 0;
    }
    if (c == ' ' || c >= 0x80U) {
      if (size - used <= 3U) {
        return 0;
      }
      out[used++] = '%';
      out[used++] = hex[c >> 4];
      out[used++] = hex[c & 15U];
    } else {
      if (size - used <= 1U) {
        return 0;
      }
      out[used++] = (char)c;
    }
  }
  out[used] = '\0';
  return 1;
}

int zzplay_http_parse_url(const char *url, ZZPlayUrl *out)
{
  const char *p;
  const char *end;
  const char *colon = NULL;
  size_t host_len;
  size_t path_len;

  if (url == NULL || out == NULL) {
    return 0;
  }
  memset(out, 0, sizeof(*out));
  if (prefix_nocase(url, "https://")) {
    out->tls = 1;
    out->port = 443U;
    p = url + 8;
  } else if (prefix_nocase(url, "http://")) {
    out->port = 80U;
    p = url + 7;
  } else {
    return 0;
  }

  end = p;
  while (*end != '\0' && *end != '/' && *end != '?' && *end != '#') {
    if (*end == '@' || *end == '[') {
      return 0;
    }
    if (*end == ':') {
      colon = end;
    }
    end++;
  }
  host_len = (size_t)((colon != NULL ? colon : end) - p);
  if (host_len == 0U || host_len >= sizeof(out->host)) {
    return 0;
  }
  {
    size_t i;
    for (i = 0U; i < host_len; i++) {
      if (!host_char(p[i])) {
        return 0;
      }
    }
  }
  memcpy(out->host, p, host_len);
  out->host[host_len] = '\0';

  if (colon != NULL) {
    const char *d = colon + 1;
    uint32_t port = 0U;

    if (d == end) {
      return 0;
    }
    while (d < end) {
      if (*d < '0' || *d > '9') {
        return 0;
      }
      port = port * 10U + (uint32_t)(*d - '0');
      if (port > 65535U) {
        return 0;
      }
      d++;
    }
    if (port == 0U) {
      return 0;
    }
    out->port = (uint16_t)port;
  }

  path_len = 0U;
  while (end[path_len] != '\0' && end[path_len] != '#') {
    path_len++;
  }
  out->path[0] = '\0';
  if (path_len == 0U || end[0] != '/') {
    out->path[0] = '/';
    out->path[1] = '\0';
  }
  return copy_path(out->path, sizeof(out->path), end, path_len);
}

/* "host" or "host:port" when the port is not the scheme's default. */
static int format_authority(const ZZPlayUrl *url, char *out, size_t size)
{
  int n;

  if ((url->tls && url->port == 443U) || (!url->tls && url->port == 80U)) {
    n = snprintf(out, size, "%s", url->host);
  } else {
    n = snprintf(out, size, "%s:%u", url->host, (unsigned)url->port);
  }
  return n > 0 && (size_t)n < size;
}

int zzplay_http_resolve(const ZZPlayUrl *base, const char *location,
                        char *out, size_t size)
{
  char authority[ZZPLAY_HTTP_HOST_MAX + 8U];
  const char *scheme;
  int n;

  if (base == NULL || location == NULL || out == NULL || size == 0U ||
      location[0] == '\0') {
    return 0;
  }
  if (zzplay_http_is_url(location)) {
    n = snprintf(out, size, "%s", location);
    return n > 0 && (size_t)n < size;
  }
  scheme = base->tls ? "https:" : "http:";
  if (location[0] == '/' && location[1] == '/') {
    n = snprintf(out, size, "%s%s", scheme, location);
    return n > 0 && (size_t)n < size;
  }
  if (!format_authority(base, authority, sizeof(authority))) {
    return 0;
  }
  if (location[0] == '/') {
    n = snprintf(out, size, "%s//%s%s", scheme, authority, location);
  } else {
    /* Relative: replace the last segment of the base path (before any
     * query string). */
    size_t dir = 0U;
    size_t i;

    for (i = 0U; base->path[i] != '\0' && base->path[i] != '?'; i++) {
      if (base->path[i] == '/') {
        dir = i + 1U;
      }
    }
    n = snprintf(out, size, "%s//%s%.*s%s", scheme, authority, (int)dir,
                 base->path, location);
  }
  return n > 0 && (size_t)n < size;
}

int zzplay_http_redirect_allowed(const ZZPlayUrl *from, const ZZPlayUrl *to)
{
  return from != NULL && to != NULL && (to->tls || !from->tls);
}

int zzplay_http_same_origin(const ZZPlayUrl *a, const ZZPlayUrl *b)
{
  size_t i;

  if (a == NULL || b == NULL || a->tls != b->tls || a->port != b->port) {
    return 0;
  }
  for (i = 0U; a->host[i] != '\0' || b->host[i] != '\0'; i++) {
    if (lower((unsigned char)a->host[i]) != lower((unsigned char)b->host[i])) {
      return 0;
    }
  }
  return 1;
}

size_t zzplay_http_format_request(char *out, size_t size, const char *method,
                                  const ZZPlayUrl *url, const char *user_agent,
                                  int has_range, uint32_t range_from, int icy,
                                  size_t body_len, const char *extra)
{
  char authority[ZZPLAY_HTTP_HOST_MAX + 8U];
  char range[48];
  char clen[48];
  const char *m;
  int is_body_method;
  int n;

  if (out == NULL || size == 0U || url == NULL ||
      !format_authority(url, authority, sizeof(authority))) {
    return 0U;
  }
  m = method != NULL ? method : "GET";
  is_body_method = (strcmp(m, "GET") != 0 && strcmp(m, "HEAD") != 0) ||
                   body_len > 0U;
  range[0] = '\0';
  if (has_range) {
    (void)snprintf(range, sizeof(range), "Range: bytes=%lu-\r\n",
                   (unsigned long)range_from);
  }
  clen[0] = '\0';
  if (is_body_method) {
    (void)snprintf(clen, sizeof(clen), "Content-Length: %lu\r\n",
                   (unsigned long)body_len);
  }
  n = snprintf(out, size,
               "%s %s HTTP/1.1\r\n"
               "Host: %s\r\n"
               "User-Agent: %s\r\n"
               "Accept: */*\r\n"
               "%s%s%s%s"
               "Connection: close\r\n\r\n",
               m, url->path, authority,
               user_agent != NULL ? user_agent : "ZZPlay (AmigaOS)", range,
               clen, icy ? "Icy-MetaData: 1\r\n" : "",
               extra != NULL ? extra : "");
  if (n <= 0 || (size_t)n >= size) {
    return 0U;
  }
  return (size_t)n;
}

/* Case-insensitive match of header `name` (lower case) at a line start. */
static int header_is(const char *line, size_t len, const char *name)
{
  size_t n = strlen(name);
  size_t i;

  if (len <= n || line[n] != ':') {
    return 0;
  }
  for (i = 0U; i < n; i++) {
    if (lower((unsigned char)line[i]) != name[i]) {
      return 0;
    }
  }
  return 1;
}

/* The header value without name, colon and surrounding blanks. */
static void header_value(const char *line, size_t len, const char *name,
                         const char **value, size_t *value_len)
{
  size_t start = strlen(name) + 1U;

  while (start < len && (line[start] == ' ' || line[start] == '\t')) {
    start++;
  }
  while (len > start && (line[len - 1U] == ' ' || line[len - 1U] == '\t')) {
    len--;
  }
  *value = line + start;
  *value_len = len - start;
}

/* Parse decimal digits into a 32-bit value. Returns the number of digits
 * read, or 0 when there are none or the value overflows. */
static size_t parse_u32(const char *text, size_t len, uint32_t *out)
{
  uint32_t value = 0U;
  size_t i = 0U;

  while (i < len && text[i] >= '0' && text[i] <= '9') {
    uint32_t digit = (uint32_t)(text[i] - '0');

    if (value > (0xFFFFFFFFUL - digit) / 10U) {
      return 0U;
    }
    value = value * 10U + digit;
    i++;
  }
  if (i != 0U) {
    *out = value;
  }
  return i;
}

static int equals_nocase(const char *text, size_t len, const char *word)
{
  size_t i;

  if (strlen(word) != len) {
    return 0;
  }
  for (i = 0U; i < len; i++) {
    if (lower((unsigned char)text[i]) != word[i]) {
      return 0;
    }
  }
  return 1;
}

static void copy_value(char *out, size_t size, const char *v, size_t vl)
{
  if (vl >= size) {
    vl = size - 1U;
  }
  memcpy(out, v, vl);
  out[vl] = '\0';
}

static int parse_status_line(const char *line, size_t len, ZZPlayHttpHead *out)
{
  size_t code_at;

  if (len >= 12U && memcmp(line, "HTTP/1.", 7) == 0 && line[8] == ' ') {
    code_at = 9U;
  } else if (len >= 7U && memcmp(line, "ICY ", 4) == 0) {
    code_at = 4U;
    out->icy = 1;
  } else {
    return 0;
  }
  if (line[code_at] < '1' || line[code_at] > '5' ||
      line[code_at + 1U] < '0' || line[code_at + 1U] > '9' ||
      line[code_at + 2U] < '0' || line[code_at + 2U] > '9') {
    return 0;
  }
  out->status = (line[code_at] - '0') * 100 + (line[code_at + 1U] - '0') * 10 +
                (line[code_at + 2U] - '0');
  return 1;
}

/* "bytes START-END/TOTAL" or "bytes START-END/ *". */
static int parse_content_range(const char *v, size_t vl, ZZPlayHttpHead *out)
{
  size_t pos;
  size_t n;
  uint32_t start;
  uint32_t last;

  if (vl < 6U || !equals_nocase(v, 6U, "bytes ")) {
    return 0;
  }
  pos = 6U;
  n = parse_u32(v + pos, vl - pos, &start);
  if (n == 0U) {
    return 0;
  }
  pos += n;
  if (pos >= vl || v[pos] != '-') {
    return 0;
  }
  pos++;
  n = parse_u32(v + pos, vl - pos, &last);
  if (n == 0U || last < start) {
    return 0;
  }
  pos += n;
  if (pos >= vl || v[pos] != '/') {
    return 0;
  }
  pos++;
  out->has_range = 1;
  out->range_start = start;
  if (pos < vl && v[pos] == '*') {
    return pos + 1U == vl;
  }
  n = parse_u32(v + pos, vl - pos, &out->total);
  if (n == 0U || pos + n != vl || out->total <= last) {
    return 0;
  }
  out->has_total = 1;
  return 1;
}

int zzplay_http_parse_head(const char *head, size_t len, ZZPlayHttpHead *out)
{
  size_t pos = 0U;
  int first = 1;

  if (head == NULL || out == NULL) {
    return 0;
  }
  memset(out, 0, sizeof(*out));

  while (pos < len) {
    size_t end = pos;
    size_t line_len;
    const char *line = head + pos;
    const char *v;
    size_t vl;

    while (end < len && head[end] != '\n') {
      end++;
    }
    line_len = end - pos;
    if (line_len != 0U && line[line_len - 1U] == '\r') {
      line_len--;
    }
    if (first) {
      if (!parse_status_line(line, line_len, out)) {
        return 0;
      }
      first = 0;
    } else if (header_is(line, line_len, "content-length")) {
      header_value(line, line_len, "content-length", &v, &vl);
      if (vl == 0U || parse_u32(v, vl, &out->content_length) != vl) {
        return 0;
      }
      out->has_length = 1;
    } else if (header_is(line, line_len, "transfer-encoding")) {
      header_value(line, line_len, "transfer-encoding", &v, &vl);
      if (equals_nocase(v, vl, "chunked")) {
        out->chunked = 1;
      } else if (!equals_nocase(v, vl, "identity")) {
        return 0;
      }
    } else if (header_is(line, line_len, "accept-ranges")) {
      header_value(line, line_len, "accept-ranges", &v, &vl);
      out->accept_ranges = equals_nocase(v, vl, "bytes");
    } else if (header_is(line, line_len, "content-range")) {
      header_value(line, line_len, "content-range", &v, &vl);
      if (!parse_content_range(v, vl, out)) {
        return 0;
      }
    } else if (header_is(line, line_len, "icy-metaint")) {
      header_value(line, line_len, "icy-metaint", &v, &vl);
      if (vl == 0U || parse_u32(v, vl, &out->icy_metaint) != vl) {
        return 0;
      }
    } else if (header_is(line, line_len, "icy-name")) {
      header_value(line, line_len, "icy-name", &v, &vl);
      copy_value(out->icy_name, sizeof(out->icy_name), v, vl);
    } else if (header_is(line, line_len, "content-type")) {
      header_value(line, line_len, "content-type", &v, &vl);
      copy_value(out->content_type, sizeof(out->content_type), v, vl);
    } else if (header_is(line, line_len, "location")) {
      header_value(line, line_len, "location", &v, &vl);
      if (vl >= sizeof(out->location)) {
        return 0;
      }
      copy_value(out->location, sizeof(out->location), v, vl);
    }
    pos = end + 1U;
  }
  return !first;
}

enum {
  CH_SIZE,         /* hex digits */
  CH_EXT,          /* chunk extension until CR */
  CH_SIZE_LF,
  CH_DATA,
  CH_DATA_CR,
  CH_DATA_LF,
  CH_TRAILER,      /* after the 0 chunk: lines until an empty one */
  CH_TRAILER_LINE,
  CH_TRAILER_LF,
  CH_DONE,
  CH_ERROR
};

void zzplay_chunked_init(ZZPlayChunked *c)
{
  c->state = CH_SIZE;
  c->remaining = 0U;
  c->digits = 0;
}

static int hex_value(uint8_t c)
{
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  c = (uint8_t)lower(c);
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

int zzplay_chunked_feed(ZZPlayChunked *c, const uint8_t *data, size_t len,
                        ZZPlayHttpSink sink, void *ctx, size_t *used)
{
  size_t i = 0U;

  while (i < len && c->state != CH_DONE && c->state != CH_ERROR) {
    uint8_t b = data[i];

    switch (c->state) {
    case CH_SIZE: {
      int h = hex_value(b);

      if (h >= 0) {
        /* Seven digits keep the size below 2^28. */
        if (++c->digits > 7) {
          c->state = CH_ERROR;
          break;
        }
        c->remaining = (c->remaining << 4) | (uint32_t)h;
        i++;
      } else if (c->digits == 0) {
        c->state = CH_ERROR;
      } else if (b == '\r') {
        c->state = CH_SIZE_LF;
        i++;
      } else if (b == ';' || b == ' ' || b == '\t') {
        c->state = CH_EXT;
        i++;
      } else {
        c->state = CH_ERROR;
      }
      break;
    }
    case CH_EXT:
      if (b == '\r') {
        c->state = CH_SIZE_LF;
      }
      i++;
      break;
    case CH_SIZE_LF:
      if (b != '\n') {
        c->state = CH_ERROR;
        break;
      }
      i++;
      c->digits = 0;
      c->state = c->remaining != 0U ? CH_DATA : CH_TRAILER;
      break;
    case CH_DATA: {
      size_t n = len - i;

      if (n > c->remaining) {
        n = c->remaining;
      }
      if (sink(ctx, data + i, n) != 0) {
        c->state = CH_ERROR;
        break;
      }
      i += n;
      c->remaining -= (uint32_t)n;
      if (c->remaining == 0U) {
        c->state = CH_DATA_CR;
      }
      break;
    }
    case CH_DATA_CR:
      c->state = b == '\r' ? CH_DATA_LF : CH_ERROR;
      i++;
      break;
    case CH_DATA_LF:
      c->state = b == '\n' ? CH_SIZE : CH_ERROR;
      i++;
      break;
    case CH_TRAILER:
      c->state = b == '\r' ? CH_TRAILER_LF : CH_TRAILER_LINE;
      i++;
      break;
    case CH_TRAILER_LINE:
      if (b == '\n') {
        c->state = CH_TRAILER;
      }
      i++;
      break;
    case CH_TRAILER_LF:
      c->state = b == '\n' ? CH_DONE : CH_ERROR;
      i++;
      break;
    default:
      c->state = CH_ERROR;
      break;
    }
  }
  if (used != NULL) {
    *used = i;
  }
  if (c->state == CH_ERROR) {
    return ZZPLAY_CHUNKED_ERROR;
  }
  return c->state == CH_DONE ? ZZPLAY_CHUNKED_DONE : ZZPLAY_CHUNKED_MORE;
}

void zzplay_icy_init(ZZPlayIcy *icy, uint32_t interval)
{
  memset(icy, 0, sizeof(*icy));
  icy->interval = interval;
  icy->audio_left = interval;
}

static void icy_block_done(ZZPlayIcy *icy)
{
  char title[ZZPLAY_HTTP_TITLE_MAX];

  icy->meta[icy->meta_len] = '\0';
  if (zzplay_icy_stream_title(icy->meta, icy->meta_len, title,
                              sizeof(title)) &&
      strcmp(title, icy->title) != 0) {
    memcpy(icy->title, title, sizeof(title));
    icy->title_changed = 1;
  }
  icy->meta_len = 0U;
  icy->audio_left = icy->interval;
}

size_t zzplay_icy_strip(ZZPlayIcy *icy, uint8_t *buf, size_t len)
{
  size_t in = 0U;
  size_t out = 0U;

  if (icy->interval == 0U) {
    return len;
  }
  while (in < len) {
    size_t take;

    if (icy->meta_left != 0U) {
      take = len - in;
      if (take > icy->meta_left) {
        take = icy->meta_left;
      }
      /* meta holds 16 * 255 bytes plus a terminator, the largest block a
       * length byte can announce. */
      memcpy(icy->meta + icy->meta_len, buf + in, take);
      icy->meta_len += (uint32_t)take;
      icy->meta_left -= (uint32_t)take;
      in += take;
      if (icy->meta_left == 0U) {
        icy_block_done(icy);
      }
    } else if (icy->audio_left != 0U) {
      take = len - in;
      if (take > icy->audio_left) {
        take = icy->audio_left;
      }
      if (out != in) {
        memmove(buf + out, buf + in, take);
      }
      out += take;
      in += take;
      icy->audio_left -= (uint32_t)take;
    } else {
      uint32_t meta = (uint32_t)buf[in++] * 16U;

      if (meta == 0U) {
        icy->audio_left = icy->interval;
      } else {
        icy->meta_left = meta;
        icy->meta_len = 0U;
      }
    }
  }
  return out;
}

/* Length of the first `len` bytes of `text` without a UTF-8 sequence that
 * the cut at `len` left incomplete. */
static size_t utf8_cut(const char *text, size_t len)
{
  size_t i = len;
  size_t back = 0U;

  while (i > 0U && back < 3U && ((unsigned char)text[i - 1U] & 0xC0U) == 0x80U) {
    i--;
    back++;
  }
  if (i > 0U) {
    unsigned char lead = (unsigned char)text[i - 1U];
    size_t need = lead >= 0xF0U ? 4U : lead >= 0xE0U ? 3U : lead >= 0xC0U ? 2U : 1U;

    if (need > 1U && back + 1U < need) {
      return i - 1U;
    }
  }
  return len;
}

/* Blanks and control characters, which a title line cannot show. */
static int title_blank(char c)
{
  return (unsigned char)c <= 0x20U || (unsigned char)c == 0x7FU;
}

int zzplay_icy_stream_title(const char *meta, size_t len, char *out,
                            size_t size)
{
  static const char key[] = "StreamTitle='";
  const size_t key_len = sizeof(key) - 1U;
  size_t start;
  size_t stop;
  size_t i;

  if (meta == NULL || out == NULL || size == 0U) {
    return 0;
  }
  /* Blocks are NUL-padded to a multiple of 16. */
  for (i = 0U; i < len && meta[i] != '\0'; i++) {
  }
  len = i;

  for (start = 0U; start + key_len <= len; start++) {
    if (memcmp(meta + start, key, key_len) == 0) {
      break;
    }
  }
  if (start + key_len > len) {
    return 0;
  }
  start += key_len;

  /* The value ends at "';", so a title may contain apostrophes ("Guns N'
   * Roses"). Without that terminator, use the last quote. */
  stop = len;
  for (i = start; i + 1U < len; i++) {
    if (meta[i] == '\'' && meta[i + 1U] == ';') {
      stop = i;
      break;
    }
  }
  if (stop == len) {
    for (i = len; i > start; i--) {
      if (meta[i - 1U] == '\'') {
        break;
      }
    }
    if (i == start) {
      return 0;
    }
    stop = i - 1U;
  }
  while (start < stop && title_blank(meta[start])) {
    start++;
  }
  while (stop > start && title_blank(meta[stop - 1U])) {
    stop--;
  }
  if (stop == start) {
    return 0;
  }
  if (stop - start >= size) {
    stop = start + utf8_cut(meta + start, size - 1U);
  }
  for (i = start; i < stop; i++) {
    out[i - start] = title_blank(meta[i]) ? ' ' : meta[i];
  }
  out[stop - start] = '\0';
  return 1;
}
