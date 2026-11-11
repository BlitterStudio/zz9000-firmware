/* A network resource read like a file: the HTTP backend behind
 * ZZPlayFile for http:// and https:// paths.
 *
 * Reads go through a read-ahead window. A seek only records the target;
 * the next read outside the cached bytes either reads through to it on
 * the open connection (short forward seeks, or any forward seek on a
 * stream) or, on a resource that supports byte ranges, opens a new
 * ranged request there. The first bytes of the resource stay cached, so
 * a probe can rewind to the start even on a live stream.
 *
 * Shoutcast/Icecast metadata is removed before any reader sees the bytes;
 * the latest StreamTitle is kept for display. A live stream that drops is
 * reconnected, up to three times in a row with growing pauses, and the
 * reader sees the new connection's audio continue at the same position.
 *
 * The transport is passed in, so the host tests drive this with a
 * scripted server. SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_HTTPFILE_H
#define ZZPLAY_HTTPFILE_H

#include <stddef.h>
#include <stdint.h>

#include "zzplay-http.h"
#include "zzplay-sys.h"

typedef struct ZZPlayHttpStreamOps {
  /* Start a GET of `url`, asking for "bytes=from-" when has_range is set
   * and for Shoutcast metadata always. On success returns a stream
   * positioned at the body and fills `head`; a status outside 200-299 is
   * a failure. On failure returns NULL, fills `err` and fills `head` when
   * a response arrived. */
  void *(*open)(void *ctx, const char *url, int has_range, uint32_t from,
                ZZPlayHttpHead *head, char *err, size_t err_size);
  /* Body bytes: the count, 0 at the end of the body, -1 on failure. */
  long (*read)(void *ctx, void *stream, uint8_t *buf, size_t size);
  /* The reason for the last -1 from read(). */
  const char *(*error)(void *ctx, void *stream);
  void (*close)(void *ctx, void *stream);
  /* Wait `ms` before a reconnect attempt; return 0 to give up (the user
   * stopped the item). */
  int (*pause)(void *ctx, uint32_t ms);
  void *ctx;
} ZZPlayHttpStreamOps;

typedef struct ZZPlayHttpFile ZZPlayHttpFile;

/* Open `url` through `ops`. Returns NULL with a reason in `err` when the
 * resource cannot be had. */
ZZPlayHttpFile *zzplay_httpfile_open(const ZZPlayHttpStreamOps *ops,
                                     const char *url, char *err,
                                     size_t err_size);
void zzplay_httpfile_close(ZZPlayHttpFile *file);

/* Same contracts as the zzplay_file_* calls of the same names. */
size_t zzplay_httpfile_read(ZZPlayHttpFile *file, void *buf, size_t len);
int zzplay_httpfile_seek(ZZPlayHttpFile *file, long offset,
                         ZZPlayFileOrigin origin);
long zzplay_httpfile_tell(const ZZPlayHttpFile *file);
int zzplay_httpfile_error(const ZZPlayHttpFile *file);
int zzplay_httpfile_eof(const ZZPlayHttpFile *file);
void zzplay_httpfile_clear_error(ZZPlayHttpFile *file);

/* Seeks anywhere work: the server named the full length and accepts byte
 * ranges. Otherwise only the cached start of the resource and forward
 * positions can be reached. */
int zzplay_httpfile_seekable(const ZZPlayHttpFile *file);
/* A Shoutcast/Icecast stream: endless, and reconnecting is meaningful. */
int zzplay_httpfile_live(const ZZPlayHttpFile *file);
/* The station name a live stream announced, or "". */
const char *zzplay_httpfile_station(const ZZPlayHttpFile *file);
/* The reason for the last read or seek failure, or "". */
const char *zzplay_httpfile_error_text(const ZZPlayHttpFile *file);

/* The latest StreamTitle. Returns 1 once per change, copying the title
 * into `out`. */
int zzplay_httpfile_take_title(ZZPlayHttpFile *file, char *out, size_t size);

#endif /* ZZPLAY_HTTPFILE_H */
