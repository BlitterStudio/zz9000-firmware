/*
 * Host behavioral tests for the WebP image-stream session implementation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdk_decode_reclaim.h"
#include "sdk_image_stream.h"
#include "memorymap.h"
#include <sys/mman.h>

#include "webp_fixtures.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ZZ9K_WEBP_SMOKE
#include <webp/decode.h>
#endif

#define PAD_BYTE 0xa5U
#define SOURCE_HANDLE 0x40000002U
#define WEBP_MAX_INPUT_BYTES (8U * 1024U * 1024U)

void host_runtime_reset(void);
void host_runtime_fail_on_allocation(unsigned attempt);
unsigned host_runtime_allocation_attempts(void);
size_t host_runtime_allocated_bytes(void);
size_t host_runtime_peak_allocated_bytes(void);
void sdk_decode_heap_free(void *ptr);
static int host_map_session_region(void)
{
  void *region = mmap((void *)SDK_IMAGE_SESSIONS_ADDRESS,
                      SDK_IMAGE_SESSIONS_MAX_BYTES,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

  return region == MAP_FAILED ? -1 : 0;
}

int smp_cpu_id(void) { return 1; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t flags) { (void)flags; }

static void begin_webp(struct SDKImageStreamBegin *begin, uint8_t *output,
                       uint32_t width, uint32_t height, uint32_t stride,
                       uint32_t output_mode)
{
  memset(begin, 0, sizeof(*begin));
  begin->codec = SDK_IMAGE_CODEC_WEBP;
  begin->output_mode = output_mode;
  begin->dst_surface = output_mode == SDK_IMAGE_OUTPUT_FRAMEBUFFER ?
      SDK_SURFACE_HANDLE_FRAMEBUFFER : 0U;
  begin->dst_width = width;
  begin->dst_height = height;
  begin->output_format = SDK_SURFACE_FORMAT_BGRA8888;
  begin->tile_handle = 0x40000001U;
  begin->tile_stride = stride;
  begin->tile_rows = height;
  begin->tile_address = (uintptr_t)output;
  begin->tile_length = stride * height;
  begin->dst_address = (uintptr_t)output;
  begin->dst_pitch = stride;
  begin->dst_length = stride * height;
  begin->core1_affine = 1U;
  begin->direct_arm_local = output_mode == SDK_IMAGE_OUTPUT_FRAMEBUFFER;
}

#ifndef ZZ9K_WEBP_SMOKE

#define TEST_REQUIRE(condition, message) do { \
  if (!(condition)) { \
    fprintf(stderr, "%s\n", message); \
    return 0; \
  } \
} while (0)

static int all_padding(const uint8_t *actual, size_t length)
{
  size_t i;

  for (i = 0U; i < length; ++i) {
    if (actual[i] != PAD_BYTE)
      return 0;
  }
  return 1;
}

static int pixels_and_padding_match(const uint8_t *actual, uint32_t width,
                                    uint32_t height, uint32_t stride,
                                    const uint8_t *expected)
{
  uint32_t y;

  for (y = 0U; y < height; ++y) {
    if (memcmp(actual + y * stride, expected + y * width * 4U,
               width * 4U) != 0)
      return 0;
    if (!all_padding(actual + y * stride + width * 4U,
                     stride - width * 4U))
      return 0;
  }
  return 1;
}

static void begin_feed(struct SDKImageStreamFeed *feed, uint32_t session,
                       uint32_t length, uint32_t flags)
{
  memset(feed, 0, sizeof(*feed));
  feed->session = session;
  feed->src_handle = SOURCE_HANDLE;
  feed->src_length = length;
  feed->flags = flags;
}

static int close_clean(uint32_t session)
{
  TEST_REQUIRE(sdk_image_stream_close(session) == SDK_STATUS_OK,
               "WebP session close failed");
  TEST_REQUIRE(sdk_image_stream_active_count() == 0U,
               "WebP session remained active after close");
  TEST_REQUIRE(sdk_decode_tracked_count() == 0U,
               "WebP close left tracked allocations");
  return 1;
}

static int decode_fragmented(const uint8_t *source, size_t source_length,
                             const uint8_t *expected, uint32_t width,
                             uint32_t height, uint32_t output_mode)
{
  static const uint8_t pieces[] = { 1U, 2U, 7U, 3U, 19U, 5U };
  struct SDKImageStreamBegin begin;
  struct SDKImageStreamFeed feed;
  struct SDKImageStreamResult result;
  uint8_t tile[32U];
  uint8_t output[6U * 4U * 32U];
  uint8_t accumulated[6U * 4U * 4U];
  const uint32_t stride = 32U;
  uint32_t session;
  uint32_t row = 0U;
  size_t offset = 0U;
  unsigned piece = 0U;

  memset(tile, PAD_BYTE, sizeof(tile));
  memset(output, PAD_BYTE, sizeof(output));
  memset(accumulated, 0, sizeof(accumulated));
  begin_webp(&begin,
             output_mode == SDK_IMAGE_OUTPUT_TILE_BUFFER ? tile : output,
             width, height, stride, output_mode);
  begin.tile_rows = output_mode == SDK_IMAGE_OUTPUT_TILE_BUFFER ? 1U : height;
  TEST_REQUIRE(sdk_image_stream_begin(&begin, &result) == SDK_STATUS_OK &&
               result.session != 0U, "WebP fragmented begin failed");
  session = result.session;

  while (offset < source_length) {
    const uint8_t *destination;
    size_t destination_length;
    size_t count = pieces[piece++ % (sizeof(pieces) / sizeof(pieces[0]))];

    if (count > source_length - offset)
      count = source_length - offset;
    begin_feed(&feed, session, (uint32_t)count,
               offset + count == source_length ? SDK_IMAGE_SESSION_FEED_EOF : 0U);
    feed.src_offset = (uint32_t)offset;
    TEST_REQUIRE(sdk_image_stream_feed(&feed, source + offset, &result) ==
                 SDK_STATUS_OK, "WebP fragmented feed failed");
    offset += count;
    if (feed.flags != 0U) {
      TEST_REQUIRE(result.bytes_consumed == count,
                   "WebP EOF did not consume every accepted byte");
      break;
    }

    destination = output_mode == SDK_IMAGE_OUTPUT_TILE_BUFFER ? tile : output;
    destination_length = output_mode == SDK_IMAGE_OUTPUT_TILE_BUFFER ?
        sizeof(tile) : sizeof(output);
    TEST_REQUIRE(result.state == SDK_IMAGE_SESSION_STATE_NEED_INPUT &&
                 result.bytes_consumed == count && result.bytes_written == 0U &&
                 result.flush_length == 0U,
                 "WebP pre-EOF feed reported progress or partial consumption");
    TEST_REQUIRE(all_padding(destination, destination_length),
                 "WebP wrote output before validated EOF");
  }

  if (output_mode == SDK_IMAGE_OUTPUT_FRAMEBUFFER) {
    TEST_REQUIRE(result.state == SDK_IMAGE_SESSION_STATE_COMPLETE &&
                 result.bytes_written == width * height * 4U &&
                 result.flush_address == (uintptr_t)output &&
                 result.flush_length == width * 4U + (height - 1U) * stride,
                 "WebP framebuffer completion metadata is wrong");
    TEST_REQUIRE(pixels_and_padding_match(output, width, height, stride, expected),
                 "WebP framebuffer pixels differ from the golden image");
  } else {
    while (row < height) {
      TEST_REQUIRE(result.state == SDK_IMAGE_SESSION_STATE_TILE_READY &&
                   result.tile_y == row && result.tile_width == width &&
                   result.tile_height == 1U && result.bytes_written == width * 4U &&
                   result.flush_address == (uintptr_t)tile &&
                   result.flush_length == width * 4U,
                   "WebP tile metadata is wrong");
      TEST_REQUIRE((row + 1U < height) ==
                   ((result.flags & SDK_IMAGE_SESSION_RESULT_PARTIAL) != 0U),
                   "WebP tile partial flag is wrong");
      TEST_REQUIRE(all_padding(tile + width * 4U, stride - width * 4U),
                   "WebP tile row overwrote padding");
      memcpy(accumulated + row * width * 4U, tile, width * 4U);
      ++row;
      if (row == height)
        break;
      begin_feed(&feed, session, 0U, SDK_IMAGE_SESSION_FEED_EOF);
      TEST_REQUIRE(sdk_image_stream_feed(&feed, 0, &result) == SDK_STATUS_OK &&
                   result.bytes_consumed == 0U,
                   "WebP tile drain failed");
    }
    TEST_REQUIRE(memcmp(accumulated, expected, width * height * 4U) == 0,
                 "Reused WebP tile buffer did not accumulate to the golden image");
    begin_feed(&feed, session, 0U, SDK_IMAGE_SESSION_FEED_EOF);
    TEST_REQUIRE(sdk_image_stream_feed(&feed, 0, &result) == SDK_STATUS_OK &&
                 result.state == SDK_IMAGE_SESSION_STATE_COMPLETE &&
                 result.bytes_consumed == 0U,
                 "WebP final tile retirement failed");
  }

  begin_feed(&feed, session, 0U, SDK_IMAGE_SESSION_FEED_EOF);
  TEST_REQUIRE(sdk_image_stream_feed(&feed, 0, &result) == SDK_STATUS_OK &&
               result.state == SDK_IMAGE_SESSION_STATE_COMPLETE &&
               result.bytes_consumed == 0U && result.bytes_written == 0U &&
               result.flush_length == 0U,
               "WebP completed session did not remain quiescent");
  begin_feed(&feed, session, 1U, 0U);
  TEST_REQUIRE(sdk_image_stream_feed(&feed, source, &result) != SDK_STATUS_OK,
               "WebP accepted input after completion");
  return close_clean(session);
}

static int test_webp_fixtures(void)
{
  TEST_REQUIRE(decode_fragmented(webp_lossy_3x5, sizeof(webp_lossy_3x5),
                                 webp_lossy_3x5_bgra, 3U, 5U,
                                 SDK_IMAGE_OUTPUT_TILE_BUFFER),
               "Lossy fragmented WebP fixture failed");
  TEST_REQUIRE(decode_fragmented(webp_alpha_3x4, sizeof(webp_alpha_3x4),
                                 webp_alpha_3x4_bgra, 3U, 4U,
                                 SDK_IMAGE_OUTPUT_FRAMEBUFFER),
               "Lossless alpha WebP fixture failed");
  TEST_REQUIRE(decode_fragmented(webp_animation_offset_6x4,
                                 sizeof(webp_animation_offset_6x4),
                                 webp_animation_offset_6x4_bgra, 6U, 4U,
                                 SDK_IMAGE_OUTPUT_TILE_BUFFER),
               "Animated WebP first canvas fixture failed");
  return 1;
}

static uint32_t bytes_per_pixel(uint32_t format)
{
  return format == SDK_SURFACE_FORMAT_RGB888 ? 3U : 4U;
}

static int output_matches_format(const uint8_t *output, uint32_t width,
                                 uint32_t height, uint32_t stride,
                                 uint32_t format, const uint8_t *bgra)
{
  uint32_t x;
  uint32_t y;
  const uint32_t bpp = bytes_per_pixel(format);

  for (y = 0U; y < height; ++y) {
    for (x = 0U; x < width; ++x) {
      const uint8_t *source = bgra + (y * width + x) * 4U;
      const uint8_t *pixel = output + y * stride + x * bpp;

      if ((format == SDK_SURFACE_FORMAT_BGRA8888 &&
           memcmp(pixel, source, 4U) != 0) ||
          (format == SDK_SURFACE_FORMAT_RGBA8888 &&
           (pixel[0] != source[2] || pixel[1] != source[1] ||
            pixel[2] != source[0] || pixel[3] != source[3])) ||
          (format == SDK_SURFACE_FORMAT_ARGB8888 &&
           (pixel[0] != source[3] || pixel[1] != source[2] ||
            pixel[2] != source[1] || pixel[3] != source[0])) ||
          (format == SDK_SURFACE_FORMAT_RGB888 &&
           (pixel[0] != source[2] || pixel[1] != source[1] ||
            pixel[2] != source[0])))
        return 0;
    }
    if (!all_padding(output + y * stride + width * bpp, stride - width * bpp))
      return 0;
  }
  return 1;
}

static int test_output_formats(void)
{
  static const uint32_t formats[] = {
    SDK_SURFACE_FORMAT_BGRA8888,
    SDK_SURFACE_FORMAT_RGBA8888,
    SDK_SURFACE_FORMAT_ARGB8888,
    SDK_SURFACE_FORMAT_RGB888
  };
  uint8_t output[128U];
  unsigned i;

  for (i = 0U; i < sizeof(formats) / sizeof(formats[0]); ++i) {
    struct SDKImageStreamBegin begin;
    struct SDKImageStreamFeed feed;
    struct SDKImageStreamResult result;
    const uint32_t bpp = bytes_per_pixel(formats[i]);
    const uint32_t stride = 3U * bpp + 5U;

    memset(output, PAD_BYTE, sizeof(output));
    begin_webp(&begin, output, 3U, 4U, stride, SDK_IMAGE_OUTPUT_FRAMEBUFFER);
    begin.output_format = formats[i];
    begin.tile_length = stride * 4U;
    begin.dst_length = stride * 4U;
    TEST_REQUIRE(sdk_image_stream_begin(&begin, &result) == SDK_STATUS_OK,
                 "WebP output-format begin failed");
    begin_feed(&feed, result.session, sizeof(webp_alpha_3x4),
               SDK_IMAGE_SESSION_FEED_EOF);
    TEST_REQUIRE(sdk_image_stream_feed(&feed, webp_alpha_3x4, &result) ==
                 SDK_STATUS_OK && result.state == SDK_IMAGE_SESSION_STATE_COMPLETE &&
                 result.bytes_written == 3U * 4U * bpp,
                 "WebP output-format decode failed");
    TEST_REQUIRE(output_matches_format(output, 3U, 4U, stride, formats[i],
                                       webp_alpha_3x4_bgra),
                 "WebP output format pixels differ from the golden image");
    TEST_REQUIRE(close_clean(feed.session), "WebP output-format close failed");
  }
  return 1;
}

static int reject_input(const uint8_t *source, size_t source_length,
                        const char *name)
{
  struct SDKImageStreamBegin begin;
  struct SDKImageStreamFeed feed;
  struct SDKImageStreamResult result;
  uint8_t output[128U];
  uint16_t status;

  memset(output, PAD_BYTE, sizeof(output));
  begin_webp(&begin, output, 3U, 1U, 32U, SDK_IMAGE_OUTPUT_TILE_BUFFER);
  TEST_REQUIRE(sdk_image_stream_begin(&begin, &result) == SDK_STATUS_OK,
               "WebP rejection begin failed");
  begin_feed(&feed, result.session, (uint32_t)source_length,
             SDK_IMAGE_SESSION_FEED_EOF);
  status = sdk_image_stream_feed(&feed, source, &result);
  if (status == SDK_STATUS_OK && result.state != SDK_IMAGE_SESSION_STATE_ERROR) {
    fprintf(stderr, "%s was accepted\n", name);
    (void)sdk_image_stream_close(feed.session);
    return 0;
  }
  if (!all_padding(output, sizeof(output)) || result.bytes_written != 0U ||
      result.flush_length != 0U) {
    fprintf(stderr, "%s produced partial output\n", name);
    (void)sdk_image_stream_close(feed.session);
    return 0;
  }
  return close_clean(feed.session);
}

static void write_le32(uint8_t *destination, uint32_t value)
{
  destination[0] = (uint8_t)value;
  destination[1] = (uint8_t)(value >> 8);
  destination[2] = (uint8_t)(value >> 16);
  destination[3] = (uint8_t)(value >> 24);
}

static size_t make_metadata_before_image(uint8_t *destination,
                                         size_t destination_size,
                                         const char metadata_type[4],
                                         uint8_t metadata_flag)
{
  static const uint8_t vp8x_payload[] = {
    0U, 0U, 0U, 0U, 2U, 0U, 0U, 4U, 0U, 0U
  };
  size_t offset = 12U;
  const size_t image_size = sizeof(webp_lossy_3x5) - 12U;

  if (destination_size < sizeof(webp_lossy_3x5) + 28U)
    return 0U;
  memcpy(destination, "RIFF", 4U);
  memcpy(destination + 8U, "WEBP", 4U);
  memcpy(destination + offset, "VP8X", 4U);
  write_le32(destination + offset + 4U, sizeof(vp8x_payload));
  memcpy(destination + offset + 8U, vp8x_payload, sizeof(vp8x_payload));
  destination[offset + 8U] = metadata_flag;
  offset += 8U + sizeof(vp8x_payload);
  memcpy(destination + offset, metadata_type, 4U);
  write_le32(destination + offset + 4U, 1U);
  destination[offset + 8U] = 0U;
  destination[offset + 9U] = 0U;
  offset += 10U;
  memcpy(destination + offset, webp_lossy_3x5 + 12U, image_size);
  offset += image_size;
  write_le32(destination + 4U, (uint32_t)(offset - 8U));
  return offset;
}

static size_t make_geometry_only_animation(uint8_t *destination,
                                           size_t destination_size,
                                           int first_frame)
{
  const size_t first_anmf = 44U;
  const size_t second_anmf = 92U;
  const size_t anmf_header = 8U;
  const size_t frame_header = 16U;
  size_t offset;

  if (destination_size < sizeof(webp_animation_offset_6x4) - 24U)
    return 0U;
  if (first_frame) {
    memcpy(destination, webp_animation_offset_6x4, first_anmf);
    offset = first_anmf;
    memcpy(destination + offset, "ANMF", 4U);
    write_le32(destination + offset + 4U, (uint32_t)frame_header);
    memcpy(destination + offset + anmf_header,
           webp_animation_offset_6x4 + first_anmf + anmf_header,
           frame_header);
    offset += anmf_header + frame_header;
    memcpy(destination + offset, webp_animation_offset_6x4 + second_anmf,
           sizeof(webp_animation_offset_6x4) - second_anmf);
    offset += sizeof(webp_animation_offset_6x4) - second_anmf;
  } else {
    memcpy(destination, webp_animation_offset_6x4, second_anmf);
    offset = second_anmf;
    memcpy(destination + offset, "ANMF", 4U);
    write_le32(destination + offset + 4U, (uint32_t)frame_header);
    memcpy(destination + offset + anmf_header,
           webp_animation_offset_6x4 + second_anmf + anmf_header,
           frame_header);
    offset += anmf_header + frame_header;
  }
  write_le32(destination + 4U, (uint32_t)(offset - 8U));
  return offset;
}

static size_t make_unknown_chunk_animation(uint8_t *destination,
                                           size_t destination_size)
{
  const size_t first_anmf = 44U;
  const size_t second_anmf = 92U;
  const size_t anmf_header = 8U;
  const size_t frame_header = 16U;
  const size_t image_chunk = second_anmf -
      (first_anmf + anmf_header + frame_header);
  size_t offset;

  if (destination_size < sizeof(webp_animation_offset_6x4) + 10U)
    return 0U;
  memcpy(destination, webp_animation_offset_6x4, first_anmf);
  offset = first_anmf;
  memcpy(destination + offset, "ANMF", 4U);
  write_le32(destination + offset + 4U,
             (uint32_t)(frame_header + 10U + image_chunk));
  offset += anmf_header;
  memcpy(destination + offset, webp_animation_offset_6x4 + first_anmf + anmf_header,
         frame_header);
  offset += frame_header;
  memcpy(destination + offset,
         webp_animation_offset_6x4 + first_anmf + anmf_header + frame_header,
         image_chunk);
  offset += image_chunk;
  memcpy(destination + offset, "JUNK", 4U);
  write_le32(destination + offset + 4U, 1U);
  destination[offset + 8U] = 0U;
  destination[offset + 9U] = 0U;
  offset += 10U;
  memcpy(destination + offset, webp_animation_offset_6x4 + second_anmf,
         sizeof(webp_animation_offset_6x4) - second_anmf);
  offset += sizeof(webp_animation_offset_6x4) - second_anmf;
  write_le32(destination + 4U, (uint32_t)(offset - 8U));
  return offset;
}

static int test_unknown_anmf_chunk(void)
{
  uint8_t source[sizeof(webp_animation_offset_6x4) + 10U];
  size_t source_length = make_unknown_chunk_animation(source, sizeof(source));

  TEST_REQUIRE(source_length != 0U &&
               decode_fragmented(source, source_length,
                                 webp_animation_offset_6x4_bgra, 6U, 4U,
                                 SDK_IMAGE_OUTPUT_TILE_BUFFER),
               "Well-framed unknown ANMF chunk changed the first canvas");
  return 1;
}

static int test_metadata_before_image(void)
{
  uint8_t source[sizeof(webp_lossy_3x5) + 28U];
  size_t source_length;

  source_length = make_metadata_before_image(source, sizeof(source), "EXIF", 8U);
  TEST_REQUIRE(source_length != 0U &&
               decode_fragmented(source, source_length, webp_lossy_3x5_bgra,
                                 3U, 5U, SDK_IMAGE_OUTPUT_FRAMEBUFFER),
               "EXIF before WebP image data was not accepted");
  source_length = make_metadata_before_image(source, sizeof(source), "XMP ", 4U);
  TEST_REQUIRE(source_length != 0U &&
               decode_fragmented(source, source_length, webp_lossy_3x5_bgra,
                                 3U, 5U, SDK_IMAGE_OUTPUT_FRAMEBUFFER),
               "XMP before WebP image data was not accepted");
  return 1;
}

static int test_rejects_image_less_animation_frames(void)
{
  uint8_t source[sizeof(webp_animation_offset_6x4)];
  size_t source_length;

  source_length = make_geometry_only_animation(source, sizeof(source), 1);
  TEST_REQUIRE(source_length != 0U &&
               reject_input(source, source_length, "image-less first animation frame"),
               "Image-less first animation frame was accepted");
  source_length = make_geometry_only_animation(source, sizeof(source), 0);
  TEST_REQUIRE(source_length != 0U &&
               reject_input(source, source_length, "image-less later animation frame"),
               "Image-less later animation frame was accepted");
  return 1;
}

static int test_arm_local_requires_full_canvas(void)
{
  struct SDKImageStreamBegin begin;
  struct SDKImageStreamFeed feed;
  struct SDKImageStreamResult result;
  uint8_t output[4U * 4U * 4U];
  uintptr_t address = 0U;
  uint32_t length = 0U;
  uint32_t session;

  memset(output, PAD_BYTE, sizeof(output));
  begin_webp(&begin, output, 4U, 4U, 16U, SDK_IMAGE_OUTPUT_FRAMEBUFFER);
  TEST_REQUIRE(sdk_image_stream_begin(&begin, &result) == SDK_STATUS_OK,
               "ARM-local full-canvas begin failed");
  session = result.session;
  begin_feed(&feed, session, sizeof(webp_alpha_3x4), SDK_IMAGE_SESSION_FEED_EOF);
  TEST_REQUIRE(sdk_image_stream_feed(&feed, webp_alpha_3x4, &result) ==
               SDK_STATUS_BAD_REQUEST && result.state == SDK_IMAGE_SESSION_STATE_ERROR &&
               result.bytes_written == 0U && result.flush_length == 0U &&
               all_padding(output, sizeof(output)),
               "ARM-local smaller WebP canvas was published into a larger destination");
  TEST_REQUIRE(!sdk_image_stream_complete_arm_local_output(session, &address, &length),
               "ARM-local smaller WebP canvas exposed a full-surface handoff");
  return close_clean(session);
}

static int test_rejections_and_bounds(void)
{
  static const uint8_t malformed[] = { 'R', 'I', 'F', 'F', 4U, 0U, 0U, 0U,
                                       'W', 'E', 'B', 'P' };
  uint8_t corrupt_frame[sizeof(webp_alpha_3x4)];
  uint8_t malformed_chunk[sizeof(webp_alpha_3x4)];
  uint8_t out_of_bounds_frame[sizeof(webp_animation_offset_6x4)];
  uint8_t over_dimension[sizeof(webp_lossy_3x5)];
  uint8_t oversized_header[12U];
  uint8_t *exact_limit;

  TEST_REQUIRE(reject_input(malformed, sizeof(malformed), "truncated RIFF"),
               "Truncated RIFF rejection failed");
  TEST_REQUIRE(reject_input(webp_lossy_3x5, sizeof(webp_lossy_3x5) - 1U,
                            "truncated image"),
               "Truncated image rejection failed");

  memcpy(corrupt_frame, webp_alpha_3x4, sizeof(corrupt_frame));
  corrupt_frame[20U] = 0U;
  TEST_REQUIRE(reject_input(corrupt_frame, sizeof(corrupt_frame),
                            "corrupt VP8L frame"),
               "Corrupt frame rejection failed");

  memcpy(malformed_chunk, webp_alpha_3x4, sizeof(malformed_chunk));
  write_le32(malformed_chunk + 16U, 0xffffffffU);
  TEST_REQUIRE(reject_input(malformed_chunk, sizeof(malformed_chunk),
                            "overflowing chunk"),
               "Chunk-bound rejection failed");

  memcpy(out_of_bounds_frame, webp_animation_offset_6x4,
         sizeof(out_of_bounds_frame));
  out_of_bounds_frame[52U] = 3U;
  TEST_REQUIRE(reject_input(out_of_bounds_frame, sizeof(out_of_bounds_frame),
                            "out-of-canvas animation frame"),
               "Animation frame-bound rejection failed");

  memcpy(over_dimension, webp_lossy_3x5, sizeof(over_dimension));
  over_dimension[26U] = 1U;
  over_dimension[27U] = 0x20U;
  TEST_REQUIRE(reject_input(over_dimension, sizeof(over_dimension),
                            "over-dimension VP8 geometry"),
               "Valid VP8 geometry limit rejection failed");

  memset(oversized_header, 0, sizeof(oversized_header));
  memcpy(oversized_header, "RIFF", 4U);
  write_le32(oversized_header + 4U, WEBP_MAX_INPUT_BYTES - 7U);
  memcpy(oversized_header + 8U, "WEBP", 4U);
  TEST_REQUIRE(reject_input(oversized_header, sizeof(oversized_header),
                            "input one byte over the limit"),
               "Input limit rejection failed");

  exact_limit = (uint8_t *)calloc(WEBP_MAX_INPUT_BYTES, 1U);
  TEST_REQUIRE(exact_limit != 0, "Could not allocate exact-limit test input");
  memcpy(exact_limit, "RIFF", 4U);
  write_le32(exact_limit + 4U, WEBP_MAX_INPUT_BYTES - 8U);
  memcpy(exact_limit + 8U, "WEBP", 4U);
  memcpy(exact_limit + 12U, "JUNK", 4U);
  write_le32(exact_limit + 16U, WEBP_MAX_INPUT_BYTES - 20U);
  TEST_REQUIRE(reject_input(exact_limit, WEBP_MAX_INPUT_BYTES,
                            "exact input limit"),
               "Exact input-limit transport failed");
  free(exact_limit);
  return 1;
}

static int test_session_limit_and_fit(void)
{
  struct SDKImageStreamBegin begin;
  struct SDKImageStreamResult result;
  uint8_t output[128U];
  uint32_t session;

  begin_webp(&begin, output, 3U, 1U, 32U, SDK_IMAGE_OUTPUT_TILE_BUFFER);
  TEST_REQUIRE(sdk_image_stream_begin(&begin, &result) == SDK_STATUS_OK,
               "WebP initial session allocation failed");
  session = result.session;
  TEST_REQUIRE(sdk_image_stream_begin(&begin, &result) == SDK_STATUS_NO_MEMORY,
               "WebP session limit was not enforced");
  TEST_REQUIRE(close_clean(session), "WebP session-limit close failed");

  begin.flags = SDK_IMAGE_DECODE_FLAG_FIT;
  TEST_REQUIRE(sdk_image_stream_begin(&begin, &result) == SDK_STATUS_UNSUPPORTED,
               "WebP FIT mode was not rejected");
  return 1;
}

static int test_reset_reclaims_retained_animation(void)
{
  struct SDKImageStreamBegin begin;
  struct SDKImageStreamFeed feed;
  struct SDKImageStreamResult result;
  uint8_t tile[32U];
  uint32_t session;
  unsigned reclaimed;

  host_runtime_reset();
  memset(tile, PAD_BYTE, sizeof(tile));
  begin_webp(&begin, tile, 6U, 4U, 32U, SDK_IMAGE_OUTPUT_TILE_BUFFER);
  begin.tile_rows = 1U;
  TEST_REQUIRE(sdk_image_stream_begin(&begin, &result) == SDK_STATUS_OK,
               "Reset test begin failed");
  session = result.session;
  begin_feed(&feed, session, sizeof(webp_animation_offset_6x4),
             SDK_IMAGE_SESSION_FEED_EOF);
  TEST_REQUIRE(sdk_image_stream_feed(&feed, webp_animation_offset_6x4, &result) ==
               SDK_STATUS_OK && result.state == SDK_IMAGE_SESSION_STATE_TILE_READY,
               "Animated input did not retain first canvas");
  TEST_REQUIRE(sdk_decode_tracked_count() > 0U &&
               host_runtime_allocated_bytes() > 0U,
               "Retained animation did not own tracked input and canvas allocations");

  sdk_image_stream_poison_core1_sessions();
  reclaimed = sdk_decode_reclaim(free);
  TEST_REQUIRE(reclaimed > 0U && sdk_decode_tracked_count() == 0U,
               "Reset did not reclaim every retained WebP allocation");
  TEST_REQUIRE(sdk_image_stream_close(session) == SDK_STATUS_OK,
               "Poisoned WebP session close failed");
  TEST_REQUIRE(sdk_image_stream_active_count() == 0U,
               "Poisoned WebP session remained active after close");
  host_runtime_reset();
  return 1;
}

static int test_reset_reclaims_pre_eof_input(void)
{
  struct SDKImageStreamBegin begin;
  struct SDKImageStreamFeed feed;
  struct SDKImageStreamResult result;
  uint8_t tile[32U];
  uint8_t recovered[3U * 4U * 4U];
  uint32_t session;
  unsigned reclaimed;

  host_runtime_reset();
  memset(tile, PAD_BYTE, sizeof(tile));
  begin_webp(&begin, tile, 3U, 1U, 32U, SDK_IMAGE_OUTPUT_TILE_BUFFER);
  TEST_REQUIRE(sdk_image_stream_begin(&begin, &result) == SDK_STATUS_OK,
               "Pre-EOF reset begin failed");
  session = result.session;
  begin_feed(&feed, session, 12U, 0U);
  TEST_REQUIRE(sdk_image_stream_feed(&feed, webp_alpha_3x4, &result) ==
               SDK_STATUS_OK && result.state == SDK_IMAGE_SESSION_STATE_NEED_INPUT &&
               result.bytes_consumed == 12U && result.bytes_written == 0U &&
               result.flush_length == 0U && sdk_decode_tracked_count() > 0U &&
               host_runtime_allocated_bytes() > 0U,
               "Pre-EOF WebP input was not retained without output");

  sdk_image_stream_poison_core1_sessions();
  reclaimed = sdk_decode_reclaim(sdk_decode_heap_free);
  TEST_REQUIRE(reclaimed > 0U && sdk_decode_tracked_count() == 0U &&
               host_runtime_allocated_bytes() == 0U,
               "Pre-EOF reset did not reclaim every input-only WebP allocation");
  TEST_REQUIRE(sdk_image_stream_close(session) == SDK_STATUS_OK &&
               sdk_image_stream_active_count() == 0U,
               "Pre-EOF poisoned WebP session remained live after close");

  memset(recovered, PAD_BYTE, sizeof(recovered));
  begin_webp(&begin, recovered, 3U, 4U, 12U, SDK_IMAGE_OUTPUT_FRAMEBUFFER);
  TEST_REQUIRE(sdk_image_stream_begin(&begin, &result) == SDK_STATUS_OK,
               "Pre-EOF reset did not restore the WebP decode budget");
  begin_feed(&feed, result.session, sizeof(webp_alpha_3x4), SDK_IMAGE_SESSION_FEED_EOF);
  TEST_REQUIRE(sdk_image_stream_feed(&feed, webp_alpha_3x4, &result) ==
               SDK_STATUS_OK && result.state == SDK_IMAGE_SESSION_STATE_COMPLETE,
               "Pre-EOF reset did not permit a reusable WebP decode");
  TEST_REQUIRE(close_clean(feed.session) && host_runtime_allocated_bytes() == 0U,
               "Reusable WebP decode leaked after pre-EOF reset");
  host_runtime_reset();
  return 1;
}

static int test_tracker_exhaustion(void)
{
  struct SDKImageStreamBegin begin;
  struct SDKImageStreamFeed feed;
  struct SDKImageStreamResult result;
  uint8_t output[32U];
  void *dummies[SDK_DECODE_MAX_TRACKED];
  uint32_t session;
  unsigned i;

  for (i = 0U; i < SDK_DECODE_MAX_TRACKED; ++i) {
    dummies[i] = malloc(1U);
    TEST_REQUIRE(dummies[i] != 0, "Could not allocate tracker-exhaustion dummy");
    sdk_decode_track(dummies[i]);
  }
  TEST_REQUIRE(sdk_decode_tracked_count() == SDK_DECODE_MAX_TRACKED,
               "Tracker-exhaustion dummies did not fill every slot");

  memset(output, PAD_BYTE, sizeof(output));
  begin_webp(&begin, output, 3U, 1U, 32U, SDK_IMAGE_OUTPUT_TILE_BUFFER);
  TEST_REQUIRE(sdk_image_stream_begin(&begin, &result) == SDK_STATUS_OK,
               "Tracker-exhaustion begin failed");
  session = result.session;
  begin_feed(&feed, session, 12U, SDK_IMAGE_SESSION_FEED_EOF);
  TEST_REQUIRE(sdk_image_stream_feed(&feed, webp_alpha_3x4, &result) ==
               SDK_STATUS_NO_MEMORY && result.state == SDK_IMAGE_SESSION_STATE_ERROR &&
               result.bytes_written == 0U && result.flush_length == 0U,
               "Tracker exhaustion did not surface WebP NO_MEMORY cleanly");
  TEST_REQUIRE(sdk_image_stream_close(session) == SDK_STATUS_OK,
               "Tracker-exhaustion WebP close failed");

  for (i = 0U; i < SDK_DECODE_MAX_TRACKED; ++i) {
    sdk_decode_untrack(dummies[i]);
    free(dummies[i]);
  }
  TEST_REQUIRE(sdk_decode_tracked_count() == 0U,
               "Tracker-exhaustion dummies leaked tracking slots");
  return 1;
}

static int run_allocation_failure_case(const uint8_t *source, size_t length,
                                       uint32_t width, uint32_t height,
                                       unsigned fail_on, unsigned *attempts,
                                       size_t *peak_bytes)
{
  struct SDKImageStreamBegin begin;
  struct SDKImageStreamFeed feed;
  struct SDKImageStreamResult result;
  uint8_t output[128U];
  uint32_t session;
  uint16_t status;

  host_runtime_reset();
  host_runtime_fail_on_allocation(fail_on);
  memset(output, PAD_BYTE, sizeof(output));
  begin_webp(&begin, output, width, height, width * 4U,
             SDK_IMAGE_OUTPUT_FRAMEBUFFER);
  TEST_REQUIRE(sdk_image_stream_begin(&begin, &result) == SDK_STATUS_OK,
               "Allocation-failure begin failed");
  session = result.session;
  begin_feed(&feed, session, (uint32_t)length, SDK_IMAGE_SESSION_FEED_EOF);
  status = sdk_image_stream_feed(&feed, source, &result);
  if (fail_on == 0U) {
    TEST_REQUIRE(status == SDK_STATUS_OK &&
                 result.state == SDK_IMAGE_SESSION_STATE_COMPLETE,
                 "Baseline WebP allocation run failed");
    *attempts = host_runtime_allocation_attempts();
    *peak_bytes = host_runtime_peak_allocated_bytes();
  } else {
    TEST_REQUIRE(status == SDK_STATUS_NO_MEMORY &&
                 result.state == SDK_IMAGE_SESSION_STATE_ERROR &&
                 result.bytes_written == 0U && result.flush_length == 0U &&
                 all_padding(output, sizeof(output)),
                 "Injected WebP allocation failure was not clean NO_MEMORY");
  }
  TEST_REQUIRE(close_clean(session), "Allocation-failure WebP close failed");
  TEST_REQUIRE(host_runtime_allocated_bytes() == 0U,
               "Allocation-failure path leaked host decoder bytes");
  return 1;
}

static int test_allocation_failures(unsigned *attempts_out, size_t *peak_out)
{
  const uint8_t *sources[] = { webp_alpha_3x4, webp_animation_offset_6x4 };
  const size_t lengths[] = { sizeof(webp_alpha_3x4),
                              sizeof(webp_animation_offset_6x4) };
  const uint32_t widths[] = { 3U, 6U };
  const uint32_t heights[] = { 4U, 4U };
  unsigned source_index;
  unsigned total_attempts = 0U;
  size_t peak_bytes = 0U;

  for (source_index = 0U;
       source_index < sizeof(sources) / sizeof(sources[0]); ++source_index) {
    unsigned attempts = 0U;
    size_t source_peak = 0U;
    unsigned failure;

    TEST_REQUIRE(run_allocation_failure_case(sources[source_index],
                                             lengths[source_index],
                                             widths[source_index],
                                             heights[source_index], 0U,
                                             &attempts, &source_peak),
                 "Could not establish WebP allocation baseline");
    /* Codec allocations come out of the session arena, so the host heap
     * only sees region requests; each one must fail cleanly. */
    TEST_REQUIRE(attempts >= 1U,
                 "WebP arena never requested a region from the decode heap");
    for (failure = 1U; failure <= attempts; ++failure) {
      TEST_REQUIRE(run_allocation_failure_case(sources[source_index],
                                               lengths[source_index],
                                               widths[source_index],
                                               heights[source_index], failure,
                                               &attempts, &source_peak),
                   "Injected WebP allocation failure leaked a session or block");
    }
    total_attempts += attempts;
    if (source_peak > peak_bytes)
      peak_bytes = source_peak;
  }
  *attempts_out = total_attempts;
  *peak_out = peak_bytes;
  return 1;
}

static int run_test(const char *name, int (*test)(void))
{
  if (test())
    return 1;
  fprintf(stderr, "%s failed\n", name);
  return 0;
}

int main(void)
{
  unsigned allocation_attempts;
  size_t peak_bytes;

  if (host_map_session_region() != 0)
    return 1;
  if (!run_test("WebP fixtures", test_webp_fixtures) ||
      !run_test("WebP output formats", test_output_formats) ||
      !run_test("WebP metadata ordering", test_metadata_before_image) ||
      !run_test("WebP rejection bounds", test_rejections_and_bounds) ||
      !run_test("WebP unknown ANMF chunk", test_unknown_anmf_chunk) ||
      !run_test("WebP ANMF framing", test_rejects_image_less_animation_frames) ||
      !run_test("WebP ARM-local full canvas", test_arm_local_requires_full_canvas) ||
      !run_test("WebP session limits", test_session_limit_and_fit) ||
      !run_test("WebP reset reclamation", test_reset_reclaims_retained_animation) ||
      !run_test("WebP pre-EOF reset reclamation", test_reset_reclaims_pre_eof_input) ||
      !run_test("WebP tracker exhaustion", test_tracker_exhaustion))
    return 2;
  if (!test_allocation_failures(&allocation_attempts, &peak_bytes))
    return 2;
  printf("WebP allocation tracking: %u region requests, peak %zu bytes\n",
         allocation_attempts, peak_bytes);
  return 0;
}
#else
int main(int argc, char **argv)
{
  struct SDKImageStreamBegin begin;
  struct SDKImageStreamFeed feed;
  struct SDKImageStreamResult result;
  FILE *input;
  FILE *output;
  long length;
  int width;
  int height;
  int y;
  int x;
  uint8_t *source;
  uint8_t *pixels;
  uint32_t stride;

  if (argc != 3)
    return 2;
  if (host_map_session_region() != 0)
    return 11;
  input = fopen(argv[1], "rb");
  if (!input || fseek(input, 0L, SEEK_END) || (length = ftell(input)) <= 0 ||
      fseek(input, 0L, SEEK_SET))
    return 3;
  source = malloc((size_t)length);
  if (!source || fread(source, 1U, (size_t)length, input) != (size_t)length)
    return 4;
  fclose(input);
  if (!WebPGetInfo(source, (size_t)length, &width, &height) || width <= 0 ||
      height <= 0)
    return 5;
  stride = (uint32_t)width * 4U;
  pixels = malloc((size_t)stride * (uint32_t)height);
  if (!pixels)
    return 6;
  begin_webp(&begin, pixels, (uint32_t)width, (uint32_t)height, stride,
             SDK_IMAGE_OUTPUT_FRAMEBUFFER);
  if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK)
    return 7;
  memset(&feed, 0, sizeof(feed));
  feed.session = result.session;
  feed.src_handle = SOURCE_HANDLE;
  feed.src_length = (uint32_t)length;
  feed.flags = SDK_IMAGE_SESSION_FEED_EOF;
  if (sdk_image_stream_feed(&feed, source, &result) != SDK_STATUS_OK ||
      result.state != SDK_IMAGE_SESSION_STATE_COMPLETE)
    return 8;
  output = fopen(argv[2], "wb");
  if (!output || fprintf(output, "P6\n%d %d\n255\n", width, height) < 0)
    return 9;
  for (y = 0; y < height; ++y) {
    for (x = 0; x < width; ++x) {
      const uint8_t *pixel = pixels + (size_t)y * stride + (size_t)x * 4U;

      if (fputc(pixel[2], output) == EOF || fputc(pixel[1], output) == EOF ||
          fputc(pixel[0], output) == EOF)
        return 10;
    }
  }
  fclose(output);
  (void)sdk_image_stream_close(feed.session);
  free(pixels);
  free(source);
  return 0;
}
#endif
