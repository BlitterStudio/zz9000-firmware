/*
 * Video-only MEDIA_SESSION lifecycle and exact firmware ABI mirror.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>

#include "overlay_path.h"
#include "sdk_media_profile.h"
#include "sdk_mailbox.h"
#include "sdk_media_session.h"
#include "sdk_video_stream.h"

static uint32_t mock_session = 7U;
static uint32_t mock_frame_number;
static uint32_t begin_calls;
static uint32_t write_calls;
static uint32_t decode_calls;
static uint32_t close_calls;
static uint32_t present_calls;
static uint32_t overlay_close_calls;
static uint16_t mock_write_status;
static uint32_t mock_bytes_accepted;
static uint8_t mock_video_active;
static int mock_present_accepted;
static uint8_t mock_media_enabled;
static uint8_t mock_decoder_ready;
static uint8_t mock_audio_done;
static uint64_t mock_pcm_produced;
static uint64_t mock_pcm_acknowledged;
static uint8_t pcm_ring[32768];

static void fill_video_result(struct SDKVideoStreamResult *result,
                              uint32_t state, uint32_t flags)
{
	memset(result, 0, sizeof(*result));
	result->session = mock_session;
	result->state = state;
	result->width = 320U;
	result->height = 240U;
	result->frame_rate_milli = 25000U;
	result->frame_number = mock_frame_number;
	result->bytes_accepted = mock_bytes_accepted;
	result->flags = flags;
	result->media_pts = SDK_VIDEO_MEDIA_NO_PTS;
	result->raw_pts = SDK_VIDEO_MEDIA_NO_PTS;
}

uint16_t sdk_video_stream_begin_owned(
	const struct SDKVideoStreamBegin *begin, uint32_t owner,
	struct SDKVideoStreamResult *result)
{
	(void)begin;
	if (owner != SDK_VIDEO_STREAM_OWNER_MEDIA)
		return SDK_STATUS_BAD_REQUEST;
	if (mock_video_active)
		return SDK_STATUS_NO_MEMORY;
	begin_calls++;
	mock_media_enabled =
		begin->audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2;
	mock_video_active = 1U;
	mock_frame_number = 0U;
	mock_bytes_accepted = 0U;
	fill_video_result(result, SDK_VIDEO_SESSION_STATE_NEED_INPUT,
	                  SDK_VIDEO_SESSION_RESULT_NEED_INPUT);
	return SDK_STATUS_OK;
}

uint16_t sdk_video_stream_write(const struct SDKVideoStreamWrite *write,
	                            struct SDKVideoStreamResult *result)
{
	write_calls++;
	if (mock_write_status != SDK_STATUS_OK)
		return mock_write_status;
	mock_decoder_ready = 1U;
	mock_bytes_accepted += write->src_length;
	fill_video_result(result, SDK_VIDEO_SESSION_STATE_READY,
	                  SDK_VIDEO_SESSION_RESULT_HEADER_READY);
	return SDK_STATUS_OK;
}

uint16_t sdk_video_stream_decode(const struct SDKVideoStreamDecode *decode,
	                             struct SDKVideoStreamResult *result)
{
	(void)decode;
	decode_calls++;
	mock_decoder_ready = 1U;
	mock_frame_number++;
	fill_video_result(result, SDK_VIDEO_SESSION_STATE_FRAME_READY,
	                  SDK_VIDEO_SESSION_RESULT_HEADER_READY |
	                  SDK_VIDEO_SESSION_RESULT_FRAME_READY);
	return SDK_STATUS_OK;
}

uint16_t sdk_video_stream_close(uint32_t session,
	                            struct SDKVideoStreamResult *result)
{
	if (session != mock_session)
		return SDK_STATUS_BAD_HANDLE;
	close_calls++;
	mock_video_active = 0U;
	fill_video_result(result, SDK_VIDEO_SESSION_STATE_DONE,
	                  SDK_VIDEO_SESSION_RESULT_DONE);
	return SDK_STATUS_OK;
}

int sdk_video_stream_session_owner(uint32_t session)
{
	return mock_video_active && session == mock_session
		? (int)SDK_VIDEO_STREAM_OWNER_MEDIA : -1;
}

int sdk_video_stream_get_media_info(
	uint32_t session, struct SDKVideoMediaInfo *info)
{
	if (!mock_video_active || session != mock_session ||
	    !mock_media_enabled || !mock_decoder_ready || !info)
		return 0;
	memset(info, 0, sizeof(*info));
	info->sample_rate = 44100U;
	info->channels = 2U;
	info->sample_format = SDK_VIDEO_MEDIA_SAMPLE_S16BE;
	info->pcm_produced = mock_pcm_produced;
	info->pcm_acknowledged = mock_pcm_acknowledged;
	info->audio_pts = mock_pcm_acknowledged < mock_pcm_produced
		? 54000U + mock_pcm_acknowledged / 4U * 90000U / 44100U
		: 54000U + mock_pcm_produced / 4U * 90000U / 44100U;
	info->current_audio_pts = 54000U;
	info->first_audio_pts = 54000U;
	info->pts_origin = 48600U;
	info->raw_pts = 54000U;
	info->audio_frames = (uint32_t)(mock_pcm_produced / 4608U);
	if (mock_pcm_produced != mock_pcm_acknowledged)
		info->flags |= SDK_VIDEO_MEDIA_FLAG_AUDIO_READY;
	if (mock_audio_done)
		info->flags |= SDK_VIDEO_MEDIA_FLAG_AUDIO_DONE;
	return 1;
}

int sdk_video_stream_ack_media(uint32_t session, uint64_t acknowledged)
{
	if (!mock_video_active || session != mock_session ||
	    !mock_media_enabled)
		return 0;
	if (!mock_decoder_ready)
		return acknowledged == 0U;
	if (acknowledged < mock_pcm_acknowledged ||
	    acknowledged > mock_pcm_produced ||
	    (acknowledged & 3U) != 0U)
		return 0;
	mock_pcm_acknowledged = acknowledged;
	return 1;
}

int overlay_video_frame_ready(uint32_t session)
{
	if (session == mock_session)
		present_calls++;
	return mock_present_accepted;
}

void overlay_video_session_closed(uint32_t session)
{
	if (session == mock_session)
		overlay_close_calls++;
}

static struct overlay_path_info mock_path;
static uint32_t mock_path_query_session;

void overlay_path_snapshot(uint32_t session, struct overlay_path_info *out)
{
	mock_path_query_session = session;
	*out = mock_path;
	out->owns_session =
		(session != 0U && session == mock_session) ? 1U : 0U;
}

static void reset_mocks(void)
{
	begin_calls = 0U;
	write_calls = 0U;
	decode_calls = 0U;
	close_calls = 0U;
	present_calls = 0U;
	overlay_close_calls = 0U;
	memset(&mock_path, 0, sizeof(mock_path));
	mock_path_query_session = 0U;
	mock_write_status = SDK_STATUS_OK;
	mock_bytes_accepted = 0U;
	mock_frame_number = 0U;
	mock_video_active = 0U;
	mock_present_accepted = 1;
	mock_media_enabled = 0U;
	mock_decoder_ready = 0U;
	mock_audio_done = 0U;
	mock_pcm_produced = 0U;
	mock_pcm_acknowledged = 0U;
}

static int test_exact_abi_mirror(void)
{
	if (SDK_CAP_MEDIA_SESSION != (1U << 22)) return 1;
	if (SDK_OP_MEDIA_SESSION_BEGIN != 0x0b04U) return 2;
	if (SDK_OP_MEDIA_SESSION_WRITE != 0x0b05U) return 3;
	if (SDK_OP_MEDIA_SESSION_DECODE != 0x0b06U) return 4;
	if (SDK_OP_MEDIA_SESSION_AUDIO_READ != 0x0b07U) return 5;
	if (SDK_OP_MEDIA_SESSION_PRESENT != 0x0b08U) return 6;
	if (SDK_OP_MEDIA_SESSION_DISCARD != 0x0b09U) return 7;
	if (SDK_OP_MEDIA_SESSION_STATUS != 0x0b0aU) return 8;
	if (SDK_OP_MEDIA_SESSION_AUDIO_BIND != 0x0b0bU) return 9;
	if (SDK_OP_MEDIA_SESSION_AUDIO_UNBIND != 0x0b0cU) return 10;
	if (SDK_OP_MEDIA_SESSION_CLOSE != 0x0b0dU) return 11;
	if (sizeof(struct SDKMediaSessionBeginPayload) != 48U) return 12;
	if (sizeof(struct SDKMediaSessionWritePayload) != 48U) return 13;
	if (sizeof(struct SDKMediaSessionCommandPayload) != 48U) return 14;
	if (sizeof(struct SDKMediaSessionStatusPayload) != 48U) return 15;
	if (sizeof(struct SDKMediaSessionMainResultPayload) != 48U) return 16;
	if (sizeof(struct SDKMediaSessionAudioResultPayload) != 48U) return 17;
	if (sizeof(struct SDKMediaSessionStatusResultPayload) != 48U) return 18;
	if (SDK_MEDIA_NO_PTS != UINT64_MAX) return 19;
	if (SDK_MEDIA_AUDIO_BIND_PAUSE != (1U << 0)) return 20;
	if (SDK_MEDIA_SESSION_RESULT_AUDIO_BOUND != (1U << 11)) return 21;
	if (SDK_MEDIA_SESSION_RESULT_AUDIO_PLAYING != (1U << 12)) return 22;
	if (SDK_MEDIA_SESSION_RESULT_AUDIO_DRAINED != (1U << 13)) return 23;
	if (SDK_MEDIA_SESSION_RESULT_AUDIO_UNDERRUN != (1U << 14)) return 24;
	if (SDK_MEDIA_STATUS_AUDIO_OUTPUT != 3U) return 25;
	if (SDK_MEDIA_STATUS_PRESENTATION != 4U) return 26;
	if (SDK_MEDIA_STATUS_PROFILE != 5U) return 33;
	if (SDK_MEDIA_PRESENT_CONFIGURED != (1U << 0)) return 27;
	if (SDK_MEDIA_PRESENT_ACTIVE != (1U << 1)) return 28;
	if (SDK_MEDIA_PRESENT_NATIVE != (1U << 2)) return 29;
	if (SDK_MEDIA_PRESENT_OWNED != (1U << 3)) return 30;
	if (SDK_MEDIA_PACK_PAIR(320U, 240U) != ((320ULL << 16) | 240ULL))
		return 31;
	/* negative coordinates must survive as sign-extendable 16-bit halves */
	if (SDK_MEDIA_PACK_PAIR(-8, -1) != ((0xfff8ULL << 16) | 0xffffULL))
		return 32;
	return 0;
}

static void fill_begin(struct SDKMediaSessionBegin *begin)
{
	memset(begin, 0, sizeof(*begin));
	begin->video_codec = SDK_VIDEO_CODEC_MPEG1;
	begin->container = SDK_VIDEO_CONTAINER_MPEG_PS;
	begin->width = 320U;
	begin->height = 240U;
	begin->output_format = SDK_VIDEO_OUTPUT_DIRECT_OVERLAY;
	begin->audio_codec = SDK_MEDIA_AUDIO_NONE;
}

static int test_video_lifecycle(void)
{
	struct SDKMediaSessionBegin begin;
	struct SDKMediaSessionWrite write;
	struct SDKMediaSessionMainResult result;
	struct SDKMediaSessionStatusResult status;
	uint8_t bytes[64];
	uint16_t rc;

	reset_mocks();
	sdk_media_session_init();
	fill_begin(&begin);
	rc = sdk_media_session_begin(&begin, &result);
	if (rc != SDK_STATUS_OK || result.session != mock_session ||
	    result.state != SDK_MEDIA_SESSION_STATE_NEED_INPUT ||
	    begin_calls != 1U)
		return 1;
	if (sdk_media_session_begin(&begin, &result) != SDK_STATUS_NO_MEMORY ||
	    begin_calls != 1U)
		return 2;

	memset(&write, 0, sizeof(write));
	write.session = mock_session;
	write.src = bytes;
	write.src_length = sizeof(bytes);
	if (sdk_media_session_write(&write, &result) != SDK_STATUS_OK ||
	    result.bytes_accepted != sizeof(bytes))
		return 3;
	if (sdk_media_session_decode(mock_session, 0U, &result) != SDK_STATUS_OK ||
	    result.state != SDK_MEDIA_SESSION_STATE_FRAME_HELD ||
	    result.video_pts != 0U ||
	    (result.flags & SDK_MEDIA_SESSION_RESULT_DERIVED_TIME) == 0U)
		return 4;
	if (sdk_media_session_decode(mock_session, 0U, &result) !=
	        SDK_STATUS_BUSY ||
	    decode_calls != 1U)
		return 5;
	if (sdk_media_session_present(mock_session, 0U, &result) !=
	        SDK_STATUS_OK ||
	    present_calls != 1U ||
	    (result.flags & SDK_MEDIA_SESSION_RESULT_PRESENTED) == 0U ||
	    (result.flags & SDK_MEDIA_SESSION_RESULT_FRAME_HELD) == 0U)
		return 6;
	/* PRESENT is accepted, but the decoder frame remains owned until the
	 * compositor task is ordered ahead of subsequent decode/close. */
	if (sdk_media_session_decode(mock_session, 0U, &result) !=
	        SDK_STATUS_BUSY ||
	    sdk_media_session_close(mock_session, 0U, &result) !=
	        SDK_STATUS_BUSY ||
	    decode_calls != 1U || close_calls != 0U)
		return 7;
	sdk_media_session_present_queued(mock_session);

	if (sdk_media_session_decode(mock_session, 0U, &result) != SDK_STATUS_OK ||
	    result.video_pts != 3600U)
		return 8;
	if (sdk_media_session_discard(mock_session, 0U, &result) !=
	        SDK_STATUS_OK ||
	    present_calls != 1U ||
	    (result.flags & SDK_MEDIA_SESSION_RESULT_DISCARDED) == 0U)
		return 9;
	if (sdk_media_session_status(mock_session, SDK_MEDIA_STATUS_TIMING,
	                             0U, &status) != SDK_STATUS_OK ||
	    status.value[0] != 0U || status.value[1] != 3600U ||
	    status.value[2] != SDK_MEDIA_NO_PTS ||
	    status.value[3] != SDK_MEDIA_NO_PTS)
		return 10;
	if (sdk_media_session_status(mock_session, SDK_MEDIA_STATUS_COUNTERS,
	                             0U, &status) != SDK_STATUS_OK ||
	    status.value[0] != sizeof(bytes) || status.value[1] != 2U ||
	    status.value[2] != 1U || status.value[3] != 1U)
		return 11;
	if (sdk_media_session_status(mock_session, SDK_MEDIA_STATUS_AUDIO,
	                             0U, &status) != SDK_STATUS_OK ||
	    status.value[0] != 0U || status.value[1] != 0U)
		return 12;

	if (sdk_media_session_close(mock_session, 0U, &result) != SDK_STATUS_OK ||
	    close_calls != 1U || overlay_close_calls != 0U)
		return 13;
	sdk_media_session_close_retired(mock_session);
	if (overlay_close_calls != 1U)
		return 14;
	if (sdk_media_session_close(mock_session, 0U, &result) != SDK_STATUS_OK ||
	    close_calls != 1U)
		return 15;
	sdk_media_session_close_retired(mock_session);
	if (overlay_close_calls != 1U)
		return 16;
	if (sdk_media_session_close(mock_session + 1U, 0U, &result) !=
	    SDK_STATUS_BAD_HANDLE)
		return 17;
	return 0;
}

static int test_errors_and_reclaim(void)
{
	struct SDKMediaSessionBegin begin;
	struct SDKMediaSessionWrite write;
	struct SDKMediaSessionMainResult result;
	struct SDKMediaSessionStatusResult status;
	uint8_t byte = 0U;

	reset_mocks();
	sdk_media_session_init();
	fill_begin(&begin);
	begin.audio_codec = 2U;
	begin.pcm_ring_handle = 0x40000001U;
	begin.pcm_ring_capacity = 32768U;
	begin.pcm_low_water_bytes = 4096U;
	begin.pcm_high_water_bytes = 24576U;
	if (sdk_media_session_begin(&begin, &result) != SDK_STATUS_UNSUPPORTED ||
	    begin_calls != 0U)
		return 1;
	fill_begin(&begin);
	if (sdk_media_session_begin(&begin, &result) != SDK_STATUS_OK)
		return 2;
	if (sdk_media_session_status(mock_session,
	                             SDK_MEDIA_STATUS_PROFILE + 1U,
	                             0U, &status) != SDK_STATUS_BAD_REQUEST ||
	    sdk_media_session_status(mock_session, SDK_MEDIA_STATUS_TIMING,
	                             1U, &status) != SDK_STATUS_BAD_REQUEST)
		return 3;
	if (sdk_media_session_present(mock_session, 0U, &result) !=
	        SDK_STATUS_BAD_REQUEST ||
	    sdk_media_session_discard(mock_session, 0U, &result) !=
	        SDK_STATUS_BAD_REQUEST)
		return 4;

	/* A temporarily unavailable overlay must not release decoder-owned
	 * planes; the client can discard the still-held frame or retry present. */
	if (sdk_media_session_decode(mock_session, 0U, &result) != SDK_STATUS_OK)
		return 5;
	mock_present_accepted = 0;
	if (sdk_media_session_present(mock_session, 0U, &result) !=
	        SDK_STATUS_BUSY ||
	    sdk_media_session_discard(mock_session, 0U, &result) !=
	        SDK_STATUS_OK)
		return 6;
	mock_present_accepted = 1;

	memset(&write, 0, sizeof(write));
	write.session = mock_session;
	write.src = &byte;
	write.src_length = 1U;
	mock_write_status = SDK_STATUS_IO_ERROR;
	if (sdk_media_session_write(&write, &result) != SDK_STATUS_IO_ERROR)
		return 7;
	if (sdk_media_session_close(mock_session, 0U, &result) != SDK_STATUS_OK)
		return 8;

	fill_begin(&begin);
	mock_write_status = SDK_STATUS_OK;
	if (sdk_media_session_begin(&begin, &result) != SDK_STATUS_OK)
		return 9;
	sdk_media_session_poison_core1();
	if (sdk_media_session_status(mock_session, SDK_MEDIA_STATUS_TIMING,
	                             0U, &status) != SDK_STATUS_OK ||
	    status.state != SDK_MEDIA_SESSION_STATE_ERROR)
		return 10;
	if (sdk_media_session_write(&write, &result) != SDK_STATUS_IO_ERROR ||
	    write_calls != 1U)
		return 11;
	if (sdk_media_session_decode(mock_session, 0U, &result) !=
	        SDK_STATUS_IO_ERROR ||
	    decode_calls != 1U)
		return 12;
	if (sdk_media_session_close(mock_session, 0U, &result) !=
	        SDK_STATUS_OK ||
	    close_calls != 2U)
		return 13;
	sdk_media_session_close_retired(mock_session);
	if (overlay_close_calls != 1U)
		return 14;
	if (sdk_media_session_begin(&begin, &result) != SDK_STATUS_OK ||
	    begin_calls != 3U)
		return 15;
	return 0;
}

/* R10 presentation-path honesty: the overlay chooses native scanout versus
 * the shadow compositor internally, so page 4 is the player's only truthful
 * source. Geometry travels as raw 16-bit halves; classification is host-side. */
static int test_presentation_status_page(void)
{
	struct SDKMediaSessionBegin begin;
	struct SDKMediaSessionMainResult result;
	struct SDKMediaSessionStatusResult status;

	reset_mocks();
	sdk_media_session_init();
	fill_begin(&begin);
	if (sdk_media_session_begin(&begin, &result) != SDK_STATUS_OK)
		return 1;

	/* An unconfigured overlay reports no path at all rather than failing. */
	if (sdk_media_session_status(mock_session,
	                             SDK_MEDIA_STATUS_PRESENTATION, 0U,
	                             &status) != SDK_STATUS_OK)
		return 2;
	if (status.page != SDK_MEDIA_STATUS_PRESENTATION ||
	    (status.flags & SDK_MEDIA_PRESENT_CONFIGURED) != 0U ||
	    (status.flags & SDK_MEDIA_PRESENT_ACTIVE) != 0U ||
	    (status.flags & SDK_MEDIA_PRESENT_NATIVE) != 0U)
		return 3;
	if (mock_path_query_session != mock_session)
		return 4;

	/* Native 1:1: destination equals source, nothing clipped. */
	mock_path.configured = 1U;
	mock_path.active = 1U;
	mock_path.hw_active = 1U;
	mock_path.src_w = 320U;
	mock_path.src_h = 240U;
	mock_path.dst_x = 40;
	mock_path.dst_y = 20;
	mock_path.dst_w = 320;
	mock_path.dst_h = 240;
	mock_path.screen_w = 640U;
	mock_path.screen_h = 480U;
	if (sdk_media_session_status(mock_session,
	                             SDK_MEDIA_STATUS_PRESENTATION, 0U,
	                             &status) != SDK_STATUS_OK)
		return 5;
	if (status.flags != (SDK_MEDIA_PRESENT_CONFIGURED |
	                     SDK_MEDIA_PRESENT_ACTIVE |
	                     SDK_MEDIA_PRESENT_NATIVE |
	                     SDK_MEDIA_PRESENT_OWNED))
		return 6;
	if (status.value[0] != SDK_MEDIA_PACK_PAIR(320U, 240U) ||
	    status.value[1] != SDK_MEDIA_PACK_PAIR(320U, 240U) ||
	    status.value[2] != SDK_MEDIA_PACK_PAIR(40U, 20U) ||
	    status.value[3] != SDK_MEDIA_PACK_PAIR(640U, 480U))
		return 7;

	/* Native scaled and clipped off the left/top edge: negative origins
	 * must survive the packing as recoverable two's-complement halves. */
	mock_path.dst_x = -16;
	mock_path.dst_y = -9;
	mock_path.dst_w = 640;
	mock_path.dst_h = 480;
	if (sdk_media_session_status(mock_session,
	                             SDK_MEDIA_STATUS_PRESENTATION, 0U,
	                             &status) != SDK_STATUS_OK)
		return 8;
	if (status.value[1] != SDK_MEDIA_PACK_PAIR(640U, 480U) ||
	    status.value[2] != ((0xfff0ULL << 16) | 0xfff7ULL))
		return 9;

	/* Software fallback: still active, but not natively scanned. */
	mock_path.hw_active = 0U;
	if (sdk_media_session_status(mock_session,
	                             SDK_MEDIA_STATUS_PRESENTATION, 0U,
	                             &status) != SDK_STATUS_OK)
		return 10;
	if ((status.flags & SDK_MEDIA_PRESENT_NATIVE) != 0U ||
	    (status.flags & SDK_MEDIA_PRESENT_ACTIVE) == 0U)
		return 11;

	/* The page bound moved by exactly one: 5 is still rejected, and a
	 * non-zero flags word remains rejected on the new page. */
	if (sdk_media_session_status(mock_session, 6U, 0U, &status) !=
	        SDK_STATUS_BAD_REQUEST)
		return 12;
	if (sdk_media_session_status(mock_session,
	                             SDK_MEDIA_STATUS_PRESENTATION, 1U,
	                             &status) != SDK_STATUS_BAD_REQUEST)
		return 13;
	if (sdk_media_session_status(mock_session + 1U,
	                             SDK_MEDIA_STATUS_PRESENTATION, 0U,
	                             &status) != SDK_STATUS_BAD_HANDLE)
		return 14;
	return 0;
}

void sdk_media_profile_host_set_now(uint32_t now_us);

/* U7: per-stage timing must accumulate per stage, survive a 32-bit timer
 * wrap, and reach the host packed as (microseconds << 32) | calls. */
static int test_profile_status_page(void)
{
	struct SDKMediaSessionBegin begin;
	struct SDKMediaSessionMainResult result;
	struct SDKMediaSessionStatusResult status;
	uint32_t start;

	reset_mocks();
	sdk_media_session_init();
	fill_begin(&begin);
	if (sdk_media_session_begin(&begin, &result) != SDK_STATUS_OK)
		return 1;

	/* sdk_media_session_init() must have zeroed the accumulators, so a
	 * second run in the same boot is not read as a continuation. */
	if (sdk_media_session_status(mock_session, SDK_MEDIA_STATUS_PROFILE,
	                             0U, &status) != SDK_STATUS_OK)
		return 2;
	if (status.value[SDK_MEDIA_PROFILE_YUY2_PACK] != 0U)
		return 3;

	/* One 1500 us pack. */
	sdk_media_profile_host_set_now(1000U);
	start = sdk_media_profile_now_us();
	sdk_media_profile_host_set_now(2500U);
	sdk_media_profile_record(SDK_MEDIA_PROFILE_YUY2_PACK, start);

	/* A second, 500 us, so the accumulation is visible. */
	sdk_media_profile_host_set_now(3000U);
	start = sdk_media_profile_now_us();
	sdk_media_profile_host_set_now(3500U);
	sdk_media_profile_record(SDK_MEDIA_PROFILE_YUY2_PACK, start);

	if (sdk_media_session_status(mock_session, SDK_MEDIA_STATUS_PROFILE,
	                             0U, &status) != SDK_STATUS_OK)
		return 4;
	if ((status.value[SDK_MEDIA_PROFILE_YUY2_PACK] >> 32) != 2000U)
		return 5;
	if ((status.value[SDK_MEDIA_PROFILE_YUY2_PACK] & 0xffffffffU) != 2U)
		return 6;
	/* Stages must not bleed into one another. */
	if (status.value[SDK_MEDIA_PROFILE_VIDEO_DECODE] != 0U)
		return 7;

	/* A start straddling the 32-bit timer wrap still yields the true
	 * elapsed time, because the subtraction is unsigned. */
	sdk_media_profile_host_set_now(0xfffffe00U);
	start = sdk_media_profile_now_us();
	sdk_media_profile_host_set_now(0x00000200U);
	sdk_media_profile_record(SDK_MEDIA_PROFILE_VIDEO_DECODE, start);
	if (sdk_media_session_status(mock_session, SDK_MEDIA_STATUS_PROFILE,
	                             0U, &status) != SDK_STATUS_OK)
		return 8;
	if ((status.value[SDK_MEDIA_PROFILE_VIDEO_DECODE] >> 32) != 1024U)
		return 9;

	/* An unmeasured build reports nothing rather than nonsense. */
	sdk_media_profile_record(SDK_MEDIA_PROFILE_PRESENT, 0U);
	if (sdk_media_session_status(mock_session, SDK_MEDIA_STATUS_PROFILE,
	                             0U, &status) != SDK_STATUS_OK)
		return 10;
	if (status.value[SDK_MEDIA_PROFILE_PRESENT] != 0U)
		return 11;

	/* Out-of-range stages are ignored, not written past the array. */
	sdk_media_profile_record(SDK_MEDIA_PROFILE_STAGES, 1U);
	sdk_media_profile_record(99U, 1U);
	if (sdk_media_profile_read(SDK_MEDIA_PROFILE_STAGES, 0, 0) != 0)
		return 12;
	return 0;
}

static int test_mp2_audio_snapshot_and_ack(void)
{
	struct SDKMediaSessionBegin begin;
	struct SDKMediaSessionMainResult result;
	struct SDKMediaSessionAudioResult audio;
	struct SDKMediaSessionStatusResult status;

	reset_mocks();
	sdk_media_session_init();
	fill_begin(&begin);
	begin.audio_codec = SDK_MEDIA_AUDIO_MP2;
	begin.pcm_ring_handle = 0x40000001U;
	begin.pcm_ring = pcm_ring;
	begin.pcm_ring_capacity = sizeof(pcm_ring);
	begin.pcm_low_water_bytes = 4608U;
	begin.pcm_high_water_bytes = 24576U;
	if (sdk_media_session_begin(&begin, &result) != SDK_STATUS_OK ||
	    !mock_media_enabled)
		return 1;
	if (sdk_media_session_audio_read(mock_session, 0U, 0U, &audio) !=
	        SDK_STATUS_OK ||
	    audio.sample_rate != 0U || audio.channels != 0U ||
	    audio.sample_format != 0U || audio.pcm_produced != 0U ||
	    audio.pcm_acknowledged != 0U || audio.audio_pts != SDK_MEDIA_NO_PTS)
		return 2;
	mock_pcm_produced = 9216U;
	if (sdk_media_session_decode(mock_session, 0U, &result) !=
	        SDK_STATUS_OK ||
	    (result.flags & SDK_MEDIA_SESSION_RESULT_AUDIO_READY) == 0U)
		return 3;
	if (sdk_media_session_status(mock_session, SDK_MEDIA_STATUS_AUDIO,
	                             0U, &status) != SDK_STATUS_OK ||
	    status.value[0] != 9216U || status.value[1] != 0U ||
	    status.value[2] != 54000U || status.value[3] != 54000U)
		return 4;
	if (sdk_media_session_audio_read(mock_session, 4U, 0U, &audio) !=
	        SDK_STATUS_OK ||
	    audio.sample_rate != 44100U || audio.channels != 2U ||
	    audio.sample_format != SDK_VIDEO_MEDIA_SAMPLE_S16BE ||
	    audio.pcm_acknowledged != 4U ||
	    audio.audio_pts != 54002U)
		return 5;
	if (sdk_media_session_audio_read(mock_session, 2U, 0U, &audio) !=
	        SDK_STATUS_BAD_REQUEST ||
	    sdk_media_session_audio_read(mock_session, 9217U, 0U, &audio) !=
	        SDK_STATUS_BAD_REQUEST)
		return 6;
	return 0;
}

static int test_mp2_direct_audio_output(void)
{
	struct SDKMediaSessionBegin begin;
	struct SDKMediaSessionMainResult result;
	struct SDKMediaSessionAudioResult audio;
	struct SDKMediaSessionStatusResult status;
	struct SDKMediaAudioSource source;
	uint64_t expected_pts;

	reset_mocks();
	sdk_media_session_init();
	fill_begin(&begin);
	begin.audio_codec = SDK_MEDIA_AUDIO_MP2;
	begin.pcm_ring_handle = 0x40000001U;
	begin.pcm_ring = pcm_ring;
	begin.pcm_ring_capacity = sizeof(pcm_ring);
	begin.pcm_low_water_bytes = 4608U;
	begin.pcm_high_water_bytes = 24576U;
	if (sdk_media_session_begin(&begin, &result) != SDK_STATUS_OK)
		return 1;
	/* Format discovery is a precondition: BIND does not cross from core 0
	 * into the decoder objects owned by core 1. */
	if (sdk_media_session_audio_bind(mock_session, 0U, &audio) !=
	        SDK_STATUS_BAD_REQUEST)
		return 2;

	mock_pcm_produced = 9216U;
	if (sdk_media_session_decode(mock_session, 0U, &result) !=
	        SDK_STATUS_OK ||
	    sdk_media_session_discard(mock_session, 0U, &result) !=
	        SDK_STATUS_OK)
		return 3;
	if (sdk_media_session_audio_bind(mock_session, 0U, &audio) !=
	        SDK_STATUS_OK ||
	    (audio.flags & (SDK_MEDIA_SESSION_RESULT_AUDIO_BOUND |
	                    SDK_MEDIA_SESSION_RESULT_AUDIO_PLAYING)) !=
	        (SDK_MEDIA_SESSION_RESULT_AUDIO_BOUND |
	         SDK_MEDIA_SESSION_RESULT_AUDIO_PLAYING) ||
	    audio.pcm_acknowledged != 0U ||
	    audio.audio_pts != 54000U)
		return 4;
	if (sdk_media_session_audio_read(mock_session, 0U, 0U, &audio) !=
	        SDK_STATUS_BUSY ||
	    sdk_media_session_close(mock_session, 0U, &result) !=
	        SDK_STATUS_BUSY)
		return 5;

	if (!sdk_media_session_audio_source(mock_session, &source) ||
	    source.ring != pcm_ring || source.capacity != sizeof(pcm_ring) ||
	    source.produced_bytes != 9216U || source.staged_bytes != 0U ||
	    source.sample_rate != 44100U || source.channels != 2U ||
	    source.sample_format != SDK_VIDEO_MEDIA_SAMPLE_S16BE)
		return 6;
	if (sdk_media_session_audio_stage(mock_session, 2U) ||
	    !sdk_media_session_audio_stage(mock_session, 4608U) ||
	    sdk_media_session_audio_stage(mock_session, 4609U))
		return 7;
	if (!sdk_media_session_audio_source(mock_session, &source) ||
	    source.staged_bytes != 4608U)
		return 8;
	if (sdk_media_session_audio_retire(mock_session, 2U) ||
	    !sdk_media_session_audio_retire(mock_session, 2304U) ||
	    sdk_media_session_audio_retire(mock_session, 2308U))
		return 9;
	expected_pts = 54000U + (576U * UINT64_C(90000)) / 44100U;
	/* UNBIND runs inline on core 0, before a core-1 operation can publish
	 * the retired cursor back into the decoder snapshot. Rebinding must
	 * continue from retirement rather than replaying those samples. */
	if (sdk_media_session_audio_unbind(mock_session, 0U, &audio) !=
	        SDK_STATUS_OK ||
	    audio.pcm_acknowledged != 2304U ||
	    audio.audio_pts != expected_pts ||
	    sdk_media_session_audio_bind(mock_session, 0U, &audio) !=
	        SDK_STATUS_OK ||
	    audio.pcm_acknowledged != 2304U ||
	    !sdk_media_session_audio_source(mock_session, &source) ||
	    source.staged_bytes != 2304U ||
	    !sdk_media_session_audio_stage(mock_session, 2304U))
		return 10;
	if (sdk_media_session_audio_bind(mock_session, 0U, &audio) !=
	        SDK_STATUS_OK ||
	    audio.pcm_acknowledged != 2304U ||
	    audio.audio_pts != expected_pts)
		return 11;
	if (sdk_media_session_status(mock_session,
	                             SDK_MEDIA_STATUS_AUDIO_OUTPUT,
	                             0U, &status) != SDK_STATUS_OK ||
	    status.value[0] != 576U || status.value[1] != 576U ||
	    status.value[2] != 1152U || status.value[3] != 0U)
		return 12;

	sdk_media_session_audio_underrun(mock_session);
	memset(&source, 0, sizeof(source));
	if (sdk_media_session_audio_bind(mock_session,
	                                 SDK_MEDIA_AUDIO_BIND_PAUSE,
	                                 &audio) != SDK_STATUS_OK ||
	    (audio.flags & SDK_MEDIA_SESSION_RESULT_AUDIO_PLAYING) != 0U ||
	    sdk_media_session_audio_source(mock_session, &source))
		return 13;
	/* A paused source is intentionally unavailable to the ISR. */
	if (sdk_media_session_audio_stage(mock_session, 4U))
		return 14;
	if (sdk_media_session_audio_bind(mock_session, 0U, &audio) !=
	        SDK_STATUS_OK ||
	    (audio.flags & SDK_MEDIA_SESSION_RESULT_AUDIO_PLAYING) == 0U ||
	    !sdk_media_session_audio_source(mock_session, &source) ||
	    source.staged_bytes != 2304U ||
	    (audio.flags & SDK_MEDIA_SESSION_RESULT_AUDIO_UNDERRUN) == 0U)
		return 15;

	if (!sdk_media_session_audio_stage(mock_session, 6912U) ||
	    !sdk_media_session_audio_retire(mock_session, 6912U))
		return 16;
	mock_audio_done = 1U;
	/* A core-1 operation publishes AUDIO_DONE and acknowledges retirement. */
	if (sdk_media_session_decode(mock_session, 0U, &result) !=
	        SDK_STATUS_OK ||
	    sdk_media_session_discard(mock_session, 0U, &result) !=
	        SDK_STATUS_OK ||
	    mock_pcm_acknowledged != 9216U ||
	    sdk_media_session_audio_bind(mock_session, 0U, &audio) !=
	        SDK_STATUS_OK ||
	    (audio.flags & SDK_MEDIA_SESSION_RESULT_AUDIO_DRAINED) == 0U)
		return 17;
	if (sdk_media_session_audio_unbind(mock_session, 0U, &audio) !=
	        SDK_STATUS_OK ||
	    (audio.flags & SDK_MEDIA_SESSION_RESULT_AUDIO_BOUND) != 0U)
		return 18;
	if (sdk_media_session_audio_read(mock_session, 9216U, 0U, &audio) !=
	        SDK_STATUS_OK)
		return 19;
	return 0;
}

static int test_mp2_direct_audio_cursor_rollover(void)
{
	struct SDKMediaSessionBegin begin;
	struct SDKMediaSessionMainResult result;
	struct SDKMediaSessionAudioResult audio;
	struct SDKMediaAudioSource source;
	const uint64_t start = UINT64_C(0xfffffffc);

	reset_mocks();
	sdk_media_session_init();
	fill_begin(&begin);
	begin.audio_codec = SDK_MEDIA_AUDIO_MP2;
	begin.pcm_ring_handle = 0x40000001U;
	begin.pcm_ring = pcm_ring;
	begin.pcm_ring_capacity = sizeof(pcm_ring);
	begin.pcm_low_water_bytes = 4608U;
	begin.pcm_high_water_bytes = 24576U;
	if (sdk_media_session_begin(&begin, &result) != SDK_STATUS_OK)
		return 1;
	mock_pcm_acknowledged = start;
	mock_pcm_produced = start + 9216U;
	if (sdk_media_session_decode(mock_session, 0U, &result) !=
	        SDK_STATUS_OK ||
	    sdk_media_session_discard(mock_session, 0U, &result) !=
	        SDK_STATUS_OK ||
	    sdk_media_session_audio_bind(mock_session, 0U, &audio) !=
	        SDK_STATUS_OK ||
	    audio.pcm_acknowledged != start)
		return 2;
	if (!sdk_media_session_audio_source(mock_session, &source) ||
	    source.produced_bytes != start + 9216U ||
	    source.staged_bytes != start ||
	    !sdk_media_session_audio_stage(mock_session, 4608U) ||
	    !sdk_media_session_audio_retire(mock_session, 4608U) ||
	    !sdk_media_session_audio_source(mock_session, &source) ||
	    source.staged_bytes != start + 4608U)
		return 3;
	if (sdk_media_session_decode(mock_session, 0U, &result) !=
	        SDK_STATUS_OK ||
	    sdk_media_session_discard(mock_session, 0U, &result) !=
	        SDK_STATUS_OK ||
	    mock_pcm_acknowledged != start + 4608U)
		return 4;
	return 0;
}

int main(void)
{
	int rc;

	rc = test_exact_abi_mirror();
	if (rc != 0) return 10 + rc;
	rc = test_video_lifecycle();
	if (rc != 0) return 50 + rc;
	rc = test_errors_and_reclaim();
	if (rc != 0) return 90 + rc;
	rc = test_presentation_status_page();
	if (rc != 0) return 230 + rc;
	rc = test_profile_status_page();
	if (rc != 0) return 245 + rc;
	rc = test_mp2_audio_snapshot_and_ack();
	if (rc != 0) return 130 + rc;
	rc = test_mp2_direct_audio_output();
	if (rc != 0) return 170 + rc;
	rc = test_mp2_direct_audio_cursor_rollover();
	if (rc != 0) return 210 + rc;
	return 0;
}
