/* Task-safe allocation and file I/O for ZZPlay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_SYS_H
#define ZZPLAY_SYS_H

#include <stddef.h>

/* Memory returned by zzplay_alloc() must be released with zzplay_free(). */
void *zzplay_alloc(size_t size);
void zzplay_free(void *ptr);

#if defined(__amigaos__) || defined(AMIGA)
typedef struct ZZPlayFile ZZPlayFile;
#else
#include <stdio.h>
typedef FILE ZZPlayFile;
#endif

typedef enum ZZPlayFileOrigin {
  ZZPLAY_FILE_BEGIN,
  ZZPLAY_FILE_CURRENT,
  ZZPLAY_FILE_END
} ZZPlayFileOrigin;

/* Open an AmigaDOS file for reading or replace it for writing. On AmigaOS
 * an http:// or https:// path opens the network resource for reading. */
ZZPlayFile *zzplay_file_open(const char *path, int writable);
int zzplay_file_close(ZZPlayFile *file);
size_t zzplay_file_read(ZZPlayFile *file, void *buf, size_t len);
size_t zzplay_file_write(ZZPlayFile *file, const void *buf, size_t len);
int zzplay_file_seek(ZZPlayFile *file, long offset, ZZPlayFileOrigin origin);
long zzplay_file_tell(ZZPlayFile *file);
int zzplay_file_error(ZZPlayFile *file);
int zzplay_file_eof(ZZPlayFile *file);
void zzplay_file_clear_error(ZZPlayFile *file);

/* 1 when any position can be reached: always for files; for a network
 * source only when the server names the length and accepts byte ranges. */
int zzplay_file_seekable(ZZPlayFile *file);
/* 1 for a network source. */
int zzplay_file_is_network(ZZPlayFile *file);
/* The station name a live network stream announced, or "". */
const char *zzplay_file_station(ZZPlayFile *file);
/* The address a network file came from after redirects, or "" for a
 * local file. */
const char *zzplay_file_url(ZZPlayFile *file);

/* Why the last zzplay_file_open() of a network path failed, for the
 * message line ("" after a file failure). The application opens network
 * paths from one process only, so one copy suffices. */
const char *zzplay_file_open_error(void);

/* Network waits call `poll` at least every 100 ms so the player stays
 * live; returning 0 cancels the transfer. `title` receives each new
 * StreamTitle a live stream announces while it is read. Either may be
 * NULL. */
void zzplay_file_set_network_hooks(int (*poll)(void *user),
                                   void (*title)(void *user,
                                                 const char *text),
                                   void (*stage)(void *user, int stage,
                                                 const char *host),
                                   void *user);

/* A network file closed by the probe stays open for the engine's reopen of
 * the same path. Call this when the item ends and before exit so that
 * connection does not outlive it. */
void zzplay_file_end_network_item(void);

/* Read a complete file no larger than max_len. A successful read returns an
 * owned buffer (including for an empty file) which must be released with
 * zzplay_free(). */
int zzplay_file_read_all(const char *path, size_t max_len, void **buf,
                         size_t *len);
/* Replace a file with `len` bytes: they go to a new file beside it first,
 * which takes the old one's place only once completely written and
 * closed. On failure the old file is left as it was. An ENV: file is the
 * session copy and is written in place (on AmigaOS 3.2 ENV: refills
 * itself from ENVARC:, so a moved-away name comes straight back); when
 * that fails it is deleted so reads fall back to ENVARC:. Write ENVARC:
 * before ENV:. */
int zzplay_file_write_all(const char *path, const void *buf, size_t len);

#endif /* ZZPLAY_SYS_H */
