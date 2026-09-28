/*
 * Public zz9k.library typed JPEG decode example.
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

struct Library *ZZ9KBase;

typedef struct DemoImage {
  const char *path;
  uint32_t length;
  uint32_t width;
  uint32_t height;
} DemoImage;

static uint16_t read_be16(const uint8_t *bytes)
{
  return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static int sof_marker(uint8_t marker)
{
  return marker >= 0xc0U && marker <= 0xcfU && marker != 0xc4U &&
         marker != 0xc8U && marker != 0xccU;
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

static int load_image_metadata(DemoImage *image)
{
  FILE *file;
  long length;
  int ok;

  file = fopen(image->path, "rb");
  if (!file) {
    printf("failed to open '%s'\n", image->path);
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
  if (!read_jpeg_dimensions(file, &image->width, &image->height))
    goto out;

  image->length = (uint32_t)length;
  ok = 1;

out:
  fclose(file);
  if (!ok)
    printf("could not read JPEG metadata from '%s'\n", image->path);
  return ok;
}

static int copy_file_to_shared(const DemoImage *image,
                               ZZ9KSharedBuffer *buffer)
{
  FILE *file;
  uint8_t scratch[4096];
  uint32_t copied;
  int ok;

  file = fopen(image->path, "rb");
  if (!file) {
    printf("failed to reopen '%s'\n", image->path);
    return 0;
  }

  ok = 1;
  copied = 0U;
  while (copied < image->length) {
    uint32_t remaining;
    size_t want;
    size_t got;

    remaining = image->length - copied;
    want = remaining < (uint32_t)sizeof(scratch) ?
           (size_t)remaining : sizeof(scratch);
    got = fread(scratch, 1U, want, file);
    if (got != want ||
        !zz9k_shared_copy_to(buffer, copied, scratch, (uint32_t)got)) {
      ok = 0;
      break;
    }
    copied += (uint32_t)got;
  }
  fclose(file);
  if (!ok)
    printf("short read from '%s'\n", image->path);
  return ok;
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

static int require_decode_support(void)
{
  ZZ9KCaps caps;
  uint32_t required;
  uint32_t missing;
  int status;

  memset(&caps, 0, sizeof(caps));
  status = ZZ9KQueryCaps(&caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("ZZ9KQueryCaps failed: %s (%d)\n", zz9k_status_text(status),
           status);
    return 0;
  }

  required = ZZ9K_CAP_SHARED_ALLOC | ZZ9K_CAP_SURFACES |
             ZZ9K_CAP_IMAGE_DECODE;
  if (zz9k_has_capabilities(caps.capability_bits, required))
    return 1;

  missing = zz9k_missing_capabilities(caps.capability_bits, required);
  print_missing_capabilities(missing);
  return 0;
}

static void usage(void)
{
  printf("usage: zz9k-typed-decode-demo file.jpg\n");
}

int main(int argc, char **argv)
{
  DemoImage image;
  ZZ9KSharedBuffer input;
  ZZ9KSurface surface;
  ZZ9KImageDecodeDesc desc;
  ZZ9KImageDecodeResult result;
  ZZ9KRect dst_rect;
  int input_allocated;
  int surface_allocated;
  int status;
  int rc;
  uint32_t output_format;
  uint32_t output_pitch;
  uint32_t expected_output_bytes;

  if (argc != 2) {
    usage();
    return 2;
  }

  memset(&image, 0, sizeof(image));
  image.path = argv[1];
  if (!load_image_metadata(&image)) {
    return 1;
  }

  memset(&input, 0, sizeof(input));
  memset(&surface, 0, sizeof(surface));
  input_allocated = 0;
  surface_allocated = 0;
  rc = 1;

  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME,
                         ZZ9K_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    printf("OpenLibrary(%s, %u) failed\n",
           ZZ9K_LIBRARY_NAME, ZZ9K_LIBRARY_VERSION);
    return 1;
  }

  if (ZZ9KBase->lib_Revision <
      ZZ9K_LIBRARY_MIN_REVISION_TYPED_IMAGE_DECODE) {
    printf("zz9k.library revision too old for typed image decode LVOs\n");
    goto out;
  }

  if (!require_decode_support()) {
    goto out;
  }

  status = ZZ9KAllocShared(image.length, 4U, 0U, &input);
  if (status != ZZ9K_STATUS_OK) {
    printf("input alloc failed: %s (%d)\n", zz9k_status_text(status),
           status);
    goto out;
  }
  input_allocated = 1;

  if (!copy_file_to_shared(&image, &input)) {
    goto out;
  }

  output_format = zz9k_surface_native_rtg_format();
  if (!zz9k_surface_layout(image.width, image.height, output_format,
                           &output_pitch, &expected_output_bytes)) {
    printf("unsupported or oversized native RTG output geometry\n");
    goto out;
  }
  status = ZZ9KAllocSurface(image.width, image.height, output_format,
                            ZZ9K_SURFACE_FLAG_ARM_LOCAL, &surface);
  if (status != ZZ9K_STATUS_OK) {
    printf("surface alloc failed: %s (%d)\n", zz9k_status_text(status),
           status);
    goto out;
  }
  surface_allocated = 1;

  dst_rect.x = 0U;
  dst_rect.y = 0U;
  dst_rect.w = surface.width;
  dst_rect.h = surface.height;
  if (!zz9k_image_build_decode_desc(&desc, input.handle, 0U,
                                    image.length, surface.handle,
                                    &dst_rect, output_format, 0U)) {
    printf("could not build typed JPEG decode descriptor\n");
    goto out;
  }

  memset(&result, 0, sizeof(result));
  status = ZZ9KDecodeJpeg(&desc, &result);
  if (status != ZZ9K_STATUS_OK) {
    printf("ZZ9KDecodeJpeg failed: %s (%d)\n", zz9k_status_text(status),
           status);
    goto out;
  }
  if (result.width != image.width || result.height != image.height ||
      result.output_format != output_format ||
      result.bytes_written != expected_output_bytes) {
    printf("unexpected decode result %lu x %lu format=%lu bytes=%lu\n",
           (unsigned long)result.width,
           (unsigned long)result.height,
           (unsigned long)result.output_format,
           (unsigned long)result.bytes_written);
    goto out;
  }

  printf("decoded '%s': %lu x %lu %s, %lu bytes on ARM surface\n",
         image.path,
         (unsigned long)result.width,
         (unsigned long)result.height,
         zz9k_surface_format_text(result.output_format),
         (unsigned long)result.bytes_written);
  rc = 0;

out:
  if (surface_allocated)
    ZZ9KFreeSurface(surface.handle);
  if (input_allocated)
    ZZ9KFreeShared(input.handle);
  CloseLibrary(ZZ9KBase);
  return rc;
}
