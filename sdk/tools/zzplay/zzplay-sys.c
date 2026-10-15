/* Task-safe allocation and file I/O for ZZPlay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-sys.h"

#if defined(__amigaos__) || defined(AMIGA)

#include <dos/dos.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>


struct ZZPlayFile {
  BPTR handle;
  int error;
  int eof;
};

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

ZZPlayFile *zzplay_file_open(const char *path, int writable)
{
  ZZPlayFile *file;
  BPTR handle;

  if (!path) {
    return 0;
  }
  /* Allocate first: MODE_NEWFILE truncates the file, so a later
   * allocation failure would lose saved preferences or a playlist. */
  file = (ZZPlayFile *)zzplay_alloc(sizeof(*file));
  if (!file) {
    return 0;
  }
  handle = Open((CONST_STRPTR)path, writable ? MODE_NEWFILE : MODE_OLDFILE);
  if (!handle) {
    zzplay_free(file);
    return 0;
  }
  file->handle = handle;
  return file;
}

int zzplay_file_close(ZZPlayFile *file)
{
  int ok;

  if (!file) {
    return 0;
  }
  ok = Close(file->handle) != 0;
  zzplay_free(file);
  return ok;
}

size_t zzplay_file_read(ZZPlayFile *file, void *buf, size_t len)
{
  size_t total = 0U;

  if (!file || (!buf && len != 0U) || len > 0x7fffffffUL) {
    if (file) {
      file->error = 1;
    }
    return 0U;
  }
  while (total < len) {
    LONG got = Read(file->handle, (UBYTE *)buf + total,
                    (LONG)(len - total));

    if (got < 0L) {
      file->error = 1;
      break;
    }
    if (got == 0L) {
      file->eof = 1;
      break;
    }
    total += (size_t)got;
  }
  return total;
}

size_t zzplay_file_write(ZZPlayFile *file, const void *buf, size_t len)
{
  size_t total = 0U;

  if (!file || (!buf && len != 0U) || len > 0x7fffffffUL) {
    if (file) {
      file->error = 1;
    }
    return 0U;
  }
  while (total < len) {
    LONG written = Write(file->handle, (APTR)((const UBYTE *)buf + total),
                         (LONG)(len - total));

    if (written < 0L) {
      file->error = 1;
      break;
    }
    if (written == 0L) {
      file->error = 1;
      break;
    }
    total += (size_t)written;
  }
  return total;
}

int zzplay_file_seek(ZZPlayFile *file, long offset, ZZPlayFileOrigin origin)
{
  LONG mode;

  if (!file) {
    return 0;
  }
  mode = origin == ZZPLAY_FILE_BEGIN ? OFFSET_BEGINNING
       : origin == ZZPLAY_FILE_CURRENT ? OFFSET_CURRENT
                                       : OFFSET_END;
  /* AmigaDOS Seek() reports the previous position, not the new one. */
  if (Seek(file->handle, (LONG)offset, mode) < 0L) {
    file->error = 1;
    return 0;
  }
  file->eof = 0;
  return 1;
}

long zzplay_file_tell(ZZPlayFile *file)
{
  LONG pos;

  if (!file || (pos = Seek(file->handle, 0L, OFFSET_CURRENT)) < 0L) {
    if (file) {
      file->error = 1;
    }
    return -1L;
  }
  return (long)pos;
}

int zzplay_file_error(ZZPlayFile *file)
{
  return file && file->error;
}

int zzplay_file_eof(ZZPlayFile *file)
{
  return file && file->eof;
}

void zzplay_file_clear_error(ZZPlayFile *file)
{
  if (file) {
    file->error = 0;
  }
}


int zzplay_file_read_all(const char *path, size_t max_len, void **buf,
                         size_t *len)
{
  ZZPlayFile *file;
  long file_len;
  void *data;

  if (!path || !buf || !len) {
    return 0;
  }
  *buf = 0;
  *len = 0U;

  file = zzplay_file_open(path, 0);
  if (!file || !zzplay_file_seek(file, 0L, ZZPLAY_FILE_END) ||
      (file_len = zzplay_file_tell(file)) < 0L ||
      (size_t)file_len > max_len ||
      !zzplay_file_seek(file, 0L, ZZPLAY_FILE_BEGIN)) {
    zzplay_file_close(file);
    return 0;
  }

  data = zzplay_alloc((size_t)file_len + 1U);
  if (!data || (file_len > 0L &&
                zzplay_file_read(file, data, (size_t)file_len) !=
                    (size_t)file_len)) {
    zzplay_free(data);
    zzplay_file_close(file);
    return 0;
  }
  zzplay_file_close(file);
  *buf = data;
  *len = (size_t)file_len;
  return 1;
}

int zzplay_file_write_all(const char *path, const void *buf, size_t len)
{
  ZZPlayFile *file;
  size_t written;

  if (!path || (!buf && len != 0U) || len > 0x7fffffffUL) {
    return 0;
  }
  file = zzplay_file_open(path, 1);
  if (!file) {
    return 0;
  }
  written = zzplay_file_write(file, buf, len);
  zzplay_file_close(file);
  return written == len;
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

ZZPlayFile *zzplay_file_open(const char *path, int writable)
{
  return path ? fopen(path, writable ? "wb" : "rb") : 0;
}

int zzplay_file_close(ZZPlayFile *file)
{
  return file && fclose(file) == 0;
}

size_t zzplay_file_read(ZZPlayFile *file, void *buf, size_t len)
{
  return file && (buf || len == 0U) ? fread(buf, 1U, len, file) : 0U;
}

size_t zzplay_file_write(ZZPlayFile *file, const void *buf, size_t len)
{
  return file && (buf || len == 0U) ? fwrite(buf, 1U, len, file) : 0U;
}

int zzplay_file_seek(ZZPlayFile *file, long offset, ZZPlayFileOrigin origin)
{
  int whence = origin == ZZPLAY_FILE_BEGIN ? SEEK_SET
             : origin == ZZPLAY_FILE_CURRENT ? SEEK_CUR
                                             : SEEK_END;

  return file && fseek(file, offset, whence) == 0;
}

long zzplay_file_tell(ZZPlayFile *file)
{
  return file ? ftell(file) : -1L;
}

int zzplay_file_error(ZZPlayFile *file)
{
  return file && ferror(file);
}

int zzplay_file_eof(ZZPlayFile *file)
{
  return file && feof(file);
}

void zzplay_file_clear_error(ZZPlayFile *file)
{
  if (file) {
    clearerr(file);
  }
}


int zzplay_file_read_all(const char *path, size_t max_len, void **buf,
                         size_t *len)
{
  ZZPlayFile *file;
  long file_len;
  void *data;

  if (!path || !buf || !len) {
    return 0;
  }
  *buf = 0;
  *len = 0U;

  file = zzplay_file_open(path, 0);
  if (!file || !zzplay_file_seek(file, 0L, ZZPLAY_FILE_END) ||
      (file_len = zzplay_file_tell(file)) < 0L ||
      (size_t)file_len > max_len ||
      !zzplay_file_seek(file, 0L, ZZPLAY_FILE_BEGIN)) {
    zzplay_file_close(file);
    return 0;
  }

  data = zzplay_alloc((size_t)file_len + 1U);
  if (!data || (file_len > 0L &&
                zzplay_file_read(file, data, (size_t)file_len) !=
                    (size_t)file_len)) {
    zzplay_free(data);
    zzplay_file_close(file);
    return 0;
  }
  zzplay_file_close(file);
  *buf = data;
  *len = (size_t)file_len;
  return 1;
}

int zzplay_file_write_all(const char *path, const void *buf, size_t len)
{
  ZZPlayFile *file;
  size_t written;

  if (!path || (!buf && len != 0U)) {
    return 0;
  }
  file = zzplay_file_open(path, 1);
  if (!file) {
    return 0;
  }
  written = zzplay_file_write(file, buf, len);
  if (!zzplay_file_close(file)) {
    return 0;
  }
  return written == len;
}

#endif
