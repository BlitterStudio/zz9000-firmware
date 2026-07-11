/*
 * pl_mpeg backend: MPEG-1 video carried in an MPEG Program Stream.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_video_backend.h"

#include <stdlib.h>
#include <string.h>

#ifndef SDK_VIDEO_HOST_TEST
#include "sdk_compression.h"
#include "sdk_decode_reclaim.h"
#include "sdk_smp_lock.h"
#endif

#define SDK_VIDEO_CODEC_MPEG1 1U
#define SDK_VIDEO_CONTAINER_MPEG_PS 1U
#define PLM_BUFFER_DEFAULT_SIZE (256U * 1024U)
#define PLM_NO_STDIO 1

#ifdef SDK_VIDEO_HOST_TEST
#define PLM_MALLOC(size) malloc(size)
#define PLM_REALLOC(ptr, size) realloc((ptr), (size))
#define PLM_FREE(ptr) free(ptr)
#else
static void *sdk_video_heap_realloc(void *ptr, size_t size)
{
	void *resized;

	if (!ptr)
		return sdk_decode_heap_alloc(size);
	if (smp_cpu_id() != 1)
		return realloc(ptr, size);

	sdk_decode_untrack(ptr);
	sdk_decode_flush_table();
	resized = realloc(ptr, size);
	if (!resized) {
		/* realloc failure preserves the old allocation. */
		sdk_decode_track(ptr);
		sdk_decode_flush_table();
		return 0;
	}
	sdk_decode_track(resized);
	sdk_decode_flush_table();
	return resized;
}

#define PLM_MALLOC(size) sdk_decode_heap_alloc(size)
#define PLM_REALLOC(ptr, size) sdk_video_heap_realloc((ptr), (size))
#define PLM_FREE(ptr) sdk_decode_heap_free(ptr)
#endif

#define PL_MPEG_IMPLEMENTATION
#include "third_party/pl_mpeg/pl_mpeg.h"

struct sdk_video_plmpeg {
	plm_buffer_t *input;
	plm_demux_t *demux;
	plm_buffer_t *video_input;
	plm_video_t *video;
	int input_eof;
	int video_eof;
	int failed;
};

static void plmpeg_load_video(plm_buffer_t *buffer, void *user)
{
	struct sdk_video_plmpeg *decoder = (struct sdk_video_plmpeg *)user;
	plm_packet_t *packet;

	if (!decoder || decoder->video_eof)
		return;
	while ((packet = plm_demux_decode(decoder->demux)) != 0) {
		if (packet->type == PLM_DEMUX_PACKET_VIDEO_1) {
			if (plm_buffer_write(buffer, packet->data, packet->length) !=
			    packet->length)
				decoder->failed = 1;
			return;
		}
	}
	if (decoder->input_eof && plm_demux_has_ended(decoder->demux)) {
		plm_buffer_signal_end(buffer);
		decoder->video_eof = 1;
	}
}

static void *plmpeg_create(void)
{
	struct sdk_video_plmpeg *decoder =
		(struct sdk_video_plmpeg *)PLM_MALLOC(sizeof(*decoder));

	if (!decoder)
		return 0;
	memset(decoder, 0, sizeof(*decoder));
	decoder->input = plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE);
	if (!decoder->input)
		goto fail;
	decoder->demux = plm_demux_create(decoder->input, 1);
	if (!decoder->demux)
		goto fail;
	decoder->video_input =
		plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE);
	if (!decoder->video_input)
		goto fail;
	plm_buffer_set_load_callback(decoder->video_input, plmpeg_load_video,
	                             decoder);
	decoder->video = plm_video_create_with_buffer(decoder->video_input, 1);
	if (!decoder->video)
		goto fail;
	/* video owns video_input from here. */
	decoder->video_input = 0;
	return decoder;

fail:
	if (decoder->video)
		plm_video_destroy(decoder->video);
	else if (decoder->video_input)
		plm_buffer_destroy(decoder->video_input);
	if (decoder->demux)
		plm_demux_destroy(decoder->demux);
	else if (decoder->input)
		plm_buffer_destroy(decoder->input);
	PLM_FREE(decoder);
	return 0;
}

static void plmpeg_destroy(void *opaque)
{
	struct sdk_video_plmpeg *decoder = (struct sdk_video_plmpeg *)opaque;

	if (!decoder)
		return;
	plm_video_destroy(decoder->video);
	plm_demux_destroy(decoder->demux);
	PLM_FREE(decoder);
}

static int plmpeg_write(void *opaque, const uint8_t *src, uint32_t length,
	                    int eof, uint32_t *accepted)
{
	struct sdk_video_plmpeg *decoder = (struct sdk_video_plmpeg *)opaque;
	size_t written = 0U;

	if (accepted)
		*accepted = 0U;
	if (!decoder || decoder->failed || (length != 0U && !src) ||
	    decoder->input_eof)
		return 0;
	if (length != 0U)
		written = plm_buffer_write(decoder->input,
		                           (uint8_t *)(uintptr_t)src, length);
	if (written != length) {
		decoder->failed = 1;
		return 0;
	}
	if (eof) {
		plm_buffer_signal_end(decoder->input);
		decoder->input_eof = 1;
	}
	if (accepted)
		*accepted = (uint32_t)written;
	return 1;
}

static int plmpeg_get_info(void *opaque, struct SDKVideoDecoderInfo *info)
{
	struct sdk_video_plmpeg *decoder = (struct sdk_video_plmpeg *)opaque;
	double rate;

	if (!decoder || !info || !plm_video_has_header(decoder->video))
		return 0;
	memset(info, 0, sizeof(*info));
	info->width = (uint32_t)plm_video_get_width(decoder->video);
	info->height = (uint32_t)plm_video_get_height(decoder->video);
	rate = plm_video_get_framerate(decoder->video);
	if (rate > 0.0 && rate < 4294967.0)
		info->frame_rate_milli = (uint32_t)(rate * 1000.0 + 0.5);
	return info->width != 0U && info->height != 0U;
}

static int plmpeg_decode(void *opaque, struct SDKVideoDecodedFrame *out)
{
	struct sdk_video_plmpeg *decoder = (struct sdk_video_plmpeg *)opaque;
	plm_frame_t *frame;

	if (!decoder || !out || decoder->failed)
		return SDK_VIDEO_BACKEND_ERROR;
	frame = plm_video_decode(decoder->video);
	if (!frame) {
		if (decoder->video_eof && plm_video_has_ended(decoder->video))
			return SDK_VIDEO_BACKEND_DONE;
		return SDK_VIDEO_BACKEND_NEED_INPUT;
	}

	memset(out, 0, sizeof(*out));
	out->width = frame->width;
	out->height = frame->height;
	out->y_pitch = frame->y.width;
	out->chroma_pitch = frame->cb.width;
	out->y = frame->y.data;
	out->cb = frame->cb.data;
	out->cr = frame->cr.data;
	if (frame->time > 0.0 && frame->time < 4294967.0)
		out->time_millis = (uint32_t)(frame->time * 1000.0 + 0.5);
	return SDK_VIDEO_BACKEND_FRAME;
}

static const struct SDKVideoDecoderOps plmpeg_ops = {
	SDK_VIDEO_CODEC_MPEG1,
	SDK_VIDEO_CONTAINER_MPEG_PS,
	"pl_mpeg MPEG-1/PS",
	plmpeg_create,
	plmpeg_destroy,
	plmpeg_write,
	plmpeg_get_info,
	plmpeg_decode,
};

const struct SDKVideoDecoderOps *sdk_video_backend_find(uint32_t codec,
	                                                     uint32_t container)
{
	/* Add future codec/container backends to this registry. */
	if (codec == plmpeg_ops.codec && container == plmpeg_ops.container)
		return &plmpeg_ops;
	return 0;
}
