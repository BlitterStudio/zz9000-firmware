/*
 * Shared-buffer media output: ABI constants and layout, the request builder,
 * the reply decoders and the header-only helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zz9k/media_output.h"
#include "zz9k/reply.h"
#include "zz9k/request.h"

static int checks;
static int failures;

#define CHECK(expr) do { \
  checks++; \
  if (!(expr)) { \
    failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
  } \
} while (0)

static ZZ9KMediaSessionOutputDesc good_desc(void)
{
  ZZ9KMediaSessionOutputDesc desc;

  memset(&desc, 0, sizeof(desc));
  desc.session = 9U;
  desc.buffer_handle = 0x40000001U;
  desc.buffer_offset = 16U;
  desc.slot_stride = 76800U;
  desc.width = 320U;
  desc.height = 240U;
  desc.format = ZZ9K_SURFACE_FORMAT_PLANAR;
  desc.pitch = 40U;
  desc.flags = 8U | ZZ9K_MEDIA_OUTPUT_PLANAR_INTERLEAVED |
               ZZ9K_MEDIA_OUTPUT_DITHER_ORDERED;
  return desc;
}

static void test_constants_and_layout(void)
{
  CHECK(ZZ9K_VIDEO_OUTPUT_SHARED_BUFFER == 2U);
  CHECK(ZZ9K_VIDEO_OUTPUT_DIRECT_OVERLAY == 1U);
  CHECK(ZZ9K_OP_MEDIA_SESSION_OUTPUT_CONFIG == ZZ9K_SERVICE_VIDEO + 0x0eU);
  CHECK(ZZ9K_OP_MEDIA_SESSION_OUTPUT_CONFIG == 0x0b0eU);
  CHECK(ZZ9K_VIDEO_OPCODE_COUNT_SHARED_OUTPUT == 15U);
  CHECK(ZZ9K_MEDIA_SESSION_RESULT_OUTPUT_SLOT1 == (1U << 15));
  CHECK(ZZ9K_MEDIA_SESSION_RESULT_OUTPUT_PENDING == (1U << 16));
  CHECK(ZZ9K_MEDIA_STATUS_OUTPUT == 6U);
  CHECK(ZZ9K_MEDIA_OUTPUT_STATUS_CONFIGURED == (1U << 0));
  CHECK(ZZ9K_MEDIA_OUTPUT_STATUS_COMPLETE == (1U << 1));
  CHECK(ZZ9K_MEDIA_OUTPUT_STATUS_SLOT1 == (1U << 2));
  CHECK(ZZ9K_MEDIA_OUTPUT_STATUS_PENDING == (1U << 3));
  CHECK(ZZ9K_MEDIA_OUTPUT_DEPTH_MASK == 0x0fU);
  CHECK(ZZ9K_MEDIA_OUTPUT_PLANAR_INTERLEAVED == (1U << 4));
  CHECK(ZZ9K_MEDIA_OUTPUT_DITHER_ORDERED == (1U << 5));
  CHECK(ZZ9K_MEDIA_OUTPUT_DITHER_DIFFUSION == (1U << 6));
  CHECK(ZZ9K_MEDIA_OUTPUT_PALETTE_HOST == (1U << 7));
  CHECK(ZZ9K_MEDIA_OUTPUT_SCALE_NEAREST == (1U << 8));
  CHECK(ZZ9K_MEDIA_OUTPUT_DEPTH(0x35U) == 5U);
  CHECK(ZZ9K_MEDIA_OUTPUT_MAX_WIDTH == 1920 &&
        ZZ9K_MEDIA_OUTPUT_MAX_HEIGHT == 1088 &&
        ZZ9K_MEDIA_OUTPUT_PALETTE_BYTES == 1024);
  /* the new flags sit above every existing result flag */
  CHECK(ZZ9K_MEDIA_SESSION_RESULT_OUTPUT_SLOT1 >
        ZZ9K_MEDIA_SESSION_RESULT_AUDIO_UNDERRUN);

  CHECK(sizeof(ZZ9KMediaSessionOutputPayload) == 48U);
  CHECK(offsetof(ZZ9KMediaSessionOutputPayload, session) == 0U);
  CHECK(offsetof(ZZ9KMediaSessionOutputPayload, buffer_handle) == 4U);
  CHECK(offsetof(ZZ9KMediaSessionOutputPayload, buffer_offset) == 8U);
  CHECK(offsetof(ZZ9KMediaSessionOutputPayload, slot_stride) == 12U);
  CHECK(offsetof(ZZ9KMediaSessionOutputPayload, width) == 16U);
  CHECK(offsetof(ZZ9KMediaSessionOutputPayload, height) == 20U);
  CHECK(offsetof(ZZ9KMediaSessionOutputPayload, format) == 24U);
  CHECK(offsetof(ZZ9KMediaSessionOutputPayload, pitch) == 28U);
  CHECK(offsetof(ZZ9KMediaSessionOutputPayload, flags) == 32U);
  CHECK(offsetof(ZZ9KMediaSessionOutputPayload, palette_handle) == 36U);
  CHECK(offsetof(ZZ9KMediaSessionOutputPayload, palette_offset) == 40U);
  CHECK(offsetof(ZZ9KMediaSessionOutputPayload, reserved) == 44U);
}

static void test_discovery(void)
{
  ZZ9KServiceInfo video;

  memset(&video, 0, sizeof(video));
  video.service_id = ZZ9K_SERVICE_VIDEO;
  video.opcode_count = 14U;
  CHECK(!zz9k_video_shared_output_available(&video));
  video.opcode_count = 15U;
  CHECK(zz9k_video_shared_output_available(&video));
  video.opcode_count = 40U;
  CHECK(zz9k_video_shared_output_available(&video));
  video.service_id = ZZ9K_SERVICE_AUDIO;
  CHECK(!zz9k_video_shared_output_available(&video));
  CHECK(!zz9k_video_shared_output_available(0));
}

static void test_request_builder(void)
{
  ZZ9KRequest request;
  ZZ9KMediaSessionOutputDesc desc = good_desc();
  const ZZ9KMediaSessionOutputPayload *p;

  CHECK(zz9k_request_media_session_output(&request, &desc) == ZZ9K_STATUS_OK);
  CHECK(request.entry.opcode == ZZ9K_OP_MEDIA_SESSION_OUTPUT_CONFIG);
  CHECK(request.entry.payload_len == 48U);
  p = (const ZZ9KMediaSessionOutputPayload *)request.entry.payload.inline_data;
  CHECK(zz9k_get_be32(p->session) == 9U);
  CHECK(zz9k_get_be32(p->buffer_handle) == 0x40000001U);
  CHECK(zz9k_get_be32(p->buffer_offset) == 16U);
  CHECK(zz9k_get_be32(p->slot_stride) == 76800U);
  CHECK(zz9k_get_be32(p->width) == 320U);
  CHECK(zz9k_get_be32(p->height) == 240U);
  CHECK(zz9k_get_be32(p->format) == ZZ9K_SURFACE_FORMAT_PLANAR);
  CHECK(zz9k_get_be32(p->pitch) == 40U);
  CHECK(zz9k_get_be32(p->flags) == desc.flags);
  CHECK(zz9k_get_be32(p->palette_handle) == 0U);
  CHECK(zz9k_get_be32(p->palette_offset) == 0U);
  CHECK(zz9k_get_be32(p->reserved) == 0U);
  /* byte order on the wire: big-endian */
  CHECK(request.entry.payload.inline_data[4] == 0x40 &&
        request.entry.payload.inline_data[7] == 0x01);

  CHECK(zz9k_request_media_session_output(0, &desc) ==
        ZZ9K_STATUS_BAD_REQUEST);
  CHECK(zz9k_request_media_session_output(&request, 0) ==
        ZZ9K_STATUS_BAD_REQUEST);
  desc = good_desc();
  desc.session = 0U;
  CHECK(zz9k_request_media_session_output(&request, &desc) ==
        ZZ9K_STATUS_BAD_REQUEST);
  desc = good_desc();
  desc.buffer_handle = 0U;
  CHECK(zz9k_request_media_session_output(&request, &desc) ==
        ZZ9K_STATUS_BAD_REQUEST);
  desc = good_desc();
  desc.buffer_handle = ZZ9K_INVALID_HANDLE;
  CHECK(zz9k_request_media_session_output(&request, &desc) ==
        ZZ9K_STATUS_BAD_REQUEST);
  desc = good_desc();
  desc.pitch = 39U;
  CHECK(zz9k_request_media_session_output(&request, &desc) ==
        ZZ9K_STATUS_BAD_REQUEST);
  desc = good_desc();
  desc.flags = 8U | ZZ9K_MEDIA_OUTPUT_DITHER_DIFFUSION;
  CHECK(zz9k_request_media_session_output(&request, &desc) ==
        ZZ9K_STATUS_UNSUPPORTED);
  desc.flags |= ZZ9K_MEDIA_OUTPUT_DITHER_ORDERED;
  CHECK(zz9k_request_media_session_output(&request, &desc) ==
        ZZ9K_STATUS_BAD_REQUEST);
  desc = good_desc();
  desc.format = ZZ9K_SURFACE_FORMAT_ARGB8888;
  CHECK(zz9k_request_media_session_output(&request, &desc) ==
        ZZ9K_STATUS_UNSUPPORTED);
  /* a host palette needs its handle, and only then */
  desc = good_desc();
  desc.flags |= ZZ9K_MEDIA_OUTPUT_PALETTE_HOST;
  CHECK(zz9k_request_media_session_output(&request, &desc) ==
        ZZ9K_STATUS_BAD_REQUEST);
  desc.palette_handle = 0x40000002U;
  desc.palette_offset = 64U;
  CHECK(zz9k_request_media_session_output(&request, &desc) == ZZ9K_STATUS_OK);
  p = (const ZZ9KMediaSessionOutputPayload *)request.entry.payload.inline_data;
  CHECK(zz9k_get_be32(p->palette_handle) == 0x40000002U &&
        zz9k_get_be32(p->palette_offset) == 64U);
  desc = good_desc();
  desc.palette_handle = 0x40000002U;
  CHECK(zz9k_request_media_session_output(&request, &desc) ==
        ZZ9K_STATUS_BAD_REQUEST);
  desc = good_desc();
  desc.palette_offset = 4U;
  CHECK(zz9k_request_media_session_output(&request, &desc) ==
        ZZ9K_STATUS_BAD_REQUEST);

  /* status requests now reach page 6 */
  CHECK(zz9k_request_media_session_status(&request, 9U,
                                          ZZ9K_MEDIA_STATUS_OUTPUT, 0U) ==
        ZZ9K_STATUS_OK);
  CHECK(zz9k_request_media_session_status(&request, 9U,
                                          ZZ9K_MEDIA_STATUS_OUTPUT + 1U,
                                          0U) == ZZ9K_STATUS_BAD_REQUEST);
}

static void test_check_and_layout(void)
{
  ZZ9KMediaSessionOutputDesc desc = good_desc();
  ZZ9KMediaOutputLayout layout;

  /* the numbers luvwagn quoted: 76.8 KB in 8 planes, 48 KB in 5, 150 KB RGB565 */
  CHECK(zz9k_media_output_check(&desc, &layout) == ZZ9K_STATUS_OK);
  CHECK(layout.depth == 8U && layout.row_bytes == 40U &&
        layout.picture_bytes == 76800U && layout.slots == 2U &&
        layout.span_bytes == 153600U);
  desc.flags = 5U;
  desc.slot_stride = 0U;
  CHECK(zz9k_media_output_check(&desc, &layout) == ZZ9K_STATUS_OK);
  CHECK(layout.picture_bytes == 48000U && layout.slots == 1U &&
        layout.span_bytes == 48000U);
  desc.format = ZZ9K_SURFACE_FORMAT_RGB565;
  desc.flags = 0U;
  desc.pitch = 640U;
  CHECK(zz9k_media_output_check(&desc, &layout) == ZZ9K_STATUS_OK);
  CHECK(layout.depth == 0U && layout.picture_bytes == 153600U);
  /* depth 0 means 8 */
  desc = good_desc();
  desc.flags = 0U;
  desc.slot_stride = 0U;
  CHECK(zz9k_media_output_check(&desc, &layout) == ZZ9K_STATUS_OK &&
        layout.depth == 8U);
  CHECK(zz9k_media_output_depth(0U) == 8U && zz9k_media_output_depth(3U) == 3U);
  desc.flags = 9U;
  CHECK(zz9k_media_output_check(&desc, &layout) == ZZ9K_STATUS_BAD_REQUEST);
  /* slot 1 must not overlap slot 0, and offsets are word aligned */
  desc = good_desc();
  desc.slot_stride = 76796U;
  CHECK(zz9k_media_output_check(&desc, &layout) == ZZ9K_STATUS_BAD_REQUEST);
  desc = good_desc();
  desc.buffer_offset = 3U;
  CHECK(zz9k_media_output_check(&desc, &layout) == ZZ9K_STATUS_BAD_REQUEST);
  desc = good_desc();
  desc.width = 1921U;
  desc.pitch = 241U;
  CHECK(zz9k_media_output_check(&desc, &layout) == ZZ9K_STATUS_BAD_REQUEST);
  CHECK(zz9k_media_output_check(&desc, 0) == ZZ9K_STATUS_BAD_REQUEST);

  /* planar row offsets for both layouts */
  CHECK(zz9k_media_output_plane_row_offset(8U, 240U, 40U, 0U, 0U) == 0U);
  CHECK(zz9k_media_output_plane_row_offset(8U, 240U, 40U, 2U, 5U) ==
        (2U * 240U + 5U) * 40U);
  CHECK(zz9k_media_output_plane_row_offset(
            8U | ZZ9K_MEDIA_OUTPUT_PLANAR_INTERLEAVED, 240U, 40U, 2U, 5U) ==
        (5U * 8U + 2U) * 40U);
  CHECK(zz9k_media_output_plane_row_offset(
            5U | ZZ9K_MEDIA_OUTPUT_PLANAR_INTERLEAVED, 240U, 40U, 4U, 239U) ==
        (239U * 5U + 4U) * 40U);
}

static void test_fixed_palettes(void)
{
  uint32_t pal[256];
  static const uint32_t used[9] = { 0, 2, 4, 8, 16, 32, 64, 125, 252 };
  uint32_t depth;

  for (depth = 1U; depth <= 8U; depth++) {
    uint32_t i;

    CHECK(zz9k_media_output_fixed_palette(depth, pal) == used[depth]);
    for (i = used[depth]; i < 256U; i++) {
      if (pal[i] != 0U) {
        break;
      }
    }
    CHECK(i == 256U);
  }
  zz9k_media_output_fixed_palette(8U, pal);
  CHECK(pal[0] == 0x000000U && pal[251] == 0xffffffU);
  CHECK(pal[(1U * 7U + 0U) * 6U + 0U] == 0x330000U);
  CHECK(pal[1] == 0x000033U);
  zz9k_media_output_fixed_palette(1U, pal);
  CHECK(pal[0] == 0x000000U && pal[1] == 0xffffffU);
  zz9k_media_output_fixed_palette(2U, pal);
  CHECK(pal[1] == 0x555555U && pal[2] == 0xaaaaaaU && pal[3] == 0xffffffU);
  zz9k_media_output_fixed_palette(3U, pal);
  CHECK(pal[1] == 0x0000ffU && pal[2] == 0x00ff00U && pal[4] == 0xff0000U &&
        pal[7] == 0xffffffU);
  CHECK(zz9k_media_output_fixed_palette(0U, pal) == 0U);
  CHECK(zz9k_media_output_fixed_palette(9U, pal) == 0U);
}

static void test_reply_decoders(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KMediaSessionMainResult main_result;
  ZZ9KMediaSessionStatusResult status_result;

  /* a PRESENT reply of a shared-buffer session */
  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_MEDIA_SESSION_PRESENT;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = 48U;
  zz9k_put_be32(&reply.payload.inline_data[0], 9U);
  zz9k_put_be32(&reply.payload.inline_data[4], ZZ9K_MEDIA_SESSION_STATE_READY);
  zz9k_put_be32(&reply.payload.inline_data[40], 76800U);
  zz9k_put_be32(&reply.payload.inline_data[44],
                ZZ9K_MEDIA_SESSION_RESULT_PRESENTED |
                    ZZ9K_MEDIA_SESSION_RESULT_OUTPUT_PENDING |
                    ZZ9K_MEDIA_SESSION_RESULT_OUTPUT_SLOT1);
  CHECK(zz9k_reply_media_session_main(
            &reply, ZZ9K_OP_MEDIA_SESSION_PRESENT, &main_result) ==
        ZZ9K_STATUS_OK);
  CHECK((main_result.flags & ZZ9K_MEDIA_SESSION_RESULT_OUTPUT_SLOT1) != 0U);
  CHECK((main_result.flags & ZZ9K_MEDIA_SESSION_RESULT_OUTPUT_PENDING) != 0U);
  CHECK(main_result.bytes_written == 76800U);
  /* the OUTPUT_CONFIG reply is a main result too */
  reply.opcode = ZZ9K_OP_MEDIA_SESSION_OUTPUT_CONFIG;
  zz9k_put_be32(&reply.payload.inline_data[44], 0U);
  CHECK(zz9k_reply_media_session_main(
            &reply, ZZ9K_OP_MEDIA_SESSION_OUTPUT_CONFIG, &main_result) ==
        ZZ9K_STATUS_OK);
  /* an unknown bit above them is still rejected */
  zz9k_put_be32(&reply.payload.inline_data[44], 1U << 17);
  CHECK(zz9k_reply_media_session_main(
            &reply, ZZ9K_OP_MEDIA_SESSION_OUTPUT_CONFIG, &main_result) ==
        ZZ9K_STATUS_INTERNAL_ERROR);

  /* the output page owns its flag namespace */
  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_MEDIA_SESSION_STATUS;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = 48U;
  zz9k_put_be32(&reply.payload.inline_data[0], 9U);
  zz9k_put_be32(&reply.payload.inline_data[4], ZZ9K_MEDIA_SESSION_STATE_READY);
  zz9k_put_be32(&reply.payload.inline_data[8], ZZ9K_MEDIA_STATUS_OUTPUT);
  zz9k_put_be32(&reply.payload.inline_data[12],
                ZZ9K_MEDIA_OUTPUT_STATUS_CONFIGURED |
                    ZZ9K_MEDIA_OUTPUT_STATUS_COMPLETE |
                    ZZ9K_MEDIA_OUTPUT_STATUS_SLOT1 |
                    ZZ9K_MEDIA_OUTPUT_STATUS_PENDING);
  zz9k_put_be32(&reply.payload.inline_data[20], 7U);   /* value 0 low */
  zz9k_put_be32(&reply.payload.inline_data[28], 8U);   /* value 1 low */
  zz9k_put_be32(&reply.payload.inline_data[36], 1234U);/* value 2 low */
  zz9k_put_be32(&reply.payload.inline_data[44], 0x1bfU);/* value 3 low */
  CHECK(zz9k_reply_media_session_status(&reply, &status_result) ==
        ZZ9K_STATUS_OK);
  CHECK(status_result.page == ZZ9K_MEDIA_STATUS_OUTPUT);
  CHECK(status_result.value[0] == 7U && status_result.value[1] == 8U &&
        status_result.value[2] == 1234U && status_result.value[3] == 0x1bfU);
  zz9k_put_be32(&reply.payload.inline_data[12], 1U << 4);
  CHECK(zz9k_reply_media_session_status(&reply, &status_result) ==
        ZZ9K_STATUS_INTERNAL_ERROR);
  /* a session-result bit is not a page-6 bit */
  zz9k_put_be32(&reply.payload.inline_data[12],
                ZZ9K_MEDIA_SESSION_RESULT_PRESENTED);
  CHECK(zz9k_reply_media_session_status(&reply, &status_result) ==
        ZZ9K_STATUS_INTERNAL_ERROR);
  zz9k_put_be32(&reply.payload.inline_data[12], 0U);
  zz9k_put_be32(&reply.payload.inline_data[8], ZZ9K_MEDIA_STATUS_OUTPUT + 1U);
  CHECK(zz9k_reply_media_session_status(&reply, &status_result) ==
        ZZ9K_STATUS_INTERNAL_ERROR);
}

int main(void)
{
  test_constants_and_layout();
  test_discovery();
  test_request_builder();
  test_check_and_layout();
  test_fixed_palettes();
  test_reply_decoders();
  if (failures != 0) {
    printf("media_output_abi_test: %d failure(s) of %d checks\n", failures,
           checks);
    return 1;
  }
  printf("media_output_abi_test: %d checks passed\n", checks);
  return 0;
}
