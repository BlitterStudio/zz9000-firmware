/*
 * Single-demux MPEG-1 video + Layer-II media backend.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_video_backend.h"
#include "sdk_video_yuy2.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpeg1_mp2_ps_fixture.inc"
#include "mpeg1_mp2_mono32_ps_fixture.inc"
#include "mpeg1_mp2_stereo48_ps_fixture.inc"
#include "mpeg1_ps_fixture.inc"

#define PCM_RING_CAPACITY (128U * 1024U)
#define TEST_MEDIA_UNCONFIGURED 0xffffffffU

struct media_summary {
	uint32_t frames;
	uint32_t sample_rate;
	uint32_t media_flags;
	uint64_t pcm_produced;
	uint64_t video_hash;
	uint64_t pcm_hash;
	uint64_t first_video_pts;
	uint64_t first_audio_pts;
	uint64_t last_video_pts;
	uint64_t current_audio_pts;
	uint64_t audio_tail_pts;
	uint64_t pcm_block_hash[16];
};

static uint8_t pcm_ring[PCM_RING_CAPACITY];
static uint8_t expected_pcm[PCM_RING_CAPACITY];
static uint8_t small_pcm_ring[10000];
static uint8_t compressed_fill[256U * 1024U];
static struct SDKVideoMediaInfo last_terminal_media;

static uint64_t fnv1a64(uint64_t hash, const uint8_t *bytes, uint32_t length)
{
	uint32_t i;

	for (i = 0U; i < length; i++) {
		hash ^= bytes[i];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static int accumulate_frame(const struct SDKVideoDecodedFrame *frame,
	                        struct media_summary *summary)
{
	uint32_t row_bytes = sdk_video_yuy2_row_bytes(frame->width);
	uint32_t bytes_written;
	uint32_t row;
	uint8_t *yuy2 =
		(uint8_t *)malloc(row_bytes * frame->height);

	if (!yuy2 || !sdk_video_yuv420_to_yuy2(
	        yuy2, row_bytes, frame->width, frame->height,
	        frame->y, frame->y_pitch, frame->cb, frame->cr,
	        frame->chroma_pitch, &bytes_written)) {
		free(yuy2);
		return 1;
	}
	for (row = 0U; row < frame->height; row++)
		summary->video_hash = fnv1a64(
			summary->video_hash, yuy2 + row * row_bytes, row_bytes);
	free(yuy2);
	if (summary->first_video_pts == SDK_VIDEO_MEDIA_NO_PTS)
		summary->first_video_pts = frame->media_pts;
	else if (frame->media_pts !=
	         summary->first_video_pts +
	             (uint64_t)summary->frames * 3600U)
		return 2;
	summary->last_video_pts = frame->media_pts;
	summary->media_flags |= frame->media_flags;
	summary->frames++;
	return 0;
}

static int decode_fixture_bytes(const uint8_t *bytes, uint32_t length,
	                            uint32_t chunk_size, uint32_t audio_codec,
	                            struct media_summary *summary)
{
	const struct SDKVideoDecoderOps *ops;
	struct SDKVideoMediaConfig config;
	struct SDKVideoMediaInfo media;
	struct SDKVideoDecodedFrame frame;
	uint32_t offset = 0U;
	uint32_t guard = 0U;
	void *decoder;
	int decode_result = SDK_VIDEO_BACKEND_NEED_INPUT;

	memset(summary, 0, sizeof(*summary));
	summary->video_hash = UINT64_C(14695981039346656037);
	summary->pcm_hash = UINT64_C(14695981039346656037);
	summary->first_video_pts = SDK_VIDEO_MEDIA_NO_PTS;
	summary->first_audio_pts = SDK_VIDEO_MEDIA_NO_PTS;
	memset(pcm_ring, 0xa5, sizeof(pcm_ring));

	ops = sdk_video_backend_find(1U, 1U);
	if (!ops || !ops->configure_media || !ops->get_media_info ||
	    !ops->ack_media)
		return 1;
	decoder = ops->create();
	if (!decoder)
		return 2;
	memset(&config, 0, sizeof(config));
	config.audio_codec = audio_codec;
	if (audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2) {
		config.pcm_ring = pcm_ring;
		config.pcm_ring_capacity = sizeof(pcm_ring);
		config.pcm_low_water_bytes = 4608U;
		config.pcm_high_water_bytes = sizeof(pcm_ring);
	}
	if (!ops->configure_media(decoder, &config)) {
		ops->destroy(decoder);
		return 3;
	}

	while (offset < length) {
		uint32_t chunk = chunk_size;
		uint32_t accepted = 0U;

		if (chunk > length - offset)
			chunk = length - offset;
		if (!ops->write(decoder, bytes + offset, chunk, 0, &accepted) ||
		    accepted != chunk) {
			ops->destroy(decoder);
			return 4;
		}
		offset += accepted;
		for (;;) {
			decode_result = ops->decode(decoder, &frame);
			if (decode_result == SDK_VIDEO_BACKEND_FRAME) {
				int frame_result =
					accumulate_frame(&frame, summary);

				if (frame_result != 0) {
					ops->destroy(decoder);
					return frame_result == 1 ? 5 : 13;
				}
				continue;
			}
			if (decode_result == SDK_VIDEO_BACKEND_PROGRESS)
				continue;
			if (decode_result == SDK_VIDEO_BACKEND_ERROR) {
				ops->destroy(decoder);
				return 6;
			}
			break;
		}
	}
	{
		uint32_t accepted = 1U;

		if (!ops->write(decoder, 0, 0U, 1, &accepted) ||
		    accepted != 0U) {
			ops->destroy(decoder);
			return 7;
		}
	}
	while (decode_result != SDK_VIDEO_BACKEND_DONE && guard++ < 1000U) {
		decode_result = ops->decode(decoder, &frame);
		if (decode_result == SDK_VIDEO_BACKEND_FRAME) {
			int frame_result =
				accumulate_frame(&frame, summary);

			if (frame_result != 0) {
				ops->destroy(decoder);
				return frame_result == 1 ? 8 : 14;
			}
		} else if (decode_result == SDK_VIDEO_BACKEND_NEED_INPUT ||
		           decode_result == SDK_VIDEO_BACKEND_ERROR) {
			ops->destroy(decoder);
			return 9;
		}
	}
	if (decode_result != SDK_VIDEO_BACKEND_DONE ||
	    !ops->get_media_info(decoder, &media)) {
		ops->destroy(decoder);
		return 10;
	}
	summary->sample_rate = media.sample_rate;
	summary->media_flags |= media.flags;
	summary->pcm_produced = media.pcm_produced;
	summary->first_audio_pts = media.first_audio_pts;
	summary->current_audio_pts = media.current_audio_pts;
	if (media.pcm_acknowledged != 0U ||
	    media.pcm_produced > sizeof(pcm_ring) ||
	    (audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2 &&
	     (media.channels != 2U ||
	      media.sample_format != SDK_VIDEO_MEDIA_SAMPLE_S16BE)) ||
	    (audio_codec == SDK_VIDEO_MEDIA_AUDIO_NONE &&
	     (media.channels != 0U || media.sample_format != 0U ||
	      media.pcm_produced != 0U))) {
		ops->destroy(decoder);
		return 11;
	}
	if (audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2) {
		uint32_t block;

		summary->pcm_hash = fnv1a64(
			summary->pcm_hash, pcm_ring,
			(uint32_t)media.pcm_produced);
		for (block = 0U; block < media.pcm_produced / 4608U; block++)
			summary->pcm_block_hash[block] = fnv1a64(
				UINT64_C(14695981039346656037),
				pcm_ring + block * 4608U, 4608U);
		if (!ops->ack_media(decoder, media.pcm_produced) ||
		    !ops->get_media_info(decoder, &media) ||
		    media.pcm_acknowledged != media.pcm_produced) {
			ops->destroy(decoder);
			return 12;
		}
		summary->media_flags |= media.flags;
	}
	summary->audio_tail_pts = media.audio_pts;
	ops->destroy(decoder);
	return 0;
}

static int decode_fixture(uint32_t chunk_size, struct media_summary *summary)
{
	return decode_fixture_bytes(
		zz9k_mpeg1_mp2_ps_fixture,
		zz9k_mpeg1_mp2_ps_fixture_len, chunk_size,
		SDK_VIDEO_MEDIA_AUDIO_MP2, summary);
}

static uint64_t ring_hash(const uint8_t *ring, uint32_t capacity,
	                      uint64_t cursor, uint32_t length)
{
	uint64_t hash = UINT64_C(14695981039346656037);
	uint32_t i;

	for (i = 0U; i < length; i++)
		hash = fnv1a64(
			hash, &ring[(uint32_t)((cursor + i) % capacity)], 1U);
	return hash;
}

static int test_ring_backpressure_and_wrap(void)
{
	const struct SDKVideoDecoderOps *ops =
		sdk_video_backend_find(1U, 1U);
	struct SDKVideoMediaConfig config;
	struct SDKVideoMediaInfo media;
	struct SDKVideoDecodedFrame frame;
	uint64_t protected_hash;
	uint64_t first_video_pcm = UINT64_MAX;
	uint32_t accepted;
	uint32_t guard = 0U;
	uint32_t video_frames = 0U;
	void *decoder;
	int result;

	if (!ops)
		return 1;
	decoder = ops->create();
	if (!decoder)
		return 2;
	memset(&config, 0, sizeof(config));
	config.audio_codec = SDK_VIDEO_MEDIA_AUDIO_MP2;
	config.pcm_ring = small_pcm_ring;
	config.pcm_ring_capacity = sizeof(small_pcm_ring);
	config.pcm_low_water_bytes = 8192U;
	config.pcm_high_water_bytes = sizeof(small_pcm_ring);
	if (!ops->configure_media(decoder, &config)) {
		ops->destroy(decoder);
		return 3;
	}
	accepted = 0U;
	if (ops->write(
	        decoder, zz9k_mpeg1_mp2_ps_fixture,
	        zz9k_mpeg1_mp2_ps_fixture_len, 1, &accepted) !=
	        SDK_VIDEO_BACKEND_WRITE_OK ||
	    accepted != zz9k_mpeg1_mp2_ps_fixture_len) {
		ops->destroy(decoder);
		return 4;
	}
	do {
		result = ops->decode(decoder, &frame);
		if (result == SDK_VIDEO_BACKEND_FRAME) {
			if (first_video_pcm == UINT64_MAX) {
				if (!ops->get_media_info(decoder, &media)) {
					ops->destroy(decoder);
					return 5;
				}
				first_video_pcm = media.pcm_produced;
			}
			video_frames++;
		}
	} while ((result == SDK_VIDEO_BACKEND_PROGRESS ||
	          result == SDK_VIDEO_BACKEND_FRAME) &&
	         guard++ < 20U);
	if (result != SDK_VIDEO_BACKEND_BACKPRESSURE ||
	    video_frames == 0U ||
	    first_video_pcm < config.pcm_low_water_bytes ||
	    !ops->get_media_info(decoder, &media) ||
	    media.pcm_produced != 9216U ||
	    media.pcm_acknowledged != 0U ||
	    media.backpressure_events != 1U ||
	    (media.flags & SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE) == 0U) {
		ops->destroy(decoder);
		return 5;
	}
	/* Timeline event bits are consumed once; level bits remain current. */
	if (!ops->get_media_info(decoder, &media) ||
	    (media.flags & (SDK_VIDEO_MEDIA_FLAG_DERIVED_TIME |
	                    SDK_VIDEO_MEDIA_FLAG_DISCONTINUITY |
	                    SDK_VIDEO_MEDIA_FLAG_REBASED)) != 0U ||
	    (media.flags & SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE) == 0U) {
		ops->destroy(decoder);
		return 6;
	}
	if (ops->ack_media(decoder, 2U) ||
	    ops->ack_media(decoder, media.pcm_produced + 4U)) {
		ops->destroy(decoder);
		return 7;
	}
	protected_hash = ring_hash(
		small_pcm_ring, sizeof(small_pcm_ring), 4608U, 4608U);
	if (!ops->ack_media(decoder, 4608U) ||
	    !ops->get_media_info(decoder, &media) ||
	    media.pcm_acknowledged != 4608U ||
	    (media.flags & SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE) != 0U ||
	    (media.flags & SDK_VIDEO_MEDIA_FLAG_AUDIO_READY) == 0U) {
		ops->destroy(decoder);
		return 8;
	}
	result = ops->decode(decoder, &frame);
	if ((result != SDK_VIDEO_BACKEND_PROGRESS &&
	     result != SDK_VIDEO_BACKEND_FRAME) ||
	    !ops->get_media_info(decoder, &media) ||
	    media.pcm_produced != 13824U ||
	    ring_hash(small_pcm_ring, sizeof(small_pcm_ring), 4608U, 4608U) !=
		    protected_hash ||
	    ring_hash(small_pcm_ring, sizeof(small_pcm_ring), 9216U, 4608U) !=
		    fnv1a64(UINT64_C(14695981039346656037),
		            expected_pcm + 9216U, 4608U)) {
		ops->destroy(decoder);
		return 9;
	}
	if (ops->ack_media(decoder, 0U) ||
	    !ops->ack_media(decoder, media.pcm_produced) ||
	    !ops->get_media_info(decoder, &media) ||
	    (media.flags & (SDK_VIDEO_MEDIA_FLAG_AUDIO_READY |
	                    SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE)) != 0U) {
		ops->destroy(decoder);
		return 10;
	}
	while (result != SDK_VIDEO_BACKEND_DONE && guard++ < 1000U) {
		result = ops->decode(decoder, &frame);
		if (result == SDK_VIDEO_BACKEND_PROGRESS ||
		    result == SDK_VIDEO_BACKEND_FRAME) {
			if (!ops->get_media_info(decoder, &media) ||
			    !ops->ack_media(decoder, media.pcm_produced)) {
				ops->destroy(decoder);
				return 11;
			}
		} else if (result == SDK_VIDEO_BACKEND_BACKPRESSURE ||
		           result == SDK_VIDEO_BACKEND_ERROR ||
		           result == SDK_VIDEO_BACKEND_UNSUPPORTED ||
		           result == SDK_VIDEO_BACKEND_NEED_INPUT) {
			fprintf(stderr, "small ring drain stopped: result=%d "
			        "produced=%llu ack=%llu flags=%lu\n",
			        result,
			        (unsigned long long)media.pcm_produced,
			        (unsigned long long)media.pcm_acknowledged,
			        (unsigned long)media.flags);
			ops->destroy(decoder);
			return 12;
		}
	}
	if (result != SDK_VIDEO_BACKEND_DONE ||
	    !ops->get_media_info(decoder, &media) ||
	    media.pcm_produced != 73728U ||
	    ops->decode(decoder, &frame) != SDK_VIDEO_BACKEND_DONE) {
		fprintf(stderr, "small ring final: result=%d produced=%llu "
		        "ack=%llu guard=%lu flags=%lu\n", result,
		        (unsigned long long)media.pcm_produced,
		        (unsigned long long)media.pcm_acknowledged,
		        (unsigned long)guard, (unsigned long)media.flags);
		ops->destroy(decoder);
		return 13;
	}
	ops->destroy(decoder);
	return 0;
}

static int decode_to_terminal(const uint8_t *bytes, uint32_t length,
	                          uint32_t audio_codec, int *terminal,
	                          uint64_t *pcm_produced)
{
	const struct SDKVideoDecoderOps *ops =
		sdk_video_backend_find(1U, 1U);
	struct SDKVideoMediaConfig config;
	struct SDKVideoMediaInfo media;
	struct SDKVideoDecodedFrame frame;
	uint32_t accepted = 0U;
	uint32_t guard = 0U;
	void *decoder;
	int result = SDK_VIDEO_BACKEND_PROGRESS;

	decoder = ops ? ops->create() : 0;
	if (!decoder)
		return 0;
	if (audio_codec != TEST_MEDIA_UNCONFIGURED) {
		memset(&config, 0, sizeof(config));
		config.audio_codec = audio_codec;
		if (audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2) {
			config.pcm_ring = pcm_ring;
			config.pcm_ring_capacity = sizeof(pcm_ring);
			config.pcm_low_water_bytes = 4608U;
			config.pcm_high_water_bytes = sizeof(pcm_ring);
		}
		if (!ops->configure_media(decoder, &config)) {
			ops->destroy(decoder);
			return 0;
		}
	}
	if (ops->write(decoder, bytes, length, 1, &accepted) !=
	        SDK_VIDEO_BACKEND_WRITE_OK ||
	    accepted != length) {
		ops->destroy(decoder);
		return 0;
	}
	while (result != SDK_VIDEO_BACKEND_DONE &&
	       result != SDK_VIDEO_BACKEND_ERROR &&
	       result != SDK_VIDEO_BACKEND_UNSUPPORTED &&
	       guard++ < 1000U) {
		result = ops->decode(decoder, &frame);
		if (result == SDK_VIDEO_BACKEND_BACKPRESSURE) {
			if (audio_codec != SDK_VIDEO_MEDIA_AUDIO_MP2 ||
			    !ops->get_media_info(decoder, &media) ||
			    !ops->ack_media(decoder, media.pcm_produced)) {
				ops->destroy(decoder);
				return 0;
			}
		} else if ((result == SDK_VIDEO_BACKEND_PROGRESS ||
		            result == SDK_VIDEO_BACKEND_FRAME) &&
		           audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2 &&
		           ops->get_media_info(decoder, &media)) {
			if (!ops->ack_media(decoder, media.pcm_produced)) {
				ops->destroy(decoder);
				return 0;
			}
		} else if (result == SDK_VIDEO_BACKEND_NEED_INPUT) {
			ops->destroy(decoder);
			return 0;
		}
	}
	if (guard >= 1000U) {
		ops->destroy(decoder);
		return 0;
	}
	*terminal = result;
	*pcm_produced = 0U;
	memset(&last_terminal_media, 0, sizeof(last_terminal_media));
	if (audio_codec != TEST_MEDIA_UNCONFIGURED &&
	    ops->get_media_info(decoder, &media)) {
		*pcm_produced = media.pcm_produced;
		last_terminal_media = media;
	}
	ops->destroy(decoder);
	return 1;
}

static int find_pes(const uint8_t *bytes, uint32_t length,
	                uint8_t stream_id, uint32_t occurrence,
	                uint32_t *control_offset, uint32_t *payload_offset,
	                uint32_t *packet_end)
{
	uint32_t found = 0U;
	uint32_t i;

	for (i = 0U; i + 6U <= length; i++) {
		uint32_t end;
		uint32_t cursor;
		uint32_t packet_length;
		uint32_t header_length;

		if (bytes[i] != 0x00U || bytes[i + 1U] != 0x00U ||
		    bytes[i + 2U] != 0x01U || bytes[i + 3U] != stream_id)
			continue;
		packet_length =
			((uint32_t)bytes[i + 4U] << 8) | bytes[i + 5U];
		end = i + 6U + packet_length;
		if (packet_length == 0U || end > length)
			continue;
		cursor = i + 6U;
		while (cursor < end && bytes[cursor] == 0xffU)
			cursor++;
		if (cursor < end && (bytes[cursor] & 0xc0U) == 0x40U)
			cursor += 2U;
		if (cursor >= end)
			continue;
		if ((bytes[cursor] & 0xf0U) == 0x20U)
			header_length = 5U;
		else if ((bytes[cursor] & 0xf0U) == 0x30U)
			header_length = 10U;
		else if (bytes[cursor] == 0x0fU)
			header_length = 1U;
		else
			continue;
		if (cursor + header_length > end)
			continue;
		if (found++ != occurrence)
			continue;
		*control_offset = cursor;
		*payload_offset = cursor + header_length;
		*packet_end = end;
		return 1;
	}
	return 0;
}

static int strip_pes_pts(uint8_t *bytes, uint32_t length,
	                     uint8_t stream_id, uint32_t occurrence)
{
	uint32_t control;
	uint32_t payload;
	uint32_t end;
	uint32_t header_length;

	if (!find_pes(bytes, length, stream_id, occurrence,
	              &control, &payload, &end))
		return 0;
	(void)payload;
	(void)end;
	if ((bytes[control] & 0xf0U) == 0x20U)
		header_length = 5U;
	else if ((bytes[control] & 0xf0U) == 0x30U)
		header_length = 10U;
	else
		return 0;
	memset(bytes + control, 0xff, header_length - 1U);
	bytes[control + header_length - 1U] = 0x0fU;
	return 1;
}

static int rewrite_pes_pts(uint8_t *bytes, uint32_t length,
	                       uint8_t stream_id, uint32_t occurrence,
	                       uint64_t pts)
{
	uint32_t control;
	uint32_t payload;
	uint32_t end;
	uint8_t prefix;

	if (!find_pes(bytes, length, stream_id, occurrence,
	              &control, &payload, &end))
		return 0;
	(void)payload;
	(void)end;
	prefix = bytes[control] & 0xf0U;
	if (prefix != 0x20U && prefix != 0x30U)
		return 0;
	pts &= (UINT64_C(1) << 33) - 1U;
	bytes[control] =
		(uint8_t)(prefix | ((pts >> 29) & 0x0eU) | 0x01U);
	bytes[control + 1U] = (uint8_t)(pts >> 22);
	bytes[control + 2U] =
		(uint8_t)(((pts >> 14) & 0xfeU) | 0x01U);
	bytes[control + 3U] = (uint8_t)(pts >> 7);
	bytes[control + 4U] =
		(uint8_t)(((pts << 1) & 0xfeU) | 0x01U);
	return 1;
}

static int corrupt_pes_payloads(uint8_t *bytes, uint32_t length,
	                            uint8_t stream_id, int preserve_fake_mp2)
{
	uint32_t control;
	uint32_t payload;
	uint32_t end;
	uint32_t occurrence = 0U;
	uint32_t first_payload = 0U;

	while (find_pes(bytes, length, stream_id, occurrence,
	                &control, &payload, &end)) {
		(void)control;
		if (occurrence == 0U)
			first_payload = payload;
		memset(bytes + payload, 0, end - payload);
		occurrence++;
	}
	if (occurrence == 0U)
		return 0;
	if (preserve_fake_mp2) {
		if (first_payload + 2U > length)
			return 0;
		bytes[first_payload] = 0xffU;
		bytes[first_payload + 1U] = 0xfcU;
	}
	return 1;
}

static int test_video_only_media_timing(void)
{
	static const uint32_t chunks[] = {1U, 137U, 4096U};
	struct media_summary expected;
	struct media_summary actual;
	uint32_t i;
	int result;

	result = decode_fixture_bytes(
		zz9k_mpeg1_ps_fixture, zz9k_mpeg1_ps_fixture_len,
		chunks[0], SDK_VIDEO_MEDIA_AUDIO_NONE, &expected);
	if (result != 0)
		return 1;
	for (i = 1U; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
		result = decode_fixture_bytes(
			zz9k_mpeg1_ps_fixture, zz9k_mpeg1_ps_fixture_len,
			chunks[i], SDK_VIDEO_MEDIA_AUDIO_NONE, &actual);
		if (result != 0 ||
		    memcmp(&actual, &expected, sizeof(actual)) != 0)
			return 2;
	}
	if (expected.frames != 5U ||
	    expected.video_hash != UINT64_C(0x12cf0e63c2e2c840) ||
	    expected.first_video_pts != 48600U ||
	    expected.last_video_pts != 63000U ||
	    expected.pcm_produced != 0U)
		return 3;
	return 0;
}

static int test_fixture_timeline_mutations(
	const struct media_summary *reference)
{
	static const uint32_t chunks[] = {1U, 137U, 4096U};
	struct media_summary expected;
	struct media_summary actual;
	uint8_t *missing;
	uint8_t *discontinuous;
	uint32_t i;
	int result = 0;

	missing = (uint8_t *)malloc(zz9k_mpeg1_mp2_ps_fixture_len);
	discontinuous = (uint8_t *)malloc(
		zz9k_mpeg1_mp2_ps_fixture_len);
	if (!missing || !discontinuous) {
		result = 1;
		goto done;
	}
	memcpy(missing, zz9k_mpeg1_mp2_ps_fixture,
	       zz9k_mpeg1_mp2_ps_fixture_len);
	memcpy(discontinuous, zz9k_mpeg1_mp2_ps_fixture,
	       zz9k_mpeg1_mp2_ps_fixture_len);
	if (!strip_pes_pts(
		    missing, zz9k_mpeg1_mp2_ps_fixture_len, 0xc0U, 0U) ||
	    !rewrite_pes_pts(
		    discontinuous, zz9k_mpeg1_mp2_ps_fixture_len,
		    0xc0U, 1U, UINT64_C(300000))) {
		result = 2;
		goto done;
	}
	result = decode_fixture_bytes(
		missing, zz9k_mpeg1_mp2_ps_fixture_len, chunks[0],
		SDK_VIDEO_MEDIA_AUDIO_MP2, &expected);
	if (result != 0 ||
	    expected.frames != reference->frames ||
	    expected.video_hash != reference->video_hash ||
	    expected.pcm_hash != reference->pcm_hash ||
	    expected.pcm_produced != reference->pcm_produced ||
	    (expected.media_flags &
	     SDK_VIDEO_MEDIA_FLAG_DERIVED_TIME) == 0U) {
		result = 3;
		goto done;
	}
	for (i = 1U; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
		result = decode_fixture_bytes(
			missing, zz9k_mpeg1_mp2_ps_fixture_len, chunks[i],
			SDK_VIDEO_MEDIA_AUDIO_MP2, &actual);
		if (result != 0 ||
		    memcmp(&actual, &expected, sizeof(actual)) != 0) {
			fprintf(stderr,
			        "missing-PTS chunk %lu mismatch: result=%d "
			        "flags=%08lx/%08lx vpts=%llu/%llu "
			        "apts=%llu/%llu\n",
			        (unsigned long)chunks[i], result,
			        (unsigned long)actual.media_flags,
			        (unsigned long)expected.media_flags,
			        (unsigned long long)actual.first_video_pts,
			        (unsigned long long)expected.first_video_pts,
			        (unsigned long long)actual.first_audio_pts,
			        (unsigned long long)expected.first_audio_pts);
			result = 4;
			goto done;
		}
	}
	result = decode_fixture_bytes(
		discontinuous, zz9k_mpeg1_mp2_ps_fixture_len, chunks[0],
		SDK_VIDEO_MEDIA_AUDIO_MP2, &expected);
	if (result != 0 ||
	    expected.frames != reference->frames ||
	    expected.video_hash != reference->video_hash ||
	    expected.pcm_hash != reference->pcm_hash ||
	    expected.pcm_produced != reference->pcm_produced ||
	    (expected.media_flags &
	     (SDK_VIDEO_MEDIA_FLAG_DISCONTINUITY |
	      SDK_VIDEO_MEDIA_FLAG_REBASED)) !=
		    (SDK_VIDEO_MEDIA_FLAG_DISCONTINUITY |
		     SDK_VIDEO_MEDIA_FLAG_REBASED)) {
		result = 5;
		goto done;
	}
	for (i = 1U; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
		result = decode_fixture_bytes(
			discontinuous, zz9k_mpeg1_mp2_ps_fixture_len,
			chunks[i], SDK_VIDEO_MEDIA_AUDIO_MP2, &actual);
		if (result != 0 ||
		    memcmp(&actual, &expected, sizeof(actual)) != 0) {
			fprintf(stderr,
			        "discontinuous-PTS chunk %lu mismatch: "
			        "result=%d flags=%08lx/%08lx "
			        "apts=%llu/%llu\n",
			        (unsigned long)chunks[i], result,
			        (unsigned long)actual.media_flags,
			        (unsigned long)expected.media_flags,
			        (unsigned long long)actual.first_audio_pts,
			        (unsigned long long)expected.first_audio_pts);
			result = 6;
			goto done;
		}
	}
	result = 0;

done:
	free(discontinuous);
	free(missing);
	return result;
}

static int test_corrupt_selected_streams_fail(void)
{
	uint8_t *corrupt_video;
	uint8_t *corrupt_audio;
	uint64_t produced;
	int terminal;
	int result = 0;

	corrupt_video = (uint8_t *)malloc(
		zz9k_mpeg1_mp2_ps_fixture_len);
	corrupt_audio = (uint8_t *)malloc(
		zz9k_mpeg1_mp2_ps_fixture_len);
	if (!corrupt_video || !corrupt_audio) {
		result = 1;
		goto done;
	}
	memcpy(corrupt_video, zz9k_mpeg1_mp2_ps_fixture,
	       zz9k_mpeg1_mp2_ps_fixture_len);
	memcpy(corrupt_audio, zz9k_mpeg1_mp2_ps_fixture,
	       zz9k_mpeg1_mp2_ps_fixture_len);
	if (!corrupt_pes_payloads(
		    corrupt_video, zz9k_mpeg1_mp2_ps_fixture_len,
		    0xe0U, 0) ||
	    !corrupt_pes_payloads(
		    corrupt_audio, zz9k_mpeg1_mp2_ps_fixture_len,
		    0xc0U, 1)) {
		result = 2;
		goto done;
	}
	if (!decode_to_terminal(
		    corrupt_video, zz9k_mpeg1_mp2_ps_fixture_len,
		    SDK_VIDEO_MEDIA_AUDIO_MP2, &terminal, &produced) ||
	    terminal != SDK_VIDEO_BACKEND_ERROR) {
		result = 3;
		goto done;
	}
	if (!decode_to_terminal(
		    corrupt_audio, zz9k_mpeg1_mp2_ps_fixture_len,
		    SDK_VIDEO_MEDIA_AUDIO_MP2, &terminal, &produced) ||
	    terminal != SDK_VIDEO_BACKEND_ERROR || produced != 0U) {
		result = 4;
		goto done;
	}

done:
	free(corrupt_audio);
	free(corrupt_video);
	return result;
}

static int test_supported_audio_variants(void)
{
	uint64_t hash;
	uint64_t produced;
	uint32_t i;
	int terminal;

	if (!decode_to_terminal(
	        zz9k_mpeg1_mp2_mono32_ps_fixture,
	        zz9k_mpeg1_mp2_mono32_ps_fixture_len,
	        SDK_VIDEO_MEDIA_AUDIO_MP2, &terminal, &produced) ||
	    terminal != SDK_VIDEO_BACKEND_DONE ||
	    last_terminal_media.sample_rate != 32000U ||
	    last_terminal_media.channels != 2U ||
	    last_terminal_media.sample_format !=
		    SDK_VIDEO_MEDIA_SAMPLE_S16BE)
		return 1;
	for (i = 0U; i < produced; i += 4U) {
		/* The media ABI is always stereo; pl_mpeg deterministically
		 * duplicates a mono MP2 source into left and right. */
		if (pcm_ring[i] != pcm_ring[i + 2U] ||
		    pcm_ring[i + 1U] != pcm_ring[i + 3U])
			return 2;
	}
	hash = fnv1a64(
		UINT64_C(14695981039346656037), pcm_ring, (uint32_t)produced);
	if (produced != 23040U ||
	    hash != UINT64_C(0x20f099ccd9e324c5) ||
	    last_terminal_media.first_audio_pts != 50847U ||
	    last_terminal_media.audio_pts != 67047U)
		return 3;

	if (!decode_to_terminal(
	        zz9k_mpeg1_mp2_stereo48_ps_fixture,
	        zz9k_mpeg1_mp2_stereo48_ps_fixture_len,
	        SDK_VIDEO_MEDIA_AUDIO_MP2, &terminal, &produced) ||
	    terminal != SDK_VIDEO_BACKEND_DONE ||
	    last_terminal_media.sample_rate != 48000U ||
	    last_terminal_media.channels != 2U ||
	    last_terminal_media.sample_format !=
		    SDK_VIDEO_MEDIA_SAMPLE_S16BE)
		return 4;
	hash = fnv1a64(
		UINT64_C(14695981039346656037), pcm_ring, (uint32_t)produced);
	if (produced != 32256U ||
	    hash != UINT64_C(0x108d40660a10a62e) ||
	    last_terminal_media.first_audio_pts != 51298U ||
	    last_terminal_media.audio_pts != 66418U)
		return 5;
	return 0;
}

static int test_terminal_policies(void)
{
	static const uint8_t malformed[] = {
		0x00U, 0x01U, 0x02U, 0x03U, 0xffU, 0x7fU, 0x00U
	};
	uint64_t produced;
	uint32_t i;
	int terminal;

	/* Explicit MP2 on a valid video-only PS is a specific unsupported
	 * policy result; legacy video-only decoding remains supported. */
	if (!decode_to_terminal(
	        zz9k_mpeg1_ps_fixture, zz9k_mpeg1_ps_fixture_len,
	        SDK_VIDEO_MEDIA_AUDIO_MP2, &terminal, &produced) ||
	    terminal != SDK_VIDEO_BACKEND_UNSUPPORTED || produced != 0U)
		return 1;
	if (!decode_to_terminal(
	        zz9k_mpeg1_ps_fixture, zz9k_mpeg1_ps_fixture_len,
	        TEST_MEDIA_UNCONFIGURED, &terminal, &produced) ||
	    terminal != SDK_VIDEO_BACKEND_DONE)
		return 2;
	/* Empty and malformed EOF must terminate rather than waiting forever. */
	if (!decode_to_terminal(
	        0, 0U, SDK_VIDEO_MEDIA_AUDIO_MP2,
	        &terminal, &produced) ||
	    terminal != SDK_VIDEO_BACKEND_ERROR)
		return 3;
	if (!decode_to_terminal(
	        malformed, sizeof(malformed),
	        SDK_VIDEO_MEDIA_AUDIO_MP2, &terminal, &produced) ||
	    terminal != SDK_VIDEO_BACKEND_ERROR)
		return 4;
	/* A truncated real PS must also reach a deterministic terminal result
	 * without publishing PCM beyond complete decoded blocks. */
	if (!decode_to_terminal(
	        zz9k_mpeg1_mp2_ps_fixture,
	        zz9k_mpeg1_mp2_ps_fixture_len / 2U,
	        SDK_VIDEO_MEDIA_AUDIO_MP2, &terminal, &produced) ||
	    terminal != SDK_VIDEO_BACKEND_DONE ||
	    produced != 27648U)
		return 5;
	if (!decode_to_terminal(
	        zz9k_mpeg1_mp2_ps_fixture,
	        zz9k_mpeg1_mp2_ps_fixture_len / 2U,
	        SDK_VIDEO_MEDIA_AUDIO_MP2, &terminal, &produced) ||
	    terminal != SDK_VIDEO_BACKEND_DONE ||
	    produced != 27648U)
		return 6;
	for (i = 0U; i < 20U; i++) {
		if (!decode_to_terminal(
		        zz9k_mpeg1_mp2_ps_fixture,
		        zz9k_mpeg1_mp2_ps_fixture_len,
		        SDK_VIDEO_MEDIA_AUDIO_MP2,
		        &terminal, &produced) ||
		    terminal != SDK_VIDEO_BACKEND_DONE ||
		    produced != 73728U)
			return 7;
	}
	return 0;
}

static int test_compressed_input_backpressure_is_recoverable(void)
{
	const struct SDKVideoDecoderOps *ops =
		sdk_video_backend_find(1U, 1U);
	struct SDKVideoDecodedFrame frame;
	uint8_t byte = 0U;
	uint32_t accepted;
	void *decoder;
	int result;

	decoder = ops ? ops->create() : 0;
	if (!decoder)
		return 1;
	accepted = 0U;
	if (ops->write(
	        decoder, compressed_fill, sizeof(compressed_fill), 0,
	        &accepted) != SDK_VIDEO_BACKEND_WRITE_OK ||
	    accepted != sizeof(compressed_fill)) {
		ops->destroy(decoder);
		return 2;
	}
	accepted = 99U;
	if (ops->write(decoder, &byte, 1U, 0, &accepted) !=
	        SDK_VIDEO_BACKEND_WRITE_BACKPRESSURE ||
	    accepted != 0U) {
		ops->destroy(decoder);
		return 3;
	}
	result = ops->decode(decoder, &frame);
	if (result != SDK_VIDEO_BACKEND_NEED_INPUT) {
		ops->destroy(decoder);
		return 4;
	}
	accepted = 0U;
	if (ops->write(decoder, &byte, 1U, 0, &accepted) !=
	        SDK_VIDEO_BACKEND_WRITE_OK ||
	    accepted != 1U) {
		ops->destroy(decoder);
		return 5;
	}
	ops->destroy(decoder);
	return 0;
}

int main(void)
{
	static const uint32_t chunks[] = {1U, 137U, 4096U};
	static const uint64_t block_hashes[16] = {
		UINT64_C(0x42ac49d252dffbc6),
		UINT64_C(0x3776106d66bd8652),
		UINT64_C(0x11ddeac184762201),
		UINT64_C(0x451815f81a9e4c0f),
		UINT64_C(0xb6140c264768f7db),
		UINT64_C(0x17a9a9308411665d),
		UINT64_C(0xdf444fe90d903fe9),
		UINT64_C(0xe729b8e7aa34c41d),
		UINT64_C(0x0623fac0fcd46ec9),
		UINT64_C(0xa3d3e15922419afd),
		UINT64_C(0x1930fb467bf02bbd),
		UINT64_C(0x83dc9f84968aea72),
		UINT64_C(0x2f424f71dc19f69e),
		UINT64_C(0xbe5af8adc89f1a37),
		UINT64_C(0x59ab4acf91bd84dd),
		UINT64_C(0xbbc61ea3de3b50d0),
	};
	struct media_summary expected;
	struct media_summary actual;
	uint32_t i;
	int result;

	if (!sdk_video_plmpeg_test_boundary_mp2_probe())
		return 1;
	result = decode_fixture(chunks[0], &expected);
	if (result != 0)
		return 10 + result;
	for (i = 1U; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
		result = decode_fixture(chunks[i], &actual);
		if (result != 0 || memcmp(&actual, &expected, sizeof(actual)) != 0)
			return 30 + (result != 0 ? result : (int)i);
	}
	if (expected.frames != 11U ||
	    expected.sample_rate != 44100U ||
	    expected.pcm_produced != 73728U ||
	    expected.video_hash != UINT64_C(0xb8e08ebc902ef95a) ||
	    expected.pcm_hash != UINT64_C(0xb6dd1795a7c9298e) ||
	    expected.first_video_pts != 48600U ||
	    expected.first_audio_pts != 54818U ||
	    expected.last_video_pts != 84600U ||
	    expected.current_audio_pts != 92435U ||
	    expected.audio_tail_pts != 94786U ||
	    expected.first_audio_pts - expected.first_video_pts != 6218U)
		return 50;
	for (i = 0U; i < 16U; i++) {
		if (expected.pcm_block_hash[i] != block_hashes[i])
			return 51;
	}
	memcpy(expected_pcm, pcm_ring, (size_t)expected.pcm_produced);
	result = test_ring_backpressure_and_wrap();
	if (result != 0)
		return 60 + result;
	result = test_terminal_policies();
	if (result != 0)
		return 80 + result;
	result = test_video_only_media_timing();
	if (result != 0)
		return 90 + result;
	result = test_supported_audio_variants();
	if (result != 0)
		return 100 + result;
	result = test_compressed_input_backpressure_is_recoverable();
	if (result != 0)
		return 110 + result;
	result = test_fixture_timeline_mutations(&expected);
	if (result != 0)
		return 120 + result;
	result = test_corrupt_selected_streams_fail();
	if (result != 0)
		return 130 + result;
	printf("media fixture: frames=%lu rate=%lu pcm=%llu "
	       "video=%016llx pcm=%016llx vpts=%llu..%llu "
	       "apts=%llu..%llu tail=%llu\n",
	       (unsigned long)expected.frames,
	       (unsigned long)expected.sample_rate,
	       (unsigned long long)expected.pcm_produced,
	       (unsigned long long)expected.video_hash,
	       (unsigned long long)expected.pcm_hash,
	       (unsigned long long)expected.first_video_pts,
	       (unsigned long long)expected.last_video_pts,
	       (unsigned long long)expected.first_audio_pts,
	       (unsigned long long)expected.current_audio_pts,
	       (unsigned long long)expected.audio_tail_pts);
	return 0;
}
