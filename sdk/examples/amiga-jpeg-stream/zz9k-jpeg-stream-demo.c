/*
 * Public zz9k.library JPEG image-session example.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/caps.h"
#include "zz9k/image.h"
#include "zz9k/shared.h"
#include "zz9k/surface.h"
#include "zz9k/text.h"
#include <proto/exec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_STAGING_BYTES (64UL * 1024UL)
#define DEMO_FILE_CHUNK_BYTES (16UL * 1024UL)
#define DEMO_TILE_TARGET_BYTES (256UL * 1024UL)

struct Library *ZZ9KBase;

typedef struct DemoInput {
  const char *path;
  uint32_t length;
  uint32_t width;
  uint32_t height;
  uint32_t tile_rows;
  int use_framebuffer;
} DemoInput;

static uint16_t read_be16(const uint8_t *bytes)
{
  return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static int sof_marker(uint8_t marker)
{
  return (marker >= 0xc0U && marker <= 0xcfU && marker != 0xc4U &&
          marker != 0xc8U && marker != 0xccU);
}

static int standalone_marker(uint8_t marker)
{
  return marker == 0x01U || (marker >= 0xd0U && marker <= 0xd9U);
}

static int read_exact(FILE *file, uint8_t *dst, uint32_t length)
{
  return fread(dst, 1U, length, file) == length;
}

static int skip_file_bytes(FILE *file, uint32_t length)
{
  return fseek(file, (long)length, SEEK_CUR) == 0;
}

static int read_jpeg_dimensions(FILE *file, uint32_t *width,
                                uint32_t *height)
{
  uint8_t bytes[8];

  if (!file || !width || !height || !read_exact(file, bytes, 2U))
    return 0;
  if (bytes[0] != 0xffU || bytes[1] != 0xd8U)
    return 0;

  for (;;) {
    uint8_t marker;
    uint16_t segment_length;
    uint32_t payload_length;

    if (!read_exact(file, bytes, 1U))
      return 0;
    while (bytes[0] != 0xffU) {
      if (!read_exact(file, bytes, 1U))
        return 0;
    }
    do {
      if (!read_exact(file, bytes, 1U))
        return 0;
    } while (bytes[0] == 0xffU);

    marker = bytes[0];
    if (standalone_marker(marker))
      continue;
    if (!read_exact(file, bytes, 2U))
      return 0;

    segment_length = read_be16(bytes);
    if (segment_length < 2U)
      return 0;
    payload_length = (uint32_t)segment_length - 2U;

    if (sof_marker(marker)) {
      if (payload_length < 6U || !read_exact(file, bytes, 6U))
        return 0;
      *height = read_be16(&bytes[1]);
      *width = read_be16(&bytes[3]);
      return *width != 0U && *height != 0U;
    }

    if (!skip_file_bytes(file, payload_length))
      return 0;
  }
}

static int load_input_metadata(DemoInput *input)
{
  FILE *file;
  long length;
  int ok;

  file = fopen(input->path, "rb");
  if (!file) {
    printf("failed to open '%s'\n", input->path);
    return 0;
  }

  ok = 0;
  if (fseek(file, 0, SEEK_END) != 0)
    goto out;
  length = ftell(file);
  if (length <= 0 || length > 0x7fffffffL)
    goto out;
  if (fseek(file, 0, SEEK_SET) != 0)
    goto out;
  if (!read_jpeg_dimensions(file, &input->width, &input->height))
    goto out;

  input->length = (uint32_t)length;
  ok = 1;

out:
  fclose(file);
  if (!ok)
    printf("could not read JPEG metadata from '%s'\n", input->path);
  return ok;
}

static uint32_t choose_tile_rows(uint32_t width, uint32_t output_format,
                                 uint32_t requested)
{
  uint32_t row_bytes;
  uint32_t rows;

  if (!zz9k_surface_min_pitch(width, output_format, &row_bytes))
    return 0U;
  if (requested != 0U)
    return requested;

  rows = DEMO_TILE_TARGET_BYTES / row_bytes;
  if (rows == 0U)
    rows = 1U;
  return rows;
}

static int parse_u32(const char *text, uint32_t *value)
{
  char *end;
  unsigned long parsed;

  if (!text || !*text || !value)
    return 0;
  parsed = strtoul(text, &end, 10);
  if (*end != '\0' || parsed > 0xffffffffUL)
    return 0;
  *value = (uint32_t)parsed;
  return 1;
}

static void usage(void)
{
  printf("usage: zz9k-jpeg-stream-demo [--fb] [--tile-rows N] file.jpg\n");
}

static int parse_args(int argc, char **argv, DemoInput *input)
{
  int i;

  memset(input, 0, sizeof(*input));
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--fb") == 0) {
      input->use_framebuffer = 1;
    } else if (strcmp(argv[i], "--tile-rows") == 0) {
      if (++i >= argc || !parse_u32(argv[i], &input->tile_rows)) {
        usage();
        return 0;
      }
    } else if (!input->path) {
      input->path = argv[i];
    } else {
      usage();
      return 0;
    }
  }

  if (!input->path) {
    usage();
    return 0;
  }

  return load_input_metadata(input);
}

static void print_missing_capabilities(uint32_t missing)
{
  uint32_t remaining;
  uint32_t count;
  uint32_t i;
  int first;

  printf("missing required capabilities:");
  remaining = missing;
  count = zz9k_known_capability_count();
  first = 1;
  for (i = 0; i < count; i++) {
    uint32_t bit;
    const char *name;

    bit = zz9k_known_capability_bit(i);
    if (bit == 0U || (missing & bit) == 0U)
      continue;
    name = zz9k_capability_name(bit);
    if (name) {
      printf("%s%s", first ? " " : ",", name);
      remaining &= ~bit;
      first = 0;
    }
  }
  if (remaining != 0U) {
    printf("%s0x%08lx", first ? " " : ",", (unsigned long)remaining);
    first = 0;
  }
  if (first)
    printf(" none");
  printf("\n");
}

static void print_missing_image_service_flags(uint32_t missing)
{
  uint32_t remaining;
  uint32_t count;
  uint32_t i;
  int first;

  printf("image service missing required flags:");
  remaining = missing;
  count = zz9k_known_service_flag_count(ZZ9K_SERVICE_IMAGE);
  first = 1;
  for (i = 0; i < count; i++) {
    uint32_t flag;
    const char *name;

    flag = zz9k_known_service_flag(ZZ9K_SERVICE_IMAGE, i);
    if (flag == 0U || (missing & flag) == 0U)
      continue;
    name = zz9k_service_flag_name(ZZ9K_SERVICE_IMAGE, flag);
    if (name) {
      printf("%s%s", first ? " " : ",", name);
      remaining &= ~flag;
      first = 0;
    }
  }
  if (remaining != 0U) {
    printf("%s0x%08lx", first ? " " : ",", (unsigned long)remaining);
    first = 0;
  }
  if (first)
    printf(" none");
  printf("\n");
}

static int require_image_session_support(int use_framebuffer)
{
  ZZ9KCaps caps;
  ZZ9KServiceInfo service;
  uint32_t required_caps;
  uint32_t flags;
  uint32_t missing;
  int status;

  memset(&caps, 0, sizeof(caps));
  status = ZZ9KQueryCaps(&caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("ZZ9KQueryCaps failed: %s (%d)\n", zz9k_status_text(status),
           status);
    return 0;
  }

  required_caps = ZZ9K_CAP_SHARED_ALLOC | ZZ9K_CAP_IMAGE_DECODE |
                  ZZ9K_CAP_SERVICE_DISCOVERY;
  if (!zz9k_has_capabilities(caps.capability_bits, required_caps)) {
    missing = zz9k_missing_capabilities(caps.capability_bits, required_caps);
    print_missing_capabilities(missing);
    return 0;
  }

  memset(&service, 0, sizeof(service));
  status = ZZ9KQueryService(ZZ9K_SERVICE_IMAGE, &service);
  if (status != ZZ9K_STATUS_OK) {
    printf("ZZ9KQueryService(image) failed: %s (%d)\n",
           zz9k_status_text(status), status);
    return 0;
  }

  if (!zz9k_image_stream_required_service_flags(
          ZZ9K_IMAGE_CODEC_JPEG,
          use_framebuffer ? ZZ9K_IMAGE_OUTPUT_FRAMEBUFFER :
                            ZZ9K_IMAGE_OUTPUT_TILE_BUFFER,
          &flags)) {
    printf("could not build required image service flags\n");
    return 0;
  }
  if (!zz9k_has_service_flags(service.flags, flags)) {
    missing = zz9k_missing_service_flags(service.flags, flags);
    print_missing_image_service_flags(missing);
    return 0;
  }

  return 1;
}

static int read_chunk_to_shared(FILE *file, ZZ9KSharedBuffer *buffer,
                                uint32_t offset, uint32_t length)
{
  uint8_t scratch[1024];
  uint32_t copied;

  copied = 0U;
  while (copied < length) {
    uint32_t want = length - copied;
    size_t got;

    if (want > (uint32_t)sizeof(scratch))
      want = (uint32_t)sizeof(scratch);
    got = fread(scratch, 1U, want, file);
    if (got != want ||
        !zz9k_shared_copy_to(buffer, offset + copied, scratch,
                             (uint32_t)got)) {
      return 0;
    }
    copied += (uint32_t)got;
  }
  return 1;
}

static int fill_staging(FILE *file, const DemoInput *input,
                        ZZ9KSharedBuffer *staging,
                        uint32_t *file_offset, uint32_t *buffered)
{
  while (*file_offset < input->length && *buffered < staging->length) {
    uint32_t want = input->length - *file_offset;
    uint32_t room = staging->length - *buffered;

    if (want > DEMO_FILE_CHUNK_BYTES)
      want = DEMO_FILE_CHUNK_BYTES;
    if (want > room)
      want = room;

    if (!read_chunk_to_shared(file, staging, *buffered, want)) {
      printf("short read from '%s'\n", input->path);
      return 0;
    }
    *file_offset += want;
    *buffered += want;
  }

  return 1;
}

static int stream_made_progress(const ZZ9KImageSessionResult *result)
{
  return result->bytes_consumed != 0U || result->bytes_written != 0U ||
         result->state == ZZ9K_IMAGE_SESSION_STATE_COMPLETE;
}

static int no_progress_is_fatal(const ZZ9KImageSessionResult *result,
                                uint32_t buffered,
                                uint32_t staging_length,
                                int eof)
{
  if (stream_made_progress(result))
    return 0;
  return eof || buffered == staging_length;
}

static int feed_stream(FILE *file, const DemoInput *input,
                       ZZ9KSharedBuffer *staging, uint32_t session,
                       ZZ9KImageSessionResult *final_result)
{
  uint32_t file_offset = 0U;
  uint32_t buffered = 0U;
  uint32_t guard = 0U;
  uint32_t tiles = 0U;

  memset(final_result, 0, sizeof(*final_result));
  final_result->state = ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT;

  while (final_result->state != ZZ9K_IMAGE_SESSION_STATE_COMPLETE) {
    ZZ9KImageSessionFeedDesc feed;
    ZZ9KImageSessionResult result;
    uint32_t consumed;
    int eof;
    int status;

    if (!fill_staging(file, input, staging, &file_offset, &buffered))
      return 0;

    eof = file_offset == input->length;
    if (!zz9k_image_build_session_feed_desc(
            &feed, session, staging->handle, 0U, buffered,
            eof ? ZZ9K_IMAGE_SESSION_FEED_EOF : 0U)) {
      printf("could not build stream feed descriptor\n");
      return 0;
    }

    status = ZZ9KImageSessionFeed(&feed, &result);
    if (status != ZZ9K_STATUS_OK) {
      printf("stream feed failed: %s (%d)\n", zz9k_status_text(status),
             status);
      return 0;
    }
    if (result.bytes_consumed > buffered) {
      printf("stream consumed beyond staging buffer\n");
      return 0;
    }

    consumed = result.bytes_consumed;
    if (consumed != 0U) {
      buffered -= consumed;
      if (buffered != 0U &&
          !zz9k_shared_move(staging, 0U, consumed, buffered)) {
        printf("stream input compaction failed\n");
        return 0;
      }
    } else if (result.state == ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT ||
               result.state == ZZ9K_IMAGE_SESSION_STATE_HEADER_READY) {
      if (no_progress_is_fatal(&result, buffered, staging->length, eof)) {
        printf("stream made no progress (state=%lu buffered=%lu "
               "file=%lu/%lu eof=%d)\n",
               (unsigned long)result.state,
               (unsigned long)buffered,
               (unsigned long)file_offset,
               (unsigned long)input->length,
               eof);
        return 0;
      }
    }

    if (result.state == ZZ9K_IMAGE_SESSION_STATE_TILE_READY) {
      tiles++;
      printf("tile %lu: y=%lu rows=%lu bytes=%lu\n",
             (unsigned long)tiles,
             (unsigned long)result.tile_y,
             (unsigned long)result.tile_height,
             (unsigned long)result.bytes_written);
    } else if (result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT &&
               result.state != ZZ9K_IMAGE_SESSION_STATE_HEADER_READY &&
               result.state != ZZ9K_IMAGE_SESSION_STATE_COMPLETE) {
      printf("unexpected stream state %lu\n", (unsigned long)result.state);
      return 0;
    }

    *final_result = result;
    if (++guard > 100000UL) {
      printf("stream exceeded guard limit\n");
      return 0;
    }
  }

  return 1;
}

static int run_stream(const DemoInput *input)
{
  ZZ9KSharedBuffer staging;
  ZZ9KSharedBuffer tile;
  ZZ9KSurface framebuffer;
  ZZ9KImageSessionBeginDesc begin;
  ZZ9KImageSessionResult result;
  FILE *file;
  uint32_t session;
  uint32_t tile_rows;
  uint32_t tile_bytes;
  uint32_t output_format;
  uint32_t output_pitch;
  int staging_allocated;
  int tile_allocated;
  int session_open;
  int status;
  int ok;

  memset(&staging, 0, sizeof(staging));
  memset(&tile, 0, sizeof(tile));
  memset(&framebuffer, 0, sizeof(framebuffer));
  memset(&result, 0, sizeof(result));
  staging_allocated = 0;
  tile_allocated = 0;
  session_open = 0;
  session = 0U;
  ok = 0;
  output_format = zz9k_surface_native_rtg_format();

  printf("input: %s\n", input->path);
  printf("jpeg: %lu x %lu, %lu bytes\n",
         (unsigned long)input->width,
         (unsigned long)input->height,
         (unsigned long)input->length);

  status = ZZ9KAllocShared(DEMO_STAGING_BYTES, 16U, 0U, &staging);
  if (status != ZZ9K_STATUS_OK || !staging.data) {
    printf("staging allocation failed: %s (%d)\n",
           zz9k_status_text(status), status);
    goto out;
  }
  staging_allocated = 1;

  if (input->use_framebuffer) {
    ZZ9KRect framebuffer_rect;

    status = ZZ9KMapFramebufferSurface(&framebuffer);
    if (status != ZZ9K_STATUS_OK) {
      printf("framebuffer map failed: %s (%d)\n",
             zz9k_status_text(status), status);
      goto out;
    }
    if (!zz9k_surface_is_native_rtg_format(framebuffer.format) ||
        input->width > framebuffer.width ||
        input->height > framebuffer.height) {
      printf("JPEG does not fit native RTG framebuffer "
             "(%lu x %lu, format=%lu)\n",
             (unsigned long)framebuffer.width,
             (unsigned long)framebuffer.height,
             (unsigned long)framebuffer.format);
      goto out;
    }

    framebuffer_rect.x = 0U;
    framebuffer_rect.y = 0U;
    framebuffer_rect.w = input->width;
    framebuffer_rect.h = input->height;
    if (!zz9k_image_build_framebuffer_session_begin_desc(
            &begin, ZZ9K_IMAGE_CODEC_JPEG, &framebuffer_rect,
            output_format, 0U)) {
      printf("could not build framebuffer session descriptor\n");
      goto out;
    }
  } else {
    tile_rows = choose_tile_rows(input->width, output_format,
                                 input->tile_rows);
    if (tile_rows == 0U ||
        !zz9k_surface_min_pitch(input->width, output_format,
                                &output_pitch) ||
        tile_rows > (0xffffffffUL / output_pitch)) {
      printf("invalid tile geometry\n");
      goto out;
    }

    tile_bytes = output_pitch * tile_rows;
    status = ZZ9KAllocShared(tile_bytes, 16U, 0U, &tile);
    if (status != ZZ9K_STATUS_OK || !tile.data) {
      printf("tile allocation failed: %s (%d)\n", zz9k_status_text(status),
             status);
      goto out;
    }
    tile_allocated = 1;

    if (!zz9k_image_build_tile_session_begin_desc(
            &begin, ZZ9K_IMAGE_CODEC_JPEG, tile.handle,
            output_pitch, tile_rows, output_format, 0U)) {
      printf("could not build tile session descriptor\n");
      goto out;
    }
  }

  status = ZZ9KImageSessionBegin(&begin, &result);
  if (status != ZZ9K_STATUS_OK || result.session == 0U ||
      result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT) {
    printf("image session begin failed: %s (%d) state=%lu session=%lu\n",
           zz9k_status_text(status), status, (unsigned long)result.state,
           (unsigned long)result.session);
    goto out;
  }
  session = result.session;
  session_open = 1;

  file = fopen(input->path, "rb");
  if (!file) {
    printf("failed to reopen '%s'\n", input->path);
    goto out;
  }
  ok = feed_stream(file, input, &staging, session, &result);
  fclose(file);
  if (!ok)
    goto out;

  if (result.image_width != input->width ||
      result.image_height != input->height) {
    printf("unexpected decoded size %lu x %lu\n",
           (unsigned long)result.image_width,
           (unsigned long)result.image_height);
    ok = 0;
    goto out;
  }

  printf("stream complete: %lu x %lu, bytes=%lu%s\n",
         (unsigned long)result.image_width,
         (unsigned long)result.image_height,
         (unsigned long)result.bytes_written,
         input->use_framebuffer ? ", framebuffer" : ", tile buffer");
  ok = 1;

out:
  if (session_open)
    ZZ9KImageSessionClose(session, 0U);
  if (tile_allocated)
    ZZ9KFreeShared(tile.handle);
  if (staging_allocated)
    ZZ9KFreeShared(staging.handle);
  return ok;
}

int main(int argc, char **argv)
{
  DemoInput input;
  int rc;

  if (!parse_args(argc, argv, &input))
    return 1;

  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME,
                         ZZ9K_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    printf("OpenLibrary(%s, %u) failed\n",
           ZZ9K_LIBRARY_NAME, ZZ9K_LIBRARY_VERSION);
    return 1;
  }

  rc = 1;
  if (ZZ9KBase->lib_Revision < ZZ9K_LIBRARY_MIN_REVISION_IMAGE_SESSIONS) {
    printf("zz9k.library revision %u is too old for image sessions\n",
           ZZ9KBase->lib_Revision);
    goto out;
  }

  if (!require_image_session_support(input.use_framebuffer))
    goto out;
  if (!run_stream(&input))
    goto out;

  rc = 0;

out:
  CloseLibrary(ZZ9KBase);
  return rc;
}
