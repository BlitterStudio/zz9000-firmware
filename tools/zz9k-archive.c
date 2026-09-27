/*
 * ZZ9000 SDK archive frontend.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/batch.h"
#include "zz9k/caps.h"
#include "zz9k/compression.h"
#include "zz9k/host.h"
#include "zz9k/shared.h"
#include "zz9k/text.h"
#include "lha-unix/zz9k_lha_unix.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#include <sys/stat.h>
#elif defined(__amigaos__)
#include <proto/dos.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

/* Ctrl-C checkpoint (Amiga): observes AND consumes SIGBREAKF_CTRL_C, the
   standard Workbench break signal, and LATCHES it -- CheckSignal is
   one-shot, so without a latch the first checkpoint to see the break
   (say, the board-decode abort guard) would consume it and the walk
   would keep extracting. Long-running phases call this between work
   units; once latched, every checkpoint reports cancelled until the run
   unwinds. zz9k_archive_run clears the latch for each invocation. */
static int zz9k_archive_cancel_latched;

static int zz9k_archive_cancelled(void)
{
#if defined(__amigaos__)
  if (!zz9k_archive_cancel_latched &&
      CheckSignal(SIGBREAKF_CTRL_C) != 0L) {
    printf("\ninterrupted\n");
    zz9k_archive_cancel_latched = 1;
  }
#endif
  return zz9k_archive_cancel_latched;
}

/* Every armed mailbox round trip (alloc, free, query, decode) can wake
   on Ctrl-C and return CANCELLED -- and Wait() has already consumed the
   break signal, so CheckSignal-based checkpoints never see those
   presses. Any status observation that sees CANCELLED latches the run's
   cancellation here. */
static void zz9k_archive_note_status(int status)
{
  if (status == ZZ9K_STATUS_CANCELLED) {
    printf("\ninterrupted\n");
    zz9k_archive_cancel_latched = 1;
  }
}


#define ZZ9K_ARCHIVE_MAX_NAME 256U
#define ZZ9K_ARCHIVE_MAX_PAX_DATA 65536U
#define ZZ9K_ARCHIVE_TAR_METHOD_STORE 0U
#define ZZ9K_ARCHIVE_TAR_FLAG_SKIP 1U
#define ZZ9K_ARCHIVE_TAR_FLAG_GNU_LONG_NAME 2U
#define ZZ9K_ARCHIVE_TAR_FLAG_PAX_HEADER 4U
#define ZZ9K_ARCHIVE_ENTRY_FLAG_CRC32 0x80000000UL
#define ZZ9K_ARCHIVE_ENTRY_FLAG_7Z_UNSUPPORTED_SPLIT 0x40000000UL
#define ZZ9K_ARCHIVE_ZIP_METHOD_STORE 0U
#define ZZ9K_ARCHIVE_ZIP_METHOD_DEFLATE 8U
#define ZZ9K_ARCHIVE_ZIP_EXTRA_ZIP64 0x0001U
#define ZZ9K_ARCHIVE_ZIP_U32_SENTINEL 0xffffffffUL
#define ZZ9K_ARCHIVE_ZIP_SUPPORTED_U32_MAX 0x7fffffffUL
#define ZZ9K_ARCHIVE_LHA_METHOD_LH0 0U
#define ZZ9K_ARCHIVE_LHA_METHOD_LH1 1U
#define ZZ9K_ARCHIVE_LHA_METHOD_LH5 5U
#define ZZ9K_ARCHIVE_LHA_METHOD_LH6 6U
#define ZZ9K_ARCHIVE_LHA_METHOD_LH7 7U
#define ZZ9K_ARCHIVE_LHA_METHOD_LHD 11U
#define ZZ9K_ARCHIVE_7Z_METHOD_COPY 0U
#define ZZ9K_ARCHIVE_7Z_METHOD_DEFLATE 0x040108UL
#define ZZ9K_ARCHIVE_7Z_METHOD_LZMA 0x030101UL
#define ZZ9K_ARCHIVE_7Z_METHOD_LZMA2 0x21U
#define ZZ9K_ARCHIVE_7Z_MAX_METHOD_PROPS 16U
#define ZZ9K_ARCHIVE_7Z_START_HEADER_SIZE 32U
#define ZZ9K_ARCHIVE_7Z_ID_END 0x00U
#define ZZ9K_ARCHIVE_7Z_ID_HEADER 0x01U
#define ZZ9K_ARCHIVE_7Z_ID_ARCHIVE_PROPERTIES 0x02U
#define ZZ9K_ARCHIVE_7Z_ID_ADDITIONAL_STREAMS_INFO 0x03U
#define ZZ9K_ARCHIVE_7Z_ID_MAIN_STREAMS_INFO 0x04U
#define ZZ9K_ARCHIVE_7Z_ID_FILES_INFO 0x05U
#define ZZ9K_ARCHIVE_7Z_ID_PACK_INFO 0x06U
#define ZZ9K_ARCHIVE_7Z_ID_UNPACK_INFO 0x07U
#define ZZ9K_ARCHIVE_7Z_ID_SUBSTREAMS_INFO 0x08U
#define ZZ9K_ARCHIVE_7Z_ID_SIZE 0x09U
#define ZZ9K_ARCHIVE_7Z_ID_CRC 0x0aU
#define ZZ9K_ARCHIVE_7Z_ID_FOLDER 0x0bU
#define ZZ9K_ARCHIVE_7Z_ID_CODERS_UNPACK_SIZE 0x0cU
#define ZZ9K_ARCHIVE_7Z_ID_NUM_UNPACK_STREAM 0x0dU
#define ZZ9K_ARCHIVE_7Z_ID_EMPTY_STREAM 0x0eU
#define ZZ9K_ARCHIVE_7Z_ID_EMPTY_FILE 0x0fU
#define ZZ9K_ARCHIVE_7Z_ID_ANTI 0x10U
#define ZZ9K_ARCHIVE_7Z_ID_NAME 0x11U
#define ZZ9K_ARCHIVE_7Z_ID_CREATION_TIME 0x12U
#define ZZ9K_ARCHIVE_7Z_ID_LAST_ACCESS_TIME 0x13U
#define ZZ9K_ARCHIVE_7Z_ID_LAST_WRITE_TIME 0x14U
#define ZZ9K_ARCHIVE_7Z_ID_WIN_ATTRIBUTES 0x15U
#define ZZ9K_ARCHIVE_7Z_ID_COMMENT 0x16U
#define ZZ9K_ARCHIVE_7Z_ID_ENCODED_HEADER 0x17U
#define ZZ9K_ARCHIVE_7Z_ID_START_POS 0x18U
#define ZZ9K_ARCHIVE_7Z_ID_DUMMY 0x19U
#define ZZ9K_ARCHIVE_PROBE_BYTES 512U
#define ZZ9K_ARCHIVE_STREAM_HOST_BUDGET (48U * 1024U)
#define ZZ9K_ARCHIVE_STREAM_CHUNK (ZZ9K_ARCHIVE_STREAM_HOST_BUDGET / 2U)
#define ZZ9K_ARCHIVE_STREAM_MIN_CHUNK 4096U
#define ZZ9K_ARCHIVE_TAR_BLOCK 512U
#define ZZ9K_ARCHIVE_LOAD_PROGRESS_THRESHOLD (1024U * 1024U)
#define ZZ9K_ARCHIVE_LOAD_PROGRESS_CHUNK (4U * 1024U * 1024U)

static int zz9k_archive_overwrite_outputs = 0;
static int zz9k_archive_skip_existing_outputs = 0;
static int zz9k_archive_dry_run_outputs = 0;
static int zz9k_archive_last_output_skipped = 0;
static int zz9k_archive_last_output_dry_run = 0;
static const char *zz9k_archive_match_filter = 0;
static uint32_t zz9k_archive_strip_components = 0U;
static const char *zz9k_archive_7z_last_parse_diagnostic = 0;
/* Runtime stream-feed chunk size. Both CPU-visible feed buffers (compressed
 * input and decoded output) live in the negotiated Zorro II host window or
 * the Z3 shared heap, so their combined budget must fit the acknowledged
 * heap: a generation-2 Zorro II layout (16 KiB window) streams with 8 KiB
 * chunks. Sized once from QUERY_CAPS in zz9k_archive_require_codec_service;
 * the compile-time default matches the Z3-era 48 KiB budget. */
static uint32_t zz9k_archive_stream_chunk = ZZ9K_ARCHIVE_STREAM_CHUNK;
/* Negotiated host-window heap size reported by QUERY_CAPS; 0 when the board
 * does not advertise one (Z3 shared heap, or an unacknowledged Z2 layout). */
static uint32_t zz9k_archive_host_window_heap = 0U;

/* Combined budget for the two CPU-visible stream feed buffers. A smaller
 * acknowledged Zorro II host window shrinks it so both halves fit. */
static uint32_t zz9k_archive_stream_budget(uint32_t host_window_heap)
{
  uint32_t budget = ZZ9K_ARCHIVE_STREAM_HOST_BUDGET;

  if (host_window_heap != 0U && host_window_heap < budget) {
    budget = host_window_heap;
  }
  return budget;
}

/* Per-buffer chunk derived from the combined budget, floored at the minimum
 * the feed loops will attempt. A heap smaller than twice the floor still
 * starts there and fails with a diagnostic that names the window. */
static uint32_t zz9k_archive_stream_budget_chunk(uint32_t budget)
{
  uint32_t chunk = budget / 2U;

  if (chunk < ZZ9K_ARCHIVE_STREAM_MIN_CHUNK) {
    chunk = ZZ9K_ARCHIVE_STREAM_MIN_CHUNK;
  }
  return chunk;
}

/* Allocation failures that mean "retry with half the request":
 * ZZ9K_STATUS_NO_MEMORY (heap full or fragmented) and
 * ZZ9K_STATUS_BAD_REQUEST (firmware rejects a request larger than the
 * negotiated host window). Anything else is fatal for the stream. */
static int zz9k_archive_alloc_shrink_retry(int status)
{
  return status == ZZ9K_STATUS_NO_MEMORY ||
         status == ZZ9K_STATUS_BAD_REQUEST;
}

/* Pair-retry decision for the second feed buffer: shrink the whole pair
 * only for the retryable statuses above the minimum chunk size. */
static int zz9k_archive_pair_shrink_retry(int status, uint32_t capacity)
{
  return zz9k_archive_alloc_shrink_retry(status) &&
         capacity > ZZ9K_ARCHIVE_STREAM_MIN_CHUNK;
}

/* Allocates the CPU-visible stream feed pair as one retry unit. The input
 * always comes from the negotiated host window; the output uses
 * `output_flags` (HOST_WINDOW when the caller consumes it, CARD_ONLY for
 * verify-only streams). When the output cannot allocate at the current
 * size, the held input is freed and the whole pair retries at half the
 * size: a contended window (e.g. resident AmiSSL scratch on a 16 KiB
 * Zorro II heap) then degrades to a balanced smaller pair instead of
 * failing the member after the input already consumed the free space.
 * Returns ZZ9K_STATUS_OK with both buffers held and the pair size in
 * *capacity_out; on failure returns the terminal status with nothing
 * held and the failed size in *failed_out. */
static int zz9k_archive_alloc_stream_pair(ZZ9KContext *ctx,
                                          uint32_t output_flags,
                                          ZZ9KSharedBuffer *input,
                                          ZZ9KSharedBuffer *decoded,
                                          uint32_t *capacity_out,
                                          uint32_t *failed_out)
{
  uint32_t capacity = zz9k_archive_stream_chunk;
  int status;

  memset(input, 0, sizeof(*input));
  memset(decoded, 0, sizeof(*decoded));
  *failed_out = capacity;

  while (capacity >= ZZ9K_ARCHIVE_STREAM_MIN_CHUNK) {
    *failed_out = capacity;
    status = zz9k_alloc_shared(ctx, capacity, 16U,
                               ZZ9K_ALLOC_HOST_WINDOW, input);
    if (status != ZZ9K_STATUS_OK) {
      if (!zz9k_archive_pair_shrink_retry(status, capacity)) {
        return status;
      }
      capacity /= 2U;
      continue;
    }
    status = zz9k_alloc_shared(ctx, capacity, 16U, output_flags, decoded);
    if (status == ZZ9K_STATUS_OK) {
      *capacity_out = capacity;
      return ZZ9K_STATUS_OK;
    }
    (void)zz9k_free_shared(ctx, input->handle);
    memset(input, 0, sizeof(*input));
    memset(decoded, 0, sizeof(*decoded));
    if (!zz9k_archive_pair_shrink_retry(status, capacity)) {
      return status;
    }
    capacity /= 2U;
  }
  return ZZ9K_STATUS_NO_MEMORY;
}

typedef enum ZZ9KArchiveFormat {
  ZZ9K_ARCHIVE_FORMAT_UNKNOWN = 0,
  ZZ9K_ARCHIVE_FORMAT_GZIP,
  ZZ9K_ARCHIVE_FORMAT_ZIP,
  ZZ9K_ARCHIVE_FORMAT_TAR,
  ZZ9K_ARCHIVE_FORMAT_7Z,
  ZZ9K_ARCHIVE_FORMAT_LZMA_ALONE,
  ZZ9K_ARCHIVE_FORMAT_LHA
} ZZ9KArchiveFormat;

typedef struct ZZ9KArchiveEntry {
  char name[ZZ9K_ARCHIVE_MAX_NAME];
  uint32_t method;
  uint32_t flags;
  uint32_t crc32;
  uint32_t data_offset;
  uint32_t decoded_offset;
  uint32_t compressed_size;
  uint32_t uncompressed_size;
  uint32_t is_dir;
  uint32_t method_props_size;
  uint8_t method_props[ZZ9K_ARCHIVE_7Z_MAX_METHOD_PROPS];
} ZZ9KArchiveEntry;

typedef struct ZZ9KArchiveGzipInfo {
  char name[ZZ9K_ARCHIVE_MAX_NAME];
  uint32_t crc32;
  uint32_t compressed_offset;
  uint32_t compressed_size;
  uint32_t uncompressed_size;
} ZZ9KArchiveGzipInfo;

typedef struct ZZ9KArchiveLzmaInfo {
  char name[ZZ9K_ARCHIVE_MAX_NAME];
  uint32_t compressed_offset;
  uint32_t compressed_size;
  uint32_t uncompressed_size;
  int size_known;
} ZZ9KArchiveLzmaInfo;

typedef struct ZZ9KArchive7zHeader {
  uint32_t next_header_offset;
  uint32_t next_header_size;
  uint32_t next_header_crc;
} ZZ9KArchive7zHeader;

typedef struct ZZ9KArchive7zCursor {
  const uint8_t *data;
  uint32_t size;
  uint32_t pos;
} ZZ9KArchive7zCursor;

typedef struct ZZ9KArchive7zStreams {
  uint32_t count;
  uint32_t *data_offsets;
  uint32_t *decoded_offsets;
  uint32_t *pack_sizes;
  uint32_t *unpack_sizes;
  uint32_t *crcs;
  uint32_t *methods;
  uint32_t *flags;
  uint8_t *crc_defined;
  uint8_t *props_sizes;
  uint8_t *props;
} ZZ9KArchive7zStreams;

typedef int (*ZZ9KArchiveDecodedChunkFn)(void *user,
                                         const uint8_t *data,
                                         uint32_t length);

typedef struct ZZ9KArchiveTarStream {
  const char *command;
  const char *output_dir;
  const char *archive_path; /* input file for the alias guard, or 0 */
  uint8_t header[ZZ9K_ARCHIVE_TAR_BLOCK];
  ZZ9KArchiveEntry entry;
  FILE *file;
  char pending_name[ZZ9K_ARCHIVE_MAX_NAME];
  uint8_t *pax_data;
  uint32_t pax_capacity;
  uint32_t pending_size;
  uint32_t header_used;
  uint32_t entry_remaining;
  uint32_t padding_remaining;
  uint32_t pax_used;
  uint32_t count;
  int pending_size_valid;
  int pending_name_skip;
  int pending_name_overflow;
  char *final_path;  /* staged output: destination of tmp_path */
  char *tmp_path;    /* staged output: temp written until complete */
  int ok;
  int done;
} ZZ9KArchiveTarStream;

typedef struct ZZ9KArchiveTarPaxInfo {
  char path[ZZ9K_ARCHIVE_MAX_NAME];
  uint32_t size;
  int has_path;
  int has_size;
  int path_skip;
} ZZ9KArchiveTarPaxInfo;

static int zz9k_archive_tar_header_checksum_valid(const uint8_t *header);
static int zz9k_archive_tar_header_empty(const uint8_t *header);
static int zz9k_archive_open_output_entry(const char *output_dir,
                                          const ZZ9KArchiveEntry *entry,
                                          FILE **file);
static int zz9k_archive_paths_same_file(const char *a, const char *b);
static int zz9k_archive_open_output_staged(const char *output_dir,
                                          const ZZ9KArchiveEntry *entry,
                                          FILE **file,
                                          char **final_path_out,
                                          char **tmp_path_out);

static uint16_t zz9k_archive_get_le16(const uint8_t *p)
{
  return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t zz9k_archive_get_le32(const uint8_t *p)
{
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t zz9k_archive_get_le64(const uint8_t *p)
{
  uint32_t lo;
  uint32_t hi;

  lo = zz9k_archive_get_le32(p);
  hi = zz9k_archive_get_le32(p + 4U);
  return (uint64_t)lo | ((uint64_t)hi << 32);
}

static uint32_t zz9k_archive_crc32(uint32_t crc,
                                   const uint8_t *data,
                                   uint32_t length)
{
  uint32_t i;

  if (!data && length != 0U) {
    return 0U;
  }
  crc ^= 0xffffffffUL;
  for (i = 0U; i < length; i++) {
    uint32_t bit;

    crc ^= data[i];
    for (bit = 0U; bit < 8U; bit++) {
      crc = (crc & 1U) != 0U ? (crc >> 1) ^ 0xedb88320UL : crc >> 1;
    }
  }
  return crc ^ 0xffffffffUL;
}

static int zz9k_archive_copy_name(char *dst, uint32_t dst_capacity,
                                  const uint8_t *src, uint32_t length)
{
  if (!dst || !src || dst_capacity == 0U ||
      length == 0U || length >= dst_capacity) {
    return 0;
  }
  memcpy(dst, src, length);
  dst[length] = '\0';
  return 1;
}

static void zz9k_archive_strip_current_dir_prefix(char *path)
{
  char *p;

  if (!path) {
    return;
  }
  while (path[0] == '.' && path[1] == '/' && path[2] != '\0') {
    memmove(path, path + 2, strlen(path + 2) + 1U);
  }
  p = path;
  while (*p != '\0') {
    if (p[0] == '/' && p[1] == '/') {
      memmove(p + 1, p + 2, strlen(p + 2) + 1U);
      continue;
    }
    if (p[0] == '/' && p[1] == '.' && p[2] == '/') {
      memmove(p + 1, p + 3, strlen(p + 3) + 1U);
      continue;
    }
    p++;
  }
}

static int zz9k_archive_copy_zip_name(char *dst, uint32_t dst_capacity,
                                      const uint8_t *src, uint32_t length)
{
  uint32_t i;

  if (!zz9k_archive_copy_name(dst, dst_capacity, src, length)) {
    return 0;
  }
  for (i = 0U; dst[i] != '\0'; i++) {
    if (dst[i] == '\\') {
      dst[i] = '/';
    }
  }
  zz9k_archive_strip_current_dir_prefix(dst);
  return 1;
}

static int zz9k_archive_copy_gzip_name(char *dst, uint32_t dst_capacity,
                                       const uint8_t *src, uint32_t length)
{
  uint32_t i;

  if (!zz9k_archive_copy_name(dst, dst_capacity, src, length)) {
    return 0;
  }
  for (i = 0U; dst[i] != '\0'; i++) {
    if (dst[i] == '\\') {
      dst[i] = '/';
    }
  }
  zz9k_archive_strip_current_dir_prefix(dst);
  return 1;
}

static int zz9k_archive_copy_lha_name(char *dst, uint32_t dst_capacity,
                                      const uint8_t *src, uint32_t length)
{
  uint32_t i;

  if (!zz9k_archive_copy_gzip_name(dst, dst_capacity, src, length)) {
    return 0;
  }
  for (i = 0U; dst[i] != '\0'; i++) {
    if ((uint8_t)dst[i] == 0xffU) {
      dst[i] = '/';
    }
  }
  zz9k_archive_strip_current_dir_prefix(dst);
  return 1;
}

static int zz9k_archive_lha_join_dir_name(char *dst,
                                          uint32_t dst_capacity,
                                          const char *dir,
                                          const char *name)
{
  size_t dir_len;
  size_t name_len;
  int need_sep;
  char name_copy[ZZ9K_ARCHIVE_MAX_NAME];

  if (!dst || !name || dst_capacity == 0U) {
    return 0;
  }
  if (!dir) {
    dir = "";
  }
  dir_len = strlen(dir);
  name_len = strlen(name);
  need_sep = dir_len != 0U && dir[dir_len - 1U] != '/';
  if (name_len >= sizeof(name_copy) ||
      dir_len + (need_sep ? 1U : 0U) + name_len >= dst_capacity) {
    return 0;
  }
  strcpy(name_copy, name);
  dst[0] = '\0';
  strcat(dst, dir);
  if (need_sep) {
    strcat(dst, "/");
  }
  strcat(dst, name_copy);
  zz9k_archive_strip_current_dir_prefix(dst);
  return 1;
}

static int zz9k_archive_name_ends_with_slash(const char *name)
{
  uint32_t length;

  if (!name) {
    return 0;
  }
  length = (uint32_t)strlen(name);
  return length > 0U && name[length - 1U] == '/';
}

static void zz9k_archive_trim_trailing_separators(char *path)
{
  uint32_t length;

  if (!path) {
    return;
  }
  length = (uint32_t)strlen(path);
  while (length > 0U &&
         (path[length - 1U] == '/' || path[length - 1U] == '\\')) {
    path[--length] = '\0';
  }
}

static int zz9k_archive_entry_matches_filter(const ZZ9KArchiveEntry *entry)
{
  if (!zz9k_archive_match_filter || zz9k_archive_match_filter[0] == '\0') {
    return 1;
  }
  return entry && strstr(entry->name, zz9k_archive_match_filter) != 0;
}

static const char *zz9k_archive_strip_entry_name(const char *name)
{
  uint32_t stripped = 0U;
  const char *p = name;

  if (!name || zz9k_archive_strip_components == 0U) {
    return name;
  }
  while (*p != '\0' && stripped < zz9k_archive_strip_components) {
    while (*p == '/') {
      p++;
    }
    while (*p != '\0' && *p != '/') {
      p++;
    }
    if (*p == '/') {
      p++;
      stripped++;
    } else {
      stripped++;
      break;
    }
  }
  while (*p == '/') {
    p++;
  }
  return stripped >= zz9k_archive_strip_components ? p : "";
}

static int zz9k_archive_output_entry(const ZZ9KArchiveEntry *entry,
                                     ZZ9KArchiveEntry *output_entry)
{
  const char *name;

  if (!entry || !output_entry) {
    return 0;
  }
  *output_entry = *entry;
  name = zz9k_archive_strip_entry_name(entry->name);
  if (!name || name[0] == '\0') {
    return 0;
  }
  if (strlen(name) >= sizeof(output_entry->name)) {
    return 0;
  }
  strcpy(output_entry->name, name);
  output_entry->is_dir = zz9k_archive_name_ends_with_slash(output_entry->name);
  return 1;
}

static int zz9k_archive_zip_entry_is_root_metadata(
    const ZZ9KArchiveEntry *entry)
{
  if (!entry || !entry->is_dir ||
      entry->method != ZZ9K_ARCHIVE_ZIP_METHOD_STORE ||
      entry->compressed_size != 0U || entry->uncompressed_size != 0U) {
    return 0;
  }
  return strcmp(entry->name, ".") == 0 || strcmp(entry->name, "./") == 0;
}

static int zz9k_archive_7z_entry_is_root_metadata(
    const ZZ9KArchiveEntry *entry)
{
  if (!entry || !entry->is_dir ||
      entry->method != ZZ9K_ARCHIVE_7Z_METHOD_COPY ||
      entry->compressed_size != 0U || entry->uncompressed_size != 0U) {
    return 0;
  }
  return strcmp(entry->name, ".") == 0 || strcmp(entry->name, "./") == 0;
}

static int zz9k_archive_path_is_safe(const char *path)
{
  const char *component;
  const char *p;
  uint32_t component_len;

  if (!path || path[0] == '\0' ||
      path[0] == '/' || path[0] == '\\') {
    return 0;
  }

  component = path;
  p = path;
  while (1) {
    if (*p == ':' || *p == '\\') {
      return 0;
    }
    if (*p == '/' || *p == '\0') {
      component_len = (uint32_t)(p - component);
      if (component_len == 0U) {
        if (*p == '\0' && p != path) {
          return 1;
        }
        return 0;
      }
      if ((component_len == 1U && component[0] == '.') ||
          (component_len == 2U && component[0] == '.' &&
           component[1] == '.')) {
        return 0;
      }
      if (*p == '\0') {
        return 1;
      }
      component = p + 1;
    }
    p++;
  }
}

static int zz9k_archive_detect_tar(const uint8_t *data, uint32_t length)
{
  if (!data || length < ZZ9K_ARCHIVE_TAR_BLOCK) {
    return 0;
  }
  if (length >= ZZ9K_ARCHIVE_TAR_BLOCK * 2U &&
      zz9k_archive_tar_header_empty(data) &&
      zz9k_archive_tar_header_empty(data + ZZ9K_ARCHIVE_TAR_BLOCK)) {
    return 1;
  }
  if (data[0] == 0U) {
    return 0;
  }
  return zz9k_archive_tar_header_checksum_valid(data);
}

static int zz9k_archive_lha_header_checksum_valid(const uint8_t *data,
                                                  uint32_t length)
{
  uint32_t header_size;
  uint32_t i;
  uint8_t checksum = 0U;

  if (!data || length < 24U) {
    return 0;
  }
  header_size = data[0];
  if (header_size == 0U) {
    return 0;
  }
  if (length >= 21U && data[20] == 2U) {
    header_size = zz9k_archive_get_le16(data);
    return header_size >= 26U && header_size <= length;
  }
  if (header_size + 2U > length) {
    return 0;
  }
  for (i = 2U; i < 2U + header_size; i++) {
    checksum = (uint8_t)(checksum + data[i]);
  }
  return checksum == data[1];
}

static int zz9k_archive_detect_lha(const uint8_t *data, uint32_t length)
{
  uint32_t header_size;
  uint8_t level;

  if (!data || length < 24U) {
    return 0;
  }
  level = data[20];
  if (level > 2U) {
    return 0;
  }
  if (level == 2U) {
    /* A level-2 header's total size (u16) can far exceed a probe buffer,
       and its "checksum" validation is only a bounds check, so detection
       validates the readable base (method bytes, sane sizes) without
       requiring the whole header to fit. Without this, a large level-2
       archive is misdetected at probe time and falls back to the
       whole-file load; the file walker grows its window and the parser
       validates the full extent later. */
    header_size = zz9k_archive_get_le16(data);
    if (header_size < 26U ||
        data[2] != '-' || data[6] != '-' ||
        data[3] != 'l' ||
        (data[4] != 'h' && data[4] != 'z')) {
      return 0;
    }
    return 1;
  }
  if (!zz9k_archive_lha_header_checksum_valid(data, length)) {
    return 0;
  }
  header_size = data[0];
  if (header_size < 22U ||
      data[2] != '-' || data[6] != '-' ||
      data[3] != 'l' ||
      (data[4] != 'h' && data[4] != 'z')) {
    return 0;
  }
  if (level == 0U && data[21] > header_size - 22U) {
    return 0;
  }
  return 1;
}

static ZZ9KArchiveFormat zz9k_archive_detect_format(const uint8_t *data,
                                                    uint32_t length)
{
  if (!data || length < 4U) {
    return ZZ9K_ARCHIVE_FORMAT_UNKNOWN;
  }
  if (length >= 6U &&
      data[0] == 0x37U && data[1] == 0x7aU &&
      data[2] == 0xbcU && data[3] == 0xafU &&
      data[4] == 0x27U && data[5] == 0x1cU) {
    return ZZ9K_ARCHIVE_FORMAT_7Z;
  }
  if (length >= 10U &&
      data[0] == 0x1fU && data[1] == 0x8bU && data[2] == 0x08U) {
    return ZZ9K_ARCHIVE_FORMAT_GZIP;
  }
  if (length >= 4U &&
      (zz9k_archive_get_le32(data) == 0x04034b50UL ||
       zz9k_archive_get_le32(data) == 0x06054b50UL)) {
    return ZZ9K_ARCHIVE_FORMAT_ZIP;
  }
  if (length >= 512U &&
      memcmp(data + 257U, "ustar", 5U) == 0) {
    return ZZ9K_ARCHIVE_FORMAT_TAR;
  }
  if (zz9k_archive_detect_tar(data, length)) {
    return ZZ9K_ARCHIVE_FORMAT_TAR;
  }
  if (zz9k_archive_detect_lha(data, length)) {
    return ZZ9K_ARCHIVE_FORMAT_LHA;
  }
  if (length >= 14U && data[0] < (9U * 5U * 5U) &&
      zz9k_archive_get_le32(data + 1U) >= 4096U) {
    return ZZ9K_ARCHIVE_FORMAT_LZMA_ALONE;
  }
  return ZZ9K_ARCHIVE_FORMAT_UNKNOWN;
}

static const char *zz9k_archive_format_name(ZZ9KArchiveFormat format)
{
  switch (format) {
    case ZZ9K_ARCHIVE_FORMAT_GZIP:
      return "gzip";
    case ZZ9K_ARCHIVE_FORMAT_ZIP:
      return "zip";
    case ZZ9K_ARCHIVE_FORMAT_TAR:
      return "tar";
    case ZZ9K_ARCHIVE_FORMAT_7Z:
      return "7z";
    case ZZ9K_ARCHIVE_FORMAT_LZMA_ALONE:
      return "lzma-alone";
    case ZZ9K_ARCHIVE_FORMAT_LHA:
      return "lha";
    default:
      return "unknown";
  }
}

static int zz9k_archive_read_file_data(FILE *file,
                                       const char *path,
                                       uint8_t *bytes,
                                       uint32_t size,
                                       int progress);

/* Loads the whole archive into memory. Large inputs are read in chunks with
   progress output: a multi-minute silent fread on slow Amiga storage looks
   like a hang, and stdout is block-buffered when redirected, so each line
   is flushed as it is printed. */
static int zz9k_archive_read_file(const char *path, uint8_t **data,
                                  uint32_t *length)
{
  FILE *file;
  long size;
  uint8_t *bytes;
  int progress;

  file = fopen(path, "rb");
  if (!file) {
    printf("open failed: %s\n", path);
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    printf("seek failed: %s\n", path);
    return 0;
  }
  size = ftell(file);
  if (size < 0 || size > 0x7fffffffL) {
    fclose(file);
    printf("unsupported file size: %s\n", path);
    return 0;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    printf("seek failed: %s\n", path);
    return 0;
  }
  progress = size >= (long)ZZ9K_ARCHIVE_LOAD_PROGRESS_THRESHOLD;
  if (progress) {
    printf("reading %luMB into RAM\n",
           (unsigned long)(size / (1024L * 1024L)));
    fflush(stdout);
  }
  bytes = 0;
  if (size > 0) {
    bytes = (uint8_t *)malloc((size_t)size);
    if (!bytes) {
      fclose(file);
      printf("input allocation failed\n");
      return 0;
    }
    if (zz9k_archive_read_file_data(file, path, bytes, (uint32_t)size,
                                    progress) != 0) {
      free(bytes);
      fclose(file);
      return 0;
    }
  }
  fclose(file);
  *data = bytes;
  *length = (uint32_t)size;
  return 1;
}

static int zz9k_archive_read_file_data(FILE *file,
                                       const char *path,
                                       uint8_t *bytes,
                                       uint32_t size,
                                       int progress)
{
  uint32_t remaining = size;
  uint32_t done = 0U;

  while (remaining != 0U) {
    uint32_t part = remaining > ZZ9K_ARCHIVE_LOAD_PROGRESS_CHUNK ?
        ZZ9K_ARCHIVE_LOAD_PROGRESS_CHUNK : remaining;

    if (fread(bytes + done, 1U, (size_t)part, file) != (size_t)part) {
      printf("read failed: %s\n", path);
      return 1;
    }
    done += part;
    remaining -= part;
    if (progress) {
      printf("\rread %lu/%luMB", (unsigned long)(done / (1024U * 1024U)),
             (unsigned long)(size / (1024U * 1024U)));
      fflush(stdout);
    }
  }
  if (progress) {
    printf("\n");
    fflush(stdout);
  }
  return 0;
}

static int zz9k_archive_read_file_range(const char *path,
                                        uint32_t offset,
                                        uint32_t length,
                                        uint8_t **data)
{
  FILE *file;
  uint8_t *bytes = 0;

  if (!path || !data || offset > 0x7fffffffUL) {
    return 0;
  }
  *data = 0;
  if (length != 0U) {
    bytes = (uint8_t *)malloc((size_t)length);
    if (!bytes) {
      printf("file range allocation failed: %lu bytes\n",
             (unsigned long)length);
      return 0;
    }
  }
  file = fopen(path, "rb");
  if (!file) {
    free(bytes);
    printf("open failed: %s\n", path);
    return 0;
  }
  if (fseek(file, (long)offset, SEEK_SET) != 0 ||
      (length != 0U &&
       fread(bytes, 1U, (size_t)length, file) != (size_t)length)) {
    free(bytes);
    fclose(file);
    printf("file range read failed: %s\n", path);
    return 0;
  }
  fclose(file);
  *data = bytes;
  return 1;
}

static int zz9k_archive_file_range_crc32(const char *path,
                                         uint32_t offset,
                                         uint32_t length,
                                         uint32_t *crc32)
{
  FILE *file;
  uint8_t *chunk;
  uint32_t remaining = length;
  uint32_t crc = 0U;
  int ok = 0;

  if (!path || !crc32 || offset > 0x7fffffffUL) {
    return 0;
  }
  chunk = (uint8_t *)malloc(ZZ9K_ARCHIVE_STREAM_CHUNK);
  if (!chunk) {
    printf("crc buffer allocation failed\n");
    return 0;
  }
  file = fopen(path, "rb");
  if (!file) {
    printf("open failed: %s\n", path);
    goto out;
  }
  if (fseek(file, (long)offset, SEEK_SET) != 0) {
    printf("file range seek failed: %s\n", path);
    goto out;
  }
  while (remaining != 0U) {
    uint32_t part = remaining > ZZ9K_ARCHIVE_STREAM_CHUNK ?
        ZZ9K_ARCHIVE_STREAM_CHUNK : remaining;

    if (fread(chunk, 1U, part, file) != part) {
      printf("file range read failed: %s\n", path);
      goto out;
    }
    crc = zz9k_archive_crc32(crc, chunk, part);
    remaining -= part;
  }
  *crc32 = crc;
  ok = 1;

out:
  if (file) {
    fclose(file);
  }
  free(chunk);
  return ok;
}

static int zz9k_archive_probe_file(const char *path,
                                   uint8_t *data,
                                   uint32_t capacity,
                                   uint32_t *length,
                                   uint32_t *file_length)
{
  FILE *file;
  long size;
  uint32_t read_length;

  if (!path || !data || capacity == 0U || !length || !file_length) {
    return 0;
  }

  file = fopen(path, "rb");
  if (!file) {
    printf("open failed: %s\n", path);
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    printf("seek failed: %s\n", path);
    return 0;
  }
  size = ftell(file);
  if (size < 0 || size > 0x7fffffffL) {
    fclose(file);
    printf("unsupported file size: %s\n", path);
    return 0;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    printf("seek failed: %s\n", path);
    return 0;
  }

  read_length = (uint32_t)size;
  if (read_length > capacity) {
    read_length = capacity;
  }
  if (read_length != 0U &&
      fread(data, 1U, (size_t)read_length, file) !=
          (size_t)read_length) {
    fclose(file);
    printf("read failed: %s\n", path);
    return 0;
  }

  fclose(file);
  *length = read_length;
  *file_length = (uint32_t)size;
  return 1;
}

static int zz9k_archive_path_exists(const char *path)
{
  if (!path || path[0] == '\0') {
    return 0;
  }
#if defined(__amigaos__)
  {
    BPTR lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock) {
      UnLock(lock);
      return 1;
    }
    return 0;
  }
#else
  {
    struct stat st;
    return stat(path, &st) == 0;
  }
#endif
}

static int zz9k_archive_path_is_dir(const char *path)
{
  if (!path || path[0] == '\0') {
    return 0;
  }
#if defined(__amigaos__)
  {
    BPTR lock;
    struct FileInfoBlock *fib;
    int ok = 0;

    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (!lock) {
      return 0;
    }
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, 0);
    if (fib) {
      ok = Examine(lock, fib) && fib->fib_DirEntryType > 0;
      FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
    return ok;
  }
#else
  {
    struct stat st;
    if (stat(path, &st) != 0) {
      return 0;
    }
#if defined(_WIN32)
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
  }
#endif
}

static int zz9k_archive_write_file(const char *path, const uint8_t *data,
                                   uint32_t length)
{
  FILE *file;

  zz9k_archive_last_output_skipped = 0;
  zz9k_archive_last_output_dry_run = 0;
  if (zz9k_archive_path_is_dir(path)) {
    printf("output path is a directory: %s\n", path);
    return 0;
  }
  if (zz9k_archive_skip_existing_outputs && zz9k_archive_path_exists(path)) {
    printf("s %s\n", path);
    zz9k_archive_last_output_skipped = 1;
    return 1;
  }
  if (!zz9k_archive_overwrite_outputs && zz9k_archive_path_exists(path)) {
    printf("output exists, use --overwrite: %s\n", path);
    return 0;
  }
  if (zz9k_archive_dry_run_outputs) {
    printf("dry %s\n", path);
    zz9k_archive_last_output_dry_run = 1;
    return 1;
  }
  file = fopen(path, "wb");
  if (!file) {
    printf("open output failed: %s\n", path);
    return 0;
  }
  if (length != 0U && fwrite(data, 1U, length, file) != length) {
    fclose(file);
    printf("write failed: %s\n", path);
    return 0;
  }
  fclose(file);
  return 1;
}

static FILE *zz9k_archive_open_discard_file(void)
{
#if defined(__amigaos__)
  return fopen("NIL:", "wb");
#elif defined(_WIN32)
  return fopen("NUL", "wb");
#else
  return fopen("/dev/null", "wb");
#endif
}

static int zz9k_archive_mkdir_one(const char *path)
{
  if (!path || path[0] == '\0') {
    return 1;
  }
#if defined(__amigaos__)
  {
    BPTR lock;

    lock = CreateDir((CONST_STRPTR)path);
    if (lock) {
      UnLock(lock);
      return 1;
    }
    return zz9k_archive_path_is_dir(path);
  }
#elif defined(_WIN32)
  if (_mkdir(path) == 0) {
    return 1;
  }
  if (errno == EEXIST) {
    return zz9k_archive_path_is_dir(path);
  }
  return 0;
#else
  if (mkdir(path, 0777) == 0) {
    return 1;
  }
  if (errno == EEXIST) {
    return zz9k_archive_path_is_dir(path);
  }
  return 0;
#endif
}

static int zz9k_archive_ascii_lower(int ch)
{
  if (ch >= 'A' && ch <= 'Z') {
    return ch - 'A' + 'a';
  }
  return ch;
}

static int zz9k_archive_has_suffix_ci(const char *text, const char *suffix)
{
  size_t text_len;
  size_t suffix_len;
  size_t i;

  if (!text || !suffix) {
    return 0;
  }
  text_len = strlen(text);
  suffix_len = strlen(suffix);
  if (suffix_len > text_len) {
    return 0;
  }
  text += text_len - suffix_len;
  for (i = 0U; i < suffix_len; i++) {
    if (zz9k_archive_ascii_lower((unsigned char)text[i]) !=
        zz9k_archive_ascii_lower((unsigned char)suffix[i])) {
      return 0;
    }
  }
  return 1;
}

static char *zz9k_archive_join_path(const char *base, const char *name)
{
  size_t base_len;
  size_t name_len;
  int need_sep;
  char *path;

  if (!name) {
    return 0;
  }
  if (!base || base[0] == '\0') {
    base = "";
  }
  base_len = strlen(base);
  name_len = strlen(name);
  need_sep = (base_len != 0U &&
              base[base_len - 1U] != '/' &&
              base[base_len - 1U] != '\\' &&
              base[base_len - 1U] != ':');
  path = (char *)malloc(base_len + (need_sep ? 1U : 0U) + name_len + 1U);
  if (!path) {
    return 0;
  }
  path[0] = '\0';
  strcat(path, base);
  if (need_sep) {
    strcat(path, "/");
  }
  strcat(path, name);
  return path;
}

static int zz9k_archive_ensure_parent_dirs(const char *base,
                                           const char *name)
{
  char *prefix;
  char *slash;
  char saved;
  int ok = 1;

  prefix = zz9k_archive_join_path(base, name);
  if (!prefix) {
    return 0;
  }

  slash = prefix;
  while (*slash != '\0') {
    if (*slash == '/' || *slash == '\\') {
      saved = *slash;
      *slash = '\0';
      if (prefix[0] != '\0' &&
          prefix[strlen(prefix) - 1U] != ':' &&
          !zz9k_archive_mkdir_one(prefix)) {
        ok = 0;
        *slash = saved;
        break;
      }
      *slash = saved;
    }
    slash++;
  }

  free(prefix);
  return ok;
}

static int zz9k_archive_gzip_header_info(
    const uint8_t *data,
    uint32_t length,
    ZZ9KArchiveGzipInfo *info,
    uint32_t *header_end)
{
  uint32_t pos;
  uint8_t flags;
  uint32_t name_start;

  if (!data || !info || !header_end || length < 10U ||
      data[0] != 0x1fU || data[1] != 0x8bU || data[2] != 0x08U) {
    return 0;
  }

  memset(info, 0, sizeof(*info));
  flags = data[3];
  pos = 10U;

  if ((flags & 0xe0U) != 0U) {
    return 0;
  }
  if ((flags & 0x04U) != 0U) {
    uint32_t extra_len;
    if (pos + 2U > length) {
      return 0;
    }
    extra_len = zz9k_archive_get_le16(data + pos);
    pos += 2U;
    if (extra_len > length || pos + extra_len > length) {
      return 0;
    }
    pos += extra_len;
  }
  if ((flags & 0x08U) != 0U) {
    name_start = pos;
    while (pos < length && data[pos] != 0U) {
      pos++;
    }
    if (pos >= length ||
        !zz9k_archive_copy_gzip_name(info->name, sizeof(info->name),
                                     data + name_start, pos - name_start)) {
      return 0;
    }
    pos++;
  }
  if ((flags & 0x10U) != 0U) {
    while (pos < length && data[pos] != 0U) {
      pos++;
    }
    if (pos >= length) {
      return 0;
    }
    pos++;
  }
  if ((flags & 0x02U) != 0U) {
    uint32_t actual;
    uint32_t expected;

    if (pos + 2U > length) {
      return 0;
    }
    actual = zz9k_archive_crc32(0U, data, pos) & 0xffffU;
    expected = zz9k_archive_get_le16(data + pos);
    if (actual != expected) {
      return 0;
    }
    pos += 2U;
  }
  if (info->name[0] == '\0') {
    strcpy(info->name, "output");
  }
  info->compressed_offset = 0U;
  *header_end = pos;
  return 1;
}

static int zz9k_archive_gzip_info(const uint8_t *data, uint32_t length,
                                  ZZ9KArchiveGzipInfo *info)
{
  uint32_t header_end;

  if (!zz9k_archive_gzip_header_info(data, length, info, &header_end) ||
      header_end + 8U > length) {
    return 0;
  }
  info->compressed_size = length;
  info->crc32 = zz9k_archive_get_le32(data + length - 8U);
  info->uncompressed_size = zz9k_archive_get_le32(data + length - 4U);
  return 1;
}

static int zz9k_archive_gzip_info_from_file(
    const char *path,
    const uint8_t *header,
    uint32_t header_length,
    uint32_t file_length,
    ZZ9KArchiveGzipInfo *info)
{
  uint8_t *tail = 0;
  uint32_t header_end;
  int ok = 0;

  if (!path || !info || file_length < 18U ||
      !zz9k_archive_gzip_header_info(
          header, header_length, info, &header_end) ||
      header_end + 8U > file_length) {
    return 0;
  }
  if (!zz9k_archive_read_file_range(path, file_length - 8U, 8U, &tail)) {
    return 0;
  }
  info->compressed_size = file_length;
  info->crc32 = zz9k_archive_get_le32(tail);
  info->uncompressed_size = zz9k_archive_get_le32(tail + 4U);
  ok = 1;

  free(tail);
  return ok;
}

static int zz9k_archive_gzip_result_matches_footer(
    const ZZ9KArchiveGzipInfo *info,
    const ZZ9KDecompressResult *result)
{
  if (!info || !result) {
    return 0;
  }
  return result->bytes_written == info->uncompressed_size &&
         result->checksum == info->crc32;
}

static int zz9k_archive_gzip_is_tar_candidate(
    const char *archive_path,
    const ZZ9KArchiveGzipInfo *info)
{
  return (info && zz9k_archive_has_suffix_ci(info->name, ".tar")) ||
         zz9k_archive_has_suffix_ci(archive_path, ".tar.gz") ||
         zz9k_archive_has_suffix_ci(archive_path, ".tgz");
}

static int zz9k_archive_lzma_info(const uint8_t *data, uint32_t length,
                                  ZZ9KArchiveLzmaInfo *info)
{
  uint64_t unpacked_size;

  if (!data || !info || length < 14U ||
      data[0] >= (9U * 5U * 5U) ||
      zz9k_archive_get_le32(data + 1U) < 4096U) {
    return 0;
  }

  memset(info, 0, sizeof(*info));
  strcpy(info->name, "output");
  info->compressed_offset = 0U;
  info->compressed_size = length;
  unpacked_size = zz9k_archive_get_le64(data + 5U);
  if (unpacked_size == ~(uint64_t)0) {
    info->size_known = 0;
    info->uncompressed_size = 0U;
    return 1;
  }
  if (unpacked_size > 0x7fffffffULL) {
    return 0;
  }
  info->size_known = 1;
  info->uncompressed_size = (uint32_t)unpacked_size;
  return 1;
}

static int zz9k_archive_lzma_info_from_header(
    const uint8_t *data,
    uint32_t length,
    uint32_t file_length,
    ZZ9KArchiveLzmaInfo *info)
{
  if (!zz9k_archive_lzma_info(data, length, info)) {
    return 0;
  }
  info->compressed_size = file_length;
  return file_length >= 14U;
}

/* Parses one LHA member header from `hdr`, where `window_len` bytes are
   readable at the member position and `archive_avail` bytes remain in the
   archive. Returns ZZ9K_ARCHIVE_LHA_PARSE_OK and fills *entry_out and
   *header_bytes on success. entry_out->data_offset is RELATIVE to hdr:
   the byte distance from the header start to the compressed data (which
   is also the header's total length, base plus extensions); walkers add
   the member position to make it absolute.

   Header bytes (base header, extensions, name, CRC) are validated against
   window_len only; the member's compressed-data extent is validated
   against archive_avail, so a caller may hand this function a small
   prefix window of a huge member without ever pulling member bodies into
   it. The verdict distinguishes a definitive INVALID (bad checksum,
   method or size fields -- provable from the bytes already inside the
   window; the file walker fails immediately without enlarging anything)
   from NEEDS_WINDOW (the base header or an extension chain extends past
   the window; the file walker grows its window and retries). A short
   window can therefore never make the parse accept -- only ask for
   more. */
#define ZZ9K_ARCHIVE_LHA_PARSE_INVALID 0
#define ZZ9K_ARCHIVE_LHA_PARSE_OK 1
#define ZZ9K_ARCHIVE_LHA_PARSE_NEEDS_WINDOW 2

static int zz9k_archive_lha_parse_header(const uint8_t *hdr,
                                         uint32_t window_len,
                                         uint32_t archive_avail,
                                         ZZ9KArchiveEntry *entry_out,
                                         uint32_t *header_bytes)
{
  uint32_t header_size;
  uint32_t compressed_size;
  uint32_t uncompressed_size;
  uint32_t name_len;
  uint32_t data_offset;
  uint32_t level;
  uint32_t method_offset;
  uint32_t base_header_size;
  uint32_t crc_offset;
  uint32_t os_offset;
  uint32_t ext_size_offset;
  uint32_t name_offset;
  uint32_t ext_size;
  uint32_t ext_pos;
  uint32_t ext_total;
  char ext_dir[ZZ9K_ARCHIVE_MAX_NAME];
  char ext_name[ZZ9K_ARCHIVE_MAX_NAME];
  ZZ9KArchiveEntry entry;

  if (!hdr || !entry_out || !header_bytes) {
    return ZZ9K_ARCHIVE_LHA_PARSE_INVALID;
  }
  if (window_len < 24U) {
    /* Fewer bytes than a minimal header (24 = 2 size/checksum bytes + a
       22-byte level-0 base): the window is too short to judge anything.
       Ask for more window -- declaring this INVALID made the single-pass
       walker reject a perfectly good member whose header happened to
       start within the last 23 bytes of a 32 KiB chunk (real-world
       casualty: member 165 of LightwaveRTGv1.1.lha at offset 3002313). */
    return ZZ9K_ARCHIVE_LHA_PARSE_NEEDS_WINDOW;
  }
  header_size = hdr[0];
  if (header_size == 0U) {
    return ZZ9K_ARCHIVE_LHA_PARSE_INVALID; /* terminator: walkers decide */
  }
  level = hdr[20U];
  if (level > 2U) {
    return ZZ9K_ARCHIVE_LHA_PARSE_INVALID;
  }
  method_offset = 2U;
  if (level == 2U) {
    header_size = zz9k_archive_get_le16(hdr);
    base_header_size = 26U;
    crc_offset = 21U;
    os_offset = 23U;
    ext_size_offset = 24U;
    name_len = 0U;
    name_offset = 0U;
    data_offset = header_size;
  } else {
    base_header_size = level == 0U ? 22U : 25U;
    crc_offset = 22U + hdr[21U];
    os_offset = crc_offset + 2U;
    ext_size_offset = os_offset + 1U;
    name_len = hdr[21U];
    name_offset = 22U;
    data_offset = 2U + header_size;
  }
  /* Window sufficiency before validation: an incomplete header only asks
     for a bigger window. Every check below this point sees a header whose
     base bytes are fully inside the window, which makes its failures
     definitive for the file walker. */
  if ((level == 2U && header_size > window_len) ||
      (level != 2U && 2U + header_size > window_len)) {
    return ZZ9K_ARCHIVE_LHA_PARSE_NEEDS_WINDOW;
  }
  if (header_size < base_header_size ||
      !zz9k_archive_lha_header_checksum_valid(hdr, window_len) ||
      hdr[method_offset] != '-' || hdr[method_offset + 4U] != '-' ||
      hdr[method_offset + 1U] != 'l' ||
      (hdr[method_offset + 2U] != 'h' && hdr[method_offset + 2U] != 'z')) {
    return ZZ9K_ARCHIVE_LHA_PARSE_INVALID;
  }
  compressed_size = zz9k_archive_get_le32(hdr + 7U);
  uncompressed_size = zz9k_archive_get_le32(hdr + 11U);
  if (level != 2U && name_len > header_size - base_header_size) {
    return ZZ9K_ARCHIVE_LHA_PARSE_INVALID;
  }
  ext_size = 0U;
  ext_total = 0U;
  ext_pos = level == 2U ? base_header_size : data_offset;
  memset(ext_dir, 0, sizeof(ext_dir));
  memset(ext_name, 0, sizeof(ext_name));
  if (level == 1U || level == 2U) {
    if (ext_size_offset + 2U > window_len) {
      return ZZ9K_ARCHIVE_LHA_PARSE_NEEDS_WINDOW;
    }
    ext_size = zz9k_archive_get_le16(hdr + ext_size_offset);
    while (ext_size != 0U) {
      uint32_t ext_data_len;
      uint32_t next_ext_size;

      if (ext_pos > window_len || ext_size > window_len - ext_pos) {
        return ZZ9K_ARCHIVE_LHA_PARSE_NEEDS_WINDOW;
      }
      if (ext_size < 3U) {
        return ZZ9K_ARCHIVE_LHA_PARSE_INVALID;
      }
      ext_total += ext_size;
      ext_data_len = ext_size - 3U;
      next_ext_size = zz9k_archive_get_le16(
          hdr + ext_pos + ext_size - 2U);
      if (hdr[ext_pos] == 0x01U && ext_data_len != 0U) {
        while (ext_data_len != 0U &&
               hdr[ext_pos + 1U + ext_data_len - 1U] == 0U) {
          ext_data_len--;
        }
        if (ext_data_len != 0U &&
            !zz9k_archive_copy_lha_name(
                ext_name, sizeof(ext_name), hdr + ext_pos + 1U,
                ext_data_len)) {
          return ZZ9K_ARCHIVE_LHA_PARSE_INVALID;
        }
      } else if (hdr[ext_pos] == 0x02U && ext_data_len != 0U) {
        while (ext_data_len != 0U &&
               hdr[ext_pos + 1U + ext_data_len - 1U] == 0U) {
          ext_data_len--;
        }
        if (ext_data_len != 0U &&
            !zz9k_archive_copy_lha_name(
                ext_dir, sizeof(ext_dir), hdr + ext_pos + 1U,
                ext_data_len)) {
          return ZZ9K_ARCHIVE_LHA_PARSE_INVALID;
        }
      }
      ext_pos += ext_size;
      ext_size = next_ext_size;
    }
    data_offset = ext_pos;
  }
  if (level == 1U && ext_total != 0U) {
    /* In an LHA level-1 header the "compressed size" field (offset 7)
       spans BOTH the extended headers and the compressed data. The data
       begins after the extended headers (data_offset == ext_pos), so the
       real compressed-data size is the field minus the total
       extended-header bytes. This correction must apply to every level-1
       entry that carries extended headers -- not only when the
       uncorrected size would overrun end-of-file. Otherwise multi-member
       archives, and any leading -lhd- directory entry (whose data size is
       zero, so the field equals ext_total exactly), over-skip by
       ext_total and mis-locate the next header. */
    if (compressed_size < ext_total) {
      return ZZ9K_ARCHIVE_LHA_PARSE_INVALID;
    }
    compressed_size -= ext_total;
  }
  if (compressed_size > archive_avail || data_offset > archive_avail ||
      compressed_size > archive_avail - data_offset) {
    return ZZ9K_ARCHIVE_LHA_PARSE_INVALID;
  }

  memset(&entry, 0, sizeof(entry));
  if (name_len != 0U) {
    if (!zz9k_archive_copy_lha_name(
            entry.name, sizeof(entry.name), hdr + name_offset, name_len)) {
      return ZZ9K_ARCHIVE_LHA_PARSE_INVALID;
    }
  } else {
    strcpy(entry.name, "unnamed");
  }
  if (ext_name[0] != '\0') {
    if (!zz9k_archive_lha_join_dir_name(
            entry.name, sizeof(entry.name), ext_dir, ext_name)) {
      return ZZ9K_ARCHIVE_LHA_PARSE_INVALID;
    }
  } else if (ext_dir[0] != '\0') {
    if (!zz9k_archive_lha_join_dir_name(
            entry.name, sizeof(entry.name), ext_dir, entry.name)) {
      return ZZ9K_ARCHIVE_LHA_PARSE_INVALID;
    }
  }
  if (memcmp(hdr + method_offset, "-lh0-", 5U) == 0 ||
      memcmp(hdr + method_offset, "-lz4-", 5U) == 0) {
    entry.method = ZZ9K_ARCHIVE_LHA_METHOD_LH0;
  } else if (memcmp(hdr + method_offset, "-lhd-", 5U) == 0) {
    entry.method = ZZ9K_ARCHIVE_LHA_METHOD_LHD;
    entry.is_dir = 1U;
    if (ext_dir[0] != '\0' && ext_name[0] == '\0') {
      strcpy(entry.name, ext_dir);
    }
    if (!zz9k_archive_name_ends_with_slash(entry.name)) {
      size_t entry_name_len = strlen(entry.name);
      if (entry_name_len + 1U >= sizeof(entry.name)) {
        return ZZ9K_ARCHIVE_LHA_PARSE_INVALID;
      }
      entry.name[entry_name_len] = '/';
      entry.name[entry_name_len + 1U] = '\0';
    }
  } else if (memcmp(hdr + method_offset, "-lh1-", 5U) == 0) {
    entry.method = ZZ9K_ARCHIVE_LHA_METHOD_LH1;
  } else if (memcmp(hdr + method_offset, "-lh5-", 5U) == 0) {
    entry.method = ZZ9K_ARCHIVE_LHA_METHOD_LH5;
  } else if (memcmp(hdr + method_offset, "-lh6-", 5U) == 0) {
    entry.method = ZZ9K_ARCHIVE_LHA_METHOD_LH6;
  } else if (memcmp(hdr + method_offset, "-lh7-", 5U) == 0) {
    entry.method = ZZ9K_ARCHIVE_LHA_METHOD_LH7;
  } else {
    entry.method = 0xffffffffUL;
  }
  if (crc_offset + 2U <= window_len) {
    entry.crc32 = zz9k_archive_get_le16(hdr + crc_offset);
    entry.flags |= ZZ9K_ARCHIVE_ENTRY_FLAG_CRC32;
  }
  entry.compressed_size = compressed_size;
  entry.uncompressed_size = uncompressed_size;
  entry.data_offset = data_offset;
  if (entry.method != ZZ9K_ARCHIVE_LHA_METHOD_LHD) {
    entry.is_dir = zz9k_archive_name_ends_with_slash(entry.name);
  }
  *entry_out = entry;
  *header_bytes = data_offset;
  return ZZ9K_ARCHIVE_LHA_PARSE_OK;
}

static int zz9k_archive_lha_list(const uint8_t *data,
                                 uint32_t length,
                                 ZZ9KArchiveEntry *entries,
                                 uint32_t max_entries,
                                 uint32_t *count)
{
  uint32_t pos = 0U;
  uint32_t entries_used = 0U;

  if (!data || !count) {
    return 0;
  }
  *count = 0U;
  while (pos < length) {
    ZZ9KArchiveEntry entry;
    uint32_t header_bytes;

    if (data[pos] == 0U) {
      *count = entries_used;
      return 1;
    }
    if (length - pos < 24U) {
      /* Fewer bytes remain than a minimal LHA header (24 = the smallest
         valid header: 2 size/checksum bytes + a 22-byte level-0 base). This
         is trailing padding or junk after the final member: some archivers
         omit or malform the terminating zero header-size byte (e.g. a stray
         "0\0\0" instead of a single 0x00). Treat it as end-of-archive and
         keep the members parsed so far -- matching lha/7-Zip -- rather than
         discarding the whole archive. (The checksum validator also
         floors at 24 bytes, so a 21-23 byte tail would otherwise hard-fail
         the whole archive here.) */
      *count = entries_used;
      return 1;
    }
    if (zz9k_archive_lha_parse_header(data + pos, length - pos,
                                      length - pos, &entry,
                                      &header_bytes) !=
        ZZ9K_ARCHIVE_LHA_PARSE_OK) {
      return 0;
    }
    entry.data_offset = pos + entry.data_offset;
    if (entries && entries_used < max_entries) {
      entries[entries_used] = entry;
    }
    entries_used++;
    pos = entry.data_offset + entry.compressed_size;
  }
  /* Loop exited because pos reached the end of the data (each entry's size
     was bounds-checked, so pos == length here). The archive's members were
     all consumed without encountering a zero terminator header -- common for
     archives that simply end at the final member's data. That is a complete,
     successful parse, not a failure. */
  *count = entries_used;
  return 1;
}

/* Read granularity for the single-pass file walk. One refill is one
   seekable-read round trip; a refill happens for essentially every
   member because consecutive headers are separated by that member's
   DATA (often far more than any chunk), so a large chunk mostly reads
   bytes the walk never looks at. 4 KiB covers every realistic header
   (base + extensions) while transferring ~8x less than a 32 KiB chunk
   over a bandwidth-starved network mount; pathological headers larger
   than it grow the window as before. Member bodies are never parsed --
   the next member's header is reached by seeking, not by reading
   through the data. */
#define ZZ9K_ARCHIVE_LHA_WALK_CHUNK (4U * 1024U)
#define ZZ9K_ARCHIVE_LHA_WALK_INITIAL_ENTRIES 1024U

typedef void (*ZZ9KArchiveLhaEntryFn)(const ZZ9KArchiveEntry *entry,
                                      void *user);

/* Walks a seekable LHA archive in ONE pass, growing the entry table on
   demand (caller frees *entries_out) and optionally handing each member
   to on_entry the moment it is parsed -- a listing over a slow network
   mount streams output instead of going quiet for minutes. Verdicts
   match the in-memory walk: same terminator and trailing-junk tolerance,
   same tri-state window handling. */
static int zz9k_archive_lha_list_file(FILE *file,
                                      uint32_t length,
                                      ZZ9KArchiveEntry **entries_out,
                                      uint32_t *count,
                                      ZZ9KArchiveLhaEntryFn on_entry,
                                      void *on_entry_user)
{
  uint32_t pos = 0U;
  uint32_t entries_used = 0U;
  uint32_t capacity = 0U;
  uint32_t window_allocated = ZZ9K_ARCHIVE_LHA_WALK_CHUNK;
  uint32_t window_base = 0U;  /* file offset of window[0] */
  uint32_t window_valid = 0U; /* readable bytes currently in the window */
  ZZ9KArchiveEntry *entries = 0;
  uint8_t *window = 0;
  int ok = 0;

  if (!file || !entries_out || !count) {
    return 0;
  }
  *entries_out = 0;
  *count = 0U;
  window = (uint8_t *)malloc(window_allocated);
  if (!window) {
    printf("lha header window allocation failed\n");
    return 0;
  }
  while (pos < length) {
    ZZ9KArchiveEntry entry;
    uint32_t avail = length - pos;
    uint32_t header_bytes;

    if (zz9k_archive_cancelled()) {

      goto out; /* terminal: never a cue to fall back or continue */
    }
    if (pos < window_base || pos >= window_base + window_valid) {
      window_base = pos;
      window_valid = avail < window_allocated ? avail : window_allocated;
      if (fseek(file, (long)pos, SEEK_SET) != 0 ||
          fread(window, 1U, window_valid, file) != window_valid) {
        printf("lha header read failed at offset %lu\n", (unsigned long)pos);
        goto out;
      }
    }
    if (window[pos - window_base] == 0U) {
      /* end-of-archive terminator byte */
      ok = 1;
      goto done;
    }
    if (avail < 24U) {
      /* Trailing padding or junk after the final member: the same
         tolerance as the in-memory walk. */
      ok = 1;
      goto done;
    }
    for (;;) {
      uint32_t offset = pos - window_base;
      uint32_t window_len = window_valid - offset;
      int parse_rc = zz9k_archive_lha_parse_header(
          window + offset, window_len, avail, &entry, &header_bytes);

      if (parse_rc == ZZ9K_ARCHIVE_LHA_PARSE_OK) {
        break;
      }
      if (parse_rc == ZZ9K_ARCHIVE_LHA_PARSE_INVALID) {
        /* A definitive error never re-reads (bounded failure: one
           chunk of I/O even inside a multi-hundred-MB archive). Name
           the member position: the caller treats this as terminal, so
           the diagnostic is all the user gets. */
        printf("lha parse failed at offset %lu (member %lu)\n",
               (unsigned long)pos, (unsigned long)entries_used);
        goto out;
      }
      if (offset != 0U) {
        /* NEEDS_WINDOW with the header not at the window start: it
           crosses the chunk edge. Re-center the window on this member
           and retry before considering growth. */
        window_base = pos;
        window_valid = avail < window_allocated ? avail : window_allocated;
        if (fseek(file, (long)pos, SEEK_SET) != 0 ||
            fread(window, 1U, window_valid, file) != window_valid) {
          printf("lha header read failed at offset %lu\n", (unsigned long)pos);
          goto out;
        }
        continue;
      }
      if (window_valid >= avail) {
        goto out; /* full view already: the in-memory walk's failure */
      }
      /* The header is larger than the whole chunk: grow (x4, clamped to
         the archive remainder) and refill. */
      {
        uint32_t grown_capacity = window_allocated * 4U;

        if (grown_capacity > avail) {
          grown_capacity = avail;
        }
        if (grown_capacity > window_allocated) {
          uint8_t *grown = (uint8_t *)realloc(window, grown_capacity);

          if (!grown) {
            printf("lha header window allocation failed\n");
            goto out;
          }
          window = grown;
          window_allocated = grown_capacity;
        }
        window_base = pos;
        window_valid = avail < window_allocated ? avail : window_allocated;
        if (fseek(file, (long)pos, SEEK_SET) != 0 ||
            fread(window, 1U, window_valid, file) != window_valid) {
          printf("lha header read failed at offset %lu\n", (unsigned long)pos);
          goto out;
        }
      }
    }
    entry.data_offset = pos + entry.data_offset;
    if (entries_used == capacity) {
      uint32_t grown_capacity = capacity != 0U ? capacity * 2U :
          ZZ9K_ARCHIVE_LHA_WALK_INITIAL_ENTRIES;
      ZZ9KArchiveEntry *grown = (ZZ9KArchiveEntry *)realloc(
          entries, (size_t)grown_capacity * sizeof(*grown));

      if (!grown) {
        printf("lha entry allocation failed\n");
        goto out;
      }
      entries = grown;
      capacity = grown_capacity;
    }
    entries[entries_used] = entry;
    if (on_entry) {
      on_entry(&entries[entries_used], on_entry_user);
    }
    entries_used++;
    pos = entry.data_offset + entry.compressed_size;
  }
  ok = 1;

done:
  if (ok) {
    *entries_out = entries;
    *count = entries_used;
    entries = 0;
  }
out:
  free(window);
  free(entries);
  return ok;
}

static int zz9k_archive_lzma_output_capacity(
    const ZZ9KArchiveLzmaInfo *info,
    uint32_t requested_capacity,
    uint32_t *output_capacity)
{
  if (!info || !output_capacity) {
    return 0;
  }
  if (info->size_known) {
    if (requested_capacity != 0U &&
        requested_capacity < info->uncompressed_size) {
      return 0;
    }
    *output_capacity = info->uncompressed_size;
    return info->uncompressed_size != 0U;
  }
  if (requested_capacity == 0U) {
    return 0;
  }
  *output_capacity = requested_capacity;
  return 1;
}

static int zz9k_archive_lzma_props_dict_size(const uint8_t *props,
                                             uint32_t props_size,
                                             uint32_t *dict_size)
{
  if (!props || props_size != 5U || !dict_size) {
    return 0;
  }
  *dict_size = zz9k_archive_get_le32(props + 1U);
  return 1;
}

static int zz9k_archive_lzma2_prop_dict_size(uint8_t prop,
                                             uint32_t *dict_size)
{
  if (!dict_size || prop > 40U) {
    return 0;
  }
  if (prop == 40U) {
    *dict_size = 0xffffffffUL;
  } else {
    *dict_size =
        ((uint32_t)(2U | (prop & 1U))) << ((uint32_t)(prop / 2U) + 11U);
  }
  return 1;
}

static int zz9k_archive_7z_read_byte(ZZ9KArchive7zCursor *cursor,
                                     uint8_t *value)
{
  if (!cursor || !value || cursor->pos >= cursor->size) {
    return 0;
  }
  *value = cursor->data[cursor->pos++];
  return 1;
}

static int zz9k_archive_7z_skip(ZZ9KArchive7zCursor *cursor,
                                uint64_t length)
{
  if (!cursor || length > 0x7fffffffULL ||
      cursor->pos > cursor->size ||
      (uint32_t)length > cursor->size - cursor->pos) {
    return 0;
  }
  cursor->pos += (uint32_t)length;
  return 1;
}

static int zz9k_archive_7z_read_number(ZZ9KArchive7zCursor *cursor,
                                       uint64_t *value)
{
  uint8_t first;
  uint8_t mask;
  uint32_t i;
  uint64_t out;

  if (!value || !zz9k_archive_7z_read_byte(cursor, &first)) {
    return 0;
  }
  if ((first & 0x80U) == 0U) {
    *value = first;
    return 1;
  }

  out = 0U;
  mask = 0x80U;
  for (i = 0U; i < 8U; i++) {
    uint8_t next_mask = (uint8_t)(mask >> 1U);

    if ((first & mask) == 0U) {
      out |= (uint64_t)(first & (mask - 1U)) << (8U * i);
      *value = out;
      return 1;
    }
    {
      uint8_t b;

      if (!zz9k_archive_7z_read_byte(cursor, &b)) {
        return 0;
      }
      out |= (uint64_t)b << (8U * i);
    }
    mask = next_mask;
  }

  *value = out;
  return 1;
}

static int zz9k_archive_7z_skip_property(ZZ9KArchive7zCursor *cursor)
{
  uint64_t size;

  if (!zz9k_archive_7z_read_number(cursor, &size)) {
    return 0;
  }
  return zz9k_archive_7z_skip(cursor, size);
}

static int zz9k_archive_7z_bit_is_set(const uint8_t *bits, uint32_t bit)
{
  return bits && (bits[bit >> 3U] & (0x80U >> (bit & 7U))) != 0U;
}

static uint32_t zz9k_archive_7z_count_bits(const uint8_t *bits,
                                           uint32_t bit_count)
{
  uint32_t i;
  uint32_t count = 0U;

  if (!bits) {
    return 0U;
  }
  for (i = 0U; i < bit_count; i++) {
    if (zz9k_archive_7z_bit_is_set(bits, i)) {
      count++;
    }
  }
  return count;
}

static int zz9k_archive_7z_read_digests(ZZ9KArchive7zCursor *cursor,
                                        uint32_t item_count,
                                        uint32_t *crcs,
                                        uint8_t *defined)
{
  uint8_t all_defined;
  uint32_t defined_count = item_count;
  const uint8_t *defined_bits = 0;
  uint32_t i;

  if (!zz9k_archive_7z_read_byte(cursor, &all_defined)) {
    return 0;
  }
  if (all_defined == 0U) {
    uint32_t bit_bytes = (item_count + 7U) >> 3U;

    if (cursor->pos > cursor->size ||
        bit_bytes > cursor->size - cursor->pos) {
      return 0;
    }
    defined_count = zz9k_archive_7z_count_bits(
        cursor->data + cursor->pos, item_count);
    defined_bits = cursor->data + cursor->pos;
    cursor->pos += bit_bytes;
  }
  if (defined_count > 0x1fffffffUL ||
      cursor->pos > cursor->size ||
      defined_count * 4U > cursor->size - cursor->pos) {
    return 0;
  }
  for (i = 0U; i < item_count; i++) {
    int is_defined = all_defined != 0U ||
        zz9k_archive_7z_bit_is_set(defined_bits, i);

    if (defined) {
      defined[i] = (uint8_t)(is_defined ? 1U : 0U);
    }
    if (is_defined) {
      uint32_t crc = zz9k_archive_get_le32(cursor->data + cursor->pos);

      cursor->pos += 4U;
      if (crcs) {
        crcs[i] = crc;
      }
    } else if (crcs) {
      crcs[i] = 0U;
    }
  }
  return 1;
}

static int zz9k_archive_7z_skip_digests(ZZ9KArchive7zCursor *cursor,
                                        uint32_t item_count)
{
  return zz9k_archive_7z_read_digests(cursor, item_count, 0, 0);
}

static void zz9k_archive_7z_clear_parse_diagnostic(void)
{
  zz9k_archive_7z_last_parse_diagnostic = 0;
}

static void zz9k_archive_7z_set_parse_diagnostic(const char *diagnostic)
{
  if (!zz9k_archive_7z_last_parse_diagnostic) {
    zz9k_archive_7z_last_parse_diagnostic = diagnostic;
  }
}

static const char *zz9k_archive_7z_parse_diagnostic(void)
{
  return zz9k_archive_7z_last_parse_diagnostic;
}

static void zz9k_archive_print_7z_parse_failure(void)
{
  const char *diagnostic = zz9k_archive_7z_parse_diagnostic();

  if (diagnostic) {
    printf("7z unsupported layout: %s\n", diagnostic);
  } else {
    printf("7z parse failed or unsupported layout\n");
  }
}

static int zz9k_archive_7z_copy_utf16_name(const uint8_t *names,
                                           uint32_t names_size,
                                           uint32_t *names_pos,
                                           char *dst,
                                           uint32_t dst_capacity)
{
  uint32_t out = 0U;

  if (!names || !names_pos || !dst || dst_capacity == 0U) {
    return 0;
  }
  while (*names_pos + 1U < names_size) {
    uint8_t low = names[*names_pos];
    uint8_t high = names[*names_pos + 1U];
    char ch;

    *names_pos += 2U;
    if (low == 0U && high == 0U) {
      if (out == 0U || out >= dst_capacity) {
        return 0;
      }
      dst[out] = '\0';
      zz9k_archive_strip_current_dir_prefix(dst);
      return 1;
    }
    if (out + 1U >= dst_capacity) {
      return 0;
    }
    ch = (high == 0U && low >= 0x20U) ? (char)low : '?';
    if (ch == '\\') {
      ch = '/';
    }
    dst[out++] = ch;
  }
  return 0;
}

static int zz9k_archive_7z_start_header_from_prefix(
    const uint8_t *data,
    uint32_t prefix_length,
    uint32_t archive_length,
    ZZ9KArchive7zHeader *header)
{
  uint64_t relative_offset;
  uint64_t header_size;
  uint32_t start_header_crc;
  uint32_t next_header_crc;
  uint32_t next_header_offset;
  uint32_t next_header_size;

  if (!data || !header ||
      prefix_length < ZZ9K_ARCHIVE_7Z_START_HEADER_SIZE ||
      archive_length < ZZ9K_ARCHIVE_7Z_START_HEADER_SIZE ||
      data[0] != 0x37U || data[1] != 0x7aU ||
      data[2] != 0xbcU || data[3] != 0xafU ||
      data[4] != 0x27U || data[5] != 0x1cU ||
      data[6] != 0U) {
    return 0;
  }
  start_header_crc = zz9k_archive_get_le32(data + 8U);
  if (zz9k_archive_crc32(0U, data + 12U, 20U) != start_header_crc) {
    return 0;
  }

  relative_offset = zz9k_archive_get_le64(data + 12U);
  header_size = zz9k_archive_get_le64(data + 20U);
  if (relative_offset > 0x7fffffffULL ||
      header_size > 0x7fffffffULL ||
      relative_offset + header_size < relative_offset ||
      ZZ9K_ARCHIVE_7Z_START_HEADER_SIZE + relative_offset >
        0x7fffffffULL ||
      ZZ9K_ARCHIVE_7Z_START_HEADER_SIZE + relative_offset + header_size >
        archive_length) {
    return 0;
  }

  next_header_offset =
      (uint32_t)(ZZ9K_ARCHIVE_7Z_START_HEADER_SIZE + relative_offset);
  next_header_size = (uint32_t)header_size;
  next_header_crc = zz9k_archive_get_le32(data + 28U);
  if (next_header_size == 0U) {
    return 0;
  }
  if (next_header_offset <= prefix_length &&
      next_header_size <= prefix_length - next_header_offset &&
      zz9k_archive_crc32(0U, data + next_header_offset, next_header_size) !=
          next_header_crc) {
    return 0;
  }

  header->next_header_offset = next_header_offset;
  header->next_header_size = next_header_size;
  header->next_header_crc = next_header_crc;
  return header->next_header_size != 0U;
}

static int zz9k_archive_7z_start_header(const uint8_t *data,
                                        uint32_t length,
                                        ZZ9KArchive7zHeader *header)
{
  return zz9k_archive_7z_start_header_from_prefix(
      data, length, length, header);
}

static int zz9k_archive_7z_header_is_encoded(const uint8_t *data,
                                             uint32_t length)
{
  ZZ9KArchive7zHeader header;

  if (!zz9k_archive_7z_start_header(data, length, &header) ||
      header.next_header_size == 0U ||
      header.next_header_offset >= length) {
    return 0;
  }
  return data[header.next_header_offset] ==
         ZZ9K_ARCHIVE_7Z_ID_ENCODED_HEADER;
}

static void zz9k_archive_7z_streams_init(ZZ9KArchive7zStreams *streams)
{
  if (streams) {
    memset(streams, 0, sizeof(*streams));
  }
}

static void zz9k_archive_7z_streams_free(ZZ9KArchive7zStreams *streams)
{
  if (!streams) {
    return;
  }
  free(streams->data_offsets);
  free(streams->decoded_offsets);
  free(streams->pack_sizes);
  free(streams->unpack_sizes);
  free(streams->crcs);
  free(streams->methods);
  free(streams->flags);
  free(streams->crc_defined);
  free(streams->props_sizes);
  free(streams->props);
  zz9k_archive_7z_streams_init(streams);
}

static int zz9k_archive_7z_streams_alloc(ZZ9KArchive7zStreams *streams,
                                         uint32_t count)
{
  if (!streams || count == 0U || count > 65535U) {
    return 0;
  }
  zz9k_archive_7z_streams_free(streams);
  streams->data_offsets = (uint32_t *)calloc((size_t)count,
                                             sizeof(uint32_t));
  streams->decoded_offsets = (uint32_t *)calloc((size_t)count,
                                                sizeof(uint32_t));
  streams->pack_sizes = (uint32_t *)calloc((size_t)count,
                                           sizeof(uint32_t));
  streams->unpack_sizes = (uint32_t *)calloc((size_t)count,
                                             sizeof(uint32_t));
  streams->crcs = (uint32_t *)calloc((size_t)count,
                                     sizeof(uint32_t));
  streams->methods = (uint32_t *)calloc((size_t)count,
                                        sizeof(uint32_t));
  streams->flags = (uint32_t *)calloc((size_t)count,
                                      sizeof(uint32_t));
  streams->crc_defined = (uint8_t *)calloc((size_t)count,
                                           sizeof(uint8_t));
  streams->props_sizes = (uint8_t *)calloc((size_t)count,
                                           sizeof(uint8_t));
  streams->props = (uint8_t *)calloc((size_t)count,
                                     ZZ9K_ARCHIVE_7Z_MAX_METHOD_PROPS);
  if (!streams->data_offsets || !streams->decoded_offsets ||
      !streams->pack_sizes ||
      !streams->unpack_sizes || !streams->crcs || !streams->methods ||
      !streams->flags || !streams->crc_defined ||
      !streams->props_sizes || !streams->props) {
    zz9k_archive_7z_streams_free(streams);
    return 0;
  }
  streams->count = count;
  return 1;
}

static int zz9k_archive_7z_parse_pack_info(ZZ9KArchive7zCursor *cursor,
                                           uint32_t archive_length,
                                           ZZ9KArchive7zStreams *streams)
{
  uint64_t pack_pos64;
  uint64_t stream_count64;
  uint64_t type;
  uint32_t stream_count;
  uint32_t data_pos;
  uint32_t i;

  if (!cursor ||
      !zz9k_archive_7z_read_number(cursor, &pack_pos64) ||
      !zz9k_archive_7z_read_number(cursor, &stream_count64) ||
      pack_pos64 > 0x7fffffffULL ||
      stream_count64 > 65535ULL) {
    return 0;
  }
  stream_count = (uint32_t)stream_count64;
  if (!zz9k_archive_7z_streams_alloc(streams, stream_count)) {
    return 0;
  }
  if (!zz9k_archive_7z_read_number(cursor, &type) ||
      type != ZZ9K_ARCHIVE_7Z_ID_SIZE) {
    return 0;
  }

  data_pos = ZZ9K_ARCHIVE_7Z_START_HEADER_SIZE + (uint32_t)pack_pos64;
  for (i = 0U; i < stream_count; i++) {
    uint64_t size64;

    if (!zz9k_archive_7z_read_number(cursor, &size64) ||
        size64 > 0x7fffffffULL ||
        data_pos > archive_length ||
        (uint32_t)size64 > archive_length - data_pos) {
      return 0;
    }
    streams->data_offsets[i] = data_pos;
    streams->pack_sizes[i] = (uint32_t)size64;
    data_pos += (uint32_t)size64;
  }

  while (1) {
    if (!zz9k_archive_7z_read_number(cursor, &type)) {
      return 0;
    }
    if (type == ZZ9K_ARCHIVE_7Z_ID_END) {
      return 1;
    }
    if (type == ZZ9K_ARCHIVE_7Z_ID_CRC) {
      if (!zz9k_archive_7z_skip_digests(cursor, stream_count)) {
        return 0;
      }
    } else {
      return 0;
    }
  }
}

static int zz9k_archive_7z_read_simple_folder(ZZ9KArchive7zCursor *cursor,
                                              uint32_t *method,
                                              uint8_t *props,
                                              uint8_t *props_size)
{
  uint64_t num_coders;
  uint8_t main_byte;
  uint32_t id_size;
  uint32_t parsed_method = 0U;
  uint32_t i;

  if (!method || !props || !props_size) {
    return 0;
  }
  *method = 0U;
  *props_size = 0U;
  memset(props, 0, ZZ9K_ARCHIVE_7Z_MAX_METHOD_PROPS);

  if (!zz9k_archive_7z_read_number(cursor, &num_coders)) {
    return 0;
  }
  if (num_coders != 1U) {
    zz9k_archive_7z_set_parse_diagnostic(
        "7z multi-coder/filter-chain folders unsupported");
    return 0;
  }
  if (!zz9k_archive_7z_read_byte(cursor, &main_byte)) {
    return 0;
  }
  if ((main_byte & 0xc0U) != 0U) {
    zz9k_archive_7z_set_parse_diagnostic(
        "7z coder attributes unsupported");
    return 0;
  }

  id_size = main_byte & 0x0fU;
  if (id_size == 0U || id_size > 4U ||
      cursor->pos > cursor->size ||
      id_size > cursor->size - cursor->pos) {
    return 0;
  }
  for (i = 0U; i < id_size; i++) {
    parsed_method = (parsed_method << 8U) | cursor->data[cursor->pos++];
  }
  if ((main_byte & 0x10U) != 0U) {
    uint64_t in_streams;
    uint64_t out_streams;

    if (!zz9k_archive_7z_read_number(cursor, &in_streams) ||
        !zz9k_archive_7z_read_number(cursor, &out_streams)) {
      return 0;
    }
    if (in_streams != 1U || out_streams != 1U) {
      zz9k_archive_7z_set_parse_diagnostic(
          "7z multiple input/output streams unsupported");
      return 0;
    }
  }
  if ((main_byte & 0x20U) != 0U) {
    uint64_t parsed_props_size;

    if (!zz9k_archive_7z_read_number(cursor, &parsed_props_size) ||
        parsed_props_size > ZZ9K_ARCHIVE_7Z_MAX_METHOD_PROPS ||
        cursor->pos > cursor->size ||
        (uint32_t)parsed_props_size > cursor->size - cursor->pos) {
      return 0;
    }
    memcpy(props, cursor->data + cursor->pos, (uint32_t)parsed_props_size);
    cursor->pos += (uint32_t)parsed_props_size;
    *props_size = (uint8_t)parsed_props_size;
  }
  if (parsed_method == ZZ9K_ARCHIVE_7Z_METHOD_COPY) {
    if (*props_size != 0U) {
      zz9k_archive_7z_set_parse_diagnostic(
          "7z Copy method properties unsupported");
      return 0;
    }
  } else if (parsed_method == ZZ9K_ARCHIVE_7Z_METHOD_DEFLATE) {
    if (*props_size != 0U) {
      zz9k_archive_7z_set_parse_diagnostic(
          "7z Deflate method properties unsupported");
      return 0;
    }
  } else if (parsed_method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA) {
    if (*props_size != 5U) {
      zz9k_archive_7z_set_parse_diagnostic(
          "7z LZMA property size unsupported");
      return 0;
    }
  } else if (parsed_method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA2) {
    if (*props_size != 1U) {
      zz9k_archive_7z_set_parse_diagnostic(
          "7z LZMA2 property size unsupported");
      return 0;
    }
  } else {
    zz9k_archive_7z_set_parse_diagnostic("7z folder method unsupported");
    return 0;
  }
  *method = parsed_method;
  return 1;
}

static int zz9k_archive_7z_parse_unpack_info(ZZ9KArchive7zCursor *cursor,
                                             ZZ9KArchive7zStreams *streams)
{
  uint64_t type;
  uint64_t folder_count64;
  uint8_t external;
  uint32_t i;

  if (!cursor || !streams || streams->count == 0U ||
      !zz9k_archive_7z_read_number(cursor, &type) ||
      type != ZZ9K_ARCHIVE_7Z_ID_FOLDER ||
      !zz9k_archive_7z_read_number(cursor, &folder_count64) ||
      folder_count64 != streams->count ||
      !zz9k_archive_7z_read_byte(cursor, &external) ||
      external != 0U) {
    return 0;
  }

  for (i = 0U; i < streams->count; i++) {
    uint8_t *props;
    uint32_t method;
    uint8_t props_size;

    props = streams->props +
            i * ZZ9K_ARCHIVE_7Z_MAX_METHOD_PROPS;
    if (!zz9k_archive_7z_read_simple_folder(cursor, &method,
                                            props, &props_size)) {
      return 0;
    }
    streams->methods[i] = method;
    streams->props_sizes[i] = props_size;
  }

  if (!zz9k_archive_7z_read_number(cursor, &type) ||
      type != ZZ9K_ARCHIVE_7Z_ID_CODERS_UNPACK_SIZE) {
    return 0;
  }
  for (i = 0U; i < streams->count; i++) {
    uint64_t unpack_size;

    if (!zz9k_archive_7z_read_number(cursor, &unpack_size) ||
        unpack_size > 0x7fffffffULL) {
      return 0;
    }
    streams->unpack_sizes[i] = (uint32_t)unpack_size;
  }
  while (1) {
    if (!zz9k_archive_7z_read_number(cursor, &type)) {
      return 0;
    }
    if (type == ZZ9K_ARCHIVE_7Z_ID_END) {
      return 1;
    }
    if (type == ZZ9K_ARCHIVE_7Z_ID_CRC) {
      if (!zz9k_archive_7z_read_digests(
              cursor, streams->count, streams->crcs,
              streams->crc_defined)) {
        return 0;
      }
    } else {
      return 0;
    }
  }
}

static int zz9k_archive_7z_streams_flatten_substreams(
    ZZ9KArchive7zStreams *streams,
    const uint32_t *substream_counts,
    const uint32_t *substream_sizes,
    const uint32_t *substream_crcs,
    const uint8_t *substream_crc_defined,
    uint32_t substream_count)
{
  uint32_t folder_count;
  uint32_t *data_offsets = 0;
  uint32_t *decoded_offsets = 0;
  uint32_t *pack_sizes = 0;
  uint32_t *unpack_sizes = 0;
  uint32_t *crcs = 0;
  uint32_t *methods = 0;
  uint32_t *flags = 0;
  uint8_t *crc_defined = 0;
  uint8_t *props_sizes = 0;
  uint8_t *props = 0;
  uint32_t folder;
  uint32_t out = 0U;
  int ok = 0;

  if (!streams || !substream_counts || !substream_sizes ||
      substream_count == 0U || substream_count > 65535U) {
    return 0;
  }
  folder_count = streams->count;
  data_offsets = (uint32_t *)calloc((size_t)substream_count,
                                    sizeof(uint32_t));
  decoded_offsets = (uint32_t *)calloc((size_t)substream_count,
                                       sizeof(uint32_t));
  pack_sizes = (uint32_t *)calloc((size_t)substream_count,
                                  sizeof(uint32_t));
  unpack_sizes = (uint32_t *)calloc((size_t)substream_count,
                                    sizeof(uint32_t));
  crcs = (uint32_t *)calloc((size_t)substream_count, sizeof(uint32_t));
  methods = (uint32_t *)calloc((size_t)substream_count, sizeof(uint32_t));
  flags = (uint32_t *)calloc((size_t)substream_count, sizeof(uint32_t));
  crc_defined = (uint8_t *)calloc((size_t)substream_count,
                                  sizeof(uint8_t));
  props_sizes = (uint8_t *)calloc((size_t)substream_count,
                                  sizeof(uint8_t));
  props = (uint8_t *)calloc((size_t)substream_count,
                            ZZ9K_ARCHIVE_7Z_MAX_METHOD_PROPS);
  if (!data_offsets || !decoded_offsets || !pack_sizes || !unpack_sizes ||
      !crcs || !methods || !flags || !crc_defined || !props_sizes || !props) {
    goto out;
  }

  for (folder = 0U; folder < folder_count; folder++) {
    uint32_t count = substream_counts[folder];
    uint32_t offset = streams->data_offsets[folder];
    uint32_t total = 0U;
    int unsupported_split = 0;
    uint32_t i;

    if (count == 0U || out + count < out || out + count > substream_count) {
      goto out;
    }
    if (count > 1U &&
        (streams->methods[folder] != ZZ9K_ARCHIVE_7Z_METHOD_COPY ||
         streams->pack_sizes[folder] != streams->unpack_sizes[folder])) {
      unsupported_split = 1;
    }
    for (i = 0U; i < count; i++) {
      uint32_t size = substream_sizes[out + i];

      if (total > streams->unpack_sizes[folder] ||
          size > streams->unpack_sizes[folder] - total) {
        goto out;
      }
      data_offsets[out + i] = unsupported_split ? streams->data_offsets[folder] :
          (count == 1U ? streams->data_offsets[folder] : offset);
      decoded_offsets[out + i] = total;
      pack_sizes[out + i] = unsupported_split ? streams->pack_sizes[folder] :
          (streams->methods[folder] == ZZ9K_ARCHIVE_7Z_METHOD_COPY ?
           size : streams->pack_sizes[folder]);
      unpack_sizes[out + i] = size;
      methods[out + i] = streams->methods[folder];
      flags[out + i] = streams->flags[folder];
      if (unsupported_split) {
        flags[out + i] |= ZZ9K_ARCHIVE_ENTRY_FLAG_7Z_UNSUPPORTED_SPLIT;
      }
      props_sizes[out + i] = streams->props_sizes[folder];
      if (props_sizes[out + i] != 0U) {
        memcpy(props + (out + i) * ZZ9K_ARCHIVE_7Z_MAX_METHOD_PROPS,
               streams->props + folder * ZZ9K_ARCHIVE_7Z_MAX_METHOD_PROPS,
               props_sizes[out + i]);
      }
      if (substream_crc_defined && substream_crc_defined[out + i]) {
        crcs[out + i] = substream_crcs[out + i];
        crc_defined[out + i] = 1U;
      } else if (count == 1U && streams->crc_defined[folder]) {
        crcs[out + i] = streams->crcs[folder];
        crc_defined[out + i] = 1U;
      }
      offset += size;
      total += size;
    }
    if (total != streams->unpack_sizes[folder]) {
      goto out;
    }
    out += count;
  }
  if (out != substream_count) {
    goto out;
  }

  free(streams->data_offsets);
  free(streams->decoded_offsets);
  free(streams->pack_sizes);
  free(streams->unpack_sizes);
  free(streams->crcs);
  free(streams->methods);
  free(streams->flags);
  free(streams->crc_defined);
  free(streams->props_sizes);
  free(streams->props);
  streams->data_offsets = data_offsets;
  streams->decoded_offsets = decoded_offsets;
  streams->pack_sizes = pack_sizes;
  streams->unpack_sizes = unpack_sizes;
  streams->crcs = crcs;
  streams->methods = methods;
  streams->flags = flags;
  streams->crc_defined = crc_defined;
  streams->props_sizes = props_sizes;
  streams->props = props;
  streams->count = substream_count;
  data_offsets = 0;
  decoded_offsets = 0;
  pack_sizes = 0;
  unpack_sizes = 0;
  crcs = 0;
  methods = 0;
  flags = 0;
  crc_defined = 0;
  props_sizes = 0;
  props = 0;
  ok = 1;

out:
  free(data_offsets);
  free(decoded_offsets);
  free(pack_sizes);
  free(unpack_sizes);
  free(crcs);
  free(methods);
  free(flags);
  free(crc_defined);
  free(props_sizes);
  free(props);
  return ok;
}

static int zz9k_archive_7z_parse_substreams_info(
    ZZ9KArchive7zCursor *cursor,
    ZZ9KArchive7zStreams *streams)
{
  uint64_t type;
  uint32_t folder_count;
  uint32_t substream_count;
  uint32_t *substream_counts = 0;
  uint32_t *substream_sizes = 0;
  uint32_t *substream_crcs = 0;
  uint8_t *substream_crc_defined = 0;
  int saw_size = 0;
  int ok = 0;
  uint32_t i;

  if (!cursor || !streams) {
    return 0;
  }
  folder_count = streams->count;
  substream_count = folder_count;
  substream_counts = (uint32_t *)calloc((size_t)folder_count,
                                        sizeof(uint32_t));
  if (!substream_counts) {
    return 0;
  }
  for (i = 0U; i < folder_count; i++) {
    substream_counts[i] = 1U;
  }

  while (1) {
    if (!zz9k_archive_7z_read_number(cursor, &type)) {
      goto out;
    }
    if (type == ZZ9K_ARCHIVE_7Z_ID_END) {
      uint32_t i;

      if (!saw_size) {
        substream_sizes = (uint32_t *)calloc((size_t)substream_count,
                                             sizeof(uint32_t));
        if (!substream_sizes) {
          goto out;
        }
        for (i = 0U; i < folder_count; i++) {
          if (substream_counts[i] != 1U) {
            goto out;
          }
          substream_sizes[i] = streams->unpack_sizes[i];
        }
      }
      ok = zz9k_archive_7z_streams_flatten_substreams(
          streams, substream_counts, substream_sizes, substream_crcs,
          substream_crc_defined, substream_count);
      goto out;
    }
    if (type == ZZ9K_ARCHIVE_7Z_ID_NUM_UNPACK_STREAM) {
      uint32_t i;

      if (saw_size || substream_crcs || substream_crc_defined) {
        goto out;
      }
      substream_count = 0U;
      for (i = 0U; i < streams->count; i++) {
        uint64_t count64;

        if (!zz9k_archive_7z_read_number(cursor, &count64) ||
            count64 == 0U || count64 > 65535ULL ||
            substream_count + (uint32_t)count64 < substream_count ||
            substream_count + (uint32_t)count64 > 65535U) {
          goto out;
        }
        substream_counts[i] = (uint32_t)count64;
        substream_count += (uint32_t)count64;
      }
    } else if (type == ZZ9K_ARCHIVE_7Z_ID_SIZE) {
      uint32_t out_index = 0U;
      uint32_t folder;

      if (saw_size) {
        goto out;
      }
      substream_sizes = (uint32_t *)calloc((size_t)substream_count,
                                           sizeof(uint32_t));
      if (!substream_sizes) {
        goto out;
      }
      for (folder = 0U; folder < folder_count; folder++) {
        uint32_t count = substream_counts[folder];
        uint32_t total = 0U;
        uint32_t i;

        for (i = 0U; i + 1U < count; i++) {
          uint64_t size64;

          if (!zz9k_archive_7z_read_number(cursor, &size64) ||
              size64 > 0x7fffffffULL ||
              total + (uint32_t)size64 < total ||
              total + (uint32_t)size64 >
                streams->unpack_sizes[folder]) {
            goto out;
          }
          substream_sizes[out_index++] = (uint32_t)size64;
          total += (uint32_t)size64;
        }
        substream_sizes[out_index++] = streams->unpack_sizes[folder] - total;
      }
      if (out_index != substream_count) {
        goto out;
      }
      saw_size = 1;
    } else if (type == ZZ9K_ARCHIVE_7Z_ID_CRC) {
      if (substream_crcs || substream_crc_defined) {
        goto out;
      }
      substream_crcs = (uint32_t *)calloc((size_t)substream_count,
                                          sizeof(uint32_t));
      substream_crc_defined = (uint8_t *)calloc((size_t)substream_count,
                                                sizeof(uint8_t));
      if (!substream_crcs || !substream_crc_defined ||
          !zz9k_archive_7z_read_digests(
              cursor, substream_count, substream_crcs,
              substream_crc_defined)) {
        goto out;
      }
    } else {
      goto out;
    }
  }

out:
  free(substream_counts);
  free(substream_sizes);
  free(substream_crcs);
  free(substream_crc_defined);
  return ok;
}

static int zz9k_archive_7z_parse_streams_info(
    ZZ9KArchive7zCursor *cursor,
    uint32_t archive_length,
    ZZ9KArchive7zStreams *streams)
{
  uint64_t type;
  int saw_pack = 0;
  int saw_unpack = 0;

  if (!cursor || !streams) {
    return 0;
  }
  while (1) {
    if (!zz9k_archive_7z_read_number(cursor, &type)) {
      return 0;
    }
    if (type == ZZ9K_ARCHIVE_7Z_ID_END) {
      return saw_pack && saw_unpack;
    }
    if (type == ZZ9K_ARCHIVE_7Z_ID_PACK_INFO) {
      if (saw_pack ||
          !zz9k_archive_7z_parse_pack_info(
              cursor, archive_length, streams)) {
        return 0;
      }
      saw_pack = 1;
    } else if (type == ZZ9K_ARCHIVE_7Z_ID_UNPACK_INFO) {
      if (!saw_pack || saw_unpack ||
          !zz9k_archive_7z_parse_unpack_info(cursor, streams)) {
        return 0;
      }
      saw_unpack = 1;
    } else if (type == ZZ9K_ARCHIVE_7Z_ID_SUBSTREAMS_INFO) {
      if (!saw_unpack ||
          !zz9k_archive_7z_parse_substreams_info(cursor, streams)) {
        return 0;
      }
    } else {
      return 0;
    }
  }
}

static int zz9k_archive_7z_files_info_property_supported(uint64_t type)
{
  switch ((uint32_t)type) {
  case ZZ9K_ARCHIVE_7Z_ID_CREATION_TIME:
  case ZZ9K_ARCHIVE_7Z_ID_LAST_ACCESS_TIME:
  case ZZ9K_ARCHIVE_7Z_ID_LAST_WRITE_TIME:
  case ZZ9K_ARCHIVE_7Z_ID_WIN_ATTRIBUTES:
  case ZZ9K_ARCHIVE_7Z_ID_COMMENT:
  case ZZ9K_ARCHIVE_7Z_ID_START_POS:
  case ZZ9K_ARCHIVE_7Z_ID_DUMMY:
    return 1;
  default:
    return 0;
  }
}

static int zz9k_archive_7z_parse_files_info(
    ZZ9KArchive7zCursor *cursor,
    const ZZ9KArchive7zStreams *streams,
    ZZ9KArchiveEntry *entries,
    uint32_t entry_capacity,
    uint32_t *entry_count)
{
  uint64_t num_files64;
  uint32_t num_files;
  const uint8_t *empty_streams = 0;
  const uint8_t *empty_files = 0;
  const uint8_t *names = 0;
  uint32_t empty_streams_size = 0U;
  uint32_t empty_files_size = 0U;
  uint32_t names_size = 0U;
  uint32_t empty_count = 0U;
  uint32_t names_pos = 0U;
  uint32_t empty_index = 0U;
  uint32_t stream_index = 0U;
  uint32_t count = 0U;
  uint32_t i;

  if (!cursor || !entry_count ||
      !zz9k_archive_7z_read_number(cursor, &num_files64) ||
      num_files64 > 65535ULL) {
    return 0;
  }
  num_files = (uint32_t)num_files64;

  while (1) {
    uint64_t type;
    uint64_t size64;
    const uint8_t *property;
    uint32_t property_size;

    if (!zz9k_archive_7z_read_number(cursor, &type)) {
      return 0;
    }
    if (type == ZZ9K_ARCHIVE_7Z_ID_END) {
      break;
    }
    if (!zz9k_archive_7z_read_number(cursor, &size64) ||
        size64 > 0x7fffffffULL ||
        cursor->pos > cursor->size ||
        (uint32_t)size64 > cursor->size - cursor->pos) {
      return 0;
    }
    property = cursor->data + cursor->pos;
    property_size = (uint32_t)size64;
    cursor->pos += property_size;

    if (type == ZZ9K_ARCHIVE_7Z_ID_EMPTY_STREAM) {
      if (property_size < ((num_files + 7U) >> 3U)) {
        return 0;
      }
      empty_streams = property;
      empty_streams_size = property_size;
      empty_count = zz9k_archive_7z_count_bits(empty_streams, num_files);
    } else if (type == ZZ9K_ARCHIVE_7Z_ID_EMPTY_FILE) {
      empty_files = property;
      empty_files_size = property_size;
    } else if (type == ZZ9K_ARCHIVE_7Z_ID_NAME) {
      if (property_size < 1U || property[0] != 0U) {
        return 0;
      }
      names = property + 1U;
      names_size = property_size - 1U;
    } else if (!zz9k_archive_7z_files_info_property_supported(type)) {
      return 0;
    }
  }

  if (num_files == 0U) {
    *entry_count = 0U;
    return 1;
  }
  if (!names ||
      (empty_streams && empty_streams_size < ((num_files + 7U) >> 3U)) ||
      (empty_files && empty_files_size < ((empty_count + 7U) >> 3U))) {
    return 0;
  }

  for (i = 0U; i < num_files; i++) {
    int is_empty = empty_streams ?
        zz9k_archive_7z_bit_is_set(empty_streams, i) : 0;
    ZZ9KArchiveEntry candidate;

    memset(&candidate, 0, sizeof(candidate));
    if (!zz9k_archive_7z_copy_utf16_name(
            names, names_size, &names_pos,
            candidate.name, sizeof(candidate.name))) {
      return 0;
    }
    candidate.method = ZZ9K_ARCHIVE_7Z_METHOD_COPY;
    if (is_empty) {
      candidate.is_dir = empty_files ?
          (zz9k_archive_7z_bit_is_set(empty_files, empty_index) ? 0U : 1U) :
          1U;
    } else {
      if (!streams || stream_index >= streams->count) {
        return 0;
      }
      candidate.data_offset = streams->data_offsets[stream_index];
      candidate.decoded_offset = streams->decoded_offsets[stream_index];
      candidate.compressed_size = streams->pack_sizes[stream_index];
      candidate.uncompressed_size = streams->unpack_sizes[stream_index];
      candidate.flags = streams->flags[stream_index];
      if (streams->crc_defined[stream_index]) {
        candidate.crc32 = streams->crcs[stream_index];
        candidate.flags |= ZZ9K_ARCHIVE_ENTRY_FLAG_CRC32;
      }
      candidate.method = streams->methods[stream_index];
      candidate.method_props_size = streams->props_sizes[stream_index];
      if (candidate.method_props_size != 0U) {
        memcpy(candidate.method_props,
               streams->props +
               stream_index * ZZ9K_ARCHIVE_7Z_MAX_METHOD_PROPS,
               candidate.method_props_size);
      }
      stream_index++;
    }
    if (is_empty) {
      empty_index++;
    }
    if (zz9k_archive_7z_entry_is_root_metadata(&candidate)) {
      continue;
    }
    if (entries) {
      if (count >= entry_capacity) {
        return 0;
      }
      entries[count] = candidate;
    }
    count++;
  }
  if (streams && stream_index != streams->count) {
    return 0;
  }

  *entry_count = count;
  return 1;
}

static int zz9k_archive_7z_encoded_header_entry(
    const uint8_t *header_data,
    uint32_t header_length,
    uint32_t archive_length,
    ZZ9KArchiveEntry *entry)
{
  ZZ9KArchive7zCursor cursor;
  ZZ9KArchive7zStreams streams;
  uint64_t type;
  int ok = 0;

  if (!header_data || header_length == 0U || !entry) {
    return 0;
  }
  memset(entry, 0, sizeof(*entry));
  zz9k_archive_7z_streams_init(&streams);

  cursor.data = header_data;
  cursor.size = header_length;
  cursor.pos = 0U;
  if (!zz9k_archive_7z_read_number(&cursor, &type) ||
      type != ZZ9K_ARCHIVE_7Z_ID_ENCODED_HEADER ||
      !zz9k_archive_7z_parse_streams_info(
          &cursor, archive_length, &streams) ||
      streams.count != 1U ||
      streams.data_offsets[0] > archive_length ||
      streams.pack_sizes[0] > archive_length - streams.data_offsets[0]) {
    goto out;
  }
  strcpy(entry->name, "7z-encoded-header");
  entry->method = streams.methods[0];
  entry->data_offset = streams.data_offsets[0];
  entry->compressed_size = streams.pack_sizes[0];
  entry->uncompressed_size = streams.unpack_sizes[0];
  entry->flags = streams.flags[0];
  entry->method_props_size = streams.props_sizes[0];
  if (entry->method_props_size != 0U) {
    memcpy(entry->method_props, streams.props,
           entry->method_props_size);
  }
  if (streams.crc_defined[0]) {
    entry->crc32 = streams.crcs[0];
    entry->flags |= ZZ9K_ARCHIVE_ENTRY_FLAG_CRC32;
  }
  ok = 1;

out:
  zz9k_archive_7z_streams_free(&streams);
  return ok;
}

static int zz9k_archive_7z_copy_encoded_header(
    const uint8_t *header_data,
    uint32_t header_length,
    const uint8_t *archive_data,
    uint32_t archive_length,
    uint8_t **decoded_header,
    uint32_t *decoded_length)
{
  ZZ9KArchive7zCursor cursor;
  ZZ9KArchive7zStreams streams;
  uint64_t type;
  uint8_t *bytes = 0;
  int ok = 0;

  if (!header_data || header_length == 0U ||
      !archive_data || !decoded_header || !decoded_length) {
    return 0;
  }
  *decoded_header = 0;
  *decoded_length = 0U;
  zz9k_archive_7z_streams_init(&streams);

  cursor.data = header_data;
  cursor.size = header_length;
  cursor.pos = 0U;
  if (!zz9k_archive_7z_read_number(&cursor, &type) ||
      type != ZZ9K_ARCHIVE_7Z_ID_ENCODED_HEADER ||
      !zz9k_archive_7z_parse_streams_info(
          &cursor, archive_length, &streams) ||
      streams.count != 1U ||
      streams.methods[0] != ZZ9K_ARCHIVE_7Z_METHOD_COPY ||
      streams.pack_sizes[0] != streams.unpack_sizes[0] ||
      streams.data_offsets[0] > archive_length ||
      streams.pack_sizes[0] > archive_length - streams.data_offsets[0]) {
    goto out;
  }
  if (streams.crc_defined[0] &&
      zz9k_archive_crc32(0U, archive_data + streams.data_offsets[0],
                         streams.pack_sizes[0]) != streams.crcs[0]) {
    printf("7z encoded header crc mismatch\n");
    goto out;
  }
  bytes = (uint8_t *)malloc((size_t)streams.unpack_sizes[0]);
  if (!bytes) {
    goto out;
  }
  memcpy(bytes, archive_data + streams.data_offsets[0],
         streams.unpack_sizes[0]);
  *decoded_header = bytes;
  *decoded_length = streams.unpack_sizes[0];
  bytes = 0;
  ok = 1;

out:
  free(bytes);
  zz9k_archive_7z_streams_free(&streams);
  return ok;
}

static int zz9k_archive_7z_copy_encoded_header_from_file(
    const uint8_t *header_data,
    uint32_t header_length,
    const char *archive_path,
    uint32_t archive_length,
    uint8_t **decoded_header,
    uint32_t *decoded_length)
{
  ZZ9KArchive7zCursor cursor;
  ZZ9KArchive7zStreams streams;
  uint64_t type;
  uint8_t *bytes = 0;
  int ok = 0;

  if (!header_data || header_length == 0U || !archive_path ||
      !decoded_header || !decoded_length) {
    return 0;
  }
  *decoded_header = 0;
  *decoded_length = 0U;
  zz9k_archive_7z_streams_init(&streams);

  cursor.data = header_data;
  cursor.size = header_length;
  cursor.pos = 0U;
  if (!zz9k_archive_7z_read_number(&cursor, &type) ||
      type != ZZ9K_ARCHIVE_7Z_ID_ENCODED_HEADER ||
      !zz9k_archive_7z_parse_streams_info(
          &cursor, archive_length, &streams) ||
      streams.count != 1U ||
      streams.methods[0] != ZZ9K_ARCHIVE_7Z_METHOD_COPY ||
      streams.pack_sizes[0] != streams.unpack_sizes[0] ||
      !zz9k_archive_read_file_range(
          archive_path, streams.data_offsets[0], streams.pack_sizes[0],
          &bytes)) {
    goto out;
  }
  if (streams.crc_defined[0] &&
      zz9k_archive_crc32(0U, bytes, streams.pack_sizes[0]) !=
        streams.crcs[0]) {
    printf("7z encoded header crc mismatch: %s\n", archive_path);
    goto out;
  }
  *decoded_header = bytes;
  *decoded_length = streams.unpack_sizes[0];
  bytes = 0;
  ok = 1;

out:
  free(bytes);
  zz9k_archive_7z_streams_free(&streams);
  return ok;
}

static int zz9k_archive_7z_list_from_header(const uint8_t *header_data,
                                            uint32_t header_length,
                                            uint32_t archive_length,
                                            ZZ9KArchiveEntry *entries,
                                            uint32_t entry_capacity,
                                            uint32_t *entry_count)
{
  ZZ9KArchive7zCursor cursor;
  ZZ9KArchive7zStreams streams;
  uint64_t type;
  int ok = 0;

  if (!header_data || header_length == 0U || !entry_count) {
    return 0;
  }
  zz9k_archive_7z_clear_parse_diagnostic();
  zz9k_archive_7z_streams_init(&streams);

  cursor.data = header_data;
  cursor.size = header_length;
  cursor.pos = 0U;
  if (!zz9k_archive_7z_read_number(&cursor, &type) ||
      type != ZZ9K_ARCHIVE_7Z_ID_HEADER) {
    goto out;
  }

  while (1) {
    if (!zz9k_archive_7z_read_number(&cursor, &type)) {
      goto out;
    }
    if (type == ZZ9K_ARCHIVE_7Z_ID_END) {
      *entry_count = 0U;
      ok = 1;
      goto out;
    }
    if (type == ZZ9K_ARCHIVE_7Z_ID_ARCHIVE_PROPERTIES) {
      while (1) {
        if (!zz9k_archive_7z_read_number(&cursor, &type)) {
          goto out;
        }
        if (type == ZZ9K_ARCHIVE_7Z_ID_END) {
          break;
        }
        if (!zz9k_archive_7z_skip_property(&cursor)) {
          goto out;
        }
      }
      continue;
    }
    if (type == ZZ9K_ARCHIVE_7Z_ID_MAIN_STREAMS_INFO) {
      if (!zz9k_archive_7z_parse_streams_info(&cursor, archive_length,
                                              &streams)) {
        goto out;
      }
      continue;
    }
    if (type == ZZ9K_ARCHIVE_7Z_ID_FILES_INFO) {
      uint32_t count;

      if (!zz9k_archive_7z_parse_files_info(
              &cursor, &streams, entries, entry_capacity, &count)) {
        goto out;
      }
      if (!zz9k_archive_7z_read_number(&cursor, &type) ||
          type != ZZ9K_ARCHIVE_7Z_ID_END) {
        goto out;
      }
      *entry_count = count;
      ok = 1;
      goto out;
    }
    if (type == ZZ9K_ARCHIVE_7Z_ID_ADDITIONAL_STREAMS_INFO ||
        type == ZZ9K_ARCHIVE_7Z_ID_ENCODED_HEADER) {
      goto out;
    }
    goto out;
  }

out:
  zz9k_archive_7z_streams_free(&streams);
  return ok;
}

static int zz9k_archive_7z_list(const uint8_t *data,
                                uint32_t length,
                                ZZ9KArchiveEntry *entries,
                                uint32_t entry_capacity,
                                uint32_t *entry_count)
{
  ZZ9KArchive7zHeader header;
  const uint8_t *header_data;
  uint32_t header_length;
  uint8_t *decoded_header = 0;
  int ok;

  zz9k_archive_7z_clear_parse_diagnostic();
  if (!zz9k_archive_7z_start_header(data, length, &header)) {
    return 0;
  }
  header_data = data + header.next_header_offset;
  header_length = header.next_header_size;
  if (header_length != 0U &&
      header_data[0] == ZZ9K_ARCHIVE_7Z_ID_ENCODED_HEADER) {
    if (!zz9k_archive_7z_copy_encoded_header(
            header_data, header_length, data, length,
            &decoded_header, &header_length)) {
      return 0;
    }
    header_data = decoded_header;
  }
  ok = zz9k_archive_7z_list_from_header(
      header_data, header_length, length, entries, entry_capacity,
      entry_count);
  free(decoded_header);
  return ok;
}

static int zz9k_archive_7z_read_header_from_file(const char *path,
                                                 uint32_t archive_length,
                                                 uint8_t **header_data,
                                                 uint32_t *header_length)
{
  uint8_t start[ZZ9K_ARCHIVE_7Z_START_HEADER_SIZE];
  ZZ9KArchive7zHeader header;
  FILE *file = 0;
  uint8_t *bytes = 0;
  int ok = 0;

  if (!path || !header_data || !header_length) {
    return 0;
  }
  *header_data = 0;
  *header_length = 0U;

  file = fopen(path, "rb");
  if (!file) {
    printf("open failed: %s\n", path);
    return 0;
  }
  if (fread(start, 1U, sizeof(start), file) != sizeof(start)) {
    printf("7z start header read failed: %s\n", path);
    goto out;
  }
  if (!zz9k_archive_7z_start_header_from_prefix(
          start, sizeof(start), archive_length, &header)) {
    goto out;
  }
  bytes = (uint8_t *)malloc((size_t)header.next_header_size);
  if (!bytes) {
    printf("7z header allocation failed: %lu bytes\n",
           (unsigned long)header.next_header_size);
    goto out;
  }
  if (fseek(file, (long)header.next_header_offset, SEEK_SET) != 0) {
    printf("7z header seek failed: %s\n", path);
    goto out;
  }
  if (fread(bytes, 1U, header.next_header_size, file) !=
      header.next_header_size) {
    printf("7z header read failed: %s\n", path);
    goto out;
  }
  if (zz9k_archive_crc32(0U, bytes, header.next_header_size) !=
      header.next_header_crc) {
    printf("7z header crc mismatch: %s\n", path);
    goto out;
  }
  if (header.next_header_size != 0U &&
      bytes[0] == ZZ9K_ARCHIVE_7Z_ID_ENCODED_HEADER) {
    uint8_t *decoded = 0;
    uint32_t decoded_size = 0U;

    if (!zz9k_archive_7z_copy_encoded_header_from_file(
            bytes, header.next_header_size, path, archive_length,
            &decoded, &decoded_size)) {
      goto out;
    }
    free(bytes);
    bytes = decoded;
    header.next_header_size = decoded_size;
  }
  *header_data = bytes;
  *header_length = header.next_header_size;
  bytes = 0;
  ok = 1;

out:
  free(bytes);
  if (file) {
    fclose(file);
  }
  return ok;
}

static int zz9k_archive_find_eocd(const uint8_t *data, uint32_t length,
                                  uint32_t *eocd_offset)
{
  uint32_t min;
  uint32_t pos;

  if (!data || length < 22U || !eocd_offset) {
    return 0;
  }
  min = length > (22U + 65535U) ? length - (22U + 65535U) : 0U;
  pos = length - 22U;
  while (1) {
    if (zz9k_archive_get_le32(data + pos) == 0x06054b50UL) {
      uint32_t comment_len = zz9k_archive_get_le16(data + pos + 20U);
      if (pos + 22U + comment_len == length) {
        *eocd_offset = pos;
        return 1;
      }
    }
    if (pos == min) {
      break;
    }
    pos--;
  }
  return 0;
}

static int zz9k_archive_zip_u64_to_supported_u32(uint64_t value,
                                                 uint32_t *out)
{
  if (!out || value > ZZ9K_ARCHIVE_ZIP_SUPPORTED_U32_MAX) {
    return 0;
  }
  *out = (uint32_t)value;
  return 1;
}

static int zz9k_archive_zip_eocd_needs_zip64(const uint8_t *eocd)
{
  if (!eocd) {
    return 0;
  }
  return zz9k_archive_get_le16(eocd + 8U) == 0xffffU ||
         zz9k_archive_get_le16(eocd + 10U) == 0xffffU ||
         zz9k_archive_get_le32(eocd + 12U) == ZZ9K_ARCHIVE_ZIP_U32_SENTINEL ||
         zz9k_archive_get_le32(eocd + 16U) == ZZ9K_ARCHIVE_ZIP_U32_SENTINEL;
}

static int zz9k_archive_zip_parse_zip64_eocd_record(
    const uint8_t *record,
    uint32_t *total_entries,
    uint32_t *cd_size,
    uint32_t *cd_offset)
{
  uint64_t entries_on_disk;
  uint64_t entries_total;

  if (!record || !total_entries || !cd_size || !cd_offset ||
      zz9k_archive_get_le32(record) != 0x06064b50UL ||
      zz9k_archive_get_le64(record + 4U) < 44U ||
      zz9k_archive_get_le32(record + 16U) != 0U ||
      zz9k_archive_get_le32(record + 20U) != 0U) {
    return 0;
  }
  entries_on_disk = zz9k_archive_get_le64(record + 24U);
  entries_total = zz9k_archive_get_le64(record + 32U);
  if (entries_on_disk != entries_total) {
    return 0;
  }
  return zz9k_archive_zip_u64_to_supported_u32(
             entries_total, total_entries) &&
         zz9k_archive_zip_u64_to_supported_u32(
             zz9k_archive_get_le64(record + 40U), cd_size) &&
         zz9k_archive_zip_u64_to_supported_u32(
             zz9k_archive_get_le64(record + 48U), cd_offset);
}

static int zz9k_archive_zip_read_eocd(
    const uint8_t *data,
    uint32_t length,
    uint32_t eocd,
    uint32_t *total_entries,
    uint32_t *cd_size,
    uint32_t *cd_offset)
{
  const uint8_t *eocd_data;

  if (!data || !total_entries || !cd_size || !cd_offset ||
      eocd > length || length - eocd < 22U) {
    return 0;
  }
  eocd_data = data + eocd;
  if (zz9k_archive_get_le16(eocd_data + 4U) != 0U ||
      zz9k_archive_get_le16(eocd_data + 6U) != 0U) {
    return 0;
  }
  if (!zz9k_archive_zip_eocd_needs_zip64(eocd_data)) {
    *total_entries = zz9k_archive_get_le16(eocd_data + 10U);
    if (*total_entries != zz9k_archive_get_le16(eocd_data + 8U)) {
      return 0;
    }
    *cd_size = zz9k_archive_get_le32(eocd_data + 12U);
    *cd_offset = zz9k_archive_get_le32(eocd_data + 16U);
    return 1;
  }
  if (eocd < 20U ||
      zz9k_archive_get_le32(data + eocd - 20U) != 0x07064b50UL ||
      zz9k_archive_get_le32(data + eocd - 16U) != 0U ||
      zz9k_archive_get_le32(data + eocd - 4U) != 1U) {
    return 0;
  } else {
    uint64_t zip64_eocd_offset64;
    uint32_t zip64_eocd_offset;
    uint64_t record_size;

    zip64_eocd_offset64 = zz9k_archive_get_le64(data + eocd - 12U);
    if (!zz9k_archive_zip_u64_to_supported_u32(
            zip64_eocd_offset64, &zip64_eocd_offset) ||
        zip64_eocd_offset > length ||
        length - zip64_eocd_offset < 56U) {
      return 0;
    }
    record_size = zz9k_archive_get_le64(data + zip64_eocd_offset + 4U);
    if (record_size > (uint64_t)(length - zip64_eocd_offset - 12U)) {
      return 0;
    }
    return zz9k_archive_zip_parse_zip64_eocd_record(
        data + zip64_eocd_offset, total_entries, cd_size, cd_offset);
  }
}

static int zz9k_archive_zip_read_eocd_from_file(
    const char *path,
    uint32_t archive_length,
    const uint8_t *tail,
    uint32_t tail_length,
    uint32_t eocd,
    uint32_t *total_entries,
    uint32_t *cd_size,
    uint32_t *cd_offset)
{
  const uint8_t *eocd_data;
  uint8_t *record = 0;
  uint32_t zip64_eocd_offset;
  uint64_t zip64_eocd_offset64;
  uint64_t record_size;
  int ok;

  if (!path || !tail || !total_entries || !cd_size || !cd_offset ||
      eocd > tail_length || tail_length - eocd < 22U) {
    return 0;
  }
  eocd_data = tail + eocd;
  if (zz9k_archive_get_le16(eocd_data + 4U) != 0U ||
      zz9k_archive_get_le16(eocd_data + 6U) != 0U) {
    return 0;
  }
  if (!zz9k_archive_zip_eocd_needs_zip64(eocd_data)) {
    *total_entries = zz9k_archive_get_le16(eocd_data + 10U);
    if (*total_entries != zz9k_archive_get_le16(eocd_data + 8U)) {
      return 0;
    }
    *cd_size = zz9k_archive_get_le32(eocd_data + 12U);
    *cd_offset = zz9k_archive_get_le32(eocd_data + 16U);
    return 1;
  }
  if (eocd < 20U ||
      zz9k_archive_get_le32(tail + eocd - 20U) != 0x07064b50UL ||
      zz9k_archive_get_le32(tail + eocd - 16U) != 0U ||
      zz9k_archive_get_le32(tail + eocd - 4U) != 1U) {
    return 0;
  }
  zip64_eocd_offset64 = zz9k_archive_get_le64(tail + eocd - 12U);
  if (!zz9k_archive_zip_u64_to_supported_u32(
          zip64_eocd_offset64, &zip64_eocd_offset) ||
      zip64_eocd_offset > archive_length ||
      archive_length - zip64_eocd_offset < 56U) {
    return 0;
  }
  if (!zz9k_archive_read_file_range(
          path, zip64_eocd_offset, 56U, &record)) {
    return 0;
  }
  record_size = zz9k_archive_get_le64(record + 4U);
  ok = record_size <=
       (uint64_t)(archive_length - zip64_eocd_offset - 12U);
  if (ok) {
    ok = zz9k_archive_zip_parse_zip64_eocd_record(
        record, total_entries, cd_size, cd_offset);
  }
  free(record);
  return ok;
}

static int zz9k_archive_zip_external_attrs_is_dir(uint32_t attrs)
{
  uint32_t unix_mode = (attrs >> 16) & 0170000U;

  return (attrs & 0x10U) != 0U || unix_mode == 0040000U;
}

static int zz9k_archive_zip_read_zip64_u32(const uint8_t *extra,
                                           uint32_t extra_len,
                                           uint32_t *pos,
                                           uint32_t *value)
{
  uint64_t v;

  if (!extra || !pos || !value || *pos > extra_len ||
      extra_len - *pos < 8U) {
    return 0;
  }
  v = zz9k_archive_get_le64(extra + *pos);
  if (!zz9k_archive_zip_u64_to_supported_u32(v, value)) {
    return 0;
  }
  *pos += 8U;
  return 1;
}

static int zz9k_archive_zip_parse_zip64_extra(
    const uint8_t *extra,
    uint32_t extra_len,
    uint32_t *compressed_size,
    uint32_t *uncompressed_size,
    uint32_t *local_offset)
{
  uint32_t pos = 0U;
  int need_uncompressed;
  int need_compressed;
  int need_local_offset;

  if (!compressed_size || !uncompressed_size) {
    return 0;
  }
  need_compressed =
      *compressed_size == ZZ9K_ARCHIVE_ZIP_U32_SENTINEL ? 1 : 0;
  need_uncompressed =
      *uncompressed_size == ZZ9K_ARCHIVE_ZIP_U32_SENTINEL ? 1 : 0;
  need_local_offset =
      local_offset && *local_offset == ZZ9K_ARCHIVE_ZIP_U32_SENTINEL ? 1 : 0;
  if (!need_compressed && !need_uncompressed && !need_local_offset) {
    return 1;
  }
  if (!extra && extra_len != 0U) {
    return 0;
  }

  while (pos + 4U <= extra_len) {
    uint32_t header_id = zz9k_archive_get_le16(extra + pos);
    uint32_t data_size = zz9k_archive_get_le16(extra + pos + 2U);
    uint32_t data_pos;

    pos += 4U;
    if (data_size > extra_len - pos) {
      return 0;
    }
    if (header_id != ZZ9K_ARCHIVE_ZIP_EXTRA_ZIP64) {
      pos += data_size;
      continue;
    }
    data_pos = 0U;
    if (need_uncompressed &&
        !zz9k_archive_zip_read_zip64_u32(
            extra + pos, data_size, &data_pos, uncompressed_size)) {
      return 0;
    }
    if (need_compressed &&
        !zz9k_archive_zip_read_zip64_u32(
            extra + pos, data_size, &data_pos, compressed_size)) {
      return 0;
    }
    if (need_local_offset &&
        !zz9k_archive_zip_read_zip64_u32(
            extra + pos, data_size, &data_pos, local_offset)) {
      return 0;
    }
    return 1;
  }
  return 0;
}

static int zz9k_archive_zip_data_offset_checked(
    const uint8_t *data,
    uint32_t length,
    uint32_t local_offset,
    const uint8_t *expected_name,
    uint32_t expected_name_len,
    uint32_t expected_flags,
    uint32_t expected_method,
    uint32_t expected_crc32,
    uint32_t expected_compressed_size,
    uint32_t expected_uncompressed_size,
    uint32_t *data_offset)
{
  uint32_t name_len;
  uint32_t extra_len;
  uint32_t flags;
  uint32_t method;
  uint32_t offset;
  uint32_t local_compressed_size;
  uint32_t local_uncompressed_size;

  if (!data || !data_offset ||
      local_offset > length ||
      length - local_offset < 30U ||
      zz9k_archive_get_le32(data + local_offset) != 0x04034b50UL) {
    return 0;
  }
  flags = zz9k_archive_get_le16(data + local_offset + 6U);
  method = zz9k_archive_get_le16(data + local_offset + 8U);
  name_len = zz9k_archive_get_le16(data + local_offset + 26U);
  extra_len = zz9k_archive_get_le16(data + local_offset + 28U);
  offset = local_offset + 30U;
  if (name_len > length - offset ||
      extra_len > length - offset - name_len) {
    return 0;
  }
  offset += name_len + extra_len;
  if (
      flags != expected_flags ||
      method != expected_method ||
      name_len != expected_name_len ||
      !expected_name ||
      memcmp(data + local_offset + 30U, expected_name, name_len) != 0) {
    return 0;
  }
  if ((flags & 0x0008U) == 0U) {
    local_compressed_size = zz9k_archive_get_le32(data + local_offset + 18U);
    local_uncompressed_size = zz9k_archive_get_le32(data + local_offset + 22U);
    if (!zz9k_archive_zip_parse_zip64_extra(
            data + local_offset + 30U + name_len, extra_len,
            &local_compressed_size, &local_uncompressed_size, 0)) {
      return 0;
    }
    if (zz9k_archive_get_le32(data + local_offset + 14U) != expected_crc32 ||
        local_compressed_size != expected_compressed_size ||
        local_uncompressed_size != expected_uncompressed_size) {
      return 0;
    }
  }
  *data_offset = offset;
  return 1;
}

static int zz9k_archive_zip_list(const uint8_t *data, uint32_t length,
                                 ZZ9KArchiveEntry *entries,
                                 uint32_t entry_capacity,
                                 uint32_t *entry_count)
{
  uint32_t eocd;
  uint32_t total_entries;
  uint32_t cd_size;
  uint32_t cd_offset;
  uint32_t pos;
  uint32_t count = 0U;
  uint32_t i;

  if (!data || !entries || !entry_count ||
      !zz9k_archive_find_eocd(data, length, &eocd)) {
    return 0;
  }
  if (!zz9k_archive_zip_read_eocd(
          data, length, eocd, &total_entries, &cd_size, &cd_offset)) {
    return 0;
  }
  if (cd_offset > length || cd_size > length - cd_offset) {
    return 0;
  }
  if (total_entries == 0U) {
    *entry_count = 0U;
    return cd_size == 0U;
  }

  pos = cd_offset;
  for (i = 0U; i < total_entries; i++) {
    uint32_t flags;
    uint32_t method;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t name_len;
    uint32_t extra_len;
    uint32_t comment_len;
    uint32_t local_offset;
    uint32_t external_attrs;
    uint32_t data_offset;
    uint32_t record_length;
    ZZ9KArchiveEntry candidate;
    ZZ9KArchiveEntry *entry;

    if (pos + 46U > length ||
        zz9k_archive_get_le32(data + pos) != 0x02014b50UL) {
      return 0;
    }
    flags = zz9k_archive_get_le16(data + pos + 8U);
    method = zz9k_archive_get_le16(data + pos + 10U);
    compressed_size = zz9k_archive_get_le32(data + pos + 20U);
    uncompressed_size = zz9k_archive_get_le32(data + pos + 24U);
    name_len = zz9k_archive_get_le16(data + pos + 28U);
    extra_len = zz9k_archive_get_le16(data + pos + 30U);
    comment_len = zz9k_archive_get_le16(data + pos + 32U);
    external_attrs = zz9k_archive_get_le32(data + pos + 38U);
    local_offset = zz9k_archive_get_le32(data + pos + 42U);
    if (name_len == 0U || pos + 46U + name_len + extra_len + comment_len >
        length) {
      return 0;
    }
    if (!zz9k_archive_zip_parse_zip64_extra(
            data + pos + 46U + name_len, extra_len,
            &compressed_size, &uncompressed_size, &local_offset)) {
      return 0;
    }
    if (!zz9k_archive_zip_data_offset_checked(
            data, length, local_offset, data + pos + 46U, name_len,
            flags, method, zz9k_archive_get_le32(data + pos + 16U),
            compressed_size, uncompressed_size, &data_offset)) {
      return 0;
    }
    if (compressed_size > length || data_offset > length ||
        data_offset + compressed_size > length) {
      return 0;
    }

    record_length = 46U + name_len + extra_len + comment_len;
    memset(&candidate, 0, sizeof(candidate));
    if (!zz9k_archive_copy_zip_name(candidate.name, sizeof(candidate.name),
                                    data + pos + 46U, name_len)) {
      return 0;
    }
    candidate.method = method;
    candidate.flags = flags | ZZ9K_ARCHIVE_ENTRY_FLAG_CRC32;
    candidate.crc32 = zz9k_archive_get_le32(data + pos + 16U);
    candidate.data_offset = data_offset;
    candidate.compressed_size = compressed_size;
    candidate.uncompressed_size = uncompressed_size;
    candidate.is_dir =
        zz9k_archive_name_ends_with_slash(candidate.name) ||
        zz9k_archive_zip_external_attrs_is_dir(external_attrs) ? 1U : 0U;
    pos += record_length;
    if (zz9k_archive_zip_entry_is_root_metadata(&candidate)) {
      continue;
    }
    if (count >= entry_capacity) {
      return 0;
    }
    entry = &entries[count++];
    *entry = candidate;
  }

  *entry_count = count;
  return 1;
}

static int zz9k_archive_zip_data_offset_from_file_checked(
    const char *path,
    uint32_t archive_length,
    uint32_t local_offset,
    const uint8_t *expected_name,
    uint32_t expected_name_len,
    uint32_t expected_flags,
    uint32_t expected_method,
    uint32_t expected_crc32,
    uint32_t expected_compressed_size,
    uint32_t expected_uncompressed_size,
    uint32_t *data_offset)
{
  uint8_t local[30];
  uint8_t *name_extra = 0;
  FILE *file;
  uint32_t name_len;
  uint32_t extra_len;
  uint32_t name_extra_len;
  uint32_t flags;
  uint32_t method;
  uint32_t offset;
  uint32_t local_compressed_size;
  uint32_t local_uncompressed_size;
  int ok = 0;

  if (!path || !data_offset ||
      local_offset > archive_length ||
      archive_length - local_offset < sizeof(local) ||
      local_offset > 0x7fffffffUL) {
    return 0;
  }
  file = fopen(path, "rb");
  if (!file) {
    printf("open failed: %s\n", path);
    return 0;
  }
  if (fseek(file, (long)local_offset, SEEK_SET) != 0 ||
      fread(local, 1U, sizeof(local), file) != sizeof(local)) {
    fclose(file);
    return 0;
  }
  fclose(file);

  if (zz9k_archive_get_le32(local) != 0x04034b50UL) {
    return 0;
  }
  flags = zz9k_archive_get_le16(local + 6U);
  method = zz9k_archive_get_le16(local + 8U);
  name_len = zz9k_archive_get_le16(local + 26U);
  extra_len = zz9k_archive_get_le16(local + 28U);
  offset = local_offset + 30U + name_len + extra_len;
  if (name_len > archive_length - local_offset - 30U ||
      extra_len > archive_length - local_offset - 30U - name_len ||
      offset < local_offset ||
      offset > archive_length) {
    return 0;
  }
  if (flags != expected_flags ||
      method != expected_method ||
      name_len != expected_name_len ||
      !expected_name) {
    return 0;
  }
  if (extra_len > UINT32_MAX - name_len) {
    return 0;
  }
  name_extra_len = name_len + extra_len;
  if (!zz9k_archive_read_file_range(
          path, local_offset + 30U, name_extra_len, &name_extra)) {
    return 0;
  }
  ok = memcmp(name_extra, expected_name, name_len) == 0;
  if (ok && (flags & 0x0008U) == 0U) {
    local_compressed_size = zz9k_archive_get_le32(local + 18U);
    local_uncompressed_size = zz9k_archive_get_le32(local + 22U);
    ok = zz9k_archive_zip_parse_zip64_extra(
        name_extra + name_len, extra_len,
        &local_compressed_size, &local_uncompressed_size, 0);
    if (ok) {
      ok = zz9k_archive_get_le32(local + 14U) == expected_crc32 &&
           local_compressed_size == expected_compressed_size &&
           local_uncompressed_size == expected_uncompressed_size;
    }
  }
  free(name_extra);
  if (!ok) {
    return 0;
  }
  *data_offset = offset;
  return 1;
}

static int zz9k_archive_zip_read_directory_from_file(
    const char *path,
    uint32_t archive_length,
    uint8_t **directory,
    uint32_t *directory_length,
    uint32_t *entry_count)
{
  FILE *file = 0;
  uint8_t *tail = 0;
  uint8_t *central = 0;
  uint32_t tail_length;
  uint32_t tail_offset;
  uint32_t eocd;
  uint32_t total_entries;
  uint32_t cd_size;
  uint32_t cd_offset;
  int ok = 0;

  if (!path || !directory || !directory_length || !entry_count ||
      archive_length < 22U) {
    return 0;
  }
  *directory = 0;
  *directory_length = 0U;
  *entry_count = 0U;

  tail_length = archive_length;
  if (tail_length > 22U + 65535U) {
    tail_length = 22U + 65535U;
  }
  tail_offset = archive_length - tail_length;
  if (tail_offset > 0x7fffffffUL) {
    return 0;
  }

  file = fopen(path, "rb");
  if (!file) {
    printf("open failed: %s\n", path);
    return 0;
  }
  tail = (uint8_t *)malloc((size_t)tail_length);
  if (!tail) {
    printf("zip tail allocation failed\n");
    goto out;
  }
  if (fseek(file, (long)tail_offset, SEEK_SET) != 0 ||
      fread(tail, 1U, tail_length, file) != tail_length) {
    printf("zip tail read failed: %s\n", path);
    goto out;
  }
  if (!zz9k_archive_find_eocd(tail, tail_length, &eocd)) {
    goto out;
  }
  if (!zz9k_archive_zip_read_eocd_from_file(
          path, archive_length, tail, tail_length, eocd,
          &total_entries, &cd_size, &cd_offset)) {
    goto out;
  }
  if (total_entries == 0U && cd_size == 0U) {
    *directory = 0;
    *directory_length = 0U;
    *entry_count = 0U;
    ok = 1;
    goto out;
  }
  if (total_entries == 0U || cd_size == 0U ||
      cd_offset > archive_length ||
      cd_size > archive_length - cd_offset ||
      cd_offset > 0x7fffffffUL) {
    goto out;
  }
  central = (uint8_t *)malloc((size_t)cd_size);
  if (!central) {
    printf("zip directory allocation failed: %lu bytes\n",
           (unsigned long)cd_size);
    goto out;
  }
  if (fseek(file, (long)cd_offset, SEEK_SET) != 0 ||
      fread(central, 1U, cd_size, file) != cd_size) {
    printf("zip directory read failed: %s\n", path);
    goto out;
  }

  *directory = central;
  *directory_length = cd_size;
  *entry_count = total_entries;
  central = 0;
  ok = 1;

out:
  free(central);
  free(tail);
  if (file) {
    fclose(file);
  }
  return ok;
}

static int zz9k_archive_zip_list_from_directory(
    const char *path,
    const uint8_t *directory,
    uint32_t directory_length,
    uint32_t archive_length,
    ZZ9KArchiveEntry *entries,
    uint32_t entry_capacity,
    uint32_t expected_entries,
    uint32_t *entry_count)
{
  uint32_t pos = 0U;
  uint32_t count = 0U;
  uint32_t i;

  if (!path || !entries || !entry_count ||
      (expected_entries != 0U && !directory)) {
    return 0;
  }
  if (expected_entries == 0U) {
    *entry_count = 0U;
    return directory_length == 0U;
  }

  for (i = 0U; i < expected_entries; i++) {
    uint32_t flags;
    uint32_t method;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t name_len;
    uint32_t extra_len;
    uint32_t comment_len;
    uint32_t local_offset;
    uint32_t external_attrs;
    uint32_t data_offset;
    uint32_t record_length;
    ZZ9KArchiveEntry candidate;
    ZZ9KArchiveEntry *entry;

    if (pos + 46U > directory_length ||
        zz9k_archive_get_le32(directory + pos) != 0x02014b50UL) {
      return 0;
    }
    flags = zz9k_archive_get_le16(directory + pos + 8U);
    method = zz9k_archive_get_le16(directory + pos + 10U);
    compressed_size = zz9k_archive_get_le32(directory + pos + 20U);
    uncompressed_size = zz9k_archive_get_le32(directory + pos + 24U);
    name_len = zz9k_archive_get_le16(directory + pos + 28U);
    extra_len = zz9k_archive_get_le16(directory + pos + 30U);
    comment_len = zz9k_archive_get_le16(directory + pos + 32U);
    external_attrs = zz9k_archive_get_le32(directory + pos + 38U);
    local_offset = zz9k_archive_get_le32(directory + pos + 42U);
    if (name_len == 0U ||
        pos + 46U + name_len + extra_len + comment_len >
          directory_length) {
      return 0;
    }
    if (!zz9k_archive_zip_parse_zip64_extra(
            directory + pos + 46U + name_len, extra_len,
            &compressed_size, &uncompressed_size, &local_offset)) {
      return 0;
    }
    if (!zz9k_archive_zip_data_offset_from_file_checked(
            path, archive_length, local_offset, directory + pos + 46U,
            name_len, flags, method,
            zz9k_archive_get_le32(directory + pos + 16U),
            compressed_size, uncompressed_size, &data_offset)) {
      return 0;
    }
    if (compressed_size > archive_length ||
        data_offset > archive_length ||
        compressed_size > archive_length - data_offset) {
      return 0;
    }

    record_length = 46U + name_len + extra_len + comment_len;
    memset(&candidate, 0, sizeof(candidate));
    if (!zz9k_archive_copy_zip_name(candidate.name, sizeof(candidate.name),
                                    directory + pos + 46U, name_len)) {
      return 0;
    }
    candidate.method = method;
    candidate.flags = flags | ZZ9K_ARCHIVE_ENTRY_FLAG_CRC32;
    candidate.crc32 = zz9k_archive_get_le32(directory + pos + 16U);
    candidate.data_offset = data_offset;
    candidate.compressed_size = compressed_size;
    candidate.uncompressed_size = uncompressed_size;
    candidate.is_dir =
        zz9k_archive_name_ends_with_slash(candidate.name) ||
        zz9k_archive_zip_external_attrs_is_dir(external_attrs) ? 1U : 0U;
    pos += record_length;
    if (zz9k_archive_zip_entry_is_root_metadata(&candidate)) {
      continue;
    }
    if (count >= entry_capacity) {
      return 0;
    }
    entry = &entries[count++];
    *entry = candidate;
  }

  *entry_count = count;
  while (pos < directory_length) {
    uint32_t extra_size;

    if (pos + 6U > directory_length ||
        zz9k_archive_get_le32(directory + pos) != 0x05054b50UL) {
      return 0;
    }
    extra_size = zz9k_archive_get_le16(directory + pos + 4U);
    if (extra_size > directory_length - pos - 6U) {
      return 0;
    }
    pos += 6U + extra_size;
  }
  return 1;
}

static int zz9k_archive_zip_list_file(const char *path,
                                      uint32_t archive_length,
                                      ZZ9KArchiveEntry *entries,
                                      uint32_t entry_capacity,
                                      uint32_t *entry_count)
{
  uint8_t *directory = 0;
  uint32_t directory_length = 0U;
  uint32_t count = 0U;
  int ok;

  if (!entry_count) {
    return 0;
  }
  if (!zz9k_archive_zip_read_directory_from_file(
          path, archive_length, &directory, &directory_length, &count)) {
    return 0;
  }
  ok = zz9k_archive_zip_list_from_directory(
      path, directory, directory_length, archive_length,
      entries, entry_capacity, count, entry_count);
  free(directory);
  return ok;
}

static int zz9k_archive_tar_name(const uint8_t *header, char *name,
                                 uint32_t capacity)
{
  uint32_t prefix_len;
  uint32_t name_len;

  name_len = 0U;
  while (name_len < 100U && header[name_len] != 0U) {
    name_len++;
  }
  prefix_len = 0U;
  while (prefix_len < 155U && header[345U + prefix_len] != 0U) {
    prefix_len++;
  }
  if (name_len == 0U) {
    return 0;
  }
  if (prefix_len != 0U) {
    if (prefix_len + 1U + name_len >= capacity) {
      return 0;
    }
    memcpy(name, header + 345U, prefix_len);
    name[prefix_len] = '/';
    memcpy(name + prefix_len + 1U, header, name_len);
    name[prefix_len + 1U + name_len] = '\0';
  } else {
    if (!zz9k_archive_copy_name(name, capacity, header, name_len)) {
      return 0;
    }
  }
  return 1;
}

static int zz9k_archive_tar_normalize_name(char *name, int *skip)
{
  size_t len;

  if (!name || !skip) {
    return 0;
  }
  *skip = 0;
  while (name[0] == '.' && name[1] == '/') {
    len = strlen(name + 2U);
    memmove(name, name + 2U, len + 1U);
  }
  zz9k_archive_strip_current_dir_prefix(name);
  if (name[0] == '\0' || strcmp(name, ".") == 0) {
    name[0] = '\0';
    *skip = 1;
  }
  return 1;
}

static int zz9k_archive_tar_copy_data_name(const uint8_t *data,
                                           uint32_t length,
                                           char *name,
                                           uint32_t capacity,
                                           int *skip)
{
  uint32_t name_len = 0U;

  if (!data || !name || !skip || capacity == 0U || length == 0U) {
    return 0;
  }
  *skip = 0;
  while (name_len < length && data[name_len] != 0U &&
         data[name_len] != (uint8_t)'\n') {
    name_len++;
  }
  if (name_len == 0U || name_len >= capacity) {
    return 0;
  }
  memcpy(name, data, name_len);
  name[name_len] = '\0';
  return zz9k_archive_tar_normalize_name(name, skip);
}

static int zz9k_archive_parse_decimal(const uint8_t *field,
                                      uint32_t length,
                                      uint32_t *value)
{
  uint32_t i;
  uint32_t out = 0U;

  if (!field || !value || length == 0U) {
    return 0;
  }
  for (i = 0U; i < length; i++) {
    uint8_t c = field[i];

    if (c < (uint8_t)'0' || c > (uint8_t)'9' ||
        out > 0x7fffffffUL / 10U) {
      return 0;
    }
    out *= 10U;
    if ((uint32_t)(c - (uint8_t)'0') > 0x7fffffffUL - out) {
      return 0;
    }
    out += (uint32_t)(c - (uint8_t)'0');
  }
  *value = out;
  return 1;
}

static int zz9k_archive_tar_parse_pax_info(const uint8_t *data,
                                           uint32_t length,
                                           ZZ9KArchiveTarPaxInfo *info)
{
  uint32_t pos = 0U;

  if (!data || !info) {
    return 0;
  }
  memset(info, 0, sizeof(*info));
  while (pos < length) {
    uint32_t record_len = 0U;
    uint32_t digits = 0U;
    uint32_t body_pos;
    uint32_t body_len;
    uint32_t value_len;
    uint32_t eq_pos;

    while (pos + digits < length &&
           data[pos + digits] >= (uint8_t)'0' &&
           data[pos + digits] <= (uint8_t)'9') {
      if (record_len > 99999U) {
        return 0;
      }
      record_len = record_len * 10U +
          (uint32_t)(data[pos + digits] - (uint8_t)'0');
      digits++;
    }
    if (digits == 0U || pos + digits >= length ||
        data[pos + digits] != (uint8_t)' ' ||
        record_len <= digits + 1U || pos + record_len > length) {
      return 0;
    }
    body_pos = pos + digits + 1U;
    body_len = record_len - digits - 1U;
    if (body_len == 0U || data[body_pos + body_len - 1U] != (uint8_t)'\n') {
      return 0;
    }
    value_len = body_len - 1U;
    eq_pos = 0U;
    while (eq_pos < value_len && data[body_pos + eq_pos] != (uint8_t)'=') {
      eq_pos++;
    }
    if (eq_pos == 4U && memcmp(data + body_pos, "path", 4U) == 0 &&
        eq_pos + 1U < value_len) {
      uint32_t path_len = value_len - eq_pos - 1U;
      int skip = 0;

      if (path_len >= sizeof(info->path)) {
        return 0;
      }
      memcpy(info->path, data + body_pos + eq_pos + 1U, path_len);
      info->path[path_len] = '\0';
      if (!zz9k_archive_tar_normalize_name(info->path, &skip)) {
        return 0;
      }
      info->has_path = 1;
      info->path_skip = skip;
    } else if (eq_pos == 4U &&
               memcmp(data + body_pos, "size", 4U) == 0 &&
               eq_pos + 1U < value_len) {
      uint32_t size_len = value_len - eq_pos - 1U;

      if (!zz9k_archive_parse_decimal(
              data + body_pos + eq_pos + 1U, size_len, &info->size)) {
        return 0;
      }
      info->has_size = 1;
    }
    pos += record_len;
  }
  return 1;
}

static int zz9k_archive_tar_parse_pax_path(const uint8_t *data,
                                           uint32_t length,
                                           char *name,
                                           uint32_t capacity)
{
  ZZ9KArchiveTarPaxInfo info;

  if (!name || capacity == 0U ||
      !zz9k_archive_tar_parse_pax_info(data, length, &info)) {
    return 0;
  }
  if (info.has_path) {
    if (strlen(info.path) >= capacity) {
      return 0;
    }
    strcpy(name, info.path);
  }
  return 1;
}

static int zz9k_archive_parse_octal(const uint8_t *field, uint32_t length,
                                    uint32_t *value)
{
  uint32_t i;
  uint32_t out = 0U;
  int saw_digit = 0;

  if (!field || !value) {
    return 0;
  }
  for (i = 0U; i < length; i++) {
    uint8_t c = field[i];
    if (c == 0U || c == (uint8_t)' ') {
      break;
    }
    if (c < (uint8_t)'0' || c > (uint8_t)'7') {
      return 0;
    }
    if (out > 0x1fffffffUL) {
      return 0;
    }
    out = (out << 3) | (uint32_t)(c - (uint8_t)'0');
    saw_digit = 1;
  }
  if (!saw_digit) {
    return 0;
  }
  *value = out;
  return 1;
}

static int zz9k_archive_parse_base256(const uint8_t *field, uint32_t length,
                                      uint32_t *value)
{
  uint32_t i;
  uint32_t out;

  if (!field || !value || length == 0U ||
      (field[0] & 0x80U) == 0U ||
      (field[0] & 0x40U) != 0U) {
    return 0;
  }
  out = (uint32_t)(field[0] & 0x3fU);
  for (i = 1U; i < length; i++) {
    if (out > 0x7fffffffUL >> 8) {
      return 0;
    }
    out = (out << 8) | (uint32_t)field[i];
  }
  if (out > 0x7fffffffUL) {
    return 0;
  }
  *value = out;
  return 1;
}

static int zz9k_archive_parse_tar_number(const uint8_t *field,
                                         uint32_t length,
                                         uint32_t *value)
{
  if (!field || length == 0U) {
    return 0;
  }
  if ((field[0] & 0x80U) != 0U) {
    return zz9k_archive_parse_base256(field, length, value);
  }
  return zz9k_archive_parse_octal(field, length, value);
}

static int zz9k_archive_tar_header_checksum_valid(const uint8_t *header)
{
  uint32_t stored;
  uint32_t sum = 0U;
  int32_t signed_sum = 0;
  uint32_t i;

  if (!header || !zz9k_archive_parse_octal(header + 148U, 8U, &stored)) {
    return 0;
  }
  for (i = 0U; i < 512U; i++) {
    if (i >= 148U && i < 156U) {
      sum += (uint32_t)' ';
      signed_sum += (int32_t)' ';
    } else {
      sum += (uint32_t)header[i];
      signed_sum += (int32_t)(int8_t)header[i];
    }
  }
  return stored == sum || stored == (uint32_t)signed_sum;
}

static int zz9k_archive_tar_header_empty(const uint8_t *header)
{
  uint32_t i;

  for (i = 0U; i < 512U; i++) {
    if (header[i] != 0U) {
      return 0;
    }
  }
  return 1;
}

static int zz9k_archive_tar_entry_from_header(const uint8_t *header,
                                              uint32_t data_offset,
                                              ZZ9KArchiveEntry *entry)
{
  uint32_t size;
  uint8_t typeflag;
  int skip = 0;

  if (!header || !entry ||
      !zz9k_archive_tar_header_checksum_valid(header) ||
      !zz9k_archive_tar_name(header, entry->name, sizeof(entry->name)) ||
      !zz9k_archive_parse_tar_number(header + 124U, 12U, &size) ||
      !zz9k_archive_tar_normalize_name(entry->name, &skip)) {
    return 0;
  }
  typeflag = header[156U];
  entry->method = ZZ9K_ARCHIVE_TAR_METHOD_STORE;
  entry->flags = skip ? ZZ9K_ARCHIVE_TAR_FLAG_SKIP : 0U;
  entry->data_offset = data_offset;
  entry->compressed_size = size;
  entry->uncompressed_size = size;
  entry->is_dir =
      typeflag == (uint8_t)'5' ||
      zz9k_archive_name_ends_with_slash(entry->name) ? 1U : 0U;
  if (typeflag == (uint8_t)'L') {
    entry->flags |= ZZ9K_ARCHIVE_TAR_FLAG_SKIP |
                    ZZ9K_ARCHIVE_TAR_FLAG_GNU_LONG_NAME;
  } else if (typeflag == (uint8_t)'K') {
    entry->flags |= ZZ9K_ARCHIVE_TAR_FLAG_SKIP;
  } else if (typeflag == (uint8_t)'x') {
    entry->flags |= ZZ9K_ARCHIVE_TAR_FLAG_SKIP |
                    ZZ9K_ARCHIVE_TAR_FLAG_PAX_HEADER;
  } else if (typeflag == (uint8_t)'g') {
    entry->flags |= ZZ9K_ARCHIVE_TAR_FLAG_SKIP;
  } else if (typeflag != 0U && typeflag != (uint8_t)'0' &&
             typeflag != (uint8_t)'5') {
    entry->flags |= ZZ9K_ARCHIVE_TAR_FLAG_SKIP;
  }
  return 1;
}

static int zz9k_archive_tar_list(const uint8_t *data, uint32_t length,
                                 ZZ9KArchiveEntry *entries,
                                 uint32_t entry_capacity,
                                 uint32_t *entry_count)
{
  uint32_t pos = 0U;
  uint32_t count = 0U;
  uint32_t pending_size = 0U;
  char pending_name[ZZ9K_ARCHIVE_MAX_NAME];
  int pending_size_valid = 0;
  int pending_name_skip = 0;

  pending_name[0] = '\0';

  if (!data || !entries || !entry_count || (length % 512U) != 0U) {
    return 0;
  }
  while (pos + 512U <= length) {
    const uint8_t *header = data + pos;
    uint32_t size;
    uint32_t blocks;
    ZZ9KArchiveEntry candidate;
    ZZ9KArchiveEntry *entry;

    if (zz9k_archive_tar_header_empty(header)) {
      *entry_count = count;
      return 1;
    }
    memset(&candidate, 0, sizeof(candidate));
    if (!zz9k_archive_tar_entry_from_header(
            header, pos + 512U, &candidate)) {
      return 0;
    }
    entry = &candidate;
    size = entry->uncompressed_size;
    if (entry->data_offset > length || size > length ||
        entry->data_offset + size > length) {
      return 0;
    }
    if ((entry->flags & ZZ9K_ARCHIVE_TAR_FLAG_GNU_LONG_NAME) != 0U) {
      int skip = 0;

      if (!zz9k_archive_tar_copy_data_name(
              data + entry->data_offset, size,
              pending_name, sizeof(pending_name), &skip)) {
        return 0;
      }
      pending_name_skip = skip;
    } else if ((entry->flags & ZZ9K_ARCHIVE_TAR_FLAG_PAX_HEADER) != 0U) {
      ZZ9KArchiveTarPaxInfo pax;

      if (!zz9k_archive_tar_parse_pax_info(
              data + entry->data_offset, size, &pax)) {
        return 0;
      }
      if (pax.has_path) {
        if (pax.path_skip) {
          pending_name[0] = '\0';
          pending_name_skip = 1;
        } else {
          strcpy(pending_name, pax.path);
          pending_name_skip = 0;
        }
      }
      if (pax.has_size) {
        pending_size = pax.size;
        pending_size_valid = 1;
      }
    } else if (pending_name_skip) {
      entry->flags |= ZZ9K_ARCHIVE_TAR_FLAG_SKIP;
      pending_name_skip = 0;
    } else if (pending_name[0] != '\0') {
      strcpy(entry->name, pending_name);
      pending_name[0] = '\0';
    }
    if ((entry->flags & (ZZ9K_ARCHIVE_TAR_FLAG_GNU_LONG_NAME |
                         ZZ9K_ARCHIVE_TAR_FLAG_PAX_HEADER)) == 0U &&
        pending_size_valid) {
      entry->compressed_size = pending_size;
      entry->uncompressed_size = pending_size;
      size = pending_size;
      pending_size_valid = 0;
    }
    if (entry->data_offset > length || size > length ||
        entry->data_offset + size > length) {
      return 0;
    }
    if ((entry->flags & ZZ9K_ARCHIVE_TAR_FLAG_SKIP) == 0U) {
      if (count >= entry_capacity) {
        return 0;
      }
      entries[count] = candidate;
      count++;
    }
    blocks = (size + 511U) / 512U;
    pos += 512U + blocks * 512U;
  }
  *entry_count = count;
  return 1;
}

static int zz9k_archive_require_codec_service(ZZ9KContext *ctx,
                                              ZZ9KServiceInfo *service)
{
  ZZ9KCaps caps;
  int status;

  status = zz9k_query_caps(ctx, &caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("query caps failed: %s (%d)\n", zz9k_status_name(status), status);
    return 0;
  }
  if ((caps.capability_bits & ZZ9K_CAP_COMPRESSION) == 0U) {
    printf("%s capability not advertised\n",
           zz9k_capability_name(ZZ9K_CAP_COMPRESSION));
    return 0;
  }
  {
    uint32_t budget = zz9k_archive_stream_budget(caps.host_window_heap_size);

    zz9k_archive_host_window_heap = caps.host_window_heap_size;
    zz9k_archive_stream_chunk = zz9k_archive_stream_budget_chunk(budget);
    if (budget < ZZ9K_ARCHIVE_STREAM_HOST_BUDGET) {
      printf("Note: Zorro II host window is %lu bytes; stream feed chunks "
             "capped at %lu bytes (default %lu)\n",
             (unsigned long)caps.host_window_heap_size,
             (unsigned long)zz9k_archive_stream_chunk,
             (unsigned long)ZZ9K_ARCHIVE_STREAM_CHUNK);
    }
  }
  status = zz9k_query_service(ctx, ZZ9K_SERVICE_CODEC, service);
  if (status != ZZ9K_STATUS_OK) {
    printf("query codec service failed: %s (%d)\n",
           zz9k_status_name(status), status);
    return 0;
  }
  return 1;
}

static int zz9k_archive_service_supports(const ZZ9KServiceInfo *service,
                                         uint32_t algorithm)
{
  uint32_t flags;

  flags = zz9k_compression_required_service_flags(algorithm);
  return service && flags != 0U && (service->flags & flags) == flags;
}

static int zz9k_archive_service_supports_decompress_test(
    const ZZ9KServiceInfo *service, uint32_t algorithm)
{
  return zz9k_archive_service_supports(service, algorithm) &&
         (service->flags & ZZ9K_SERVICE_FLAG_CODEC_DECOMPRESS_TEST) != 0U;
}

static int zz9k_archive_service_supports_decompress_stream(
    const ZZ9KServiceInfo *service, uint32_t algorithm)
{
  return zz9k_archive_service_supports(service, algorithm) &&
         (service->flags & ZZ9K_SERVICE_FLAG_CODEC_DECOMPRESS_STREAM) != 0U;
}

static int zz9k_archive_service_supports_decompress_feed(
    const ZZ9KServiceInfo *service, uint32_t algorithm)
{
  uint32_t flags;

  if (algorithm == ZZ9K_COMPRESSION_DEFLATE_RAW &&
      (!service ||
       (service->flags & ZZ9K_SERVICE_FLAG_CODEC_DEFLATE_FEED) == 0U)) {
    return 0;
  }
  flags = zz9k_compression_required_feed_service_flags(algorithm);
  return zz9k_archive_service_supports_decompress_stream(service, algorithm) &&
         flags != 0U && (service->flags & flags) == flags;
}

static int zz9k_archive_ensure_codec_open(ZZ9KContext **ctx,
                                          ZZ9KServiceInfo *service,
                                          int *codec_ready)
{
  int status;

  if (!ctx || !service || !codec_ready) {
    return 0;
  }
  if (*codec_ready) {
    return 1;
  }
  status = zz9k_open(ctx);
  if (status != ZZ9K_STATUS_OK) {
    printf("open failed: %s (%d)\n", zz9k_status_name(status), status);
    return 0;
  }
  /* Best-effort: block on the completion IRQ instead of busy-polling. On any
   * failure the tool keeps the existing poll path unchanged.
   * Only the codec-service path (all handle_*_file handlers funnel through
   * here) is armed; the standalone-LZMA and in-memory fallback open sites
   * intentionally stay on the busy-poll path this increment. */
  (void)zz9k_arm_completion_irq(*ctx);
  if (!zz9k_archive_require_codec_service(*ctx, service)) {
    return 0;
  }
  *codec_ready = 1;
  return 1;
}

static void zz9k_archive_print_shared_diag(ZZ9KContext *ctx,
                                           const char *label,
                                           uint32_t requested)
{
  ZZ9KDiagInfo diag;
  int status;

  if (!ctx) {
    return;
  }

  memset(&diag, 0, sizeof(diag));
  status = zz9k_read_diag(ctx, &diag);
  if (status != ZZ9K_STATUS_OK) {
    printf("Archive diag (%s): unavailable: %s (%d)\n",
           label, zz9k_status_name(status), status);
    return;
  }

  printf("Archive diag (%s): requested=%lu bytes\n",
         label, (unsigned long)requested);
  printf("  Last status:         %s (%lu)\n",
         zz9k_status_name((int)diag.last_status),
         (unsigned long)diag.last_status);
  printf("  Shared buffers used: %lu\n",
         (unsigned long)diag.shared_buffers_used);
  printf("  Shared heap free:    %lu bytes\n",
         (unsigned long)diag.shared_heap_free);
  printf("  Largest free block:  %lu bytes\n",
         (unsigned long)diag.shared_heap_largest_free);
  {
    ZZ9KDiagMemoryInfo memory;

    memset(&memory, 0, sizeof(memory));
    if (zz9k_read_diag_memory(ctx, &memory) == ZZ9K_STATUS_OK &&
        memory.host_total != 0U) {
      printf("  Host window total:   %lu bytes\n",
             (unsigned long)memory.host_total);
      printf("  Host window free:    %lu bytes\n",
             (unsigned long)memory.host_free);
      printf("  Largest win block:   %lu bytes\n",
             (unsigned long)memory.host_largest_free);
    }
  }
}

/* Explains the Zorro II host-window overflow behind a BAD_REQUEST
 * allocation failure, when the caps reply made the window size known. */
static void zz9k_archive_print_window_overflow(uint32_t requested, int status)
{
  if (status == ZZ9K_STATUS_BAD_REQUEST &&
      zz9k_archive_host_window_heap != 0U &&
      requested > zz9k_archive_host_window_heap) {
    printf("  (%lu bytes exceeds the negotiated Zorro II host window of "
           "%lu bytes)\n",
           (unsigned long)requested,
           (unsigned long)zz9k_archive_host_window_heap);
  }
}

static int zz9k_archive_decompress_to_memory_ex(ZZ9KContext *ctx,
                                                const ZZ9KServiceInfo *service,
                                                uint32_t algorithm,
                                                const uint8_t *compressed,
                                                uint32_t compressed_length,
                                                uint32_t output_capacity,
                                                uint8_t **output,
                                                ZZ9KDecompressResult *result,
                                                int verify_only)
{
  ZZ9KSharedBuffer input;
  ZZ9KSharedBuffer decoded;
  ZZ9KDecompressDesc desc;
  uint8_t *bytes;
  int status;
  int ok = 0;

  memset(&input, 0, sizeof(input));
  memset(&decoded, 0, sizeof(decoded));
  *output = 0;

  if (!zz9k_archive_service_supports(service, algorithm)) {
    printf("%s not advertised by codec service\n",
           zz9k_compression_algorithm_text(algorithm));
    return 0;
  }
  if (compressed_length == 0U || output_capacity == 0U) {
    printf("unsupported empty codec job\n");
    return 0;
  }
  bytes = 0;
  if (!verify_only) {
    bytes = (uint8_t *)malloc((size_t)output_capacity);
    if (!bytes) {
      printf("output allocation failed\n");
      return 0;
    }
  }

  status = zz9k_alloc_shared(ctx, compressed_length, 16U, 0U, &input);
  if (status != ZZ9K_STATUS_OK) {
    zz9k_archive_note_status(status);
    printf("alloc compressed failed: %s (%d), requested=%lu bytes\n",
           zz9k_status_name(status), status,
           (unsigned long)compressed_length);
    zz9k_archive_print_shared_diag(ctx, "compressed input",
                                   compressed_length);
    goto out;
  }
  status = zz9k_alloc_shared(ctx, output_capacity, 16U, 0U, &decoded);
  if (status != ZZ9K_STATUS_OK) {
    zz9k_archive_note_status(status);
    printf("alloc decoded failed: %s (%d), requested=%lu bytes\n",
           zz9k_status_name(status), status,
           (unsigned long)output_capacity);
    zz9k_archive_print_shared_diag(ctx, "decoded output",
                                   output_capacity);
    goto out;
  }
  if (!zz9k_shared_copy_to(&input, 0U, compressed, compressed_length)) {
    printf("compressed copy failed\n");
    goto out;
  }
  if (!zz9k_compression_build_decompress_desc(
          &desc, algorithm, input.handle, 0U, compressed_length,
          decoded.handle, 0U, output_capacity,
          ZZ9K_DECOMPRESS_FLAG_EXPECT_END)) {
    printf("could not build decompression descriptor\n");
    goto out;
  }
  memset(result, 0, sizeof(*result));
  status = zz9k_decompress(ctx, &desc, result);
  if (status != ZZ9K_STATUS_OK) {
    if (status == ZZ9K_STATUS_CANCELLED) {
      /* The armed wait consumed SIGBREAKF_CTRL_C (Wait clears the bit),
         so CheckSignal-based checkpoints cannot see this press. Latch
         the cancellation here so every checkpoint stops the run. */
      zz9k_archive_cancel_latched = 1;
      /* The library's drain now owns late_irq_expected: retired drains
         clear it, timed-out drains set it. Marking here unconditionally
         made even cleanly retired cancellations spin disarm's watcher
         for its full window. */
      /* Never free the board buffers: a timed-out drain means the ARM
         may still be decoding into them. Board-heap slots freed now get
         reused by the next allocation (this or a later process) while
         the firmware is still writing -- the delayed crash after a
         Ctrl-C stop. Leaking on an interrupted run is a few hundred KB
         of board RAM and entirely safe. */
      printf("%s decompress interrupted: board buffers abandoned\n",
             zz9k_compression_algorithm_text(algorithm));
      free(bytes);
      return 0;
    }
    printf("%s decompress failed: %s (%d), input=%lu output=%lu\n",
           zz9k_compression_algorithm_text(algorithm),
           zz9k_status_name(status), status,
           (unsigned long)compressed_length,
           (unsigned long)output_capacity);
    zz9k_archive_print_shared_diag(ctx, "codec failure",
                                   output_capacity);
    goto out;
  }
  if (verify_only) {
    /* "test": the board has already decoded and checksummed the member, so
       the caller only needs result->checksum / result->bytes_written. Skip
       copying the decoded plaintext back across Zorro -- on a large archive
       that removes a full pass of the uncompressed size over the bus, which
       is the entire point of the verify path. */
    ok = 1;
  } else {
    if (result->bytes_written > output_capacity ||
        !zz9k_shared_copy_from(bytes, &decoded, 0U, result->bytes_written)) {
      printf("decoded copy failed\n");
      goto out;
    }
    *output = bytes;
    bytes = 0;
    ok = 1;
  }

out:
  if (decoded.handle != 0U && decoded.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, decoded.handle);
  }
  if (input.handle != 0U && input.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, input.handle);
  }
  free(bytes);
  return ok;
}

static int zz9k_archive_decompress_to_memory(ZZ9KContext *ctx,
                                             const ZZ9KServiceInfo *service,
                                             uint32_t algorithm,
                                             const uint8_t *compressed,
                                             uint32_t compressed_length,
                                             uint32_t output_capacity,
                                             uint8_t **output,
                                             ZZ9KDecompressResult *result)
{
  return zz9k_archive_decompress_to_memory_ex(
      ctx, service, algorithm, compressed, compressed_length, output_capacity,
      output, result, 0);
}

static void zz9k_archive_print_entry(const ZZ9KArchiveEntry *entry)
{
  printf("%c %10lu %s\n",
         entry->is_dir ? 'd' : '-',
         (unsigned long)entry->uncompressed_size,
         entry->name);
}

static int zz9k_archive_alloc_entries(uint32_t capacity,
                                      ZZ9KArchiveEntry **entries)
{
  uint32_t alloc_capacity;

  if (!entries || capacity > 65535U) {
    return 0;
  }
  alloc_capacity = capacity == 0U ? 1U : capacity;
  *entries = (ZZ9KArchiveEntry *)calloc((size_t)alloc_capacity,
                                        sizeof(ZZ9KArchiveEntry));
  return *entries != 0;
}

static int zz9k_archive_count_zip_entries(const uint8_t *data,
                                          uint32_t length,
                                          uint32_t *count)
{
  uint32_t eocd;
  uint32_t cd_size;
  uint32_t cd_offset;

  if (!zz9k_archive_find_eocd(data, length, &eocd) ||
      !zz9k_archive_zip_read_eocd(
          data, length, eocd, count, &cd_size, &cd_offset)) {
    return 0;
  }
  return 1;
}

static int zz9k_archive_count_tar_entries(const uint8_t *data,
                                          uint32_t length,
                                          uint32_t *count)
{
  uint32_t pos = 0U;
  uint32_t entries = 0U;
  uint32_t pending_size = 0U;
  int pending_size_valid = 0;
  int pending_name_skip = 0;

  if (!data || (length % 512U) != 0U) {
    return 0;
  }
  while (pos + 512U <= length) {
    uint32_t size;
    uint32_t blocks;
    ZZ9KArchiveEntry entry;

    if (zz9k_archive_tar_header_empty(data + pos)) {
      *count = entries;
      return 1;
    }
    memset(&entry, 0, sizeof(entry));
    if (!zz9k_archive_tar_entry_from_header(
            data + pos, pos + 512U, &entry)) {
      return 0;
    }
    size = entry.uncompressed_size;
    if (pos + 512U + size > length) {
      return 0;
    }
    if ((entry.flags & ZZ9K_ARCHIVE_TAR_FLAG_PAX_HEADER) != 0U) {
      ZZ9KArchiveTarPaxInfo pax;

      if (!zz9k_archive_tar_parse_pax_info(
              data + entry.data_offset, size, &pax)) {
        return 0;
      }
      if (pax.has_size) {
        pending_size = pax.size;
        pending_size_valid = 1;
      }
      if (pax.has_path) {
        pending_name_skip = pax.path_skip;
      }
    } else if ((entry.flags & ZZ9K_ARCHIVE_TAR_FLAG_GNU_LONG_NAME) != 0U) {
      char scratch[ZZ9K_ARCHIVE_MAX_NAME];
      int skip = 0;

      if (!zz9k_archive_tar_copy_data_name(
              data + entry.data_offset, size,
              scratch, sizeof(scratch), &skip)) {
        return 0;
      }
      pending_name_skip = skip;
    } else if ((entry.flags & (ZZ9K_ARCHIVE_TAR_FLAG_GNU_LONG_NAME |
                               ZZ9K_ARCHIVE_TAR_FLAG_PAX_HEADER)) == 0U) {
      if (pending_name_skip) {
        entry.flags |= ZZ9K_ARCHIVE_TAR_FLAG_SKIP;
        pending_name_skip = 0;
      }
      if (pending_size_valid) {
        size = pending_size;
        pending_size_valid = 0;
        if (pos + 512U + size > length) {
          return 0;
        }
      }
    }
    if ((entry.flags & ZZ9K_ARCHIVE_TAR_FLAG_SKIP) == 0U) {
      entries++;
    }
    blocks = (size + 511U) / 512U;
    pos += 512U + blocks * 512U;
  }
  *count = entries;
  return 1;
}

static int zz9k_archive_write_entry(const char *output_dir,
                                    const ZZ9KArchiveEntry *entry,
                                    const uint8_t *data)
{
  char *path;
  ZZ9KArchiveEntry output_entry;
  int ok;

  if (!zz9k_archive_output_entry(entry, &output_entry)) {
    return 1;
  }
  if (!zz9k_archive_path_is_safe(output_entry.name)) {
    printf("unsafe path rejected: %s\n", entry->name);
    return 0;
  }
  path = zz9k_archive_join_path(output_dir, output_entry.name);
  if (!path) {
    printf("path allocation failed\n");
    return 0;
  }
  if (output_entry.is_dir) {
    zz9k_archive_trim_trailing_separators(path);
  }
  if (output_entry.is_dir && zz9k_archive_path_exists(path) &&
      !zz9k_archive_path_is_dir(path)) {
    printf("output path is a file: %s\n", path);
    free(path);
    return 0;
  }
  if (!output_entry.is_dir && zz9k_archive_path_is_dir(path)) {
    printf("output path is a directory: %s\n", path);
    free(path);
    return 0;
  }
  if (!output_entry.is_dir && zz9k_archive_skip_existing_outputs &&
      zz9k_archive_path_exists(path)) {
    printf("s %s\n", path);
    zz9k_archive_last_output_skipped = 1;
    free(path);
    return 1;
  }
  if (!output_entry.is_dir && !zz9k_archive_overwrite_outputs &&
      zz9k_archive_path_exists(path)) {
    printf("output exists, use --overwrite: %s\n", path);
    free(path);
    return 0;
  }
  if (zz9k_archive_dry_run_outputs) {
    printf("dry %s\n", output_entry.name);
    zz9k_archive_last_output_skipped = 0;
    zz9k_archive_last_output_dry_run = 1;
    free(path);
    return 1;
  }
  if (!zz9k_archive_ensure_parent_dirs(output_dir, output_entry.name)) {
    printf("could not create parent directories for %s\n", output_entry.name);
    free(path);
    return 0;
  }
  if (output_entry.is_dir) {
    ok = zz9k_archive_mkdir_one(path);
  } else {
    ok = zz9k_archive_write_file(path, data, output_entry.uncompressed_size);
  }
  if (ok && !zz9k_archive_last_output_skipped &&
      !zz9k_archive_last_output_dry_run) {
    printf("x %s\n", output_entry.name);
  }
  free(path);
  return ok;
}

/* Picks a sibling path not currently in use: "<path><base>", or when
   that sibling exists, "<path><tag>N" for N in 1..31. The probe STARTS
   from a per-process unique offset (vblank/tick entropy ^ a counter),
   so two concurrent extractions racing between this check and their
   "wb" open pick different names in practice. The caller allocates
   strlen(path) + 16 bytes. Never opens or truncates anything, so an
   unrelated user file -- or another archive member named like the
   suffix -- is never clobbered. Returns 0 when every candidate
   exists. */
static uint32_t zz9k_archive_probe_entropy(void)
{
#if defined(__amigaos__)
  return *(volatile uint32_t *)0x422UL; /* vertical-blank counter */
#else
  return (uint32_t)time(0);
#endif
}

static uint32_t zz9k_archive_probe_seed(void)
{
  static uint32_t counter;

  counter += 1U;
  return zz9k_archive_probe_entropy() ^ (counter * 2654435761U);
}

static int zz9k_archive_probe_sibling(char *dst,
                                      const char *path,
                                      const char *base,
                                      const char *tag)
{
  size_t path_len = strlen(path);
  size_t comp_start = path_len;
  uint32_t seed = zz9k_archive_probe_seed() % 32U;
  uint32_t attempt;

  /* Staging names are transient (renamed onto the destination or
     removed), so only legality and uniqueness matter -- not identity.
     Trim the final component so the longest suffix still fits common
     filesystem component limits (Amiga FFS: 107 bytes; POSIX: 255):
     an untrimmed near-limit basename would make every probe an
     ENAMETOOLONG "free" name that fopen then rejects. 96 + ".zz9k-t31"
     (9) + NUL fits the tightest of them. */
  while (comp_start > 0U && path[comp_start - 1U] != '/' &&
         path[comp_start - 1U] != ':' &&
         path[comp_start - 1U] != '\\') {
    comp_start--;
  }
  if (path_len - comp_start > 96U) {
    path_len = comp_start + 96U;
  }
  for (attempt = 0U; attempt < 32U; attempt++) {
    uint32_t slot = (seed + attempt) % 32U;

    if (slot == 0U) {
      sprintf(dst, "%.*s%s", (int)path_len, path, base);
    } else {
      sprintf(dst, "%.*s%s%u", (int)path_len, path, tag,
              (unsigned int)slot);
    }
    if (!zz9k_archive_path_exists(dst)) {
      return 1;
    }
  }
  return 0;
}

/* Replaces `path` with the fully-written `tmp_path` without ever leaving
   the destination destroyed: the old file moves to a collision-probed
   backup first and is restored if the final rename fails. On failure the
   temporary is removed and the destination is untouched (or restored);
   on success the backup is removed. Returns 1 on success. */
static int zz9k_archive_replace_with_staged(const char *tmp_path,
                                            const char *path,
                                            const char *name)
{
  char *backup = 0;

  if (zz9k_archive_path_exists(path)) {
    backup = (char *)malloc(strlen(path) + 16U);
    if (!backup) {
      remove(tmp_path);
      printf("path allocation failed\n");
      return 0;
    }
    if (!zz9k_archive_probe_sibling(backup, path, ".zz9k-old", ".zz9k-o")) {
      free(backup);
      remove(tmp_path);
      printf("output backup name unavailable: %s\n", name);
      return 0;
    }
    if (rename(path, backup) != 0) {
      free(backup);
      remove(tmp_path);
      printf("output rename failed: %s\n", name);
      return 0;
    }
  }
  if (rename(tmp_path, path) != 0) {
    remove(tmp_path);
    if (backup) {
      if (rename(backup, path) != 0) {
        /* Both renames failed (a dropped network volume can do this):
         the destination is gone and the user's original survives ONLY
         at the backup path -- say so, or the file looks simply lost. */
        printf("output restore failed: %s (original kept as %s)\n",
               name, backup);
      }
      free(backup);
    }
    printf("output rename failed: %s\n", name);
    return 0;
  }
  if (backup) {
    if (remove(backup) != 0) {
      printf("output backup removal failed: %s\n", backup);
    }
    free(backup);
  }
  return 1;
}

static int zz9k_archive_write_file_range_entry(
    const char *output_dir,
    const ZZ9KArchiveEntry *entry,
    const char *input_path,
    int verify_crc)
{
  FILE *input = 0;
  FILE *output = 0;
  char *path = 0;
  char *tmp_path = 0;
  ZZ9KArchiveEntry output_entry;
  uint8_t *chunk = 0;
  uint32_t range_crc = 0U;
  uint32_t remaining;
  int ok = 0;

  if (!entry || !input_path || entry->is_dir ||
      entry->compressed_size != entry->uncompressed_size ||
      entry->data_offset > 0x7fffffffUL) {
    return 0;
  }
  zz9k_archive_last_output_skipped = 0;
  zz9k_archive_last_output_dry_run = 0;
  if (!zz9k_archive_output_entry(entry, &output_entry)) {
    return 1;
  }
  if (!zz9k_archive_path_is_safe(output_entry.name)) {
    printf("unsafe path rejected: %s\n", entry->name);
    return 0;
  }
  path = zz9k_archive_join_path(output_dir, output_entry.name);
  if (!path) {
    printf("path allocation failed\n");
    return 0;
  }
  if (zz9k_archive_path_is_dir(path)) {
    printf("output path is a directory: %s\n", path);
    goto out;
  }
  if (zz9k_archive_overwrite_outputs &&
      !zz9k_archive_dry_run_outputs &&
      !zz9k_archive_skip_existing_outputs &&
      zz9k_archive_paths_same_file(input_path, path)) {
    /* Same guard as the LHA and tar engines, at the one choke point all
       file-backed range writes (ZIP store, 7z Copy, LHA LH0) share: a
       member named like the archive would otherwise run the staged
       backup-rename dance on the still-open source archive and replace
       it with the member's own bytes. Only --overwrite can clobber:
       the other modes refuse or skip existing outputs before any
       rename. */
    printf("output path is the archive itself, refusing: %s\n",
           output_entry.name);
    goto out;
  }
  if (zz9k_archive_skip_existing_outputs && zz9k_archive_path_exists(path)) {
    printf("s %s\n", path);
    zz9k_archive_last_output_skipped = 1;
    ok = 1;
    goto out;
  }
  if (!zz9k_archive_overwrite_outputs && zz9k_archive_path_exists(path)) {
    printf("output exists, use --overwrite: %s\n", path);
    goto out;
  }
  if (zz9k_archive_dry_run_outputs) {
    printf("dry %s\n", output_entry.name);
    zz9k_archive_last_output_dry_run = 1;
    ok = 1;
    goto out;
  }
  if (!zz9k_archive_ensure_parent_dirs(output_dir, output_entry.name)) {
    printf("could not create parent directories for %s\n", output_entry.name);
    goto out;
  }
  input = fopen(input_path, "rb");
  if (!input) {
    printf("open failed: %s\n", input_path);
    goto out;
  }
  if (fseek(input, (long)entry->data_offset, SEEK_SET) != 0) {
    printf("file range seek failed: %s\n", input_path);
    goto out;
  }
  /* ALL file-backed range writes stage to a collision-safe sibling
     temporary and replace the destination only on success: a corrupt
     member under --overwrite -- or a Ctrl-C between chunks -- must
     never destroy the file that was already there. Verified callers
     additionally gate the replacement on the inline CRC. */
  {
    tmp_path = (char *)malloc(strlen(path) + 16U);
    if (!tmp_path) {
      printf("path allocation failed\n");
      goto out;
    }
    if (!zz9k_archive_probe_sibling(tmp_path, path, ".zz9k-tmp",
                                    ".zz9k-t")) {
      printf("output temporary name unavailable: %s\n", output_entry.name);
      goto out;
    }
    output = fopen(tmp_path, "wb");
  }
  if (!output) {
    printf("open output failed: %s\n", path);
    goto out;
  }
  chunk = (uint8_t *)malloc(ZZ9K_ARCHIVE_STREAM_CHUNK);
  if (!chunk) {
    printf("file range chunk allocation failed\n");
    goto out;
  }

  remaining = entry->compressed_size;
  while (remaining != 0U) {
    uint32_t part = remaining > ZZ9K_ARCHIVE_STREAM_CHUNK ?
        ZZ9K_ARCHIVE_STREAM_CHUNK : remaining;

    if (zz9k_archive_cancelled()) {

      goto out;
    }

    if (fread(chunk, 1U, part, input) != part) {
      printf("file range read failed: %s\n", input_path);
      goto out;
    }
    if (verify_crc) {
      range_crc = zz9k_archive_crc32(range_crc, chunk, part);
    }
    if (fwrite(chunk, 1U, part, output) != part) {
      printf("file range write failed: %s\n", output_entry.name);
      goto out;
    }
    remaining -= part;
  }
  if (verify_crc && (entry->flags & ZZ9K_ARCHIVE_ENTRY_FLAG_CRC32) != 0U &&
      range_crc != entry->crc32) {
    /* Verification failed: the destination was never touched. */
    fclose(output);
    output = 0;
    remove(tmp_path);
    printf("stored entry crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
           output_entry.name, (unsigned long)range_crc,
           (unsigned long)entry->crc32);
    goto out;
  }
  if (tmp_path) {
    /* Verified clean: swap the temporary in, preserving the old
       destination until the replacement succeeds. */
    if (fclose(output) != 0) {
      output = 0;
      remove(tmp_path);
      printf("file range write failed: %s\n", output_entry.name);
      goto out;
    }
    output = 0;
    if (!zz9k_archive_replace_with_staged(tmp_path, path,
                                         output_entry.name)) {
      goto out;
    }
  }

  ok = 1;

out:
  if (output && fclose(output) != 0) {
    ok = 0;
  }
  if (!ok && tmp_path) {
    remove(tmp_path); /* never leave a stale temporary behind */
  }
  if (input) {
    fclose(input);
  }
  if (ok && !zz9k_archive_last_output_skipped &&
      !zz9k_archive_last_output_dry_run) {
    printf("x %s\n", output_entry.name);
  }
  free(chunk);
  free(tmp_path);
  free(path);
  return ok;
}

static int zz9k_archive_lha_method_supported(uint32_t method)
{
  return method == ZZ9K_ARCHIVE_LHA_METHOD_LH5 ||
         method == ZZ9K_ARCHIVE_LHA_METHOD_LH1 ||
         method == ZZ9K_ARCHIVE_LHA_METHOD_LH6 ||
         method == ZZ9K_ARCHIVE_LHA_METHOD_LH7;
}

static uint32_t zz9k_archive_lha_method_to_compression(uint32_t method)
{
  switch (method) {
  case ZZ9K_ARCHIVE_LHA_METHOD_LH1: return ZZ9K_COMPRESSION_LH1;
  case ZZ9K_ARCHIVE_LHA_METHOD_LH5: return ZZ9K_COMPRESSION_LH5;
  case ZZ9K_ARCHIVE_LHA_METHOD_LH6: return ZZ9K_COMPRESSION_LH6;
  case ZZ9K_ARCHIVE_LHA_METHOD_LH7: return ZZ9K_COMPRESSION_LH7;
  default: return 0U;
  }
}

/* Diagnostic tally for the LHA decode-offload path.  A whole-partition backup
   is thousands of small members, and every fall-back to the 68k software
   decoder is silent -- board alloc failures print, but a firmware CRC/size
   rejection (else-branch below) re-decodes the whole member on the CPU without
   a word.  Without this count there is no way to tell an archive that offloaded
   cleanly (so 98% CPU == transfer/overhead-bound) from one that quietly
   re-decoded every member on the 68k (so 98% CPU == no offload at all).
   Printed once per test/extract run. */
static unsigned long zz9k_lha_diag_offloaded;
static unsigned long zz9k_lha_diag_batched;
static unsigned long zz9k_lha_diag_chunks;
static unsigned long zz9k_lha_diag_sw_crc_miss;
static unsigned long zz9k_lha_diag_sw_codec_fail;
static unsigned long zz9k_lha_diag_sw_unavailable;
static unsigned long zz9k_lha_diag_bytes_in;

/* Lazily-fetched, once-per-run snapshot of the board's shared-heap state,
   consulted before a per-member offload attempt so members that cannot
   possibly fit are skipped straight to software instead of paying for a
   doomed board allocation (and its diagnostic printout) every time.
   0 = not yet queried, 1 = queried OK, -1 = query failed (behave as before
   and just try the board). */
static ZZ9KDiagInfo zz9k_lha_board_diag;
static int zz9k_lha_board_diag_valid;

static void zz9k_lha_diag_reset(void)
{
  zz9k_lha_diag_offloaded = 0UL;
  zz9k_lha_diag_batched = 0UL;
  zz9k_lha_diag_chunks = 0UL;
  zz9k_lha_diag_sw_crc_miss = 0UL;
  zz9k_lha_diag_sw_codec_fail = 0UL;
  zz9k_lha_diag_sw_unavailable = 0UL;
  zz9k_lha_diag_bytes_in = 0UL;
  zz9k_lha_board_diag_valid = 0;
}

static void zz9k_lha_diag_report(void)
{
  printf("lha offload diag: offloaded=%lu batched=%lu chunks=%lu "
         "sw:crc/size-miss=%lu sw:codec-fail=%lu sw:unavailable=%lu "
         "bytes-in=%lu\n",
         zz9k_lha_diag_offloaded, zz9k_lha_diag_batched,
         zz9k_lha_diag_chunks, zz9k_lha_diag_sw_crc_miss,
         zz9k_lha_diag_sw_codec_fail, zz9k_lha_diag_sw_unavailable,
         zz9k_lha_diag_bytes_in);
}

/* Input source for the LHA decode paths: either a whole-archive memory
   image (the in-memory engine and the host tests) or a seekable archive
   file (the file-backed engine, which never loads the whole archive).
   File mode keeps one archive handle open for the whole run and reuses a
   growable bounce buffer for the compressed bytes of one member at a
   time, so peak tool RAM stays bounded by the largest batch blob (plus
   one oversize member on the per-member offload path), never by the
   archive size. */
typedef struct ZZ9KLhaSource {
  const uint8_t *data;         /* whole-archive image, or 0 in file mode */
  uint32_t length;             /* archive length in bytes */
  const char *path;            /* archive path in file mode, else 0 */
  FILE *file;                  /* open archive handle in file mode, else 0 */
  uint8_t *bounce;             /* file-mode member buffer (grows on demand) */
  uint32_t bounce_capacity;
} ZZ9KLhaSource;

static void zz9k_archive_lha_source_init_mem(ZZ9KLhaSource *src,
                                             const uint8_t *data,
                                             uint32_t length)
{
  memset(src, 0, sizeof(*src));
  src->data = data;
  src->length = length;
}

static int zz9k_archive_lha_source_open_file(ZZ9KLhaSource *src,
                                             const char *path,
                                             uint32_t length)
{
  memset(src, 0, sizeof(*src));
  src->path = path;
  src->length = length;
  src->file = fopen(path, "rb");
  return src->file != 0;
}

static void zz9k_archive_lha_source_close(ZZ9KLhaSource *src)
{
  if (src->file) {
    fclose(src->file);
    src->file = 0;
  }
  free(src->bounce);
  src->bounce = 0;
  src->bounce_capacity = 0U;
}

/* Pointer to a member's compressed bytes. Memory mode returns a direct
   pointer into the archive image. File mode reads the range into the
   bounce buffer; the pointer stays valid until the next call. Returns 0
   when the range cannot be brought into RAM (read error, or an allocation
   the heap cannot satisfy) -- callers fall back to decode paths that do
   not need the whole member resident. */
static int zz9k_archive_lha_src_member(ZZ9KLhaSource *src,
                                       const ZZ9KArchiveEntry *entry,
                                       const uint8_t **member)
{
  if (!src || !entry || !member) {
    return 0;
  }
  if (entry->data_offset > src->length ||
      entry->compressed_size > src->length - entry->data_offset) {
    return 0;
  }
  if (src->data) {
    *member = src->data + entry->data_offset;
    return 1;
  }
  if (!src->file) {
    return 0;
  }
  if (entry->compressed_size == 0U) {
    *member = src->bounce; /* may be NULL; callers treat 0-length input */
    return 1;
  }
  if (entry->compressed_size > src->bounce_capacity) {
    uint8_t *grown =
        (uint8_t *)realloc(src->bounce, (size_t)entry->compressed_size);

    if (!grown) {
      return 0;
    }
    src->bounce = grown;
    src->bounce_capacity = entry->compressed_size;
  }
  if (fseek(src->file, (long)entry->data_offset, SEEK_SET) != 0 ||
      fread(src->bounce, 1U, (size_t)entry->compressed_size, src->file) !=
          (size_t)entry->compressed_size) {
    return 0;
  }
  *member = src->bounce;
  return 1;
}

/* Pure-software LHA member decode (the 68k fallback). output == NULL means
   verify-only. Kept separate from the offload path so batch fallbacks can
   decode in software WITHOUT re-attempting the offload that already
   produced a bad result for the member.

   File mode streams the member straight out of the archive: the decoder
   consumes exactly compressed_size input bytes (its acceptance check
   counts them), so the seek positions it as precisely as the memory
   image's tmpfile copy -- and skips that copy entirely. */
static int zz9k_archive_lha_software_decode_to_file(
    ZZ9KLhaSource *src,
    const ZZ9KArchiveEntry *entry,
    FILE *output)
{
  FILE *input;
  uint16_t decoded_crc = 0U;
  int input_is_archive;
  int ok;

  if (!src || !entry) {
    return 0;
  }
  if (entry->data_offset > src->length ||
      entry->compressed_size > src->length - entry->data_offset) {
    return 0;
  }
  if (src->file) {
    if (fseek(src->file, (long)entry->data_offset, SEEK_SET) != 0) {
      printf("lha member seek failed: %s\n", entry->name);
      return 0;
    }
    input = src->file;
    input_is_archive = 1;
  } else {
    input = tmpfile();
    if (!input) {
      printf("lha temporary input failed: %s\n", entry->name);
      return 0;
    }
    input_is_archive = 0;
    if (entry->compressed_size != 0U &&
        fwrite(src->data + entry->data_offset, 1U,
               (size_t)entry->compressed_size, input) !=
            (size_t)entry->compressed_size) {
      fclose(input);
      printf("lha temporary input write failed: %s\n", entry->name);
      return 0;
    }
    if (fseek(input, 0L, SEEK_SET) != 0) {
      fclose(input);
      printf("lha temporary input seek failed: %s\n", entry->name);
      return 0;
    }
  }
  ok = zz9k_lha_unix_decode_method(
      input, output, entry->uncompressed_size, entry->compressed_size,
      (uint16_t)entry->crc32,
      (entry->flags & ZZ9K_ARCHIVE_ENTRY_FLAG_CRC32) != 0U,
      &decoded_crc,
      (int)entry->method);
  if (!input_is_archive) {
    fclose(input);
  }
  if (!ok) {
    if ((entry->flags & ZZ9K_ARCHIVE_ENTRY_FLAG_CRC32) != 0U) {
      printf("lha lh%lu decode failed: %s crc=0x%04x expected=0x%04lx\n",
             (unsigned long)entry->method, entry->name,
             (unsigned int)decoded_crc, (unsigned long)entry->crc32);
    } else {
      printf("lha lh%lu decode failed: %s (no stored CRC; size/stream "
             "check)\n",
             (unsigned long)entry->method, entry->name);
    }
  }
  return ok;
}

/* Can this member's per-member offload possibly get its board buffers?
   Needs one block for the compressed input and one for the decoded output
   (verify-only still allocates the output on the board). Conservative:
   both must fit the largest free block, and their sum (plus alignment
   slack) must fit the total free space. Computed in 64-bit so oversize
   archive-reported sizes cannot wrap the check. */
static int zz9k_archive_lha_offload_fits(const ZZ9KDiagInfo *diag,
                                         const ZZ9KArchiveEntry *entry)
{
  uint64_t needed;
  uint32_t slack = 64U;

  if (!diag) {
    return 1; /* no diag info: try the board as before */
  }
  if (entry->compressed_size > diag->shared_heap_largest_free ||
      entry->uncompressed_size > diag->shared_heap_largest_free) {
    return 0;
  }
  needed = (uint64_t)entry->compressed_size +
           (uint64_t)entry->uncompressed_size + (uint64_t)slack;
  if (needed > (uint64_t)diag->shared_heap_free) {
    return 0;
  }
  return 1;
}

static int zz9k_archive_lha_decode_method_to_file(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    ZZ9KLhaSource *src,
    const ZZ9KArchiveEntry *entry,
    FILE *output)
{
  if (!src || !entry || entry->data_offset > src->length ||
      entry->compressed_size > src->length - entry->data_offset ||
      !zz9k_archive_lha_method_supported(entry->method)) {
    return 0;
  }

  if (ctx && service &&
      zz9k_archive_lha_method_supported(entry->method)) {
    uint32_t algo = zz9k_archive_lha_method_to_compression(entry->method);
    if (algo != 0U && zz9k_archive_service_supports(service, algo)) {
      uint8_t *decoded = 0;
      ZZ9KDecompressResult res;
      int try_offload = 1;

      if (zz9k_lha_board_diag_valid == 0) {
        int status = zz9k_read_diag(ctx, &zz9k_lha_board_diag);

        zz9k_archive_note_status(status);
        if (status == ZZ9K_STATUS_CANCELLED) {
          /* The wait consumed the break: with try_offload still true the
             member would fully decode (and write) before the next walk
             checkpoint sees the latch. Stop before starting another
             codec operation. */
          return 0;
        }
        zz9k_lha_board_diag_valid = (status == ZZ9K_STATUS_OK) ? 1 : -1;
      }
      if (zz9k_lha_board_diag_valid == 1 &&
          !zz9k_archive_lha_offload_fits(&zz9k_lha_board_diag, entry)) {
        printf("lha lh%lu too large for board heap, software decode: %s\n",
               (unsigned long)entry->method, entry->name);
        zz9k_lha_diag_sw_codec_fail++;
        try_offload = 0;
      }

      /* "test" passes output == NULL: we only need the ARM-computed CRC, so
         run the codec in verify-only mode. The board still decodes and
         checksums the member, but the decoded plaintext is never copied back
         across Zorro -- on a whole-partition backup that spares a second pass
         of the entire uncompressed size over the bus. Extract keeps the
         plaintext so it can be written out. */
      if (try_offload) {
        const uint8_t *member = 0;
        int verify_only = (output == 0);

        if (!zz9k_archive_lha_src_member(src, entry, &member)) {
          /* The member's compressed bytes could not be brought into RAM
             (oversize for the heap, or a read error). The software path
             below streams it straight from the file instead. */
          printf("lha lh%lu member not resident, software decode: %s\n",
                 (unsigned long)entry->method, entry->name);
          zz9k_lha_diag_sw_codec_fail++;
        } else {
          zz9k_lha_diag_bytes_in += (unsigned long)entry->compressed_size;
          if (zz9k_archive_decompress_to_memory_ex(
                  ctx, service, algo, member,
                  entry->compressed_size, entry->uncompressed_size,
                  &decoded, &res, verify_only)) {
            int crc_ok = 1;
            if ((entry->flags & ZZ9K_ARCHIVE_ENTRY_FLAG_CRC32) != 0U) {
              crc_ok = ((uint16_t)res.checksum == (uint16_t)entry->crc32);
            }
            if (crc_ok && res.bytes_written == entry->uncompressed_size) {
              int wrote_ok = 1;
              if (output && entry->uncompressed_size != 0U &&
                  fwrite(decoded, 1U, (size_t)entry->uncompressed_size,
                         output) != (size_t)entry->uncompressed_size) {
                wrote_ok = 0;
              }
              free(decoded); /* NULL in verify-only mode; free(NULL) is safe */
              if (wrote_ok) {
                zz9k_lha_diag_offloaded++;
                return 1;   /* offloaded */
              }
              printf("lha lh%lu offload write failed: %s\n",
                     (unsigned long)entry->method, entry->name);
              return 0;
            } else {
              free(decoded);    /* CRC/size miss -> fall back to software */
              zz9k_lha_diag_sw_crc_miss++;
            }
          } else {
            /* board alloc / codec error (the helper already printed why).
               A Ctrl-C that aborted the board decode must stop the run,
               never fall back to a full software decode of the member. */
            if (zz9k_archive_cancelled()) {

              return 0;
            }
            zz9k_lha_diag_sw_codec_fail++;
          }
        }
      }
    } else {
      zz9k_lha_diag_sw_unavailable++;   /* method not advertised by service */
    }
  } else {
    zz9k_lha_diag_sw_unavailable++;     /* no board/service: software-only */
  }

  return zz9k_archive_lha_software_decode_to_file(src, entry, output);
}

static int zz9k_archive_extract_lha_lh5(ZZ9KContext *ctx,
                                        const ZZ9KServiceInfo *service,
                                        ZZ9KLhaSource *src,
                                        const char *output_dir,
                                        const ZZ9KArchiveEntry *entry)
{
  FILE *file = 0;
  char *final_path = 0;
  char *tmp_path = 0;
  int ok;

  /* Staged like the ZIP/7z verified paths: a cancelled or failed decode
     must leave the pre-existing destination untouched rather than
     truncated or partially replaced. */
  if (!zz9k_archive_open_output_staged(output_dir, entry, &file,
                                       &final_path, &tmp_path)) {
    return 0;
  }
  ok = zz9k_archive_lha_decode_method_to_file(ctx, service, src,
                                              entry, file);
  if (fclose(file) != 0) {
    ok = 0;
  }
  if (ok && tmp_path) {
    ok = zz9k_archive_replace_with_staged(tmp_path, final_path,
                                          entry->name);
  } else if (!ok && tmp_path) {
    remove(tmp_path); /* failed decode: keep the old destination */
  }
  free(final_path);
  free(tmp_path);
  if (ok && !zz9k_archive_last_output_skipped &&
      !zz9k_archive_last_output_dry_run) {
    printf("x %s\n", entry->name);
  }
  return ok;
}

static int zz9k_archive_extract_lha_software(ZZ9KLhaSource *src,
                                             const char *output_dir,
                                             const ZZ9KArchiveEntry *entry)
{
  FILE *file = 0;
  char *final_path = 0;
  char *tmp_path = 0;
  int ok;

  /* Same staging as the offload path: a cancelled software decode keeps
     the pre-existing destination intact. */
  if (!zz9k_archive_open_output_staged(output_dir, entry, &file,
                                       &final_path, &tmp_path)) {
    return 0;
  }
  ok = zz9k_archive_lha_software_decode_to_file(src, entry, file);
  if (fclose(file) != 0) {
    ok = 0;
  }
  if (ok && tmp_path) {
    ok = zz9k_archive_replace_with_staged(tmp_path, final_path,
                                          entry->name);
  } else if (!ok && tmp_path) {
    remove(tmp_path); /* failed decode: keep the old destination */
  }
  free(final_path);
  free(tmp_path);
  if (ok && !zz9k_archive_last_output_skipped &&
      !zz9k_archive_last_output_dry_run) {
    printf("x %s\n", entry->name);
  }
  return ok;
}

static int zz9k_archive_handle_tar(ZZ9KContext *ctx,
                                   const ZZ9KServiceInfo *service,
                                   const uint8_t *data,
                                   uint32_t length,
                                   const char *command,
                                   const char *output_dir)
{
  ZZ9KArchiveEntry *entries;
  uint32_t count;
  uint32_t i;
  int ok = 1;

  (void)ctx;
  (void)service;
  if (!zz9k_archive_count_tar_entries(data, length, &count) ||
      !zz9k_archive_alloc_entries(count, &entries)) {
    printf("tar parse failed\n");
    return 0;
  }
  if (!zz9k_archive_tar_list(data, length, entries, count, &count)) {
    printf("tar parse failed\n");
    free(entries);
    return 0;
  }

  for (i = 0U; i < count; i++) {
    if (!zz9k_archive_entry_matches_filter(&entries[i])) {
      continue;
    }
    if (strcmp(command, "l") == 0) {
      zz9k_archive_print_entry(&entries[i]);
    } else if (strcmp(command, "t") == 0) {
      if (!zz9k_archive_path_is_safe(entries[i].name)) {
        printf("unsafe path rejected: %s\n", entries[i].name);
        ok = 0;
      }
    } else {
      ok &= zz9k_archive_write_entry(output_dir, &entries[i],
                                     data + entries[i].data_offset);
    }
  }
  if (strcmp(command, "t") == 0 && ok) {
    printf("tar test ok: %lu entries\n", (unsigned long)count);
  }
  free(entries);
  return ok;
}

static int zz9k_archive_decompress_test_to_result(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    uint32_t algorithm,
    const uint8_t *compressed,
    uint32_t compressed_length,
    uint32_t output_limit,
    ZZ9KDecompressResult *result)
{
  ZZ9KSharedBuffer input;
  ZZ9KDecompressTestDesc desc;
  int status;
  int ok = 0;

  memset(&input, 0, sizeof(input));
  memset(result, 0, sizeof(*result));

  if (!zz9k_archive_service_supports_decompress_test(service, algorithm)) {
    printf("%s streamed test path not advertised by codec service\n",
           zz9k_compression_algorithm_text(algorithm));
    return 0;
  }
  if (compressed_length == 0U || output_limit == 0U) {
    printf("unsupported empty codec test job\n");
    return 0;
  }

  status = zz9k_alloc_shared(ctx, compressed_length, 16U, 0U, &input);
  if (status != ZZ9K_STATUS_OK) {
    printf("alloc compressed failed: %s (%d), requested=%lu bytes\n",
           zz9k_status_name(status), status,
           (unsigned long)compressed_length);
    zz9k_archive_print_shared_diag(ctx, "compressed input",
                                   compressed_length);
    goto out;
  }
  if (!zz9k_shared_copy_to(&input, 0U, compressed, compressed_length)) {
    printf("compressed copy failed\n");
    goto out;
  }
  if (!zz9k_compression_build_decompress_test_desc(
          &desc, algorithm, input.handle, 0U, compressed_length,
          output_limit, ZZ9K_DECOMPRESS_FLAG_EXPECT_END)) {
    printf("could not build decompression test descriptor\n");
    goto out;
  }

  status = zz9k_decompress_test(ctx, &desc, result);
  if (status != ZZ9K_STATUS_OK) {
    printf("%s streamed test failed: %s (%d), input=%lu limit=%lu\n",
           zz9k_compression_algorithm_text(algorithm),
           zz9k_status_name(status), status,
           (unsigned long)compressed_length,
           (unsigned long)output_limit);
    zz9k_archive_print_shared_diag(ctx, "codec test failure",
                                   compressed_length);
    goto out;
  }
  ok = 1;

out:
  if (input.handle != 0U && input.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, input.handle);
  }
  return ok;
}

static int zz9k_archive_open_output_entry(const char *output_dir,
                                          const ZZ9KArchiveEntry *entry,
                                          FILE **file)
{
  char *path;
  ZZ9KArchiveEntry output_entry;

  *file = 0;
  zz9k_archive_last_output_skipped = 0;
  zz9k_archive_last_output_dry_run = 0;
  if (!zz9k_archive_output_entry(entry, &output_entry)) {
    *file = zz9k_archive_open_discard_file();
    zz9k_archive_last_output_skipped = 1;
    return *file != 0;
  }
  if (!zz9k_archive_path_is_safe(output_entry.name)) {
    printf("unsafe path rejected: %s\n", entry->name);
    return 0;
  }
  path = zz9k_archive_join_path(output_dir, output_entry.name);
  if (!path) {
    printf("path allocation failed\n");
    return 0;
  }
  if (zz9k_archive_path_is_dir(path)) {
    printf("output path is a directory: %s\n", path);
    free(path);
    return 0;
  }
  if (zz9k_archive_skip_existing_outputs && zz9k_archive_path_exists(path)) {
    printf("s %s\n", path);
    zz9k_archive_last_output_skipped = 1;
    *file = zz9k_archive_open_discard_file();
    free(path);
    return *file != 0;
  }
  if (!zz9k_archive_overwrite_outputs && zz9k_archive_path_exists(path)) {
    printf("output exists, use --overwrite: %s\n", path);
    free(path);
    return 0;
  }
  if (zz9k_archive_dry_run_outputs) {
    printf("dry %s\n", output_entry.name);
    zz9k_archive_last_output_dry_run = 1;
    *file = zz9k_archive_open_discard_file();
    free(path);
    return *file != 0;
  }
  if (!zz9k_archive_ensure_parent_dirs(output_dir, output_entry.name)) {
    printf("could not create parent directories for %s\n", output_entry.name);
    free(path);
    return 0;
  }
  *file = fopen(path, "wb");
  if (!*file) {
    printf("open output failed: %s\n", path);
    free(path);
    return 0;
  }
  free(path);
  return 1;
}

/* Staged open for member extraction: identical checks to
   zz9k_archive_open_output_entry, but payload bytes go to a
   collision-probed temporary sibling; the caller replaces the
   destination only after the member completes. On the discard/skip/dry
   paths *tmp_path_out stays 0 (nothing staged, nothing to replace) and
   the semantics match open_output_entry exactly. Callers free both
   strings on completion. */
static int zz9k_archive_open_output_staged(
    const char *output_dir,
    const ZZ9KArchiveEntry *entry,
    FILE **file,
    char **final_path_out,
    char **tmp_path_out)
{
  char *path;
  ZZ9KArchiveEntry output_entry;

  *file = 0;
  *final_path_out = 0;
  *tmp_path_out = 0;
  zz9k_archive_last_output_skipped = 0;
  zz9k_archive_last_output_dry_run = 0;
  if (!zz9k_archive_output_entry(entry, &output_entry)) {
    *file = zz9k_archive_open_discard_file();
    zz9k_archive_last_output_skipped = 1;
    return *file != 0;
  }
  if (!zz9k_archive_path_is_safe(output_entry.name)) {
    printf("unsafe path rejected: %s\n", entry->name);
    return 0;
  }
  path = zz9k_archive_join_path(output_dir, output_entry.name);
  if (!path) {
    printf("path allocation failed\n");
    return 0;
  }
  if (zz9k_archive_path_is_dir(path)) {
    printf("output path is a directory: %s\n", path);
    free(path);
    return 0;
  }
  if (zz9k_archive_skip_existing_outputs && zz9k_archive_path_exists(path)) {
    printf("s %s\n", path);
    zz9k_archive_last_output_skipped = 1;
    *file = zz9k_archive_open_discard_file();
    free(path);
    return *file != 0;
  }
  if (!zz9k_archive_overwrite_outputs && zz9k_archive_path_exists(path)) {
    printf("output exists, use --overwrite: %s\n", path);
    free(path);
    return 0;
  }
  if (zz9k_archive_dry_run_outputs) {
    printf("dry %s\n", output_entry.name);
    zz9k_archive_last_output_dry_run = 1;
    *file = zz9k_archive_open_discard_file();
    free(path);
    return *file != 0;
  }
  if (!zz9k_archive_ensure_parent_dirs(output_dir, output_entry.name)) {
    printf("could not create parent directories for %s\n",
           output_entry.name);
    free(path);
    return 0;
  }
  {
    char *tmp = (char *)malloc(strlen(path) + 16U);

    if (!tmp) {
      printf("path allocation failed\n");
      free(path);
      return 0;
    }
    if (!zz9k_archive_probe_sibling(tmp, path, ".zz9k-tmp", ".zz9k-t")) {
      printf("output temporary name unavailable: %s\n", output_entry.name);
      free(tmp);
      free(path);
      return 0;
    }
    *file = fopen(tmp, "wb");
    if (!*file) {
      printf("open output failed: %s\n", path);
      free(tmp);
      free(path);
      return 0;
    }
    *final_path_out = path;
    *tmp_path_out = tmp;
  }
  return 1;
}
static void zz9k_archive_tar_stream_init(ZZ9KArchiveTarStream *stream,
                                         const char *command,
                                         const char *output_dir,
                                         const char *archive_path)
{
  memset(stream, 0, sizeof(*stream));
  stream->command = command;
  stream->output_dir = output_dir;
  stream->archive_path = archive_path;
  stream->ok = 1;
}

static void zz9k_archive_tar_stream_cleanup(ZZ9KArchiveTarStream *stream)
{
  if (stream && stream->file) {
    fclose(stream->file);
    stream->file = 0;
    /* A file still open at cleanup means the archive ended mid-member:
       the staged temporary is partial, so remove it -- the destination
       was never touched. */
    if (stream->tmp_path) {
      remove(stream->tmp_path);
    }
  }
  if (stream) {
    free(stream->tmp_path);
    free(stream->final_path);
    stream->tmp_path = 0;
    stream->final_path = 0;
    free(stream->pax_data);
    stream->pax_data = 0;
    stream->pax_capacity = 0U;
  }
}

static int zz9k_archive_tar_stream_prepare_pax(
    ZZ9KArchiveTarStream *stream,
    uint32_t size)
{
  uint8_t *bytes;
  uint32_t capacity = size == 0U ? 1U : size;

  if (!stream || size > ZZ9K_ARCHIVE_MAX_PAX_DATA) {
    return 0;
  }
  if (stream->pax_capacity >= capacity) {
    return 1;
  }
  bytes = (uint8_t *)malloc((size_t)capacity);
  if (!bytes) {
    return 0;
  }
  free(stream->pax_data);
  stream->pax_data = bytes;
  stream->pax_capacity = capacity;
  return 1;
}

static int zz9k_archive_tar_stream_close_file(ZZ9KArchiveTarStream *stream)
{
  if (!stream || !stream->file) {
    return 1;
  }
  if (fclose(stream->file) != 0) {
    stream->file = 0;
    if (stream->tmp_path) {
      remove(stream->tmp_path); /* incomplete write: keep the old file */
    }
    stream->ok = 0;
    return 0;
  }
  stream->file = 0;
  if (stream->tmp_path && stream->final_path) {
    /* Member complete: swap the staged temporary in, preserving the old
       destination until the replacement succeeds. */
    if (!zz9k_archive_replace_with_staged(stream->tmp_path,
                                          stream->final_path,
                                          stream->entry.name)) {
      free(stream->tmp_path);
      free(stream->final_path);
      stream->tmp_path = 0;
      stream->final_path = 0;
      stream->ok = 0;
      return 0;
    }
  }
  free(stream->tmp_path);
  free(stream->final_path);
  stream->tmp_path = 0;
  stream->final_path = 0;
  if (!zz9k_archive_last_output_skipped &&
      !zz9k_archive_last_output_dry_run) {
    printf("x %s\n", stream->entry.name);
  }
  return 1;
}

/* Staged output open for the tar stream: identical checks to
   zz9k_archive_open_output_entry, but payload bytes go to a
   collision-probed temporary sibling that replaces the destination only
   when the member completes -- a truncated archive must never leave the
   user's previous file replaced by partial data. */
static int zz9k_archive_tar_stream_open_staged(ZZ9KArchiveTarStream *stream)
{
  char *path;
  ZZ9KArchiveEntry output_entry;

  stream->file = 0;
  stream->tmp_path = 0;
  stream->final_path = 0;
  zz9k_archive_last_output_skipped = 0;
  zz9k_archive_last_output_dry_run = 0;
  if (!zz9k_archive_output_entry(&stream->entry, &output_entry)) {
    stream->file = zz9k_archive_open_discard_file();
    zz9k_archive_last_output_skipped = 1;
    return stream->file != 0;
  }
  if (!zz9k_archive_path_is_safe(output_entry.name)) {
    printf("unsafe path rejected: %s\n", stream->entry.name);
    return 0;
  }
  path = zz9k_archive_join_path(stream->output_dir, output_entry.name);
  if (!path) {
    printf("path allocation failed\n");
    return 0;
  }
  if (zz9k_archive_path_is_dir(path)) {
    printf("output path is a directory: %s\n", path);
    free(path);
    return 0;
  }
  if (zz9k_archive_skip_existing_outputs && zz9k_archive_path_exists(path)) {
    printf("s %s\n", path);
    zz9k_archive_last_output_skipped = 1;
    stream->file = zz9k_archive_open_discard_file();
    free(path);
    return stream->file != 0;
  }
  if (!zz9k_archive_overwrite_outputs && zz9k_archive_path_exists(path)) {
    printf("output exists, use --overwrite: %s\n", path);
    free(path);
    return 0;
  }
  if (zz9k_archive_dry_run_outputs) {
    printf("dry %s\n", output_entry.name);
    zz9k_archive_last_output_dry_run = 1;
    stream->file = zz9k_archive_open_discard_file();
    free(path);
    return stream->file != 0;
  }
  if (!zz9k_archive_ensure_parent_dirs(stream->output_dir,
                                       output_entry.name)) {
    printf("could not create parent directories for %s\n",
           output_entry.name);
    free(path);
    return 0;
  }
  stream->tmp_path = (char *)malloc(strlen(path) + 16U);
  if (!stream->tmp_path) {
    printf("path allocation failed\n");
    free(path);
    return 0;
  }
  if (!zz9k_archive_probe_sibling(stream->tmp_path, path, ".zz9k-tmp",
                                  ".zz9k-t")) {
    printf("output temporary name unavailable: %s\n", output_entry.name);
    free(stream->tmp_path);
    stream->tmp_path = 0;
    free(path);
    return 0;
  }
  stream->file = fopen(stream->tmp_path, "wb");
  if (!stream->file) {
    printf("open output failed: %s\n", path);
    free(stream->tmp_path);
    stream->tmp_path = 0;
    free(path);
    return 0;
  }
  stream->final_path = path;
  return 1;
}

static int zz9k_archive_tar_stream_start_entry(
    ZZ9KArchiveTarStream *stream)
{
  uint32_t size;

  if (!stream || !stream->ok) {
    return 0;
  }
  if (zz9k_archive_tar_header_empty(stream->header)) {
    stream->done = 1;
    stream->header_used = 0U;
    return 1;
  }
  memset(&stream->entry, 0, sizeof(stream->entry));
  if (!zz9k_archive_tar_entry_from_header(
          stream->header, 0U, &stream->entry)) {
    stream->ok = 0;
    return 0;
  }
  if ((stream->entry.flags &
       (ZZ9K_ARCHIVE_TAR_FLAG_GNU_LONG_NAME |
        ZZ9K_ARCHIVE_TAR_FLAG_PAX_HEADER)) == 0U &&
      stream->pending_name_skip) {
    stream->entry.flags |= ZZ9K_ARCHIVE_TAR_FLAG_SKIP;
    stream->pending_name_skip = 0;
    stream->pending_name[0] = '\0';
  } else if ((stream->entry.flags &
       (ZZ9K_ARCHIVE_TAR_FLAG_GNU_LONG_NAME |
        ZZ9K_ARCHIVE_TAR_FLAG_PAX_HEADER)) == 0U &&
      stream->pending_name[0] != '\0') {
    strcpy(stream->entry.name, stream->pending_name);
    stream->pending_name[0] = '\0';
  }
  if ((stream->entry.flags &
       (ZZ9K_ARCHIVE_TAR_FLAG_GNU_LONG_NAME |
        ZZ9K_ARCHIVE_TAR_FLAG_PAX_HEADER)) == 0U &&
      stream->pending_size_valid) {
    stream->entry.compressed_size = stream->pending_size;
    stream->entry.uncompressed_size = stream->pending_size;
    stream->pending_size_valid = 0;
  }
  size = stream->entry.uncompressed_size;
  stream->entry_remaining = size;
  stream->pax_used = 0U;
  stream->padding_remaining = (ZZ9K_ARCHIVE_TAR_BLOCK -
      (size % ZZ9K_ARCHIVE_TAR_BLOCK)) % ZZ9K_ARCHIVE_TAR_BLOCK;
  stream->header_used = 0U;
  if ((stream->entry.flags & ZZ9K_ARCHIVE_TAR_FLAG_PAX_HEADER) != 0U &&
      !zz9k_archive_tar_stream_prepare_pax(stream, size)) {
    stream->ok = 0;
    return 0;
  }
  if ((stream->entry.flags & ZZ9K_ARCHIVE_TAR_FLAG_SKIP) != 0U) {
    return 1;
  }
  if (!zz9k_archive_entry_matches_filter(&stream->entry)) {
    return 1;
  }
  if (!zz9k_archive_path_is_safe(stream->entry.name)) {
    printf("unsafe path rejected: %s\n", stream->entry.name);
    stream->ok = 0;
    return 0;
  }
  stream->count++;

  if (strcmp(stream->command, "l") == 0) {
    zz9k_archive_print_entry(&stream->entry);
    return 1;
  }
  if (strcmp(stream->command, "t") == 0) {
    return 1;
  }
  if (strcmp(stream->command, "x") != 0) {
    stream->ok = 0;
    return 0;
  }

  if (stream->archive_path && !stream->entry.is_dir &&
      zz9k_archive_overwrite_outputs &&
      !zz9k_archive_dry_run_outputs &&
      !zz9k_archive_skip_existing_outputs) {
    /* Same guard as the LHA file engine: a member whose output path IS
       the (still-open) archive would truncate the source mid-read --
       including via the zero-size write below. Refuse by file identity
       before any output is opened. */
    char *out_path = zz9k_archive_join_path(stream->output_dir,
                                            stream->entry.name);

    if (out_path) {
      int alias = zz9k_archive_paths_same_file(stream->archive_path,
                                               out_path);
      free(out_path);
      if (alias) {
        printf("output path is the archive itself, refusing: %s\n",
               stream->entry.name);
        stream->ok = 0;
        return 0;
      }
    }
  }
  if (stream->entry.is_dir || size == 0U) {
    if (!zz9k_archive_write_entry(stream->output_dir, &stream->entry, 0)) {
      stream->ok = 0;
      return 0;
    }
    return 1;
  }
  if (!zz9k_archive_tar_stream_open_staged(stream)) {
    stream->ok = 0;
    return 0;
  }
  return 1;
}

static int zz9k_archive_tar_stream_consume(ZZ9KArchiveTarStream *stream,
                                           const uint8_t *data,
                                           uint32_t length)
{
  uint32_t pos = 0U;

  if (!stream || !data || !stream->ok) {
    return 0;
  }
  while (pos < length) {
    uint32_t part;

    if (stream->done) {
      return 1;
    }
    if (stream->entry_remaining != 0U) {
      part = stream->entry_remaining;
      if (part > length - pos) {
        part = length - pos;
      }
      if ((stream->entry.flags & ZZ9K_ARCHIVE_TAR_FLAG_GNU_LONG_NAME) != 0U) {
        uint32_t used = (uint32_t)strlen(stream->pending_name);
        uint32_t space =
            (uint32_t)sizeof(stream->pending_name) - 1U - used;
        uint32_t copy_len = part;

        if (copy_len > space) {
          /* Overflow only when non-NUL name bytes exceed the buffer: a
             record of exactly capacity bytes holding a 255-char name
             plus its terminator FITS (the in-memory parser accepts it),
             so judge by the first byte that would not fit -- a NUL
             there means the name ended exactly at the limit. */
          uint8_t boundary = space != 0U ? data[pos + space] : data[pos];

          copy_len = space;
          if (boundary != 0U) {
            /* The in-memory walker rejects names this long outright; the
               streaming path must not silently keep a truncated prefix
               that could collide with another member's output path. */
            stream->pending_name_overflow = 1;
          }
        }
        if (copy_len != 0U) {
          memcpy(stream->pending_name + used, data + pos, copy_len);
          stream->pending_name[used + copy_len] = '\0';
        }
      } else if ((stream->entry.flags &
                  ZZ9K_ARCHIVE_TAR_FLAG_PAX_HEADER) != 0U) {
        if (!stream->pax_data ||
            stream->pax_used + part > stream->pax_capacity) {
          stream->ok = 0;
          return 0;
        }
        memcpy(stream->pax_data + stream->pax_used, data + pos, part);
        stream->pax_used += part;
      } else if (stream->file &&
          fwrite(data + pos, 1U, part, stream->file) != part) {
        printf("tar stream write failed: %s\n", stream->entry.name);
        stream->ok = 0;
        return 0;
      }
      stream->entry_remaining -= part;
      pos += part;
      if (stream->entry_remaining == 0U &&
          (stream->entry.flags & ZZ9K_ARCHIVE_TAR_FLAG_GNU_LONG_NAME) != 0U) {
        int skip = 0;

        if (stream->pending_name_overflow) {
          printf("tar long name too long: %s...\n", stream->pending_name);
          stream->ok = 0;
          return 0;
        }
        if (!zz9k_archive_tar_normalize_name(
                stream->pending_name, &skip)) {
          stream->ok = 0;
          return 0;
        }
        stream->pending_name_skip = skip;
      } else if (stream->entry_remaining == 0U &&
          (stream->entry.flags & ZZ9K_ARCHIVE_TAR_FLAG_PAX_HEADER) != 0U) {
        ZZ9KArchiveTarPaxInfo pax;

        if (!zz9k_archive_tar_parse_pax_info(
                stream->pax_data, stream->pax_used, &pax)) {
          stream->ok = 0;
          return 0;
        }
        if (pax.has_path) {
          if (pax.path_skip) {
            stream->pending_name[0] = '\0';
            stream->pending_name_skip = 1;
          } else {
            strcpy(stream->pending_name, pax.path);
            stream->pending_name_skip = 0;
          }
        }
        if (pax.has_size) {
          stream->pending_size = pax.size;
          stream->pending_size_valid = 1;
        }
      }
      if (stream->entry_remaining == 0U &&
          !zz9k_archive_tar_stream_close_file(stream)) {
        return 0;
      }
      continue;
    }
    if (stream->padding_remaining != 0U) {
      part = stream->padding_remaining;
      if (part > length - pos) {
        part = length - pos;
      }
      stream->padding_remaining -= part;
      pos += part;
      continue;
    }

    part = ZZ9K_ARCHIVE_TAR_BLOCK - stream->header_used;
    if (part > length - pos) {
      part = length - pos;
    }
    memcpy(stream->header + stream->header_used, data + pos, part);
    stream->header_used += part;
    pos += part;
    if (stream->header_used == ZZ9K_ARCHIVE_TAR_BLOCK &&
        !zz9k_archive_tar_stream_start_entry(stream)) {
      return 0;
    }
  }
  return 1;
}

static int zz9k_archive_tar_stream_finish(ZZ9KArchiveTarStream *stream)
{
  if (!stream || !stream->ok) {
    return 0;
  }
  if (stream->file || stream->entry_remaining != 0U ||
      stream->padding_remaining != 0U || stream->header_used != 0U) {
    zz9k_archive_tar_stream_cleanup(stream);
    return 0;
  }
  return 1;
}

static int zz9k_archive_tar_stream_chunk(void *user,
                                         const uint8_t *data,
                                         uint32_t length)
{
  return zz9k_archive_tar_stream_consume(
      (ZZ9KArchiveTarStream *)user, data, length);
}

static int zz9k_archive_decompress_stream_to_file(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    uint32_t algorithm,
    const uint8_t *compressed,
    uint32_t compressed_length,
    uint32_t output_limit,
    const char *output_dir,
    const ZZ9KArchiveEntry *entry,
    ZZ9KDecompressResult *final_result)
{
  ZZ9KSharedBuffer input;
  ZZ9KSharedBuffer decoded;
  ZZ9KDecompressStreamBeginDesc begin_desc;
  ZZ9KDecompressStreamReadDesc read_desc;
  ZZ9KDecompressStreamResult stream_result;
  FILE *file = 0;
  uint8_t *chunk = 0;
  uint32_t chunk_capacity = zz9k_archive_stream_chunk;
  uint32_t total_written = 0U;
  uint32_t session = 0U;
  int status;
  int ok = 0;

  memset(&input, 0, sizeof(input));
  memset(&decoded, 0, sizeof(decoded));
  memset(final_result, 0, sizeof(*final_result));

  if (!zz9k_archive_service_supports_decompress_stream(service, algorithm)) {
    printf("%s decompress-stream not advertised by codec service\n",
           zz9k_compression_algorithm_text(algorithm));
    return 0;
  }
  if (compressed_length == 0U || output_limit == 0U || !entry) {
    printf("unsupported empty codec stream job\n");
    return 0;
  }

  status = zz9k_alloc_shared(ctx, compressed_length, 16U, 0U, &input);
  if (status != ZZ9K_STATUS_OK) {
    printf("alloc compressed failed: %s (%d), requested=%lu bytes\n",
           zz9k_status_name(status), status,
           (unsigned long)compressed_length);
    zz9k_archive_print_shared_diag(ctx, "compressed input",
                                   compressed_length);
    goto out;
  }
  while (chunk_capacity >= ZZ9K_ARCHIVE_STREAM_MIN_CHUNK) {
    status = zz9k_alloc_shared(ctx, chunk_capacity, 16U, 0U, &decoded);
    if (status == ZZ9K_STATUS_OK) {
      break;
    }
    if (!zz9k_archive_alloc_shrink_retry(status) ||
        chunk_capacity == ZZ9K_ARCHIVE_STREAM_MIN_CHUNK) {
      printf("alloc stream output failed: %s (%d), requested=%lu bytes\n",
             zz9k_status_name(status), status,
             (unsigned long)chunk_capacity);
      zz9k_archive_print_shared_diag(ctx, "stream output",
                                     chunk_capacity);
      goto out;
    }
    chunk_capacity /= 2U;
  }
  chunk = (uint8_t *)malloc((size_t)chunk_capacity);
  if (!chunk) {
    printf("stream chunk allocation failed\n");
    goto out;
  }
  if (!zz9k_shared_copy_to(&input, 0U, compressed, compressed_length)) {
    printf("compressed copy failed\n");
    goto out;
  }
  if (!zz9k_compression_build_decompress_stream_begin_desc(
          &begin_desc, algorithm, input.handle, 0U, compressed_length,
          output_limit, ZZ9K_DECOMPRESS_FLAG_EXPECT_END)) {
    printf("could not build decompression stream begin descriptor\n");
    goto out;
  }

  status = zz9k_decompress_stream_begin(ctx, &begin_desc, &stream_result);
  if (status != ZZ9K_STATUS_OK) {
    printf("%s stream begin failed: %s (%d), input=%lu limit=%lu\n",
           zz9k_compression_algorithm_text(algorithm),
           zz9k_status_name(status), status,
           (unsigned long)compressed_length,
           (unsigned long)output_limit);
    zz9k_archive_print_shared_diag(ctx, "stream begin failure",
                                   compressed_length);
    goto out;
  }
  session = stream_result.session;
  if (!zz9k_archive_open_output_entry(output_dir, entry, &file)) {
    goto out;
  }

  while (1) {
    if (!zz9k_compression_build_decompress_stream_read_desc(
            &read_desc, session, decoded.handle, 0U, decoded.length, 0U)) {
      printf("could not build decompression stream read descriptor\n");
      goto out;
    }
    memset(&stream_result, 0, sizeof(stream_result));
    status = zz9k_decompress_stream_read(ctx, &read_desc, &stream_result);
    if (status != ZZ9K_STATUS_OK) {
      printf("%s stream read failed: %s (%d), written=%lu\n",
             zz9k_compression_algorithm_text(algorithm),
             zz9k_status_name(status), status,
             (unsigned long)total_written);
      zz9k_archive_print_shared_diag(ctx, "stream read failure",
                                     decoded.length);
      session = 0U;
      goto out;
    }
    if (stream_result.bytes_written > decoded.length ||
        total_written > output_limit ||
        stream_result.bytes_written > output_limit - total_written) {
      printf("%s stream output exceeded limit\n",
             zz9k_compression_algorithm_text(algorithm));
      goto out;
    }
    if (stream_result.bytes_written != 0U) {
      if (!zz9k_shared_copy_from(chunk, &decoded, 0U,
                                 stream_result.bytes_written)) {
        printf("stream copy failed\n");
        goto out;
      }
      if (fwrite(chunk, 1U, stream_result.bytes_written, file) !=
          stream_result.bytes_written) {
        printf("stream output write failed: %s\n", entry->name);
        goto out;
      }
      total_written += stream_result.bytes_written;
    }
    if ((stream_result.flags & ZZ9K_DECOMPRESS_RESULT_STREAM_END) != 0U) {
      final_result->bytes_consumed = stream_result.bytes_consumed;
      final_result->bytes_written = total_written;
      final_result->checksum = stream_result.checksum;
      final_result->algorithm = stream_result.algorithm;
      final_result->flags = stream_result.flags;
      ok = 1;
      break;
    }
    if (stream_result.bytes_written == 0U) {
      printf("%s stream made no output progress\n",
             zz9k_compression_algorithm_text(algorithm));
      goto out;
    }
  }

out:
  if (session != 0U) {
    zz9k_decompress_stream_close(ctx, session, 0U);
  }
  if (file) {
    if (fclose(file) != 0) {
      ok = 0;
    }
  }
  if (ok && !zz9k_archive_last_output_skipped &&
      !zz9k_archive_last_output_dry_run) {
    printf("x %s\n", entry->name);
  }
  if (decoded.handle != 0U && decoded.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, decoded.handle);
  }
  if (input.handle != 0U && input.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, input.handle);
  }
  free(chunk);
  return ok;
}

static int zz9k_archive_copy_feed_input_chunk(
    ZZ9KSharedBuffer *input,
    const uint8_t *prefix,
    uint32_t prefix_length,
    const uint8_t *compressed,
    uint32_t compressed_length,
    uint32_t input_offset,
    uint32_t max_length,
    uint32_t *copied)
{
  uint32_t total_length;
  uint32_t remaining;
  uint32_t written = 0U;

  if (!input || !copied || max_length == 0U ||
      prefix_length > 0xffffffffUL - compressed_length) {
    return 0;
  }
  if ((prefix_length != 0U && !prefix) ||
      (compressed_length != 0U && !compressed)) {
    return 0;
  }

  total_length = prefix_length + compressed_length;
  if (input_offset >= total_length) {
    return 0;
  }
  remaining = total_length - input_offset;
  if (remaining > max_length) {
    remaining = max_length;
  }
  if (remaining > input->length) {
    return 0;
  }

  if (input_offset < prefix_length) {
    uint32_t prefix_remaining = prefix_length - input_offset;
    uint32_t part = prefix_remaining < remaining ?
        prefix_remaining : remaining;

    if (!zz9k_shared_copy_to(input, written,
                             prefix + input_offset, part)) {
      return 0;
    }
    written += part;
    remaining -= part;
  }

  if (remaining != 0U) {
    uint32_t compressed_offset = input_offset + written - prefix_length;

    if (!zz9k_shared_copy_to(input, written,
                             compressed + compressed_offset,
                             remaining)) {
      return 0;
    }
    written += remaining;
  }

  *copied = written;
  return written != 0U;
}

static int zz9k_archive_copy_feed_file_input_chunk(
    ZZ9KSharedBuffer *input,
    FILE *input_file,
    const uint8_t *prefix,
    uint32_t prefix_length,
    uint32_t file_length,
    uint32_t input_offset,
    uint32_t max_length,
    uint32_t *copied)
{
  uint32_t total_length;
  uint32_t remaining;
  uint32_t written = 0U;

  if (!input || !input_file || !copied || max_length == 0U ||
      prefix_length > 0xffffffffUL - file_length) {
    return 0;
  }
  if (prefix_length != 0U && !prefix) {
    return 0;
  }

  total_length = prefix_length + file_length;
  if (input_offset >= total_length) {
    return 0;
  }
  remaining = total_length - input_offset;
  if (remaining > max_length) {
    remaining = max_length;
  }
  if (remaining > input->length) {
    return 0;
  }

  if (input_offset < prefix_length) {
    uint32_t prefix_remaining = prefix_length - input_offset;
    uint32_t part = prefix_remaining < remaining ?
        prefix_remaining : remaining;

    if (!zz9k_shared_copy_to(input, written,
                             prefix + input_offset, part)) {
      return 0;
    }
    written += part;
    remaining -= part;
  }

  if (remaining != 0U) {
    uint8_t *dst = (uint8_t *)(void *)input->data;

    if (fread(dst + written, 1U, (size_t)remaining, input_file) !=
        (size_t)remaining) {
      return 0;
    }
    written += remaining;
  }

  *copied = written;
  return written != 0U;
}

static int zz9k_archive_decompress_feed_stream_parts_to_file(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    uint32_t algorithm,
    const uint8_t *prefix,
    uint32_t prefix_length,
    const uint8_t *compressed,
    uint32_t compressed_length,
    uint32_t output_limit,
    const char *output_dir,
    const ZZ9KArchiveEntry *entry,
    ZZ9KDecompressResult *final_result)
{
  ZZ9KSharedBuffer input;
  ZZ9KSharedBuffer decoded;
  ZZ9KDecompressStreamBeginDesc begin_desc;
  ZZ9KDecompressStreamFeedDesc feed_desc;
  ZZ9KDecompressStreamReadDesc read_desc;
  ZZ9KDecompressStreamResult stream_result;
  FILE *file = 0;
  uint8_t *chunk = 0;
  uint32_t pair_capacity = 0U;
  uint32_t failed_capacity = 0U;
  uint32_t input_offset = 0U;
  uint32_t total_input;
  uint32_t total_written = 0U;
  uint32_t session = 0U;
  int need_input = 1;
  int status;
  int ok = 0;

  memset(&input, 0, sizeof(input));
  memset(&decoded, 0, sizeof(decoded));
  memset(final_result, 0, sizeof(*final_result));

  if (!zz9k_archive_service_supports_decompress_feed(service, algorithm)) {
    printf("%s decompress-feed not advertised by codec service\n",
           zz9k_compression_algorithm_text(algorithm));
    return 0;
  }
  if (prefix_length > 0xffffffffUL - compressed_length) {
    printf("unsupported oversized codec feed stream job\n");
    return 0;
  }
  total_input = prefix_length + compressed_length;
  if (total_input == 0U || output_limit == 0U || !entry ||
      (prefix_length != 0U && !prefix) ||
      (compressed_length != 0U && !compressed)) {
    printf("unsupported empty codec feed stream job\n");
    return 0;
  }

  status = zz9k_archive_alloc_stream_pair(ctx, ZZ9K_ALLOC_HOST_WINDOW,
                                          &input, &decoded,
                                          &pair_capacity, &failed_capacity);
  if (status != ZZ9K_STATUS_OK) {
    printf("alloc stream buffer pair failed: %s (%d), requested=%lu bytes\n",
           zz9k_status_name(status), status,
           (unsigned long)failed_capacity);
    zz9k_archive_print_window_overflow(failed_capacity, status);
    zz9k_archive_print_shared_diag(ctx, "stream buffer pair",
                                   failed_capacity);
    goto out;
  }

  chunk = (uint8_t *)malloc((size_t)pair_capacity);
  if (!chunk) {
    printf("stream chunk allocation failed\n");
    goto out;
  }
  if (!zz9k_compression_build_decompress_stream_begin_desc(
          &begin_desc, algorithm, ZZ9K_INVALID_HANDLE, 0U, 0U,
          output_limit,
          ZZ9K_DECOMPRESS_FLAG_EXPECT_END |
          ZZ9K_DECOMPRESS_FLAG_FEED_INPUT)) {
    printf("could not build decompression feed stream begin descriptor\n");
    goto out;
  }

  status = zz9k_decompress_stream_begin(ctx, &begin_desc, &stream_result);
  if (status != ZZ9K_STATUS_OK) {
    printf("%s feed stream begin failed: %s (%d), input=%lu limit=%lu\n",
           zz9k_compression_algorithm_text(algorithm),
           zz9k_status_name(status), status,
           (unsigned long)total_input,
           (unsigned long)output_limit);
    zz9k_archive_print_shared_diag(ctx, "feed stream begin failure",
                                   total_input);
    goto out;
  }
  session = stream_result.session;
  if (!zz9k_archive_open_output_entry(output_dir, entry, &file)) {
    goto out;
  }

  while (1) {
    if (need_input) {
      uint32_t feed_len;
      uint32_t feed_flags = 0U;

      if (input_offset >= total_input) {
        printf("%s feed stream exhausted input\n",
               zz9k_compression_algorithm_text(algorithm));
        goto out;
      }
      feed_len = total_input - input_offset;
      if (feed_len > input.length) {
        feed_len = input.length;
      }
      if (input_offset + feed_len == total_input) {
        feed_flags |= ZZ9K_DECOMPRESS_STREAM_FEED_EOF;
      }
      if (!zz9k_archive_copy_feed_input_chunk(
              &input, prefix, prefix_length, compressed,
              compressed_length, input_offset, feed_len, &feed_len)) {
        printf("stream input copy failed\n");
        goto out;
      }
      if (!zz9k_compression_build_decompress_stream_feed_desc(
              &feed_desc, session, input.handle, 0U, feed_len,
              feed_flags)) {
        printf("could not build decompression stream feed descriptor\n");
        goto out;
      }
      status = zz9k_decompress_stream_feed(ctx, &feed_desc,
                                           &stream_result);
      if (status != ZZ9K_STATUS_OK) {
        printf("%s feed stream feed failed: %s (%d), input=%lu/%lu\n",
               zz9k_compression_algorithm_text(algorithm),
               zz9k_status_name(status), status,
               (unsigned long)input_offset,
               (unsigned long)total_input);
        zz9k_archive_print_shared_diag(ctx, "feed stream failure",
                                       feed_len);
        session = 0U;
        goto out;
      }
      input_offset += feed_len;
      need_input = 0;
    }

    if (!zz9k_compression_build_decompress_stream_read_desc(
            &read_desc, session, decoded.handle, 0U, decoded.length, 0U)) {
      printf("could not build decompression stream read descriptor\n");
      goto out;
    }
    memset(&stream_result, 0, sizeof(stream_result));
    status = zz9k_decompress_stream_read(ctx, &read_desc, &stream_result);
    if (status != ZZ9K_STATUS_OK) {
      printf("%s feed stream read failed: %s (%d), written=%lu\n",
             zz9k_compression_algorithm_text(algorithm),
             zz9k_status_name(status), status,
             (unsigned long)total_written);
      zz9k_archive_print_shared_diag(ctx, "feed stream read failure",
                                     decoded.length);
      session = 0U;
      goto out;
    }
    if (stream_result.bytes_written > decoded.length ||
        total_written > output_limit ||
        stream_result.bytes_written > output_limit - total_written) {
      printf("%s feed stream output exceeded limit\n",
             zz9k_compression_algorithm_text(algorithm));
      goto out;
    }
    if (stream_result.bytes_written != 0U) {
      if (!zz9k_shared_copy_from(chunk, &decoded, 0U,
                                 stream_result.bytes_written)) {
        printf("feed stream output copy failed\n");
        goto out;
      }
      if (fwrite(chunk, 1U, stream_result.bytes_written, file) !=
          stream_result.bytes_written) {
        printf("feed stream output write failed: %s\n", entry->name);
        goto out;
      }
      total_written += stream_result.bytes_written;
    }
    if ((stream_result.flags & ZZ9K_DECOMPRESS_RESULT_STREAM_END) != 0U) {
      final_result->bytes_consumed = stream_result.bytes_consumed;
      final_result->bytes_written = total_written;
      final_result->checksum = stream_result.checksum;
      final_result->algorithm = stream_result.algorithm;
      final_result->flags = stream_result.flags;
      ok = 1;
      break;
    }
    if ((stream_result.flags & ZZ9K_DECOMPRESS_RESULT_NEED_INPUT) != 0U) {
      need_input = 1;
    } else if (stream_result.bytes_written == 0U) {
      printf("%s feed stream made no output progress\n",
             zz9k_compression_algorithm_text(algorithm));
      goto out;
    }
  }

out:
  if (session != 0U) {
    zz9k_decompress_stream_close(ctx, session, 0U);
  }
  if (file) {
    if (fclose(file) != 0) {
      ok = 0;
    }
  }
  if (ok && !zz9k_archive_last_output_skipped &&
      !zz9k_archive_last_output_dry_run) {
    printf("x %s\n", entry->name);
  }
  if (decoded.handle != 0U && decoded.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, decoded.handle);
  }
  if (input.handle != 0U && input.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, input.handle);
  }
  free(chunk);
  return ok;
}

static int zz9k_archive_decompress_feed_stream_to_file(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    uint32_t algorithm,
    const uint8_t *compressed,
    uint32_t compressed_length,
    uint32_t output_limit,
    const char *output_dir,
    const ZZ9KArchiveEntry *entry,
    ZZ9KDecompressResult *final_result)
{
  return zz9k_archive_decompress_feed_stream_parts_to_file(
      ctx, service, algorithm, 0, 0U, compressed, compressed_length,
      output_limit, output_dir, entry, final_result);
}

static int zz9k_archive_decompress_feed_file_parts_core(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    uint32_t algorithm,
    const uint8_t *prefix,
    uint32_t prefix_length,
    const char *input_path,
    uint32_t input_start,
    uint32_t input_length,
    uint32_t output_limit,
    int write_output,
    const char *output_dir,
    const ZZ9KArchiveEntry *entry,
    ZZ9KArchiveDecodedChunkFn on_chunk,
    void *on_chunk_user,
    ZZ9KDecompressResult *final_result,
    int *failure_status)
{
  ZZ9KSharedBuffer input;
  ZZ9KSharedBuffer decoded;
  ZZ9KDecompressStreamBeginDesc begin_desc;
  ZZ9KDecompressStreamFeedDesc feed_desc;
  ZZ9KDecompressStreamReadDesc read_desc;
  ZZ9KDecompressStreamResult stream_result;
  FILE *input_file = 0;
  FILE *file = 0;
  uint8_t *chunk = 0;
  uint32_t pair_capacity = 0U;
  uint32_t failed_capacity = 0U;
  uint32_t input_offset = 0U;
  uint32_t total_input;
  uint32_t total_written = 0U;
  uint32_t session = 0U;
  int need_input = 1;
  int status;
  int ok = 0;

  if (failure_status) {
    *failure_status = ZZ9K_STATUS_OK;
  }
  if (!final_result) {
    return 0;
  }

  memset(&input, 0, sizeof(input));
  memset(&decoded, 0, sizeof(decoded));
  memset(final_result, 0, sizeof(*final_result));

  if (!zz9k_archive_service_supports_decompress_feed(service, algorithm)) {
    printf("%s decompress-feed not advertised by codec service\n",
           zz9k_compression_algorithm_text(algorithm));
    return 0;
  }
  if (prefix_length > 0xffffffffUL - input_length) {
    printf("unsupported oversized codec feed file job\n");
    return 0;
  }
  total_input = prefix_length + input_length;
  if (!input_path || total_input == 0U || output_limit == 0U ||
      (write_output && (!entry || !output_dir)) ||
      (prefix_length != 0U && !prefix)) {
    printf("unsupported empty codec feed file job\n");
    return 0;
  }
  if (input_start > 0x7fffffffUL) {
    printf("unsupported codec feed file offset\n");
    return 0;
  }

  input_file = fopen(input_path, "rb");
  if (!input_file) {
    printf("open failed: %s\n", input_path);
    return 0;
  }
  if (fseek(input_file, (long)input_start, SEEK_SET) != 0) {
    printf("file stream input seek failed: %s\n", input_path);
    goto out;
  }

  status = zz9k_archive_alloc_stream_pair(
      ctx,
      (write_output || on_chunk) ? ZZ9K_ALLOC_HOST_WINDOW
                                 : ZZ9K_ALLOC_CARD_ONLY,
      &input, &decoded, &pair_capacity, &failed_capacity);
  if (status != ZZ9K_STATUS_OK) {
    if (failure_status) {
      *failure_status = status;
    }
    printf("alloc file stream buffer pair failed: %s (%d), "
           "requested=%lu bytes\n",
           zz9k_status_name(status), status,
           (unsigned long)failed_capacity);
    zz9k_archive_print_window_overflow(failed_capacity, status);
    zz9k_archive_print_shared_diag(ctx, "file stream buffer pair",
                                   failed_capacity);
    goto out;
  }

  if (write_output || on_chunk) {
    chunk = (uint8_t *)malloc((size_t)pair_capacity);
    if (!chunk) {
      printf("file stream chunk allocation failed\n");
      goto out;
    }
  }
  if (!zz9k_compression_build_decompress_stream_begin_desc(
          &begin_desc, algorithm, ZZ9K_INVALID_HANDLE, 0U, 0U,
          output_limit,
          ZZ9K_DECOMPRESS_FLAG_EXPECT_END |
          ZZ9K_DECOMPRESS_FLAG_FEED_INPUT)) {
    printf("could not build decompression feed file begin descriptor\n");
    goto out;
  }

  status = zz9k_decompress_stream_begin(ctx, &begin_desc, &stream_result);
  if (status != ZZ9K_STATUS_OK) {
    if (failure_status) {
      *failure_status = status;
    }
    printf("%s feed file begin failed: %s (%d), input=%lu limit=%lu\n",
           zz9k_compression_algorithm_text(algorithm),
           zz9k_status_name(status), status,
           (unsigned long)total_input,
           (unsigned long)output_limit);
    zz9k_archive_print_shared_diag(ctx, "feed file begin failure",
                                   total_input);
    goto out;
  }
  session = stream_result.session;
  if (write_output &&
      !zz9k_archive_open_output_entry(output_dir, entry, &file)) {
    goto out;
  }

  while (1) {
    if (need_input) {
      uint32_t feed_len;
      uint32_t feed_flags = 0U;

      if (input_offset >= total_input) {
        printf("%s feed file exhausted input\n",
               zz9k_compression_algorithm_text(algorithm));
        goto out;
      }
      feed_len = total_input - input_offset;
      if (feed_len > input.length) {
        feed_len = input.length;
      }
      if (!zz9k_archive_copy_feed_file_input_chunk(
              &input, input_file, prefix, prefix_length, input_length,
              input_offset, feed_len, &feed_len)) {
        printf("file stream input read failed: %s\n", input_path);
        goto out;
      }
      if (input_offset + feed_len == total_input) {
        feed_flags |= ZZ9K_DECOMPRESS_STREAM_FEED_EOF;
      }
      if (!zz9k_compression_build_decompress_stream_feed_desc(
              &feed_desc, session, input.handle, 0U, feed_len,
              feed_flags)) {
        printf("could not build decompression file feed descriptor\n");
        goto out;
      }
      status = zz9k_decompress_stream_feed(ctx, &feed_desc,
                                           &stream_result);
      if (status != ZZ9K_STATUS_OK) {
        if (failure_status) {
          *failure_status = status;
        }
        printf("%s feed file feed failed: %s (%d), input=%lu/%lu\n",
               zz9k_compression_algorithm_text(algorithm),
               zz9k_status_name(status), status,
               (unsigned long)input_offset,
               (unsigned long)total_input);
        zz9k_archive_print_shared_diag(ctx, "feed file failure",
                                       feed_len);
        session = 0U;
        goto out;
      }
      input_offset += feed_len;
      need_input = 0;
    }

    if (!zz9k_compression_build_decompress_stream_read_desc(
            &read_desc, session, decoded.handle, 0U, decoded.length, 0U)) {
      printf("could not build decompression file read descriptor\n");
      goto out;
    }
    memset(&stream_result, 0, sizeof(stream_result));
    status = zz9k_decompress_stream_read(ctx, &read_desc, &stream_result);
    if (status != ZZ9K_STATUS_OK) {
      if (failure_status) {
        *failure_status = status;
      }
      printf("%s feed file read failed: %s (%d), written=%lu\n",
             zz9k_compression_algorithm_text(algorithm),
             zz9k_status_name(status), status,
             (unsigned long)total_written);
      zz9k_archive_print_shared_diag(ctx, "feed file read failure",
                                     decoded.length);
      session = 0U;
      goto out;
    }
    if (stream_result.bytes_written > decoded.length ||
        total_written > output_limit ||
        stream_result.bytes_written > output_limit - total_written) {
      printf("%s feed file output exceeded limit\n",
             zz9k_compression_algorithm_text(algorithm));
      goto out;
    }
    if (stream_result.bytes_written != 0U) {
      if (write_output || on_chunk) {
        if (!zz9k_shared_copy_from(chunk, &decoded, 0U,
                                   stream_result.bytes_written)) {
          printf("feed file output copy failed\n");
          goto out;
        }
      }
      if (write_output) {
        if (fwrite(chunk, 1U, stream_result.bytes_written, file) !=
            stream_result.bytes_written) {
          printf("feed file output write failed: %s\n", entry->name);
          goto out;
        }
      }
      if (on_chunk &&
          !on_chunk(on_chunk_user, chunk, stream_result.bytes_written)) {
        printf("%s feed file output consumer failed\n",
               zz9k_compression_algorithm_text(algorithm));
        goto out;
      }
      total_written += stream_result.bytes_written;
    }
    if ((stream_result.flags & ZZ9K_DECOMPRESS_RESULT_STREAM_END) != 0U) {
      final_result->bytes_consumed = stream_result.bytes_consumed;
      final_result->bytes_written = total_written;
      final_result->checksum = stream_result.checksum;
      final_result->algorithm = stream_result.algorithm;
      final_result->flags = stream_result.flags;
      ok = 1;
      break;
    }
    if ((stream_result.flags & ZZ9K_DECOMPRESS_RESULT_NEED_INPUT) != 0U) {
      need_input = 1;
    } else if (stream_result.bytes_written == 0U) {
      printf("%s feed file made no output progress\n",
             zz9k_compression_algorithm_text(algorithm));
      goto out;
    }
  }

out:
  if (session != 0U) {
    zz9k_decompress_stream_close(ctx, session, 0U);
  }
  if (file) {
    if (fclose(file) != 0) {
      ok = 0;
    }
  }
  if (input_file) {
    fclose(input_file);
  }
  if (ok && write_output && !zz9k_archive_last_output_skipped &&
      !zz9k_archive_last_output_dry_run) {
    printf("x %s\n", entry->name);
  }
  if (decoded.handle != 0U && decoded.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, decoded.handle);
  }
  if (input.handle != 0U && input.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, input.handle);
  }
  free(chunk);
  return ok;
}

static int zz9k_archive_decompress_feed_file_parts_to_file(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    uint32_t algorithm,
    const uint8_t *prefix,
    uint32_t prefix_length,
    const char *input_path,
    uint32_t input_start,
    uint32_t input_length,
    uint32_t output_limit,
    const char *output_dir,
    const ZZ9KArchiveEntry *entry,
    ZZ9KDecompressResult *final_result)
{
  return zz9k_archive_decompress_feed_file_parts_core(
      ctx, service, algorithm, prefix, prefix_length, input_path,
      input_start, input_length, output_limit, 1, output_dir, entry,
      0, 0, final_result, 0);
}

static int zz9k_archive_decompress_feed_file_parts_to_file_status(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    uint32_t algorithm,
    const uint8_t *prefix,
    uint32_t prefix_length,
    const char *input_path,
    uint32_t input_start,
    uint32_t input_length,
    uint32_t output_limit,
    const char *output_dir,
    const ZZ9KArchiveEntry *entry,
    ZZ9KDecompressResult *final_result,
    int *failure_status)
{
  return zz9k_archive_decompress_feed_file_parts_core(
      ctx, service, algorithm, prefix, prefix_length, input_path,
      input_start, input_length, output_limit, 1, output_dir, entry,
      0, 0, final_result, failure_status);
}

static int zz9k_archive_decompress_feed_file_parts_to_result_status(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    uint32_t algorithm,
    const uint8_t *prefix,
    uint32_t prefix_length,
    const char *input_path,
    uint32_t input_start,
    uint32_t input_length,
    uint32_t output_limit,
    ZZ9KDecompressResult *final_result,
    int *failure_status)
{
  return zz9k_archive_decompress_feed_file_parts_core(
      ctx, service, algorithm, prefix, prefix_length, input_path,
      input_start, input_length, output_limit, 0, 0, 0, 0, 0,
      final_result, failure_status);
}

static int zz9k_archive_decompress_feed_file_parts_to_result(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    uint32_t algorithm,
    const uint8_t *prefix,
    uint32_t prefix_length,
    const char *input_path,
    uint32_t input_start,
    uint32_t input_length,
    uint32_t output_limit,
    ZZ9KDecompressResult *final_result)
{
  return zz9k_archive_decompress_feed_file_parts_to_result_status(
      ctx, service, algorithm, prefix, prefix_length, input_path,
      input_start, input_length, output_limit, final_result, 0);
}

static int zz9k_archive_decompress_feed_file_to_callback(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    uint32_t algorithm,
    const char *input_path,
    uint32_t input_length,
    uint32_t output_limit,
    ZZ9KArchiveDecodedChunkFn on_chunk,
    void *on_chunk_user,
    ZZ9KDecompressResult *final_result)
{
  return zz9k_archive_decompress_feed_file_parts_core(
      ctx, service, algorithm, 0, 0U, input_path, 0U, input_length,
      output_limit, 0, 0, 0, on_chunk, on_chunk_user, final_result, 0);
}

static int zz9k_archive_decompress_feed_file_to_file(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    uint32_t algorithm,
    const char *input_path,
    uint32_t input_length,
    uint32_t output_limit,
    const char *output_dir,
    const ZZ9KArchiveEntry *entry,
    ZZ9KDecompressResult *final_result)
{
  return zz9k_archive_decompress_feed_file_parts_to_file(
      ctx, service, algorithm, 0, 0U, input_path, 0U, input_length,
      output_limit, output_dir, entry, final_result);
}

static int zz9k_archive_decompress_feed_file_to_result(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    uint32_t algorithm,
    const char *input_path,
    uint32_t input_length,
    uint32_t output_limit,
    ZZ9KDecompressResult *final_result)
{
  return zz9k_archive_decompress_feed_file_parts_to_result(
      ctx, service, algorithm, 0, 0U, input_path, 0U, input_length,
      output_limit, final_result);
}

static int zz9k_archive_zip_file_can_test_entries(
    const ZZ9KArchiveEntry *entries,
    uint32_t count,
    int *needs_deflate)
{
  uint32_t i;

  if (!entries || count == 0U || !needs_deflate) {
    return 0;
  }
  *needs_deflate = 0;
  for (i = 0U; i < count; i++) {
    if (!zz9k_archive_entry_matches_filter(&entries[i])) {
      continue;
    }
    if (entries[i].is_dir) {
      continue;
    }
    if (entries[i].method == ZZ9K_ARCHIVE_ZIP_METHOD_STORE) {
      continue;
    }
    if (entries[i].method == ZZ9K_ARCHIVE_ZIP_METHOD_DEFLATE) {
      *needs_deflate = 1;
      continue;
    }
    return 0;
  }
  return 1;
}

static uint32_t zz9k_archive_zip_test_output_limit(uint32_t output_size)
{
  return output_size == UINT32_MAX ? output_size : output_size + 1U;
}

static int zz9k_archive_entry_has_crc32(const ZZ9KArchiveEntry *entry)
{
  return entry &&
         (entry->flags & ZZ9K_ARCHIVE_ENTRY_FLAG_CRC32) != 0U;
}

static int zz9k_archive_7z_entry_has_unsupported_split(
    const ZZ9KArchiveEntry *entry)
{
  return entry &&
         (entry->flags &
          ZZ9K_ARCHIVE_ENTRY_FLAG_7Z_UNSUPPORTED_SPLIT) != 0U;
}

static int zz9k_archive_7z_entry_has_split_substream(
    const ZZ9KArchiveEntry *entry)
{
  return zz9k_archive_7z_entry_has_unsupported_split(entry);
}

static void zz9k_archive_print_7z_entry_unsupported(
    const ZZ9KArchiveEntry *entry)
{
  if (!entry) {
    return;
  }
  if (zz9k_archive_7z_entry_has_unsupported_split(entry)) {
    printf("7z non-Copy multi-substream unsupported: %s\n", entry->name);
  } else {
    printf("7z entry unsupported: %s\n", entry->name);
  }
}

static int zz9k_archive_zip_result_matches_entry(
    const ZZ9KArchiveEntry *entry,
    const ZZ9KDecompressResult *result)
{
  if (!entry || !result) {
    return 0;
  }
  return result->bytes_written == entry->uncompressed_size &&
         result->checksum == entry->crc32;
}

static int zz9k_archive_zip_stored_entry_crc_matches(
    const ZZ9KArchiveEntry *entry,
    const uint8_t *data)
{
  if (!entry || (!data && entry->uncompressed_size != 0U) ||
      entry->compressed_size != entry->uncompressed_size) {
    return 0;
  }
  return zz9k_archive_crc32(0U, data, entry->uncompressed_size) ==
         entry->crc32;
}

static int zz9k_archive_7z_copy_entry_crc_matches(
    const ZZ9KArchiveEntry *entry,
    const uint8_t *data)
{
  if (!entry ||
      entry->compressed_size != entry->uncompressed_size ||
      (!data && entry->uncompressed_size != 0U)) {
    return 0;
  }
  if (!zz9k_archive_entry_has_crc32(entry)) {
    return 1;
  }
  return zz9k_archive_crc32(0U, data, entry->uncompressed_size) ==
         entry->crc32;
}

static int zz9k_archive_7z_copy_file_crc_matches(
    const char *archive_path,
    const ZZ9KArchiveEntry *entry,
    uint32_t *actual_crc)
{
  uint32_t crc = 0U;

  if (!archive_path || !entry ||
      entry->compressed_size != entry->uncompressed_size) {
    return 0;
  }
  if (!zz9k_archive_entry_has_crc32(entry)) {
    return 1;
  }
  if (!zz9k_archive_file_range_crc32(
          archive_path, entry->data_offset, entry->uncompressed_size,
          &crc)) {
    return 0;
  }
  if (actual_crc) {
    *actual_crc = crc;
  }
  return crc == entry->crc32;
}

static int zz9k_archive_7z_result_matches_entry(
    const ZZ9KArchiveEntry *entry,
    const ZZ9KDecompressResult *result)
{
  if (!entry || !result ||
      result->bytes_written != entry->uncompressed_size) {
    return 0;
  }
  if (!zz9k_archive_entry_has_crc32(entry)) {
    return 1;
  }
  return result->checksum == entry->crc32;
}

static int zz9k_archive_zip_stored_file_crc_matches(
    const char *archive_path,
    const ZZ9KArchiveEntry *entry)
{
  uint8_t *data = 0;
  int ok;

  if (!archive_path || !entry) {
    return 0;
  }
  if (!zz9k_archive_read_file_range(
          archive_path, entry->data_offset, entry->compressed_size, &data)) {
    return 0;
  }
  ok = zz9k_archive_zip_stored_entry_crc_matches(entry, data);
  free(data);
  return ok;
}

static int zz9k_archive_zip_test_deflate_entry(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    const char *archive_path,
    const ZZ9KArchiveEntry *entry)
{
  uint8_t *compressed = 0;
  ZZ9KDecompressResult result;
  uint32_t output_limit;
  int ok = 0;

  if (!ctx || !service || !archive_path || !entry) {
    return 0;
  }
  output_limit =
      zz9k_archive_zip_test_output_limit(entry->uncompressed_size);
  if (zz9k_archive_service_supports_decompress_feed(
          service, ZZ9K_COMPRESSION_DEFLATE_RAW)) {
    if (!zz9k_archive_decompress_feed_file_parts_to_result(
            ctx, service, ZZ9K_COMPRESSION_DEFLATE_RAW,
            0, 0U, archive_path, entry->data_offset,
            entry->compressed_size, output_limit, &result)) {
      printf("zip deflate feed test failed: %s packed=%lu unpacked=%lu "
             "offset=%lu flags=0x%04lx limit=%lu\n",
             entry->name,
             (unsigned long)entry->compressed_size,
             (unsigned long)entry->uncompressed_size,
             (unsigned long)entry->data_offset,
             (unsigned long)entry->flags,
             (unsigned long)output_limit);
      goto out;
    }
    goto check_result;
  }
  if (service &&
      (service->flags & ZZ9K_SERVICE_FLAG_CODEC_DECOMPRESS_FEED) != 0U &&
      (service->flags & ZZ9K_SERVICE_FLAG_CODEC_DEFLATE_FEED) == 0U) {
    printf("zip deflate-feed not advertised; using one-shot test: %s\n",
           entry->name);
  }
  if (!zz9k_archive_read_file_range(
          archive_path, entry->data_offset, entry->compressed_size,
          &compressed)) {
    goto out;
  }
  if (!zz9k_archive_decompress_test_to_result(
          ctx, service, ZZ9K_COMPRESSION_DEFLATE_RAW,
          compressed, entry->compressed_size, output_limit, &result)) {
    printf("zip deflate test failed: %s packed=%lu unpacked=%lu "
           "offset=%lu flags=0x%04lx limit=%lu\n",
           entry->name,
           (unsigned long)entry->compressed_size,
           (unsigned long)entry->uncompressed_size,
           (unsigned long)entry->data_offset,
           (unsigned long)entry->flags,
           (unsigned long)output_limit);
    goto out;
  }
check_result:
  if (result.bytes_consumed != entry->compressed_size) {
    printf("zip entry input mismatch: %s consumed=%lu expected=%lu\n",
           entry->name,
           (unsigned long)result.bytes_consumed,
           (unsigned long)entry->compressed_size);
    goto out;
  }
  if (result.bytes_written != entry->uncompressed_size) {
    printf("zip entry size mismatch: %s decoded=%lu expected=%lu\n",
           entry->name,
           (unsigned long)result.bytes_written,
           (unsigned long)entry->uncompressed_size);
    goto out;
  }
  if (!zz9k_archive_zip_result_matches_entry(entry, &result)) {
    printf("zip entry crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
           entry->name,
           (unsigned long)result.checksum,
           (unsigned long)entry->crc32);
    goto out;
  }
  ok = 1;

out:
  free(compressed);
  return ok;
}

static int zz9k_archive_zip_extract_deflate_entry(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    const char *archive_path,
    const char *output_dir,
    const ZZ9KArchiveEntry *entry)
{
  ZZ9KDecompressResult result;
  uint32_t output_limit;
  int ok = 0;

  if (!ctx || !service || !archive_path || !output_dir || !entry) {
    return 0;
  }
  if (!zz9k_archive_service_supports_decompress_feed(
          service, ZZ9K_COMPRESSION_DEFLATE_RAW)) {
    printf("zip deflate-feed not advertised; cannot stream extract: %s\n",
           entry->name);
    return 0;
  }
  if (entry->compressed_size == 0U && entry->uncompressed_size == 0U) {
    return zz9k_archive_write_entry(output_dir, entry, 0);
  }
  if (entry->uncompressed_size == 0U) {
    output_limit = zz9k_archive_zip_test_output_limit(
        entry->uncompressed_size);
    if (!zz9k_archive_decompress_feed_file_parts_to_result(
            ctx, service, ZZ9K_COMPRESSION_DEFLATE_RAW, 0, 0U,
            archive_path, entry->data_offset, entry->compressed_size,
            output_limit, &result)) {
      printf("zip deflate feed extract failed: %s packed=%lu unpacked=%lu "
             "offset=%lu flags=0x%04lx limit=%lu\n",
             entry->name,
             (unsigned long)entry->compressed_size,
             (unsigned long)entry->uncompressed_size,
             (unsigned long)entry->data_offset,
             (unsigned long)entry->flags,
             (unsigned long)output_limit);
      goto out;
    }
    if (result.bytes_consumed != entry->compressed_size) {
      printf("zip entry input mismatch: %s consumed=%lu expected=%lu\n",
             entry->name,
             (unsigned long)result.bytes_consumed,
             (unsigned long)entry->compressed_size);
      goto out;
    }
    if (result.bytes_written != 0U) {
      printf("zip entry size mismatch: %s decoded=%lu expected=0\n",
             entry->name,
             (unsigned long)result.bytes_written);
      goto out;
    }
    if (!zz9k_archive_zip_result_matches_entry(entry, &result)) {
      printf("zip entry crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
             entry->name,
             (unsigned long)result.checksum,
             (unsigned long)entry->crc32);
      goto out;
    }
    return zz9k_archive_write_entry(output_dir, entry, 0);
  }

  output_limit = entry->uncompressed_size;
  if (!zz9k_archive_decompress_feed_file_parts_to_file(
          ctx, service, ZZ9K_COMPRESSION_DEFLATE_RAW, 0, 0U,
          archive_path, entry->data_offset, entry->compressed_size,
          output_limit, output_dir, entry, &result)) {
    printf("zip deflate feed extract failed: %s packed=%lu unpacked=%lu "
           "offset=%lu flags=0x%04lx limit=%lu\n",
           entry->name,
           (unsigned long)entry->compressed_size,
           (unsigned long)entry->uncompressed_size,
           (unsigned long)entry->data_offset,
           (unsigned long)entry->flags,
           (unsigned long)output_limit);
    goto out;
  }
  if (result.bytes_consumed != entry->compressed_size) {
    printf("zip entry input mismatch: %s consumed=%lu expected=%lu\n",
           entry->name,
           (unsigned long)result.bytes_consumed,
           (unsigned long)entry->compressed_size);
    goto out;
  }
  if (result.bytes_written != entry->uncompressed_size) {
    printf("zip entry size mismatch: %s decoded=%lu expected=%lu\n",
           entry->name,
           (unsigned long)result.bytes_written,
           (unsigned long)entry->uncompressed_size);
    goto out;
  }
  if (!zz9k_archive_zip_result_matches_entry(entry, &result)) {
    printf("zip entry crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
           entry->name,
           (unsigned long)result.checksum,
           (unsigned long)entry->crc32);
    goto out;
  }
  ok = 1;

out:
  return ok;
}

static int zz9k_archive_zip_file_can_extract_entries(
    const ZZ9KArchiveEntry *entries,
    uint32_t count,
    int *needs_deflate)
{
  uint32_t i;

  if (!entries || count == 0U || !needs_deflate) {
    return 0;
  }
  *needs_deflate = 0;
  for (i = 0U; i < count; i++) {
    if (!zz9k_archive_entry_matches_filter(&entries[i])) {
      continue;
    }
    if (entries[i].is_dir) {
      continue;
    }
    if (entries[i].method == ZZ9K_ARCHIVE_ZIP_METHOD_STORE) {
      continue;
    }
    if (entries[i].method == ZZ9K_ARCHIVE_ZIP_METHOD_DEFLATE) {
      *needs_deflate = 1;
      continue;
    }
    return 0;
  }
  return 1;
}

static int zz9k_archive_handle_zip_file(ZZ9KContext **ctx,
                                        ZZ9KServiceInfo *service,
                                        int *codec_ready,
                                        const char *archive_path,
                                        uint32_t archive_length,
                                        const char *command,
                                        const char *output_dir,
                                        int *attempted)
{
  uint8_t *directory = 0;
  ZZ9KArchiveEntry *entries = 0;
  uint32_t directory_length = 0U;
  uint32_t count = 0U;
  uint32_t i;
  int is_list;
  int is_test;
  int is_extract;
  int needs_deflate = 0;
  int ok = 1;

  if (!attempted) {
    return 0;
  }
  *attempted = 0;
  is_list = command && strcmp(command, "l") == 0;
  is_test = command && strcmp(command, "t") == 0;
  is_extract = command && strcmp(command, "x") == 0;
  if (!archive_path || (!is_list && !is_test && !is_extract)) {
    return 0;
  }
  if (!zz9k_archive_zip_read_directory_from_file(
          archive_path, archive_length, &directory,
          &directory_length, &count) ||
      !zz9k_archive_alloc_entries(count, &entries)) {
    goto out;
  }
  if (!zz9k_archive_zip_list_from_directory(
          archive_path, directory, directory_length, archive_length,
          entries, count, count, &count)) {
    goto out;
  }

  if (is_list) {
    *attempted = 1;
    for (i = 0U; i < count; i++) {
      if (!zz9k_archive_entry_matches_filter(&entries[i])) {
        continue;
      }
      zz9k_archive_print_entry(&entries[i]);
    }
    goto out;
  }
  if (is_test) {
    if (!zz9k_archive_zip_file_can_test_entries(
            entries, count, &needs_deflate)) {
      goto out;
    }
    if (needs_deflate &&
        (!ctx || !service || !codec_ready ||
         !zz9k_archive_ensure_codec_open(ctx, service, codec_ready) ||
         !zz9k_archive_service_supports_decompress_test(
             service, ZZ9K_COMPRESSION_DEFLATE_RAW))) {
      goto out;
    }
  } else if (is_extract) {
    if (!zz9k_archive_zip_file_can_extract_entries(
            entries, count, &needs_deflate)) {
      goto out;
    }
    if (needs_deflate) {
      if (!ctx || !service || !codec_ready ||
          !zz9k_archive_ensure_codec_open(ctx, service, codec_ready)) {
        goto out;
      }
      if (!zz9k_archive_service_supports_decompress_feed(
              service, ZZ9K_COMPRESSION_DEFLATE_RAW)) {
        printf("zip deflate-feed not advertised; using legacy extract path\n");
        goto out;
      }
    }
  }

  *attempted = 1;
  for (i = 0U; i < count; i++) {
    ZZ9KArchiveEntry *entry = &entries[i];

    if (!zz9k_archive_entry_matches_filter(entry)) {
      continue;
    }
    if (!zz9k_archive_path_is_safe(entry->name)) {
      printf("unsafe path rejected: %s\n", entry->name);
      ok = 0;
      continue;
    }
    if ((entry->flags & 1U) != 0U) {
      printf("encrypted zip entry unsupported: %s\n", entry->name);
      ok = 0;
      continue;
    }
    if (entry->is_dir) {
      if (is_extract) {
        ok &= zz9k_archive_write_entry(output_dir, entry, 0);
      }
      continue;
    }
    if (entry->method == ZZ9K_ARCHIVE_ZIP_METHOD_STORE &&
        entry->compressed_size != entry->uncompressed_size) {
      printf("stored zip entry size mismatch: %s\n", entry->name);
      ok = 0;
      continue;
    }
    if (entry->method == ZZ9K_ARCHIVE_ZIP_METHOD_STORE && is_test &&
        !zz9k_archive_zip_stored_file_crc_matches(archive_path, entry)) {
      printf("stored zip entry crc mismatch: %s\n", entry->name);
      ok = 0;
      continue;
    }
    if (entry->method == ZZ9K_ARCHIVE_ZIP_METHOD_STORE && is_extract) {
      /* One pass: the range copy verifies the CRC inline instead of a
         separate read-first verification round over the network. */
      ok &= zz9k_archive_write_file_range_entry(
          output_dir, entry, archive_path, 1);
    } else if (entry->method == ZZ9K_ARCHIVE_ZIP_METHOD_DEFLATE &&
               is_test) {
      if (!ctx || !*ctx || !service ||
          !zz9k_archive_zip_test_deflate_entry(
              *ctx, service, archive_path, entry)) {
        ok = 0;
      }
    } else if (entry->method == ZZ9K_ARCHIVE_ZIP_METHOD_DEFLATE &&
               is_extract) {
      if (!ctx || !*ctx || !service ||
          !zz9k_archive_zip_extract_deflate_entry(
              *ctx, service, archive_path, output_dir, entry)) {
        ok = 0;
      }
    }
  }
  if (is_test && ok) {
    printf("zip test ok: %lu entries\n", (unsigned long)count);
  }

out:
  free(entries);
  free(directory);
  return ok;
}

static int zz9k_archive_handle_zip(ZZ9KContext *ctx,
                                   const ZZ9KServiceInfo *service,
                                   const uint8_t *data,
                                   uint32_t length,
                                   const char *command,
                                   const char *output_dir)
{
  ZZ9KArchiveEntry *entries;
  uint32_t count;
  uint32_t i;
  int ok = 1;

  if (!zz9k_archive_count_zip_entries(data, length, &count) ||
      !zz9k_archive_alloc_entries(count, &entries)) {
    printf("zip parse failed\n");
    return 0;
  }
  if (!zz9k_archive_zip_list(data, length, entries, count, &count)) {
    printf("zip parse failed\n");
    free(entries);
    return 0;
  }

  for (i = 0U; i < count; i++) {
    ZZ9KArchiveEntry *entry = &entries[i];
    uint8_t *decoded = 0;
    ZZ9KDecompressResult result;

    if (!zz9k_archive_entry_matches_filter(entry)) {
      continue;
    }
    if (strcmp(command, "l") == 0) {
      zz9k_archive_print_entry(entry);
      continue;
    }
    if (!zz9k_archive_path_is_safe(entry->name)) {
      printf("unsafe path rejected: %s\n", entry->name);
      ok = 0;
      continue;
    }
    if ((entry->flags & 1U) != 0U) {
      printf("encrypted zip entry unsupported: %s\n", entry->name);
      ok = 0;
      continue;
    }
    if (entry->is_dir) {
      if (strcmp(command, "x") == 0) {
        ok &= zz9k_archive_write_entry(output_dir, entry, data);
      }
      continue;
    }
    if (entry->method == ZZ9K_ARCHIVE_ZIP_METHOD_STORE) {
      if (entry->compressed_size != entry->uncompressed_size) {
        printf("stored zip entry size mismatch: %s\n", entry->name);
        ok = 0;
        continue;
      }
      if (!zz9k_archive_zip_stored_entry_crc_matches(
              entry, data + entry->data_offset)) {
        printf("stored zip entry crc mismatch: %s\n", entry->name);
        ok = 0;
        continue;
      }
      decoded = (uint8_t *)malloc((size_t)entry->uncompressed_size + 1U);
      if (!decoded) {
        printf("entry allocation failed: %s\n", entry->name);
        ok = 0;
        continue;
      }
      if (entry->uncompressed_size != 0U) {
        memcpy(decoded, data + entry->data_offset, entry->uncompressed_size);
      }
    } else if (entry->method == ZZ9K_ARCHIVE_ZIP_METHOD_DEFLATE) {
      if (!ctx || !service) {
        printf("codec service unavailable\n");
        ok = 0;
        continue;
      }
      if (!zz9k_archive_decompress_to_memory(
              ctx, service, ZZ9K_COMPRESSION_DEFLATE_RAW,
              data + entry->data_offset, entry->compressed_size,
              entry->uncompressed_size, &decoded, &result)) {
        printf("zip deflate legacy decode failed: %s packed=%lu "
               "unpacked=%lu offset=%lu flags=0x%04lx\n",
               entry->name,
               (unsigned long)entry->compressed_size,
               (unsigned long)entry->uncompressed_size,
               (unsigned long)entry->data_offset,
               (unsigned long)entry->flags);
        ok = 0;
        continue;
      }
      if (result.bytes_written != entry->uncompressed_size) {
        printf("zip entry size mismatch: %s\n", entry->name);
        free(decoded);
        ok = 0;
        continue;
      }
      if (!zz9k_archive_zip_result_matches_entry(entry, &result)) {
        printf("zip entry crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
               entry->name,
               (unsigned long)result.checksum,
               (unsigned long)entry->crc32);
        free(decoded);
        ok = 0;
        continue;
      }
    } else {
      printf("zip method unsupported: %lu %s\n",
             (unsigned long)entry->method, entry->name);
      ok = 0;
      continue;
    }

    if (strcmp(command, "x") == 0) {
      ok &= zz9k_archive_write_entry(output_dir, entry, decoded);
    }
    free(decoded);
  }
  if (strcmp(command, "t") == 0 && ok) {
    printf("zip test ok: %lu entries\n", (unsigned long)count);
  }
  free(entries);
  return ok;
}

/* ---- LHA batch decode-offload driver (ZZ9K_OP_DECOMPRESS_BATCH) ---- */

/* Per-entry batch outcome, consumed by the zz9k_archive_handle_lha loop. */
enum {
  ZZ9K_LHA_BATCH_NONE = 0,  /* not batched: existing per-member path */
  ZZ9K_LHA_BATCH_DONE = 1,  /* offloaded + verified (extract: written) */
  ZZ9K_LHA_BATCH_SW = 2,    /* board produced a bad result: software only */
  ZZ9K_LHA_BATCH_FAILED = 3 /* extract drain write failure (reported) */
};

typedef struct ZZ9KLhaBatchChunk {
  uint32_t first;         /* index into the member-index list */
  uint32_t count;
  uint32_t blob_length;
  uint32_t output_length; /* extract only */
} ZZ9KLhaBatchChunk;

#define ZZ9K_BATCH_INPUT_BUDGET_DEFAULT_KB 1024U
#define ZZ9K_BATCH_OUTPUT_BUDGET_DEFAULT_KB 2048U
#define ZZ9K_BATCH_MAX_MEMBERS_DEFAULT 64U
#define ZZ9K_BATCH_MAX_MEMBERS_CAP 256U
#define ZZ9K_BATCH_TEST_UNCOMP_BUDGET_DEFAULT_KB 8192U

static int zz9k_archive_lha_batch_member_eligible(
    const ZZ9KArchiveEntry *entry, uint32_t length)
{
  return !entry->is_dir &&
         zz9k_archive_lha_method_supported(entry->method) &&
         entry->compressed_size != 0U && entry->uncompressed_size != 0U &&
         entry->data_offset <= length &&
         entry->compressed_size <= length - entry->data_offset &&
         zz9k_archive_entry_matches_filter(entry) &&
         zz9k_archive_path_is_safe(entry->name);
}

/* Greedily pack members[first..] into one chunk. Returns the number of
   members packed; 0 means members[first] alone exceeds a budget (the
   caller must advance past it).

   `output_capacity` is a cumulative-uncompressed-size cap applied whenever
   it is nonzero, in ANY mode: EXTRACT passes its output-region capacity
   (the chunk's decoded bytes must fit the arena's output region); TEST
   passes a decode-time budget (output is discarded on the board, but each
   chunk is still one synchronous mailbox op, so cumulative uncompressed
   size bounds firmware decode time per op). 0 means unbounded. */
static uint32_t zz9k_archive_lha_batch_plan_chunk(
    const ZZ9KArchiveEntry *entries, const uint32_t *members,
    uint32_t member_total, uint32_t first, uint32_t mode,
    uint32_t member_cap, uint32_t blob_capacity, uint32_t output_capacity,
    ZZ9KLhaBatchChunk *chunk)
{
  uint32_t blob = 0U;
  uint32_t out = 0U;
  uint32_t n = 0U;

  (void)mode;
  chunk->first = first;
  chunk->count = 0U;
  chunk->blob_length = 0U;
  chunk->output_length = 0U;

  while (first + n < member_total && n < member_cap) {
    const ZZ9KArchiveEntry *entry = &entries[members[first + n]];

    if (entry->compressed_size > blob_capacity - blob) {
      break;
    }
    if (output_capacity != 0U &&
        entry->uncompressed_size > output_capacity - out) {
      break;
    }
    blob += entry->compressed_size;
    out += entry->uncompressed_size;
    n++;
  }
  chunk->count = n;
  chunk->blob_length = blob;
  chunk->output_length = out;
  return n;
}

/* Serialize the header + descriptor table for one chunk into `image`
   (ZZ9K_BATCH_HEADER_SIZE + chunk->count * ZZ9K_BATCH_DESC_SIZE bytes).
   Blob/output offsets are assigned in packing order and must be mirrored
   by the caller when copying compressed bytes / draining output. */
static void zz9k_archive_lha_batch_write_tables(
    const ZZ9KArchiveEntry *entries, const uint32_t *members,
    const ZZ9KLhaBatchChunk *chunk, const ZZ9KBatchLayout *layout,
    uint8_t *image)
{
  uint32_t blob = 0U;
  uint32_t out = 0U;
  uint32_t i;

  zz9k_batch_write_header(image, layout, chunk->count, chunk->blob_length);
  for (i = 0U; i < chunk->count; i++) {
    const ZZ9KArchiveEntry *entry = &entries[members[chunk->first + i]];
    ZZ9KBatchMemberDesc desc;

    desc.algorithm = zz9k_archive_lha_method_to_compression(entry->method);
    desc.src_offset = blob;
    desc.src_length = entry->compressed_size;
    desc.dst_offset = out;
    desc.uncompressed_size = entry->uncompressed_size;
    desc.expected_crc = entry->crc32 & 0xffffUL;
    desc.flags = ((entry->flags & ZZ9K_ARCHIVE_ENTRY_FLAG_CRC32) != 0U)
                     ? ZZ9K_BATCH_MEMBER_FLAG_HAVE_CRC
                     : 0U;
    zz9k_batch_write_desc(
        image + ZZ9K_BATCH_HEADER_SIZE + i * ZZ9K_BATCH_DESC_SIZE, &desc);
    blob += entry->compressed_size;
    out += entry->uncompressed_size;
  }
}

/* Judge one member's batch result: DONE when the board's size and (when
   stored) CRC-16 match; otherwise SW so the member is re-decoded on the
   68k WITHOUT retrying the offload. Updates the offload diagnostics. */
static int zz9k_archive_lha_batch_judge(const ZZ9KArchiveEntry *entry,
                                        const ZZ9KBatchMemberResult *result)
{
  if (result->status != ZZ9K_STATUS_OK) {
    zz9k_lha_diag_sw_codec_fail++;
    return ZZ9K_LHA_BATCH_SW;
  }
  if (result->bytes_written != entry->uncompressed_size ||
      (((entry->flags & ZZ9K_ARCHIVE_ENTRY_FLAG_CRC32) != 0U) &&
       (uint16_t)result->checksum != (uint16_t)entry->crc32)) {
    zz9k_lha_diag_sw_crc_miss++;
    return ZZ9K_LHA_BATCH_SW;
  }
  zz9k_lha_diag_batched++;
  return ZZ9K_LHA_BATCH_DONE;
}

/* Drain one batch-extracted member from the arena to the filesystem,
   preserving skip/dry-run semantics. Skipped/dry-run members do not pay
   the Zorro copy-back.

   A malloc or copy_from failure means NOTHING has been written to `file`
   yet, so it is recoverable: decode the member in software directly into
   the same still-open file rather than hard-failing the whole archive.
   Do NOT re-open the output for the retry -- that would trip the "output
   exists" check on the file this drain already created. An fwrite failure
   is different: bytes may already be partially written, and falling back
   to software there would interleave two decodes into the same file (a
   bug class fixed once before in this codebase), so that stays a hard
   failure with no retry. */
static int zz9k_archive_lha_batch_drain_member(ZZ9KContext *ctx,
                                               const ZZ9KSharedBuffer *arena,
                                               const ZZ9KBatchLayout *layout,
                                               uint32_t dst_offset,
                                               const char *output_dir,
                                               ZZ9KLhaSource *src,
                                               const ZZ9KArchiveEntry *entry)
{
  FILE *file = 0;
  uint8_t *bytes;
  int ok = 1;
  int hard_fail = 0;

  (void)ctx;
  if (!zz9k_archive_open_output_entry(output_dir, entry, &file)) {
    return 0;
  }
  if (!zz9k_archive_last_output_skipped &&
      !zz9k_archive_last_output_dry_run && entry->uncompressed_size != 0U) {
    bytes = (uint8_t *)malloc((size_t)entry->uncompressed_size);
    if (!bytes) {
      ok = zz9k_archive_lha_software_decode_to_file(src, entry, file);
    } else if (!zz9k_shared_copy_from(bytes, arena,
                                      layout->output_offset + dst_offset,
                                      entry->uncompressed_size)) {
      free(bytes);
      ok = zz9k_archive_lha_software_decode_to_file(src, entry, file);
    } else {
      if (fwrite(bytes, 1U, (size_t)entry->uncompressed_size, file) !=
          (size_t)entry->uncompressed_size) {
        ok = 0;
        hard_fail = 1;
      }
      free(bytes);
    }
  }
  if (fclose(file) != 0) {
    ok = 0;
    hard_fail = 1;
  }
  if (ok && !zz9k_archive_last_output_skipped &&
      !zz9k_archive_last_output_dry_run) {
    printf("x %s\n", entry->name);
  }
  if (!ok && hard_fail) {
    printf("lha lh%lu offload write failed: %s\n",
           (unsigned long)entry->method, entry->name);
  }
  return ok;
}

typedef struct ZZ9KLhaBatchHashSlot {
  uint32_t hash;
  uint32_t index;
  uint32_t is_dir;
  char name[ZZ9K_ARCHIVE_MAX_NAME];
} ZZ9KLhaBatchHashSlot;

static unsigned char zz9k_archive_lha_ascii_lower(unsigned char c)
{
  return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

static uint32_t zz9k_archive_lha_name_hash(const char *name)
{
  uint32_t hash = 2166136261UL;
  const char *p;

  for (p = name; *p != '\0'; p++) {
    hash ^= (uint32_t)zz9k_archive_lha_ascii_lower((unsigned char)*p);
    hash *= 16777619UL;
  }
  return hash == 0U ? 1U : hash;
}

static int zz9k_archive_lha_batch_hash_slot_init(
    const ZZ9KArchiveEntry *entry, uint32_t index,
    ZZ9KLhaBatchHashSlot *slot)
{
  ZZ9KArchiveEntry output_entry;
  size_t length;

  memset(slot, 0, sizeof(*slot));
  slot->index = index;
  if (!zz9k_archive_output_entry(entry, &output_entry)) {
    return 0;
  }
  strcpy(slot->name, output_entry.name);
  slot->is_dir = output_entry.is_dir;
  length = strlen(slot->name);
  while (length > 0U && slot->name[length - 1U] == '/') {
    slot->name[--length] = '\0';
  }
  if (slot->name[0] == '\0') {
    return 0;
  }
  slot->hash = zz9k_archive_lha_name_hash(slot->name);
  return 1;
}

/* Case-folded FNV-1a over the entry's POST-TRANSFORM output name (from
   zz9k_archive_output_entry, not the raw archive name: --strip-components
   can make distinct raw names, e.g. a/foo and b/foo, resolve to the same
   output file). Trailing slashes are canonicalized away so file-vs-directory
   conflicts (a vs a/) share the same hash. Returns 0 for entries that produce
   no output at all -- 0 is reserved and never participates in a collision
   run. */
static uint32_t zz9k_archive_lha_output_name_hash(
    const ZZ9KArchiveEntry *entry)
{
  ZZ9KLhaBatchHashSlot slot;

  return zz9k_archive_lha_batch_hash_slot_init(entry, 0U, &slot)
             ? slot.hash
             : 0U;
}

static int zz9k_archive_lha_batch_hash_slot_cmp(const void *a, const void *b)
{
  const ZZ9KLhaBatchHashSlot *sa = (const ZZ9KLhaBatchHashSlot *)a;
  const ZZ9KLhaBatchHashSlot *sb = (const ZZ9KLhaBatchHashSlot *)b;

  if (sa->hash < sb->hash) return -1;
  if (sa->hash > sb->hash) return 1;
  return 0;
}

static int zz9k_archive_lha_batch_name_slot_cmp(const void *a, const void *b)
{
  const ZZ9KLhaBatchHashSlot *sa = (const ZZ9KLhaBatchHashSlot *)a;
  const ZZ9KLhaBatchHashSlot *sb = (const ZZ9KLhaBatchHashSlot *)b;
  const unsigned char *pa = (const unsigned char *)sa->name;
  const unsigned char *pb = (const unsigned char *)sb->name;

  while (*pa != '\0' && *pb != '\0') {
    unsigned char ca = zz9k_archive_lha_ascii_lower(*pa);
    unsigned char cb = zz9k_archive_lha_ascii_lower(*pb);

    if (ca < cb) return -1;
    if (ca > cb) return 1;
    pa++;
    pb++;
  }
  if (*pa == '\0' && *pb != '\0') return -1;
  if (*pa != '\0' && *pb == '\0') return 1;
  if (sa->index < sb->index) return -1;
  if (sa->index > sb->index) return 1;
  return 0;
}

static int zz9k_archive_lha_output_name_is_parent_of(const char *parent,
                                                     const char *child)
{
  size_t i;

  if (!parent || !child || parent[0] == '\0') {
    return 0;
  }
  for (i = 0U; parent[i] != '\0'; i++) {
    if (zz9k_archive_lha_ascii_lower((unsigned char)parent[i]) !=
        zz9k_archive_lha_ascii_lower((unsigned char)child[i])) {
      return 0;
    }
  }
  return child[i] == '/';
}

static int zz9k_archive_lha_output_name_has_prefix(const char *prefix,
                                                   const char *name)
{
  size_t i;

  if (!prefix || !name || prefix[0] == '\0') {
    return 0;
  }
  for (i = 0U; prefix[i] != '\0'; i++) {
    if (zz9k_archive_lha_ascii_lower((unsigned char)prefix[i]) !=
        zz9k_archive_lha_ascii_lower((unsigned char)name[i])) {
      return 0;
    }
  }
  return 1;
}

/* Sort-based replacement for the old O(n^2) path_collides scan: compute every
   entry's post-transform output name exactly once, sort by hash to mark exact
   duplicates, then sort by name to mark file-parent/child conflicts. Batch-
   extract drains members before non-batched entries, so any conflicting output
   path (lha-update-style archives or file/directory conflicts) must stay on
   the sequential per-entry path to preserve archive-order semantics.

   Hash equality is trusted WITHOUT re-checking the strings: this is
   deliberate. A false-positive collision (two different names hashing the
   same) only routes an innocent member to the safe, ordered per-entry path
   instead of the batch path -- it can never cause a real collision to be
   missed. Returns 1 on success, 0 on malloc failure (the caller must treat
   that as "collision info unavailable" and skip batching rather than
   guess). */
static int zz9k_archive_lha_batch_mark_collisions(
    const ZZ9KArchiveEntry *entries, uint32_t count, uint8_t *collides)
{
  ZZ9KLhaBatchHashSlot *slots;
  uint32_t i;

  slots = (ZZ9KLhaBatchHashSlot *)malloc((size_t)count * sizeof(*slots));
  if (!slots) {
    return 0;
  }
  for (i = 0U; i < count; i++) {
    zz9k_archive_lha_batch_hash_slot_init(&entries[i], i, &slots[i]);
  }
  qsort(slots, count, sizeof(*slots), zz9k_archive_lha_batch_hash_slot_cmp);
  i = 0U;
  while (i < count) {
    uint32_t run_end = i + 1U;

    while (run_end < count && slots[run_end].hash == slots[i].hash) {
      run_end++;
    }
    if (slots[i].hash != 0U && run_end - i >= 2U) {
      uint32_t j;
      for (j = i; j < run_end; j++) {
        collides[slots[j].index] = 1U;
      }
    }
    i = run_end;
  }
  qsort(slots, count, sizeof(*slots), zz9k_archive_lha_batch_name_slot_cmp);
  for (i = 0U; i < count; i++) {
    uint32_t j;

    if (slots[i].hash == 0U || slots[i].is_dir) {
      continue;
    }
    for (j = i + 1U; j < count &&
                       zz9k_archive_lha_output_name_has_prefix(
                           slots[i].name, slots[j].name);
         j++) {
      if (zz9k_archive_lha_output_name_is_parent_of(slots[i].name,
                                                    slots[j].name)) {
        collides[slots[i].index] = 1U;
        collides[slots[j].index] = 1U;
      }
    }
  }
  free(slots);
  return 1;
}

/* Batched LHA decode driver. Fills state[] with ZZ9K_LHA_BATCH_* per
   entry; entries left at ZZ9K_LHA_BATCH_NONE take the existing per-member
   offload/software path. Fallback chain: batch -> per-member -> software.
   Never worse than today: any refusal or failure leaves states at NONE. */
static void zz9k_archive_lha_batch_run_src(ZZ9KContext *ctx,
                                       const ZZ9KServiceInfo *service,
                                       ZZ9KLhaSource *src,
                                       const ZZ9KArchiveEntry *entries,
                                       uint32_t count,
                                       const char *command,
                                       const char *output_dir,
                                       uint8_t *state)
{
  ZZ9KBatchLayout layout;
  ZZ9KSharedBuffer arena;
  ZZ9KBoard board;
  uint32_t *members = 0;
  uint8_t *tables = 0;
  uint8_t *results = 0;
  uint8_t *collides = 0;
  uint32_t member_total = 0U;
  uint32_t largest = 0U;
  uint32_t largest_uncomp = 0U;
  uint32_t mode;
  uint32_t member_cap;
  uint32_t blob_capacity;
  uint32_t output_capacity;
  uint32_t chunk_uncomp_cap = 0U;
  uint32_t first;
  uint32_t i;

  memset(&arena, 0, sizeof(arena));

  if (!ctx || !service ||
      (service->flags & ZZ9K_SERVICE_FLAG_CODEC_DECOMPRESS_BATCH) == 0U) {
    return;
  }
  if (strcmp(command, "t") == 0) {
    mode = ZZ9K_BATCH_MODE_TEST;
  } else if (strcmp(command, "x") == 0) {
    mode = ZZ9K_BATCH_MODE_EXTRACT;
  } else {
    return;
  }

  memset(&board, 0, sizeof(board));
  if (zz9k_find_board(&board) == ZZ9K_STATUS_OK &&
      board.zorro_version == 2U) {
    /* Batch arenas are deliberately large and CPU-visible. Keep Z2 on the
       bounded feed-stream/per-member path instead of consuming its compact
       host window or risking an unmappable shared-heap result. */
    return;
  }

  members = (uint32_t *)malloc((size_t)count * sizeof(*members));
  if (!members) {
    return;
  }
  if (mode == ZZ9K_BATCH_MODE_EXTRACT) {
    /* Precompute the whole collision mask in one pass (see
       zz9k_archive_lha_batch_mark_collisions) instead of the old per-member
       O(n) rescan. If we can't get collision info, never guess: skip
       batching entirely rather than risk draining a duplicated output path
       out of archive order. */
    collides = (uint8_t *)calloc(count, 1U);
    if (!collides ||
        !zz9k_archive_lha_batch_mark_collisions(entries, count, collides)) {
      free(collides);
      free(members);
      return;
    }
  }
  for (i = 0U; i < count; i++) {
    const ZZ9KArchiveEntry *entry = &entries[i];
    uint32_t algo;

    if (!zz9k_archive_lha_batch_member_eligible(entry, src->length)) {
      continue;
    }
    algo = zz9k_archive_lha_method_to_compression(entry->method);
    if (algo == 0U || !zz9k_archive_service_supports(service, algo)) {
      continue;
    }
    if (mode == ZZ9K_BATCH_MODE_TEST &&
        entry->uncompressed_size > ZZ9K_BATCH_TEST_MAX_EXPECTED) {
      /* The firmware rejects TEST rows whose expected size exceeds this
         cap (decode-and-discard has no arena region to bound it). Take
         the per-member/software path instead of offloading a row the
         board will refuse. */
      continue;
    }
    if (mode == ZZ9K_BATCH_MODE_EXTRACT && collides[i]) {
      continue; /* duplicated path: keep archive-order semantics */
    }
    members[member_total++] = i;
    if (entry->compressed_size > largest) {
      largest = entry->compressed_size;
    }
    if (entry->uncompressed_size > largest_uncomp) {
      largest_uncomp = entry->uncompressed_size;
    }
  }
  if (member_total == 0U) {
    free(collides);
    free(members);
    return;
  }

  member_cap = zz9k_env_u32("ZZ9K_BATCH_MAX_MEMBERS",
                            ZZ9K_BATCH_MAX_MEMBERS_DEFAULT);
  if (member_cap > ZZ9K_BATCH_MAX_MEMBERS_CAP) {
    member_cap = ZZ9K_BATCH_MAX_MEMBERS_CAP;
  }
  blob_capacity = zz9k_env_u32("ZZ9K_BATCH_INPUT_KB",
                               ZZ9K_BATCH_INPUT_BUDGET_DEFAULT_KB) * 1024U;
  output_capacity =
      (mode == ZZ9K_BATCH_MODE_EXTRACT)
          ? zz9k_env_u32("ZZ9K_BATCH_OUTPUT_KB",
                         ZZ9K_BATCH_OUTPUT_BUDGET_DEFAULT_KB) * 1024U
          : 0U;

  if (mode == ZZ9K_BATCH_MODE_TEST) {
    chunk_uncomp_cap = zz9k_env_u32("ZZ9K_BATCH_TEST_UNCOMP_KB",
                                    ZZ9K_BATCH_TEST_UNCOMP_BUDGET_DEFAULT_KB) *
                       1024U;
    /* Never cap below the largest member: a multi-MB member must still fit
       a (single-member) chunk -- bounding per-op decode time must not push
       big members back to the software path. */
    if (chunk_uncomp_cap < largest_uncomp) {
      chunk_uncomp_cap = largest_uncomp;
    }
  }

  /* `test` streams-and-discards on the board, so a member's only arena
     footprint is its compressed bytes: try to grow the blob region to fit
     the largest member (this is what lets multi-MB members offload at
     all). If that arena does not fit the shared heap, fall back to the
     configured budget and leave oversize members to the per-member path. */
  if (mode == ZZ9K_BATCH_MODE_TEST && largest > blob_capacity) {
    if (zz9k_batch_layout_init(&layout, mode, member_cap, largest, 0U) &&
        zz9k_alloc_shared(ctx, layout.total_size, 16U, 0U, &arena) ==
            ZZ9K_STATUS_OK) {
      blob_capacity = largest;
    } else {
      memset(&arena, 0, sizeof(arena));
    }
  }
  if (arena.handle == 0U || arena.handle == ZZ9K_INVALID_HANDLE) {
    if (!zz9k_batch_layout_init(&layout, mode, member_cap, blob_capacity,
                                output_capacity) ||
        zz9k_alloc_shared(ctx, layout.total_size, 16U, 0U, &arena) !=
            ZZ9K_STATUS_OK) {
      free(collides);
      free(members);
      return;
    }
  }

  tables = (uint8_t *)malloc((size_t)ZZ9K_BATCH_HEADER_SIZE +
                             (size_t)member_cap * ZZ9K_BATCH_DESC_SIZE);
  results =
      (uint8_t *)malloc((size_t)member_cap * ZZ9K_BATCH_RESULT_SIZE);
  if (!tables || !results) {
    goto out;
  }

  first = 0U;
  while (first < member_total) {
    ZZ9KLhaBatchChunk chunk;
    ZZ9KDecompressBatchDesc desc;
    /* Aggregate counts are advisory; the per-member result table is
       authoritative (firmware does not CRC-check). */
    ZZ9KDecompressBatchResult batch_result;
    uint32_t blob;
    uint32_t out_cursor;
    unsigned long chunk_bytes_in;
    int status;

    if (zz9k_archive_lha_batch_plan_chunk(
            entries, members, member_total, first, mode, member_cap,
            blob_capacity,
            mode == ZZ9K_BATCH_MODE_TEST ? chunk_uncomp_cap : output_capacity,
            &chunk) == 0U) {
      first++; /* member fits no chunk -> per-member path */
      continue;
    }

    zz9k_archive_lha_batch_write_tables(entries, members, &chunk, &layout,
                                        tables);
    if (!zz9k_shared_copy_to(&arena, 0U, tables,
                             ZZ9K_BATCH_HEADER_SIZE +
                                 chunk.count * ZZ9K_BATCH_DESC_SIZE)) {
      break;
    }
    blob = 0U;
    chunk_bytes_in = 0UL;
    {
      /* Network-friendly blob fill: when the chunk's members sit close
         enough together in the archive (the common no-filter case --
         they are consecutive, separated only by each other's headers),
         read the whole span in ONE seek+read and slice the members out
         of it, instead of one round trip per member. A sparse --match
         filter makes the span large; those fall back to per-member
         reads rather than dragging unrelated data over the wire. */
      const ZZ9KArchiveEntry *first_entry =
          &entries[members[chunk.first]];
      const ZZ9KArchiveEntry *last_entry =
          &entries[members[chunk.first + chunk.count - 1U]];
      uint32_t span_start = first_entry->data_offset;
      uint32_t span_end =
          last_entry->data_offset + last_entry->compressed_size;
      uint32_t span = span_end - span_start;
      int spanned = 0;
      uint8_t *span_buf = 0;

      if (src->file && span >= chunk.blob_length &&
          span - chunk.blob_length <= 256U * 1024U) {
        span_buf = (uint8_t *)malloc(span);
        if (span_buf &&
            fseek(src->file, (long)span_start, SEEK_SET) == 0 &&
            fread(span_buf, 1U, span, src->file) == span) {
          spanned = 1;
        }
      }
      for (i = 0U; i < chunk.count; i++) {
        const ZZ9KArchiveEntry *entry = &entries[members[chunk.first + i]];
        const uint8_t *member = 0;

        if (spanned) {
          member = span_buf + (entry->data_offset - span_start);
        } else if (!zz9k_archive_lha_src_member(src, entry, &member)) {
          free(span_buf);
          goto out; /* read failure -> per-member for the rest */
        }
        if (!zz9k_shared_copy_to(&arena, layout.blob_offset + blob,
                                 member, entry->compressed_size)) {
          free(span_buf);
          goto out; /* Zorro copy failure -> per-member for the rest */
        }
        blob += entry->compressed_size;
        chunk_bytes_in += (unsigned long)entry->compressed_size;
      }
      free(span_buf);
    }

    desc.arena_handle = arena.handle;
    desc.arena_offset = 0U;
    desc.arena_length = layout.total_size;
    status = zz9k_decompress_batch(ctx, &desc, &batch_result);
    if (status != ZZ9K_STATUS_OK) {
      /* Old firmware (UNSUPPORTED) or a transport/validation failure:
         everything not yet judged stays NONE -> per-member fallback. Do not
         count these bytes: they will be retried (and counted) on the
         per-member path. */
      printf("lha batch decode failed: %s (%d)\n", zz9k_status_name(status),
             status);
      zz9k_archive_note_status(status);
      if (zz9k_archive_cancelled()) {
        /* The armed Wait consumed SIGBREAKF_CTRL_C: without latching it
           here, the per-member fallback would happily keep decoding on
           a working board to the end of the archive. */
        return;
      }
      break;
    }
    zz9k_lha_diag_chunks++;
    zz9k_lha_diag_bytes_in += chunk_bytes_in;

    if (!zz9k_shared_copy_from(results, &arena, layout.result_offset,
                               chunk.count * ZZ9K_BATCH_RESULT_SIZE)) {
      break;
    }
    out_cursor = 0U;
    for (i = 0U; i < chunk.count; i++) {
      uint32_t entry_index = members[chunk.first + i];
      const ZZ9KArchiveEntry *entry = &entries[entry_index];
      ZZ9KBatchMemberResult member_result;
      int member_state;

      zz9k_batch_read_result(results + i * ZZ9K_BATCH_RESULT_SIZE,
                             &member_result);
      member_state = zz9k_archive_lha_batch_judge(entry, &member_result);
      if (member_state == ZZ9K_LHA_BATCH_DONE &&
          mode == ZZ9K_BATCH_MODE_EXTRACT) {
        if (!zz9k_archive_lha_batch_drain_member(ctx, &arena, &layout,
                                                 out_cursor, output_dir,
                                                 src, entry)) {
          member_state = ZZ9K_LHA_BATCH_FAILED;
        }
      }
      state[entry_index] = (uint8_t)member_state;
      out_cursor += entry->uncompressed_size;
    }

    first += chunk.count;
  }

out:
  free(results);
  free(tables);
  free(members);
  free(collides);
  if (arena.handle != 0U && arena.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, arena.handle);
  }
}

/* True when two paths denote the same existing file. Amiga: SameLock on
   shared locks. Windows: the volume serial and file index of read-only
   handles (st_ino is meaningless there; read-only opens never truncate).
   POSIX: stat device/inode pairs. Both paths exist whenever a collision
   is possible (the output would overwrite an existing file), so the
   probes never create anything. */
static int zz9k_archive_paths_same_file(const char *a, const char *b)
{
  if (!a || !b) {
    return 0;
  }
#if defined(__amigaos__)
  {
    BPTR lock_a = Lock(a, SHARED_LOCK);
    BPTR lock_b = Lock(b, SHARED_LOCK);
    int same = 0;

    if (lock_a && lock_b) {
      same = (SameLock(lock_a, lock_b) == LOCK_SAME);
    }
    if (lock_a) {
      UnLock(lock_a);
    }
    if (lock_b) {
      UnLock(lock_b);
    }
    return same;
  }
#elif defined(_WIN32)
  {
    HANDLE file_a = CreateFileA(a, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    HANDLE file_b = CreateFileA(b, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    int same = 0;

    if (file_a != INVALID_HANDLE_VALUE && file_b != INVALID_HANDLE_VALUE) {
      BY_HANDLE_FILE_INFORMATION info_a;
      BY_HANDLE_FILE_INFORMATION info_b;

      if (GetFileInformationByHandle(file_a, &info_a) &&
          GetFileInformationByHandle(file_b, &info_b)) {
        same = info_a.dwVolumeSerialNumber == info_b.dwVolumeSerialNumber &&
               info_a.nFileIndexHigh == info_b.nFileIndexHigh &&
               info_a.nFileIndexLow == info_b.nFileIndexLow;
      }
    }
    if (file_a != INVALID_HANDLE_VALUE) {
      CloseHandle(file_a);
    }
    if (file_b != INVALID_HANDLE_VALUE) {
      CloseHandle(file_b);
    }
    return same;
  }
#else
  {
    struct stat stat_a;
    struct stat stat_b;

    return stat(a, &stat_a) == 0 && stat(b, &stat_b) == 0 &&
           stat_a.st_dev == stat_b.st_dev && stat_a.st_ino == stat_b.st_ino;
  }
#endif
}

/* File-mode extraction safety: true when a member's output path denotes the
   archive file itself. Outputs are opened with "wb", which would truncate
   the archive before its member bytes are read through src->file -- the
   in-memory engine was immune because the source bytes were already in
   RAM. Compared by file identity, not spelling: absolute, relative and
   "./"-prefixed aliases of the archive must collide too. */
static int zz9k_archive_lha_output_is_archive(const ZZ9KLhaSource *src,
                                              const char *output_dir,
                                              const ZZ9KArchiveEntry *entry)
{
  ZZ9KArchiveEntry output_entry;
  char *output_path;
  int collision;

  if (!src || !src->file || !src->path || !entry || entry->is_dir) {
    return 0;
  }
  if (!zz9k_archive_output_entry(entry, &output_entry)) {
    return 0;
  }
  output_path = zz9k_archive_join_path(output_dir, output_entry.name);
  if (!output_path) {
    return 0;
  }
  collision = zz9k_archive_paths_same_file(src->path, output_path);
  free(output_path);
  return collision;
}

/* Shared LHA command engine: the batch offload pass (t/x) followed by the
   per-entry loop. Both the in-memory and the file-backed handler parse the
   archive into an entry table and a source, then funnel through here. */
static int zz9k_archive_lha_command_loop(ZZ9KContext *ctx,
                                         const ZZ9KServiceInfo *service,
                                         ZZ9KLhaSource *src,
                                         ZZ9KArchiveEntry *entries,
                                         uint32_t count,
                                         const char *command,
                                         const char *output_dir)
{
  uint8_t *batch_state = 0;
  uint32_t i;
  int ok = 1;

  if (src->file && strcmp(command, "x") == 0 &&
      zz9k_archive_overwrite_outputs &&
      !zz9k_archive_dry_run_outputs &&
      !zz9k_archive_skip_existing_outputs) {
    /* Refuse before ANY output is opened (the batch drains members too):
       extracting a member onto the archive itself would truncate the
       source mid-read and destroy it. Only the --overwrite path can
       truncate: --skip-existing returns before opening an existing
       output, --dry-run never opens one, and without --overwrite an
       existing output is refused per member without truncation. */

    for (i = 0U; i < count; i++) {
      if (!zz9k_archive_entry_matches_filter(&entries[i])) {
        continue;
      }
      if (zz9k_archive_lha_output_is_archive(src, output_dir, &entries[i])) {
        printf("output path is the archive itself, refusing: %s\n",
               entries[i].name);
        return 0;
      }
    }
  }

  zz9k_lha_diag_reset();

  if (count != 0U && ctx && service &&
      (strcmp(command, "t") == 0 || strcmp(command, "x") == 0)) {
    batch_state = (uint8_t *)calloc(count, 1U);
    if (batch_state) {
      zz9k_archive_lha_batch_run_src(ctx, service, src, entries, count,
                                     command, output_dir, batch_state);
    }
  }

  for (i = 0U; i < count; i++) {
    ZZ9KArchiveEntry *entry = &entries[i];

    if (!zz9k_archive_entry_matches_filter(entry)) {
      continue;
    }
    if (strcmp(command, "l") == 0) {
      printf("%c %10lu %s\n", entry->is_dir ? 'd' : '-',
             (unsigned long)entry->uncompressed_size, entry->name);
      continue;
    }

    if (zz9k_archive_cancelled()) {
      /* Same checkpoint as the file walker: once Ctrl-C is latched
         (batch decode, an armed wait, or a direct press), the run stops
         -- per-member work must never continue past it. */
      ok = 0;
      break;
    }
    if (!zz9k_archive_path_is_safe(entry->name)) {
      printf("unsafe path rejected: %s\n", entry->name);
      ok = 0;
      continue;
    }
    if (entry->is_dir) {
      if (strcmp(command, "x") == 0) {
        ok &= zz9k_archive_write_entry(output_dir, entry, 0);
      }
      continue;
    }
    if (zz9k_archive_lha_method_supported(entry->method)) {
      if (batch_state && batch_state[i] == ZZ9K_LHA_BATCH_DONE) {
        continue; /* decoded + verified (extract: written) by the batch */
      }
      if (batch_state && batch_state[i] == ZZ9K_LHA_BATCH_FAILED) {
        ok = 0; /* extract drain already reported the failure */
        continue;
      }
      if (batch_state && batch_state[i] == ZZ9K_LHA_BATCH_SW) {
        /* The board already produced a bad result for this member: decode
           in software directly, never retry the offload. */
        if (strcmp(command, "t") == 0) {
          ok &= zz9k_archive_lha_software_decode_to_file(src, entry, 0);
        } else {
          ok &= zz9k_archive_extract_lha_software(src, output_dir, entry);
        }
        continue;
      }
      if (strcmp(command, "t") == 0) {
        ok &= zz9k_archive_lha_decode_method_to_file(ctx, service, src,
                                                     entry, 0);
      } else if (strcmp(command, "x") == 0) {
        ok &= zz9k_archive_extract_lha_lh5(ctx, service, src,
                                           output_dir, entry);
      }
      continue;
    }
    if (entry->method != ZZ9K_ARCHIVE_LHA_METHOD_LH0) {
      printf("lha method unsupported: %s\n", entry->name);
      ok = 0;
      continue;
    }
    if (entry->compressed_size != entry->uncompressed_size ||
        entry->data_offset > src->length ||
        entry->uncompressed_size > src->length - entry->data_offset) {
      printf("lha stored entry size mismatch: %s\n", entry->name);
      ok = 0;
      continue;
    }
    if (strcmp(command, "x") == 0) {
      if (src->file) {
        /* Stored member: stream the range straight from the archive file,
           exactly like the file-backed ZIP store path. */
        ok &= zz9k_archive_write_file_range_entry(output_dir, entry,
                                                  src->path, 0);
      } else {
        ok &= zz9k_archive_write_entry(
            output_dir, entry, src->data + entry->data_offset);
      }
    }
  }
  if (strcmp(command, "t") == 0 && ok) {
    printf("lha test ok: %lu entries\n", (unsigned long)count);
  }
  if (strcmp(command, "t") == 0 || strcmp(command, "x") == 0) {
    zz9k_lha_diag_report();
  }
  free(batch_state);
  return ok;
}

static int zz9k_archive_handle_lha(ZZ9KContext *ctx,
                                   const ZZ9KServiceInfo *service,
                                   const uint8_t *data,
                                   uint32_t length,
                                   const char *command,
                                   const char *output_dir)
{
  ZZ9KArchiveEntry *entries = 0;
  ZZ9KLhaSource src;
  uint32_t count = 0U;
  int ok;

  if (!zz9k_archive_lha_list(data, length, 0, 0U, &count)) {
    printf("lha parse failed\n");
    return 0;
  }
  if (count != 0U) {
    entries = (ZZ9KArchiveEntry *)calloc(count, sizeof(*entries));
    if (!entries) {
      printf("lha entry allocation failed\n");
      return 0;
    }
  }
  if (!zz9k_archive_lha_list(data, length, entries, count, &count)) {
    printf("lha parse failed\n");
    free(entries);
    return 0;
  }

  zz9k_archive_lha_source_init_mem(&src, data, length);
  ok = zz9k_archive_lha_command_loop(ctx, service, &src, entries, count,
                                     command, output_dir);
  free(entries);
  return ok;
}
/* Streaming list callback: prints each member as the single-pass file
   walk parses it, honoring the match filter, so a listing over a slow
   network mount shows progress immediately instead of after the walk. */
static void zz9k_archive_lha_print_entry(const ZZ9KArchiveEntry *entry,
                                         void *user)
{
  (void)user;
  if (zz9k_archive_entry_matches_filter(entry)) {
    printf("%c %10lu %s\n", entry->is_dir ? 'd' : '-',
           (unsigned long)entry->uncompressed_size, entry->name);
  }
}

/* Streaming extract/test callback: mirrors the per-member logic of
   zz9k_archive_lha_command_loop's non-batch path, invoked the moment the
   walk parses each header -- the first file appears after ONE header
   read plus its own data, exactly like the classic lha tool, instead of
   after a full-archive scan. */
typedef struct ZZ9KLhaStreamWork {
  ZZ9KContext *ctx;
  const ZZ9KServiceInfo *service;
  ZZ9KLhaSource *src;
  const char *command;
  const char *output_dir;
  uint32_t done;
  int ok;
} ZZ9KLhaStreamWork;

static void zz9k_archive_lha_work_entry(const ZZ9KArchiveEntry *entry,
                                        void *user)
{
  ZZ9KLhaStreamWork *work = (ZZ9KLhaStreamWork *)user;
  int is_test = strcmp(work->command, "t") == 0;

  work->done++;
  if (!zz9k_archive_entry_matches_filter(entry)) {
    return;
  }
  if (!zz9k_archive_path_is_safe(entry->name)) {
    printf("unsafe path rejected: %s\n", entry->name);
    work->ok = 0;
    return;
  }
  if (entry->is_dir) {
    if (!is_test) {
      work->ok &= zz9k_archive_write_entry(work->output_dir, entry, 0);
    }
    return;
  }
  if (zz9k_archive_overwrite_outputs &&
      !zz9k_archive_dry_run_outputs &&
      !zz9k_archive_skip_existing_outputs &&
      zz9k_archive_lha_output_is_archive(work->src, work->output_dir,
                                         entry)) {
    /* Extracting this member onto the archive itself would truncate the
       source mid-read: refuse it before any output is opened. */
    printf("output path is the archive itself, refusing: %s\n", entry->name);
    work->ok = 0;
    return;
  }
  if (zz9k_archive_lha_method_supported(entry->method)) {
    if (is_test) {
      work->ok &= zz9k_archive_lha_decode_method_to_file(
          work->ctx, work->service, work->src, entry, 0);
    } else {
      work->ok &= zz9k_archive_extract_lha_lh5(
          work->ctx, work->service, work->src, work->output_dir, entry);
    }
    return;
  }
  if (entry->method != ZZ9K_ARCHIVE_LHA_METHOD_LH0) {
    printf("lha method unsupported: %s\n", entry->name);
    work->ok = 0;
    return;
  }
  if (entry->compressed_size != entry->uncompressed_size ||
      entry->data_offset > work->src->length ||
      entry->uncompressed_size > work->src->length - entry->data_offset) {
    printf("lha stored entry size mismatch: %s\n", entry->name);
    work->ok = 0;
    return;
  }
  if (!is_test) {
    work->ok &= zz9k_archive_write_file_range_entry(
        work->output_dir, entry, work->src->path, 0);
  }
}

/* File-backed LHA engine: walks member headers with seeks and reads each
   member's compressed bytes on demand. A large archive never has to fit in
   RAM before the first member is listed, tested or extracted -- the whole
   point of this path (issue #104: a 180 MB archive used to cost a silent
   multi-minute full-file load before the first output line).

   Returns *attempted = 0 only when the file cannot be opened (the caller
   falls back and reports it) or the command is not l/t/x. Once the file
   is open this engine owns the archive: a mid-walk failure is TERMINAL
   with its own diagnostic, never a cue to load the whole archive into
   RAM -- on the memory-constrained machines this path exists for, that
   fallback just masked the real failure under an allocation error. */
static int zz9k_archive_handle_lha_file(ZZ9KContext **ctx,
                                        ZZ9KServiceInfo *service,
                                        int *codec_ready,
                                        const char *archive_path,
                                        uint32_t archive_length,
                                        const char *command,
                                        const char *output_dir,
                                        int *attempted)
{
  ZZ9KArchiveEntry *entries = 0;
  ZZ9KLhaSource src;
  ZZ9KLhaStreamWork work;
  uint32_t count = 0U;
  int is_list;
  int is_test;
  int is_extract;
  int ok;

  if (!attempted) {
    return 0;
  }
  *attempted = 0;
  is_list = command && strcmp(command, "l") == 0;
  is_test = command && strcmp(command, "t") == 0;
  is_extract = command && strcmp(command, "x") == 0;
  if (!archive_path || (!is_list && !is_test && !is_extract)) {
    return 0;
  }
  if (!zz9k_archive_lha_source_open_file(&src, archive_path,
                                         archive_length)) {
    return 0; /* the in-memory fallback reports the open failure */
  }
  *attempted = 1; /* the file engine owns the archive from here on */

  if ((is_test || is_extract) && !*codec_ready) {
    /* The board codec is optional for LHA: on any open or service failure
       decode in software, exactly like the in-memory engine's caller. */
    int status = zz9k_open(ctx);

    zz9k_archive_note_status(status);
    if (status == ZZ9K_STATUS_OK) {
      if (zz9k_archive_require_codec_service(*ctx, service)) {
        *codec_ready = 1;
        (void)zz9k_arm_completion_irq(*ctx);
      } else {
        zz9k_close(*ctx);
        *ctx = 0;
      }
    } else {
      *ctx = 0;
    }
    if (zz9k_archive_cancelled()) {
      /* A Ctrl-C consumed by any codec-open round trip (the armed Wait
         clears SIGBREAKF_CTRL_C, so checkpoints cannot see it) must stop
         the run -- never silently demote the whole archive to a slow
         full software decode the user asked to abort. */
      zz9k_archive_lha_source_close(&src);
      return 0;
    }
  }

  /* Streaming: one pass parses each header and -- for t/x -- decodes and
     writes that member before moving on, exactly like the classic lha
     tool. The first extracted file costs one header read plus its own
     compressed bytes; no archive-wide scan ever precedes output. The
     batch offload stays available to the in-memory engine, where the
     whole archive is already resident and the scan is free. */
  if (is_list) {
    if (!zz9k_archive_lha_list_file(src.file, archive_length, &entries,
                                    &count, zz9k_archive_lha_print_entry,
                                    0)) {
      zz9k_archive_lha_source_close(&src);
      free(entries);
      return 0;
    }
    zz9k_archive_lha_source_close(&src);
    free(entries);
    return 1;
  }

  memset(&work, 0, sizeof(work));
  work.ctx = *codec_ready ? *ctx : 0;
  work.service = *codec_ready ? service : 0;
  work.src = &src;
  work.command = command;
  work.output_dir = output_dir;
  work.ok = 1;
  zz9k_lha_diag_reset();
  if (!zz9k_archive_lha_list_file(src.file, archive_length, &entries,
                                  &count, zz9k_archive_lha_work_entry,
                                  &work)) {
    /* Terminal: the walk has already printed its diagnostic. */
    zz9k_archive_lha_source_close(&src);
    free(entries);
    return 0;
  }
  ok = work.ok;
  if (is_test && ok) {
    printf("lha test ok: %lu entries\n", (unsigned long)count);
  }
  zz9k_lha_diag_report();
  zz9k_archive_lha_source_close(&src);
  free(entries);
  return ok;
}



static int zz9k_archive_handle_gzip(ZZ9KContext *ctx,
                                    const ZZ9KServiceInfo *service,
                                    const uint8_t *data,
                                    uint32_t length,
                                    const char *command,
                                    const char *output_dir)
{
  ZZ9KArchiveGzipInfo info;
  ZZ9KDecompressResult result;
  uint8_t *decoded = 0;
  ZZ9KArchiveFormat inner;
  int ok = 0;

  if (!zz9k_archive_gzip_info(data, length, &info)) {
    printf("gzip parse failed\n");
    return 0;
  }
  if (!ctx || !service) {
    printf("codec service unavailable\n");
    return 0;
  }
  if (!zz9k_archive_decompress_to_memory(
          ctx, service, ZZ9K_COMPRESSION_GZIP,
          data, length, info.uncompressed_size, &decoded, &result)) {
    return 0;
  }
  if (result.bytes_written != info.uncompressed_size) {
    printf("gzip size mismatch\n");
    goto out;
  }
  if (result.checksum != info.crc32) {
    printf("gzip crc mismatch: decoded=0x%08lx expected=0x%08lx\n",
           (unsigned long)result.checksum,
           (unsigned long)info.crc32);
    goto out;
  }

  inner = zz9k_archive_detect_format(decoded, result.bytes_written);
  if (inner == ZZ9K_ARCHIVE_FORMAT_TAR) {
    ok = zz9k_archive_handle_tar(ctx, service, decoded,
                                 result.bytes_written, command, output_dir);
  } else if (strcmp(command, "l") == 0) {
    printf("- %10lu %s\n", (unsigned long)info.uncompressed_size, info.name);
    ok = 1;
  } else if (strcmp(command, "t") == 0) {
    printf("gzip test ok: out=%lu crc32=0x%08lx\n",
           (unsigned long)result.bytes_written,
           (unsigned long)result.checksum);
    ok = 1;
  } else {
    ZZ9KArchiveEntry entry;

    memset(&entry, 0, sizeof(entry));
    strcpy(entry.name, info.name);
    entry.method = ZZ9K_COMPRESSION_GZIP;
    entry.uncompressed_size = result.bytes_written;
    ok = zz9k_archive_write_entry(output_dir, &entry, decoded);
  }

out:
  free(decoded);
  return ok;
}

static int zz9k_archive_handle_tar_gzip_feed(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    const char *archive_path,
    uint32_t file_length,
    const char *command,
    const char *output_dir,
    const ZZ9KArchiveGzipInfo *info)
{
  ZZ9KArchiveTarStream tar_stream;
  ZZ9KDecompressResult result;
  uint32_t output_limit;
  int ok = 0;

  if (!ctx || !service || !archive_path || !command || !info) {
    return 0;
  }
  output_limit = zz9k_archive_zip_test_output_limit(info->uncompressed_size);
  zz9k_archive_tar_stream_init(&tar_stream, command, output_dir,
                               archive_path);
  if (!zz9k_archive_decompress_feed_file_to_callback(
          ctx, service, ZZ9K_COMPRESSION_GZIP, archive_path, file_length,
          output_limit, zz9k_archive_tar_stream_chunk, &tar_stream,
          &result)) {
    printf("tar.gz feed failed: packed=%lu unpacked=%lu limit=%lu\n",
           (unsigned long)info->compressed_size,
           (unsigned long)info->uncompressed_size,
           (unsigned long)output_limit);
    goto out;
  }
  if (!zz9k_archive_tar_stream_finish(&tar_stream)) {
    printf("tar.gz stream parse failed\n");
    goto out;
  }
  if (result.bytes_consumed != info->compressed_size) {
    printf("tar.gz input mismatch: consumed=%lu expected=%lu\n",
           (unsigned long)result.bytes_consumed,
           (unsigned long)info->compressed_size);
    goto out;
  }
  if (result.bytes_written != info->uncompressed_size) {
    printf("tar.gz size mismatch: decoded=%lu expected=%lu\n",
           (unsigned long)result.bytes_written,
           (unsigned long)info->uncompressed_size);
    goto out;
  }
  if (result.checksum != info->crc32) {
    printf("tar.gz crc mismatch: decoded=0x%08lx expected=0x%08lx\n",
           (unsigned long)result.checksum,
           (unsigned long)info->crc32);
    goto out;
  }
  if (strcmp(command, "t") == 0) {
    printf("tar test ok: %lu entries\n", (unsigned long)tar_stream.count);
  }
  ok = 1;

out:
  zz9k_archive_tar_stream_cleanup(&tar_stream);
  return ok;
}

#define ZZ9K_ARCHIVE_TAR_WALK_CHUNK (64U * 1024U)

/* File-backed plain-tar engine: one sequential pass of chunked reads fed
   through the same streaming parser tar.gz uses. No whole-archive RAM
   load, listing and extraction stream as members complete, and the only
   I/O pattern is a long sequential read -- the best a network mount can
   offer. Attempted stays 0 only when the file cannot be opened, letting
   the in-memory fallback report it exactly as before. */
static int zz9k_archive_handle_tar_file(const char *archive_path,
                                        uint32_t archive_length,
                                        const char *command,
                                        const char *output_dir,
                                        int *attempted)
{
  ZZ9KArchiveTarStream stream;
  uint8_t *chunk = 0;
  FILE *file;
  uint32_t remaining;
  int ok = 0;

  if (!attempted) {
    return 0;
  }
  *attempted = 0;
  if (!archive_path || !command ||
      (strcmp(command, "l") != 0 && strcmp(command, "t") != 0 &&
       strcmp(command, "x") != 0)) {
    return 0;
  }
  file = fopen(archive_path, "rb");
  if (!file) {
    return 0; /* the in-memory fallback reports the open failure */
  }
  *attempted = 1;
  /* Init before any allocation that can fail: the out path calls
     tar_stream_cleanup, which must never see an uninitialized stream. */
  zz9k_archive_tar_stream_init(&stream, command, output_dir,
                               archive_path);
  chunk = (uint8_t *)malloc(ZZ9K_ARCHIVE_TAR_WALK_CHUNK);
  if (!chunk) {
    printf("tar stream chunk allocation failed\n");
    goto out;
  }
  remaining = archive_length;
  while (remaining != 0U) {
    uint32_t part = remaining > ZZ9K_ARCHIVE_TAR_WALK_CHUNK ?
        ZZ9K_ARCHIVE_TAR_WALK_CHUNK : remaining;

    if (zz9k_archive_cancelled()) {

      goto out;
    }
    if (fread(chunk, 1U, part, file) != part) {
      printf("read failed: %s\n", archive_path);
      goto out;
    }
    remaining -= part;
    if (!zz9k_archive_tar_stream_consume(&stream, chunk, part)) {
      printf("tar parse failed\n");
      goto out;
    }
  }
  if (!zz9k_archive_tar_stream_finish(&stream)) {
    printf("tar parse failed\n");
    goto out;
  }
  if (strcmp(command, "t") == 0) {
    printf("tar test ok: %lu entries\n", (unsigned long)stream.count);
  }
  ok = 1;

out:
  zz9k_archive_tar_stream_cleanup(&stream);
  free(chunk);
  if (file) {
    fclose(file);
  }
  return ok;
}
static int zz9k_archive_handle_gzip_file(
    ZZ9KContext **ctx,
    ZZ9KServiceInfo *service,
    int *codec_ready,
    const char *archive_path,
    const uint8_t *probe,
    uint32_t probe_length,
    uint32_t file_length,
    const char *command,
    const char *output_dir,
    int *attempted)
{
  ZZ9KArchiveGzipInfo info;
  ZZ9KArchiveEntry entry;
  ZZ9KDecompressResult result;
  uint32_t output_limit;
  int is_list;
  int is_test;
  int is_extract;
  int ok = 0;

  if (!attempted) {
    return 0;
  }
  *attempted = 0;
  is_list = command && strcmp(command, "l") == 0;
  is_test = command && strcmp(command, "t") == 0;
  is_extract = command && strcmp(command, "x") == 0;
  if (!archive_path || !probe || (!is_list && !is_test && !is_extract) ||
      !zz9k_archive_gzip_info_from_file(
          archive_path, probe, probe_length, file_length, &info)) {
    return 0;
  }
  if (zz9k_archive_gzip_is_tar_candidate(archive_path, &info)) {
    if (!ctx || !service || !codec_ready ||
        !zz9k_archive_ensure_codec_open(ctx, service, codec_ready)) {
      return 0;
    }
    if (!zz9k_archive_service_supports_decompress_feed(
            service, ZZ9K_COMPRESSION_GZIP)) {
      if ((service->flags & ZZ9K_SERVICE_FLAG_CODEC_DECOMPRESS_FEED) != 0U &&
          (service->flags & ZZ9K_SERVICE_FLAG_CODEC_GZIP_FEED) == 0U) {
        printf("gzip-feed not advertised; using legacy tar.gz path\n");
      }
      return 0;
    }
    *attempted = 1;
    return zz9k_archive_handle_tar_gzip_feed(
        *ctx, service, archive_path, file_length, command, output_dir, &info);
  }
  if (is_list) {
    *attempted = 1;
    printf("- %10lu %s\n", (unsigned long)info.uncompressed_size, info.name);
    return 1;
  }
  if (!ctx || !service || !codec_ready ||
      !zz9k_archive_ensure_codec_open(ctx, service, codec_ready)) {
    return 0;
  }
  if (!zz9k_archive_service_supports_decompress_feed(
          service, ZZ9K_COMPRESSION_GZIP)) {
    if ((service->flags & ZZ9K_SERVICE_FLAG_CODEC_DECOMPRESS_FEED) != 0U &&
        (service->flags & ZZ9K_SERVICE_FLAG_CODEC_GZIP_FEED) == 0U) {
      printf("gzip-feed not advertised; using one-shot gzip path\n");
    }
    return 0;
  }

  *attempted = 1;
  output_limit = zz9k_archive_zip_test_output_limit(info.uncompressed_size);
  if (is_test) {
    if (!zz9k_archive_decompress_feed_file_to_result(
            *ctx, service, ZZ9K_COMPRESSION_GZIP, archive_path,
            file_length, output_limit, &result)) {
      printf("gzip feed test failed: packed=%lu unpacked=%lu limit=%lu\n",
             (unsigned long)info.compressed_size,
             (unsigned long)info.uncompressed_size,
             (unsigned long)output_limit);
      goto out;
    }
    if (result.bytes_consumed != info.compressed_size) {
      printf("gzip input mismatch: consumed=%lu expected=%lu\n",
             (unsigned long)result.bytes_consumed,
             (unsigned long)info.compressed_size);
      goto out;
    }
    if (result.bytes_written != info.uncompressed_size) {
      printf("gzip size mismatch: decoded=%lu expected=%lu\n",
             (unsigned long)result.bytes_written,
             (unsigned long)info.uncompressed_size);
      goto out;
    }
    if (!zz9k_archive_gzip_result_matches_footer(&info, &result)) {
      printf("gzip crc mismatch: decoded=0x%08lx expected=0x%08lx\n",
             (unsigned long)result.checksum,
             (unsigned long)info.crc32);
      goto out;
    }
    printf("gzip test ok: out=%lu crc32=0x%08lx\n",
           (unsigned long)result.bytes_written,
           (unsigned long)result.checksum);
    ok = 1;
    goto out;
  }

  memset(&entry, 0, sizeof(entry));
  strcpy(entry.name, info.name);
  entry.method = ZZ9K_COMPRESSION_GZIP;
  entry.compressed_size = info.compressed_size;
  entry.uncompressed_size = info.uncompressed_size;
  if (info.uncompressed_size == 0U) {
    if (!zz9k_archive_decompress_feed_file_to_result(
            *ctx, service, ZZ9K_COMPRESSION_GZIP, archive_path,
            file_length, output_limit, &result)) {
      printf("gzip feed extract failed: %s packed=%lu unpacked=%lu "
             "limit=%lu\n",
             entry.name,
             (unsigned long)info.compressed_size,
             (unsigned long)info.uncompressed_size,
             (unsigned long)output_limit);
      goto out;
    }
    if (result.bytes_written != 0U) {
      printf("gzip size mismatch: decoded=%lu expected=0\n",
             (unsigned long)result.bytes_written);
      goto out;
    }
    if (!zz9k_archive_gzip_result_matches_footer(&info, &result)) {
      printf("gzip crc mismatch: decoded=0x%08lx expected=0x%08lx\n",
             (unsigned long)result.checksum,
             (unsigned long)info.crc32);
      goto out;
    }
    ok = zz9k_archive_write_entry(output_dir, &entry, 0);
    goto out;
  }
  if (!zz9k_archive_decompress_feed_file_to_file(
          *ctx, service, ZZ9K_COMPRESSION_GZIP, archive_path, file_length,
          info.uncompressed_size, output_dir, &entry, &result)) {
    printf("gzip feed extract failed: %s packed=%lu unpacked=%lu limit=%lu\n",
           entry.name,
           (unsigned long)info.compressed_size,
           (unsigned long)info.uncompressed_size,
           (unsigned long)info.uncompressed_size);
    goto out;
  }
  if (result.bytes_consumed != info.compressed_size) {
    printf("gzip input mismatch: consumed=%lu expected=%lu\n",
           (unsigned long)result.bytes_consumed,
           (unsigned long)info.compressed_size);
    goto out;
  }
  if (result.bytes_written != info.uncompressed_size) {
    printf("gzip size mismatch: decoded=%lu expected=%lu\n",
           (unsigned long)result.bytes_written,
           (unsigned long)info.uncompressed_size);
    goto out;
  }
  if (!zz9k_archive_gzip_result_matches_footer(&info, &result)) {
    printf("gzip crc mismatch: decoded=0x%08lx expected=0x%08lx\n",
           (unsigned long)result.checksum,
           (unsigned long)info.crc32);
    goto out;
  }
  ok = 1;

out:
  return ok;
}

static int zz9k_archive_handle_lzma(ZZ9KContext *ctx,
                                    const ZZ9KServiceInfo *service,
                                    const uint8_t *data,
                                    uint32_t length,
                                    const char *command,
                                    const char *output_dir,
                                    uint32_t requested_capacity)
{
  ZZ9KArchiveLzmaInfo info;
  ZZ9KDecompressResult result;
  uint8_t *decoded = 0;
  uint32_t output_capacity;
  int ok = 0;

  if (!zz9k_archive_lzma_info(data, length, &info)) {
    printf("lzma-alone parse failed\n");
    return 0;
  }
  if (strcmp(command, "l") == 0) {
    if (info.size_known) {
      printf("- %10lu %s\n", (unsigned long)info.uncompressed_size,
             info.name);
    } else {
      printf("-          ? %s\n", info.name);
    }
    return 1;
  }
  if (!zz9k_archive_lzma_output_capacity(&info, requested_capacity,
                                         &output_capacity)) {
    if (!info.size_known) {
      printf("lzma-alone unknown output size requires --capacity bytes\n");
    } else {
      printf("lzma-alone output capacity too small\n");
    }
    return 0;
  }
  if (!ctx || !service) {
    printf("codec service unavailable\n");
    return 0;
  }
  if (strcmp(command, "t") == 0 &&
      zz9k_archive_service_supports_decompress_test(
          service, ZZ9K_COMPRESSION_LZMA_ALONE)) {
    if (!zz9k_archive_decompress_test_to_result(
            ctx, service, ZZ9K_COMPRESSION_LZMA_ALONE,
            data, length, output_capacity, &result)) {
      return 0;
    }
    if (info.size_known && result.bytes_written != info.uncompressed_size) {
      printf("lzma-alone size mismatch\n");
      return 0;
    }
    printf("lzma-alone test ok: out=%lu crc32=0x%08lx\n",
           (unsigned long)result.bytes_written,
           (unsigned long)result.checksum);
    return 1;
  }
  if (strcmp(command, "x") == 0 &&
      zz9k_archive_service_supports_decompress_feed(
          service, ZZ9K_COMPRESSION_LZMA_ALONE)) {
    ZZ9KArchiveEntry entry;

    memset(&entry, 0, sizeof(entry));
    strcpy(entry.name, info.name);
    entry.method = ZZ9K_COMPRESSION_LZMA_ALONE;
    entry.uncompressed_size = info.size_known ?
        info.uncompressed_size : output_capacity;
    if (!zz9k_archive_decompress_feed_stream_to_file(
            ctx, service, ZZ9K_COMPRESSION_LZMA_ALONE,
            data, length, output_capacity, output_dir, &entry, &result)) {
      return 0;
    }
    if (info.size_known && result.bytes_written != info.uncompressed_size) {
      printf("lzma-alone size mismatch\n");
      return 0;
    }
    return 1;
  }
  if (strcmp(command, "x") == 0 &&
      zz9k_archive_service_supports_decompress_stream(
          service, ZZ9K_COMPRESSION_LZMA_ALONE)) {
    ZZ9KArchiveEntry entry;

    memset(&entry, 0, sizeof(entry));
    strcpy(entry.name, info.name);
    entry.method = ZZ9K_COMPRESSION_LZMA_ALONE;
    entry.uncompressed_size = info.size_known ?
        info.uncompressed_size : output_capacity;
    if (!zz9k_archive_decompress_stream_to_file(
            ctx, service, ZZ9K_COMPRESSION_LZMA_ALONE,
            data, length, output_capacity, output_dir, &entry, &result)) {
      return 0;
    }
    if (info.size_known && result.bytes_written != info.uncompressed_size) {
      printf("lzma-alone size mismatch\n");
      return 0;
    }
    return 1;
  }
  if (!zz9k_archive_decompress_to_memory(
          ctx, service, ZZ9K_COMPRESSION_LZMA_ALONE,
          data, length, output_capacity, &decoded, &result)) {
    return 0;
  }
  if (info.size_known && result.bytes_written != info.uncompressed_size) {
    printf("lzma-alone size mismatch\n");
    goto out;
  }

  if (strcmp(command, "t") == 0) {
    printf("lzma-alone test ok: out=%lu crc32=0x%08lx\n",
           (unsigned long)result.bytes_written,
           (unsigned long)result.checksum);
    ok = 1;
  } else {
    ZZ9KArchiveEntry entry;

    memset(&entry, 0, sizeof(entry));
    strcpy(entry.name, info.name);
    entry.method = ZZ9K_COMPRESSION_LZMA_ALONE;
    entry.uncompressed_size = result.bytes_written;
    ok = zz9k_archive_write_entry(output_dir, &entry, decoded);
  }

out:
  free(decoded);
  return ok;
}

static int zz9k_archive_7z_lzma_alone_header(
    const ZZ9KArchiveEntry *entry,
    uint8_t header[13])
{
  uint32_t i;

  if (!entry || !header ||
      entry->method != ZZ9K_ARCHIVE_7Z_METHOD_LZMA ||
      entry->method_props_size != 5U) {
    return 0;
  }

  memcpy(header, entry->method_props, 5U);
  for (i = 0U; i < 8U; i++) {
    header[5U + i] =
        (uint8_t)(((uint64_t)entry->uncompressed_size >> (i * 8U)) & 0xffU);
  }
  return 1;
}

static int zz9k_archive_7z_build_lzma_alone_payload(
    const ZZ9KArchiveEntry *entry,
    const uint8_t *compressed,
    uint32_t compressed_length,
    uint8_t **payload,
    uint32_t *payload_length)
{
  uint8_t *bytes;

  if (!entry || !compressed || !payload || !payload_length ||
      entry->method != ZZ9K_ARCHIVE_7Z_METHOD_LZMA ||
      entry->method_props_size != 5U ||
      compressed_length != entry->compressed_size ||
      compressed_length > 0x7fffffffUL - 13U) {
    return 0;
  }

  bytes = (uint8_t *)malloc((size_t)compressed_length + 13U);
  if (!bytes) {
    return 0;
  }
  if (!zz9k_archive_7z_lzma_alone_header(entry, bytes)) {
    free(bytes);
    return 0;
  }
  memcpy(bytes + 13U, compressed, compressed_length);
  *payload = bytes;
  *payload_length = compressed_length + 13U;
  return 1;
}

static int zz9k_archive_7z_build_lzma2_payload(
    const ZZ9KArchiveEntry *entry,
    const uint8_t *compressed,
    uint32_t compressed_length,
    uint8_t **payload,
    uint32_t *payload_length)
{
  uint8_t *bytes;

  if (!entry || !compressed || !payload || !payload_length ||
      entry->method != ZZ9K_ARCHIVE_7Z_METHOD_LZMA2 ||
      entry->method_props_size != 1U ||
      compressed_length != entry->compressed_size ||
      compressed_length > 0x7fffffffUL - 1U) {
    return 0;
  }

  bytes = (uint8_t *)malloc((size_t)compressed_length + 1U);
  if (!bytes) {
    return 0;
  }
  bytes[0] = entry->method_props[0];
  memcpy(bytes + 1U, compressed, compressed_length);
  *payload = bytes;
  *payload_length = compressed_length + 1U;
  return 1;
}

static int zz9k_archive_7z_lzma2_feed_output_limit(
    const ZZ9KArchiveEntry *entry,
    uint32_t *output_limit)
{
  if (!entry || !output_limit ||
      entry->method != ZZ9K_ARCHIVE_7Z_METHOD_LZMA2 ||
      entry->uncompressed_size == 0xffffffffUL) {
    return 0;
  }
  *output_limit = entry->uncompressed_size + 1U;
  return *output_limit != 0U;
}

static void zz9k_archive_print_7z_lzma_diag(const ZZ9KArchiveEntry *entry)
{
  uint32_t dict_size;
  uint32_t i;

  if (!entry) {
    return;
  }

  printf("7z LZMA diagnostics: name=%s packed=%lu output=%lu",
         entry->name,
         (unsigned long)entry->compressed_size,
         (unsigned long)entry->uncompressed_size);
  if (zz9k_archive_lzma_props_dict_size(
          entry->method_props, entry->method_props_size, &dict_size)) {
    printf(" dictionary=%lu", (unsigned long)dict_size);
  } else {
    printf(" dictionary=?");
  }
  printf(" props=");
  for (i = 0U; i < entry->method_props_size; i++) {
    printf("%02x", (unsigned int)entry->method_props[i]);
  }
  printf("\n");
}

static void zz9k_archive_print_7z_lzma2_diag(const ZZ9KArchiveEntry *entry)
{
  uint32_t dict_size;

  if (!entry) {
    return;
  }

  printf("7z LZMA2 diagnostics: name=%s packed=%lu output=%lu",
         entry->name,
         (unsigned long)entry->compressed_size,
         (unsigned long)entry->uncompressed_size);
  if (entry->method_props_size == 1U) {
    printf(" prop=0x%02x", (unsigned int)entry->method_props[0]);
    if (zz9k_archive_lzma2_prop_dict_size(
            entry->method_props[0], &dict_size)) {
      if (dict_size == 0xffffffffUL) {
        printf(" LZMA2 dictionary=4GiB");
      } else {
        printf(" LZMA2 dictionary=%lu", (unsigned long)dict_size);
      }
    }
  } else {
    printf(" invalid-props=%lu",
           (unsigned long)entry->method_props_size);
  }
  printf("\n");
}

static int zz9k_archive_7z_lzma2_fallback_file_range(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    const char *archive_path,
    const char *output_dir,
    const ZZ9KArchiveEntry *entry,
    int write_output,
    ZZ9KDecompressResult *result)
{
  uint8_t *packed = 0;
  uint8_t *wrapped = 0;
  uint8_t *decoded = 0;
  uint32_t wrapped_length = 0U;
  int ok = 0;

  if (!ctx || !service || !archive_path || !entry || !result ||
      entry->method != ZZ9K_ARCHIVE_7Z_METHOD_LZMA2) {
    return 0;
  }
  if (write_output && !output_dir) {
    return 0;
  }
  if (!zz9k_archive_read_file_range(
          archive_path, entry->data_offset, entry->compressed_size,
          &packed)) {
    return 0;
  }
  if (!zz9k_archive_7z_build_lzma2_payload(
          entry, packed, entry->compressed_size, &wrapped,
          &wrapped_length)) {
    goto out;
  }
  if (!zz9k_archive_decompress_to_memory(
          ctx, service, ZZ9K_COMPRESSION_LZMA2,
          wrapped, wrapped_length, entry->uncompressed_size,
          &decoded, result)) {
    goto out;
  }
  if (write_output) {
    if (!zz9k_archive_write_entry(output_dir, entry, decoded)) {
      goto out;
    }
  }
  ok = 1;

out:
  free(decoded);
  free(wrapped);
  free(packed);
  return ok;
}

typedef struct ZZ9KArchive7zSplitWriter {
  const char *output_dir;
  const ZZ9KArchiveEntry *entries;
  uint32_t count;
  uint32_t current;
  uint32_t entry_written;
  uint32_t entry_crc;
  uint32_t total_written;
  FILE *file;
  int write_output;
  int entry_started;
} ZZ9KArchive7zSplitWriter;

static int zz9k_archive_7z_same_split_folder(
    const ZZ9KArchiveEntry *a,
    const ZZ9KArchiveEntry *b)
{
  if (!a || !b ||
      !zz9k_archive_7z_entry_has_split_substream(a) ||
      !zz9k_archive_7z_entry_has_split_substream(b) ||
      a->method != b->method ||
      a->data_offset != b->data_offset ||
      a->compressed_size != b->compressed_size ||
      a->method_props_size != b->method_props_size) {
    return 0;
  }
  if (a->method_props_size != 0U &&
      memcmp(a->method_props, b->method_props, a->method_props_size) != 0) {
    return 0;
  }
  return 1;
}

static uint32_t zz9k_archive_7z_split_group_count(
    const ZZ9KArchiveEntry *entries,
    uint32_t count,
    uint32_t first)
{
  uint32_t group_count = 0U;

  if (!entries || first >= count ||
      !zz9k_archive_7z_entry_has_split_substream(&entries[first])) {
    return 0U;
  }
  while (first + group_count < count &&
         zz9k_archive_7z_same_split_folder(
             &entries[first], &entries[first + group_count])) {
    group_count++;
  }
  return group_count;
}

static int zz9k_archive_7z_split_group_has_match(
    const ZZ9KArchiveEntry *entries,
    uint32_t count)
{
  uint32_t i;

  if (!entries) {
    return 0;
  }
  for (i = 0U; i < count; i++) {
    if (zz9k_archive_entry_matches_filter(&entries[i])) {
      return 1;
    }
  }
  return 0;
}

static int zz9k_archive_7z_split_group_is_safe(
    const ZZ9KArchiveEntry *entries,
    uint32_t count)
{
  uint32_t i;

  if (!entries) {
    return 0;
  }
  for (i = 0U; i < count; i++) {
    if (zz9k_archive_entry_matches_filter(&entries[i]) &&
        !zz9k_archive_path_is_safe(entries[i].name)) {
      printf("unsafe path rejected: %s\n", entries[i].name);
      return 0;
    }
  }
  return 1;
}

static int zz9k_archive_7z_split_group_unpacked_size(
    const ZZ9KArchiveEntry *entries,
    uint32_t count,
    uint32_t *unpacked_size)
{
  uint32_t total = 0U;
  uint32_t i;

  if (!entries || !unpacked_size || count == 0U) {
    return 0;
  }
  for (i = 0U; i < count; i++) {
    if (entries[i].decoded_offset != total ||
        entries[i].uncompressed_size > 0x7fffffffUL - total) {
      return 0;
    }
    total += entries[i].uncompressed_size;
  }
  *unpacked_size = total;
  return 1;
}

static void zz9k_archive_7z_split_writer_init(
    ZZ9KArchive7zSplitWriter *writer,
    const char *output_dir,
    const ZZ9KArchiveEntry *entries,
    uint32_t count,
    int write_output)
{
  memset(writer, 0, sizeof(*writer));
  writer->output_dir = output_dir;
  writer->entries = entries;
  writer->count = count;
  writer->write_output = write_output;
}

static int zz9k_archive_7z_split_writer_close_entry(
    ZZ9KArchive7zSplitWriter *writer)
{
  int ok = 1;

  if (!writer) {
    return 0;
  }
  if (writer->file) {
    if (fclose(writer->file) != 0) {
      ok = 0;
    }
    writer->file = 0;
    if (ok && writer->write_output &&
        !zz9k_archive_last_output_skipped &&
        !zz9k_archive_last_output_dry_run) {
      printf("x %s\n", writer->entries[writer->current].name);
    }
  }
  return ok;
}

static void zz9k_archive_7z_split_writer_cleanup(
    ZZ9KArchive7zSplitWriter *writer)
{
  if (writer && writer->file) {
    fclose(writer->file);
    writer->file = 0;
  }
}

static int zz9k_archive_7z_split_writer_start_entry(
    ZZ9KArchive7zSplitWriter *writer)
{
  const ZZ9KArchiveEntry *entry;

  if (!writer || writer->current >= writer->count) {
    return 0;
  }
  if (writer->entry_started) {
    return 1;
  }
  entry = &writer->entries[writer->current];
  writer->entry_written = 0U;
  writer->entry_crc = 0U;
  if (writer->write_output &&
      zz9k_archive_entry_matches_filter(entry) &&
      !zz9k_archive_open_output_entry(
          writer->output_dir, entry, &writer->file)) {
    return 0;
  }
  writer->entry_started = 1;
  return 1;
}

static int zz9k_archive_7z_split_writer_finish_entry(
    ZZ9KArchive7zSplitWriter *writer)
{
  const ZZ9KArchiveEntry *entry;
  int ok = 1;

  if (!writer || writer->current >= writer->count ||
      !writer->entry_started) {
    return 0;
  }
  entry = &writer->entries[writer->current];
  if (zz9k_archive_entry_matches_filter(entry) &&
      zz9k_archive_entry_has_crc32(entry) &&
      writer->entry_crc != entry->crc32) {
    printf("7z split crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
           entry->name,
           (unsigned long)writer->entry_crc,
           (unsigned long)entry->crc32);
    ok = 0;
  }
  if (!zz9k_archive_7z_split_writer_close_entry(writer)) {
    ok = 0;
  }
  writer->current++;
  writer->entry_written = 0U;
  writer->entry_crc = 0U;
  writer->entry_started = 0;
  return ok;
}

static int zz9k_archive_7z_split_writer_advance_empty(
    ZZ9KArchive7zSplitWriter *writer)
{
  while (writer && writer->current < writer->count &&
         writer->entries[writer->current].uncompressed_size == 0U) {
    if (!zz9k_archive_7z_split_writer_start_entry(writer) ||
        !zz9k_archive_7z_split_writer_finish_entry(writer)) {
      return 0;
    }
  }
  return writer != 0;
}

static int zz9k_archive_7z_split_writer_chunk(void *user,
                                              const uint8_t *data,
                                              uint32_t length)
{
  ZZ9KArchive7zSplitWriter *writer =
      (ZZ9KArchive7zSplitWriter *)user;
  uint32_t pos = 0U;

  if (!writer || (!data && length != 0U) ||
      !zz9k_archive_7z_split_writer_advance_empty(writer)) {
    return 0;
  }

  while (pos < length) {
    const ZZ9KArchiveEntry *entry;
    uint32_t remaining;
    uint32_t part;

    if (writer->current >= writer->count ||
        !zz9k_archive_7z_split_writer_start_entry(writer)) {
      printf("7z split output exceeded expected substreams\n");
      return 0;
    }
    entry = &writer->entries[writer->current];
    remaining = entry->uncompressed_size - writer->entry_written;
    part = length - pos;
    if (part > remaining) {
      part = remaining;
    }
    if (part == 0U) {
      if (!zz9k_archive_7z_split_writer_finish_entry(writer)) {
        return 0;
      }
      continue;
    }
    writer->entry_crc = zz9k_archive_crc32(
        writer->entry_crc, data + pos, part);
    if (writer->file &&
        fwrite(data + pos, 1U, part, writer->file) != part) {
      printf("7z split output write failed: %s\n", entry->name);
      return 0;
    }
    writer->entry_written += part;
    writer->total_written += part;
    pos += part;
    if (writer->entry_written == entry->uncompressed_size &&
        !zz9k_archive_7z_split_writer_finish_entry(writer)) {
      return 0;
    }
  }

  return zz9k_archive_7z_split_writer_advance_empty(writer);
}

static int zz9k_archive_7z_split_writer_finish(
    ZZ9KArchive7zSplitWriter *writer)
{
  if (!zz9k_archive_7z_split_writer_advance_empty(writer)) {
    return 0;
  }
  if (writer->current != writer->count) {
    printf("7z split output truncated: %s\n",
           writer->entries[writer->current].name);
    return 0;
  }
  return 1;
}

static int zz9k_archive_handle_7z_split_group_file(
    ZZ9KContext *ctx,
    const ZZ9KServiceInfo *service,
    const char *command,
    const char *archive_path,
    const char *output_dir,
    const ZZ9KArchiveEntry *entries,
    uint32_t count)
{
  ZZ9KArchiveEntry folder_entry;
  ZZ9KArchive7zSplitWriter writer;
  ZZ9KDecompressResult result;
  uint8_t lzma_header[13];
  const uint8_t *prefix = 0;
  uint32_t prefix_length = 0U;
  uint32_t algorithm = 0U;
  uint32_t unpacked_size = 0U;
  uint32_t output_limit = 0U;
  int is_test;
  int is_extract;
  int ok = 0;

  if (!ctx || !service || !command || !archive_path || !entries ||
      count == 0U) {
    return 0;
  }
  is_test = strcmp(command, "t") == 0;
  is_extract = strcmp(command, "x") == 0;
  if (!is_test && !is_extract) {
    return 0;
  }
  if (!zz9k_archive_7z_split_group_has_match(entries, count)) {
    return 1;
  }
  if (!zz9k_archive_7z_split_group_is_safe(entries, count)) {
    return 0;
  }
  if (!zz9k_archive_7z_split_group_unpacked_size(
          entries, count, &unpacked_size)) {
    printf("7z split group not contiguous: %s\n", entries[0].name);
    return 0;
  }

  folder_entry = entries[0];
  folder_entry.decoded_offset = 0U;
  folder_entry.uncompressed_size = unpacked_size;
  folder_entry.flags &= ~ZZ9K_ARCHIVE_ENTRY_FLAG_CRC32;
  folder_entry.crc32 = 0U;

  if (folder_entry.method == ZZ9K_ARCHIVE_7Z_METHOD_DEFLATE) {
    algorithm = ZZ9K_COMPRESSION_DEFLATE_RAW;
    output_limit = is_test ?
        zz9k_archive_zip_test_output_limit(unpacked_size) : unpacked_size;
  } else if (folder_entry.method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA) {
    algorithm = ZZ9K_COMPRESSION_LZMA_ALONE;
    output_limit = unpacked_size;
    if (!zz9k_archive_7z_lzma_alone_header(
            &folder_entry, lzma_header)) {
      zz9k_archive_print_7z_lzma_diag(&folder_entry);
      return 0;
    }
    prefix = lzma_header;
    prefix_length = sizeof(lzma_header);
  } else if (folder_entry.method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA2) {
    algorithm = ZZ9K_COMPRESSION_LZMA2;
    if (!zz9k_archive_7z_lzma2_feed_output_limit(
            &folder_entry, &output_limit)) {
      zz9k_archive_print_7z_lzma2_diag(&folder_entry);
      return 0;
    }
    prefix = folder_entry.method_props;
    prefix_length = folder_entry.method_props_size;
  } else {
    zz9k_archive_print_7z_entry_unsupported(&folder_entry);
    return 0;
  }

  if (unpacked_size == 0U) {
    zz9k_archive_7z_split_writer_init(
        &writer, output_dir, entries, count, is_extract);
    ok = zz9k_archive_7z_split_writer_finish(&writer);
    zz9k_archive_7z_split_writer_cleanup(&writer);
    return ok;
  }
  if (!zz9k_archive_service_supports_decompress_feed(service, algorithm)) {
    printf("7z compressed multi-substream requires decompress-feed: %s\n",
           folder_entry.name);
    return 0;
  }

  zz9k_archive_7z_split_writer_init(
      &writer, output_dir, entries, count, is_extract);
  if (!zz9k_archive_decompress_feed_file_parts_core(
          ctx, service, algorithm, prefix, prefix_length, archive_path,
          folder_entry.data_offset, folder_entry.compressed_size,
          output_limit, 0, 0, 0, zz9k_archive_7z_split_writer_chunk,
          &writer, &result, 0)) {
    printf("7z compressed multi-substream %s failed: %s\n",
           is_test ? "test" : "extract", folder_entry.name);
    goto out;
  }
  if (!zz9k_archive_7z_split_writer_finish(&writer)) {
    goto out;
  }
  if (result.bytes_written != unpacked_size ||
      writer.total_written != unpacked_size) {
    printf("7z compressed multi-substream size mismatch: %s\n",
           folder_entry.name);
    goto out;
  }
  ok = 1;

out:
  zz9k_archive_7z_split_writer_cleanup(&writer);
  return ok;
}

static int zz9k_archive_7z_decode_encoded_header_from_file(
    ZZ9KContext **ctx,
    ZZ9KServiceInfo *service,
    int *codec_ready,
    const uint8_t *encoded_header,
    uint32_t encoded_header_length,
    const char *archive_path,
    uint32_t archive_length,
    uint8_t **decoded_header,
    uint32_t *decoded_length)
{
  ZZ9KArchiveEntry entry;
  ZZ9KDecompressResult result;
  uint8_t *packed = 0;
  uint8_t *wrapped = 0;
  uint8_t *decoded = 0;
  const uint8_t *codec_input = 0;
  uint32_t codec_input_length = 0U;
  uint32_t algorithm = 0U;
  int ok = 0;

  if (!ctx || !service || !codec_ready || !encoded_header ||
      !archive_path || !decoded_header || !decoded_length) {
    return 0;
  }
  *decoded_header = 0;
  *decoded_length = 0U;
  memset(&result, 0, sizeof(result));
  if (!zz9k_archive_7z_encoded_header_entry(
          encoded_header, encoded_header_length, archive_length, &entry)) {
    return 0;
  }
  if (entry.method == ZZ9K_ARCHIVE_7Z_METHOD_COPY) {
    if (entry.compressed_size != entry.uncompressed_size ||
        !zz9k_archive_read_file_range(
            archive_path, entry.data_offset, entry.compressed_size,
            &decoded)) {
      goto out;
    }
    if (zz9k_archive_entry_has_crc32(&entry) &&
        zz9k_archive_crc32(0U, decoded, entry.uncompressed_size) !=
          entry.crc32) {
      printf("7z encoded header crc mismatch: %s\n", archive_path);
      goto out;
    }
    *decoded_header = decoded;
    *decoded_length = entry.uncompressed_size;
    decoded = 0;
    ok = 1;
    goto out;
  }
  if (entry.method == ZZ9K_ARCHIVE_7Z_METHOD_DEFLATE) {
    algorithm = ZZ9K_COMPRESSION_DEFLATE_RAW;
  } else if (entry.method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA) {
    algorithm = ZZ9K_COMPRESSION_LZMA_ALONE;
  } else if (entry.method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA2) {
    algorithm = ZZ9K_COMPRESSION_LZMA2;
  } else {
    printf("unsupported encoded 7z header method: 0x%08lx\n",
           (unsigned long)entry.method);
    goto out;
  }
  if (!zz9k_archive_ensure_codec_open(ctx, service, codec_ready) ||
      !zz9k_archive_read_file_range(
          archive_path, entry.data_offset, entry.compressed_size, &packed)) {
    goto out;
  }
  codec_input = packed;
  codec_input_length = entry.compressed_size;
  if (entry.method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA) {
    if (!zz9k_archive_7z_build_lzma_alone_payload(
            &entry, packed, entry.compressed_size,
            &wrapped, &codec_input_length)) {
      printf("7z encoded header LZMA decode failed: %s\n", archive_path);
      goto out;
    }
    codec_input = wrapped;
  } else if (entry.method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA2) {
    if (!zz9k_archive_7z_build_lzma2_payload(
            &entry, packed, entry.compressed_size,
            &wrapped, &codec_input_length)) {
      printf("7z encoded header LZMA2 decode failed: %s\n", archive_path);
      goto out;
    }
    codec_input = wrapped;
  }
  if (!zz9k_archive_decompress_to_memory(
          *ctx, service, algorithm, codec_input, codec_input_length,
          entry.uncompressed_size, &decoded, &result)) {
    if (entry.method == ZZ9K_ARCHIVE_7Z_METHOD_DEFLATE) {
      printf("7z encoded header Deflate decode failed: %s\n", archive_path);
    } else if (entry.method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA) {
      printf("7z encoded header LZMA decode failed: %s\n", archive_path);
    } else {
      printf("7z encoded header LZMA2 decode failed: %s\n", archive_path);
    }
    goto out;
  }
  if (!zz9k_archive_7z_result_matches_entry(&entry, &result)) {
    printf("7z encoded header crc/size mismatch: %s\n", archive_path);
    goto out;
  }
  *decoded_header = decoded;
  *decoded_length = result.bytes_written;
  decoded = 0;
  ok = 1;

out:
  free(decoded);
  free(wrapped);
  free(packed);
  return ok;
}

static int zz9k_archive_7z_read_header_from_file_with_codec(
    ZZ9KContext **ctx,
    ZZ9KServiceInfo *service,
    int *codec_ready,
    const char *path,
    uint32_t archive_length,
    uint8_t **header_data,
    uint32_t *header_length)
{
  uint8_t *start = 0;
  uint8_t *bytes = 0;
  ZZ9KArchive7zHeader header;
  int ok = 0;

  if (!path || !header_data || !header_length) {
    return 0;
  }
  *header_data = 0;
  *header_length = 0U;
  if (!zz9k_archive_read_file_range(
          path, 0U, ZZ9K_ARCHIVE_7Z_START_HEADER_SIZE, &start)) {
    return 0;
  }
  if (!zz9k_archive_7z_start_header_from_prefix(
          start, ZZ9K_ARCHIVE_7Z_START_HEADER_SIZE,
          archive_length, &header)) {
    goto out;
  }
  if (!zz9k_archive_read_file_range(
          path, header.next_header_offset, header.next_header_size,
          &bytes)) {
    goto out;
  }
  if (zz9k_archive_crc32(0U, bytes, header.next_header_size) !=
      header.next_header_crc) {
    printf("7z header crc mismatch: %s\n", path);
    goto out;
  }
  if (header.next_header_size != 0U &&
      bytes[0] == ZZ9K_ARCHIVE_7Z_ID_ENCODED_HEADER) {
    uint8_t *decoded = 0;
    uint32_t decoded_size = 0U;

    if (!zz9k_archive_7z_decode_encoded_header_from_file(
            ctx, service, codec_ready, bytes, header.next_header_size,
            path, archive_length, &decoded, &decoded_size)) {
      goto out;
    }
    free(bytes);
    bytes = decoded;
    header.next_header_size = decoded_size;
  }
  *header_data = bytes;
  *header_length = header.next_header_size;
  bytes = 0;
  ok = 1;

out:
  free(bytes);
  free(start);
  return ok;
}

static int zz9k_archive_7z_file_can_handle_entries(
    const ZZ9KArchiveEntry *entries,
    uint32_t count,
    uint32_t archive_length,
    int *needs_deflate,
    int *needs_lzma,
    int *needs_lzma2)
{
  uint32_t i;

  if (!needs_deflate || !needs_lzma || !needs_lzma2) {
    return 0;
  }
  *needs_deflate = 0;
  *needs_lzma = 0;
  *needs_lzma2 = 0;
  if (count == 0U) {
    return 1;
  }
  if (!entries) {
    return 0;
  }
  for (i = 0U; i < count; i++) {
    if (!zz9k_archive_entry_matches_filter(&entries[i])) {
      continue;
    }
    if (entries[i].is_dir) {
      continue;
    }
    if (entries[i].data_offset > archive_length ||
        entries[i].compressed_size >
          archive_length - entries[i].data_offset) {
      return 0;
    }
    if (zz9k_archive_7z_entry_has_split_substream(&entries[i]) &&
        entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_COPY) {
      return 0;
    }
    if (entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_COPY) {
      if (entries[i].compressed_size != entries[i].uncompressed_size) {
        return 0;
      }
    } else if (entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_DEFLATE) {
      *needs_deflate = 1;
    } else if (entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA) {
      *needs_lzma = 1;
    } else if (entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA2) {
      *needs_lzma2 = 1;
    } else {
      return 0;
    }
  }
  return 1;
}

static int zz9k_archive_handle_7z_file(ZZ9KContext **ctx,
                                       ZZ9KServiceInfo *service,
                                       int *codec_ready,
                                       const char *command,
                                       const char *archive_path,
                                       uint32_t archive_length,
                                       const char *output_dir,
                                       int *attempted)
{
  uint8_t *header = 0;
  ZZ9KArchiveEntry *entries = 0;
  uint32_t header_length = 0U;
  uint32_t count = 0U;
  uint32_t i;
  int is_list;
  int is_test;
  int is_extract;
  int needs_deflate = 0;
  int needs_lzma = 0;
  int needs_lzma2 = 0;
  int ok = 1;

  if (!attempted) {
    return 0;
  }
  *attempted = 0;
  if (!ctx || !service || !codec_ready || !archive_path) {
    return 0;
  }
  is_list = command && strcmp(command, "l") == 0;
  is_test = command && strcmp(command, "t") == 0;
  is_extract = command && strcmp(command, "x") == 0;
  if (!is_list && !is_test && !is_extract) {
    return 0;
  }
  if (!zz9k_archive_7z_read_header_from_file_with_codec(
          ctx, service, codec_ready, archive_path, archive_length,
          &header, &header_length)) {
    goto out;
  }
  if (header_length == 0U ||
      header[0] == ZZ9K_ARCHIVE_7Z_ID_ENCODED_HEADER) {
    goto out;
  }
  if (!zz9k_archive_7z_list_from_header(
          header, header_length, archive_length, 0, 0U, &count) ||
      !zz9k_archive_alloc_entries(count, &entries)) {
    zz9k_archive_print_7z_parse_failure();
    *attempted = 1;
    ok = 0;
    goto out;
  }
  if (!zz9k_archive_7z_list_from_header(
          header, header_length, archive_length, entries, count, &count)) {
    zz9k_archive_print_7z_parse_failure();
    *attempted = 1;
    ok = 0;
    goto out;
  }

  if (is_list) {
    *attempted = 1;
    for (i = 0U; i < count; i++) {
      if (!zz9k_archive_entry_matches_filter(&entries[i])) {
        continue;
      }
      zz9k_archive_print_entry(&entries[i]);
    }
    goto out;
  }

  if (!zz9k_archive_7z_file_can_handle_entries(
          entries, count, archive_length, &needs_deflate,
          &needs_lzma, &needs_lzma2)) {
    goto out;
  }
  if (needs_deflate || needs_lzma || needs_lzma2) {
    if (!zz9k_archive_ensure_codec_open(ctx, service, codec_ready)) {
      *attempted = 1;
      ok = 0;
      goto out;
    }
    if ((needs_deflate &&
         !zz9k_archive_service_supports_decompress_feed(
             service, ZZ9K_COMPRESSION_DEFLATE_RAW)) ||
        (needs_lzma &&
         !zz9k_archive_service_supports_decompress_feed(
             service, ZZ9K_COMPRESSION_LZMA_ALONE)) ||
        (needs_lzma2 &&
         !zz9k_archive_service_supports_decompress_feed(
             service, ZZ9K_COMPRESSION_LZMA2))) {
      goto out;
    }
  }

  *attempted = 1;
  for (i = 0U; i < count; i++) {
    if (zz9k_archive_7z_entry_has_split_substream(&entries[i])) {
      uint32_t group_count;

      group_count = zz9k_archive_7z_split_group_count(entries, count, i);
      if (group_count == 0U) {
        zz9k_archive_print_7z_entry_unsupported(&entries[i]);
        ok = 0;
        continue;
      }
      if (!zz9k_archive_handle_7z_split_group_file(
              *ctx, service, command, archive_path, output_dir,
              &entries[i], group_count)) {
        ok = 0;
      }
      i += group_count - 1U;
      continue;
    }
    if (!zz9k_archive_entry_matches_filter(&entries[i])) {
      continue;
    }
    if (!zz9k_archive_path_is_safe(entries[i].name)) {
      printf("unsafe path rejected: %s\n", entries[i].name);
      ok = 0;
      continue;
    }
    if (entries[i].is_dir) {
      if (is_extract) {
        ok &= zz9k_archive_write_entry(output_dir, &entries[i], header);
      }
    } else if (entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_COPY) {
      uint32_t actual_crc = 0U;

      if (is_test && !zz9k_archive_7z_copy_file_crc_matches(
              archive_path, &entries[i], &actual_crc)) {
        printf("7z entry crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
               entries[i].name,
               (unsigned long)actual_crc,
               (unsigned long)entries[i].crc32);
        ok = 0;
        continue;
      }
      if (is_extract) {
        /* One pass: the range copy verifies the CRC inline instead of a
           separate read-first verification round over the network. */
        ok &= zz9k_archive_write_file_range_entry(
            output_dir, &entries[i], archive_path, 1);
      }
    } else if (entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_DEFLATE) {
      ZZ9KDecompressResult result;
      uint32_t output_limit;

      output_limit = zz9k_archive_zip_test_output_limit(
          entries[i].uncompressed_size);
      if (is_test) {
        if (!zz9k_archive_decompress_feed_file_parts_to_result(
                *ctx, service, ZZ9K_COMPRESSION_DEFLATE_RAW,
                0, 0U, archive_path, entries[i].data_offset,
                entries[i].compressed_size, output_limit, &result)) {
          printf("7z Deflate test failed: %s packed=%lu unpacked=%lu "
                 "offset=%lu limit=%lu\n",
                 entries[i].name,
                 (unsigned long)entries[i].compressed_size,
                 (unsigned long)entries[i].uncompressed_size,
                 (unsigned long)entries[i].data_offset,
                 (unsigned long)output_limit);
          ok = 0;
          continue;
        }
      } else if (!zz9k_archive_decompress_feed_file_parts_to_file(
                     *ctx, service, ZZ9K_COMPRESSION_DEFLATE_RAW,
                     0, 0U, archive_path, entries[i].data_offset,
                     entries[i].compressed_size, output_limit,
                     output_dir, &entries[i], &result)) {
        printf("7z Deflate extract failed: %s packed=%lu unpacked=%lu "
               "offset=%lu limit=%lu\n",
               entries[i].name,
               (unsigned long)entries[i].compressed_size,
               (unsigned long)entries[i].uncompressed_size,
               (unsigned long)entries[i].data_offset,
               (unsigned long)output_limit);
        ok = 0;
        continue;
      }
      if (!zz9k_archive_7z_result_matches_entry(&entries[i], &result)) {
        if (result.bytes_written == entries[i].uncompressed_size &&
            zz9k_archive_entry_has_crc32(&entries[i])) {
          printf("7z Deflate crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
                 entries[i].name,
                 (unsigned long)result.checksum,
                 (unsigned long)entries[i].crc32);
        } else {
          printf("7z Deflate size mismatch: %s\n", entries[i].name);
        }
        ok = 0;
      }
    } else if (entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA) {
      uint8_t lzma_header[13];
      ZZ9KDecompressResult result;

      if (!zz9k_archive_7z_lzma_alone_header(&entries[i], lzma_header)) {
        zz9k_archive_print_7z_lzma_diag(&entries[i]);
        printf("7z LZMA %s failed: %s\n",
               is_test ? "test" : "extract", entries[i].name);
        ok = 0;
        continue;
      }
      if (is_test) {
        if (!zz9k_archive_decompress_feed_file_parts_to_result(
                *ctx, service, ZZ9K_COMPRESSION_LZMA_ALONE,
                lzma_header, sizeof(lzma_header), archive_path,
                entries[i].data_offset, entries[i].compressed_size,
                entries[i].uncompressed_size, &result)) {
          zz9k_archive_print_7z_lzma_diag(&entries[i]);
          printf("7z LZMA test failed: %s\n", entries[i].name);
          ok = 0;
          continue;
        }
      } else if (!zz9k_archive_decompress_feed_file_parts_to_file(
                     *ctx, service, ZZ9K_COMPRESSION_LZMA_ALONE,
                     lzma_header, sizeof(lzma_header), archive_path,
                     entries[i].data_offset, entries[i].compressed_size,
                     entries[i].uncompressed_size, output_dir, &entries[i],
                     &result)) {
        zz9k_archive_print_7z_lzma_diag(&entries[i]);
        printf("7z LZMA extract failed: %s\n", entries[i].name);
        ok = 0;
        continue;
      }
      if (!zz9k_archive_7z_result_matches_entry(&entries[i], &result)) {
        if (result.bytes_written == entries[i].uncompressed_size &&
            zz9k_archive_entry_has_crc32(&entries[i])) {
          printf("7z LZMA crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
                 entries[i].name,
                 (unsigned long)result.checksum,
                 (unsigned long)entries[i].crc32);
        } else {
          printf("7z LZMA size mismatch: %s\n", entries[i].name);
        }
        ok = 0;
      }
    } else if (entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA2) {
      ZZ9KDecompressResult result;
      int failure_status = ZZ9K_STATUS_OK;
      uint32_t lzma2_output_limit;

      if (entries[i].method_props_size != 1U ||
          !zz9k_archive_7z_lzma2_feed_output_limit(
              &entries[i], &lzma2_output_limit)) {
        zz9k_archive_print_7z_lzma2_diag(&entries[i]);
        printf("7z LZMA2 %s failed: %s\n",
               is_test ? "test" : "extract", entries[i].name);
        ok = 0;
        continue;
      }
      if (is_test) {
        if (!zz9k_archive_decompress_feed_file_parts_to_result_status(
                *ctx, service, ZZ9K_COMPRESSION_LZMA2,
                entries[i].method_props, entries[i].method_props_size,
                archive_path, entries[i].data_offset,
                entries[i].compressed_size, lzma2_output_limit,
                &result, &failure_status) &&
            (failure_status != ZZ9K_STATUS_NO_MEMORY ||
             !zz9k_archive_7z_lzma2_fallback_file_range(
                 *ctx, service, archive_path, output_dir, &entries[i],
                 0, &result))) {
          zz9k_archive_print_7z_lzma2_diag(&entries[i]);
          printf("7z LZMA2 test failed: %s\n", entries[i].name);
          ok = 0;
          continue;
        }
      } else if (!zz9k_archive_decompress_feed_file_parts_to_file_status(
                     *ctx, service, ZZ9K_COMPRESSION_LZMA2,
                     entries[i].method_props, entries[i].method_props_size,
                     archive_path, entries[i].data_offset,
                     entries[i].compressed_size, lzma2_output_limit,
                     output_dir, &entries[i], &result, &failure_status) &&
                 (failure_status != ZZ9K_STATUS_NO_MEMORY ||
                  !zz9k_archive_7z_lzma2_fallback_file_range(
                      *ctx, service, archive_path, output_dir, &entries[i],
                      1, &result))) {
        zz9k_archive_print_7z_lzma2_diag(&entries[i]);
        printf("7z LZMA2 extract failed: %s\n", entries[i].name);
        ok = 0;
        continue;
      }
      if (!zz9k_archive_7z_result_matches_entry(&entries[i], &result)) {
        if (result.bytes_written == entries[i].uncompressed_size &&
            zz9k_archive_entry_has_crc32(&entries[i])) {
          printf("7z LZMA2 crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
                 entries[i].name,
                 (unsigned long)result.checksum,
                 (unsigned long)entries[i].crc32);
        } else {
          printf("7z LZMA2 size mismatch: %s\n", entries[i].name);
        }
        ok = 0;
      }
    } else {
      zz9k_archive_print_7z_entry_unsupported(&entries[i]);
      ok = 0;
    }
  }

out:
  if (*attempted && is_test && ok) {
    printf("7z list ok: %lu entries\n", (unsigned long)count);
  }
  free(entries);
  free(header);
  return ok;
}

static int zz9k_archive_handle_7z_feed_file(ZZ9KContext **ctx,
                                            ZZ9KServiceInfo *service,
                                            int *codec_ready,
                                            const char *command,
                                            const char *archive_path,
                                            uint32_t archive_length,
                                            const char *output_dir,
                                            int *attempted)
{
  return zz9k_archive_handle_7z_file(
      ctx, service, codec_ready, command, archive_path, archive_length,
      output_dir, attempted);
}

static int zz9k_archive_handle_7z(ZZ9KContext **ctx,
                                  ZZ9KServiceInfo *service,
                                  int *codec_ready,
                                  const uint8_t *data,
                                  uint32_t length,
                                  const char *command,
                                  const char *output_dir)
{
  ZZ9KArchiveEntry *entries;
  uint32_t count;
  uint32_t i;
  int ok = 1;

  if (!zz9k_archive_7z_list(data, length, 0, 0U, &count) ||
      !zz9k_archive_alloc_entries(count, &entries)) {
    if (zz9k_archive_7z_header_is_encoded(data, length)) {
      printf("unsupported encoded 7z header\n");
    } else {
      zz9k_archive_print_7z_parse_failure();
    }
    return 0;
  }
  if (!zz9k_archive_7z_list(data, length, entries, count, &count)) {
    zz9k_archive_print_7z_parse_failure();
    free(entries);
    return 0;
  }

  for (i = 0U; i < count; i++) {
    if (!zz9k_archive_entry_matches_filter(&entries[i])) {
      continue;
    }
    if (strcmp(command, "l") == 0) {
      zz9k_archive_print_entry(&entries[i]);
    } else if (strcmp(command, "t") == 0) {
      if (!zz9k_archive_path_is_safe(entries[i].name)) {
        printf("unsafe path rejected: %s\n", entries[i].name);
        ok = 0;
        continue;
      }
      if (zz9k_archive_7z_entry_has_unsupported_split(&entries[i])) {
        zz9k_archive_print_7z_entry_unsupported(&entries[i]);
        ok = 0;
        continue;
      }
      if (!entries[i].is_dir &&
          entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_COPY) {
        uint32_t actual_crc = 0U;

        if (entries[i].data_offset > length ||
            entries[i].compressed_size > length - entries[i].data_offset ||
            entries[i].compressed_size != entries[i].uncompressed_size) {
          zz9k_archive_print_7z_entry_unsupported(&entries[i]);
          ok = 0;
          continue;
        }
        if (zz9k_archive_entry_has_crc32(&entries[i])) {
          actual_crc = zz9k_archive_crc32(
              0U, data + entries[i].data_offset,
              entries[i].uncompressed_size);
        }
        if (!zz9k_archive_7z_copy_entry_crc_matches(
                &entries[i], data + entries[i].data_offset)) {
          printf("7z entry crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
                 entries[i].name,
                 (unsigned long)actual_crc,
                 (unsigned long)entries[i].crc32);
          ok = 0;
          continue;
        }
      } else if (!entries[i].is_dir &&
                 entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_DEFLATE) {
        uint8_t *decoded = 0;
        ZZ9KDecompressResult result;
        uint32_t output_limit;

        output_limit = zz9k_archive_zip_test_output_limit(
            entries[i].uncompressed_size);
        if (entries[i].data_offset > length ||
            entries[i].compressed_size > length - entries[i].data_offset ||
            !zz9k_archive_ensure_codec_open(ctx, service, codec_ready)) {
          printf("7z Deflate test failed: %s\n", entries[i].name);
          ok = 0;
          free(decoded);
          continue;
        }
        if (zz9k_archive_service_supports_decompress_test(
                service, ZZ9K_COMPRESSION_DEFLATE_RAW)) {
          if (!zz9k_archive_decompress_test_to_result(
                  *ctx, service, ZZ9K_COMPRESSION_DEFLATE_RAW,
                  data + entries[i].data_offset, entries[i].compressed_size,
                  output_limit, &result)) {
            printf("7z Deflate test failed: %s\n", entries[i].name);
            ok = 0;
            continue;
          }
        } else if (!zz9k_archive_decompress_to_memory(
                       *ctx, service, ZZ9K_COMPRESSION_DEFLATE_RAW,
                       data + entries[i].data_offset,
                       entries[i].compressed_size, output_limit,
                       &decoded, &result)) {
          printf("7z Deflate test failed: %s\n", entries[i].name);
          ok = 0;
          free(decoded);
          continue;
        }
        if (!zz9k_archive_7z_result_matches_entry(&entries[i], &result)) {
          if (result.bytes_written == entries[i].uncompressed_size &&
              zz9k_archive_entry_has_crc32(&entries[i])) {
            printf("7z Deflate crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
                   entries[i].name,
                   (unsigned long)result.checksum,
                   (unsigned long)entries[i].crc32);
          } else {
            printf("7z Deflate size mismatch: %s\n", entries[i].name);
          }
          ok = 0;
        }
        free(decoded);
      } else if (!entries[i].is_dir &&
                 entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA) {
        uint8_t *wrapped = 0;
        uint8_t *decoded = 0;
        uint32_t wrapped_length = 0U;
        ZZ9KDecompressResult result;

        if (entries[i].data_offset > length ||
            entries[i].compressed_size > length - entries[i].data_offset ||
            !zz9k_archive_ensure_codec_open(ctx, service, codec_ready) ||
            !zz9k_archive_7z_build_lzma_alone_payload(
                &entries[i], data + entries[i].data_offset,
                entries[i].compressed_size, &wrapped, &wrapped_length)) {
          zz9k_archive_print_7z_lzma_diag(&entries[i]);
          printf("7z LZMA test failed: %s\n", entries[i].name);
          ok = 0;
          free(wrapped);
          free(decoded);
          continue;
        }
        if (zz9k_archive_service_supports_decompress_test(
                service, ZZ9K_COMPRESSION_LZMA_ALONE)) {
          if (!zz9k_archive_decompress_test_to_result(
                  *ctx, service, ZZ9K_COMPRESSION_LZMA_ALONE,
                  wrapped, wrapped_length, entries[i].uncompressed_size,
                  &result)) {
            zz9k_archive_print_7z_lzma_diag(&entries[i]);
            printf("7z LZMA test failed: %s\n", entries[i].name);
            ok = 0;
            free(wrapped);
            continue;
          }
        } else if (!zz9k_archive_decompress_to_memory(
                       *ctx, service, ZZ9K_COMPRESSION_LZMA_ALONE,
                       wrapped, wrapped_length, entries[i].uncompressed_size,
                       &decoded, &result)) {
          zz9k_archive_print_7z_lzma_diag(&entries[i]);
          printf("7z LZMA test failed: %s\n", entries[i].name);
          ok = 0;
          free(wrapped);
          free(decoded);
          continue;
        }
        if (!zz9k_archive_7z_result_matches_entry(&entries[i], &result)) {
          if (result.bytes_written == entries[i].uncompressed_size &&
              zz9k_archive_entry_has_crc32(&entries[i])) {
            printf("7z LZMA crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
                   entries[i].name,
                   (unsigned long)result.checksum,
                   (unsigned long)entries[i].crc32);
          } else {
            printf("7z LZMA size mismatch: %s\n", entries[i].name);
          }
          ok = 0;
        }
        free(wrapped);
        free(decoded);
      } else if (!entries[i].is_dir &&
                 entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA2) {
        uint8_t *wrapped = 0;
        uint8_t *decoded = 0;
        uint32_t wrapped_length = 0U;
        ZZ9KDecompressResult result;

        if (entries[i].data_offset > length ||
            entries[i].compressed_size > length - entries[i].data_offset ||
            !zz9k_archive_ensure_codec_open(ctx, service, codec_ready) ||
            !zz9k_archive_7z_build_lzma2_payload(
                &entries[i], data + entries[i].data_offset,
                entries[i].compressed_size, &wrapped, &wrapped_length)) {
          zz9k_archive_print_7z_lzma2_diag(&entries[i]);
          printf("7z LZMA2 test failed: %s\n", entries[i].name);
          ok = 0;
          free(wrapped);
          free(decoded);
          continue;
        }
        if (zz9k_archive_service_supports_decompress_test(
                service, ZZ9K_COMPRESSION_LZMA2)) {
          if (!zz9k_archive_decompress_test_to_result(
                  *ctx, service, ZZ9K_COMPRESSION_LZMA2,
                  wrapped, wrapped_length, entries[i].uncompressed_size,
                  &result)) {
            zz9k_archive_print_7z_lzma2_diag(&entries[i]);
            printf("7z LZMA2 test failed: %s\n", entries[i].name);
            ok = 0;
            free(wrapped);
            continue;
          }
        } else if (!zz9k_archive_decompress_to_memory(
                       *ctx, service, ZZ9K_COMPRESSION_LZMA2,
                       wrapped, wrapped_length, entries[i].uncompressed_size,
                       &decoded, &result)) {
          zz9k_archive_print_7z_lzma2_diag(&entries[i]);
          printf("7z LZMA2 test failed: %s\n", entries[i].name);
          ok = 0;
          free(wrapped);
          free(decoded);
          continue;
        }
        if (!zz9k_archive_7z_result_matches_entry(&entries[i], &result)) {
          if (result.bytes_written == entries[i].uncompressed_size &&
              zz9k_archive_entry_has_crc32(&entries[i])) {
            printf("7z LZMA2 crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
                   entries[i].name,
                   (unsigned long)result.checksum,
                   (unsigned long)entries[i].crc32);
          } else {
            printf("7z LZMA2 size mismatch: %s\n", entries[i].name);
          }
          ok = 0;
        }
        free(wrapped);
        free(decoded);
      }
    } else {
      if (entries[i].is_dir) {
        ok &= zz9k_archive_write_entry(output_dir, &entries[i], data);
      } else if (zz9k_archive_7z_entry_has_unsupported_split(&entries[i])) {
        zz9k_archive_print_7z_entry_unsupported(&entries[i]);
        ok = 0;
        continue;
      } else if (entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_COPY) {
        if (entries[i].compressed_size != entries[i].uncompressed_size ||
            entries[i].data_offset > length ||
            entries[i].compressed_size > length - entries[i].data_offset) {
          zz9k_archive_print_7z_entry_unsupported(&entries[i]);
          ok = 0;
          continue;
        }
        if (!zz9k_archive_7z_copy_entry_crc_matches(
                &entries[i], data + entries[i].data_offset)) {
          printf("7z entry crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
                 entries[i].name,
                 (unsigned long)zz9k_archive_crc32(
                     0U, data + entries[i].data_offset,
                     entries[i].uncompressed_size),
                 (unsigned long)entries[i].crc32);
          ok = 0;
          continue;
        }
        ok &= zz9k_archive_write_entry(
            output_dir, &entries[i], data + entries[i].data_offset);
      } else if (entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_DEFLATE) {
        uint8_t *decoded = 0;
        ZZ9KDecompressResult result;
        uint32_t output_limit;

        output_limit = zz9k_archive_zip_test_output_limit(
            entries[i].uncompressed_size);
        if (entries[i].data_offset > length ||
            entries[i].compressed_size > length - entries[i].data_offset ||
            !zz9k_archive_ensure_codec_open(ctx, service, codec_ready)) {
          printf("7z Deflate extract failed: %s\n", entries[i].name);
          ok = 0;
          continue;
        }
        if (zz9k_archive_service_supports_decompress_feed(
                service, ZZ9K_COMPRESSION_DEFLATE_RAW)) {
          if (!zz9k_archive_decompress_feed_stream_parts_to_file(
                  *ctx, service, ZZ9K_COMPRESSION_DEFLATE_RAW,
                  0, 0U, data + entries[i].data_offset,
                  entries[i].compressed_size, output_limit,
                  output_dir, &entries[i], &result)) {
            printf("7z Deflate extract failed: %s\n", entries[i].name);
            ok = 0;
            continue;
          }
        } else if (zz9k_archive_service_supports_decompress_stream(
                service, ZZ9K_COMPRESSION_DEFLATE_RAW)) {
          if (!zz9k_archive_decompress_stream_to_file(
                  *ctx, service, ZZ9K_COMPRESSION_DEFLATE_RAW,
                  data + entries[i].data_offset, entries[i].compressed_size,
                  output_limit, output_dir, &entries[i], &result)) {
            printf("7z Deflate extract failed: %s\n", entries[i].name);
            ok = 0;
            continue;
          }
        } else if (!zz9k_archive_decompress_to_memory(
                       *ctx, service, ZZ9K_COMPRESSION_DEFLATE_RAW,
                       data + entries[i].data_offset,
                       entries[i].compressed_size, output_limit,
                       &decoded, &result)) {
          printf("7z Deflate extract failed: %s\n", entries[i].name);
          ok = 0;
          free(decoded);
          continue;
        }
        if (!zz9k_archive_7z_result_matches_entry(&entries[i], &result)) {
          if (result.bytes_written == entries[i].uncompressed_size &&
              zz9k_archive_entry_has_crc32(&entries[i])) {
            printf("7z Deflate crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
                   entries[i].name,
                   (unsigned long)result.checksum,
                   (unsigned long)entries[i].crc32);
          } else {
            printf("7z Deflate size mismatch: %s\n", entries[i].name);
          }
          ok = 0;
        } else if (decoded) {
          entries[i].uncompressed_size = result.bytes_written;
          ok &= zz9k_archive_write_entry(output_dir, &entries[i], decoded);
        }
        free(decoded);
      } else if (entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA) {
        uint8_t lzma_header[13];
        uint8_t *wrapped = 0;
        uint8_t *decoded = 0;
        uint32_t wrapped_length = 0U;
        ZZ9KDecompressResult result;

        if (entries[i].data_offset > length ||
            entries[i].compressed_size > length - entries[i].data_offset ||
            !zz9k_archive_ensure_codec_open(ctx, service, codec_ready)) {
          zz9k_archive_print_7z_lzma_diag(&entries[i]);
          printf("7z LZMA extract failed: %s\n", entries[i].name);
          ok = 0;
          continue;
        }
        if (zz9k_archive_service_supports_decompress_feed(
                service, ZZ9K_COMPRESSION_LZMA_ALONE)) {
          if (!zz9k_archive_7z_lzma_alone_header(
                  &entries[i], lzma_header) ||
              !zz9k_archive_decompress_feed_stream_parts_to_file(
                  *ctx, service, ZZ9K_COMPRESSION_LZMA_ALONE,
                  lzma_header, sizeof(lzma_header),
                  data + entries[i].data_offset, entries[i].compressed_size,
                  entries[i].uncompressed_size, output_dir, &entries[i],
                  &result)) {
            zz9k_archive_print_7z_lzma_diag(&entries[i]);
            printf("7z LZMA extract failed: %s\n", entries[i].name);
            ok = 0;
            continue;
          }
        } else if (zz9k_archive_service_supports_decompress_stream(
                service, ZZ9K_COMPRESSION_LZMA_ALONE)) {
          if (!zz9k_archive_7z_build_lzma_alone_payload(
                  &entries[i], data + entries[i].data_offset,
                  entries[i].compressed_size, &wrapped, &wrapped_length)) {
            zz9k_archive_print_7z_lzma_diag(&entries[i]);
            printf("7z LZMA extract failed: %s\n", entries[i].name);
            ok = 0;
            continue;
          }
          if (!zz9k_archive_decompress_stream_to_file(
                  *ctx, service, ZZ9K_COMPRESSION_LZMA_ALONE,
                  wrapped, wrapped_length, entries[i].uncompressed_size,
                  output_dir, &entries[i], &result)) {
            zz9k_archive_print_7z_lzma_diag(&entries[i]);
            printf("7z LZMA extract failed: %s\n", entries[i].name);
            ok = 0;
            free(wrapped);
            continue;
          }
        } else {
          if (!zz9k_archive_7z_build_lzma_alone_payload(
                  &entries[i], data + entries[i].data_offset,
                  entries[i].compressed_size, &wrapped, &wrapped_length) ||
              !zz9k_archive_decompress_to_memory(
                  *ctx, service, ZZ9K_COMPRESSION_LZMA_ALONE,
                  wrapped, wrapped_length, entries[i].uncompressed_size,
                  &decoded, &result)) {
            zz9k_archive_print_7z_lzma_diag(&entries[i]);
            printf("7z LZMA extract failed: %s\n", entries[i].name);
            ok = 0;
            free(wrapped);
            free(decoded);
            continue;
          }
        }
        if (!zz9k_archive_7z_result_matches_entry(&entries[i], &result)) {
          if (result.bytes_written == entries[i].uncompressed_size &&
              zz9k_archive_entry_has_crc32(&entries[i])) {
            printf("7z LZMA crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
                   entries[i].name,
                   (unsigned long)result.checksum,
                   (unsigned long)entries[i].crc32);
          } else {
            printf("7z LZMA size mismatch: %s\n", entries[i].name);
          }
          ok = 0;
        } else if (decoded) {
          entries[i].uncompressed_size = result.bytes_written;
          ok &= zz9k_archive_write_entry(output_dir, &entries[i], decoded);
        }
        free(wrapped);
        free(decoded);
      } else if (entries[i].method == ZZ9K_ARCHIVE_7Z_METHOD_LZMA2) {
        uint8_t *wrapped = 0;
        uint8_t *decoded = 0;
        uint32_t wrapped_length = 0U;
        uint32_t lzma2_output_limit;
        ZZ9KDecompressResult result;

        if (entries[i].data_offset > length ||
            entries[i].compressed_size > length - entries[i].data_offset ||
            !zz9k_archive_ensure_codec_open(ctx, service, codec_ready)) {
          zz9k_archive_print_7z_lzma2_diag(&entries[i]);
          printf("7z LZMA2 extract failed: %s\n", entries[i].name);
          ok = 0;
          continue;
        }
        if (zz9k_archive_service_supports_decompress_feed(
                service, ZZ9K_COMPRESSION_LZMA2)) {
          if (
              !zz9k_archive_7z_lzma2_feed_output_limit(
                  &entries[i], &lzma2_output_limit) ||
              !zz9k_archive_decompress_feed_stream_parts_to_file(
                  *ctx, service, ZZ9K_COMPRESSION_LZMA2,
                  entries[i].method_props, entries[i].method_props_size,
                  data + entries[i].data_offset, entries[i].compressed_size,
                  lzma2_output_limit, output_dir, &entries[i],
                  &result)) {
            zz9k_archive_print_7z_lzma2_diag(&entries[i]);
            printf("7z LZMA2 extract failed: %s\n", entries[i].name);
            ok = 0;
            continue;
          }
        } else if (zz9k_archive_service_supports_decompress_stream(
                service, ZZ9K_COMPRESSION_LZMA2)) {
          if (!zz9k_archive_7z_build_lzma2_payload(
                  &entries[i], data + entries[i].data_offset,
                  entries[i].compressed_size, &wrapped, &wrapped_length)) {
            zz9k_archive_print_7z_lzma2_diag(&entries[i]);
            printf("7z LZMA2 extract failed: %s\n", entries[i].name);
            ok = 0;
            continue;
          }
          if (!zz9k_archive_decompress_stream_to_file(
                  *ctx, service, ZZ9K_COMPRESSION_LZMA2,
                  wrapped, wrapped_length, entries[i].uncompressed_size,
                  output_dir, &entries[i], &result)) {
            zz9k_archive_print_7z_lzma2_diag(&entries[i]);
            printf("7z LZMA2 extract failed: %s\n", entries[i].name);
            ok = 0;
            free(wrapped);
            continue;
          }
        } else {
          if (!zz9k_archive_7z_build_lzma2_payload(
                  &entries[i], data + entries[i].data_offset,
                  entries[i].compressed_size, &wrapped, &wrapped_length) ||
              !zz9k_archive_decompress_to_memory(
                  *ctx, service, ZZ9K_COMPRESSION_LZMA2,
                  wrapped, wrapped_length, entries[i].uncompressed_size,
                  &decoded, &result)) {
            zz9k_archive_print_7z_lzma2_diag(&entries[i]);
            printf("7z LZMA2 extract failed: %s\n", entries[i].name);
            ok = 0;
            free(wrapped);
            free(decoded);
            continue;
          }
        }
        if (!zz9k_archive_7z_result_matches_entry(&entries[i], &result)) {
          if (result.bytes_written == entries[i].uncompressed_size &&
              zz9k_archive_entry_has_crc32(&entries[i])) {
            printf("7z LZMA2 crc mismatch: %s decoded=0x%08lx expected=0x%08lx\n",
                   entries[i].name,
                   (unsigned long)result.checksum,
                   (unsigned long)entries[i].crc32);
          } else {
            printf("7z LZMA2 size mismatch: %s\n", entries[i].name);
          }
          ok = 0;
        } else if (decoded) {
          entries[i].uncompressed_size = result.bytes_written;
          ok &= zz9k_archive_write_entry(output_dir, &entries[i], decoded);
        }
        free(wrapped);
        free(decoded);
      } else {
        zz9k_archive_print_7z_entry_unsupported(&entries[i]);
        ok = 0;
      }
    }
  }
  if (strcmp(command, "t") == 0 && ok) {
    printf("7z list ok: %lu entries\n", (unsigned long)count);
  }
  free(entries);
  return ok;
}

static int zz9k_archive_run(const char *command, const char *archive_path,
                            const char *output_dir,
                            uint32_t lzma_capacity)
{
  uint8_t *data = 0;
  uint8_t probe[ZZ9K_ARCHIVE_PROBE_BYTES];
  uint32_t probe_length = 0U;
  uint32_t file_length = 0U;
  uint32_t length = 0U;
  ZZ9KArchiveFormat format;
  ZZ9KContext *ctx = 0;
  ZZ9KServiceInfo service;
  int need_codec;
  int codec_ready = 0;
  int status;
  int ok = 0;

  zz9k_archive_cancel_latched = 0;
  memset(&service, 0, sizeof(service));
  if (!zz9k_archive_probe_file(archive_path, probe, sizeof(probe),
                               &probe_length, &file_length)) {
    return 0;
  }
  format = zz9k_archive_detect_format(probe, probe_length);
  printf("zz9k-archive v2.8.0 2026-09-27\n");
  printf("archive: %s (%s)\n", archive_path, zz9k_archive_format_name(format));

  if (format == ZZ9K_ARCHIVE_FORMAT_LZMA_ALONE &&
      (strcmp(command, "t") == 0 || strcmp(command, "x") == 0)) {
    ZZ9KArchiveLzmaInfo info;
    ZZ9KArchiveEntry entry;
    ZZ9KDecompressResult result;
    uint32_t output_capacity;

    if (!zz9k_archive_lzma_info_from_header(
            probe, probe_length, file_length, &info)) {
      printf("lzma-alone parse failed\n");
      goto out;
    }
    if (!zz9k_archive_lzma_output_capacity(&info, lzma_capacity,
                                           &output_capacity)) {
      if (!info.size_known) {
        printf("lzma-alone unknown output size requires --capacity bytes\n");
      } else {
        printf("lzma-alone output capacity too small\n");
      }
      goto out;
    }
    status = zz9k_open(&ctx);
    if (status != ZZ9K_STATUS_OK) {
      printf("open failed: %s (%d)\n", zz9k_status_name(status), status);
      goto out;
    }
    if (!zz9k_archive_require_codec_service(ctx, &service)) {
      goto out;
    }
    codec_ready = 1;
    if (zz9k_archive_service_supports_decompress_feed(
            &service, ZZ9K_COMPRESSION_LZMA_ALONE)) {
      if (strcmp(command, "t") == 0) {
        if (!zz9k_archive_decompress_feed_file_to_result(
                ctx, &service, ZZ9K_COMPRESSION_LZMA_ALONE,
                archive_path, file_length, output_capacity, &result)) {
          goto out;
        }
      } else {
        memset(&entry, 0, sizeof(entry));
        strcpy(entry.name, info.name);
        entry.method = ZZ9K_COMPRESSION_LZMA_ALONE;
        entry.uncompressed_size = info.size_known ?
            info.uncompressed_size : output_capacity;
        if (!zz9k_archive_decompress_feed_file_to_file(
                ctx, &service, ZZ9K_COMPRESSION_LZMA_ALONE,
                archive_path, file_length, output_capacity,
                output_dir, &entry, &result)) {
          goto out;
        }
      }
      if (info.size_known && result.bytes_written != info.uncompressed_size) {
        printf("lzma-alone size mismatch\n");
        goto out;
      }
      if (strcmp(command, "t") == 0) {
        printf("lzma-alone test ok: out=%lu crc32=0x%08lx\n",
               (unsigned long)result.bytes_written,
               (unsigned long)result.checksum);
      }
      ok = 1;
      goto out;
    }
  }

  if (format == ZZ9K_ARCHIVE_FORMAT_7Z) {
    int file_attempted = 0;
    int file_ok = zz9k_archive_handle_7z_file(
        &ctx, &service, &codec_ready, command, archive_path, file_length,
        output_dir, &file_attempted);

    if (file_attempted) {
      ok = file_ok;
      goto out;
    }
  }

  if (format == ZZ9K_ARCHIVE_FORMAT_GZIP) {
    int file_attempted = 0;
    int file_ok = zz9k_archive_handle_gzip_file(
        &ctx, &service, &codec_ready, archive_path, probe, probe_length,
        file_length, command, output_dir, &file_attempted);

    if (file_attempted) {
      ok = file_ok;
      goto out;
    }
  }

  if (format == ZZ9K_ARCHIVE_FORMAT_ZIP) {
    int file_attempted = 0;
    int file_ok = zz9k_archive_handle_zip_file(
        &ctx, &service, &codec_ready, archive_path, file_length,
        command, output_dir, &file_attempted);

    if (file_attempted) {
      ok = file_ok;
      goto out;
    }
  }

  if (format == ZZ9K_ARCHIVE_FORMAT_LHA) {
    int file_attempted = 0;
    int file_ok = zz9k_archive_handle_lha_file(
        &ctx, &service, &codec_ready, archive_path, file_length,
        command, output_dir, &file_attempted);

    if (file_attempted) {
      ok = file_ok;
      goto out;
    }
  }
  if (format == ZZ9K_ARCHIVE_FORMAT_TAR) {
    int file_attempted = 0;
    int file_ok = zz9k_archive_handle_tar_file(
        archive_path, file_length, command, output_dir, &file_attempted);

    if (file_attempted) {
      ok = file_ok;
      goto out;
    }
  }

  if (!zz9k_archive_read_file(archive_path, &data, &length)) {
    goto out;
  }
  format = zz9k_archive_detect_format(data, length);

  need_codec = format == ZZ9K_ARCHIVE_FORMAT_GZIP ||
               (strcmp(command, "l") != 0 &&
                 format == ZZ9K_ARCHIVE_FORMAT_LZMA_ALONE) ||
                (strcmp(command, "l") != 0 &&
                 format == ZZ9K_ARCHIVE_FORMAT_ZIP) ||
                (strcmp(command, "l") != 0 &&
                 format == ZZ9K_ARCHIVE_FORMAT_LHA);
  if (need_codec && !codec_ready) {
    int lha_soft = (format == ZZ9K_ARCHIVE_FORMAT_LHA);

    status = zz9k_open(&ctx);
    if (status != ZZ9K_STATUS_OK) {
      if (!lha_soft) {
        printf("open failed: %s (%d)\n", zz9k_status_name(status), status);
        goto out;
      }
      ctx = 0;                    /* LHA: no board -> software decode */
    } else if (!zz9k_archive_require_codec_service(ctx, &service)) {
      if (!lha_soft) {
        goto out;
      }
      zz9k_close(ctx);
      ctx = 0;                    /* LHA: no codec service -> software */
    } else {
      codec_ready = 1;
    }
  }

  if (format == ZZ9K_ARCHIVE_FORMAT_GZIP) {
    ok = zz9k_archive_handle_gzip(ctx, &service, data, length,
                                  command, output_dir);
  } else if (format == ZZ9K_ARCHIVE_FORMAT_ZIP) {
    ok = zz9k_archive_handle_zip(ctx, need_codec ? &service : 0,
                                 data, length, command, output_dir);
  } else if (format == ZZ9K_ARCHIVE_FORMAT_TAR) {
    ok = zz9k_archive_handle_tar(ctx, &service, data, length,
                                 command, output_dir);
  } else if (format == ZZ9K_ARCHIVE_FORMAT_LHA) {
    ok = zz9k_archive_handle_lha(ctx, codec_ready ? &service : 0,
                                 data, length, command, output_dir);
  } else if (format == ZZ9K_ARCHIVE_FORMAT_LZMA_ALONE) {
    ok = zz9k_archive_handle_lzma(ctx, need_codec ? &service : 0,
                                  data, length, command, output_dir,
                                  lzma_capacity);
  } else if (format == ZZ9K_ARCHIVE_FORMAT_7Z) {
    ok = zz9k_archive_handle_7z(&ctx, &service, &codec_ready, data, length,
                                command, output_dir);
  } else {
    printf("unsupported archive format\n");
    ok = 0;
  }

out:
  if (ctx) {
    zz9k_disarm_completion_irq(ctx);
    zz9k_close(ctx);
  }
  free(data);
  return ok;
}

#ifndef ZZ9K_ARCHIVE_NO_MAIN
static void usage(const char *name)
{
  printf("usage: %s l|t|x [-o output-dir] [--capacity bytes] [--match text] "
         "[--strip-components n] [--dry-run] "
         "[--overwrite|--skip-existing] <archive>\n",
         name);
  printf("       commands: l=list, t=test, x=extract\n");
  printf("       formats: gzip, lzma-alone, tar, tar.gz, lha -lh0-, "
         "zip store/deflate; 7z Copy/Deflate/LZMA/LZMA2 unencoded headers\n");
}

int main(int argc, char **argv)
{
  const char *command = 0;
  const char *archive_path = 0;
  const char *output_dir = "";
  uint32_t lzma_capacity = 0U;
  int arg;

  if (argc < 3) {
    usage(argv[0]);
    return 2;
  }
  command = argv[1];
  if (strcmp(command, "l") != 0 &&
      strcmp(command, "t") != 0 &&
      strcmp(command, "x") != 0) {
    usage(argv[0]);
    return 2;
  }

  arg = 2;
  while (arg < argc) {
    if (strcmp(argv[arg], "-o") == 0) {
      arg++;
      if (arg >= argc) {
        usage(argv[0]);
        return 2;
      }
      output_dir = argv[arg++];
    } else if (strcmp(argv[arg], "--capacity") == 0) {
      char *end = 0;
      unsigned long parsed;

      arg++;
      if (arg >= argc) {
        usage(argv[0]);
        return 2;
      }
      parsed = strtoul(argv[arg], &end, 0);
      if (!end || *end != '\0' || parsed == 0UL ||
          parsed > 0x7fffffffUL) {
        usage(argv[0]);
        return 2;
      }
      lzma_capacity = (uint32_t)parsed;
      arg++;
    } else if (strcmp(argv[arg], "--match") == 0) {
      arg++;
      if (arg >= argc || argv[arg][0] == '\0') {
        usage(argv[0]);
        return 2;
      }
      zz9k_archive_match_filter = argv[arg++];
    } else if (strcmp(argv[arg], "--strip-components") == 0) {
      char *end = 0;
      unsigned long parsed;

      arg++;
      if (arg >= argc) {
        usage(argv[0]);
        return 2;
      }
      parsed = strtoul(argv[arg], &end, 0);
      if (!end || *end != '\0' || parsed > 255UL) {
        usage(argv[0]);
        return 2;
      }
      zz9k_archive_strip_components = (uint32_t)parsed;
      arg++;
    } else if (strcmp(argv[arg], "--overwrite") == 0) {
      zz9k_archive_overwrite_outputs = 1;
      zz9k_archive_skip_existing_outputs = 0;
      arg++;
    } else if (strcmp(argv[arg], "--skip-existing") == 0) {
      zz9k_archive_skip_existing_outputs = 1;
      zz9k_archive_overwrite_outputs = 0;
      arg++;
    } else if (strcmp(argv[arg], "--dry-run") == 0) {
      zz9k_archive_dry_run_outputs = 1;
      arg++;
    } else {
      if (archive_path) {
        usage(argv[0]);
        return 2;
      }
      archive_path = argv[arg++];
    }
  }
  if (!archive_path) {
    usage(argv[0]);
    return 2;
  }

  return zz9k_archive_run(command, archive_path, output_dir,
                          lzma_capacity) ? 0 : 1;
}
#endif
