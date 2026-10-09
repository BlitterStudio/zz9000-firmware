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
#include "webm_parse.h"

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
		(config->audio_codec == SDK_VIDEO_MEDIA_AUDIO_NONE ||
		 config->audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2);
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

static uint32_t last_decode_flags;

static void mock_set_decode_flags(void *decoder, uint32_t flags)
{
	(void)decoder;
	last_decode_flags = flags;
}

static uint32_t geometry_calls;

/* Stands in for the WebM backend's admission rule. */
static int mock_webm_geometry_ok(uint32_t width, uint32_t height)
{
	geometry_calls++;
	return webm_size_allowed(width, height);
}

static int mock_accept_any_geometry(uint32_t width, uint32_t height)
{
	(void)width;
	(void)height;
	geometry_calls++;
	return 1;
}

/* Stands in for an MPEG-1 PS backend's audio rule (MP2 or none). */
static int mock_mp2_audio_ok(uint32_t audio_codec)
{
	return audio_codec == SDK_MEDIA_AUDIO_NONE ||
	       audio_codec == SDK_MEDIA_AUDIO_MP2;
}

static struct SDKVideoDecoderOps mock_ops = {
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
	mock_set_decode_flags,
	0,
	0
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

static int test_webm_stream_envelopes(void)
{
	struct SDKVideoStreamBegin begin;
	struct SDKVideoStreamResult result;

	sdk_video_stream_init();
	mock_ops.codec = SDK_VIDEO_CODEC_VP8;
	mock_ops.container = SDK_VIDEO_CONTAINER_WEBM;
	mock_ops.geometry_ok = mock_webm_geometry_ok;
	geometry_calls = 0U;
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_VIDEO_CODEC_VP8;
	begin.container = SDK_VIDEO_CONTAINER_WEBM;
	begin.output_format = SDK_VIDEO_OUTPUT_DIRECT_OVERLAY;
	/* Oversize width or height -> UNSUPPORTED, not BAD_REQUEST. */
	begin.width = 2560U;
	begin.height = 1440U;
	if (sdk_video_stream_begin_owned(
		    &begin, SDK_VIDEO_STREAM_OWNER_MEDIA, &result) !=
	    SDK_STATUS_UNSUPPORTED)
		return 10;

	begin.width = 1920U;
	begin.height = 1089U;
	if (sdk_video_stream_begin_owned(
		    &begin, SDK_VIDEO_STREAM_OWNER_MEDIA, &result) !=
	    SDK_STATUS_UNSUPPORTED)
		return 11;

	begin.width = 1921U;
	begin.height = 1080U;
	if (sdk_video_stream_begin_owned(
		    &begin, SDK_VIDEO_STREAM_OWNER_MEDIA, &result) !=
	    SDK_STATUS_UNSUPPORTED)
		return 12;
	/* The backend, not the session layer, owns the WebM caps. */
	if (geometry_calls != 3U)
		return 18;
	/* Zero dimension -> BAD_REQUEST. */
	begin.width = 0U;
	begin.height = 120U;
	if (sdk_video_stream_begin_owned(
		    &begin, SDK_VIDEO_STREAM_OWNER_MEDIA, &result) !=
	    SDK_STATUS_BAD_REQUEST)
		return 13;

	/* Opus/Vorbis on a container whose backend carries only MP2 ->
	 * UNSUPPORTED. The mock serves MPEG-1 PS here with that rule, so a
	 * backend exists and only its audio check can refuse. */
	mock_ops.codec = SDK_VIDEO_CODEC_MPEG1;
	mock_ops.container = SDK_VIDEO_CONTAINER_MPEG_PS;
	mock_ops.geometry_ok = 0;
	mock_ops.audio_ok = mock_mp2_audio_ok;
	begin.codec = SDK_VIDEO_CODEC_MPEG1;
	begin.container = SDK_VIDEO_CONTAINER_MPEG_PS;
	begin.width = 320U;
	begin.height = 240U;
	begin.audio_codec = SDK_MEDIA_AUDIO_MP2;
	if (sdk_video_stream_begin_owned(
		    &begin, SDK_VIDEO_STREAM_OWNER_MEDIA, &result) !=
	    SDK_STATUS_OK)
		return 19;
	sdk_video_stream_close(result.session, &result);
	begin.audio_codec = SDK_MEDIA_AUDIO_OPUS;
	if (sdk_video_stream_begin_owned(
		    &begin, SDK_VIDEO_STREAM_OWNER_MEDIA, &result) !=
	    SDK_STATUS_UNSUPPORTED)
		return 14;
	begin.audio_codec = SDK_MEDIA_AUDIO_VORBIS;
	if (sdk_video_stream_begin_owned(
		    &begin, SDK_VIDEO_STREAM_OWNER_MEDIA, &result) !=
	    SDK_STATUS_UNSUPPORTED)
		return 15;
	mock_ops.codec = SDK_VIDEO_CODEC_VP8;
	mock_ops.container = SDK_VIDEO_CONTAINER_WEBM;
	mock_ops.geometry_ok = mock_webm_geometry_ok;
	mock_ops.audio_ok = 0;

	/* Portrait and odd dimensions pass the size check. */
	begin.codec = SDK_VIDEO_CODEC_VP8;
	begin.container = SDK_VIDEO_CONTAINER_WEBM;
	begin.audio_codec = SDK_MEDIA_AUDIO_NONE;
	begin.width = 1080U;
	begin.height = 1920U;
	if (sdk_video_stream_begin_owned(
		    &begin, SDK_VIDEO_STREAM_OWNER_MEDIA, &result) !=
	    SDK_STATUS_OK)
		return 16;
	sdk_video_stream_close(result.session, &result);

	begin.width = 271U;
	begin.height = 481U;
	if (sdk_video_stream_begin_owned(
		    &begin, SDK_VIDEO_STREAM_OWNER_MEDIA, &result) !=
	    SDK_STATUS_OK)
		return 17;
	sdk_video_stream_close(result.session, &result);

	mock_ops.codec = SDK_VIDEO_CODEC_MPEG1;
	mock_ops.container = SDK_VIDEO_CONTAINER_MPEG_PS;
	mock_ops.geometry_ok = 0;
	return 0;
}

static int test_generic_caps_without_geometry_op(void)
{
	struct SDKVideoStreamBegin begin;
	struct SDKVideoStreamResult result;

	sdk_video_stream_init();
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_VIDEO_CODEC_MPEG1;
	begin.container = SDK_VIDEO_CONTAINER_MPEG_PS;
	begin.output_format = SDK_VIDEO_OUTPUT_DIRECT_OVERLAY;

	/* No geometry_ok: the generic cap applies and refuses BAD_REQUEST. */
	begin.width = 1921U;
	begin.height = 1080U;
	if (sdk_video_stream_begin(&begin, &result) != SDK_STATUS_BAD_REQUEST)
		return 30;
	begin.width = 1920U;
	begin.height = 1080U;
	if (sdk_video_stream_begin(&begin, &result) != SDK_STATUS_OK)
		return 31;
	sdk_video_stream_close(result.session, &result);

	/* A backend op replaces the generic cap entirely. */
	mock_ops.geometry_ok = mock_accept_any_geometry;
	geometry_calls = 0U;
	begin.width = 2560U;
	begin.height = 1440U;
	if (sdk_video_stream_begin(&begin, &result) != SDK_STATUS_OK ||
	    geometry_calls != 1U)
		return 32;
	sdk_video_stream_close(result.session, &result);
	mock_ops.geometry_ok = 0;
	return 0;
}

static int test_decode_flag_forwarding(void)
{
	struct SDKVideoStreamBegin begin;
	struct SDKVideoStreamDecode decode;
	struct SDKVideoStreamResult result;

	mock_ops.codec = SDK_VIDEO_CODEC_VP8;
	mock_ops.container = SDK_VIDEO_CONTAINER_WEBM;
	last_decode_flags = 0U;

	sdk_video_stream_init();
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_VIDEO_CODEC_VP8;
	begin.container = SDK_VIDEO_CONTAINER_WEBM;
	begin.width = 320U;
	begin.height = 240U;
	begin.output_format = SDK_VIDEO_OUTPUT_DIRECT_OVERLAY;
	if (sdk_video_stream_begin_owned(
		    &begin, SDK_VIDEO_STREAM_OWNER_MEDIA, &result) !=
	    SDK_STATUS_OK)
		return 20;

	memset(&decode, 0, sizeof(decode));
	decode.session = result.session;
	decode.flags = SDK_MEDIA_DECODE_SKIP_TO_KEYFRAME;
	if (sdk_video_stream_decode(&decode, &result) != SDK_STATUS_OK)
		return 21;
	if (last_decode_flags != SDK_MEDIA_DECODE_SKIP_TO_KEYFRAME)
		return 22;

	sdk_video_stream_close(result.session, &result);
	mock_ops.codec = SDK_VIDEO_CODEC_MPEG1;
	mock_ops.container = SDK_VIDEO_CONTAINER_MPEG_PS;
	return 0;
}

int main(void)
{
	int rc;

	rc = test_zero_ack_before_lazy_decoder();
	if (rc != 0) return rc;
	rc = test_webm_stream_envelopes();
	if (rc != 0) return rc;
	rc = test_generic_caps_without_geometry_op();
	if (rc != 0) return rc;
	rc = test_decode_flag_forwarding();
	if (rc != 0) return rc;
	return 0;
}
