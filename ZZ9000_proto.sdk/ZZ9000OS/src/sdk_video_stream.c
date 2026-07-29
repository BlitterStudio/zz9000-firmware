/*
 * Codec-neutral streaming video session manager.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_video_stream.h"

#include "memorymap.h"
#include "overlay.h"
#include "sdk_video_backend.h"

#include <string.h>
#include <xil_cache.h>

#define SDK_VIDEO_MAX_WIDTH 1920U
#define SDK_VIDEO_MAX_HEIGHT 1080U
#define SDK_VIDEO_MAX_PIXELS (SDK_VIDEO_MAX_WIDTH * SDK_VIDEO_MAX_HEIGHT)
#define SDK_VIDEO_MAX_WRITE_BYTES (256U * 1024U)

struct SDKVideoStreamSession {
	uint32_t id;
	uint32_t codec;
	uint32_t container;
	uint32_t width;
	uint32_t height;
	uint32_t output_format;
	uint32_t frame_rate_milli;
	uint32_t frame_number;
	uint32_t frame_time_millis;
	uint32_t bytes_accepted;
	struct SDKVideoMediaConfig media;
	const struct SDKVideoDecoderOps *ops;
	void *decoder;
	struct SDKVideoDecodedFrame direct_frame;
	uint8_t input_eof;
	uint8_t failed;
	uint8_t faulted;
	uint8_t in_use;
	uint8_t direct_frame_valid;
	uint8_t owner;
};

static struct SDKVideoStreamSession *const video_sessions =
	(struct SDKVideoStreamSession *)SDK_VIDEO_SESSIONS_ADDRESS;
static uint32_t next_video_session_id;

typedef char video_sessions_fit_check[
	(SDK_MAX_VIDEO_SESSIONS * sizeof(struct SDKVideoStreamSession) <=
	 SDK_VIDEO_SESSIONS_MAX_BYTES) ? 1 : -1];

static struct SDKVideoStreamSession *find_session(uint32_t id)
{
	uint32_t i;

	for (i = 0U; i < SDK_MAX_VIDEO_SESSIONS; i++) {
		if (video_sessions[i].in_use && video_sessions[i].id == id)
			return &video_sessions[i];
	}
	return 0;
}

static struct SDKVideoStreamSession *find_free_session(void)
{
	uint32_t i;

	for (i = 0U; i < SDK_MAX_VIDEO_SESSIONS; i++) {
		if (!video_sessions[i].in_use)
			return &video_sessions[i];
	}
	return 0;
}

static uint32_t next_session_id(void)
{
	uint32_t id = next_video_session_id++;

	if (next_video_session_id == 0U || next_video_session_id == 0xffffffffU)
		next_video_session_id = 1U;
	if (id == 0U || id == 0xffffffffU)
		return next_session_id();
	return id;
}

static void fill_result(const struct SDKVideoStreamSession *session,
	                    uint32_t state, uint32_t bytes_written,
	                    uint32_t flags,
	                    struct SDKVideoStreamResult *result)
{
	memset(result, 0, sizeof(*result));
	result->session = session->id;
	result->state = state;
	result->width = session->width;
	result->height = session->height;
	result->frame_rate_milli = session->frame_rate_milli;
	result->frame_number = session->frame_number;
	result->frame_time_millis = session->frame_time_millis;
	result->bytes_accepted = session->bytes_accepted;
	result->bytes_written = bytes_written;
	result->flags = flags;
	result->media_pts = SDK_VIDEO_MEDIA_NO_PTS;
	result->raw_pts = SDK_VIDEO_MEDIA_NO_PTS;
}

static void update_info(struct SDKVideoStreamSession *session)
{
	struct SDKVideoDecoderInfo info;

	if (!session->decoder ||
	    !session->ops->get_info(session->decoder, &info))
		return;
	if (info.width == session->width && info.height == session->height)
		session->frame_rate_milli = info.frame_rate_milli;
}

static uint16_t ensure_decoder(struct SDKVideoStreamSession *session)
{
	if (session->faulted || session->failed)
		return SDK_STATUS_IO_ERROR;
	if (session->decoder)
		return SDK_STATUS_OK;
	session->decoder = session->ops->create();
	if (!session->decoder)
		return SDK_STATUS_NO_MEMORY;
	if (session->owner == SDK_VIDEO_STREAM_OWNER_MEDIA) {
		if (!session->ops->configure_media ||
		    !session->ops->configure_media(
			    session->decoder, &session->media)) {
			session->ops->destroy(session->decoder);
			session->decoder = 0;
			return SDK_STATUS_UNSUPPORTED;
		}
	}
	return SDK_STATUS_OK;
}

void sdk_video_stream_init(void)
{
	uint32_t bytes =
		SDK_MAX_VIDEO_SESSIONS * sizeof(struct SDKVideoStreamSession);

	memset(video_sessions, 0, bytes);
	Xil_DCacheFlushRange((INTPTR)(uintptr_t)video_sessions, bytes);
	next_video_session_id = 1U;
}

uint32_t sdk_video_stream_active_count(void)
{
	uint32_t count = 0U;
	uint32_t i;

	for (i = 0U; i < SDK_MAX_VIDEO_SESSIONS; i++) {
		if (video_sessions[i].in_use)
			count++;
	}
	return count;
}

int sdk_video_stream_session_core1(uint32_t session)
{
	return find_session(session) ? 1 : -1;
}

int sdk_video_stream_session_owner(uint32_t session)
{
	struct SDKVideoStreamSession *found = find_session(session);

	return found ? (int)found->owner : -1;
}

uint32_t sdk_video_stream_session_height(uint32_t session)
{
	struct SDKVideoStreamSession *found = find_session(session);

	return found ? found->height : 0U;
}

int sdk_video_stream_has_core1_sessions(void)
{
	return sdk_video_stream_active_count() != 0U;
}

void sdk_video_stream_poison_core1_sessions(void)
{
	uint32_t i;

	for (i = 0U; i < SDK_MAX_VIDEO_SESSIONS; i++) {
		struct SDKVideoStreamSession *session = &video_sessions[i];

		if (!session->in_use)
			continue;
		/* The decode-reclaim pass already freed the backend graph. */
		session->decoder = 0;
		memset(&session->direct_frame, 0, sizeof(session->direct_frame));
		session->direct_frame_valid = 0U;
		session->faulted = 1U;
		session->failed = 1U;
		/* Poisoning runs on core 0. Detach before any pending overlay poll can
		 * reuse decoder-owned planes that the reclaim pass just freed. */
		overlay_video_session_closed(session->id);
	}
}

int sdk_video_stream_get_direct_frame(uint32_t id,
	                                  struct SDKVideoDecodedFrame *frame)
{
	struct SDKVideoStreamSession *session = find_session(id);

	if (!session || !frame || session->failed || session->faulted ||
	    !session->direct_frame_valid)
		return 0;
	*frame = session->direct_frame;
	return 1;
}

int sdk_video_stream_get_media_info(
	uint32_t id, struct SDKVideoMediaInfo *info)
{
	struct SDKVideoStreamSession *session = find_session(id);

	if (!session || !info || session->owner != SDK_VIDEO_STREAM_OWNER_MEDIA ||
	    !session->decoder || !session->ops->get_media_info)
		return 0;
	return session->ops->get_media_info(session->decoder, info);
}

int sdk_video_stream_ack_media(uint32_t id, uint64_t acknowledged)
{
	struct SDKVideoStreamSession *session = find_session(id);

	if (!session || session->owner != SDK_VIDEO_STREAM_OWNER_MEDIA ||
	    !session->decoder || !session->ops->ack_media)
		return 0;
	return session->ops->ack_media(session->decoder, acknowledged);
}

uint16_t sdk_video_stream_begin(const struct SDKVideoStreamBegin *begin,
	                            struct SDKVideoStreamResult *result)
{
	return sdk_video_stream_begin_owned(
		begin, SDK_VIDEO_STREAM_OWNER_LEGACY, result);
}

uint16_t sdk_video_stream_begin_owned(
	const struct SDKVideoStreamBegin *begin, uint32_t owner,
	struct SDKVideoStreamResult *result)
{
	const struct SDKVideoDecoderOps *ops;
	struct SDKVideoStreamSession *session;
	if (!begin || !result ||
	    (owner != SDK_VIDEO_STREAM_OWNER_LEGACY &&
	     owner != SDK_VIDEO_STREAM_OWNER_MEDIA) ||
	    begin->flags != 0U ||
	    begin->output_format != SDK_VIDEO_OUTPUT_DIRECT_OVERLAY ||
	    begin->width == 0U || begin->height == 0U ||
	    begin->width > SDK_VIDEO_MAX_WIDTH ||
	    begin->height > SDK_VIDEO_MAX_HEIGHT ||
	    begin->width > SDK_VIDEO_MAX_PIXELS / begin->height)
		return SDK_STATUS_BAD_REQUEST;
	if (owner == SDK_VIDEO_STREAM_OWNER_LEGACY &&
	    (begin->audio_codec != SDK_VIDEO_MEDIA_AUDIO_NONE ||
	     begin->pcm_ring || begin->pcm_ring_capacity != 0U ||
	     begin->pcm_low_water_bytes != 0U ||
	     begin->pcm_high_water_bytes != 0U))
		return SDK_STATUS_BAD_REQUEST;
	if (owner == SDK_VIDEO_STREAM_OWNER_MEDIA &&
	    begin->audio_codec != SDK_VIDEO_MEDIA_AUDIO_NONE &&
	    begin->audio_codec != SDK_VIDEO_MEDIA_AUDIO_MP2)
		return SDK_STATUS_UNSUPPORTED;
	ops = sdk_video_backend_find(begin->codec, begin->container);
	if (!ops)
		return SDK_STATUS_UNSUPPORTED;
	session = find_free_session();
	if (!session)
		return SDK_STATUS_NO_MEMORY;

	memset(session, 0, sizeof(*session));
	session->id = next_session_id();
	session->codec = begin->codec;
	session->container = begin->container;
	session->width = begin->width;
	session->height = begin->height;
	session->output_format = begin->output_format;
	session->ops = ops;
	session->media.audio_codec = begin->audio_codec;
	session->media.pcm_ring = begin->pcm_ring;
	session->media.pcm_ring_capacity = begin->pcm_ring_capacity;
	session->media.pcm_low_water_bytes =
		begin->pcm_low_water_bytes;
	session->media.pcm_high_water_bytes =
		begin->pcm_high_water_bytes;
	session->owner = (uint8_t)owner;
	session->in_use = 1U;
	fill_result(session, SDK_VIDEO_SESSION_STATE_NEED_INPUT, 0U,
	            SDK_VIDEO_SESSION_RESULT_NEED_INPUT, result);
	return SDK_STATUS_OK;
}

uint16_t sdk_video_stream_write(const struct SDKVideoStreamWrite *write,
	                            struct SDKVideoStreamResult *result)
{
	struct SDKVideoStreamSession *session;
	uint32_t accepted = 0U;
	uint16_t status;
	uint32_t result_flags;
	uint32_t state;

	if (!write || !result || write->session == 0U ||
	    (write->flags & ~SDK_VIDEO_SESSION_WRITE_EOF) != 0U ||
	    (write->src_length == 0U &&
	     (write->flags & SDK_VIDEO_SESSION_WRITE_EOF) == 0U) ||
	    write->src_length > SDK_VIDEO_MAX_WRITE_BYTES ||
	    (write->src_length != 0U && !write->src))
		return SDK_STATUS_BAD_REQUEST;
	session = find_session(write->session);
	if (!session)
		return SDK_STATUS_BAD_HANDLE;
	status = ensure_decoder(session);
	if (status != SDK_STATUS_OK)
		return status;
	{
		int write_result = session->ops->write(
			session->decoder, write->src, write->src_length,
			(write->flags & SDK_VIDEO_SESSION_WRITE_EOF) != 0U,
			&accepted);

		if (write_result == SDK_VIDEO_BACKEND_WRITE_BACKPRESSURE)
			return SDK_STATUS_BUSY;
		if (write_result != SDK_VIDEO_BACKEND_WRITE_OK) {
		session->failed = 1U;
		return SDK_STATUS_IO_ERROR;
		}
	}
	if (session->bytes_accepted > 0xffffffffU - accepted)
		session->bytes_accepted = 0xffffffffU;
	else
		session->bytes_accepted += accepted;
	if ((write->flags & SDK_VIDEO_SESSION_WRITE_EOF) != 0U)
		session->input_eof = 1U;
	update_info(session);

	result_flags = session->frame_rate_milli != 0U
		? SDK_VIDEO_SESSION_RESULT_HEADER_READY
		: SDK_VIDEO_SESSION_RESULT_NEED_INPUT;
	state = session->frame_rate_milli != 0U
		? SDK_VIDEO_SESSION_STATE_READY
		: SDK_VIDEO_SESSION_STATE_NEED_INPUT;
	fill_result(session, state, 0U, result_flags, result);
	return SDK_STATUS_OK;
}

uint16_t sdk_video_stream_decode(const struct SDKVideoStreamDecode *decode,
	                             struct SDKVideoStreamResult *result)
{
	struct SDKVideoStreamSession *session;
	struct SDKVideoDecodedFrame frame;
	uint32_t flags;
	uint16_t status;
	int decoded;

	if (!decode || !result || decode->session == 0U)
		return SDK_STATUS_BAD_REQUEST;
	session = find_session(decode->session);
	if (!session)
		return SDK_STATUS_BAD_HANDLE;
	status = ensure_decoder(session);
	if (status != SDK_STATUS_OK)
		return status;
	decoded = session->ops->decode(session->decoder, &frame);
	if (decoded != SDK_VIDEO_BACKEND_PROGRESS ||
	    session->frame_rate_milli == 0U)
		update_info(session);
	flags = session->frame_rate_milli != 0U
		? SDK_VIDEO_SESSION_RESULT_HEADER_READY : 0U;

	if (decoded == SDK_VIDEO_BACKEND_NEED_INPUT) {
		fill_result(session, SDK_VIDEO_SESSION_STATE_NEED_INPUT, 0U,
		            flags | SDK_VIDEO_SESSION_RESULT_NEED_INPUT, result);
		return SDK_STATUS_OK;
	}
	if (decoded == SDK_VIDEO_BACKEND_DONE) {
		fill_result(session, SDK_VIDEO_SESSION_STATE_DONE, 0U,
		            flags | SDK_VIDEO_SESSION_RESULT_DONE, result);
		return SDK_STATUS_OK;
	}
	if (decoded == SDK_VIDEO_BACKEND_PROGRESS) {
		fill_result(session, SDK_VIDEO_SESSION_STATE_READY, 0U,
		            flags, result);
		return SDK_STATUS_OK;
	}
	if (decoded == SDK_VIDEO_BACKEND_BACKPRESSURE)
		return SDK_STATUS_BUSY;
	if (decoded == SDK_VIDEO_BACKEND_UNSUPPORTED)
		return SDK_STATUS_UNSUPPORTED;
	if (decoded != SDK_VIDEO_BACKEND_FRAME) {
		session->failed = 1U;
		return SDK_STATUS_IO_ERROR;
	}
	if (frame.width != session->width || frame.height != session->height) {
		session->failed = 1U;
		return SDK_STATUS_BAD_REQUEST;
	}
	/* The compose task is queued ahead of the client's next decode, so
	 * pl_mpeg's decoder-owned planes remain valid until it retires. */
	session->direct_frame = frame;
	session->direct_frame_valid = 1U;
	session->frame_number++;
	session->frame_time_millis = frame.time_millis;
	fill_result(session, SDK_VIDEO_SESSION_STATE_FRAME_READY, 0U,
	            flags | SDK_VIDEO_SESSION_RESULT_FRAME_READY, result);
	result->media_pts = frame.media_pts;
	result->raw_pts = frame.raw_pts;
	result->media_flags = frame.media_flags;
	return SDK_STATUS_OK;
}

uint16_t sdk_video_stream_close(uint32_t id,
	                            struct SDKVideoStreamResult *result)
{
	struct SDKVideoStreamSession *session;

	if (!result || id == 0U)
		return SDK_STATUS_BAD_REQUEST;
	session = find_session(id);
	if (!session)
		return SDK_STATUS_BAD_HANDLE;
	fill_result(session, SDK_VIDEO_SESSION_STATE_DONE, 0U,
	            SDK_VIDEO_SESSION_RESULT_DONE, result);
	if (session->decoder && !session->faulted)
		session->ops->destroy(session->decoder);
	memset(session, 0, sizeof(*session));
	return SDK_STATUS_OK;
}
