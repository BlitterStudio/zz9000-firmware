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

/* Open an AmigaDOS file for reading or replace it for writing. */
ZZPlayFile *zzplay_file_open(const char *path, int writable);
int zzplay_file_close(ZZPlayFile *file);
size_t zzplay_file_read(ZZPlayFile *file, void *buf, size_t len);
size_t zzplay_file_write(ZZPlayFile *file, const void *buf, size_t len);
int zzplay_file_seek(ZZPlayFile *file, long offset, ZZPlayFileOrigin origin);
long zzplay_file_tell(ZZPlayFile *file);
int zzplay_file_error(ZZPlayFile *file);
int zzplay_file_eof(ZZPlayFile *file);
void zzplay_file_clear_error(ZZPlayFile *file);


/* Read a complete file no larger than max_len. A successful read returns an
 * owned buffer (including for an empty file) which must be released with
 * zzplay_free(). */
int zzplay_file_read_all(const char *path, size_t max_len, void **buf,
                         size_t *len);
int zzplay_file_write_all(const char *path, const void *buf, size_t len);

#endif /* ZZPLAY_SYS_H */
