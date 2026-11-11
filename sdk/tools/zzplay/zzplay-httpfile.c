/* A network resource read like a file. SPDX-License-Identifier:
 * GPL-3.0-or-later */

#include "zzplay-httpfile.h"

#include <stdio.h>
#include <string.h>

/* Bytes kept from the start of the resource: enough for every probe and
 * tag read that rewinds (an MP3 probe reads 4 KiB after an ID3 tag). */
#define HTTPFILE_HEAD_BYTES (32U * 1024U)
/* Read-ahead window: the most recent block read from the connection. */
#define HTTPFILE_WINDOW_BYTES (16U * 1024U)
/* Forward seeks shorter than this read through on the open connection
 * instead of starting a new ranged request. */
#define HTTPFILE_READ_THROUGH (64U * 1024U)
#define HTTPFILE_ERROR_MAX 192U

struct ZZPlayHttpFile {
  ZZPlayHttpStreamOps ops;
  void *stream;
  char url[ZZPLAY_HTTP_URL_MAX];

  uint32_t pos;       /* next byte the reader asks for */
  uint32_t conn_pos;  /* next byte the open stream delivers */
  int seekable;
  int live;
  int has_total;
  uint32_t total;
  int error;
  int eof;
  int stream_ended;   /* the open stream reached the end of its body */
  uint32_t drops;     /* live reconnects since the last audio arrived */
  char err[HTTPFILE_ERROR_MAX];
  char station[ZZPLAY_HTTP_NAME_MAX];

  int icy_active;
  ZZPlayIcy icy;
  char title[ZZPLAY_HTTP_TITLE_MAX];
  int title_pending;

  uint32_t head_len;
  uint32_t win_start;
  uint32_t win_len;
  uint8_t head[HTTPFILE_HEAD_BYTES];
  uint8_t win[HTTPFILE_WINDOW_BYTES];
};

static void set_error(ZZPlayHttpFile *f, const char *text)
{
  f->error = 1;
  (void)snprintf(f->err, sizeof(f->err), "%s", text != NULL ? text : "");
}

static void close_stream(ZZPlayHttpFile *f)
{
  if (f->stream != NULL) {
    f->ops.close(f->ops.ctx, f->stream);
    f->stream = NULL;
  }
}

/* Learn what the response allows. `from` is the offset that was asked. */
static void take_head(ZZPlayHttpFile *f, const ZZPlayHttpHead *head)
{
  if (head->icy || head->icy_metaint != 0U || head->icy_name[0] != '\0') {
    f->live = 1;
    f->seekable = 0;
    f->has_total = 0;
    (void)snprintf(f->station, sizeof(f->station), "%s", head->icy_name);
  } else if (head->status == 206 && head->has_total) {
    f->seekable = 1;
    f->has_total = 1;
    f->total = head->total;
  } else if (head->status == 200 && head->has_length && !head->chunked) {
    f->has_total = 1;
    f->total = head->content_length;
    f->seekable = head->accept_ranges;
  } else {
    f->seekable = 0;
  }
  if (head->icy_metaint != 0U) {
    f->icy_active = 1;
    zzplay_icy_init(&f->icy, head->icy_metaint);
  } else {
    f->icy_active = 0;
  }
}

/* Open the stream at `from`. A server that ignores the range answers 200
 * from the start; reading through then reaches `from`. */
static int open_at(ZZPlayHttpFile *f, uint32_t from, int first)
{
  ZZPlayHttpHead head;
  char err[HTTPFILE_ERROR_MAX];

  close_stream(f);
  f->stream = f->ops.open(f->ops.ctx, f->url, 1, from, &head, err,
                          sizeof(err));
  if (f->stream == NULL && head.status == 416 && from == 0U) {
    /* Some servers refuse any Range on a resource they cannot range,
     * even bytes=0-. */
    f->stream = f->ops.open(f->ops.ctx, f->url, 0, 0U, &head, err,
                            sizeof(err));
  }
  if (f->stream == NULL) {
    set_error(f, err);
    return 0;
  }
  f->stream_ended = 0;
  if (first) {
    take_head(f, &head);
  } else if (head.icy_metaint != 0U) {
    zzplay_icy_init(&f->icy, head.icy_metaint);
  }
  if (head.status == 206 && head.has_range) {
    f->conn_pos = head.range_start;
  } else {
    f->conn_pos = 0U;
    if (from != 0U) {
      /* The server ignored the range: further seeks would cost a full
       * read-through each time. */
      f->seekable = 0;
    }
  }
  if (f->conn_pos > from) {
    close_stream(f);
    set_error(f, "The server answered a different byte range");
    return 0;
  }
  return 1;
}

/* Read the next block from the stream into the window. Returns the bytes
 * now in the window, 0 at the end of the body, -1 on failure. */
static long fill_window(ZZPlayHttpFile *f)
{
  for (;;) {
    long n;
    uint32_t got;

    if (f->stream_ended) {
      return 0;
    }
    n = f->ops.read(f->ops.ctx, f->stream, f->win, sizeof(f->win));
    if (n < 0) {
      set_error(f, f->ops.error(f->ops.ctx, f->stream));
      return -1;
    }
    if (n == 0) {
      f->stream_ended = 1;
      return 0;
    }
    got = (uint32_t)n;
    if (f->icy_active) {
      got = (uint32_t)zzplay_icy_strip(&f->icy, f->win, got);
      if (f->icy.title_changed) {
        f->icy.title_changed = 0;
        memcpy(f->title, f->icy.title, sizeof(f->title));
        f->title_pending = 1;
      }
      if (got == 0U) {
        continue; /* the block held only metadata */
      }
    }
    f->win_start = f->conn_pos;
    f->win_len = got;
    f->conn_pos += got;
    /* Keep the start of the resource, while the stream is still in it. */
    if (f->win_start == f->head_len && f->head_len < sizeof(f->head)) {
      uint32_t keep = (uint32_t)sizeof(f->head) - f->head_len;

      if (keep > got) {
        keep = got;
      }
      memcpy(f->head + f->head_len, f->win, keep);
      f->head_len += keep;
    }
    f->drops = 0;
    return (long)got;
  }
}

/* A live stream dropped: open it again and let its audio continue at the
 * current position. Three drops or failed opens in a row without any
 * audio in between end the stream, and so does a pause that reports the
 * user stopped the item. */
static int reconnect_live(ZZPlayHttpFile *f)
{
  static const uint32_t pause_ms[3] = { 1000U, 2000U, 4000U };
  char reason[HTTPFILE_ERROR_MAX];

  (void)snprintf(reason, sizeof(reason), "%s",
                 f->error ? f->err : "The stream ended");
  close_stream(f);
  while (f->drops < 3U) {
    ZZPlayHttpHead head;

    if (f->ops.pause != NULL &&
        !f->ops.pause(f->ops.ctx, pause_ms[f->drops])) {
      set_error(f, "Cancelled");
      return 0;
    }
    f->drops++;
    f->stream = f->ops.open(f->ops.ctx, f->url, 0, 0U, &head, reason,
                            sizeof(reason));
    if (f->stream != NULL) {
      f->error = 0;
      f->err[0] = '\0';
      f->stream_ended = 0;
      f->icy_active = head.icy_metaint != 0U;
      if (f->icy_active) {
        zzplay_icy_init(&f->icy, head.icy_metaint);
      }
      return 1;
    }
  }
  set_error(f, reason);
  return 0;
}

ZZPlayHttpFile *zzplay_httpfile_open(const ZZPlayHttpStreamOps *ops,
                                     const char *url, char *err,
                                     size_t err_size)
{
  ZZPlayHttpFile *f;

  if (err != NULL && err_size != 0U) {
    err[0] = '\0';
  }
  if (ops == NULL || url == NULL || strlen(url) >= ZZPLAY_HTTP_URL_MAX) {
    if (err != NULL) {
      (void)snprintf(err, err_size, "%s", "The address is too long");
    }
    return NULL;
  }
  f = (ZZPlayHttpFile *)zzplay_alloc(sizeof(*f));
  if (f == NULL) {
    if (err != NULL) {
      (void)snprintf(err, err_size, "%s", "Out of memory");
    }
    return NULL;
  }
  memset(f, 0, sizeof(*f));
  f->ops = *ops;
  memcpy(f->url, url, strlen(url) + 1U);
  if (!open_at(f, 0U, 1)) {
    if (err != NULL) {
      (void)snprintf(err, err_size, "%s", f->err);
    }
    zzplay_httpfile_close(f);
    return NULL;
  }
  return f;
}

void zzplay_httpfile_close(ZZPlayHttpFile *file)
{
  if (file == NULL) {
    return;
  }
  close_stream(file);
  zzplay_free(file);
}

/* Copy cached bytes at file->pos. Returns how many were copied. */
static size_t copy_cached(ZZPlayHttpFile *f, uint8_t *out, size_t want)
{
  const uint8_t *src = NULL;
  uint32_t avail = 0U;

  if (f->pos < f->head_len) {
    src = f->head + f->pos;
    avail = f->head_len - f->pos;
  } else if (f->pos >= f->win_start && f->pos - f->win_start < f->win_len) {
    src = f->win + (f->pos - f->win_start);
    avail = f->win_len - (f->pos - f->win_start);
  }
  if (src == NULL) {
    return 0U;
  }
  if (want > avail) {
    want = avail;
  }
  memcpy(out, src, want);
  f->pos += (uint32_t)want;
  return want;
}

size_t zzplay_httpfile_read(ZZPlayHttpFile *file, void *buf, size_t len)
{
  uint8_t *out = (uint8_t *)buf;
  size_t total = 0U;

  if (file == NULL || (buf == NULL && len != 0U)) {
    return 0U;
  }
  while (total < len) {
    size_t got = copy_cached(file, out + total, len - total);
    long n;

    if (got != 0U) {
      total += got;
      continue;
    }
    if (file->has_total && file->pos >= file->total) {
      file->eof = 1;
      break;
    }
    /* The bytes at pos are not cached: make the stream deliver them. */
    if (file->stream == NULL || file->pos < file->conn_pos ||
        (file->seekable &&
         file->pos - file->conn_pos > HTTPFILE_READ_THROUGH)) {
      if (!file->seekable) {
        set_error(file, "Cannot go back in a stream that has no byte ranges");
        break;
      }
      if (!open_at(file, file->pos, 0)) {
        break;
      }
    }
    n = fill_window(file);
    if (n <= 0 && file->live) {
      if (reconnect_live(file)) {
        continue;
      }
      break;
    }
    if (n < 0) {
      break;
    }
    if (n == 0) {
      if (file->has_total && file->conn_pos < file->total) {
        set_error(file, "The connection closed before the end of the data");
      } else {
        file->eof = 1;
      }
      break;
    }
  }
  return total;
}

int zzplay_httpfile_seek(ZZPlayHttpFile *file, long offset,
                         ZZPlayFileOrigin origin)
{
  long base;
  long target;

  if (file == NULL) {
    return 0;
  }
  if (origin == ZZPLAY_FILE_BEGIN) {
    base = 0L;
  } else if (origin == ZZPLAY_FILE_CURRENT) {
    base = (long)file->pos;
  } else {
    if (!file->has_total) {
      return 0;
    }
    base = (long)file->total;
  }
  if ((offset < 0L && -offset > base) ||
      (offset > 0L && offset > 0x7fffffffL - base)) {
    return 0;
  }
  target = base + offset;
  if (!file->seekable && (uint32_t)target < file->conn_pos &&
      (uint32_t)target >= file->head_len &&
      !((uint32_t)target >= file->win_start &&
        (uint32_t)target - file->win_start < file->win_len)) {
    /* Neither cached nor ahead of the stream. */
    return 0;
  }
  file->pos = (uint32_t)target;
  file->eof = 0;
  return 1;
}

long zzplay_httpfile_tell(const ZZPlayHttpFile *file)
{
  return file != NULL ? (long)file->pos : -1L;
}

int zzplay_httpfile_error(const ZZPlayHttpFile *file)
{
  return file != NULL && file->error;
}

int zzplay_httpfile_eof(const ZZPlayHttpFile *file)
{
  return file != NULL && file->eof;
}

void zzplay_httpfile_clear_error(ZZPlayHttpFile *file)
{
  if (file != NULL) {
    file->error = 0;
  }
}

int zzplay_httpfile_seekable(const ZZPlayHttpFile *file)
{
  return file != NULL && file->seekable;
}

int zzplay_httpfile_live(const ZZPlayHttpFile *file)
{
  return file != NULL && file->live;
}

const char *zzplay_httpfile_station(const ZZPlayHttpFile *file)
{
  return file != NULL ? file->station : "";
}

const char *zzplay_httpfile_error_text(const ZZPlayHttpFile *file)
{
  return file != NULL ? file->err : "";
}

int zzplay_httpfile_take_title(ZZPlayHttpFile *file, char *out, size_t size)
{
  if (file == NULL || !file->title_pending || out == NULL || size == 0U) {
    return 0;
  }
  file->title_pending = 0;
  (void)snprintf(out, size, "%s", file->title);
  return 1;
}
