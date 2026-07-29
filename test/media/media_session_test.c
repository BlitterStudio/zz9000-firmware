/*
 * Video-only MEDIA_SESSION lifecycle and exact firmware ABI mirror.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>

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

static void reset_mocks(void)
{
	begin_calls = 0U;
	write_calls = 0U;
	decode_calls = 0U;
	close_calls = 0U;
	present_calls = 0U;
	overlay_close_calls = 0U;
	mock_write_status = SDK_STATUS_OK;
	mock_bytes_accepted = 0U;
	mock_frame_number = 0U;
	mock_video_active = 0U;
	mock_present_accepted = 1;
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
	begin.audio_codec = SDK_MEDIA_AUDIO_MP2;
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
	if (sdk_media_session_status(mock_session, SDK_MEDIA_STATUS_COUNTERS + 1U,
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

int main(void)
{
	int rc;

	rc = test_exact_abi_mirror();
	if (rc != 0) return 10 + rc;
	rc = test_video_lifecycle();
	if (rc != 0) return 50 + rc;
	rc = test_errors_and_reclaim();
	if (rc != 0) return 90 + rc;
	return 0;
}
