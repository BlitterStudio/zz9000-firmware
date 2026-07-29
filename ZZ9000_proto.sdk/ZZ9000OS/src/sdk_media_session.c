/*
 * Additive video-first media session layered over sdk_video_stream.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_media_session.h"

#include "overlay.h"
#include "sdk_video_stream.h"

#include <string.h>

struct SDKMediaSessionState {
	uint32_t session;
	uint32_t state;
	uint32_t width;
	uint32_t height;
	uint32_t frame_rate_num;
	uint32_t frame_rate_den;
	uint32_t frame_number;
	uint64_t video_pts;
	uint64_t pts_origin;
	uint64_t pts_remainder;
	uint32_t bytes_accepted;
	uint32_t decoded_frames;
	uint32_t presented_frames;
	uint32_t discarded_frames;
	uint32_t just_closed_session;
	uint8_t active;
	uint8_t held;
	uint8_t header_ready;
	uint8_t close_retired;
	uint8_t present_pending;
	struct SDKMediaSessionMainResult last_close;
};

static struct SDKMediaSessionState media;

static uint32_t gcd_u32(uint32_t a, uint32_t b)
{
	while (b != 0U) {
		uint32_t next = a % b;

		a = b;
		b = next;
	}
	return a;
}

static void rate_from_milli(uint32_t milli, uint32_t *num, uint32_t *den)
{
	uint32_t divisor;

	switch (milli) {
	case 23976U:
		*num = 24000U;
		*den = 1001U;
		return;
	case 29970U:
		*num = 30000U;
		*den = 1001U;
		return;
	case 59940U:
		*num = 60000U;
		*den = 1001U;
		return;
	default:
		break;
	}
	if (milli == 0U) {
		*num = 0U;
		*den = 0U;
		return;
	}
	divisor = gcd_u32(milli, 1000U);
	*num = milli / divisor;
	*den = 1000U / divisor;
}

static uint32_t mapped_flags(uint32_t video_flags)
{
	uint32_t flags = 0U;

	if ((video_flags & SDK_VIDEO_SESSION_RESULT_HEADER_READY) != 0U)
		flags |= SDK_MEDIA_SESSION_RESULT_HEADER_READY;
	if ((video_flags & SDK_VIDEO_SESSION_RESULT_NEED_INPUT) != 0U)
		flags |= SDK_MEDIA_SESSION_RESULT_NEED_INPUT;
	if ((video_flags & SDK_VIDEO_SESSION_RESULT_FRAME_READY) != 0U)
		flags |= SDK_MEDIA_SESSION_RESULT_FRAME_HELD;
	if ((video_flags & SDK_VIDEO_SESSION_RESULT_DONE) != 0U)
		flags |= SDK_MEDIA_SESSION_RESULT_DONE;
	return flags;
}

static uint32_t mapped_state(uint32_t video_state)
{
	switch (video_state) {
	case SDK_VIDEO_SESSION_STATE_NEED_INPUT:
		return SDK_MEDIA_SESSION_STATE_NEED_INPUT;
	case SDK_VIDEO_SESSION_STATE_READY:
		return SDK_MEDIA_SESSION_STATE_READY;
	case SDK_VIDEO_SESSION_STATE_FRAME_READY:
		return SDK_MEDIA_SESSION_STATE_FRAME_HELD;
	case SDK_VIDEO_SESSION_STATE_DONE:
		return SDK_MEDIA_SESSION_STATE_DONE;
	default:
		return SDK_MEDIA_SESSION_STATE_ERROR;
	}
}

static void update_from_video(const struct SDKVideoStreamResult *video)
{
	media.width = video->width;
	media.height = video->height;
	media.frame_number = video->frame_number;
	media.bytes_accepted = video->bytes_accepted;
	rate_from_milli(video->frame_rate_milli,
	                &media.frame_rate_num, &media.frame_rate_den);
	if ((video->flags & SDK_VIDEO_SESSION_RESULT_HEADER_READY) != 0U)
		media.header_ready = 1U;
}

static uint32_t state_flags(void)
{
	uint32_t flags = 0U;

	if (media.header_ready)
		flags |= SDK_MEDIA_SESSION_RESULT_HEADER_READY;
	if (media.held)
		flags |= SDK_MEDIA_SESSION_RESULT_FRAME_HELD;
	if (media.state == SDK_MEDIA_SESSION_STATE_NEED_INPUT)
		flags |= SDK_MEDIA_SESSION_RESULT_NEED_INPUT;
	if (media.state == SDK_MEDIA_SESSION_STATE_DONE)
		flags |= SDK_MEDIA_SESSION_RESULT_DONE;
	if (media.video_pts != SDK_MEDIA_NO_PTS)
		flags |= SDK_MEDIA_SESSION_RESULT_DERIVED_TIME;
	return flags;
}

static void fill_main(uint32_t bytes_written, uint32_t extra_flags,
	                  struct SDKMediaSessionMainResult *result)
{
	memset(result, 0, sizeof(*result));
	result->session = media.session;
	result->state = media.state;
	result->width = media.width;
	result->height = media.height;
	result->frame_rate_num = media.frame_rate_num;
	result->frame_rate_den = media.frame_rate_den;
	result->frame_number = media.frame_number;
	result->video_pts = media.video_pts;
	result->bytes_accepted = media.bytes_accepted;
	result->bytes_written = bytes_written;
	result->flags = state_flags() | extra_flags;
}

static int active_session(uint32_t session)
{
	return media.active && media.session == session &&
	       sdk_video_stream_session_owner(session) ==
	           SDK_VIDEO_STREAM_OWNER_MEDIA;
}

static void advance_video_pts(const struct SDKVideoStreamResult *video)
{
	uint64_t scaled;

	if (media.decoded_frames == 0U) {
		media.video_pts = 0U;
		media.pts_origin = 0U;
		media.pts_remainder = 0U;
		return;
	}
	if (media.frame_rate_num != 0U && media.frame_rate_den != 0U) {
		scaled = UINT64_C(90000) * media.frame_rate_den +
		         media.pts_remainder;
		media.video_pts += scaled / media.frame_rate_num;
		media.pts_remainder = scaled % media.frame_rate_num;
		return;
	}
	media.video_pts = (uint64_t)video->frame_time_millis * 90U;
	if (media.pts_origin == SDK_MEDIA_NO_PTS)
		media.pts_origin = media.video_pts;
}

void sdk_media_session_init(void)
{
	memset(&media, 0, sizeof(media));
	media.video_pts = SDK_MEDIA_NO_PTS;
	media.pts_origin = SDK_MEDIA_NO_PTS;
}

int sdk_media_session_core1(uint32_t session)
{
	return active_session(session) ? 1 : -1;
}

int sdk_media_session_close_known(uint32_t session)
{
	return active_session(session) ||
	       (!media.active && media.just_closed_session == session);
}

void sdk_media_session_poison_core1(void)
{
	if (!media.active)
		return;
	/* sdk_video_stream_poison_core1_sessions() keeps the faulted slot alive
	 * specifically so CLOSE can release it without touching reclaimed decoder
	 * memory. Keep the wrapper handle for the same error/status/close path. */
	media.state = SDK_MEDIA_SESSION_STATE_ERROR;
	media.held = 0U;
	media.present_pending = 0U;
}

void sdk_media_session_present_queued(uint32_t session)
{
	if (!active_session(session) || !media.present_pending)
		return;
	/* The compose now precedes any later decode in the single-producer
	 * core-1 queue, so the decoder-owned planes may be reused safely. */
	media.present_pending = 0U;
	media.held = 0U;
	media.state = SDK_MEDIA_SESSION_STATE_READY;
}

void sdk_media_session_close_retired(uint32_t session)
{
	if (media.just_closed_session == session && !media.close_retired) {
		overlay_video_session_closed(session);
		media.close_retired = 1U;
	}
}

uint16_t sdk_media_session_begin(
	const struct SDKMediaSessionBegin *begin,
	struct SDKMediaSessionMainResult *result)
{
	struct SDKVideoStreamBegin video_begin;
	struct SDKVideoStreamResult video_result;
	uint16_t status;

	if (!begin || !result || begin->flags != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (begin->audio_codec != SDK_MEDIA_AUDIO_NONE)
		return SDK_STATUS_UNSUPPORTED;
	if (begin->pcm_ring_handle != 0U ||
	    begin->pcm_ring_capacity != 0U ||
	    begin->pcm_low_water_bytes != 0U ||
	    begin->pcm_high_water_bytes != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (media.active)
		return SDK_STATUS_NO_MEMORY;

	memset(&video_begin, 0, sizeof(video_begin));
	video_begin.codec = begin->video_codec;
	video_begin.container = begin->container;
	video_begin.width = begin->width;
	video_begin.height = begin->height;
	video_begin.output_format = begin->output_format;
	status = sdk_video_stream_begin_owned(
		&video_begin, SDK_VIDEO_STREAM_OWNER_MEDIA, &video_result);
	if (status != SDK_STATUS_OK)
		return status;

	memset(&media, 0, sizeof(media));
	media.session = video_result.session;
	media.state = mapped_state(video_result.state);
	media.video_pts = SDK_MEDIA_NO_PTS;
	media.pts_origin = SDK_MEDIA_NO_PTS;
	media.active = 1U;
	update_from_video(&video_result);
	fill_main(video_result.bytes_written,
	          mapped_flags(video_result.flags), result);
	return SDK_STATUS_OK;
}

uint16_t sdk_media_session_write(
	const struct SDKMediaSessionWrite *write,
	struct SDKMediaSessionMainResult *result)
{
	struct SDKVideoStreamWrite video_write;
	struct SDKVideoStreamResult video_result;
	uint16_t status;

	if (!write || !result || write->session == 0U ||
	    (write->flags & ~SDK_MEDIA_SESSION_WRITE_EOF) != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (!active_session(write->session))
		return SDK_STATUS_BAD_HANDLE;
	if (media.state == SDK_MEDIA_SESSION_STATE_ERROR)
		return SDK_STATUS_IO_ERROR;
	memset(&video_write, 0, sizeof(video_write));
	video_write.session = write->session;
	video_write.src = write->src;
	video_write.src_length = write->src_length;
	video_write.flags =
		(write->flags & SDK_MEDIA_SESSION_WRITE_EOF) != 0U
			? SDK_VIDEO_SESSION_WRITE_EOF : 0U;
	status = sdk_video_stream_write(&video_write, &video_result);
	if (status != SDK_STATUS_OK) {
		if (status == SDK_STATUS_IO_ERROR)
			media.state = SDK_MEDIA_SESSION_STATE_ERROR;
		return status;
	}
	update_from_video(&video_result);
	if (!media.held)
		media.state = mapped_state(video_result.state);
	fill_main(video_result.bytes_written,
	          mapped_flags(video_result.flags), result);
	return SDK_STATUS_OK;
}

uint16_t sdk_media_session_decode(
	uint32_t session, uint32_t flags,
	struct SDKMediaSessionMainResult *result)
{
	struct SDKVideoStreamDecode decode;
	struct SDKVideoStreamResult video_result;
	uint16_t status;

	if (!result || session == 0U || flags != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (!active_session(session))
		return SDK_STATUS_BAD_HANDLE;
	if (media.state == SDK_MEDIA_SESSION_STATE_ERROR)
		return SDK_STATUS_IO_ERROR;
	if (media.held) {
		fill_main(0U, SDK_MEDIA_SESSION_RESULT_BACKPRESSURE, result);
		return SDK_STATUS_BUSY;
	}
	memset(&decode, 0, sizeof(decode));
	decode.session = session;
	status = sdk_video_stream_decode(&decode, &video_result);
	if (status != SDK_STATUS_OK) {
		if (status == SDK_STATUS_IO_ERROR)
			media.state = SDK_MEDIA_SESSION_STATE_ERROR;
		return status;
	}
	update_from_video(&video_result);
	media.state = mapped_state(video_result.state);
	if ((video_result.flags & SDK_VIDEO_SESSION_RESULT_FRAME_READY) != 0U) {
		advance_video_pts(&video_result);
		media.decoded_frames++;
		media.held = 1U;
		media.state = SDK_MEDIA_SESSION_STATE_FRAME_HELD;
	}
	fill_main(video_result.bytes_written,
	          mapped_flags(video_result.flags), result);
	return SDK_STATUS_OK;
}

uint16_t sdk_media_session_present(
	uint32_t session, uint32_t flags,
	struct SDKMediaSessionMainResult *result)
{
	if (!result || session == 0U || flags != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (!active_session(session))
		return SDK_STATUS_BAD_HANDLE;
	if (media.state == SDK_MEDIA_SESSION_STATE_ERROR)
		return SDK_STATUS_IO_ERROR;
	if (!media.held)
		return SDK_STATUS_BAD_REQUEST;
	if (media.present_pending)
		return SDK_STATUS_BUSY;
	if (!overlay_video_frame_ready(session))
		return SDK_STATUS_BUSY;
	/* Keep the frame held until overlay_main_poll has actually enqueued its
	 * compose. sdk_mailbox_task may drain PRESENT and DECODE/CLOSE from one
	 * host batch before that poll runs. */
	media.present_pending = 1U;
	media.presented_frames++;
	fill_main(0U, SDK_MEDIA_SESSION_RESULT_PRESENTED, result);
	return SDK_STATUS_OK;
}

uint16_t sdk_media_session_discard(
	uint32_t session, uint32_t flags,
	struct SDKMediaSessionMainResult *result)
{
	if (!result || session == 0U || flags != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (!active_session(session))
		return SDK_STATUS_BAD_HANDLE;
	if (media.state == SDK_MEDIA_SESSION_STATE_ERROR)
		return SDK_STATUS_IO_ERROR;
	if (!media.held)
		return SDK_STATUS_BAD_REQUEST;
	if (media.present_pending)
		return SDK_STATUS_BUSY;
	media.held = 0U;
	media.discarded_frames++;
	media.state = SDK_MEDIA_SESSION_STATE_READY;
	fill_main(0U, SDK_MEDIA_SESSION_RESULT_DISCARDED, result);
	return SDK_STATUS_OK;
}

uint16_t sdk_media_session_status(
	uint32_t session, uint32_t page, uint32_t flags,
	struct SDKMediaSessionStatusResult *result)
{
	if (!result || session == 0U || flags != 0U ||
	    page > SDK_MEDIA_STATUS_COUNTERS)
		return SDK_STATUS_BAD_REQUEST;
	if (!active_session(session))
		return SDK_STATUS_BAD_HANDLE;
	memset(result, 0, sizeof(*result));
	result->session = session;
	result->state = media.state;
	result->page = page;
	if (page == SDK_MEDIA_STATUS_TIMING) {
		result->flags = media.video_pts != SDK_MEDIA_NO_PTS
			? SDK_MEDIA_SESSION_RESULT_DERIVED_TIME : 0U;
		result->value[0] = media.pts_origin;
		result->value[1] = media.video_pts;
		result->value[2] = SDK_MEDIA_NO_PTS;
		result->value[3] = SDK_MEDIA_NO_PTS;
	} else if (page == SDK_MEDIA_STATUS_COUNTERS) {
		result->value[0] = media.bytes_accepted;
		result->value[1] = media.decoded_frames;
		result->value[2] = media.presented_frames;
		result->value[3] = media.discarded_frames;
	}
	return SDK_STATUS_OK;
}

uint16_t sdk_media_session_close(
	uint32_t session, uint32_t flags,
	struct SDKMediaSessionMainResult *result)
{
	struct SDKVideoStreamResult video_result;
	uint16_t status;

	if (!result || session == 0U || flags != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (!media.active) {
		if (media.just_closed_session != session)
			return SDK_STATUS_BAD_HANDLE;
		*result = media.last_close;
		return SDK_STATUS_OK;
	}
	if (!active_session(session))
		return SDK_STATUS_BAD_HANDLE;
	if (media.present_pending)
		return SDK_STATUS_BUSY;
	status = sdk_video_stream_close(session, &video_result);
	if (status != SDK_STATUS_OK)
		return status;
	update_from_video(&video_result);
	media.state = SDK_MEDIA_SESSION_STATE_DONE;
	media.held = 0U;
	media.present_pending = 0U;
	fill_main(video_result.bytes_written,
	          SDK_MEDIA_SESSION_RESULT_DONE, result);
	media.last_close = *result;
	media.just_closed_session = session;
	media.close_retired = 0U;
	media.active = 0U;
	return SDK_STATUS_OK;
}
