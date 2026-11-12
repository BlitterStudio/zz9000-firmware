/* Task-safe allocation and file I/O for ZZPlay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-sys.h"

#include <string.h>

#if defined(__amigaos__) || defined(AMIGA)

#include <stdio.h>

#include <dos/dos.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include "zzplay-http.h"
#include "zzplay-httpfile.h"
#include "zzplay-net.h"

struct ZZPlayFile {
  BPTR handle;
  ZZPlayHttpFile *http; /* set for a network source instead of handle */
  int error;
  int eof;
};

static int (*network_poll)(void *user);
static void (*network_title)(void *user, const char *text);
static void (*network_stage)(void *user, int stage, const char *host);
static void *network_user;
static char open_error[ZZPLAY_NET_ERROR_MAX];

void zzplay_file_set_network_hooks(int (*poll)(void *user),
                                   void (*title)(void *user,
                                                 const char *text),
                                   void (*stage)(void *user, int stage,
                                                 const char *host),
                                   void *user)
{
  network_poll = poll;
  network_title = title;
  network_stage = stage;
  network_user = user;
}

void zzplay_file_end_network_item(void)
{
  zzplay_httpfile_drop_cached();
}

const char *zzplay_file_open_error(void)
{
  return open_error;
}

static int network_keep_running(void *user)
{
  (void)user;
  return network_poll == 0 || network_poll(network_user);
}

static void network_stage_forward(void *user, ZZPlayNetStage stage,
                                  const char *host)
{
  if (network_stage != NULL) {
    network_stage(user, (int)stage, host);
  }
}

static void *network_open(void *ctx, const char *url, int has_range,
                          uint32_t from, ZZPlayHttpHead *head, char *err,
                          size_t err_size)
{
  ZZPlayNetRequest req;

  (void)ctx;
  memset(&req, 0, sizeof(req));
  req.url = url;
  req.has_range = has_range;
  req.range_from = from;
  req.icy = 1;
  req.keep_running = network_keep_running;
  req.stage = network_stage != NULL ? network_stage_forward : NULL;
  req.user = network_user;
  return zzplay_net_get(&req, head, err, err_size, 0);
}

static long network_read(void *ctx, void *stream, uint8_t *buf, size_t size)
{
  (void)ctx;
  return zzplay_net_read((ZZPlayNet *)stream, buf, size);
}

static const char *network_error(void *ctx, void *stream)
{
  (void)ctx;
  return zzplay_net_error((const ZZPlayNet *)stream);
}

static void network_close(void *ctx, void *stream)
{
  (void)ctx;
  zzplay_net_close((ZZPlayNet *)stream);
}

/* Wait before a live reconnect in 100 ms steps, staying cancellable. */
static int network_pause(void *ctx, uint32_t ms)
{
  (void)ctx;
  while (ms != 0U) {
    uint32_t step = ms < 100U ? ms : 100U;

    if (!network_keep_running(0)) {
      return 0;
    }
    Delay((LONG)((step + 19U) / 20U)); /* 50 ticks per second */
    ms -= step;
  }
  return network_keep_running(0);
}

static const ZZPlayHttpStreamOps network_ops = {
  network_open, network_read, network_error, network_close, network_pause, 0
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
  open_error[0] = '\0';
  if (zzplay_http_is_url(path)) {
    if (!writable) {
      file->http = zzplay_httpfile_open_cached(&network_ops, path, open_error,
                                               sizeof(open_error));
    }
    if (!file->http) {
      zzplay_free(file);
      return 0;
    }
    return file;
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
  if (file->http) {
    zzplay_httpfile_close_cached(file->http);
    ok = 1;
  } else {
    ok = Close(file->handle) != 0;
  }
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
  if (file->http) {
    char title[ZZPLAY_HTTP_TITLE_MAX];

    total = zzplay_httpfile_read(file->http, buf, len);
    if (zzplay_httpfile_error(file->http)) {
      file->error = 1;
    }
    if (zzplay_httpfile_eof(file->http)) {
      file->eof = 1;
    }
    if (network_title != 0 &&
        zzplay_httpfile_take_title(file->http, title, sizeof(title))) {
      network_title(network_user, title);
    }
    return total;
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
  if (file->http) {
    file->error = 1;
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
  if (file->http) {
    if (!zzplay_httpfile_seek(file->http, offset, origin)) {
      return 0;
    }
    file->eof = 0;
    return 1;
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

  if (file && file->http) {
    return zzplay_httpfile_tell(file->http);
  }
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
    if (file->http) {
      zzplay_httpfile_clear_error(file->http);
    }
  }
}

int zzplay_file_seekable(ZZPlayFile *file)
{
  return file && (!file->http || zzplay_httpfile_seekable(file->http));
}

int zzplay_file_is_network(ZZPlayFile *file)
{
  return file && file->http;
}

const char *zzplay_file_station(ZZPlayFile *file)
{
  return file && file->http ? zzplay_httpfile_station(file->http) : "";
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

/* `path` with `suffix` appended. Owned; NULL when out of memory. */
static char *suffixed_path(const char *path, const char *suffix)
{
  size_t plen = strlen(path);
  size_t slen = strlen(suffix);
  char *out = (char *)zzplay_alloc(plen + slen + 1U);

  if (out) {
    memcpy(out, path, plen);
    memcpy(out + plen, suffix, slen + 1U);
  }
  return out;
}

/* ENV: holds only the running session's copy. On AmigaOS 3.2 it is also
 * filled on demand from ENVARC:, so a name moved away there comes back at
 * once and the replace below cannot work; the file is written in place. */
static int path_is_env(const char *path)
{
  return (path[0] == 'E' || path[0] == 'e') &&
         (path[1] == 'N' || path[1] == 'n') &&
         (path[2] == 'V' || path[2] == 'v') && path[3] == ':';
}

static int write_handle(BPTR handle, const void *buf, size_t len)
{
  size_t total = 0U;

  while (total < len) {
    LONG n = Write(handle, (APTR)((const UBYTE *)buf + total),
                   (LONG)(len - total));

    if (n <= 0L) {
      break;
    }
    total += (size_t)n;
  }
  /* A full disk can show only when the buffers are flushed. */
  return Close(handle) != 0 && total == len;
}

/* Moved-aside copies are "<path>.zzo", "<path>.zzo1" ... A copy can stay
 * in use for a while (on AmigaOS 3.2 by ENV:'s link to ENVARC:), so a
 * name that cannot be deleted is skipped, and every save tries to delete
 * the ones left from before. */
#define ZZPLAY_OLD_NAMES 4

/* Put `tmp` in place of `path`. AmigaDOS does not rename over an existing
 * file, so the old copy moves aside first and comes back if the new one
 * cannot take its place. */
static int replace_file(const char *tmp, const char *path)
{
  static const char *const old_suffix[ZZPLAY_OLD_NAMES] = {
    ".zzo", ".zzo1", ".zzo2", ".zzo3"
  };
  char *old = NULL;
  int i;
  int ok = 0;

  for (i = 0; i < ZZPLAY_OLD_NAMES; i++) {
    char *name = suffixed_path(path, old_suffix[i]);
    BPTR lock;

    if (!name) {
      break;
    }
    (void)DeleteFile((CONST_STRPTR)name);
    lock = Lock((CONST_STRPTR)name, SHARED_LOCK);
    if (lock) {
      UnLock(lock);
      zzplay_free(name);
    } else if (!old) {
      old = name;
    } else {
      zzplay_free(name);
    }
  }
  if (Rename((CONST_STRPTR)tmp, (CONST_STRPTR)path)) {
    ok = 1;
  } else if (old && Rename((CONST_STRPTR)path, (CONST_STRPTR)old)) {
    if (Rename((CONST_STRPTR)tmp, (CONST_STRPTR)path)) {
      (void)DeleteFile((CONST_STRPTR)old);
      ok = 1;
    } else {
      (void)Rename((CONST_STRPTR)old, (CONST_STRPTR)path);
    }
  }
  zzplay_free(old);
  return ok;
}

int zzplay_file_write_all(const char *path, const void *buf, size_t len)
{
  char *tmp;
  BPTR handle;
  int ok = 0;

  if (!path || (!buf && len != 0U) || len > 0x7fffffffUL ||
      zzplay_http_is_url(path)) {
    return 0;
  }
  if (path_is_env(path)) {
    handle = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (handle) {
      ok = write_handle(handle, buf, len);
    }
    if (!ok) {
      /* Without the session copy, reads fall back to ENVARC:. */
      (void)DeleteFile((CONST_STRPTR)path);
    }
    return ok;
  }
  tmp = suffixed_path(path, ".zzn");
  if (tmp) {
    handle = Open((CONST_STRPTR)tmp, MODE_NEWFILE);
    if (handle) {
      ok = write_handle(handle, buf, len) && replace_file(tmp, path);
      if (!ok) {
        (void)DeleteFile((CONST_STRPTR)tmp);
      }
    }
  }
  zzplay_free(tmp);
  return ok;
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

int zzplay_file_seekable(ZZPlayFile *file)
{
  return file != 0;
}

int zzplay_file_is_network(ZZPlayFile *file)
{
  (void)file;
  return 0;
}

const char *zzplay_file_station(ZZPlayFile *file)
{
  (void)file;
  return "";
}

const char *zzplay_file_open_error(void)
{
  return "";
}

void zzplay_file_set_network_hooks(int (*poll)(void *user),
                                   void (*title)(void *user,
                                                 const char *text),
                                   void (*stage)(void *user, int stage,
                                                 const char *host),
                                   void *user)
{
  (void)poll;
  (void)title;
  (void)stage;
  (void)user;
}

void zzplay_file_end_network_item(void)
{
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

/* Write beside the target, then rename over it: the old file stays until
 * the new one is complete. */
int zzplay_file_write_all(const char *path, const void *buf, size_t len)
{
  char *tmp;
  FILE *file;
  size_t written;
  int ok;

  if (!path || (!buf && len != 0U)) {
    return 0;
  }
  tmp = (char *)malloc(strlen(path) + 5U);
  if (!tmp) {
    return 0;
  }
  memcpy(tmp, path, strlen(path));
  memcpy(tmp + strlen(path), ".new", 5U);
  file = fopen(tmp, "wb");
  if (!file) {
    free(tmp);
    return 0;
  }
  written = len != 0U ? fwrite(buf, 1U, len, file) : 0U;
  ok = fclose(file) == 0 && written == len;
  if (ok && rename(tmp, path) != 0) {
    /* Where rename cannot replace a file. */
    ok = remove(path) == 0 && rename(tmp, path) == 0;
  }
  if (!ok) {
    (void)remove(tmp);
  }
  free(tmp);
  return ok;
}

#endif
