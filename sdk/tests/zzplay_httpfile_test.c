/*
 * The HTTP backend behind ZZPlayFile, driven by a scripted server.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/zzplay/zzplay-httpfile.h"

#define RESOURCE_BYTES 200000U

static uint8_t resource[RESOURCE_BYTES];

typedef struct Server {
  uint32_t size;
  int ranges;            /* honours Range with 206 */
  int accept_ranges;     /* advertises Accept-Ranges on a 200 */
  int refuse_range_416;  /* answers any Range with 416 */
  int no_length;         /* chunked-style: no length */
  uint32_t icy_interval; /* Shoutcast stream with metadata */
  const char *icy_title;
  uint32_t read_size;    /* bytes per read() */
  int not_found;         /* answers 404 */
  uint32_t fail_after;   /* fail a read once this many bytes were sent */
  /* What the client asked for. */
  int opens;
  int last_has_range;
  uint32_t last_from;
} Server;

typedef struct Stream {
  Server *server;
  uint32_t pos;       /* resource offset of the next audio byte */
  uint32_t audio_in;  /* audio bytes sent since the last metadata block */
  uint32_t sent;
  int failed;
} Stream;

static void *fake_open(void *ctx, const char *url, int has_range,
                       uint32_t from, ZZPlayHttpHead *head, char *err,
                       size_t err_size)
{
  Server *s = (Server *)ctx;
  Stream *st;

  (void)url;
  memset(head, 0, sizeof(*head));
  s->opens++;
  if (s->not_found) {
    head->status = 404;
    (void)snprintf(err, err_size, "The server answered HTTP 404");
    return NULL;
  }
  s->last_has_range = has_range;
  s->last_from = from;
  if (has_range && s->refuse_range_416) {
    head->status = 416;
    (void)snprintf(err, err_size, "HTTP 416");
    return NULL;
  }
  st = (Stream *)calloc(1U, sizeof(*st));
  st->server = s;
  if (s->icy_interval != 0U) {
    head->status = 200;
    head->icy = 1;
    head->icy_metaint = s->icy_interval;
    (void)snprintf(head->icy_name, sizeof(head->icy_name), "Test FM");
    return st;
  }
  if (has_range && s->ranges) {
    head->status = 206;
    head->has_range = 1;
    head->range_start = from;
    head->has_total = 1;
    head->total = s->size;
    head->has_length = 1;
    head->content_length = s->size - from;
    st->pos = from;
  } else {
    head->status = 200;
    if (!s->no_length) {
      head->has_length = 1;
      head->content_length = s->size;
    } else {
      head->chunked = 1;
    }
    head->accept_ranges = s->accept_ranges;
  }
  return st;
}

static long fake_read(void *ctx, void *stream, uint8_t *buf, size_t size)
{
  Server *s = (Server *)ctx;
  Stream *st = (Stream *)stream;
  size_t n = 0U;

  if (s->fail_after != 0U && st->sent >= s->fail_after) {
    st->failed = 1;
    return -1;
  }
  if (size > s->read_size) size = s->read_size;
  while (n < size) {
    if (s->icy_interval != 0U && st->audio_in == s->icy_interval) {
      /* Insert one metadata block: length byte plus 16-byte blocks. */
      char meta[64];
      size_t ml;
      size_t blocks;

      if (size - n < 1U + 64U) break;
      memset(meta, 0, sizeof(meta));
      (void)snprintf(meta, sizeof(meta), "StreamTitle='%s';", s->icy_title);
      ml = strlen(meta);
      blocks = (ml + 15U) / 16U;
      buf[n++] = (uint8_t)blocks;
      memcpy(buf + n, meta, blocks * 16U);
      n += blocks * 16U;
      st->audio_in = 0U;
      continue;
    }
    if (st->pos >= s->size) break;
    buf[n++] = resource[st->pos++];
    st->audio_in++;
  }
  st->sent += (uint32_t)n;
  return (long)n;
}

static const char *fake_error(void *ctx, void *stream)
{
  (void)ctx;
  (void)stream;
  return "connection reset";
}

static void fake_close(void *ctx, void *stream)
{
  (void)ctx;
  free(stream);
}

static ZZPlayHttpFile *open_server(Server *s)
{
  ZZPlayHttpStreamOps ops;
  char err[128];

  ops.open = fake_open;
  ops.read = fake_read;
  ops.error = fake_error;
  ops.close = fake_close;
  ops.ctx = s;
  return zzplay_httpfile_open(&ops, "http://h/x", err, sizeof(err));
}

static void server_defaults(Server *s)
{
  memset(s, 0, sizeof(*s));
  s->size = 5000U;
  s->ranges = 1;
  s->read_size = 1000U;
}

static int same(const uint8_t *got, uint32_t at, size_t len)
{
  return memcmp(got, resource + at, len) == 0;
}

static int test_whole_read(void)
{
  Server s;
  ZZPlayHttpFile *f;
  static uint8_t got[6000];

  server_defaults(&s);
  f = open_server(&s);
  if (f == NULL) return 1;
  if (!zzplay_httpfile_seekable(f) || zzplay_httpfile_live(f)) return 2;
  if (zzplay_httpfile_read(f, got, sizeof(got)) != 5000U) return 3;
  if (!same(got, 0U, 5000U)) return 4;
  if (!zzplay_httpfile_eof(f) || zzplay_httpfile_error(f)) return 5;
  if (s.opens != 1 || !s.last_has_range || s.last_from != 0U) return 6;
  zzplay_httpfile_close(f);
  return 0;
}

static int test_ranged_seek(void)
{
  Server s;
  ZZPlayHttpFile *f;
  uint8_t got[200];

  server_defaults(&s);
  s.size = RESOURCE_BYTES;
  f = open_server(&s);
  if (f == NULL) return 1;
  /* A probe reads the start first, which the file keeps. */
  if (zzplay_httpfile_read(f, got, 100U) != 100U || !same(got, 0U, 100U))
    return 16;
  /* Far forward: one new ranged request at the target. */
  if (!zzplay_httpfile_seek(f, 150000L, ZZPLAY_FILE_BEGIN)) return 2;
  if (zzplay_httpfile_read(f, got, 100U) != 100U || !same(got, 150000U, 100U))
    return 3;
  if (s.opens != 2 || s.last_from != 150000U) return 4;
  /* Backward inside the read-ahead window: no request. */
  if (!zzplay_httpfile_seek(f, 150050L, ZZPLAY_FILE_BEGIN)) return 5;
  if (zzplay_httpfile_read(f, got, 50U) != 50U || !same(got, 150050U, 50U))
    return 6;
  if (s.opens != 2) return 7;
  /* From the end: the length is known. */
  if (!zzplay_httpfile_seek(f, -128L, ZZPLAY_FILE_END)) return 8;
  if (zzplay_httpfile_tell(f) != (long)(RESOURCE_BYTES - 128U)) return 9;
  if (zzplay_httpfile_read(f, got, 200U) != 128U ||
      !same(got, RESOURCE_BYTES - 128U, 128U) || !zzplay_httpfile_eof(f))
    return 10;
  /* Back to the start: served from the cached head. */
  s.opens = 0;
  if (!zzplay_httpfile_seek(f, 0L, ZZPLAY_FILE_BEGIN)) return 11;
  if (zzplay_httpfile_read(f, got, 100U) != 100U || !same(got, 0U, 100U))
    return 12;
  if (s.opens != 0) return 13;
  /* A short forward seek reads through instead of reconnecting. */
  if (!zzplay_httpfile_seek(f, 40000L, ZZPLAY_FILE_BEGIN)) return 14;
  if (zzplay_httpfile_read(f, got, 10U) != 10U || !same(got, 40000U, 10U))
    return 15;
  zzplay_httpfile_close(f);
  return 0;
}

static int test_no_ranges(void)
{
  Server s;
  ZZPlayHttpFile *f;
  uint8_t got[100];

  /* 200 without Accept-Ranges: not seekable, but forward seeks read
   * through and the cached start is still reachable. */
  server_defaults(&s);
  s.size = RESOURCE_BYTES;
  s.ranges = 0;
  f = open_server(&s);
  if (f == NULL || zzplay_httpfile_seekable(f)) return 1;
  if (!zzplay_httpfile_seek(f, 120000L, ZZPLAY_FILE_BEGIN)) return 2;
  if (zzplay_httpfile_read(f, got, 10U) != 10U || !same(got, 120000U, 10U))
    return 3;
  if (s.opens != 1) return 4;
  if (zzplay_httpfile_seek(f, 50000L, ZZPLAY_FILE_BEGIN)) return 5;
  if (zzplay_httpfile_tell(f) != 120010L) return 6;
  if (!zzplay_httpfile_seek(f, 16L, ZZPLAY_FILE_BEGIN)) return 7;
  if (zzplay_httpfile_read(f, got, 10U) != 10U || !same(got, 16U, 10U))
    return 8;
  zzplay_httpfile_close(f);

  /* 200 with Accept-Ranges and a length counts as seekable. */
  server_defaults(&s);
  s.size = RESOURCE_BYTES;
  s.accept_ranges = 1;
  s.ranges = 0;
  s.refuse_range_416 = 0;
  f = open_server(&s);
  if (f == NULL || !zzplay_httpfile_seekable(f)) return 9;
  zzplay_httpfile_close(f);

  /* Unknown length: no seeking from the end. */
  server_defaults(&s);
  s.ranges = 0;
  s.no_length = 1;
  f = open_server(&s);
  if (f == NULL || zzplay_httpfile_seekable(f)) return 10;
  if (zzplay_httpfile_seek(f, 0L, ZZPLAY_FILE_END)) return 11;
  zzplay_httpfile_close(f);
  return 0;
}

static int test_range_refused(void)
{
  Server s;
  ZZPlayHttpFile *f;
  uint8_t got[100];

  /* A server answering 416 to bytes=0- is retried without a range. */
  server_defaults(&s);
  s.refuse_range_416 = 1;
  s.ranges = 0;
  f = open_server(&s);
  if (f == NULL) return 1;
  if (s.opens != 2 || s.last_has_range) return 2;
  if (zzplay_httpfile_read(f, got, 100U) != 100U || !same(got, 0U, 100U))
    return 3;
  zzplay_httpfile_close(f);
  return 0;
}

static int test_live_stream(void)
{
  Server s;
  ZZPlayHttpFile *f;
  static uint8_t got[8000];
  char title[64];

  server_defaults(&s);
  s.size = RESOURCE_BYTES;
  s.icy_interval = 1000U;
  s.icy_title = "Artist - Song";
  s.read_size = 777U;
  f = open_server(&s);
  if (f == NULL) return 1;
  if (!zzplay_httpfile_live(f) || zzplay_httpfile_seekable(f)) return 2;
  if (strcmp(zzplay_httpfile_station(f), "Test FM") != 0) return 3;
  /* The reader sees only audio, in order, across metadata blocks. */
  if (zzplay_httpfile_read(f, got, sizeof(got)) != sizeof(got)) return 4;
  if (!same(got, 0U, sizeof(got))) return 5;
  if (!zzplay_httpfile_take_title(f, title, sizeof(title)) ||
      strcmp(title, "Artist - Song") != 0)
    return 6;
  if (zzplay_httpfile_take_title(f, title, sizeof(title))) return 7;
  /* A probe can rewind to the cached start of a live stream... */
  if (!zzplay_httpfile_seek(f, 0L, ZZPLAY_FILE_BEGIN)) return 8;
  if (zzplay_httpfile_read(f, got, 4U) != 4U || !same(got, 0U, 4U)) return 9;
  /* ...but no length is known. */
  if (zzplay_httpfile_seek(f, 0L, ZZPLAY_FILE_END)) return 10;
  zzplay_httpfile_close(f);
  return 0;
}

static int test_failure(void)
{
  Server s;
  ZZPlayHttpFile *f;
  static uint8_t got[5000];

  server_defaults(&s);
  s.fail_after = 3000U;
  f = open_server(&s);
  if (f == NULL) return 1;
  if (zzplay_httpfile_read(f, got, sizeof(got)) != 3000U) return 2;
  if (!zzplay_httpfile_error(f)) return 3;
  if (strcmp(zzplay_httpfile_error_text(f), "connection reset") != 0) return 4;
  zzplay_httpfile_clear_error(f);
  if (zzplay_httpfile_error(f)) return 5;
  zzplay_httpfile_close(f);

  /* An open that fails reports the server's reason. */
  {
    ZZPlayHttpStreamOps ops;
    char err[64];

    server_defaults(&s);
    s.not_found = 1;
    ops.open = fake_open;
    ops.read = fake_read;
    ops.error = fake_error;
    ops.close = fake_close;
    ops.ctx = &s;
    if (zzplay_httpfile_open(&ops, "http://h/missing", err, sizeof(err)) !=
        NULL)
      return 6;
    if (strcmp(err, "The server answered HTTP 404") != 0) return 7;
  }
  return 0;
}

int main(void)
{
  uint32_t i;
  int rc;

  for (i = 0U; i < RESOURCE_BYTES; i++) {
    resource[i] = (uint8_t)((i * 7U + (i >> 8)) & 0xFFU);
  }
  rc = test_whole_read();
  if (rc != 0) { printf("whole_read %d\n", rc); return 10 + rc; }
  rc = test_ranged_seek();
  if (rc != 0) { printf("ranged_seek %d\n", rc); return 30 + rc; }
  rc = test_no_ranges();
  if (rc != 0) { printf("no_ranges %d\n", rc); return 50 + rc; }
  rc = test_range_refused();
  if (rc != 0) { printf("range_refused %d\n", rc); return 70 + rc; }
  rc = test_live_stream();
  if (rc != 0) { printf("live_stream %d\n", rc); return 90 + rc; }
  rc = test_failure();
  if (rc != 0) { printf("failure %d\n", rc); return 110 + rc; }
  printf("zzplay_httpfile_test: all checks passed\n");
  return 0;
}
