/*
 * Real zz9k.library one-shot PNG decode test for AmigaOS.
 *
 * Decodes a PNG file through ZZ9KDecodePng() into a CPU-visible surface,
 * decodes the same file through an image session into a second surface,
 * and prints both pixel checksums. The checksum is 32-bit FNV-1a over the
 * decoded rows, width * 4 bytes per row, in the surface format named in the
 * output. It also shows that PNG fit scaling and the GIF opcode are refused.
 *
 * Usage: zz9k-pngdecode FILE.png
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
#include <string.h>

struct Library *ZZ9KBase;

#define PNG_MAX_INPUT_BYTES (16UL * 1024UL * 1024UL)
#define PNG_READ_CHUNK_BYTES 4096UL
#define PNG_FEED_BYTES (64UL * 1024UL)

static int require_cap(uint32_t caps, uint32_t bit)
{
  const char *name;

  if ((caps & bit) != 0U) {
    return 1;
  }

  name = zz9k_capability_name(bit);
  printf("zz9k-pngdecode: missing required capability: %s\n",
         name ? name : "unknown");
  return 0;
}

static uint32_t be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t surface_checksum(const ZZ9KSurface *surface,
                                 uint32_t width, uint32_t height)
{
  const volatile uint8_t *base = (const volatile uint8_t *)surface->data;
  uint32_t hash = 2166136261UL;
  uint32_t x;
  uint32_t y;

  for (y = 0U; y < height; y++) {
    const volatile uint8_t *row = base + (y * surface->pitch);

    for (x = 0U; x < width * 4U; x++) {
      hash ^= row[x];
      hash *= 16777619UL;
    }
  }
  return hash;
}

/* Copies the file into the shared buffer; returns its length or 0. */
static uint32_t load_file(const char *path, ZZ9KSharedBuffer *buffer,
                          uint8_t *header, uint32_t header_bytes)
{
  uint8_t chunk[PNG_READ_CHUNK_BYTES];
  FILE *file = fopen(path, "rb");
  uint32_t total = 0U;
  size_t got;

  if (!file) {
    printf("zz9k-pngdecode: cannot open %s\n", path);
    return 0U;
  }
  while ((got = fread(chunk, 1U, sizeof(chunk), file)) != 0U) {
    if (total + got > buffer->length) {
      printf("zz9k-pngdecode: %s is larger than the input buffer\n", path);
      total = 0U;
      break;
    }
    if (total < header_bytes) {
      uint32_t keep = header_bytes - total;

      memcpy(header + total, chunk, got < keep ? got : keep);
    }
    if (!zz9k_shared_copy_to(buffer, total, chunk, (uint32_t)got)) {
      printf("zz9k-pngdecode: input copy failed\n");
      total = 0U;
      break;
    }
    total += (uint32_t)got;
  }
  fclose(file);
  return total;
}

static uint32_t file_length(const char *path)
{
  FILE *file = fopen(path, "rb");
  long length;

  if (!file) {
    return 0U;
  }
  if (fseek(file, 0L, SEEK_END) != 0) {
    fclose(file);
    return 0U;
  }
  length = ftell(file);
  fclose(file);
  if (length <= 0L || (unsigned long)length > PNG_MAX_INPUT_BYTES) {
    return 0U;
  }
  return (uint32_t)length;
}

static int decode_with_session(const ZZ9KSharedBuffer *input,
                               uint32_t input_length,
                               const ZZ9KSurface *surface,
                               uint32_t output_format)
{
  ZZ9KImageSessionBeginDesc begin;
  ZZ9KImageSessionFeedDesc feed;
  ZZ9KImageSessionResult result;
  ZZ9KRect target;
  uint32_t offset = 0U;
  int status;

  target.x = 0U;
  target.y = 0U;
  target.w = surface->width;
  target.h = surface->height;
  if (!zz9k_image_build_surface_session_begin_desc(
          &begin, ZZ9K_IMAGE_CODEC_PNG, surface->handle, &target,
          output_format, 0U)) {
    printf("could not build session begin descriptor\n");
    return 0;
  }
  memset(&result, 0, sizeof(result));
  status = ZZ9KImageSessionBegin(&begin, &result);
  if (status != ZZ9K_STATUS_OK) {
    printf("session begin failed: %s (%d)\n", zz9k_status_text(status),
           status);
    return 0;
  }

  for (;;) {
    uint32_t session = result.session;
    uint32_t chunk = input_length - offset;
    uint32_t flags;

    if (chunk > PNG_FEED_BYTES) {
      chunk = PNG_FEED_BYTES;
    }
    flags = (offset + chunk == input_length) ?
            ZZ9K_IMAGE_SESSION_FEED_EOF : 0U;
    if (!zz9k_image_build_session_feed_desc(&feed, session, input->handle,
                                            offset, chunk, flags)) {
      printf("could not build session feed descriptor\n");
      (void)ZZ9KImageSessionClose(session, 0U);
      return 0;
    }
    status = ZZ9KImageSessionFeed(&feed, &result);
    if (status != ZZ9K_STATUS_OK) {
      printf("session feed failed: %s (%d)\n", zz9k_status_text(status),
             status);
      (void)ZZ9KImageSessionClose(session, 0U);
      return 0;
    }
    offset += chunk;
    if (result.state == ZZ9K_IMAGE_SESSION_STATE_COMPLETE) {
      break;
    }
    if (offset >= input_length) {
      printf("session ended without a complete image\n");
      (void)ZZ9KImageSessionClose(session, 0U);
      return 0;
    }
  }
  (void)ZZ9KImageSessionClose(result.session, 0U);
  return 1;
}

int main(int argc, char **argv)
{
  ZZ9KCaps caps;
  ZZ9KServiceInfo image_service;
  ZZ9KSharedBuffer input;
  ZZ9KSurface one_shot;
  ZZ9KSurface session;
  ZZ9KImageDecodeDesc desc;
  ZZ9KImageDecodeResult result;
  ZZ9KRect dst_rect;
  uint8_t header[24];
  int input_allocated = 0;
  int one_shot_allocated = 0;
  int session_allocated = 0;
  int status;
  int rc = 1;
  uint32_t input_length;
  uint32_t width;
  uint32_t height;
  uint32_t output_format;
  uint32_t one_shot_sum;
  uint32_t session_sum;

  memset(&input, 0, sizeof(input));
  memset(&one_shot, 0, sizeof(one_shot));
  memset(&session, 0, sizeof(session));
  memset(header, 0, sizeof(header));

  if (argc != 2) {
    printf("usage: zz9k-pngdecode FILE.png\n");
    return 5;
  }
  input_length = file_length(argv[1]);
  if (input_length == 0U) {
    printf("zz9k-pngdecode: %s is missing, empty or over %lu bytes\n",
           argv[1], PNG_MAX_INPUT_BYTES);
    return 5;
  }

  printf("zz9k-pngdecode: opening zz9k.library\n");
  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME,
                         ZZ9K_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    printf("open failed\n");
    return 20;
  }

  printf("open ok base=0x%08lx version=%u revision=%u\n",
         (unsigned long)ZZ9KBase, ZZ9KBase->lib_Version,
         ZZ9KBase->lib_Revision);
  if (ZZ9KBase->lib_Revision <
      ZZ9K_LIBRARY_MIN_REVISION_TYPED_IMAGE_DECODE) {
    printf("zz9k.library revision too old for typed image decode LVOs\n");
    goto cleanup;
  }

  memset(&caps, 0, sizeof(caps));
  status = ZZ9KQueryCaps(&caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("query caps failed: %s (%d)\n", zz9k_status_text(status),
           status);
    goto cleanup;
  }
  if (!require_cap(caps.capability_bits, ZZ9K_CAP_SHARED_ALLOC) ||
      !require_cap(caps.capability_bits, ZZ9K_CAP_SURFACES) ||
      !require_cap(caps.capability_bits, ZZ9K_CAP_IMAGE_DECODE)) {
    goto cleanup;
  }

  memset(&image_service, 0, sizeof(image_service));
  status = ZZ9KQueryService(ZZ9K_SERVICE_IMAGE, &image_service);
  if (status != ZZ9K_STATUS_OK) {
    printf("image service query failed: %s (%d)\n",
           zz9k_status_text(status), status);
    goto cleanup;
  }
  printf("image service flags 0x%08lx, one-shot PNG %s\n",
         (unsigned long)image_service.flags,
         (image_service.flags & ZZ9K_SERVICE_FLAG_IMAGE_PNG_ONE_SHOT) != 0U ?
         "advertised" : "not advertised");
  if ((image_service.flags & ZZ9K_SERVICE_FLAG_IMAGE_PNG_ONE_SHOT) == 0U) {
    printf("this firmware has no one-shot PNG decode\n");
    rc = 10;
    goto cleanup;
  }

  status = ZZ9KAllocShared(input_length, 4U, 0U, &input);
  if (status != ZZ9K_STATUS_OK) {
    printf("input alloc failed: %s (%d)\n", zz9k_status_text(status),
           status);
    goto cleanup;
  }
  input_allocated = 1;
  if (load_file(argv[1], &input, header, sizeof(header)) != input_length) {
    goto cleanup;
  }
  if (memcmp(header, "\211PNG\r\n\032\n", 8) != 0 ||
      memcmp(header + 12, "IHDR", 4) != 0) {
    printf("zz9k-pngdecode: %s is not a PNG file\n", argv[1]);
    goto cleanup;
  }
  width = be32(header + 16);
  height = be32(header + 20);
  printf("file %s: %lu bytes, %lu x %lu\n", argv[1],
         (unsigned long)input_length, (unsigned long)width,
         (unsigned long)height);

  output_format = zz9k_surface_native_rtg_format();
  status = ZZ9KAllocSurface(width, height, output_format, 0U, &one_shot);
  if (status != ZZ9K_STATUS_OK || !one_shot.data) {
    printf("surface alloc failed: %s (%d)\n", zz9k_status_text(status),
           status);
    goto cleanup;
  }
  one_shot_allocated = 1;

  dst_rect.x = 0U;
  dst_rect.y = 0U;
  dst_rect.w = one_shot.width;
  dst_rect.h = one_shot.height;
  if (!zz9k_image_build_decode_desc(&desc, input.handle, 0U, input_length,
                                    one_shot.handle, &dst_rect,
                                    output_format, 0U)) {
    printf("could not build decode descriptor\n");
    goto cleanup;
  }

  memset(&result, 0, sizeof(result));
  status = ZZ9KDecodePng(&desc, &result);
  printf("ZZ9KDecodePng: %s (%d)\n", zz9k_status_text(status), status);
  if (status != ZZ9K_STATUS_OK) {
    goto cleanup;
  }
  printf("result %lu x %lu %s, %lu bytes written\n",
         (unsigned long)result.width, (unsigned long)result.height,
         zz9k_surface_format_text(result.output_format),
         (unsigned long)result.bytes_written);
  one_shot_sum = surface_checksum(&one_shot, result.width, result.height);
  printf("one-shot pixel checksum 0x%08lx\n", (unsigned long)one_shot_sum);

  status = ZZ9KAllocSurface(width, height, output_format, 0U, &session);
  if (status != ZZ9K_STATUS_OK || !session.data) {
    printf("session surface alloc failed: %s (%d)\n",
           zz9k_status_text(status), status);
    goto cleanup;
  }
  session_allocated = 1;
  if (!decode_with_session(&input, input_length, &session, output_format)) {
    goto cleanup;
  }
  session_sum = surface_checksum(&session, result.width, result.height);
  printf("session pixel checksum 0x%08lx\n", (unsigned long)session_sum);
  if (session_sum != one_shot_sum) {
    printf("MISMATCH: one-shot and session pixels differ\n");
    goto cleanup;
  }
  printf("one-shot and session pixels match\n");

  desc.flags = ZZ9K_IMAGE_DECODE_FLAG_FIT;
  status = ZZ9KDecodePng(&desc, &result);
  printf("ZZ9KDecodePng with FIT: %s (%d), expected UNSUPPORTED\n",
         zz9k_status_text(status), status);
  if (status != ZZ9K_STATUS_UNSUPPORTED) {
    goto cleanup;
  }
  desc.flags = 0U;
  status = ZZ9KDecodeGif(&desc, &result);
  printf("ZZ9KDecodeGif: %s (%d), expected UNSUPPORTED\n",
         zz9k_status_text(status), status);
  if (status != ZZ9K_STATUS_UNSUPPORTED) {
    goto cleanup;
  }

  printf("one-shot PNG decode ok\n");
  rc = 0;

cleanup:
  if (session_allocated) {
    ZZ9KFreeSurface(session.handle);
  }
  if (one_shot_allocated) {
    ZZ9KFreeSurface(one_shot.handle);
  }
  if (input_allocated) {
    ZZ9KFreeShared(input.handle);
  }
  if (ZZ9KBase) {
    CloseLibrary(ZZ9KBase);
  }
  return rc;
}
