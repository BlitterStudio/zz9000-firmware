/* Task-safe allocation and whole-file I/O for ZZPlay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_SYS_H
#define ZZPLAY_SYS_H

#include <stddef.h>

/* Memory returned by zzplay_alloc() must be released with zzplay_free(). */
void *zzplay_alloc(size_t size);
void zzplay_free(void *ptr);

/* Read a complete file no larger than max_len. A successful read returns an
 * owned buffer (including for an empty file) which must be released with
 * zzplay_free(). */
int zzplay_file_read_all(const char *path, size_t max_len, void **buf,
                         size_t *len);
int zzplay_file_write_all(const char *path, const void *buf, size_t len);

#endif /* ZZPLAY_SYS_H */
