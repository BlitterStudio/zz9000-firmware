/*
 * Codec-neutral video stream lifecycle regression tests.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>

#include "sdk_mailbox.h"
#include "sdk_video_backend.h"
#include "sdk_video_stream.h"

static uint8_t decoder_storage;
static uint32_t create_calls;
static uint32_t destroy_calls;
static uint32_t write_calls;
static uint32_t ack_calls;

static void *mock_create(void)
{
	create_calls++;
	return &decoder_storage;
}

static void mock_destroy(void *decoder)
{
	if (decoder == &decoder_storage)
		destroy_calls++;
}

static int mock_write(void *decoder, const uint8_t *src, uint32_t length,
                      int eof, uint32_t *accepted)
{
	(void)src;
	(void)eof;
	if (decoder != &decoder_storage || !accepted)
		return SDK_VIDEO_BACKEND_WRITE_ERROR;
	write_calls++;
	*accepted = length;
	return SDK_VIDEO_BACKEND_WRITE_OK;
}

static int mock_get_info(void *decoder, struct SDKVideoDecoderInfo *info)
{
	if (decoder != &decoder_storage || !info)
		return 0;
	memset(info, 0, sizeof(*info));
	info->width = 320U;
	info->height = 240U;
	info->frame_rate_milli = 25000U;
	return 1;
}

static int mock_decode(void *decoder, struct SDKVideoDecodedFrame *frame)
{
	(void)frame;
	return decoder == &decoder_storage
		? SDK_VIDEO_BACKEND_NEED_INPUT : SDK_VIDEO_BACKEND_ERROR;
}

static int mock_configure_media(
	void *decoder, const struct SDKVideoMediaConfig *config)
{
	return decoder == &decoder_storage && config &&
		config->audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2;
}

static int mock_get_media_info(
	void *decoder, struct SDKVideoMediaInfo *info)
{
	if (decoder != &decoder_storage || !info)
		return 0;
	memset(info, 0, sizeof(*info));
	return 1;
}

static int mock_ack_media(void *decoder, uint64_t acknowledged)
{
	if (decoder != &decoder_storage)
		return 0;
	ack_calls++;
	return acknowledged == 0U;
}

static const struct SDKVideoDecoderOps mock_ops = {
	SDK_VIDEO_CODEC_MPEG1,
	SDK_VIDEO_CONTAINER_MPEG_PS,
	"mock",
	mock_create,
	mock_destroy,
	mock_write,
	mock_get_info,
	mock_decode,
	mock_configure_media,
	mock_get_media_info,
	mock_ack_media,
};

const struct SDKVideoDecoderOps *sdk_video_backend_find(
	uint32_t codec, uint32_t container)
{
	if (codec == mock_ops.codec && container == mock_ops.container)
		return &mock_ops;
	return 0;
}

void overlay_video_session_closed(uint32_t session)
{
	(void)session;
}

static int test_zero_ack_before_lazy_decoder(void)
{
	struct SDKVideoStreamBegin begin;
	struct SDKVideoStreamWrite write;
	struct SDKVideoStreamResult result;
	uint8_t pcm_ring[8192];
	uint8_t input = 0U;
	uint32_t session;

	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_VIDEO_CODEC_MPEG1;
	begin.container = SDK_VIDEO_CONTAINER_MPEG_PS;
	begin.width = 320U;
	begin.height = 240U;
	begin.output_format = SDK_VIDEO_OUTPUT_DIRECT_OVERLAY;
	begin.audio_codec = SDK_VIDEO_MEDIA_AUDIO_MP2;
	begin.pcm_ring = pcm_ring;
	begin.pcm_ring_capacity = sizeof(pcm_ring);
	begin.pcm_low_water_bytes = 1024U;
	begin.pcm_high_water_bytes = 6144U;

	sdk_video_stream_init();
	if (sdk_video_stream_begin_owned(
		    &begin, SDK_VIDEO_STREAM_OWNER_MEDIA, &result) !=
	        SDK_STATUS_OK)
		return 1;
	session = result.session;
	if (create_calls != 0U)
		return 2;
	if (!sdk_video_stream_ack_media(session, 0U) ||
	    create_calls != 0U || ack_calls != 0U)
		return 3;
	if (sdk_video_stream_ack_media(session, 4U) ||
	    create_calls != 0U || ack_calls != 0U)
		return 4;

	memset(&write, 0, sizeof(write));
	write.session = session;
	write.src = &input;
	write.src_length = 1U;
	if (sdk_video_stream_write(&write, &result) != SDK_STATUS_OK ||
	    create_calls != 1U || write_calls != 1U)
		return 5;
	if (!sdk_video_stream_ack_media(session, 0U) || ack_calls != 1U)
		return 6;
	if (sdk_video_stream_close(session, &result) != SDK_STATUS_OK ||
	    destroy_calls != 1U)
		return 7;
	return 0;
}

int main(void)
{
	return test_zero_ack_before_lazy_decoder();
}
