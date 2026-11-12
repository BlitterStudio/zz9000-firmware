/*
 * HTTP and Shoutcast parsing used by ZZPlay's network sources.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "../tools/zzplay/zzplay-http.h"

static int test_parse_url(void)
{
  ZZPlayUrl url;

  if (!zzplay_http_parse_url("http://host/a.mp3", &url)) return 1;
  if (url.tls || url.port != 80U || strcmp(url.host, "host") != 0 ||
      strcmp(url.path, "/a.mp3") != 0)
    return 2;
  if (!zzplay_http_parse_url("HTTPS://Host:8443/p?q=1#frag", &url)) return 3;
  if (!url.tls || url.port != 8443U || strcmp(url.host, "Host") != 0 ||
      strcmp(url.path, "/p?q=1") != 0)
    return 4;
  if (!zzplay_http_parse_url("http://192.168.1.10:32400/x", &url)) return 5;
  if (strcmp(url.host, "192.168.1.10") != 0 || url.port != 32400U) return 6;
  /* No path, and a query without a path. */
  if (!zzplay_http_parse_url("http://radio.example", &url) ||
      strcmp(url.path, "/") != 0)
    return 7;
  if (!zzplay_http_parse_url("http://radio.example?id=3", &url) ||
      strcmp(url.path, "/?id=3") != 0)
    return 8;
  /* Spaces and non-ASCII are percent-encoded rather than refused. */
  if (!zzplay_http_parse_url("http://h/My Song\xe9.mp3", &url) ||
      strcmp(url.path, "/My%20Song%E9.mp3") != 0)
    return 9;
  /* Refusals. */
  if (zzplay_http_parse_url("ftp://h/x", &url)) return 10;
  if (zzplay_http_parse_url("http:///x", &url)) return 11;
  if (zzplay_http_parse_url("http://user@h/x", &url)) return 12;
  if (zzplay_http_parse_url("http://[::1]/x", &url)) return 13;
  if (zzplay_http_parse_url("http://h:0/x", &url)) return 14;
  if (zzplay_http_parse_url("http://h:65536/x", &url)) return 15;
  if (zzplay_http_parse_url("http://h:/x", &url)) return 16;
  if (zzplay_http_parse_url("http://h/a\nb", &url)) return 17;
  {
    char longhost[200];
    char text[260];
    memset(longhost, 'a', sizeof(longhost) - 1U);
    longhost[sizeof(longhost) - 1U] = '\0';
    (void)snprintf(text, sizeof(text), "http://%s/", longhost);
    if (zzplay_http_parse_url(text, &url)) return 18;
  }
  if (!zzplay_http_is_url("Https://x") || zzplay_http_is_url("Work:a.mp3"))
    return 19;
  return 0;
}

static int test_resolve(void)
{
  ZZPlayUrl base;
  char out[ZZPLAY_HTTP_URL_MAX];

  if (!zzplay_http_parse_url("http://h:8000/dir/list.pls?x=1", &base)) return 1;
  if (!zzplay_http_resolve(&base, "https://other/x", out, sizeof(out)) ||
      strcmp(out, "https://other/x") != 0)
    return 2;
  if (!zzplay_http_resolve(&base, "//cdn/y", out, sizeof(out)) ||
      strcmp(out, "http://cdn/y") != 0)
    return 3;
  if (!zzplay_http_resolve(&base, "/stream", out, sizeof(out)) ||
      strcmp(out, "http://h:8000/stream") != 0)
    return 4;
  if (!zzplay_http_resolve(&base, "live.mp3", out, sizeof(out)) ||
      strcmp(out, "http://h:8000/dir/live.mp3") != 0)
    return 5;
  if (zzplay_http_resolve(&base, "/stream", out, 10U)) return 6;
  if (!zzplay_http_parse_url("https://h/a", &base)) return 7;
  if (!zzplay_http_resolve(&base, "/b", out, sizeof(out)) ||
      strcmp(out, "https://h/b") != 0)
    return 8;
  return 0;
}

static int test_redirect_policy(void)
{
  ZZPlayUrl a;
  ZZPlayUrl b;

  if (!zzplay_http_parse_url("https://h/a", &a) ||
      !zzplay_http_parse_url("http://h/a", &b))
    return 1;
  if (zzplay_http_redirect_allowed(&a, &b)) return 2;
  if (!zzplay_http_redirect_allowed(&b, &a)) return 3;
  if (!zzplay_http_redirect_allowed(&a, &a)) return 4;
  if (zzplay_http_same_origin(&a, &b)) return 5;
  if (!zzplay_http_parse_url("https://H/other?x", &b) ||
      !zzplay_http_same_origin(&a, &b))
    return 6;
  if (!zzplay_http_parse_url("https://h:444/a", &b) ||
      zzplay_http_same_origin(&a, &b))
    return 7;
  if (!zzplay_http_parse_url("https://h2/a", &b) ||
      zzplay_http_same_origin(&a, &b))
    return 8;
  if (!zzplay_http_parse_url("https://hh/a", &b) ||
      zzplay_http_same_origin(&a, &b))
    return 9;
  return 0;
}

static int test_request(void)
{
  ZZPlayUrl url;
  char out[512];
  size_t n;

  if (!zzplay_http_parse_url("http://h:8000/s", &url)) return 1;
  n = zzplay_http_format_request(out, sizeof(out), NULL, &url, "ZZPlay/1", 1,
                                 4900U, 1, 0U, "X-Plex-Token: t\r\n");
  if (n == 0U || n != strlen(out)) return 2;
  if (strncmp(out, "GET /s HTTP/1.1\r\nHost: h:8000\r\n", 31) != 0) return 3;
  if (strstr(out, "Range: bytes=4900-\r\n") == NULL) return 4;
  if (strstr(out, "Icy-MetaData: 1\r\n") == NULL) return 5;
  if (strstr(out, "X-Plex-Token: t\r\n") == NULL) return 6;
  if (strcmp(out + n - 4U, "\r\n\r\n") != 0) return 7;
  /* Default ports stay out of the Host header; no Range unless asked. */
  if (!zzplay_http_parse_url("https://h/s", &url)) return 8;
  n = zzplay_http_format_request(out, sizeof(out), NULL, &url, "ZZPlay/1", 0, 0U, 0,
                                 0U, NULL);
  if (n == 0U || strstr(out, "Host: h\r\n") == NULL ||
      strstr(out, "Range:") != NULL || strstr(out, "Icy-MetaData") != NULL)
    return 9;
  if (zzplay_http_format_request(out, 40U, NULL, &url, "ZZPlay/1", 0, 0U, 0, 0U, NULL) !=
      0U)
    return 10;

  /* POST request with Content-Length (including 0 body length) */
  n = zzplay_http_format_request(out, sizeof(out), "POST", &url, "ZZPlay/1", 0,
                                 0U, 0, 0U, "Accept: application/xml\r\n");
  if (n == 0U || n != strlen(out)) return 11;
  if (strncmp(out, "POST /s HTTP/1.1\r\nHost: h\r\n", 27) != 0) return 12;
  if (strstr(out, "Content-Length: 0\r\n") == NULL) return 13;
  if (strstr(out, "Accept: application/xml\r\n") == NULL) return 14;

  n = zzplay_http_format_request(out, sizeof(out), "POST", &url, "ZZPlay/1", 0,
                                 0U, 0, 42U, NULL);
  if (n == 0U || strstr(out, "Content-Length: 42\r\n") == NULL) return 15;
  return 0;
}

static int parse(const char *text, ZZPlayHttpHead *head)
{
  return zzplay_http_parse_head(text, strlen(text), head);
}

static int test_head(void)
{
  ZZPlayHttpHead head;

  if (!parse("HTTP/1.1 206 Partial Content\r\n"
             "Content-Length: 100\r\n"
             "Content-Range: bytes 100-199/5000\r\n"
             "Accept-Ranges: bytes\r\n"
             "Content-Type: audio/flac\r\n",
             &head))
    return 1;
  if (head.status != 206 || !head.has_length || head.content_length != 100U ||
      !head.has_range || head.range_start != 100U || !head.has_total ||
      head.total != 5000U || !head.accept_ranges ||
      strcmp(head.content_type, "audio/flac") != 0)
    return 2;

  if (!parse("ICY 200 OK\r\nicy-name: Test FM\r\nicy-metaint: 16000\r\n",
             &head))
    return 3;
  if (head.status != 200 || !head.icy || head.icy_metaint != 16000U ||
      strcmp(head.icy_name, "Test FM") != 0 || head.has_length)
    return 4;

  if (!parse("HTTP/1.0 302 Found\nLOCATION:  http://x/y \n", &head) ||
      head.status != 302 || strcmp(head.location, "http://x/y") != 0)
    return 5;
  if (!parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n", &head) ||
      !head.chunked)
    return 6;
  if (!parse("HTTP/1.1 206 x\r\nContent-Range: bytes 0-9/*\r\n", &head) ||
      !head.has_range || head.has_total)
    return 7;

  /* Lengths that do not fit 32 bits fail rather than wrap. */
  if (parse("HTTP/1.1 200 OK\r\nContent-Length: 4294967296\r\n", &head))
    return 8;
  if (!parse("HTTP/1.1 200 OK\r\nContent-Length: 4294967295\r\n", &head) ||
      head.content_length != 0xFFFFFFFFUL)
    return 9;
  if (parse("HTTP/1.1 200 OK\r\nContent-Length: 12x\r\n", &head)) return 10;
  if (parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n", &head)) return 11;
  if (parse("HTTP/1.1 206 x\r\nContent-Range: bytes 10-5/20\r\n", &head))
    return 12;
  if (parse("SIP/2.0 200 OK\r\n", &head)) return 13;
  if (parse("", &head)) return 14;
  return 0;
}

struct collect {
  uint8_t data[256];
  size_t len;
};

static int collect_sink(void *ctx, const uint8_t *data, size_t len)
{
  struct collect *c = (struct collect *)ctx;

  if (len > sizeof(c->data) - c->len) return 1;
  memcpy(c->data + c->len, data, len);
  c->len += len;
  return 0;
}

static int test_chunked(void)
{
  static const char body[] =
      "4\r\nWiki\r\n6;ext=1\r\npedia \r\nE\r\nin \r\n\r\nchunks.\r\n0\r\n"
      "Trailer: x\r\n\r\nNEXT";
  const size_t body_len = sizeof(body) - 1U - 4U;
  static const char want[] = "Wikipedia in \r\n\r\nchunks.";
  size_t split;

  /* Every two-piece split of the body reassembles the same bytes. */
  for (split = 0U; split <= body_len; split++) {
    ZZPlayChunked c;
    struct collect out;
    size_t used = 0U;
    int rc;

    zzplay_chunked_init(&c);
    out.len = 0U;
    rc = zzplay_chunked_feed(&c, (const uint8_t *)body, split, collect_sink,
                             &out, &used);
    if (rc == ZZPLAY_CHUNKED_ERROR || used != split) return 1;
    rc = zzplay_chunked_feed(&c, (const uint8_t *)body + split,
                             sizeof(body) - 1U - split, collect_sink, &out,
                             &used);
    if (rc != ZZPLAY_CHUNKED_DONE) return 2;
    if (split + used != body_len) return 3;
    if (out.len != sizeof(want) - 1U || memcmp(out.data, want, out.len) != 0)
      return 4;
  }
  {
    ZZPlayChunked c;
    struct collect out;
    out.len = 0U;
    zzplay_chunked_init(&c);
    if (zzplay_chunked_feed(&c, (const uint8_t *)"zz\r\n", 4U, collect_sink,
                            &out, NULL) != ZZPLAY_CHUNKED_ERROR)
      return 5;
    zzplay_chunked_init(&c);
    if (zzplay_chunked_feed(&c, (const uint8_t *)"FFFFFFFF\r\n", 10U,
                            collect_sink, &out, NULL) != ZZPLAY_CHUNKED_ERROR)
      return 6;
  }
  return 0;
}

/* Build a stream: `interval` audio bytes, then a metadata block, then
 * more audio. Audio bytes are their index mod 251 so order is checked. */
static size_t build_icy(uint8_t *out, uint32_t interval, const char *meta,
                        uint32_t tail)
{
  size_t n = 0U;
  uint32_t i;
  size_t meta_len = strlen(meta);
  size_t blocks = (meta_len + 15U) / 16U;

  for (i = 0U; i < interval; i++) out[n++] = (uint8_t)(i % 251U);
  out[n++] = (uint8_t)blocks;
  memset(out + n, 0, blocks * 16U);
  memcpy(out + n, meta, meta_len);
  n += blocks * 16U;
  for (i = 0U; i < tail; i++) out[n++] = (uint8_t)((interval + i) % 251U);
  return n;
}

static int test_icy_strip(void)
{
  static uint8_t stream[2048];
  static uint8_t work[2048];
  const uint32_t interval = 600U;
  const char *meta = "StreamTitle='Artist - Song';StreamUrl='';";
  size_t total = build_icy(stream, interval, meta, 300U);
  size_t step;

  /* Feed in steps of every size from 1 to 97 bytes. */
  for (step = 1U; step < 98U; step++) {
    ZZPlayIcy icy;
    size_t pos = 0U;
    size_t audio = 0U;

    zzplay_icy_init(&icy, interval);
    while (pos < total) {
      size_t n = total - pos < step ? total - pos : step;
      size_t kept;
      size_t i;

      memcpy(work, stream + pos, n);
      kept = zzplay_icy_strip(&icy, work, n);
      for (i = 0U; i < kept; i++) {
        if (work[i] != (uint8_t)((audio + i) % 251U)) return 1;
      }
      audio += kept;
      pos += n;
    }
    if (audio != interval + 300U) return 2;
    if (!icy.title_changed || strcmp(icy.title, "Artist - Song") != 0)
      return 3;
  }
  {
    /* A zero length byte is no metadata; the next interval follows. */
    ZZPlayIcy icy;
    size_t n = 0U;
    uint32_t i;

    for (i = 0U; i < 8U; i++) work[n++] = (uint8_t)i;
    work[n++] = 0U;
    for (i = 8U; i < 12U; i++) work[n++] = (uint8_t)i;
    zzplay_icy_init(&icy, 8U);
    if (zzplay_icy_strip(&icy, work, n) != 12U) return 4;
    for (i = 0U; i < 12U; i++)
      if (work[i] != (uint8_t)i) return 5;
    if (icy.title_changed) return 6;
  }
  {
    /* The same title again is not a change. */
    ZZPlayIcy icy;
    total = build_icy(stream, 16U, meta, 0U);
    zzplay_icy_init(&icy, 16U);
    (void)zzplay_icy_strip(&icy, stream, total);
    icy.title_changed = 0;
    total = build_icy(stream, 16U, meta, 0U);
    (void)zzplay_icy_strip(&icy, stream + 16U, total - 16U);
    if (icy.title_changed) return 7;
  }
  {
    /* No interval: everything is audio. */
    ZZPlayIcy icy;
    zzplay_icy_init(&icy, 0U);
    if (zzplay_icy_strip(&icy, work, 77U) != 77U) return 8;
  }
  return 0;
}

static int title(const char *meta, const char *want)
{
  char out[ZZPLAY_HTTP_TITLE_MAX];
  int ok = zzplay_icy_stream_title(meta, strlen(meta), out, sizeof(out));

  if (want == NULL) return ok ? 1 : 0;
  return ok && strcmp(out, want) == 0 ? 0 : 1;
}

static int test_stream_title(void)
{
  if (title("StreamTitle='A - B';StreamUrl='x';", "A - B")) return 1;
  if (title("StreamTitle='Guns N' Roses - Patience';", "Guns N' Roses - Patience"))
    return 2;
  if (title("StreamTitle='No terminator'", "No terminator")) return 3;
  if (title("StreamTitle='';", NULL)) return 4;
  if (title("StreamUrl='x';", NULL)) return 5;
  if (title("StreamTitle='unterminated", NULL)) return 6;
  if (title("StreamTitle='  padded  ';", "padded")) return 7;
  {
    char out[8];
    if (!zzplay_icy_stream_title("StreamTitle='Long title';", 25U, out,
                                 sizeof(out)) ||
        strcmp(out, "Long ti") != 0)
      return 8;
  }
  return 0;
}

int main(void)
{
  int rc;

  rc = test_parse_url();
  if (rc != 0) { printf("parse_url %d\n", rc); return 10 + rc; }
  rc = test_resolve();
  if (rc != 0) { printf("resolve %d\n", rc); return 40 + rc; }
  rc = test_redirect_policy();
  if (rc != 0) { printf("redirect_policy %d\n", rc); return 50 + rc; }
  rc = test_request();
  if (rc != 0) { printf("request %d\n", rc); return 60 + rc; }
  rc = test_head();
  if (rc != 0) { printf("head %d\n", rc); return 80 + rc; }
  rc = test_chunked();
  if (rc != 0) { printf("chunked %d\n", rc); return 100 + rc; }
  rc = test_icy_strip();
  if (rc != 0) { printf("icy_strip %d\n", rc); return 120 + rc; }
  rc = test_stream_title();
  if (rc != 0) { printf("stream_title %d\n", rc); return 140 + rc; }
  printf("zzplay_http_test: all checks passed\n");
  return 0;
}
