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
  int stop;              /* the user stopped the item: pauses give up */
  int fail_status;       /* with fail_text: open answers this status (0 for */
  const char *fail_text; /* none) and gives this reason */
  int silent_fail;       /* open fails and says nothing at all */
  uint32_t close_after;  /* the body ends once this many bytes were sent */
  uint32_t extra_bytes;  /* bytes sent past the length the head named */
  const char *read_error; /* error() text, NULL for the default */
  int fast_live;         /* endless live stream of pattern blocks, no
                          * metadata, filled a block at a time */
  int pauses;
  uint32_t paused_ms;
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
  uint32_t extra;     /* bytes sent past the resource */
  uint64_t live_pos;  /* fast_live: stream offset of the next byte */
  int failed;
} Stream;

/* The byte value of 4 KiB block `block` of a fast_live stream: block
 * 2^20 (4 GiB) and on differ from the blocks 2^20 earlier. */
static uint8_t live_value(uint64_t block)
{
  return (uint8_t)((block ^ ((block >> 20) * 0x55U)) & 0xFFU);
}

static void *fake_open(void *ctx, const char *url, int has_range,
                       uint32_t from, ZZPlayHttpHead *head, char *err,
                       size_t err_size)
{
  Server *s = (Server *)ctx;
  Stream *st;

  (void)url;
  if (s->silent_fail) {
    /* A transport that fails before any answer, leaving head and err
     * untouched. */
    s->opens++;
    return NULL;
  }
  memset(head, 0, sizeof(*head));
  s->opens++;
  if (s->not_found) {
    head->status = 404;
    (void)snprintf(err, err_size, "The server answered HTTP 404");
    return NULL;
  }
  if (s->fail_text != NULL) {
    head->status = s->fail_status;
    (void)snprintf(err, err_size, "%s", s->fail_text);
    return NULL;
  }
  s->last_has_range = has_range;
  if (s->fast_live) {
    st = (Stream *)calloc(1U, sizeof(*st));
    st->server = s;
    head->status = 200;
    head->icy = 1;
    return st;
  }
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

  if (s->fast_live) {
    /* Every 4 KiB block holds one value, which changes past 4 GiB in a
     * way the first 4 GiB never repeat. */
    while (n < size) {
      uint64_t block = st->live_pos >> 12;
      size_t left = (size_t)(4096U - (st->live_pos & 4095U));

      if (left > size - n) left = size - n;
      memset(buf + n, (int)live_value(block), left);
      n += left;
      st->live_pos += left;
    }
    return (long)n;
  }
  if (s->fail_after != 0U && st->sent >= s->fail_after) {
    st->failed = 1;
    return -1;
  }
  if (s->close_after != 0U) {
    if (st->sent >= s->close_after) return 0;
    if (size > s->close_after - st->sent) size = s->close_after - st->sent;
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
    if (st->pos >= s->size) {
      if (st->extra >= s->extra_bytes) break;
      buf[n++] = 0xEEU;
      st->extra++;
      continue;
    }
    buf[n++] = resource[st->pos++];
    st->audio_in++;
  }
  st->sent += (uint32_t)n;
  return (long)n;
}

static const char *fake_error(void *ctx, void *stream)
{
  Server *s = (Server *)ctx;

  (void)stream;
  return s->read_error != NULL ? s->read_error : "connection reset";
}

static void fake_close(void *ctx, void *stream)
{
  (void)ctx;
  free(stream);
}

static int fake_pause(void *ctx, uint32_t ms)
{
  Server *s = (Server *)ctx;

  s->pauses++;
  s->paused_ms += ms;
  return !s->stop;
}

static ZZPlayHttpFile *open_server(Server *s)
{
  ZZPlayHttpStreamOps ops;
  char err[128];

  ops.open = fake_open;
  ops.read = fake_read;
  ops.error = fake_error;
  ops.close = fake_close;
  ops.pause = fake_pause;
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

static int test_live_reconnect(void)
{
  Server s;
  ZZPlayHttpFile *f;
  static uint8_t got[8000];

  /* Each connection drops after 3000 bytes; the reader still gets all
   * the audio it asks for, over two reconnects. */
  server_defaults(&s);
  s.size = RESOURCE_BYTES;
  s.icy_interval = 1000U;
  s.icy_title = "A";
  s.fail_after = 3000U;
  f = open_server(&s);
  if (f == NULL) return 1;
  if (zzplay_httpfile_read(f, got, sizeof(got)) != sizeof(got)) return 2;
  if (zzplay_httpfile_error(f)) return 3;
  if (s.opens != 3 || s.pauses != 2) return 4;
  if (s.last_has_range) return 5;
  zzplay_httpfile_close(f);

  /* Three failed reopens in a row end the stream with the last reason. */
  server_defaults(&s);
  s.size = RESOURCE_BYTES;
  s.icy_interval = 1000U;
  s.icy_title = "A";
  s.fail_after = 3000U;
  f = open_server(&s);
  if (f == NULL) return 6;
  s.not_found = 1;
  if (zzplay_httpfile_read(f, got, sizeof(got)) >= sizeof(got)) return 7;
  if (!zzplay_httpfile_error(f)) return 8;
  if (strcmp(zzplay_httpfile_error_text(f), "The server answered HTTP 404") !=
      0)
    return 9;
  if (s.opens != 4 || s.pauses != 3 || s.paused_ms != 7000U) return 10;
  zzplay_httpfile_close(f);

  /* Stopping the item during the pause gives up at once. */
  server_defaults(&s);
  s.size = RESOURCE_BYTES;
  s.icy_interval = 1000U;
  s.icy_title = "A";
  s.fail_after = 3000U;
  f = open_server(&s);
  if (f == NULL) return 11;
  s.stop = 1;
  if (zzplay_httpfile_read(f, got, sizeof(got)) >= sizeof(got)) return 12;
  if (s.opens != 1 || s.pauses != 1) return 13;
  zzplay_httpfile_close(f);

  /* A finite download that drops is an error, not a reconnect. */
  server_defaults(&s);
  s.fail_after = 3000U;
  f = open_server(&s);
  if (f == NULL) return 14;
  if (zzplay_httpfile_read(f, got, 5000U) != 3000U || s.pauses != 0)
    return 15;
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
    ops.pause = fake_pause;
    ops.ctx = &s;
    if (zzplay_httpfile_open(&ops, "http://h/missing", err, sizeof(err)) !=
        NULL)
      return 6;
    if (strcmp(err, "The server answered HTTP 404") != 0) return 7;
  }
  return 0;
}

static ZZPlayHttpFile *open_server_err(Server *s, char *err, size_t err_size)
{
  ZZPlayHttpStreamOps ops;

  ops.open = fake_open;
  ops.read = fake_read;
  ops.error = fake_error;
  ops.close = fake_close;
  ops.pause = fake_pause;
  ops.ctx = s;
  return zzplay_httpfile_open(&ops, "http://h/x", err, err_size);
}

/* A live stream keeps going past 4 GiB (about 74 hours at 128 kbit/s):
 * the bytes after it come from the stream, not from the cached start a
 * wrapped 32-bit position would point at; tell and seek, whose interface
 * is a long, refuse positions they cannot hold. */
static int test_live_past_4gib(void)
{
  static uint8_t got[1024U * 1024U];
  const uint64_t four_gib = (uint64_t)1U << 32;
  uint64_t at = 0U;
  Server s;
  ZZPlayHttpFile *f;
  size_t i;
  int rc = 0;

  server_defaults(&s);
  s.fast_live = 1;
  f = open_server(&s);
  if (f == NULL) return 1;
  if (!zzplay_httpfile_live(f)) { rc = 2; goto done; }
  while (at < four_gib) {
    if (zzplay_httpfile_read(f, got, sizeof(got)) != sizeof(got)) {
      rc = 3;
      goto done;
    }
    if (got[0] != live_value(at >> 12)) { rc = 4; goto done; }
    at += sizeof(got);
  }
  if (zzplay_httpfile_tell(f) != -1L) { rc = 5; goto done; }
  if (zzplay_httpfile_seek(f, 1L, ZZPLAY_FILE_CURRENT)) { rc = 6; goto done; }
  /* The first 64 KiB past 4 GiB, where a wrapped position would read the
   * cached first 32 KiB. */
  if (zzplay_httpfile_read(f, got, 65536U) != 65536U) { rc = 7; goto done; }
  for (i = 0U; i < 65536U; i++) {
    if (got[i] != live_value((at + i) >> 12)) { rc = 8; goto done; }
  }
  if (s.opens != 1 || zzplay_httpfile_error(f)) { rc = 9; goto done; }
  /* The cached start can still be reached from the beginning. */
  if (!zzplay_httpfile_seek(f, 0L, ZZPLAY_FILE_BEGIN) ||
      zzplay_httpfile_read(f, got, 16U) != 16U || got[0] != live_value(0U)) {
    rc = 10;
  }
done:
  zzplay_httpfile_close(f);
  return rc;
}

/* Faults a server or the network produce: each ends with a reason to
 * show, and nothing stays open (the sanitizer's leak check sees to it). */
static int test_faults(void)
{
  static const struct {
    int status;
    const char *text;
  } refusals[] = {
    { 401, "The server answered HTTP 401" },
    { 404, "The server answered HTTP 404" },
    { 500, "The server answered HTTP 500" },
    { 0, "The server sent a malformed response" },  /* a truncated head */
    { 0, "The server stopped responding" },         /* a stalled answer */
    { 0, "Cancelled" },                             /* stopped while stalled */
    { 0, "Cannot connect to h" }                    /* the server is down */
  };
  static uint8_t got[9000];
  Server s;
  ZZPlayHttpFile *f;
  char err[128];
  size_t i;

  for (i = 0U; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
    server_defaults(&s);
    s.fail_status = refusals[i].status;
    s.fail_text = refusals[i].text;
    if (open_server_err(&s, err, sizeof(err)) != NULL) return 1;
    if (strcmp(err, refusals[i].text) != 0) return 2;
    if (s.opens != 1) return 3;
  }

  /* A transport that fails without a word still leaves a reason. */
  server_defaults(&s);
  s.silent_fail = 1;
  memset(err, 'x', sizeof(err));
  if (open_server_err(&s, err, sizeof(err)) != NULL) return 4;
  if (strcmp(err, "The network connection failed") != 0) return 5;

  /* The server drops the connection mid-body. */
  server_defaults(&s);
  s.close_after = 3000U;
  f = open_server(&s);
  if (f == NULL) return 6;
  if (zzplay_httpfile_read(f, got, 5000U) != 3000U || !same(got, 0U, 3000U))
    return 7;
  if (!zzplay_httpfile_error(f) ||
      strcmp(zzplay_httpfile_error_text(f),
             "The connection closed before the end of the data") != 0)
    return 8;
  zzplay_httpfile_close(f);

  /* A stalled read the user stops fails with the transport's reason. */
  server_defaults(&s);
  s.fail_after = 2000U;
  s.read_error = "Cancelled";
  f = open_server(&s);
  if (f == NULL) return 9;
  if (zzplay_httpfile_read(f, got, 5000U) != 2000U) return 10;
  if (strcmp(zzplay_httpfile_error_text(f), "Cancelled") != 0) return 11;
  zzplay_httpfile_close(f);

  /* A read failure without a reason still has one to show. */
  server_defaults(&s);
  s.fail_after = 1000U;
  s.read_error = "";
  f = open_server(&s);
  if (f == NULL) return 12;
  if (zzplay_httpfile_read(f, got, 5000U) != 1000U) return 13;
  if (strcmp(zzplay_httpfile_error_text(f), "The network connection failed") !=
      0)
    return 14;
  zzplay_httpfile_close(f);

  /* A body longer than its Content-Length: the reader gets the named
   * bytes and the end, never the surplus. 700-byte reads put the end of
   * the resource inside one block. */
  server_defaults(&s);
  s.ranges = 0;
  s.extra_bytes = 3000U;
  s.read_size = 700U;
  f = open_server(&s);
  if (f == NULL) return 15;
  if (zzplay_httpfile_read(f, got, sizeof(got)) != 5000U || !same(got, 0U, 5000U))
    return 16;
  if (!zzplay_httpfile_eof(f) || zzplay_httpfile_error(f)) return 17;
  zzplay_httpfile_close(f);

  /* A live stream that stalls, then the user stops the item while it
   * waits to reconnect. */
  server_defaults(&s);
  s.size = RESOURCE_BYTES;
  s.icy_interval = 1000U;
  s.icy_title = "A";
  s.fail_after = 3000U;
  s.read_error = "The server stopped responding";
  f = open_server(&s);
  if (f == NULL) return 18;
  s.stop = 1;
  if (zzplay_httpfile_read(f, got, sizeof(got)) >= sizeof(got)) return 19;
  if (strcmp(zzplay_httpfile_error_text(f), "Cancelled") != 0) return 20;
  zzplay_httpfile_close(f);
  return 0;
}

/* The open path for one network track: the byte accesses zzplay.c makes
 * between choosing the entry and the engine's first reads - the tag
 * reader's length seek and ID3v2 read (network sources never fetch the
 * ID3v1 trailer at the far end), the file-size seek, the probe's
 * reads, then the close and the engine's reopen. Open, tags and probe
 * share the first connection, and so does the engine start; only a real
 * seek pays for a new ranged request. */
static int test_open_path_connections(void)
{
  static uint8_t got[16384];
  static const char title[] = "Net Title";
  uint8_t orig[30];
  ZZPlayHttpStreamOps ops;
  Server s;
  ZZPlayHttpFile *f;
  char err[64];
  const uint32_t tag_total = 30U;
  int rc = 0;
  size_t n;
  size_t r;

  memcpy(orig, resource, sizeof(orig));
  /* ID3v2.3 with one TIT2 frame: 10-byte tag header, 10-byte frame
   * header, encoding byte, 9 characters of title. */
  memset(resource, 0, tag_total);
  resource[0] = 'I';
  resource[1] = 'D';
  resource[2] = '3';
  resource[3] = 3;
  resource[9] = 20;   /* synchsafe body size */
  memcpy(resource + 10, "TIT2", 4);
  resource[17] = 10;  /* frame size */
  memcpy(resource + 19, title, sizeof(title) - 1U);

  server_defaults(&s);
  s.size = RESOURCE_BYTES;
  ops.open = fake_open;
  ops.read = fake_read;
  ops.error = fake_error;
  ops.close = fake_close;
  ops.pause = fake_pause;
  ops.ctx = &s;
  f = zzplay_httpfile_open_cached(&ops, "http://h/x", err, sizeof(err));
  if (f == NULL || s.opens != 1) { rc = 1; goto done; }
  /* zzplay_tags_read. */
  if (!zzplay_httpfile_seek(f, 0L, ZZPLAY_FILE_END) ||
      zzplay_httpfile_tell(f) != (long)RESOURCE_BYTES ||
      !zzplay_httpfile_seek(f, 0L, ZZPLAY_FILE_BEGIN)) {
    rc = 2;
    goto done;
  }
  if (zzplay_httpfile_read(f, got, 10U) != 10U) { rc = 3; goto done; }
  if (!zzplay_httpfile_seek(f, 0L, ZZPLAY_FILE_BEGIN) ||
      zzplay_httpfile_read(f, got, tag_total) != tag_total ||
      memcmp(got, resource, tag_total) != 0) {
    rc = 4;
    goto done;
  }
  /* zzplay.c: the file-size seek. */
  if (!zzplay_httpfile_seek(f, 0L, ZZPLAY_FILE_END) ||
      zzplay_httpfile_tell(f) != (long)RESOURCE_BYTES ||
      !zzplay_httpfile_seek(f, 0L, ZZPLAY_FILE_BEGIN)) {
    rc = 5;
    goto done;
  }
  /* zzplay_probe_media_file. */
  if (zzplay_httpfile_read(f, got, 4096U) != 4096U ||
      memcmp(got, resource, 4096U) != 0) {
    rc = 6;
    goto done;
  }
  if (!zzplay_httpfile_seek(f, 0L, ZZPLAY_FILE_BEGIN) ||
      zzplay_httpfile_read(f, got, 10U) != 10U) {
    rc = 7;
    goto done;
  }
  if (!zzplay_httpfile_seek(f, (long)tag_total, ZZPLAY_FILE_BEGIN) ||
      zzplay_httpfile_read(f, got, 4096U) != 4096U ||
      memcmp(got, resource + tag_total, 4096U) != 0) {
    rc = 8;
    goto done;
  }
  if (!zzplay_httpfile_seek(f, 0L, ZZPLAY_FILE_BEGIN)) { rc = 9; goto done; }
  if (s.opens != 1) { rc = 10; goto done; }
  /* The engine reopens the path: the pool hands back the same file. */
  zzplay_httpfile_close_cached(f);
  f = zzplay_httpfile_open_cached(&ops, "http://h/x", err, sizeof(err));
  if (f == NULL || !zzplay_httpfile_seekable(f)) { rc = 11; goto done; }
  if (!zzplay_httpfile_seek(f, (long)tag_total, ZZPLAY_FILE_BEGIN)) {
    rc = 12;
    goto done;
  }
  n = 0U;
  while (n < sizeof(got)) {
    r = zzplay_httpfile_read(f, got + n, sizeof(got) - n);
    if (r == 0U) break;
    n += r;
  }
  if (n != sizeof(got) || memcmp(got, resource + tag_total, n) != 0) {
    rc = 13;
    goto done;
  }
  if (s.opens != 1) { rc = 14; goto done; }
  /* A real seek still makes a new ranged request. */
  if (!zzplay_httpfile_seek(f, 150000L, ZZPLAY_FILE_BEGIN) ||
      zzplay_httpfile_read(f, got, 200U) != 200U ||
      memcmp(got, resource + 150000U, 200U) != 0) {
    rc = 15;
    goto done;
  }
  if (s.opens != 2) rc = 16;

done:
  zzplay_httpfile_close(f);
  memcpy(resource, orig, sizeof(orig));
  return rc;
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
  rc = test_live_reconnect();
  if (rc != 0) { printf("live_reconnect %d\n", rc); return 130 + rc; }
  rc = test_failure();
  if (rc != 0) { printf("failure %d\n", rc); return 110 + rc; }
  rc = test_open_path_connections();
  if (rc != 0) { printf("open_path_connections %d\n", rc); return 150 + rc; }
  rc = test_faults();
  if (rc != 0) { printf("faults %d\n", rc); return 170 + rc; }
  rc = test_live_past_4gib();
  if (rc != 0) { printf("live_past_4gib %d\n", rc); return 200 + rc; }
  printf("zzplay_httpfile_test: all checks passed\n");
  return 0;
}
