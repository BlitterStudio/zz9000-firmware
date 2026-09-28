/*
 * Tests for small public text helpers that are safe for library-only callers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/text.h"
#include <string.h>

static int expect_text(const char *actual, const char *expected)
{
  return strcmp(actual, expected) == 0;
}

static int test_status_text(void)
{
  if (!expect_text(zz9k_status_text(ZZ9K_STATUS_OK), "ok")) return 1;
  if (!expect_text(zz9k_status_text(ZZ9K_STATUS_BAD_REQUEST),
                   "bad-request")) return 2;
  if (!expect_text(zz9k_status_text(ZZ9K_STATUS_INTERNAL_ERROR),
                   "internal-error")) return 3;
  if (!expect_text(zz9k_status_text(12345), "error")) return 4;
  return 0;
}

static int test_surface_format_text(void)
{
  if (!expect_text(zz9k_surface_format_text(ZZ9K_SURFACE_FORMAT_BGRA8888),
                   "BGRA8888")) return 1;
  if (!expect_text(zz9k_surface_format_text(ZZ9K_SURFACE_FORMAT_ARGB8888),
                   "ARGB8888")) return 2;
  if (!expect_text(zz9k_surface_format_text(ZZ9K_SURFACE_FORMAT_RGB888),
                   "RGB888")) return 4;
  if (!expect_text(zz9k_surface_format_text(0x12345678UL),
                   "unknown")) return 3;
  return 0;
}

static int test_image_codec_text(void)
{
  if (!expect_text(zz9k_image_codec_text(ZZ9K_IMAGE_CODEC_JPEG),
                   "JPEG")) return 1;
  if (!expect_text(zz9k_image_codec_text(ZZ9K_IMAGE_CODEC_PNG),
                   "PNG")) return 2;
  if (!expect_text(zz9k_image_codec_text(ZZ9K_IMAGE_CODEC_GIF),
                   "GIF")) return 3;
  if (!expect_text(zz9k_image_codec_text(0xffffffffUL),
                   "unknown")) return 4;
  return 0;
}

static int test_compression_algorithm_text(void)
{
  if (!expect_text(zz9k_compression_algorithm_text(
                       ZZ9K_COMPRESSION_DEFLATE_RAW),
                   "deflate-raw")) return 1;
  if (!expect_text(zz9k_compression_algorithm_text(ZZ9K_COMPRESSION_GZIP),
                   "gzip")) return 2;
  if (!expect_text(zz9k_compression_algorithm_text(ZZ9K_COMPRESSION_LZMA2),
                   "lzma2")) return 3;
  if (!expect_text(zz9k_compression_algorithm_text(ZZ9K_COMPRESSION_LH1),
                   "lh1")) return 5;
  if (!expect_text(zz9k_compression_algorithm_text(ZZ9K_COMPRESSION_LH5),
                   "lh5")) return 6;
  if (!expect_text(zz9k_compression_algorithm_text(ZZ9K_COMPRESSION_LH6),
                   "lh6")) return 7;
  if (!expect_text(zz9k_compression_algorithm_text(ZZ9K_COMPRESSION_LH7),
                   "lh7")) return 8;
  if (!expect_text(zz9k_compression_algorithm_text(0xffffffffUL),
                   "unknown")) return 4;
  return 0;
}

static int test_service_text(void)
{
  if (!expect_text(zz9k_service_text(ZZ9K_SERVICE_CORE), "core")) return 1;
  if (!expect_text(zz9k_service_text(ZZ9K_SERVICE_IMAGE), "image")) return 2;
  if (!expect_text(zz9k_service_text(ZZ9K_SERVICE_CRYPTO), "crypto")) return 3;
  if (!expect_text(zz9k_service_text(ZZ9K_SERVICE_VIDEO), "video")) return 5;
  if (!expect_text(zz9k_service_text(0xffffffffUL), "unknown")) return 4;
  return 0;
}

static int test_known_service_iteration(void)
{
  if (zz9k_known_service_count() != 12U) return 1;
  if (zz9k_known_service_id(0) != ZZ9K_SERVICE_CORE) return 2;
  if (zz9k_known_service_id(4) != ZZ9K_SERVICE_IMAGE) return 3;
  if (zz9k_known_service_id(10) != ZZ9K_SERVICE_MODULE) return 4;
  if (zz9k_known_service_id(11) != ZZ9K_SERVICE_VIDEO) return 5;
  if (zz9k_known_service_id(12) != 0xffffffffUL) return 6;
  return 0;
}

int main(void)
{
  int result;

  result = test_status_text();
  if (result != 0) return 10 + result;
  result = test_surface_format_text();
  if (result != 0) return 20 + result;
  result = test_image_codec_text();
  if (result != 0) return 30 + result;
  result = test_compression_algorithm_text();
  if (result != 0) return 35 + result;
  result = test_service_text();
  if (result != 0) return 40 + result;
  result = test_known_service_iteration();
  if (result != 0) return 50 + result;
  return 0;
}
