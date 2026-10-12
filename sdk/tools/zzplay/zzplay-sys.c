/* Task-safe allocation and whole-file I/O for ZZPlay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-sys.h"

#if defined(__amigaos__) || defined(AMIGA)

#include <dos/dos.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>

void *zzplay_alloc(size_t size)
{
  if (size > (size_t)~(ULONG)0) {
    return 0;
  }
  return AllocVec((ULONG)size, MEMF_PUBLIC | MEMF_CLEAR);
}

void zzplay_free(void *ptr)
{
  if (ptr) {
    FreeVec(ptr);
  }
}

int zzplay_file_read_all(const char *path, size_t max_len, void **buf,
                         size_t *len)
{
  BPTR file;
  LONG file_len;
  void *data;

  if (!path || !buf || !len) {
    return 0;
  }
  *buf = 0;
  *len = 0U;

  file = Open((CONST_STRPTR)path, MODE_OLDFILE);
  if (!file) {
    return 0;
  }
  if (Seek(file, 0L, OFFSET_END) < 0L ||
      (file_len = Seek(file, 0L, OFFSET_BEGINNING)) < 0L ||
      (size_t)file_len > max_len) {
    Close(file);
    return 0;
  }

  data = zzplay_alloc((size_t)file_len + 1U);
  if (!data || (file_len > 0L && Read(file, data, file_len) != file_len)) {
    zzplay_free(data);
    Close(file);
    return 0;
  }
  Close(file);
  *buf = data;
  *len = (size_t)file_len;
  return 1;
}

int zzplay_file_write_all(const char *path, const void *buf, size_t len)
{
  BPTR file;
  LONG written;

  if (!path || (!buf && len != 0U) || len > (size_t)~(ULONG)0) {
    return 0;
  }
  file = Open((CONST_STRPTR)path, MODE_NEWFILE);
  if (!file) {
    return 0;
  }
  written = len == 0U ? 0L : Write(file, (APTR)buf, (LONG)len);
  Close(file);
  return written == (LONG)len;
}

#else

#include <stdio.h>
#include <stdlib.h>

void *zzplay_alloc(size_t size)
{
  return calloc(1U, size);
}

void zzplay_free(void *ptr)
{
  free(ptr);
}

int zzplay_file_read_all(const char *path, size_t max_len, void **buf,
                         size_t *len)
{
  FILE *file;
  long file_len;
  void *data;

  if (!path || !buf || !len) {
    return 0;
  }
  *buf = 0;
  *len = 0U;

  file = fopen(path, "rb");
  if (!file) {
    return 0;
  }
  if (fseek(file, 0L, SEEK_END) != 0 ||
      (file_len = ftell(file)) < 0L || (size_t)file_len > max_len ||
      fseek(file, 0L, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }

  data = zzplay_alloc((size_t)file_len + 1U);
  if (!data || (file_len > 0L &&
                fread(data, 1U, (size_t)file_len, file) != (size_t)file_len)) {
    zzplay_free(data);
    fclose(file);
    return 0;
  }
  fclose(file);
  *buf = data;
  *len = (size_t)file_len;
  return 1;
}

int zzplay_file_write_all(const char *path, const void *buf, size_t len)
{
  FILE *file;
  size_t written;

  if (!path || (!buf && len != 0U)) {
    return 0;
  }
  file = fopen(path, "wb");
  if (!file) {
    return 0;
  }
  written = len == 0U ? 0U : fwrite(buf, 1U, len, file);
  if (fclose(file) != 0) {
    return 0;
  }
  return written == len;
}

#endif
