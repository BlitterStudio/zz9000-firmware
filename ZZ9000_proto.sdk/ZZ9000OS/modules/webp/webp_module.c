#include <stdint.h>
#include <string.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include "sdk_mailbox.h"
#include "sdk_webp_module.h"
#include "sdk_webp_alloc.h"
#include "zzm_format.h"
#include "zzm_runtime.h"

#define ZZM_WEBP_MAX_INPUT_BYTES (8U * 1024U * 1024U)
#define ZZM_WEBP_MAX_DIMENSION 8192U
#define ZZM_WEBP_MAX_CANVAS_PIXELS (6U * 1024U * 1024U)
#define ZZM_WEBP_ARENA_REGION_BYTES (256U * 1024U)
#define ZZM_WEBP_ARENA_LIMIT (72U * 1024U * 1024U)
#define ZZM_WEBP_OWNER(session) (0x03000000U | ((session) & 0x00ffffffU))

static uint32_t bytes_per_pixel(uint32_t format)
{
	switch (format) {
	case SDK_SURFACE_FORMAT_RGB888:
		return 3U;
	case SDK_SURFACE_FORMAT_ARGB8888:
	case SDK_SURFACE_FORMAT_RGBA8888:
	case SDK_SURFACE_FORMAT_BGRA8888:
		return 4U;
	default:
		return 0U;
	}
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int anmf_has_image(const uint8_t *data, uint32_t length)
{
	uint32_t offset = 16U;
	int saw_alpha = 0;
	int saw_image = 0;

	if (length < offset)
		return 0;
	while (offset < length) {
		uint32_t chunk_length;
		uint32_t padded_length;

		if (length - offset < 8U)
			return 0;
		chunk_length = le32(data + offset + 4U);
		padded_length = chunk_length + (chunk_length & 1U);
		if (padded_length < chunk_length ||
		    padded_length > length - offset - 8U)
			return 0;
		if (memcmp(data + offset, "ALPH", 4U) == 0) {
			if (saw_alpha || saw_image || chunk_length == 0U)
				return 0;
			saw_alpha = 1;
		} else if (memcmp(data + offset, "VP8 ", 4U) == 0) {
			if (saw_image || chunk_length < 10U)
				return 0;
			saw_image = 1;
		} else if (memcmp(data + offset, "VP8L", 4U) == 0) {
			if (saw_alpha || saw_image || chunk_length < 5U)
				return 0;
			saw_image = 1;
		}
		offset += 8U + padded_length;
	}
	return offset == length && saw_image;
}

static void result_fill(const struct zzm_webp_session *session, uint32_t state,
			uint32_t consumed, struct SDKImageStreamResult *result)
{
	memset(result, 0, sizeof(*result));
	result->session = session->session;
	result->state = state;
	result->image_width = session->image_width;
	result->image_height = session->image_height;
	result->output_format = session->output_format;
	result->bytes_consumed = consumed;
}

static void result_direct(const struct zzm_webp_session *session,
			  uint32_t width, uint32_t height,
			  struct SDKImageStreamResult *result)
{
	uint32_t bpp = bytes_per_pixel(session->output_format);

	result->flush_address = session->dst_address +
		((uintptr_t)session->dst_y * session->dst_pitch) +
		((uintptr_t)session->dst_x * bpp);
	result->flush_length = (height - 1U) * session->dst_pitch + width * bpp;
	result->bytes_written = width * height * bpp;
}

static int output_valid(const struct zzm_webp_session *session,
			uint32_t width, uint32_t height, uint32_t bpp)
{
	uint32_t row_bytes;

	if (width == 0U || height == 0U || bpp == 0U ||
		width > 0xffffffffU / bpp)
		return 0;
	row_bytes = width * bpp;
	if (session->output_mode == SDK_IMAGE_OUTPUT_TILE_BUFFER)
		return session->tile_rows != 0U && session->tile_stride >= row_bytes &&
			session->tile_address != 0U &&
			session->tile_length >= session->tile_stride * session->tile_rows;
	if (session->dst_x > 0xffffffffU / bpp ||
		session->dst_x + width > session->dst_width ||
		session->dst_y + height > session->dst_height ||
		session->dst_pitch < row_bytes || session->dst_address == 0U)
		return 0;
	return session->dst_length >= session->dst_pitch * session->dst_height;
}

static void convert_row(uint8_t *dst, const uint8_t *src, uint32_t pixels,
			uint32_t format)
{
	uint32_t x;

	for (x = 0U; x < pixels; ++x) {
		const uint8_t *in = src + x * 4U;
		uint8_t *out = dst + x * bytes_per_pixel(format);
		switch (format) {
		case SDK_SURFACE_FORMAT_ARGB8888:
			out[0] = in[3]; out[1] = in[0]; out[2] = in[1]; out[3] = in[2];
			break;
		case SDK_SURFACE_FORMAT_RGBA8888:
			out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; out[3] = in[3];
			break;
		case SDK_SURFACE_FORMAT_RGB888:
			out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
			break;
		default:
			out[0] = in[2]; out[1] = in[1]; out[2] = in[0]; out[3] = in[3];
			break;
		}
	}
}

static void release_decode(struct zzm_webp_session *session)
{
	zzm_webp_alloc_release(&session->arena);
	session->input = 0U;
	session->input_length = 0U;
	session->input_capacity = 0U;
	session->canvas = 0U;
	session->anim_decoder = 0U;
	session->decoded = 0U;
}

static void close_session(struct zzm_webp_session *session)
{
	if (!session)
		return;
	release_decode(session);
	memset(session, 0, sizeof(*session));
}

static uint16_t fail(struct zzm_webp_session *session, uint16_t status,
			 uint32_t consumed, struct SDKImageStreamResult *result)
{
	release_decode(session);
	session->failed = 1U;
	result_fill(session, SDK_IMAGE_SESSION_STATE_ERROR, consumed, result);
	return status;
}

static int append_input(struct zzm_webp_session *session, const uint8_t *src,
			uint32_t length)
{
	uint32_t take;
	uint32_t total;
	uint8_t *input;

	if (length == 0U)
		return 1;
	if (session->input == 0U) {
		take = 12U - session->header_length;
		if (take > length)
			take = length;
		memcpy(session->header + session->header_length, src, take);
		session->header_length += (uint8_t)take;
		src += take;
		length -= take;
		if (session->header_length != 12U)
			return 1;
		if (memcmp(session->header, "RIFF", 4U) != 0 ||
			memcmp(session->header + 8U, "WEBP", 4U) != 0)
			return -2;
		total = le32(session->header + 4U);
		if (total > 0xffffffffU - 8U)
			return -2;
		total += 8U;
		if (total < 12U || total > ZZM_WEBP_MAX_INPUT_BYTES)
			return -1;
		zzm_webp_alloc_select(&session->arena);
		input = sdk_webp_malloc(total);
		if (!input)
			return 0;
		memcpy(input, session->header, 12U);
		session->input = (uintptr_t)input;
		session->input_capacity = total;
		session->input_length = 12U;
	}
	if (length > session->input_capacity - session->input_length)
		return -1;
	memcpy((uint8_t *)session->input + session->input_length, src, length);
	session->input_length += length;
	return 1;
}

static int is_animated(const struct zzm_webp_session *session)
{
	const uint8_t *data = (const uint8_t *)session->input;
	uint32_t offset;
	int animated = 0;

	if (!data || session->input_length < 12U ||
	    le32(data + 4U) != session->input_length - 8U)
		return -1;
	for (offset = 12U; offset < session->input_length;) {
		uint32_t length;
		uint32_t padded;

		if (session->input_length - offset < 8U)
			return -1;
		length = le32(data + offset + 4U);
		padded = length + (length & 1U);
		if (padded < length || padded > session->input_length - offset - 8U)
			return -1;
		if (memcmp(data + offset, "VP8X", 4U) == 0) {
			if (length != 10U)
				return -1;
			animated = (data[offset + 8U] & 2U) != 0U;
		} else if (animated && memcmp(data + offset, "ANMF", 4U) == 0 &&
		           !anmf_has_image(data + offset + 8U, length)) {
			return -1;
		}
		offset += 8U + padded;
	}
	return offset == session->input_length ? animated : -1;
}

static int emit_tile(struct zzm_webp_session *session, uint32_t consumed,
			 struct SDKImageStreamResult *result)
{
	uint32_t bpp = bytes_per_pixel(session->output_format);
	uint32_t rows;
	uint32_t row_bytes;
	uint32_t y;

	if (!session->canvas || bpp == 0U || session->tile_rows == 0U ||
		session->image_width > 0xffffffffU / bpp)
		return 0;
	row_bytes = session->image_width * bpp;
	if (session->tile_stride < row_bytes || session->tile_next_y >= session->image_height)
		return 0;
	rows = session->image_height - session->tile_next_y;
	if (rows > session->tile_rows)
		rows = session->tile_rows;
	for (y = 0U; y < rows; ++y)
		convert_row((uint8_t *)session->tile_address + y * session->tile_stride,
			(const uint8_t *)session->canvas +
				(session->tile_next_y + y) * session->image_width * 4U,
			session->image_width, session->output_format);
	result_fill(session, SDK_IMAGE_SESSION_STATE_TILE_READY, consumed, result);
	result->tile_y = session->tile_next_y;
	result->tile_width = session->image_width;
	result->tile_height = rows;
	result->bytes_written = row_bytes * rows;
	result->flush_address = session->tile_address;
	result->flush_length = row_bytes * rows;
	session->tile_next_y += rows;
	if (session->tile_next_y < session->image_height)
		result->flags |= SDK_IMAGE_SESSION_RESULT_PARTIAL;
	return 1;
}

static int write_canvas(struct zzm_webp_session *session,
			const uint8_t *rgba, struct SDKImageStreamResult *result)
{
	uint32_t bpp = bytes_per_pixel(session->output_format);
	uint32_t y;

	if (!output_valid(session, session->image_width, session->image_height, bpp) ||
		session->output_mode == SDK_IMAGE_OUTPUT_TILE_BUFFER ||
		(session->direct_arm_local &&
		 (session->dst_x != 0U || session->dst_y != 0U ||
		  session->dst_width != session->image_width ||
		  session->dst_height != session->image_height)))
		return 0;
	for (y = 0U; y < session->image_height; ++y)
		convert_row((uint8_t *)(session->dst_address +
			(session->dst_y + y) * session->dst_pitch + session->dst_x * bpp),
			rgba + y * session->image_width * 4U, session->image_width,
			session->output_format);
	result_direct(session, session->image_width, session->image_height, result);
	return 1;
}

static uint16_t module_begin(struct zzm_webp_session *session,
			     uint32_t session_id, const struct SDKImageStreamBegin *begin,
			     struct SDKImageStreamResult *result)
{
	if (!session || !begin || !result)
		return SDK_STATUS_BAD_REQUEST;
	memset(session, 0, sizeof(*session));
	session->session = session_id;
	session->output_mode = begin->output_mode;
	session->dst_x = begin->dst_x;
	session->dst_y = begin->dst_y;
	session->dst_width = begin->dst_width;
	session->dst_height = begin->dst_height;
	session->output_format = begin->output_format;
	session->tile_stride = begin->tile_stride;
	session->tile_rows = begin->tile_rows;
	session->dst_address = begin->dst_address;
	session->dst_pitch = begin->dst_pitch;
	session->dst_length = begin->dst_length;
	session->tile_address = begin->tile_address;
	session->tile_length = begin->tile_length;
	session->direct_arm_local = begin->direct_arm_local ? 1U : 0U;
	session->is_animation =
		(begin->flags & SDK_IMAGE_SESSION_BEGIN_ANIMATION) != 0U ? 1U : 0U;
	session->anim_state = SDK_IMAGE_SESSION_STATE_NEED_INPUT;
	session->arena.region_size = ZZM_WEBP_ARENA_REGION_BYTES;
	session->arena.limit = ZZM_WEBP_ARENA_LIMIT;
	session->arena.owner = ZZM_WEBP_OWNER(session_id);
	result_fill(session, SDK_IMAGE_SESSION_STATE_NEED_INPUT, 0U, result);
	return SDK_STATUS_OK;
}

static uint16_t decode(struct zzm_webp_session *session, uint32_t consumed,
			   struct SDKImageStreamResult *result)
{
	WebPBitstreamFeatures features;
	int animated;
	uint8_t *rgba;

	animated = is_animated(session);
	if (animated < 0)
		return fail(session, SDK_STATUS_IO_ERROR, consumed, result);
	zzm_webp_alloc_select(&session->arena);
	if (session->is_animation || animated) {
		WebPData data;
		WebPAnimDecoderOptions options;
		WebPAnimInfo info;
		WebPAnimDecoder *decoder;
		int timestamp;

		if (session->is_animation && !animated)
			return fail(session, SDK_STATUS_BAD_REQUEST, consumed, result);
		if (!WebPAnimDecoderOptionsInit(&options))
			return fail(session, SDK_STATUS_INTERNAL_ERROR, consumed, result);
		options.color_mode = MODE_RGBA;
		data.bytes = (const uint8_t *)session->input;
		data.size = session->input_length;
		decoder = WebPAnimDecoderNew(&data, &options);
		if (!decoder || !WebPAnimDecoderGetInfo(decoder, &info) ||
			(!session->is_animation && !WebPAnimDecoderGetNext(decoder, &rgba, &timestamp))) {
			if (decoder)
				WebPAnimDecoderDelete(decoder);
			return fail(session, zzm_webp_alloc_failed() ? SDK_STATUS_NO_MEMORY : SDK_STATUS_IO_ERROR,
					consumed, result);
		}
		session->image_width = info.canvas_width;
		session->image_height = info.canvas_height;
		if (session->image_width == 0U || session->image_height == 0U ||
			session->image_width > ZZM_WEBP_MAX_DIMENSION ||
			session->image_height > ZZM_WEBP_MAX_DIMENSION ||
			session->image_width > ZZM_WEBP_MAX_CANVAS_PIXELS / session->image_height) {
			WebPAnimDecoderDelete(decoder);
			return fail(session, SDK_STATUS_BAD_REQUEST, consumed, result);
		}
		session->anim_decoder = (uintptr_t)decoder;
		if (session->is_animation) {
			session->anim_loop_count = info.loop_count;
			session->anim_state = SDK_IMAGE_SESSION_STATE_ANIMATION_READY;
			session->decoded = 1U;
			result_fill(session, session->anim_state, consumed, result);
			return SDK_STATUS_OK;
		}
		session->canvas = (uintptr_t)rgba;
	} else {
		if (WebPGetFeatures((const uint8_t *)session->input, session->input_length,
			&features) != VP8_STATUS_OK || features.width <= 0 || features.height <= 0)
			return fail(session, SDK_STATUS_IO_ERROR, consumed, result);
		session->image_width = (uint32_t)features.width;
		session->image_height = (uint32_t)features.height;
		if (session->image_width > ZZM_WEBP_MAX_DIMENSION ||
			session->image_height > ZZM_WEBP_MAX_DIMENSION ||
			session->image_width > ZZM_WEBP_MAX_CANVAS_PIXELS / session->image_height)
			return fail(session, SDK_STATUS_BAD_REQUEST, consumed, result);
		rgba = WebPDecodeRGBA((const uint8_t *)session->input, session->input_length,
			&features.width, &features.height);
		if (!rgba)
			return fail(session, zzm_webp_alloc_failed() ? SDK_STATUS_NO_MEMORY : SDK_STATUS_IO_ERROR,
					consumed, result);
		session->canvas = (uintptr_t)rgba;
	}
	session->decoded = 1U;
	if (session->output_mode == SDK_IMAGE_OUTPUT_TILE_BUFFER) {
		if (!emit_tile(session, consumed, result))
			return fail(session, SDK_STATUS_BAD_REQUEST, consumed, result);
		return SDK_STATUS_OK;
	}
	result_fill(session, SDK_IMAGE_SESSION_STATE_COMPLETE, consumed, result);
	if (!write_canvas(session, (const uint8_t *)session->canvas, result))
		return fail(session, SDK_STATUS_BAD_REQUEST, consumed, result);
	release_decode(session);
	session->output_complete = 1U;
	return SDK_STATUS_OK;
}

static uint16_t module_feed(struct zzm_webp_session *session, const uint8_t *src,
			    uint32_t length, uint32_t eof, struct SDKImageStreamResult *result)
{
	int appended;

	if (!session || !result || (length != 0U && !src))
		return SDK_STATUS_BAD_REQUEST;
	if (session->failed)
		return SDK_STATUS_IO_ERROR;
	if (session->output_complete) {
		if (length != 0U)
			return fail(session, SDK_STATUS_BAD_REQUEST, 0U, result);
		result_fill(session, SDK_IMAGE_SESSION_STATE_COMPLETE, 0U, result);
		return SDK_STATUS_OK;
	}
	if (session->decoded && session->is_animation) {
		if (length != 0U)
			return fail(session, SDK_STATUS_BAD_REQUEST, 0U, result);
		result_fill(session, session->anim_state, 0U, result);
		return SDK_STATUS_OK;
	}
	if (session->decoded && !session->is_animation) {
		if (length != 0U)
			return fail(session, SDK_STATUS_BAD_REQUEST, 0U, result);
		if (session->tile_next_y < session->image_height)
			return emit_tile(session, 0U, result) ? SDK_STATUS_OK :
				fail(session, SDK_STATUS_BAD_REQUEST, 0U, result);
		result_fill(session, SDK_IMAGE_SESSION_STATE_COMPLETE, 0U, result);
		release_decode(session);
		session->output_complete = 1U;
		return SDK_STATUS_OK;
	}
	appended = append_input(session, src, length);
	if (appended == 0)
		return fail(session, SDK_STATUS_NO_MEMORY, length, result);
	if (appended == -1)
		return fail(session, SDK_STATUS_BAD_REQUEST, 0U, result);
	if (appended < 0)
		return fail(session, SDK_STATUS_IO_ERROR, 0U, result);
	if (!eof) {
		result_fill(session, SDK_IMAGE_SESSION_STATE_NEED_INPUT, length, result);
		return SDK_STATUS_OK;
	}
	return decode(session, length, result);
}

static void rgba_to_yuv422(const uint8_t *rgba, uint32_t width, uint32_t height,
			   uint8_t *dst, uint32_t pitch)
{
	uint32_t y;
	for (y = 0U; y < height; ++y) {
		const uint8_t *src = rgba + y * width * 4U;
		uint8_t *out = dst + y * pitch;
		uint32_t x;
		for (x = 0U; x < width; x += 2U) {
			const uint8_t *a = src + x * 4U;
			const uint8_t *b = src + (x + (x + 1U < width ? 1U : 0U)) * 4U;
			int32_t r = (a[0] + b[0]) >> 1, g = (a[1] + b[1]) >> 1, bl = (a[2] + b[2]) >> 1;
			int32_t y0 = ((66 * a[0] + 129 * a[1] + 25 * a[2] + 128) >> 8) + 16;
			int32_t y1 = ((66 * b[0] + 129 * b[1] + 25 * b[2] + 128) >> 8) + 16;
			int32_t u = ((-38 * r - 74 * g + 112 * bl + 128) >> 8) + 128;
			int32_t v = ((112 * r - 94 * g - 18 * bl + 128) >> 8) + 128;
			out[(x >> 1) * 4U] = (uint8_t)y0;
			out[(x >> 1) * 4U + 1U] = (uint8_t)u;
			out[(x >> 1) * 4U + 2U] = (uint8_t)y1;
			out[(x >> 1) * 4U + 3U] = (uint8_t)v;
		}
	}
}

static uint16_t frame_next(struct zzm_webp_session *session, uint32_t flags,
			   struct SDKImageAnimationFrameResult *result)
{
	WebPAnimDecoder *decoder;
	uint8_t *rgba;
	int timestamp;

	(void)flags;
	if (!session || !result)
		return SDK_STATUS_BAD_REQUEST;
	memset(result, 0, sizeof(*result));
	if (session->failed)
		return SDK_STATUS_IO_ERROR;
	if (!session->is_animation || !session->decoded ||
	    session->anim_token_outstanding)
		return SDK_STATUS_BAD_REQUEST;
	decoder = (WebPAnimDecoder *)session->anim_decoder;
	if (!decoder)
		return SDK_STATUS_BAD_REQUEST;
	zzm_webp_alloc_select(&session->arena);
	if (!WebPAnimDecoderHasMoreFrames(decoder)) {
		if (session->anim_loop_count != 0U && session->anim_loop_index + 1U >= session->anim_loop_count) {
			session->anim_ended = 1U;
			session->anim_state = SDK_IMAGE_SESSION_STATE_ANIMATION_ENDED;
			result->session = session->session;
			result->state = session->anim_state;
			result->canvas_width = session->image_width;
			result->canvas_height = session->image_height;
			result->loop_index = session->anim_loop_index;
			result->loop_count = session->anim_loop_count;
			result->output_format = SDK_SURFACE_FORMAT_YUV422CGX;
			result->flags = SDK_IMAGE_ANIMATION_FRAME_FLAG_ENDED;
			return SDK_STATUS_OK;
		}
		WebPAnimDecoderReset(decoder);
		session->anim_loop_index++;
		session->anim_frame_index = 0U;
		session->anim_prev_timestamp = 0;
	}
	if (!WebPAnimDecoderGetNext(decoder, &rgba, &timestamp)) {
		session->failed = 1U;
		return zzm_webp_alloc_failed() ? SDK_STATUS_NO_MEMORY : SDK_STATUS_IO_ERROR;
	}
	rgba_to_yuv422(rgba, session->image_width, session->image_height,
		(uint8_t *)session->dst_address, session->dst_pitch);
	session->anim_token = ++session->anim_token_seq;
	if (session->anim_token == 0U)
		session->anim_token = ++session->anim_token_seq;
	session->anim_token_outstanding = 1U;
	result->session = session->session;
	result->state = SDK_IMAGE_SESSION_STATE_ANIMATION_READY;
	result->canvas_width = session->image_width;
	result->canvas_height = session->image_height;
	result->frame_index = session->anim_frame_index++;
	result->frame_duration_ms = timestamp >= session->anim_prev_timestamp ?
		(uint32_t)(timestamp - session->anim_prev_timestamp) : 0U;
	session->anim_prev_timestamp = timestamp;
	result->loop_index = session->anim_loop_index;
	result->loop_count = session->anim_loop_count;
	result->frame_token = session->anim_token;
	result->output_format = SDK_SURFACE_FORMAT_YUV422CGX;
	result->flags = SDK_IMAGE_ANIMATION_FRAME_FLAG_FRAME_READY;
	if (!WebPAnimDecoderHasMoreFrames(decoder) &&
	    session->anim_loop_count != 0U &&
	    session->anim_loop_index + 1U >= session->anim_loop_count)
		result->flags |= SDK_IMAGE_ANIMATION_FRAME_FLAG_LAST_FRAME;
	return SDK_STATUS_OK;
}

static uint16_t frame_present(struct zzm_webp_session *session, uint32_t token,
			      uint32_t flags, struct SDKImageAnimationFrameResult *result)
{
	(void)flags;
	if (!session || !result)
		return SDK_STATUS_BAD_REQUEST;
	if (session->failed)
		return SDK_STATUS_IO_ERROR;
	if (token == 0U || !session->is_animation ||
		!session->anim_token_outstanding || token != session->anim_token ||
		session->anim_presented)
		return SDK_STATUS_BAD_REQUEST;
	memset(result, 0, sizeof(*result));
	session->anim_presented = 1U;
	result->session = session->session;
	result->state = session->anim_state;
	result->canvas_width = session->image_width;
	result->canvas_height = session->image_height;
	result->frame_index = session->anim_frame_index - 1U;
	result->loop_index = session->anim_loop_index;
	result->loop_count = session->anim_loop_count;
	result->frame_token = token;
	result->output_format = SDK_SURFACE_FORMAT_YUV422CGX;
	result->flags = SDK_IMAGE_ANIMATION_FRAME_FLAG_PRESENTED;
	return SDK_STATUS_OK;
}

static uint16_t frame_retire(struct zzm_webp_session *session, uint32_t token,
			     uint32_t flags, struct SDKImageAnimationFrameResult *result)
{
	(void)flags;
	if (!session || !result)
		return SDK_STATUS_BAD_REQUEST;
	if (session->failed)
		return SDK_STATUS_IO_ERROR;
	if (token == 0U || !session->is_animation ||
		!session->anim_token_outstanding || token != session->anim_token ||
		!session->anim_presented)
		return SDK_STATUS_BAD_REQUEST;
	memset(result, 0, sizeof(*result));
	session->anim_token_outstanding = 0U;
	session->anim_presented = 0U;
	session->anim_token = 0U;
	if (!WebPAnimDecoderHasMoreFrames(
		    (WebPAnimDecoder *)session->anim_decoder) &&
	    session->anim_loop_count != 0U &&
	    session->anim_loop_index + 1U >= session->anim_loop_count) {
		session->anim_ended = 1U;
		session->anim_state = SDK_IMAGE_SESSION_STATE_ANIMATION_ENDED;
	}
	result->session = session->session;
	result->state = session->anim_state;
	result->canvas_width = session->image_width;
	result->canvas_height = session->image_height;
	result->frame_index = session->anim_frame_index - 1U;
	result->loop_index = session->anim_loop_index;
	result->loop_count = session->anim_loop_count;
	result->output_format = SDK_SURFACE_FORMAT_YUV422CGX;
	result->flags = session->anim_ended ?
		SDK_IMAGE_ANIMATION_FRAME_FLAG_ENDED : 0U;
	return SDK_STATUS_OK;
}

static uint16_t restart(struct zzm_webp_session *session, uint32_t flags,
			struct SDKImageAnimationFrameResult *result)
{
	WebPData data;
	WebPAnimDecoderOptions options;
	WebPAnimDecoder *decoder;
	WebPAnimInfo info;

	(void)flags;
	if (!session || !result)
		return SDK_STATUS_BAD_REQUEST;
	if (session->failed)
		return SDK_STATUS_IO_ERROR;
	if (!session->is_animation || session->anim_token_outstanding ||
	    !session->decoded || !session->input)
		return SDK_STATUS_BAD_REQUEST;
	zzm_webp_alloc_select(&session->arena);
	if (session->anim_decoder)
		WebPAnimDecoderDelete((WebPAnimDecoder *)session->anim_decoder);
	if (!WebPAnimDecoderOptionsInit(&options))
		return SDK_STATUS_INTERNAL_ERROR;
	options.color_mode = MODE_RGBA;
	data.bytes = (const uint8_t *)session->input;
	data.size = session->input_length;
	decoder = WebPAnimDecoderNew(&data, &options);
	if (!decoder || !WebPAnimDecoderGetInfo(decoder, &info)) {
		if (decoder)
			WebPAnimDecoderDelete(decoder);
		session->failed = 1U;
		return zzm_webp_alloc_failed() ? SDK_STATUS_NO_MEMORY : SDK_STATUS_IO_ERROR;
	}
	session->anim_decoder = (uintptr_t)decoder;
	session->anim_loop_count = info.loop_count;
	session->anim_loop_index = 0U;
	session->anim_frame_index = 0U;
	session->anim_token = 0U;
	session->anim_token_outstanding = 0U;
	session->anim_presented = 0U;
	session->anim_ended = 0U;
	session->anim_prev_timestamp = 0;
	memset(result, 0, sizeof(*result));
	result->session = session->session;
	result->state = SDK_IMAGE_SESSION_STATE_ANIMATION_READY;
	result->canvas_width = session->image_width;
	result->canvas_height = session->image_height;
	result->loop_count = session->anim_loop_count;
	result->output_format = SDK_SURFACE_FORMAT_YUV422CGX;
	return SDK_STATUS_OK;
}

static int complete_arm_local(const struct zzm_webp_session *session,
			      uintptr_t *address, uint32_t *length)
{
	if (!session || (!session->is_animation &&
	                 (!session->direct_arm_local || !session->output_complete)))
		return 0;
	if (address)
		*address = session->dst_address;
	if (length)
		*length = session->dst_length;
	return session->dst_address != 0U && session->dst_length != 0U;
}

static int presented_canvas(const struct zzm_webp_session *session,
			uintptr_t *address, uint32_t *pitch, uint32_t *width, uint32_t *height)
{
	if (!session || !session->is_animation || session->failed ||
		!session->anim_token_outstanding || !session->anim_presented || !session->dst_address)
		return 0;
	*address = session->dst_address;
	*pitch = session->dst_pitch;
	*width = session->image_width;
	*height = session->image_height;
	return 1;
}

static void poison(struct zzm_webp_session *session)
{
	if (!session)
		return;
	memset(&session->arena, 0, sizeof(session->arena));
	session->input = 0U;
	session->canvas = 0U;
	session->anim_decoder = 0U;
	session->failed = 1U;
}

static const struct zzm_webp_ops module_ops = {
	ZZM_WEBP_OPS_VERSION, sizeof(module_ops), module_begin, module_feed,
	close_session, complete_arm_local, presented_canvas, poison,
	frame_next, frame_present, frame_retire, restart,
};

const uint32_t zzm_module_descriptor = ZZM_MODULE_WEBP;

int zzm_module_init(const zzm_services_t *services)
{
	if (!zzm_rt_bind(services, ZZM_MODULE_WEBP, ZZM_MEMORY_CLASS_IMAGE,
	                 32U << 20))
		return -1;
	zzm_webp_alloc_init(services);
	return services->register_ops(ZZM_MODULE_WEBP, &module_ops,
	                              sizeof(module_ops)) ? 0 : -1;
}
