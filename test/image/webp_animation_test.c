/*
 * Host behavioral tests for the firmware WebP animation session engine
 * (U2b). Every scenario drives the real public image-session entry
 * points (sdk_image_stream_begin/feed/close plus the four animation
 * frame operations) against actual libwebp decode results.
 *
 * Composited-frame references come from a SECOND, independent
 * WebPAnimDecoder instance driven by this test; the expected packed
 * YUV422CGX bytes are produced by a test-owned BT.601 converter, never
 * by the firmware conversion internals, so a parity failure names the
 * engine (or the fixture) rather than the oracle.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdk_decode_reclaim.h"
#include "sdk_image_stream.h"
#include "sdk_vorbis_alloc.h"
#include "sdk_webp_alloc.h"
#include "memorymap.h"
#include <sys/mman.h>

#include "webp_animation_fixtures.h"
#include "webp_fixtures.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <webp/decode.h>
#include <webp/demux.h>

#define PAD_BYTE 0xa5U
#define SOURCE_HANDLE 0x40000002U

#define CANVAS_W 8U
#define CANVAS_H 6U
#define PIP_PITCH 32U                       /* >= ((8+1)/2)*4, leaves padding */
#define PIP_ROW_BYTES (((CANVAS_W + 1U) / 2U) * 4U)
#define PIP_LEN (PIP_PITCH * CANVAS_H)

void host_runtime_reset(void);
void sdk_decode_heap_free(void *ptr);
size_t host_runtime_allocated_bytes(void);
size_t host_runtime_peak_allocated_bytes(void);

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

#define TEST_REQUIRE(condition, message) do { \
  if (!(condition)) { \
    fprintf(stderr, "%s\n", message); \
    return 0; \
  } \
} while (0)

/* ---------------------------------------------------------------- helpers */

static void begin_animation(struct SDKImageStreamBegin *begin, uint8_t *pip,
                            uint32_t width, uint32_t height, uint32_t pitch)
{
  memset(begin, 0, sizeof(*begin));
  begin->codec = SDK_IMAGE_CODEC_WEBP;
  begin->output_mode = SDK_IMAGE_OUTPUT_SURFACE;
  begin->output_format = SDK_SURFACE_FORMAT_YUV422CGX;
  begin->flags = SDK_IMAGE_SESSION_BEGIN_ANIMATION;
  begin->dst_width = width;
  begin->dst_height = height;
  begin->dst_address = (uintptr_t)pip;
  begin->dst_pitch = pitch;
  begin->dst_length = pitch * height;
  begin->core1_affine = 1U;
}

static void begin_static_framebuffer(struct SDKImageStreamBegin *begin,
                                     uint8_t *out, uint32_t width,
                                     uint32_t height, uint32_t stride)
{
  memset(begin, 0, sizeof(*begin));
  begin->codec = SDK_IMAGE_CODEC_WEBP;
  begin->output_mode = SDK_IMAGE_OUTPUT_FRAMEBUFFER;
  begin->output_format = SDK_SURFACE_FORMAT_BGRA8888;
  begin->dst_surface = SDK_SURFACE_HANDLE_FRAMEBUFFER;
  begin->dst_width = width;
  begin->dst_height = height;
  begin->dst_address = (uintptr_t)out;
  begin->dst_pitch = stride;
  begin->dst_length = stride * height;
  begin->core1_affine = 1U;
  begin->direct_arm_local = 1U;
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

/* Begin an animation session and feed one whole fixture with EOF in a
 * single call. Returns the feed status; *session is always set so the
 * caller can close even on rejection paths. */
static uint16_t animation_open_and_feed(const uint8_t *data, size_t len,
                                        uint8_t *pip,
                                        struct SDKImageStreamResult *sres,
                                        uint32_t *session)
{
  struct SDKImageStreamBegin begin;
  struct SDKImageStreamFeed feed;
  uint16_t status;

  memset(pip, PAD_BYTE, PIP_LEN);
  begin_animation(&begin, pip, CANVAS_W, CANVAS_H, PIP_PITCH);
  if (sdk_image_stream_begin(&begin, sres) != SDK_STATUS_OK)
    return SDK_STATUS_NO_MEMORY;
  *session = sres->session;
  begin_feed(&feed, *session, (uint32_t)len, SDK_IMAGE_SESSION_FEED_EOF);
  status = sdk_image_stream_feed(&feed, data, sres);
  if (status == SDK_STATUS_OK &&
      sres->state != SDK_IMAGE_SESSION_STATE_ANIMATION_READY)
    return SDK_STATUS_INTERNAL_ERROR;
  return status;
}

/* Test-owned BT.601 limited-range RGBA -> packed YUV422CGX (Y0 U Y1 V)
 * reference. One chroma pair per two horizontally adjacent pixels; an
 * odd final pixel duplicates its luma and reuses its chroma. This is an
 * independent transcription of the color math, deliberately not shared
 * with the firmware converter. */
static int32_t clamp8(int32_t v)
{
  return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static void ref_rgba_to_yuv422cgx(const uint8_t *rgba, uint32_t width,
                                  uint32_t height, uint8_t *dst,
                                  uint32_t dst_pitch)
{
  uint32_t y;

  for (y = 0U; y < height; ++y) {
    const uint8_t *src = rgba + (size_t)y * width * 4U;
    uint8_t *row = dst + (size_t)y * dst_pitch;
    uint32_t x;

    for (x = 0U; x < width; x += 2U) {
      const uint8_t *p0 = src + (size_t)x * 4U;
      const uint8_t *p1 = (x + 1U < width) ? p0 + 4U : p0;
      int32_t r0 = p0[0], g0 = p0[1], b0 = p0[2];
      int32_t r1 = p1[0], g1 = p1[1], b1 = p1[2];
      int32_t ra = (r0 + r1) / 2, ga = (g0 + g1) / 2, ba = (b0 + b1) / 2;
      int32_t y0 = clamp8(((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16);
      int32_t y1 = clamp8(((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16);
      int32_t u = clamp8(((-38 * ra - 74 * ga + 112 * ba + 128) >> 8) + 128);
      int32_t v = clamp8(((112 * ra - 94 * ga - 18 * ba + 128) >> 8) + 128);
      uint8_t *mp = row + (x / 2U) * 4U;

      mp[0] = (uint8_t)y0;
      mp[1] = (uint8_t)u;
      mp[2] = (uint8_t)y1;
      mp[3] = (uint8_t)v;
    }
  }
}

static int all_padding(const uint8_t *buf, size_t len)
{
  size_t i;

  for (i = 0U; i < len; ++i) {
    if (buf[i] != PAD_BYTE)
      return 0;
  }
  return 1;
}

static int pip_matches_reference(const uint8_t *pip, const uint8_t *ref_rgba,
                                 uint32_t width, uint32_t height)
{
  uint8_t expected[PIP_LEN];
  uint32_t y;

  memset(expected, PAD_BYTE, sizeof(expected));
  ref_rgba_to_yuv422cgx(ref_rgba, width, height, expected, PIP_PITCH);
  for (y = 0U; y < height; ++y) {
    if (memcmp(pip + y * PIP_PITCH, expected + y * PIP_PITCH,
               PIP_ROW_BYTES) != 0)
      return 0;
    if (!all_padding(pip + y * PIP_PITCH + PIP_ROW_BYTES,
                     PIP_PITCH - PIP_ROW_BYTES))
      return 0;
  }
  return 1;
}

/* Independent oracle over the same fixture bytes. The host libwebp build
 * routes allocations through the same shim as the engine, so the oracle
 * owns a separate arena and selects it before every libwebp call; nothing
 * it allocates is billed to an engine session. */
struct ref_decoder {
  WebPAnimDecoder *dec;
  uint32_t loop_count;
  uint32_t frame_count;
};

static struct sdk_vorbis_heap ref_heap;

static int ref_open(struct ref_decoder *ref, const uint8_t *data, size_t len)
{
  WebPData webp_data;
  WebPAnimDecoderOptions options;
  WebPAnimInfo info;

  memset(ref, 0, sizeof(*ref));
  sdk_vorbis_heap_init(&ref_heap, 256U * 1024U, 64U * 1024U * 1024U);
  sdk_webp_alloc_select(&ref_heap);
  if (!WebPAnimDecoderOptionsInit(&options))
    return 0;
  options.color_mode = MODE_RGBA;
  webp_data.bytes = data;
  webp_data.size = len;
  ref->dec = WebPAnimDecoderNew(&webp_data, &options);
  if (!ref->dec || !WebPAnimDecoderGetInfo(ref->dec, &info))
    return 0;
  ref->loop_count = info.loop_count;
  ref->frame_count = info.frame_count;
  return 1;
}

static int ref_next(struct ref_decoder *ref, uint8_t **rgba, int *timestamp)
{
  sdk_webp_alloc_select(&ref_heap);
  return WebPAnimDecoderGetNext(ref->dec, rgba, timestamp);
}

static void ref_reset(struct ref_decoder *ref)
{
  sdk_webp_alloc_select(&ref_heap);
  WebPAnimDecoderReset(ref->dec);
}

static void ref_close(struct ref_decoder *ref)
{
  /* Dropping the arena frees everything the oracle allocated. */
  sdk_vorbis_heap_release(&ref_heap);
  memset(ref, 0, sizeof(*ref));
}

/* One full present/retire cycle for the currently outstanding token. */
static int retire_current(uint32_t session, uint32_t token,
                          struct SDKImageAnimationFrameResult *ares)
{
  TEST_REQUIRE(sdk_image_stream_frame_present(session, token, 0U, ares) ==
                   SDK_STATUS_OK,
               "frame present failed");
  TEST_REQUIRE((ares->flags & SDK_IMAGE_ANIMATION_FRAME_FLAG_PRESENTED) != 0U,
               "present did not report PRESENTED");
  TEST_REQUIRE(sdk_image_stream_frame_retire(session, token, 0U, ares) ==
                   SDK_STATUS_OK,
               "frame retire failed");
  return 1;
}

/* ------------------------------------------------- 1: ops before EOF fail */

static int test_animation_rejects_ops_before_eof(void)
{
  struct SDKImageStreamBegin begin;
  struct SDKImageStreamFeed feed;
  struct SDKImageStreamResult sres;
  struct SDKImageAnimationFrameResult ares;
  uint8_t pip[PIP_LEN];
  uint32_t session;

  host_runtime_reset();
  memset(pip, PAD_BYTE, sizeof(pip));
  begin_animation(&begin, pip, CANVAS_W, CANVAS_H, PIP_PITCH);
  TEST_REQUIRE(sdk_image_stream_begin(&begin, &sres) == SDK_STATUS_OK,
               "pre-EOF begin failed");
  session = sres.session;

  /* first half of the container only, no EOF */
  begin_feed(&feed, session, sizeof(webp_anim3_blend_dispose_8x6) / 2U, 0U);
  TEST_REQUIRE(sdk_image_stream_feed(&feed, webp_anim3_blend_dispose_8x6,
                                     &sres) == SDK_STATUS_OK &&
                   sres.state == SDK_IMAGE_SESSION_STATE_NEED_INPUT,
               "partial animation feed did not ask for more input");

  memset(&ares, 0, sizeof(ares));
  TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                   SDK_STATUS_BAD_REQUEST,
               "NEXT accepted before EOF");
  TEST_REQUIRE(sdk_image_stream_frame_present(session, 1U, 0U, &ares) ==
                   SDK_STATUS_BAD_REQUEST,
               "PRESENT accepted before EOF");
  TEST_REQUIRE(sdk_image_stream_frame_retire(session, 1U, 0U, &ares) ==
                   SDK_STATUS_BAD_REQUEST,
               "RETIRE accepted before EOF");
  TEST_REQUIRE(sdk_image_stream_restart(session, 0U, &ares) ==
                   SDK_STATUS_BAD_REQUEST,
               "RESTART accepted before EOF");
  TEST_REQUIRE(all_padding(pip, PIP_LEN),
               "PIP target was written before any frame was decoded");

  TEST_REQUIRE(sdk_image_stream_close(session) == SDK_STATUS_OK,
               "pre-EOF close failed");
  host_runtime_reset();
  return 1;
}

/* ------------------------------- 2: EOF metadata and static-mode rejection */

static int test_animation_eof_state_and_static_rejection(void)
{
  struct SDKImageStreamResult sres;
  struct SDKImageAnimationFrameResult ares;
  uint8_t pip[PIP_LEN];
  uint32_t session;
  uint16_t status;

  host_runtime_reset();
  status = animation_open_and_feed(webp_anim3_blend_dispose_8x6,
                                   sizeof(webp_anim3_blend_dispose_8x6), pip,
                                   &sres, &session);
  TEST_REQUIRE(status == SDK_STATUS_OK,
               "animation session did not reach ANIMATION_READY at EOF");
  TEST_REQUIRE(sres.image_width == CANVAS_W && sres.image_height == CANVAS_H,
               "EOF result did not report the canvas geometry");
  TEST_REQUIRE(sres.output_format == SDK_SURFACE_FORMAT_YUV422CGX,
               "EOF result lost the animation output format");
  /* loop metadata rides on every frame result; spot-check it now */
  memset(&ares, 0, sizeof(ares));
  TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                   SDK_STATUS_OK &&
                   ares.loop_count == 0U && ares.canvas_width == CANVAS_W &&
                   ares.canvas_height == CANVAS_H &&
                   ares.output_format == SDK_SURFACE_FORMAT_YUV422CGX,
               "infinite-loop metadata wrong on first NEXT");
  TEST_REQUIRE(retire_current(session, ares.frame_token, &ares),
               "first frame cycle failed");
  TEST_REQUIRE(sdk_image_stream_close(session) == SDK_STATUS_OK,
               "close after metadata check failed");

  /* static WebP input must be rejected in animation mode */
  memset(&sres, 0, sizeof(sres));
  status = animation_open_and_feed(webp_lossy_3x5, sizeof(webp_lossy_3x5),
                                   pip, &sres, &session);
  TEST_REQUIRE(status == SDK_STATUS_BAD_REQUEST,
               "static input was not rejected in animation mode");
  TEST_REQUIRE(all_padding(pip, PIP_LEN),
               "static input wrote the PIP target before rejection");
  TEST_REQUIRE(sdk_image_stream_close(session) == SDK_STATUS_OK &&
                   sdk_image_stream_active_count() == 0U,
               "close after static rejection failed");

  host_runtime_reset();
  return 1;
}

/* --------------------- 3+4: composited parity, durations incl. zero, wrap */

static int test_animation_next_matches_reference(void)
{
  struct SDKImageAnimationFrameResult ares;
  struct SDKImageStreamResult sres;
  struct ref_decoder ref;
  uint8_t pip[PIP_LEN];
  uint32_t session;
  int timestamp = 0;
  int prev_timestamp = 0;
  static const uint32_t durations[3] = {100U, 0U, 250U};
  uint32_t i;

  host_runtime_reset();
  TEST_REQUIRE(animation_open_and_feed(webp_anim3_blend_dispose_8x6,
                                       sizeof(webp_anim3_blend_dispose_8x6),
                                       pip, &sres, &session) ==
                   SDK_STATUS_OK,
               "parity session did not reach ready");
  TEST_REQUIRE(ref_open(&ref, webp_anim3_blend_dispose_8x6,
                        sizeof(webp_anim3_blend_dispose_8x6)) &&
                   ref.frame_count == 3U && ref.loop_count == 0U,
               "reference decoder rejected the fixture");

  for (i = 0U; i < 3U; ++i) {
    uint8_t *rgba;

    TEST_REQUIRE(ref_next(&ref, &rgba, &timestamp),
                 "reference decoder ran out of frames early");
    memset(&ares, 0, sizeof(ares));
    TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                     SDK_STATUS_OK,
                 "frame NEXT failed");
    TEST_REQUIRE((ares.flags & SDK_IMAGE_ANIMATION_FRAME_FLAG_FRAME_READY) != 0U,
                 "NEXT did not report FRAME_READY");
    TEST_REQUIRE(ares.frame_index == i && ares.loop_index == 0U &&
                     ares.loop_count == 0U,
                 "frame bookkeeping wrong on NEXT");
    TEST_REQUIRE(ares.frame_token != 0U, "NEXT returned a zero token");
    /* durations must be reported verbatim, including the zero-duration
     * middle frame (oracle: difference of libwebp timestamps) */
    TEST_REQUIRE(ares.frame_duration_ms ==
                     (uint32_t)(timestamp - prev_timestamp),
                 "duration mismatch against reference timestamps");
    TEST_REQUIRE(ares.frame_duration_ms == durations[i],
                 "fixture duration not reported verbatim");
    prev_timestamp = timestamp;

    TEST_REQUIRE(pip_matches_reference(pip, rgba, CANVAS_W, CANVAS_H),
                 "composited YUV422CGX output mismatch");
    TEST_REQUIRE(retire_current(session, ares.frame_token, &ares),
                 "frame cycle failed");
  }

  /* infinite loop: NEXT after the last frame wraps to loop 1 frame 0 */
  {
    uint8_t *rgba;

    ref_reset(&ref);
    TEST_REQUIRE(1, "");
    TEST_REQUIRE(ref_next(&ref, &rgba, &timestamp),
                 "reference could not restart for the wrap check");
    memset(&ares, 0, sizeof(ares));
    TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                     SDK_STATUS_OK,
                 "infinite-loop wrap NEXT failed");
    TEST_REQUIRE(ares.loop_index == 1U && ares.frame_index == 0U,
                 "wrap did not advance loop_index / reset frame_index");
    TEST_REQUIRE(pip_matches_reference(pip, rgba, CANVAS_W, CANVAS_H),
                 "wrapped frame output mismatch");
    TEST_REQUIRE(retire_current(session, ares.frame_token, &ares),
                 "wrapped frame cycle failed");
  }

  ref_close(&ref);
  TEST_REQUIRE(sdk_image_stream_close(session) == SDK_STATUS_OK,
               "parity close failed");
  host_runtime_reset();
  return 1;
}

/* ------------------------------------------- 5: one-outstanding-token rules */

static int test_animation_token_enforcement(void)
{
  struct SDKImageAnimationFrameResult ares;
  struct SDKImageStreamResult sres;
  uint8_t pip[PIP_LEN];
  uint32_t session, token;

  host_runtime_reset();
  TEST_REQUIRE(animation_open_and_feed(webp_anim3_blend_dispose_8x6,
                                       sizeof(webp_anim3_blend_dispose_8x6),
                                       pip, &sres, &session) ==
                   SDK_STATUS_OK,
               "token session did not reach ready");

  memset(&ares, 0, sizeof(ares));
  TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                   SDK_STATUS_OK,
               "first NEXT failed");
  token = ares.frame_token;
  TEST_REQUIRE(token != 0U, "token must be nonzero");

  /* NEXT before RETIRE */
  TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                   SDK_STATUS_BAD_REQUEST,
               "NEXT accepted while a token was outstanding");

  /* PRESENT: zero token, stale token, then duplicate */
  TEST_REQUIRE(sdk_image_stream_frame_present(session, 0U, 0U, &ares) ==
                   SDK_STATUS_BAD_REQUEST,
               "PRESENT accepted a zero token");
  TEST_REQUIRE(sdk_image_stream_frame_present(session, token + 99U, 0U,
                                              &ares) ==
                   SDK_STATUS_BAD_REQUEST,
               "PRESENT accepted a stale token");
  TEST_REQUIRE(sdk_image_stream_frame_present(session, token, 0U, &ares) ==
                   SDK_STATUS_OK,
               "PRESENT rejected the live token");
  TEST_REQUIRE(sdk_image_stream_frame_present(session, token, 0U, &ares) ==
                   SDK_STATUS_BAD_REQUEST,
               "duplicate PRESENT accepted");

  /* RETIRE: stale token, live token, duplicate */
  TEST_REQUIRE(sdk_image_stream_frame_retire(session, token + 99U, 0U,
                                             &ares) ==
                   SDK_STATUS_BAD_REQUEST,
               "RETIRE accepted a stale token");
  TEST_REQUIRE(sdk_image_stream_frame_retire(session, token, 0U, &ares) ==
                   SDK_STATUS_OK,
               "RETIRE rejected the live token");
  TEST_REQUIRE(sdk_image_stream_frame_retire(session, token, 0U, &ares) ==
                   SDK_STATUS_BAD_REQUEST,
               "duplicate RETIRE accepted");

  /* RETIRE before PRESENT on the next frame. The rejected call still
   * resets the shared result struct, so the token is captured first. */
  memset(&ares, 0, sizeof(ares));
  TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                   SDK_STATUS_OK,
               "second NEXT failed");
  {
    uint32_t second_token = ares.frame_token;

    TEST_REQUIRE(second_token != 0U && second_token != token,
                 "second token must differ from the first");
    TEST_REQUIRE(sdk_image_stream_frame_retire(session, second_token, 0U,
                                               &ares) ==
                     SDK_STATUS_BAD_REQUEST,
                 "RETIRE accepted a frame that was never presented");
    TEST_REQUIRE(retire_current(session, second_token, &ares),
                 "second frame cycle failed");
  }

  TEST_REQUIRE(sdk_image_stream_close(session) == SDK_STATUS_OK,
               "token close failed");
  host_runtime_reset();
  return 1;
}

/* -------------------------------------- 6: finite loops, end flag, restart */

static int test_animation_finite_loop_and_restart(void)
{
  struct SDKImageAnimationFrameResult ares;
  struct SDKImageStreamResult sres;
  struct ref_decoder ref;
  uint8_t pip[PIP_LEN];
  uint32_t session;
  uint32_t i;
  uint8_t *rgba;
  int timestamp;

  host_runtime_reset();
  TEST_REQUIRE(animation_open_and_feed(webp_anim3_finite_loop_8x6,
                                       sizeof(webp_anim3_finite_loop_8x6),
                                       pip, &sres, &session) ==
                   SDK_STATUS_OK,
               "finite session did not reach ready");
  TEST_REQUIRE(ref_open(&ref, webp_anim3_finite_loop_8x6,
                        sizeof(webp_anim3_finite_loop_8x6)) &&
                   ref.loop_count == 2U && ref.frame_count == 3U,
               "reference rejected the finite fixture");

  for (i = 0U; i < 6U; ++i) {
    uint32_t loop = i / 3U;
    uint32_t index = i % 3U;
    int is_last = (i == 5U);

    if ((i % 3U) == 0U && i != 0U) {
      /* the oracle does not loop by itself; the engine does */
      ref_reset(&ref);
    }
    TEST_REQUIRE(ref_next(&ref, &rgba, &timestamp),
                 "reference exhausted frames before the native end");
    memset(&ares, 0, sizeof(ares));
    TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                     SDK_STATUS_OK,
                 "finite NEXT failed");
    TEST_REQUIRE(ares.loop_index == loop && ares.frame_index == index &&
                     ares.loop_count == 2U,
                 "finite loop bookkeeping wrong");
    if (is_last) {
      TEST_REQUIRE((ares.flags &
                    SDK_IMAGE_ANIMATION_FRAME_FLAG_LAST_FRAME) != 0U,
                   "final frame of the final loop lacks LAST_FRAME");
    } else {
      TEST_REQUIRE((ares.flags &
                    SDK_IMAGE_ANIMATION_FRAME_FLAG_LAST_FRAME) == 0U,
                   "non-final frame wrongly flagged LAST_FRAME");
    }
    TEST_REQUIRE(pip_matches_reference(pip, rgba, CANVAS_W, CANVAS_H),
                 "finite-loop composited output mismatch");
    TEST_REQUIRE(retire_current(session, ares.frame_token, &ares),
                 "finite frame cycle failed");
  }

  /* native end: NEXT after the final retired frame reports ENDED */
  memset(&ares, 0, sizeof(ares));
  TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                   SDK_STATUS_OK,
               "post-end NEXT failed");
  TEST_REQUIRE(ares.state == SDK_IMAGE_SESSION_STATE_ANIMATION_ENDED &&
                   (ares.flags & SDK_IMAGE_ANIMATION_FRAME_FLAG_ENDED) != 0U &&
                   ares.frame_token == 0U,
               "post-end NEXT did not report ANIMATION_ENDED");
  /* idempotent: a further NEXT keeps reporting the ended state */
  TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                   SDK_STATUS_OK &&
                   ares.state == SDK_IMAGE_SESSION_STATE_ANIMATION_ENDED,
               "repeated post-end NEXT changed the ended state");

  /* explicit restart rebuilds from the retained input */
  memset(&ares, 0, sizeof(ares));
  TEST_REQUIRE(sdk_image_stream_restart(session, 0U, &ares) == SDK_STATUS_OK,
               "RESTART failed");
  TEST_REQUIRE(ares.state == SDK_IMAGE_SESSION_STATE_ANIMATION_READY &&
                   ares.loop_count == 2U && ares.frame_token == 0U,
               "RESTART lost the canvas/loop metadata");
  ref_reset(&ref);
  TEST_REQUIRE(1, "");
  TEST_REQUIRE(ref_next(&ref, &rgba, &timestamp),
               "reference could not reset for the restart check");
  memset(&ares, 0, sizeof(ares));
  TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                   SDK_STATUS_OK,
               "first NEXT after RESTART failed");
  TEST_REQUIRE(ares.loop_index == 0U && ares.frame_index == 0U,
               "RESTART did not reset the loop/frame indices");
  TEST_REQUIRE(pip_matches_reference(pip, rgba, CANVAS_W, CANVAS_H),
               "post-restart first frame output mismatch");
  TEST_REQUIRE(retire_current(session, ares.frame_token, &ares),
               "post-restart frame cycle failed");

  ref_close(&ref);
  TEST_REQUIRE(sdk_image_stream_close(session) == SDK_STATUS_OK,
               "finite close failed");
  host_runtime_reset();
  return 1;
}

/* ------------------------------------------ 7: malformed later ANMF frame */

static int test_animation_malformed_later_frame(void)
{
  struct SDKImageAnimationFrameResult ares;
  struct SDKImageStreamResult sres;
  uint8_t pip[PIP_LEN];
  uint32_t session;

  host_runtime_reset();
  TEST_REQUIRE(animation_open_and_feed(webp_anim3_malformed_8x6,
                                       sizeof(webp_anim3_malformed_8x6), pip,
                                       &sres, &session) ==
                   SDK_STATUS_OK,
               "demux of the malformed container should still succeed");

  /* frame 0 is intact */
  memset(&ares, 0, sizeof(ares));
  TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                   SDK_STATUS_OK,
               "valid first frame failed on the malformed container");
  TEST_REQUIRE(retire_current(session, ares.frame_token, &ares),
               "valid first frame cycle failed");

  /* frame 1 decodes the corrupted VP8L payload and must fail cleanly */
  TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                   SDK_STATUS_IO_ERROR,
               "corrupt frame did not fail with IO_ERROR");
  /* the failure poisons the session: further ops fail, no partial state */
  TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                   SDK_STATUS_IO_ERROR,
               "post-failure NEXT did not stay failed");
  TEST_REQUIRE(sdk_image_stream_restart(session, 0U, &ares) ==
                   SDK_STATUS_IO_ERROR,
               "post-failure RESTART did not stay failed");

  /* close still works and releases everything */
  TEST_REQUIRE(sdk_image_stream_close(session) == SDK_STATUS_OK &&
                   sdk_image_stream_active_count() == 0U,
               "close after malformed frame failed");
  TEST_REQUIRE(host_runtime_allocated_bytes() == 0U,
               "malformed session leaked host allocations");
  host_runtime_reset();
  return 1;
}

/* ------------------------------------- 8: close during ready and presented */

static int test_animation_close_releases_everything(void)
{
  struct SDKImageAnimationFrameResult ares;
  struct SDKImageStreamResult sres;
  uint8_t pip[PIP_LEN];
  uint32_t session;

  host_runtime_reset();

  /* (a) close while ANIMATION_READY, nothing outstanding */
  TEST_REQUIRE(animation_open_and_feed(webp_anim3_blend_dispose_8x6,
                                       sizeof(webp_anim3_blend_dispose_8x6),
                                       pip, &sres, &session) ==
                   SDK_STATUS_OK,
               "ready-close session did not reach ready");
  TEST_REQUIRE(sdk_decode_tracked_count() > 0U,
               "retained animation owns no tracked allocations");
  TEST_REQUIRE(sdk_image_stream_close(session) == SDK_STATUS_OK &&
                   sdk_image_stream_active_count() == 0U,
               "close during ready failed");
  TEST_REQUIRE(sdk_decode_tracked_count() == 0U &&
                   host_runtime_allocated_bytes() == 0U,
               "close during ready leaked allocations");

  /* (b) close while a frame is PRESENTED but not retired */
  TEST_REQUIRE(animation_open_and_feed(webp_anim3_blend_dispose_8x6,
                                       sizeof(webp_anim3_blend_dispose_8x6),
                                       pip, &sres, &session) ==
                   SDK_STATUS_OK,
               "presented-close session did not reach ready");
  memset(&ares, 0, sizeof(ares));
  TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                   SDK_STATUS_OK,
               "presented-close NEXT failed");
  TEST_REQUIRE(sdk_image_stream_frame_present(session, ares.frame_token, 0U,
                                              &ares) == SDK_STATUS_OK,
               "presented-close PRESENT failed");
  TEST_REQUIRE(sdk_image_stream_close(session) == SDK_STATUS_OK &&
                   sdk_image_stream_active_count() == 0U,
               "close during a presented frame failed");
  TEST_REQUIRE(sdk_decode_tracked_count() == 0U &&
                   host_runtime_allocated_bytes() == 0U,
               "close during a presented frame leaked allocations");

  host_runtime_reset();
  return 1;
}

/* -------------------------- 9: core-1 poison mid-animation, budget reuse */

static int test_animation_poison_without_uaf(void)
{
  struct SDKImageAnimationFrameResult ares;
  struct SDKImageStreamResult sres;
  struct SDKImageStreamBegin begin;
  struct SDKImageStreamFeed feed;
  uint8_t pip[PIP_LEN];
  uint8_t recovered[3U * 5U * 4U];
  uint32_t session;
  unsigned reclaimed;

  host_runtime_reset();
  TEST_REQUIRE(animation_open_and_feed(webp_anim3_blend_dispose_8x6,
                                       sizeof(webp_anim3_blend_dispose_8x6),
                                       pip, &sres, &session) ==
                   SDK_STATUS_OK,
               "poison session did not reach ready");
  memset(&ares, 0, sizeof(ares));
  TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                   SDK_STATUS_OK,
               "poison NEXT failed");
  TEST_REQUIRE(sdk_image_stream_frame_present(session, ares.frame_token, 0U,
                                              &ares) == SDK_STATUS_OK,
               "poison PRESENT failed (fault arrives mid-display)");
  TEST_REQUIRE(sdk_decode_tracked_count() > 0U,
               "poison session owns no tracked allocations");

  /* scheduler fault path: poison (no destructors!) then reclaim */
  sdk_image_stream_poison_core1_sessions();
  reclaimed = sdk_decode_reclaim(sdk_decode_heap_free);
  TEST_REQUIRE(reclaimed > 0U && sdk_decode_tracked_count() == 0U,
               "fault reclaim did not free every animation allocation");
  TEST_REQUIRE(host_runtime_allocated_bytes() == 0U,
               "fault reclaim left host bytes allocated");

  /* poisoned session: ops fail IO_ERROR, close is a plain slot reset */
  TEST_REQUIRE(sdk_image_stream_frame_retire(session, ares.frame_token, 0U,
                                             &ares) == SDK_STATUS_IO_ERROR,
               "poisoned RETIRE did not fail cleanly");
  TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                   SDK_STATUS_IO_ERROR,
               "poisoned NEXT did not fail cleanly");
  TEST_REQUIRE(sdk_image_stream_close(session) == SDK_STATUS_OK &&
                   sdk_image_stream_active_count() == 0U,
               "poisoned close did not reduce to a slot reset");

  /* the decode-state budget must be reusable by a new static decode */
  memset(recovered, PAD_BYTE, sizeof(recovered));
  begin_static_framebuffer(&begin, recovered, 3U, 5U, 3U * 4U);
  TEST_REQUIRE(sdk_image_stream_begin(&begin, &sres) == SDK_STATUS_OK,
               "budget was not reusable after the fault reclaim");
  begin_feed(&feed, sres.session, (uint32_t)sizeof(webp_lossy_3x5),
             SDK_IMAGE_SESSION_FEED_EOF);
  TEST_REQUIRE(sdk_image_stream_feed(&feed, webp_lossy_3x5, &sres) ==
                   SDK_STATUS_OK,
               "static decode after the fault reclaim failed");
  TEST_REQUIRE(sdk_image_stream_close(sres.session) == SDK_STATUS_OK,
               "post-fault static close failed");

  host_runtime_reset();
  return 1;
}

/* ------------------------------- 9: many frames, bounded tracker slots */

/* Repeats the first ANMF chunk of the 3-frame fixture `frames` times
 * behind its RIFF/VP8X/ANIM header. Returns a malloc'd container. */
static uint8_t *build_long_animation(uint32_t frames, size_t *length)
{
  const uint8_t *src = webp_anim3_blend_dispose_8x6;
  size_t src_len = sizeof(webp_anim3_blend_dispose_8x6);
  size_t off = 12U, anmf = 0U, anmf_len = 0U, total;
  uint8_t *out;
  uint32_t i;

  while (off + 8U <= src_len) {
    size_t size = (size_t)src[off + 4] | ((size_t)src[off + 5] << 8) |
                  ((size_t)src[off + 6] << 16) | ((size_t)src[off + 7] << 24);

    if (memcmp(src + off, "ANMF", 4U) == 0) {
      anmf = off;
      anmf_len = 8U + size + (size & 1U);
      break;
    }
    off += 8U + size + (size & 1U);
  }
  if (anmf_len == 0U)
    return 0;
  total = anmf + (size_t)frames * anmf_len;
  out = (uint8_t *)malloc(total);
  if (!out)
    return 0;
  memcpy(out, src, anmf);
  for (i = 0U; i < frames; ++i)
    memcpy(out + anmf + (size_t)i * anmf_len, src + anmf, anmf_len);
  out[4] = (uint8_t)(total - 8U);
  out[5] = (uint8_t)((total - 8U) >> 8);
  out[6] = (uint8_t)((total - 8U) >> 16);
  out[7] = (uint8_t)((total - 8U) >> 24);
  *length = total;
  return out;
}

/* libwebp's demuxer allocates per frame. On hardware a 72-frame clip
 * exhausted the 64-slot core-1 decode tracker and failed as out of memory;
 * the session arena must keep the tracked-block count bounded however long
 * the animation is, and every frame must still match the reference. */
static int test_animation_many_frames_bounded_slots(void)
{
  enum { FRAMES = 120 };
  struct SDKImageAnimationFrameResult ares;
  struct SDKImageStreamResult sres;
  struct ref_decoder ref;
  uint8_t pip[PIP_LEN];
  uint8_t *expected;
  uint8_t *data;
  size_t length = 0U;
  uint32_t session = 0U;
  uint32_t max_tracked = 0U;
  uint32_t i;
  int timestamp = 0;

  host_runtime_reset();
  data = build_long_animation(FRAMES, &length);
  expected = (uint8_t *)malloc((size_t)FRAMES * PIP_LEN);
  TEST_REQUIRE(data && expected, "cannot build the long animation");

  TEST_REQUIRE(ref_open(&ref, data, length) && ref.frame_count == FRAMES,
               "reference rejected the long animation");
  for (i = 0U; i < FRAMES; ++i) {
    uint8_t *rgba;

    TEST_REQUIRE(ref_next(&ref, &rgba, &timestamp),
                 "reference ran out of frames");
    memset(expected + (size_t)i * PIP_LEN, PAD_BYTE, PIP_LEN);
    ref_rgba_to_yuv422cgx(rgba, CANVAS_W, CANVAS_H,
                          expected + (size_t)i * PIP_LEN, PIP_PITCH);
  }
  ref_close(&ref);
  TEST_REQUIRE(sdk_decode_tracked_count() == 0U,
               "reference left tracked blocks behind");

  TEST_REQUIRE(animation_open_and_feed(data, length, pip, &sres, &session) ==
                   SDK_STATUS_OK,
               "long animation did not reach ready");
  for (i = 0U; i < FRAMES; ++i) {
    memset(&ares, 0, sizeof(ares));
    TEST_REQUIRE(sdk_image_stream_frame_next(session, 0U, &ares) ==
                     SDK_STATUS_OK &&
                     ares.frame_index == i,
                 "long animation NEXT failed");
    if (sdk_decode_tracked_count() > max_tracked)
      max_tracked = sdk_decode_tracked_count();
    TEST_REQUIRE(memcmp(pip, expected + (size_t)i * PIP_LEN, PIP_LEN) == 0,
                 "long animation frame mismatch");
    TEST_REQUIRE(retire_current(session, ares.frame_token, &ares),
                 "long animation frame cycle failed");
  }
  TEST_REQUIRE(max_tracked <= SDK_VORBIS_HEAP_MAX_REGIONS,
               "long animation held more tracked blocks than its arena");
  TEST_REQUIRE(sdk_image_stream_close(session) == SDK_STATUS_OK &&
                   sdk_decode_tracked_count() == 0U,
               "long animation close left tracked blocks");
  printf("long animation: %u frames, at most %u tracked blocks\n",
         (unsigned)FRAMES, (unsigned)max_tracked);
  free(expected);
  free(data);
  host_runtime_reset();
  return 1;
}

/* ------------------------------------------------------------------ main */

static int failures;

static void run_test(const char *name, int (*fn)(void))
{
  if (fn()) {
    printf("ok   %s\n", name);
  } else {
    printf("FAIL %s\n", name);
    failures++;
  }
}

int main(void)
{
  if (host_map_session_region() != 0) {
    fprintf(stderr, "cannot map the session region\n");
    return 2;
  }

  sdk_image_stream_init();
  host_runtime_reset();

  run_test("animation ops rejected before EOF",
           test_animation_rejects_ops_before_eof);
  run_test("animation EOF metadata and static rejection",
           test_animation_eof_state_and_static_rejection);
  run_test("animation NEXT matches independent reference",
           test_animation_next_matches_reference);
  run_test("animation one-outstanding-token enforcement",
           test_animation_token_enforcement);
  run_test("animation finite loop, native end and restart",
           test_animation_finite_loop_and_restart);
  run_test("animation malformed later ANMF fails cleanly",
           test_animation_malformed_later_frame);
  run_test("animation close during ready and presented",
           test_animation_close_releases_everything);
  run_test("animation core-1 poison without UAF",
           test_animation_poison_without_uaf);
  run_test("animation of 120 frames holds a bounded arena",
           test_animation_many_frames_bounded_slots);

  printf("WebP animation host peak allocated bytes: %zu\n",
         host_runtime_peak_allocated_bytes());
  return failures ? 1 : 0;
}
